// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxsource.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>

#include "library/rekordbox/rekordboxsourceparser.h"

namespace {
using namespace mixxx::rekordbox;

SourceDescriptor source(SourceKind kind = SourceKind::Xml) {
    return {QUuid::createUuid().toString(QUuid::WithoutBraces),
            kind,
            QStringLiteral("/test/export.xml"),
            QStringLiteral("Same name 100%_-->"),
            {}};
}

SourceResult success(const SourceRequest& request) {
    auto snapshot = std::make_shared<SourceSnapshot>();
    snapshot->source = request.source;
    snapshot->generation = request.generation;
    snapshot->fingerprint = QStringLiteral("fixture-v1");
    snapshot->accessLease = request.accessLease;
    return {request, ImportOutcome::Success, {}, snapshot};
}

TEST(RekordboxSourceTest, NewGenerationCancelsAndRejectsOldResult) {
    SourceSessions sessions;
    const auto descriptor = source();
    ASSERT_TRUE(sessions.add(descriptor, true));
    const auto first = *sessions.begin(descriptor.id);
    const auto second = *sessions.begin(descriptor.id);
    EXPECT_TRUE(first.isCancelled());
    EXPECT_FALSE(sessions.finish(success(first)));
    EXPECT_TRUE(sessions.loading());
    EXPECT_TRUE(sessions.finish(success(second)));
    EXPECT_FALSE(sessions.loading());
}

TEST(RekordboxSourceTest, DeviceIdentityRejectsReusedMountPathAndAllowsVerifiedRelocation) {
    SourceSession existing;
    existing.source = source(SourceKind::Usb);
    existing.available = true;
    existing.source.volumeIdentity = QStringLiteral("filesystem-uuid:old");
    auto discovered = existing.source;
    discovered.volumeIdentity = QStringLiteral("filesystem-uuid:new");
    EXPECT_FALSE(matchesDiscoveredSource(existing, discovered));
    discovered.volumeIdentity.clear();
    EXPECT_FALSE(matchesDiscoveredSource(existing, discovered));
    discovered = existing.source;
    discovered.locator = QStringLiteral("/different/mount");
    existing.available = false;
    EXPECT_TRUE(matchesDiscoveredSource(existing, discovered));
    existing.source.volumeIdentity.clear();
    discovered = existing.source;
    EXPECT_FALSE(matchesDiscoveredSource(existing, discovered));
    existing.available = true;
    EXPECT_FALSE(matchesDiscoveredSource(existing, discovered));
}

TEST(RekordboxSourceTest, RemovalRejectsQueuedResultAndRetainsWorkerLease) {
    SourceSessions sessions;
    const auto descriptor = source();
    ASSERT_TRUE(sessions.add(descriptor, true));
    auto lease = std::make_shared<int>(7);
    std::weak_ptr<int> observed = lease;
    auto request = *sessions.begin(descriptor.id, lease);
    lease.reset();
    sessions.remove(descriptor.id);
    EXPECT_FALSE(observed.expired());
    EXPECT_FALSE(sessions.finish(success(request)));
    request.accessLease.reset();
    EXPECT_TRUE(observed.expired());
}

TEST(RekordboxSourceTest, RelocationKeepsIdentityAndRejectsOldLocator) {
    SourceSessions sessions;
    const auto descriptor = source();
    ASSERT_TRUE(sessions.add(descriptor, true));
    const auto old = *sessions.begin(descriptor.id);
    ASSERT_TRUE(sessions.relocate(descriptor.id, QStringLiteral("/new/export.xml")));
    EXPECT_FALSE(sessions.finish(success(old)));
    const auto current = *sessions.begin(descriptor.id);
    EXPECT_EQ(current.source.id, descriptor.id);
    EXPECT_EQ(current.source.locator, QStringLiteral("/new/export.xml"));
    EXPECT_TRUE(sessions.finish(success(current)));
}

TEST(RekordboxSourceTest, FailureClearsLoadingAndPreservesLastGoodSnapshot) {
    SourceSessions sessions;
    const auto descriptor = source();
    ASSERT_TRUE(sessions.add(descriptor, true));
    ASSERT_TRUE(sessions.finish(success(*sessions.begin(descriptor.id))));
    const auto prior = sessions.get(descriptor.id)->snapshot;
    const auto request = *sessions.begin(descriptor.id);
    ASSERT_TRUE(sessions.finish({request,
            ImportOutcome::Invalid,
            {QStringLiteral("truncated")},
            {}}));
    EXPECT_FALSE(sessions.loading());
    EXPECT_EQ(sessions.get(descriptor.id)->snapshot, prior);
    EXPECT_EQ(sessions.get(descriptor.id)->lastOutcome, ImportOutcome::Invalid);
    EXPECT_TRUE(sessions.finish(success(*sessions.begin(descriptor.id))));
}

TEST(RekordboxSourceTest, EmptyUsbRefreshPreservesXmlAndDuplicateNames) {
    SourceSessions sessions;
    const auto xml = source();
    const auto usb = source(SourceKind::Usb);
    ASSERT_TRUE(sessions.add(xml, true));
    ASSERT_TRUE(sessions.add(usb, true));
    const auto missing = sessions.missingUsbSources({});
    ASSERT_EQ(missing.size(), 1);
    EXPECT_EQ(missing.front(), usb.id);
    sessions.unavailable(usb.id);
    EXPECT_TRUE(sessions.get(xml.id)->available);
    EXPECT_FALSE(sessions.get(usb.id)->available);
}

TEST(RekordboxSourceTest, SourceJobsFinishIndependently) {
    SourceSessions sessions;
    const auto first = source();
    const auto second = source();
    ASSERT_TRUE(sessions.add(first, true));
    ASSERT_TRUE(sessions.add(second, true));
    const auto a = *sessions.begin(first.id);
    const auto b = *sessions.begin(second.id);
    ASSERT_TRUE(sessions.finish(success(a)));
    EXPECT_TRUE(sessions.loading());
    ASSERT_TRUE(sessions.finish({b, ImportOutcome::Denied, {QStringLiteral("permission")}, {}}));
    EXPECT_FALSE(sessions.loading());
}

class RekordboxExternalSourceTest : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_TRUE(folder.isValid());
        connection = QUuid::createUuid().toString();
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(folder.filePath(QStringLiteral("collection.sqlite")));
        ASSERT_TRUE(db.open()) << db.lastError().text().toStdString();
        QString error;
        ASSERT_TRUE(createExternalSourceTables(db, &error)) << error.toStdString();
    }
    void TearDown() override {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(connection);
    }
    SourceSnapshot snapshot(const SourceDescriptor& descriptor) {
        SourceSnapshot result;
        result.source = descriptor;
        result.generation = 1;
        result.fingerprint = QStringLiteral("fixture-1");
        for (int id : {202, 101}) {
            ExternalTrack track;
            track.sourceTrackId = QString::number(id);
            track.metadata.id = id;
            track.metadata.location = QStringLiteral("/same/audio%1.wav").arg(id);
            track.metadata.title = QStringLiteral("original");
            result.tracks.push_back(track);
        }
        result.playlists = {{QStringLiteral("one"),
                                    {},
                                    QStringLiteral("duplicate"),
                                    false,
                                    {QStringLiteral("202"),
                                            QStringLiteral("101"),
                                            QStringLiteral("202")}},
                {QStringLiteral("two"),
                        {},
                        QStringLiteral("duplicate"),
                        true,
                        {}},
                {QStringLiteral("empty"),
                        QStringLiteral("two"),
                        {},
                        false,
                        {}}};
        return result;
    }
    int count(const QString& table) {
        QSqlQuery query(db);
        EXPECT_TRUE(query.exec(QStringLiteral("SELECT count(*) FROM ") + table));
        EXPECT_TRUE(query.next());
        return query.value(0).toInt();
    }
    QTemporaryDir folder;
    QString connection;
    QSqlDatabase db;
};

TEST_F(RekordboxExternalSourceTest, PreservesDuplicateMembershipNamesAndEmptyContainers) {
    const auto records = snapshot(source());
    QString error;
    ASSERT_TRUE(publishExternalSource(db, records, &error)) << error.toStdString();
    EXPECT_EQ(count(QStringLiteral("rekordbox_playlists")), 4);
    QSqlQuery query(db);
    query.prepare(QStringLiteral(
            "SELECT t.source_track_id FROM rekordbox_playlist_tracks m "
            "JOIN rekordbox_library t ON t.id=m.track_id "
            "JOIN rekordbox_playlists p ON p.id=m.playlist_id WHERE p.name=? ORDER BY m.position"));
    query.addBindValue(externalPlaylistKey(records.source.id, QStringLiteral("one")));
    ASSERT_TRUE(query.exec());
    QStringList ids;
    while (query.next()) {
        ids.append(query.value(0).toString());
    }
    EXPECT_EQ(ids,
            (QStringList{QStringLiteral("202"),
                    QStringLiteral("101"),
                    QStringLiteral("202")}));
}

TEST_F(RekordboxExternalSourceTest, SharedAudioRemainsSourceScopedAndRemovalUsesExactIdentity) {
    const auto a = snapshot(source());
    auto b = snapshot(source(SourceKind::Usb));
    b.source.displayName = a.source.displayName;
    b.tracks[0].metadata.title = QStringLiteral("second source");
    QString error;
    ASSERT_TRUE(publishExternalSource(db, a, &error)) << error.toStdString();
    ASSERT_TRUE(publishExternalSource(db, b, &error)) << error.toStdString();
    EXPECT_EQ(count(QStringLiteral("rekordbox_library")), 4);
    QSqlQuery canonical(db);
    ASSERT_TRUE(canonical.exec(QStringLiteral(
            "CREATE TABLE library (id INTEGER PRIMARY KEY,title TEXT)")));
    ASSERT_TRUE(canonical.exec(QStringLiteral("INSERT INTO library VALUES(7,'local')")));
    ASSERT_TRUE(removeExternalSource(db, a.source.id, &error)) << error.toStdString();
    EXPECT_EQ(count(QStringLiteral("rekordbox_library")), 2);
    EXPECT_EQ(count(QStringLiteral("rekordbox_playlists")), 4);
    EXPECT_EQ(count(QStringLiteral("library")), 1);
}

TEST_F(RekordboxExternalSourceTest, FailedReplacementRollsBackLastGoodView) {
    auto records = snapshot(source());
    QString error;
    ASSERT_TRUE(publishExternalSource(db, records, &error)) << error.toStdString();
    QSqlQuery trigger(db);
    ASSERT_TRUE(trigger.exec(QStringLiteral(
            "CREATE TRIGGER fail_source_insert BEFORE INSERT ON rekordbox_library "
            "WHEN NEW.title='fail' BEGIN SELECT RAISE(ABORT,'injected'); END")));
    records.tracks[1].metadata.title = QStringLiteral("fail");
    EXPECT_FALSE(publishExternalSource(db, records, &error));
    EXPECT_TRUE(error.contains(QStringLiteral("injected")));
    EXPECT_EQ(count(QStringLiteral("rekordbox_library")), 2);
    EXPECT_EQ(count(QStringLiteral("rekordbox_playlist_tracks")), 5);
}

TEST_F(RekordboxExternalSourceTest, InvalidMembershipAndHierarchyCannotReplaceView) {
    auto records = snapshot(source());
    QString error;
    ASSERT_TRUE(publishExternalSource(db, records, &error)) << error.toStdString();
    records.playlists[0].trackIds.append(QStringLiteral("missing"));
    EXPECT_FALSE(publishExternalSource(db, records, &error));
    records.playlists[0].trackIds.removeLast();
    records.playlists[1].parentId = QStringLiteral("empty");
    EXPECT_FALSE(publishExternalSource(db, records, &error));
    EXPECT_EQ(count(QStringLiteral("rekordbox_playlists")), 4);
}

TEST_F(RekordboxExternalSourceTest, OpaquePlaylistIdentitySurvivesCloseAndReopen) {
    const auto records = snapshot(source());
    QString error;
    ASSERT_TRUE(publishExternalSource(db, records, &error)) << error.toStdString();
    db.close();
    ASSERT_TRUE(db.open());
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT label FROM rekordbox_playlists WHERE name=?"));
    query.addBindValue(externalPlaylistKey(records.source.id, QStringLiteral("two")));
    ASSERT_TRUE(query.exec());
    ASSERT_TRUE(query.next());
    EXPECT_EQ(query.value(0).toString(), QStringLiteral("duplicate"));
}

TEST(RekordboxSourceReaderTest, LocationKeyTracksKeepRepeatedOrderedMembership) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    auto descriptor = source();
    descriptor.locator = directory.filePath(QStringLiteral("export.xml"));
    QFile file(descriptor.locator);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(
            R"xml(<DJ_PLAYLISTS Version="1.0.0"><PRODUCT Name="rekordbox" Version="test"/>
<COLLECTION><TRACK Location="file:///music/one%20track.wav"/><TRACK Location="file:///music/two.wav"/></COLLECTION>
<PLAYLISTS><NODE Type="0" Name="ROOT"><NODE Type="1" Name="Repeated" KeyType="1">
<TRACK Key="file:///music/two.wav"/><TRACK Key="file:///music/one%20track.wav"/><TRACK Key="file:///music/two.wav"/>
</NODE></NODE></PLAYLISTS></DJ_PLAYLISTS>)xml");
    file.close();
    SourceRequest request{descriptor, 1, std::make_shared<std::atomic_bool>(false), {}};
    const auto result = decodeSource(request);
    ASSERT_TRUE(result.snapshot) << result.diagnostics.join(QLatin1Char('\n')).toStdString();
    ASSERT_EQ(result.snapshot->tracks.size(), 2);
    ASSERT_EQ(result.snapshot->playlists.size(), 2);
    const auto& order = result.snapshot->playlists[1].trackIds;
    ASSERT_EQ(order.size(), 3);
    EXPECT_EQ(order[0], result.snapshot->tracks[1].sourceTrackId);
    EXPECT_EQ(order[1], result.snapshot->tracks[0].sourceTrackId);
    EXPECT_EQ(order[2], result.snapshot->tracks[1].sourceTrackId);
}

TEST(RekordboxSourceReaderTest, EmptyHiddenPioneerSourceIsValidAndMalformedRetryHasNoSnapshot) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir(directory.path()).mkpath(QStringLiteral(".PIONEER/rekordbox")));
    const QString path = directory.filePath(QStringLiteral(".PIONEER/rekordbox/export.pdb"));
    QByteArray bytes(4096, '\0');
    qToLittleEndian<quint32>(4096, bytes.data() + 4);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
    auto descriptor = source(SourceKind::Usb);
    descriptor.locator = directory.path();
    EXPECT_EQ(rekordboxPdbPath(directory.path()), path);
    SourceRequest request{descriptor, 1, std::make_shared<std::atomic_bool>(false), {}};
    auto result = decodeSource(request);
    ASSERT_TRUE(result.snapshot) << result.diagnostics.join(QLatin1Char('\n')).toStdString();
    EXPECT_TRUE(result.snapshot->tracks.isEmpty());
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("bad");
    file.close();
    result = decodeSource(request);
    EXPECT_EQ(result.outcome, ImportOutcome::Invalid);
    EXPECT_FALSE(result.snapshot);
}

TEST(RekordboxSourceReaderTest, CancellationAndUnavailableNeverPublish) {
    const auto descriptor = source();
    SourceRequest request{descriptor, 1, std::make_shared<std::atomic_bool>(true), {}};
    const auto cancelled = decodeSource(request);
    EXPECT_EQ(cancelled.outcome, ImportOutcome::Cancelled);
    EXPECT_FALSE(cancelled.snapshot);
    request.cancelled->store(false);
    const auto unavailable = decodeSource(request);
    EXPECT_EQ(unavailable.outcome, ImportOutcome::Unavailable);
    EXPECT_FALSE(unavailable.snapshot);
}
TEST(RekordboxSourceReaderTest, PageTypeAndIdentityMustMatchTheTableReference) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    ASSERT_TRUE(QDir(directory.path()).mkpath(QStringLiteral("PIONEER/rekordbox")));
    const QString path = directory.filePath(QStringLiteral("PIONEER/rekordbox/export.pdb"));
    QByteArray bytes(8192, '\0');
    qToLittleEndian<quint32>(4096, bytes.data() + 4);
    qToLittleEndian<quint32>(1, bytes.data() + 8);
    qToLittleEndian<quint32>(1, bytes.data() + 36);
    qToLittleEndian<quint32>(1, bytes.data() + 40);
    qToLittleEndian<quint32>(1, bytes.data() + 4096 + 4);
    qToLittleEndian<quint32>(1, bytes.data() + 4096 + 8);
    auto descriptor = source(SourceKind::Usb);
    descriptor.locator = directory.path();
    SourceRequest request{descriptor, 1, std::make_shared<std::atomic_bool>(false), {}};
    const auto read = [&] {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        EXPECT_EQ(file.write(bytes), bytes.size());
        file.close();
        return decodeSource(request);
    };
    const auto wrongType = read();
    EXPECT_EQ(wrongType.outcome, ImportOutcome::Invalid);
    EXPECT_FALSE(wrongType.snapshot);
    qToLittleEndian<quint32>(0, bytes.data() + 4096 + 8);
    qToLittleEndian<quint32>(2, bytes.data() + 4096 + 4);
    const auto wrongIndex = read();
    EXPECT_EQ(wrongIndex.outcome, ImportOutcome::Invalid);
    EXPECT_FALSE(wrongIndex.snapshot);
    qToLittleEndian<quint32>(1, bytes.data() + 4096 + 4);
    EXPECT_TRUE(read().snapshot);
}
} // namespace
// End of code prepared by ChatGPT for human review and submission.
