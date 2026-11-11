// Prepared by ChatGPT for human review. Binary fixtures below are schema-authored.
#include "library/rekordbox/rekordboxanlzimporter.h"

#include <gtest/gtest.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include "library/dao/cuedao.h"
#include "test/sqlitecommitfailure.h"
#include "track/beats.h"

namespace {
using namespace mixxx;
using namespace mixxx::rekordbox;

void u32(QByteArray* bytes, qsizetype offset, quint32 value) {
    for (int i = 3; i >= 0; --i) {
        (*bytes)[offset + i] = static_cast<char>(value & 255);
        value >>= 8;
    }
}

void u16(QByteArray* bytes, qsizetype offset, quint16 value) {
    (*bytes)[offset] = static_cast<char>(value >> 8);
    (*bytes)[offset + 1] = static_cast<char>(value & 255);
}

QByteArray classic(quint32 slot,
        quint32 start,
        std::optional<quint32> end = {},
        quint32 status = 1) {
    QByteArray bytes(56, '\0');
    bytes.replace(0, 4, "PCPT");
    u32(&bytes, 4, 28);
    u32(&bytes, 8, 56);
    u32(&bytes, 12, slot);
    u32(&bytes, 16, status);
    u16(&bytes, 24, 0xffff);
    u16(&bytes, 26, 0xffff);
    bytes[28] = end ? 2 : 1;
    u32(&bytes, 32, start);
    u32(&bytes, 36, end.value_or(0));
    return bytes;
}

QByteArray extended(quint32 slot,
        quint32 start,
        std::optional<quint32> end,
        const QString& label,
        quint32 rgb = 0) {
    const auto labelLength = (label.size() + 1) * 2;
    QByteArray bytes(44 + labelLength + 4, '\0');
    bytes.replace(0, 4, "PCP2");
    u32(&bytes, 4, 16);
    u32(&bytes, 8, bytes.size());
    u32(&bytes, 12, slot);
    bytes[16] = end ? 2 : 1;
    u32(&bytes, 20, start);
    u32(&bytes, 24, end.value_or(0));
    u32(&bytes, 40, labelLength);
    for (qsizetype i = 0; i < label.size(); ++i)
        u16(&bytes, 44 + i * 2, label[i].unicode());
    const auto offset = 44 + labelLength;
    bytes[offset] = 0x22;
    bytes[offset + 1] = static_cast<char>(rgb >> 16);
    bytes[offset + 2] = static_cast<char>(rgb >> 8);
    bytes[offset + 3] = static_cast<char>(rgb);
    return bytes;
}

QByteArray list(bool rich, bool hot, const QList<QByteArray>& cues) {
    QByteArray bytes(rich ? 20 : 24, '\0');
    bytes.replace(0, 4, rich ? "PCO2" : "PCOB");
    u32(&bytes, 4, bytes.size());
    u32(&bytes, 12, hot ? 1 : 0);
    u16(&bytes, rich ? 16 : 18, cues.size());
    for (const auto& cue : cues)
        bytes += cue;
    u32(&bytes, 8, bytes.size());
    return bytes;
}

QByteArray explicitBeats() {
    const quint16 phases[]{3, 4, 1, 2, 3};
    const quint32 times[]{26, 526, 1026, 1626, 2226};
    QByteArray bytes(64, '\0');
    bytes.replace(0, 4, "PQTZ");
    u32(&bytes, 4, 24);
    u32(&bytes, 8, bytes.size());
    u32(&bytes, 20, 5);
    for (int i = 0; i < 5; ++i) {
        u16(&bytes, 24 + 8 * i, phases[i]);
        u16(&bytes, 26 + 8 * i, i == 4 ? 9000 : 12000);
        u32(&bytes, 28 + 8 * i, times[i]);
    }
    return bytes;
}

QByteArray anlz(const QList<QByteArray>& sections) {
    QByteArray bytes(28, '\0');
    bytes.replace(0, 4, "PMAI");
    u32(&bytes, 4, 28);
    for (const auto& section : sections)
        bytes += section;
    u32(&bytes, 8, bytes.size());
    return bytes;
}

class RekordboxAnlzImporterTest : public testing::Test {
  protected:
    QTemporaryDir directory;
    QString error;
    std::unique_ptr<ImportSourceRegistry> registry;
    std::shared_ptr<SourceSnapshot> source;
    XmlMediaFacts media;
    XmlCanonicalAccess access;
    ImportPolicy policy{QStringLiteral("anlz-test-policy-v1"), false, false};
    bool cacheAllowed = true;

    void SetUp() override {
        ASSERT_TRUE(directory.isValid());
        registry = ImportSourceRegistry::open(
                directory.filePath(QStringLiteral("collection.sqlite")),
                &error);
        ASSERT_TRUE(registry) << error.toStdString();
        const auto supplied = qEnvironmentVariable("REKORDBOX_TEST_SCHEMA");
        QFile schema(supplied.isEmpty() ? QStringLiteral(":/schema.xml") : supplied);
        ASSERT_TRUE(schema.open(QIODevice::ReadOnly));
        QXmlStreamReader xml(&schema);
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("sql")) {
                for (const auto& statement : xml.readElementText().split(
                             QLatin1Char(';'), Qt::SkipEmptyParts)) {
                    if (!statement.trimmed().isEmpty())
                        exec(statement);
                }
            }
        }
        ASSERT_FALSE(xml.hasError());
        source = std::make_shared<SourceSnapshot>();
        const auto id = registry->createSource(QStringLiteral("usb"),
                directory.path(),
                QStringLiteral("USB"),
                &error);
        ASSERT_TRUE(id);
        const auto ticket = registry->requestGeneration(*id, &error);
        ASSERT_TRUE(ticket);
        source->source = {*id,
                SourceKind::Usb,
                directory.path(),
                QStringLiteral("USB"),
                QStringLiteral("test-volume")};
        source->generation = ticket->generation;
        source->fingerprint = QStringLiteral("immutable-pdb-fixture-v1");
        media = {directory.filePath(QStringLiteral("media.wav")),
                48000,
                6,
                30,
                1536,
                1000,
                QStringLiteral("wav"),
                QDateTime::fromString(
                        QStringLiteral("2024-01-01T00:00:00Z"), Qt::ISODate)};
        ExternalTrack track;
        track.sourceTrackId = QStringLiteral("pdb:101");
        track.metadata.location = media.location;
        track.metadata.title = QStringLiteral("PDB source title");
        track.analyzePath = directory.filePath(QStringLiteral("ANLZ0000.DAT"));
        source->tracks.append(track);
        access.lease = std::make_shared<const int>(1);
        access.externalCollectionsAbsent = true;
        access.check = [&](qint64, const QString&, QString* why) {
            if (!cacheAllowed && why)
                *why = QStringLiteral("Cached target");
            return cacheAllowed;
        };
    }

    void exec(const QString& sql) {
        QSqlQuery query(registry->database());
        ASSERT_TRUE(query.exec(sql)) << query.lastError().text().toStdString()
                                     << " SQL " << sql.toStdString();
    }

    QVariant scalar(const QString& sql) {
        QSqlQuery query(registry->database());
        EXPECT_TRUE(query.exec(sql)) << query.lastError().text().toStdString();
        EXPECT_TRUE(query.next());
        return query.value(0);
    }

    QByteArray read(const QString& extension) {
        QFile file(directory.filePath(QStringLiteral("ANLZ0000.") + extension));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }

    AnlzFilesResult write(const QByteArray& dat, const std::optional<QByteArray>& ext = {}) {
        for (const auto& pair : {qMakePair(QStringLiteral("DAT"), std::optional<QByteArray>(dat)),
                     qMakePair(QStringLiteral("EXT"), ext)}) {
            QFile file(directory.filePath(QStringLiteral("ANLZ0000.") + pair.first));
            if (!pair.second) {
                if (file.exists()) {
                    EXPECT_TRUE(file.remove());
                }
                continue;
            }
            EXPECT_TRUE(file.open(QIODevice::WriteOnly));
            EXPECT_EQ(pair.second->size(), file.write(*pair.second));
        }
        return readAnlzFiles(source->tracks[0].analyzePath);
    }

    XmlTrackImportResult apply(const std::shared_ptr<const AnlzFiles>& files,
            qint64 correction = 0) {
        return importAnlzTrack(registry.get(), source, 0, files, media, policy, access, correction);
    }

    void expectNothingApplied() {
        EXPECT_EQ(0, scalar(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
        EXPECT_EQ(0, scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt());
        EXPECT_EQ(0,
                scalar(QStringLiteral("SELECT COUNT(*) FROM "
                                      "rekordbox_import_track_revisions_v1"))
                        .toInt());
    }
};

TEST_F(RekordboxAnlzImporterTest, ReconcilesPhysicalMemoryAndNativeHotSlotsWithCloseReopen) {
    const auto dat = anlz({explicitBeats(),
            list(false, false, {classic(0, 26), classic(0, 26)}),
            list(false, true, {classic(1, 26), classic(8, 1026, 2226)})});
    const auto ext = anlz(
            {list(true,
                     false,
                     {extended(0, 26, {}, QString::fromUtf8("Å 🎵"), 0xff0000),
                             extended(0,
                                     26,
                                     {},
                                     QStringLiteral("Same position"),
                                     0x0000ff)}),
                    list(true,
                            true,
                            {extended(
                                     8, 1026, 2226, QStringLiteral("Hot loop")),
                                    extended(1,
                                            26,
                                            {},
                                            QStringLiteral(
                                                    "Black at zero"))})});
    const auto decoded = write(dat, ext);
    ASSERT_TRUE(decoded.snapshot);
    const auto applied = apply(decoded.snapshot, 26);
    ASSERT_TRUE(applied.application.committed);
    EXPECT_TRUE(applied.createdTrack);
    EXPECT_EQ(dat, read(QStringLiteral("DAT")));
    EXPECT_EQ(ext, read(QStringLiteral("EXT")));
    const auto repeat = apply(decoded.snapshot, 26);
    EXPECT_TRUE(repeat.application.committed);
    EXPECT_EQ(0, repeat.application.canonicalWrites);

    const auto databasePath = registry->database().databaseName();
    registry.reset();
    registry = ImportSourceRegistry::open(databasePath, &error);
    ASSERT_TRUE(registry);
    CueDAO dao;
    dao.initialize(registry->database());
    const auto cues = dao.getCuesForTrack(TrackId(QVariant(applied.canonicalTrackId)));
    ASSERT_EQ(4, cues.size());
    int memoryCount = 0;
    QSet<QString> memoryIds;
    for (const auto& cue : cues) {
        if (cue->getType() == CueType::Memory) {
            ++memoryCount;
            memoryIds.insert(cue->getId().toString());
            EXPECT_EQ(audio::FramePos(0), cue->getPosition());
        } else if (cue->getHotCue() == 0) {
            EXPECT_EQ(CueType::HotCue, cue->getType());
            EXPECT_EQ(audio::FramePos(0), cue->getPosition());
            EXPECT_EQ(RgbColor(0), cue->getColor());
        } else {
            EXPECT_EQ(CueType::Loop, cue->getType());
            EXPECT_EQ(7, cue->getHotCue());
            EXPECT_EQ(audio::FramePos(48000), cue->getPosition());
            EXPECT_EQ(audio::FramePos(105600), cue->getEndPosition());
        }
    }
    EXPECT_EQ(2, memoryCount);
    EXPECT_EQ(2, memoryIds.size());
    EXPECT_EQ(2,
            scalar(QStringLiteral("SELECT COUNT(*) FROM "
                                  "rekordbox_import_cue_bindings_v1"))
                    .toInt());
    EXPECT_EQ(QStringLiteral("PDB source title"),
            scalar(QStringLiteral("SELECT title FROM library")).toString());
    EXPECT_EQ(6, scalar(QStringLiteral("SELECT channels FROM library")).toInt());
    EXPECT_EQ(media.sourceSynchronizedAt.toMSecsSinceEpoch(),
            scalar(QStringLiteral("SELECT source_synchronized_ms FROM library")).toLongLong());
    const auto beats = Beats::fromByteArray(audio::SampleRate(48000),
            scalar(QStringLiteral("SELECT beats_version FROM library")).toString(),
            scalar(QStringLiteral("SELECT beats_sub_version FROM library")).toString(),
            scalar(QStringLiteral("SELECT beats FROM library")).toByteArray());
    ASSERT_TRUE(beats);
    EXPECT_EQ(2, beats->getDownbeatsOffset());
    EXPECT_EQ(audio::FramePos(76800), beats->findNextBeat(audio::FramePos(48001)));
    EXPECT_EQ(audio::FramePos(137600), beats->findNextBeat(audio::FramePos(105601)));
    EXPECT_EQ(0,
            scalar(QStringLiteral("SELECT applied_generation FROM "
                                  "rekordbox_import_sources_v1"))
                    .toInt());
}

TEST_F(RekordboxAnlzImporterTest, InconsistentMemoryPairStopsBeforeWrites) {
    const auto decoded = write(anlz({list(false, false, {classic(0, 1000)})}),
            anlz({list(true, false, {extended(0, 1001, {}, QStringLiteral("Disagrees"))})}));
    ASSERT_TRUE(decoded.snapshot);
    const auto result = apply(decoded.snapshot);
    EXPECT_EQ(ImportOutcome::Unsupported, result.application.outcome);
    EXPECT_FALSE(result.application.committed);
    expectNothingApplied();
}

TEST_F(RekordboxAnlzImporterTest, PresentBrokenExtInvalidatesOtherwiseValidDat) {
    const auto decoded = write(anlz({list(false, false, {classic(0, 1000)})}), QByteArray("PMAI"));
    EXPECT_FALSE(decoded.snapshot);
    EXPECT_EQ(ImportOutcome::Invalid, decoded.outcome);
    EXPECT_FALSE(apply(decoded.snapshot).application.committed);
    expectNothingApplied();
}

TEST_F(RekordboxAnlzImporterTest, SqlFailureRollsBackMediaCuesAndTrackRevision) {
    const auto decoded = write(anlz({list(false, false, {classic(0, 1000)})}));
    ASSERT_TRUE(decoded.snapshot);
    exec(QStringLiteral(
            "CREATE TRIGGER reject_cue BEFORE INSERT ON cues BEGIN SELECT "
            "RAISE(ABORT,'injected failure'); END"));
    EXPECT_FALSE(apply(decoded.snapshot).application.committed);
    expectNothingApplied();
    exec(QStringLiteral("DROP TRIGGER reject_cue"));
    EXPECT_TRUE(apply(decoded.snapshot).application.committed);
}

TEST_F(RekordboxAnlzImporterTest, CommitFailureRollsBackMediaCuesBindingsAndRevision) {
    const auto decoded = write(anlz({list(false, false, {classic(0, 1000)})}));
    ASSERT_TRUE(decoded.snapshot);
    SqliteCommitFailure failure(registry->database());
    if (!failure.isValid()) {
        GTEST_SKIP() << "Native SQLite commit hook is unavailable";
    }
    EXPECT_FALSE(apply(decoded.snapshot).application.committed);
    EXPECT_EQ(1, failure.attempts());
    expectNothingApplied();
    EXPECT_EQ(0,
            scalar(QStringLiteral("SELECT COUNT(*) FROM "
                                  "rekordbox_import_cue_bindings_v1"))
                    .toInt());
    failure.disable();
    EXPECT_TRUE(apply(decoded.snapshot).application.committed);
}

TEST_F(RekordboxAnlzImporterTest, LocalCueEditAndOccupiedHotSlotArePreserved) {
    const auto initial = write(anlz({list(false, true, {classic(1, 1000)})}));
    ASSERT_TRUE(initial.snapshot);
    ASSERT_TRUE(apply(initial.snapshot).application.committed);
    exec(QStringLiteral("UPDATE cues SET label='Local edit'"));
    const auto refreshed = write(anlz({list(false, true, {classic(1, 2000)})}));
    ASSERT_TRUE(refreshed.snapshot);
    EXPECT_EQ(ImportOutcome::Denied, apply(refreshed.snapshot).application.outcome);
    policy.acceptPreservedConflicts = true;
    ASSERT_TRUE(apply(refreshed.snapshot).application.committed);
    EXPECT_EQ(QStringLiteral("Local edit"),
            scalar(QStringLiteral("SELECT label FROM cues")).toString());
    EXPECT_EQ(96000, scalar(QStringLiteral("SELECT position FROM cues")).toLongLong());
    const auto secondId = registry->createSource(QStringLiteral("usb"),
            QStringLiteral("/other"),
            QStringLiteral("Other"),
            &error);
    ASSERT_TRUE(secondId);
    source->source.id = *secondId;
    source->generation = registry->requestGeneration(*secondId, &error)->generation;
    ASSERT_TRUE(apply(refreshed.snapshot).application.committed);
    EXPECT_EQ(1, scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt());
    EXPECT_EQ(QStringLiteral("Local edit"),
            scalar(QStringLiteral("SELECT label FROM cues")).toString());
}

TEST_F(RekordboxAnlzImporterTest, AbsentBankDoesNotDeleteButDeclaredEmptyBankDoes) {
    const auto initial = write(anlz({list(false, false, {classic(0, 1000)})}));
    ASSERT_TRUE(initial.snapshot);
    ASSERT_TRUE(apply(initial.snapshot).application.committed);
    const auto absent = write(anlz({}));
    ASSERT_TRUE(absent.snapshot);
    ASSERT_TRUE(apply(absent.snapshot).application.committed);
    EXPECT_EQ(1, scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt());
    const auto empty = write(anlz({list(false, false, {})}));
    ASSERT_TRUE(empty.snapshot);
    ASSERT_EQ(std::size_t{1}, empty.snapshot->dat->cueLists.size());
    EXPECT_EQ(quint32{0}, empty.snapshot->dat->cueLists.front().entryCount);
    ASSERT_TRUE(apply(empty.snapshot).application.committed);
    EXPECT_EQ(0, scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt());
}

TEST_F(RekordboxAnlzImporterTest, ActiveLoopRequiresExplicitLossReview) {
    const auto decoded = write(anlz({list(false, false, {classic(0, 1000, 3000, 4)})}));
    ASSERT_TRUE(decoded.snapshot);
    const auto denied = apply(decoded.snapshot);
    EXPECT_EQ(ImportOutcome::Denied, denied.application.outcome);
    expectNothingApplied();
    policy.acceptPreservedConflicts = true;
    const auto accepted = apply(decoded.snapshot);
    ASSERT_TRUE(accepted.application.committed);
    EXPECT_EQ(ImportOutcome::Warning, accepted.application.outcome);
    EXPECT_EQ(static_cast<int>(CueType::Loop),
            scalar(QStringLiteral("SELECT type FROM cues")).toInt());
    EXPECT_EQ(192000, scalar(QStringLiteral("SELECT length FROM cues")).toLongLong());
}

TEST_F(RekordboxAnlzImporterTest, NegativeCorrectedCueIsReportedWithoutClamping) {
    const auto decoded = write(anlz({list(false, false, {classic(0, 0)})}));
    ASSERT_TRUE(decoded.snapshot);
    EXPECT_EQ(ImportOutcome::Denied, apply(decoded.snapshot, 26).application.outcome);
    expectNothingApplied();
    policy.acceptPreservedConflicts = true;
    const auto accepted = apply(decoded.snapshot, 26);
    EXPECT_TRUE(accepted.application.committed);
    EXPECT_EQ(ImportOutcome::Warning, accepted.application.outcome);
    EXPECT_EQ(0, scalar(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt());
}

TEST_F(RekordboxAnlzImporterTest, PhaseDiscontinuityDoesNotProduceAnInventedGrid) {
    auto section = explicitBeats();
    u16(&section, 24 + 8 * 3, 4);
    const auto decoded = write(anlz({section}));
    ASSERT_TRUE(decoded.snapshot);
    EXPECT_EQ(ImportOutcome::Denied, apply(decoded.snapshot).application.outcome);
    expectNothingApplied();
    policy.acceptPreservedConflicts = true;
    EXPECT_TRUE(apply(decoded.snapshot).application.committed);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT beats FROM library")).toByteArray().isEmpty());
}

TEST_F(RekordboxAnlzImporterTest, GridBeyondSignedFrameStorageIsNotTruncated) {
    auto section = explicitBeats();
    for (int i = 0; i < 5; ++i) {
        u32(&section, 28 + 8 * i, 50000000 + 500 * i);
    }
    const auto decoded = write(anlz({section}));
    ASSERT_TRUE(decoded.snapshot);
    const auto prepared = prepareAnlzTrackImport(
            registry->database(), source, 0, decoded.snapshot, media, access);
    ASSERT_TRUE(prepared.snapshot);
    ASSERT_FALSE(prepared.issues.isEmpty());
    EXPECT_EQ(QStringLiteral("grid_storage_range"), prepared.issues.front().code);
    EXPECT_EQ(ImportOutcome::Denied, apply(decoded.snapshot).application.outcome);
    expectNothingApplied();
    policy.acceptPreservedConflicts = true;
    EXPECT_TRUE(apply(decoded.snapshot).application.committed);
    EXPECT_TRUE(scalar(QStringLiteral("SELECT beats FROM library")).toByteArray().isEmpty());
}

TEST_F(RekordboxAnlzImporterTest, UnchangedFastPathNeedsNoCacheLeaseAndTracksCorrection) {
    const auto decoded = write(anlz({list(false, false, {classic(0, 1000)})}));
    ASSERT_TRUE(decoded.snapshot);
    const auto applied = apply(decoded.snapshot);
    ASSERT_TRUE(applied.application.committed);
    cacheAllowed = false;
    EXPECT_EQ(applied.canonicalTrackId,
            currentAnlzTrackRevision(
                    *registry, *source, 0, *decoded.snapshot, policy));
    EXPECT_FALSE(currentAnlzTrackRevision(*registry, *source, 0, *decoded.snapshot, policy, 26));
    EXPECT_EQ(ImportOutcome::Denied, apply(decoded.snapshot).application.outcome);
    exec(QStringLiteral("UPDATE library SET header_parsed=0"));
    EXPECT_FALSE(currentAnlzTrackRevision(*registry, *source, 0, *decoded.snapshot, policy));
}

TEST_F(RekordboxAnlzImporterTest, PreviewAndCancellationDoNotWriteCanonicalRows) {
    const auto decoded = write(anlz({list(false, false, {classic(0, 1000)})}));
    ASSERT_TRUE(decoded.snapshot);
    const auto prepared = prepareAnlzTrackImport(
            registry->database(), source, 0, decoded.snapshot, media, access);
    ASSERT_TRUE(prepared.snapshot);
    expectNothingApplied();
    XmlCanonicalAdapter adapter(prepared.target, access);
    const auto plan = registry->plan(*prepared.snapshot, policy, adapter);
    ASSERT_TRUE(plan.plan);
    EXPECT_EQ(ImportOutcome::Cancelled,
            registry->apply(*plan.plan, &adapter, [] { return true; }).outcome);
    expectNothingApplied();
    EXPECT_FALSE(readAnlzFiles(source->tracks[0].analyzePath, [] { return true; }).snapshot);
}

TEST_F(RekordboxAnlzImporterTest, PdbMetadataUsesCanonicalPayloadsAndRetainsRawLosses) {
    auto& track = source->tracks[0];
    track.metadata.composer = QStringLiteral("Composer");
    track.metadata.key = QStringLiteral("8A");
    track.metadata.playCount = 17;
    track.metadata.dateAdded = QStringLiteral("2020-01-02");
    track.metadata.lastPlayed = QStringLiteral("2020-01-03T04:05:06Z");
    track.metadata.artworkReference = directory.filePath(QStringLiteral("cover.jpg"));
    track.metadata.label = QStringLiteral("Source-only label");
    track.metadata.discNumber = 2;
    track.color = 0;
    const auto decoded = write(anlz({}));
    ASSERT_TRUE(decoded.snapshot);
    EXPECT_EQ(ImportOutcome::Denied, apply(decoded.snapshot).application.outcome);
    expectNothingApplied();
    policy.acceptPreservedConflicts = true;
    const auto applied = apply(decoded.snapshot);
    ASSERT_TRUE(applied.application.committed);
    EXPECT_EQ(ImportOutcome::Warning, applied.application.outcome);
    EXPECT_EQ(QStringLiteral("Composer"),
            scalar(QStringLiteral("SELECT composer FROM library")).toString());
    EXPECT_EQ(QStringLiteral("8A"), scalar(QStringLiteral("SELECT key FROM library")).toString());
    EXPECT_FALSE(scalar(QStringLiteral("SELECT keys FROM library")).toByteArray().isEmpty());
    EXPECT_EQ(17, scalar(QStringLiteral("SELECT timesplayed FROM library")).toInt());
    EXPECT_EQ(0, scalar(QStringLiteral("SELECT color FROM library")).toInt());
    EXPECT_EQ(QStringLiteral("2020-01-02 00:00:00"),
            scalar(QStringLiteral("SELECT datetime_added FROM library"))
                    .toString());
    EXPECT_EQ(track.metadata.artworkReference,
            scalar(QStringLiteral("SELECT coverart_location FROM library"))
                    .toString());
    EXPECT_EQ(2, scalar(QStringLiteral("SELECT coverart_type FROM library")).toInt());
    const auto details = QJsonDocument::fromJson(
            scalar(QStringLiteral("SELECT details_json FROM "
                                  "rekordbox_import_provenance_v1 WHERE "
                                  "annotation_id='media'"))
                    .toByteArray())
                                 .object();
    EXPECT_EQ(QStringLiteral("Source-only label"),
            details.value(QStringLiteral("pdbRecord"))
                    .toObject()
                    .value(QStringLiteral("label"))
                    .toString());
    EXPECT_EQ(2,
            details.value(QStringLiteral("pdbRecord"))
                    .toObject()
                    .value(QStringLiteral("discNumber"))
                    .toInt());
    track.metadata.dateAdded = QStringLiteral("");
    track.metadata.lastPlayed = QStringLiteral("");
    const auto cleared = apply(decoded.snapshot);
    ASSERT_TRUE(cleared.application.committed);
    EXPECT_EQ(ImportOutcome::Warning, cleared.application.outcome);
    EXPECT_EQ(QStringLiteral("2020-01-02 00:00:00"),
            scalar(QStringLiteral("SELECT datetime_added FROM library")).toString());
    EXPECT_TRUE(scalar(QStringLiteral("SELECT last_played_at FROM library")).toString().isEmpty());
    EXPECT_TRUE(scalar(QStringLiteral("SELECT losses_json FROM "
                                      "rekordbox_import_track_revisions_v1"))
                    .toString()
                    .contains(QStringLiteral("empty_required_timestamp")));
}

} // namespace
// End of code prepared by ChatGPT for human review and submission.
