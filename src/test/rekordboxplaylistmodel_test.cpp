// Prepared by ChatGPT for human review.
#include <QAbstractButton>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QMessageBox>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QtEndian>

#include "library/basetrackcache.h"
#include "library/memorycuepromotionconfig.h"
#include "library/rekordbox/rekordboxfeature.h"
#include "library/rekordbox/rekordboxsourceparser.h"
#include "mixer/playerinfo.h"
#include "test/librarytest.h"
#include "track/cue.h"
#include "track/track.h"

namespace {
using namespace mixxx::rekordbox;

class RekordboxPlaylistModelTest : public LibraryTest {
  protected:
    ControlObject crossfader{ConfigKey(QStringLiteral("[Master]"), QStringLiteral("crossfader"))};
    ControlObject decks{ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_decks"))};
    ControlObject samplers{ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_samplers"))};
    ControlObject previews{ConfigKey(QStringLiteral("[App]"), QStringLiteral("num_preview_decks"))};
    QString error;
    QString mediaPath;
    QString xmlPath;
    QString sourceId;
    std::shared_ptr<ImportSourceRegistry> registry;
    std::shared_ptr<SourceSessions> sessions = std::make_shared<SourceSessions>();
    QSharedPointer<BaseTrackCache> cache;
    std::unique_ptr<RekordboxPlaylistModel> model;

    void SetUp() override {
        PlayerInfo::create();
        config()->setValue(mixxx::library::kMainCuePromotionConfigKey, false);
        ASSERT_TRUE(createExternalSourceTables(dbConnection(), &error)) << error.toStdString();
        registry = ImportSourceRegistry::attach(dbConnection(), &error);
        ASSERT_TRUE(registry) << error.toStdString();
        mediaPath = getTestDataDir().filePath(QStringLiteral("source.wav"));
        xmlPath = getTestDataDir().filePath(QStringLiteral("source.xml"));
        QFile wave(mediaPath);
        ASSERT_TRUE(wave.open(QIODevice::WriteOnly));
        QDataStream stream(&wave);
        stream.setByteOrder(QDataStream::LittleEndian);
        constexpr quint32 bytes = 48000 * 4 * 4;
        stream.writeRawData("RIFF", 4);
        stream << quint32(36 + bytes);
        stream.writeRawData("WAVEfmt ", 8);
        stream << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
               << quint32(192000) << quint16(4) << quint16(16);
        stream.writeRawData("data", 4);
        stream << bytes;
        ASSERT_EQ(wave.write(QByteArray(bytes, '\0')), bytes);
        wave.close();
        const auto created = registry->createSource(QStringLiteral("rekordbox-xml"),
                xmlPath,
                QStringLiteral("Model source"),
                &error);
        ASSERT_TRUE(created) << error.toStdString();
        sourceId = *created;
        ASSERT_TRUE(sessions->add({sourceId,
                                          SourceKind::Xml,
                                          xmlPath,
                                          QStringLiteral("Model source"),
                                          {}},
                true));
    }

    void TearDown() override {
        model.reset();
        cache.clear();
        PlayerInfo::destroy();
    }

    void publish(const QString& attributes = {}) {
        QFile xml(xmlPath);
        ASSERT_TRUE(xml.open(QIODevice::WriteOnly));
        const auto bytes =
                (QStringLiteral(
                         "<DJ_PLAYLISTS Version=\"1.0.0\"><COLLECTION><TRACK "
                         "TrackID=\"17\" Name=\"Source title\" Artist=\"Source "
                         "artist\" Location=\"") +
                        mediaPath.toHtmlEscaped() + QStringLiteral("\" ") +
                        attributes +
                        QStringLiteral(
                                "><TEMPO Inizio=\"0\" Bpm=\"120\" "
                                "Metro=\"4/4\" Battito=\"1\"/>"
                                "<POSITION_MARK Name=\"Black loop\" Type=\"4\" "
                                "Num=\"0\" Start=\"0\" End=\"1\" Red=\"0\" "
                                "Green=\"0\" Blue=\"0\"/>"
                                "<POSITION_MARK Name=\"Memory one\" Type=\"0\" "
                                "Num=\"-1\" Start=\"0.5\"/>"
                                "<POSITION_MARK Name=\"Memory two\" Type=\"0\" "
                                "Num=\"-1\" Start=\"0.5\"/>"
                                "</TRACK></COLLECTION><PLAYLISTS><NODE "
                                "Type=\"0\" Name=\"root\"/>"
                                "</PLAYLISTS></DJ_PLAYLISTS>"))
                        .toUtf8();
        ASSERT_EQ(xml.write(bytes), bytes.size());
        xml.close();
        const auto ticket = registry->requestGeneration(sourceId, &error);
        ASSERT_TRUE(ticket) << error.toStdString();
        const auto request = sessions->begin(
                sourceId, std::make_shared<const int>(1), ticket->generation);
        ASSERT_TRUE(request);
        const auto decoded = decodeSource(*request);
        ASSERT_TRUE(decoded.snapshot) << decoded.diagnostics.join(QLatin1Char('\n')).toStdString();
        ASSERT_TRUE(publishExternalSource(
                dbConnection(), *decoded.snapshot, &error))
                << error.toStdString();
        ASSERT_TRUE(sessions->finish(decoded));
        buildModel();
    }

    void buildModel() {
        const QStringList columns{QStringLiteral("id"),
                QStringLiteral("artist"),
                QStringLiteral("title"),
                QStringLiteral("album"),
                QStringLiteral("year"),
                QStringLiteral("genre"),
                QStringLiteral("tracknumber"),
                QStringLiteral("location"),
                QStringLiteral("comment"),
                QStringLiteral("rating"),
                QStringLiteral("duration"),
                QStringLiteral("bitrate"),
                QStringLiteral("bpm"),
                QStringLiteral("key"),
                QStringLiteral("color"),
                QStringLiteral("coverart"),
                QStringLiteral("analyze_path"),
                QStringLiteral("xml_metadata"),
                QStringLiteral("source_id"),
                QStringLiteral("source_track_id"),
                QStringLiteral("source_ordinal")};
        cache = QSharedPointer<BaseTrackCache>::create(internalCollection(),
                QStringLiteral("rekordbox_library"),
                QStringLiteral("id"),
                columns,
                QStringList{QStringLiteral("title")},
                false);
        model = std::make_unique<RekordboxPlaylistModel>(nullptr,
                trackCollectionManager(),
                cache,
                config(),
                sessions,
                registry);
        model->setPlaylist(externalPlaylistKey(sourceId));
        model->select();
        ASSERT_EQ(model->rowCount(), 1);
    }

    void publishUsbDto() {
        publish();
        ASSERT_TRUE(model);
        auto external = sessions->get(sourceId)->snapshot->tracks.front();
        external.sourceTrackId = QStringLiteral("pdb:17");
        external.metadata.attributes.clear();
        external.metadata.beatgrid.clear();
        external.metadata.cues.clear();
        const auto root = getTestDataDir().filePath(QStringLiteral("device"));
        const auto directory = root + QStringLiteral("/PIONEER/rekordbox");
        ASSERT_TRUE(QDir().mkpath(directory));
        QFile pdb(directory + QStringLiteral("/export.pdb"));
        ASSERT_TRUE(pdb.open(QIODevice::WriteOnly));
        QByteArray emptyPdb(4096, '\0');
        qToLittleEndian<quint32>(4096, emptyPdb.data() + 4);
        ASSERT_EQ(pdb.write(emptyPdb), emptyPdb.size());
        pdb.close();
        for (const auto& suffix : {QStringLiteral("DAT"), QStringLiteral("EXT")}) {
            const auto name = QStringLiteral("ANLZ0000.") + suffix;
            ASSERT_TRUE(QFile::copy(
                    getTestDir().filePath(
                            QStringLiteral(
                                    "rekordboxfixtures/anlz-synthetic/") +
                            name),
                    directory + QLatin1Char('/') + name));
        }
        external.analyzePath = directory + QStringLiteral("/ANLZ0000.DAT");
        const auto created = registry->createSource(QStringLiteral("rekordbox-usb"),
                root,
                QStringLiteral("Synthetic USB DTO"),
                &error);
        ASSERT_TRUE(created) << error.toStdString();
        sourceId = *created;
        ASSERT_TRUE(sessions->add({sourceId,
                                          SourceKind::Usb,
                                          root,
                                          QStringLiteral("Synthetic USB DTO"),
                                          {}},
                true));
        const auto ticket = registry->requestGeneration(sourceId, &error);
        ASSERT_TRUE(ticket);
        const auto request = sessions->begin(
                sourceId, std::make_shared<const int>(2), ticket->generation);
        ASSERT_TRUE(request);
        auto decoded = decodeSource(*request);
        ASSERT_TRUE(decoded.snapshot);
        auto snapshot = std::make_shared<SourceSnapshot>(*decoded.snapshot);
        // Prepared by ChatGPT: test the real model/ANLZ admission boundary using
        // a frozen track DTO. The empty PDB above does not qualify track decoding.
        // End of ChatGPT-prepared test-scope note.
        snapshot->tracks.push_back(std::move(external));
        decoded.snapshot = snapshot;
        ASSERT_TRUE(publishExternalSource(dbConnection(), *snapshot, &error))
                << error.toStdString();
        ASSERT_TRUE(sessions->finish(decoded));
        buildModel();
    }

    qint64 count(const QString& table) {
        QSqlQuery query(dbConnection());
        EXPECT_TRUE(query.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table))
                << query.lastError().text().toStdString();
        EXPECT_TRUE(query.next());
        return query.value(0).toLongLong();
    }

    QByteArray digest(const QString& path) {
        QFile input(path);
        EXPECT_TRUE(input.open(QIODevice::ReadOnly));
        return QCryptographicHash::hash(input.readAll(), QCryptographicHash::Sha256);
    }

    bool execute(const QString& statement) {
        QSqlQuery query(dbConnection());
        const auto success = query.exec(statement);
        EXPECT_TRUE(success) << query.lastError().text().toStdString();
        return success;
    }

    TrackPointer load(const std::function<void(QMessageBox*)>& onDialog = {}) {
        // Exercise the real nested-dialog path without making failures hang CI.
        QTimer dismiss;
        QObject::connect(&dismiss, &QTimer::timeout, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* box = qobject_cast<QMessageBox*>(widget);
                if (box && box->isVisible()) {
                    if (onDialog) {
                        onDialog(box);
                    } else {
                        ADD_FAILURE() << "Unexpected import dialog: " << box->text().toStdString();
                        box->done(QMessageBox::Cancel);
                    }
                }
            }
        });
        dismiss.start(1);
        return model->getTrack(model->index(0, 0));
    }
};

TEST_F(RekordboxPlaylistModelTest,
        DisplayingSourceRowsIsReadOnlyAndTrackRequestsCommitBeforeNotification) {
    publish();
    ASSERT_TRUE(model);
    EXPECT_EQ(count(QStringLiteral("library")), 0);
    EXPECT_EQ(count(QStringLiteral("cues")), 0);
    const auto xmlBefore = digest(xmlPath);
    const auto audioBefore = digest(mediaPath);
    QSignalSpy added(&internalCollection()->getTrackDAO(), &TrackDAO::tracksAdded);
    const auto track = load();
    ASSERT_TRUE(track);
    EXPECT_EQ(added.count(), 1);
    EXPECT_EQ(track->getTitle(), QStringLiteral("Source title"));
    EXPECT_TRUE(track->getDateAdded().isValid());
    EXPECT_EQ(track->getSampleRate().value(), 48000);
    EXPECT_EQ(track->getMainCuePosition(), mixxx::audio::kStartFramePos);
    EXPECT_EQ(count(QStringLiteral("library")), 1);
    EXPECT_EQ(count(QStringLiteral("cues")), 3);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_track_revisions_v1")), 1);
    int memories = 0;
    bool blackLoop = false;
    for (const auto& cue : track->getCuePoints()) {
        if (cue->getType() == mixxx::CueType::Memory) {
            ++memories;
        }
        if (cue->getHotCue() == 0) {
            EXPECT_EQ(cue->getPosition(), mixxx::audio::FramePos(0));
            EXPECT_EQ(cue->getEndPosition(), mixxx::audio::FramePos(48000));
            EXPECT_EQ(cue->getColor(), mixxx::RgbColor(0));
            blackLoop = true;
        }
    }
    EXPECT_EQ(memories, 2);
    EXPECT_TRUE(blackLoop);
    EXPECT_EQ(digest(xmlPath), xmlBefore);
    EXPECT_EQ(digest(mediaPath), audioBefore);
}

TEST_F(RekordboxPlaylistModelTest, UnchangedSourceReturnsCachedTrackAndPreservesLocalEdits) {
    publish();
    const auto track = load();
    ASSERT_TRUE(track);
    track->setTitle(QStringLiteral("Edited in Mixxx"));
    const auto repeated = load();
    ASSERT_TRUE(repeated);
    EXPECT_EQ(track.get(), repeated.get());
    EXPECT_EQ(repeated->getTitle(), QStringLiteral("Edited in Mixxx"));
    EXPECT_EQ(count(QStringLiteral("cues")), 3);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_track_revisions_v1")), 1);
}

TEST_F(RekordboxPlaylistModelTest, FailedCueInsertLeavesNoCanonicalTrackAndRetryWorks) {
    publish();
    ASSERT_TRUE(execute(QStringLiteral(
            "CREATE TRIGGER reject_model_cue BEFORE INSERT ON cues "
            "BEGIN SELECT RAISE(ABORT, 'model cue failure'); END")));
    QSignalSpy added(&internalCollection()->getTrackDAO(), &TrackDAO::tracksAdded);
    int dialogs = 0;
    const auto failed = load([&](QMessageBox* box) {
        ++dialogs;
        box->done(QMessageBox::Cancel);
    });
    EXPECT_FALSE(failed);
    EXPECT_EQ(dialogs, 1);
    EXPECT_EQ(added.count(), 0);
    EXPECT_EQ(count(QStringLiteral("library")), 0);
    EXPECT_EQ(count(QStringLiteral("track_locations")), 0);
    EXPECT_EQ(count(QStringLiteral("cues")), 0);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_track_revisions_v1")), 0);
    ASSERT_TRUE(execute(QStringLiteral("DROP TRIGGER reject_model_cue")));
    EXPECT_TRUE(load());
    EXPECT_EQ(added.count(), 1);
}

TEST_F(RekordboxPlaylistModelTest, RemovalDuringConsentCancelsWithoutCanonicalMutation) {
    publish(QStringLiteral("UnsupportedField=\"retained source value\""));
    int dialogs = 0;
    const auto cancelled = load([&](QMessageBox* box) {
        ++dialogs;
        if (box->standardButtons().testFlag(QMessageBox::Ok)) {
            sessions->unavailable(sourceId);
            box->button(QMessageBox::Ok)->click();
        } else {
            box->done(QMessageBox::Cancel);
        }
    });
    EXPECT_FALSE(cancelled);
    EXPECT_GE(dialogs, 1);
    EXPECT_EQ(count(QStringLiteral("library")), 0);
    EXPECT_EQ(count(QStringLiteral("cues")), 0);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_track_revisions_v1")), 0);
}

TEST_F(RekordboxPlaylistModelTest, RemovedSourceRefusesAnExistingView) {
    publish();
    sessions->remove(sourceId);
    int dialogs = 0;
    EXPECT_FALSE(load([&](QMessageBox* box) {
        ++dialogs;
        box->done(QMessageBox::Cancel);
    }));
    EXPECT_EQ(dialogs, 1);
    EXPECT_EQ(count(QStringLiteral("library")), 0);
}

TEST_F(RekordboxPlaylistModelTest,
        FrozenUsbDtoUsesTransactionalAnlzPathAndRepeatsWithoutDuplication) {
    publishUsbDto();
    ASSERT_TRUE(model);
    EXPECT_EQ(count(QStringLiteral("library")), 0);
    const auto dat = sessions->get(sourceId)->snapshot->tracks.front().analyzePath;
    const auto datBefore = digest(dat);
    const QString ext = dat.left(dat.size() - 3) + QStringLiteral("EXT");
    const auto extBefore = digest(ext);
    const auto track = load();
    ASSERT_TRUE(track);
    EXPECT_EQ(track->getTitle(), QStringLiteral("Source title"));
    EXPECT_EQ(count(QStringLiteral("cues")), 4);
    EXPECT_EQ(track->getMainCuePosition(), mixxx::audio::kStartFramePos);
    int memories = 0;
    bool point = false;
    bool loop = false;
    for (const auto& cue : track->getCuePoints()) {
        if (cue->getType() == mixxx::CueType::Memory) {
            ++memories;
        }
        if (cue->getHotCue() == 0) {
            point = true;
            EXPECT_EQ(cue->getPosition(), mixxx::audio::FramePos(1248));
            EXPECT_EQ(cue->getColor(), mixxx::RgbColor(0));
        }
        if (cue->getHotCue() == 7) {
            loop = true;
            EXPECT_EQ(cue->getPosition(), mixxx::audio::FramePos(49248));
            EXPECT_EQ(cue->getEndPosition(), mixxx::audio::FramePos(106848));
        }
    }
    EXPECT_EQ(memories, 2);
    EXPECT_TRUE(point);
    EXPECT_TRUE(loop);
    const auto repeated = load();
    ASSERT_TRUE(repeated);
    EXPECT_EQ(repeated.get(), track.get());
    EXPECT_EQ(count(QStringLiteral("cues")), 4);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_track_revisions_v1")), 1);
    EXPECT_EQ(digest(dat), datBefore);
    EXPECT_EQ(digest(ext), extBefore);
}

TEST_F(RekordboxPlaylistModelTest, CorruptUsbExtendedFileCannotCreateCanonicalTrackAndCanRetry) {
    publishUsbDto();
    ASSERT_TRUE(model);
    const auto dat = sessions->get(sourceId)->snapshot->tracks.front().analyzePath;
    QFile ext(dat.left(dat.size() - 3) + QStringLiteral("EXT"));
    ASSERT_TRUE(ext.open(QIODevice::WriteOnly));
    ASSERT_EQ(ext.write("broken", 6), 6);
    ext.close();
    EXPECT_FALSE(load([](QMessageBox* box) { box->done(QMessageBox::Cancel); }));
    EXPECT_EQ(count(QStringLiteral("library")), 0);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_track_revisions_v1")), 0);
    ASSERT_TRUE(ext.remove());
    ASSERT_TRUE(QFile::copy(
            getTestDir().filePath(QStringLiteral(
                    "rekordboxfixtures/anlz-synthetic/ANLZ0000.EXT")),
            ext.fileName()));
    EXPECT_TRUE(load());
    EXPECT_EQ(count(QStringLiteral("cues")), 4);
}
TEST_F(RekordboxPlaylistModelTest, RequestedMainCuePromotionRequiresReviewAndKeepsMemoryRecords) {
    config()->setValue(mixxx::library::kMainCuePromotionConfigKey, true);
    publish();
    int dialogs = 0;
    const auto track = load([&](QMessageBox* box) {
        ++dialogs;
        EXPECT_TRUE(box->text().contains(QStringLiteral("Main Cue")));
        box->button(QMessageBox::Ok)->click();
    });
    ASSERT_TRUE(track);
    EXPECT_EQ(dialogs, 1);
    EXPECT_EQ(count(QStringLiteral("cues")), 3);
    EXPECT_FALSE(track->findCueByType(mixxx::CueType::MainCue));
    EXPECT_EQ(track->getMainCuePosition(), mixxx::audio::kStartFramePos);
    EXPECT_TRUE(load());
    EXPECT_EQ(count(QStringLiteral("cues")), 3);
}
TEST_F(RekordboxPlaylistModelTest, ExplicitSourceMetadataSurvivesTaggedAudioLoading) {
    QByteArray info("INFO", 4);
    QByteArray title("Embedded title", 14);
    title.append('\0');
    QDataStream tags(&info, QIODevice::Append);
    tags.setByteOrder(QDataStream::LittleEndian);
    tags.writeRawData("INAM", 4);
    tags << quint32(title.size());
    tags.writeRawData(title.constData(), title.size());
    if (title.size() % 2) {
        tags << quint8(0);
    }
    QFile wave(mediaPath);
    ASSERT_TRUE(wave.open(QIODevice::ReadWrite));
    ASSERT_TRUE(wave.seek(wave.size()));
    QDataStream data(&wave);
    data.setByteOrder(QDataStream::LittleEndian);
    data.writeRawData("LIST", 4);
    data << quint32(info.size());
    data.writeRawData(info.constData(), info.size());
    const auto riffLength = wave.size() - 8;
    ASSERT_TRUE(wave.seek(4));
    data << quint32(riffLength);
    wave.close();
    {
        const auto temporary = ::Track::newTemporary(mediaPath);
        const auto read = SoundSourceProxy(temporary).updateTrackFromSource(
                SoundSourceProxy::UpdateTrackFromSourceMode::Once, SyncTrackMetadataParams{});
        ASSERT_EQ(read, SoundSourceProxy::UpdateTrackFromSourceResult::MetadataImportedAndUpdated);
        ASSERT_EQ(temporary->getTitle(), QStringLiteral("Embedded title"));
    }
    const auto before = digest(mediaPath);
    publish();
    const auto track = load();
    ASSERT_TRUE(track);
    EXPECT_EQ(track->getTitle(), QStringLiteral("Source title"));
    EXPECT_TRUE(track->getSourceSynchronizedAt().isValid());
    EXPECT_EQ(digest(mediaPath), before);
}

TEST_F(RekordboxPlaylistModelTest, ReentrantTrackRequestDuringConsentCannotCommitAnInnerImport) {
    config()->setValue(mixxx::library::kMainCuePromotionConfigKey, true);
    publish();
    QSignalSpy added(&internalCollection()->getTrackDAO(), &TrackDAO::tracksAdded);
    int reviews = 0;
    int errors = 0;
    int nestedDialogs = 0;
    TrackPointer nested;
    const auto track = load([&](QMessageBox* box) {
        if (!box->standardButtons().testFlag(QMessageBox::Cancel)) {
            ++errors;
            box->button(QMessageBox::Ok)->click();
            return;
        }
        ++reviews;
        QTimer closeNested;
        QObject::connect(&closeNested, &QTimer::timeout, [&] {
            auto* active = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (active && active != box) {
                ++nestedDialogs;
                active->button(QMessageBox::Ok)->click();
            }
        });
        closeNested.start(1);
        nested = model->getTrack(model->index(0, 0));
        closeNested.stop();
        box->button(QMessageBox::Ok)->click();
    });
    ASSERT_TRUE(track);
    EXPECT_FALSE(nested);
    EXPECT_EQ(reviews, 1);
    EXPECT_EQ(errors, 0);
    EXPECT_EQ(nestedDialogs, 0);
    EXPECT_EQ(added.count(), 1);
    EXPECT_EQ(count(QStringLiteral("library")), 1);
    EXPECT_EQ(count(QStringLiteral("cues")), 3);
    EXPECT_EQ(count(QStringLiteral("rekordbox_import_track_revisions_v1")), 1);
    EXPECT_EQ(load().get(), track.get());
}
} // namespace
// End of code prepared by ChatGPT for human review and submission.
