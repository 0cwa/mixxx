#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QFile>
#include <QSignalSpy>
#include <QSqlDriver>
#include <QSqlError>

#ifdef __SQLITE3__
#include <sqlite3.h>
#endif

#include "test/librarytest.h"
#include "test/sqlitecommitfailure.h"
#include "track/globaltrackcache.h"
#include "track/track.h"

using ::testing::UnorderedElementsAre;

class TrackDAOTest : public LibraryTest {
    // Prepared by ChatGPT for human review.
  protected:
    TrackPointer newTrack(const QString& name, const QString& cueLabel) const {
        auto track = Track::newTemporary(mixxx::FileAccess(
                mixxx::FileInfo(getTestDataDir(), name)));
        track->setDuration(30);
        track->setTitle(QStringLiteral("original title"));
        CuePointer cue(new Cue(mixxx::CueType::Memory,
                Cue::kNoHotCue,
                mixxx::audio::FramePos(48000),
                mixxx::audio::kInvalidFramePos,
                mixxx::RgbColor(0x123456)));
        cue->setLabel(cueLabel);
        track->setCuePoints({cue});
        return track;
    }

    bool execute(const QString& sql) const {
        QSqlQuery query(dbConnection());
        const bool ok = query.exec(sql);
        EXPECT_TRUE(ok) << query.lastError().text().toStdString();
        return ok;
    }

    QVariant readValue(const QString& sql, const QVariantList& values = {}) const {
        QSqlQuery query(dbConnection());
        if (!query.prepare(sql)) {
            ADD_FAILURE() << query.lastError().text().toStdString();
            return {};
        }
        for (const auto& value : values) {
            query.addBindValue(value);
        }
        if (!query.exec() || !query.next()) {
            ADD_FAILURE() << query.lastError().text().toStdString();
            return {};
        }
        return query.value(0);
    }

    // End of ChatGPT-prepared code for human review.
};


TEST_F(TrackDAOTest, detectMovedTracks) {
    TrackDAO& trackDAO = internalCollection()->getTrackDAO();

    QString filename = QStringLiteral("file.mp3");

    mixxx::FileInfo oldFile(QDir(QDir::tempPath() + QStringLiteral("/old/dir1")), filename);
    mixxx::FileInfo newFile(QDir(QDir::tempPath() + QStringLiteral("/new/dir1")), filename);
    mixxx::FileInfo otherFile(QDir(QDir::tempPath() + QStringLiteral("/new")), filename);

    TrackPointer pOldTrack = Track::newTemporary(mixxx::FileAccess(oldFile));
    TrackPointer pNewTrack = Track::newTemporary(mixxx::FileAccess(newFile));
    TrackPointer pOtherTrack = Track::newTemporary(mixxx::FileAccess(otherFile));

    // Arbitrary duration
    pOldTrack->setDuration(135);
    pNewTrack->setDuration(135.7);
    pOtherTrack->setDuration(135.7);

    TrackId oldId = internalCollection()->addTrack(pOldTrack, false);
    TrackId newId = internalCollection()->addTrack(pNewTrack, false);
    internalCollection()->addTrack(pOtherTrack, false);

    // Mark as missing
    QSqlQuery query(dbConnection());
    query.prepare("UPDATE track_locations SET fs_deleted=1 WHERE location=:location");
    query.bindValue(":location", oldFile.location());
    query.exec();

    QList<RelocatedTrack> relocatedTracks;
    QStringList addedTracks(newFile.location());
    bool cancel = false;
    trackDAO.detectMovedTracks(&relocatedTracks, addedTracks, &cancel);

    QSet<TrackId> updatedTrackIds;
    QSet<TrackId> removedTrackIds;
    for (const auto& relocatedTrack : std::as_const(relocatedTracks)) {
        updatedTrackIds.insert(relocatedTrack.updatedTrackRef().getId());
        removedTrackIds.insert(relocatedTrack.deletedTrackId());
    }

    EXPECT_THAT(updatedTrackIds, UnorderedElementsAre(oldId));
    EXPECT_THAT(removedTrackIds, UnorderedElementsAre(newId));

    QSet<QString> trackLocations = trackDAO.getAllTrackLocations();
    EXPECT_THAT(trackLocations, UnorderedElementsAre(newFile.location(), otherFile.location()));
}

// Regression test for the bug where a BPM-locked track without a beatgrid
// loses its lock when reloaded from the database.
// https://github.com/mixxxdj/mixxx/issues/15196
TEST_F(TrackDAOTest, bpmLockPreservedForTrackWithoutBeats) {
    const mixxx::FileInfo fileInfo(
            QDir(QDir::tempPath()), QStringLiteral("bpmlocked-no-beats.mp3"));
    TrackPointer pTrack = Track::newTemporary(mixxx::FileAccess(fileInfo));
    pTrack->setDuration(135);
    // Lock the BPM although the track has no beatgrid at all.
    pTrack->setBpmLocked(true);
    ASSERT_FALSE(pTrack->getBeats());
    ASSERT_TRUE(pTrack->isBpmLocked());

    const TrackId trackId = internalCollection()->addTrack(pTrack, false);
    ASSERT_TRUE(trackId.isValid());

    // Dropping the last reference evicts the track from the cache
    // synchronously (eviction runs as a direct call on this thread), so the
    // lookup below reloads it from the database instead of returning the
    // cached in-memory object whose lock flag was never lost.
    pTrack.reset();
    ASSERT_TRUE(GlobalTrackCacheLocker().isEmpty());

    const TrackPointer pReloaded = internalCollection()->getTrackById(trackId);
    ASSERT_TRUE(pReloaded);
    EXPECT_FALSE(pReloaded->getBeats());
    EXPECT_TRUE(pReloaded->isBpmLocked());
}

TEST_F(TrackDAOTest, markTrackLocationsAsVerifiedRecoversPresentFilesOnly) {
    // Regression cover for both directions of mixxxdj/mixxx#13533:
    //   1. A track erroneously flagged fs_deleted=1 must be revived when the
    //      file is still present in an unchanged-hash directory (the original
    //      bug).
    //   2. A track legitimately flagged fs_deleted=1 because the file was
    //      removed before the saved directory hash was last updated must NOT
    //      be revived on a subsequent unchanged-hash rescan.
    //
    // markTrackLocationsAsVerified is the cleanup-phase entry point that the
    // scanner uses for every verified location, including those collected
    // from unchanged-hash directories. It should clear fs_deleted /
    // needs_verification for exactly the locations passed in, and leave
    // unrelated rows untouched.
    TrackDAO& trackDAO = internalCollection()->getTrackDAO();

    QDir dir(QDir::tempPath() + QStringLiteral("/verified_dir"));
    mixxx::FileInfo presentFile(dir, QStringLiteral("present.mp3"));
    mixxx::FileInfo deletedFile(dir, QStringLiteral("deleted.mp3"));

    TrackPointer pPresent = Track::newTemporary(mixxx::FileAccess(presentFile));
    TrackPointer pDeleted = Track::newTemporary(mixxx::FileAccess(deletedFile));
    pPresent->setDuration(180);
    pDeleted->setDuration(180);

    TrackId presentId = internalCollection()->addTrack(pPresent, false);
    TrackId deletedId = internalCollection()->addTrack(pDeleted, false);
    ASSERT_TRUE(presentId.isValid());
    ASSERT_TRUE(deletedId.isValid());

    // Both rows simulate the post-`invalidateTrackLocationsInLibrary` state at
    // the start of a scan. The "deleted" row additionally carries the
    // fs_deleted=1 that the previous scan's verifyRemainingTracks set when
    // the file disappeared from disk between scans.
    QSqlQuery setQuery(dbConnection());
    setQuery.prepare(
            "UPDATE track_locations "
            "SET fs_deleted=:fs_deleted, needs_verification=1 "
            "WHERE location=:location");
    setQuery.bindValue(":fs_deleted", 1);
    setQuery.bindValue(":location", presentFile.location());
    ASSERT_TRUE(setQuery.exec());
    setQuery.bindValue(":fs_deleted", 1);
    setQuery.bindValue(":location", deletedFile.location());
    ASSERT_TRUE(setQuery.exec());

    // The recursive scanner only feeds locations whose file is currently
    // present in the directory into markTrackLocationsAsVerified. Simulate
    // that for a delete-then-rescan: only `presentFile` is in the list.
    trackDAO.markTrackLocationsAsVerified(QStringList{presentFile.location()});

    QSqlQuery readQuery(dbConnection());
    readQuery.prepare(
            "SELECT fs_deleted, needs_verification FROM track_locations "
            "WHERE location=:location");

    readQuery.bindValue(":location", presentFile.location());
    ASSERT_TRUE(readQuery.exec());
    ASSERT_TRUE(readQuery.next());
    EXPECT_EQ(0, readQuery.value(0).toInt())
            << "fs_deleted should be cleared for a still-present file";
    EXPECT_EQ(0, readQuery.value(1).toInt())
            << "needs_verification should be cleared for a still-present file";

    readQuery.bindValue(":location", deletedFile.location());
    ASSERT_TRUE(readQuery.exec());
    ASSERT_TRUE(readQuery.next());
    EXPECT_EQ(1, readQuery.value(0).toInt())
            << "fs_deleted must be preserved for a file no longer in the directory";
    EXPECT_EQ(1, readQuery.value(1).toInt())
            << "needs_verification must remain set so verifyRemainingTracks can confirm deletion";
}

// Prepared by ChatGPT for human review.
TEST_F(TrackDAOTest, UpdateCueFailureRollsBackTrackMetadata) {
    auto& dao = internalCollection()->getTrackDAO();
    const auto track = newTrack(QStringLiteral("update-cue.wav"), QStringLiteral("original cue"));
    const auto id = internalCollection()->addTrack(track, false);
    ASSERT_TRUE(id.isValid());
    const auto cue = track->getCuePoints().front();
    const auto cueId = cue->getId();
    ASSERT_TRUE(cueId.isValid());
    ASSERT_FALSE(track->isDirty());

    track->setTitle(QStringLiteral("updated title"));
    cue->setLabel(QStringLiteral("updated cue"));
    ASSERT_TRUE(execute(QStringLiteral(
            "CREATE TRIGGER b_reject_update BEFORE UPDATE ON cues "
            "BEGIN SELECT RAISE(ABORT,'injected cue update failure'); END")));
    QSignalSpy cleanSignals(&dao, &TrackDAO::trackClean);
    EXPECT_FALSE(dao.saveTrack(track.get()));
    EXPECT_TRUE(track->isDirty());
    EXPECT_TRUE(cue->isDirty());
    EXPECT_EQ(cueId, cue->getId());
    EXPECT_EQ(0, cleanSignals.count());
    EXPECT_EQ(QStringLiteral("original title"),
            readValue(QStringLiteral("SELECT title FROM library WHERE id=?"),
                    {id.toVariant()})
                    .toString());
    EXPECT_EQ(QStringLiteral("original cue"),
            readValue(QStringLiteral("SELECT label FROM cues WHERE id=?"),
                    {cueId.toVariant()})
                    .toString());
    ASSERT_TRUE(execute(QStringLiteral("DROP TRIGGER b_reject_update")));
    ASSERT_TRUE(dao.saveTrack(track.get()));
    EXPECT_FALSE(track->isDirty());
    EXPECT_FALSE(cue->isDirty());
}

TEST_F(TrackDAOTest, CommitFailureKeepsNewCueUnassigned) {
    auto& dao = internalCollection()->getTrackDAO();
    const auto track = newTrack(QStringLiteral("update-commit.wav"), QStringLiteral("old cue"));
    const auto id = internalCollection()->addTrack(track, false);
    ASSERT_TRUE(id.isValid());
    CuePointer newCue(new Cue(mixxx::CueType::HotCue,
            0,
            mixxx::audio::FramePos(0),
            mixxx::audio::kInvalidFramePos,
            mixxx::RgbColor(0)));
    newCue->setLabel(QStringLiteral("black cue at zero"));
    auto cues = track->getCuePoints();
    cues.append(newCue);
    track->setCuePoints(cues);
    track->setTitle(QStringLiteral("updated title"));
    SqliteCommitFailure failure(dbConnection());
    if (!failure.isValid()) {
        GTEST_SKIP() << "Native SQLite commit hook is unavailable";
    }

    EXPECT_FALSE(dao.saveTrack(track.get()));
    EXPECT_TRUE(track->isDirty());
    EXPECT_FALSE(newCue->getId().isValid());
    EXPECT_TRUE(newCue->isDirty());
    EXPECT_EQ(1, failure.attempts());
    EXPECT_EQ(1, readValue(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt());
    EXPECT_EQ(QStringLiteral("original title"),
            readValue(QStringLiteral("SELECT title FROM library WHERE id=?"),
                    {id.toVariant()})
                    .toString());

    failure.disable();
    ASSERT_TRUE(dao.saveTrack(track.get()));
    EXPECT_FALSE(track->isDirty());
    EXPECT_TRUE(newCue->getId().isValid());
    EXPECT_FALSE(newCue->isDirty());
    EXPECT_EQ(2, readValue(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt());
}

TEST_F(TrackDAOTest, AddCueFailureDoesNotPublishIds) {
    auto& dao = internalCollection()->getTrackDAO();
    const auto track = newTrack(QStringLiteral("add-cue.wav"), QStringLiteral("reject"));
    const auto cue = track->getCuePoints().front();
    ASSERT_TRUE(execute(QStringLiteral(
            "CREATE TRIGGER b_reject_insert BEFORE INSERT ON cues "
            "BEGIN SELECT RAISE(ABORT,'injected cue insert failure'); END")));
    QSignalSpy addedSignals(&dao, &TrackDAO::tracksAdded);
    EXPECT_FALSE(internalCollection()->addTrack(track, false).isValid());
    EXPECT_FALSE(track->getId().isValid());
    EXPECT_FALSE(track->getDateAdded().isValid());
    EXPECT_FALSE(cue->getId().isValid());
    EXPECT_TRUE(track->isDirty());
    EXPECT_TRUE(cue->isDirty());
    EXPECT_EQ(0, addedSignals.count());
    EXPECT_EQ(0, readValue(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    EXPECT_EQ(0, readValue(QStringLiteral("SELECT COUNT(*) FROM track_locations")).toInt());
    ASSERT_TRUE(execute(QStringLiteral("DROP TRIGGER b_reject_insert")));
    EXPECT_TRUE(internalCollection()->addTrack(track, false).isValid());
    EXPECT_FALSE(track->isDirty());
    EXPECT_FALSE(cue->isDirty());
    EXPECT_EQ(1, addedSignals.count());
}

TEST_F(TrackDAOTest, AddCommitFailureDoesNotPublishIds) {
    auto& dao = internalCollection()->getTrackDAO();
    const auto track = newTrack(QStringLiteral("add-commit.wav"), QStringLiteral("cue"));
    const auto cue = track->getCuePoints().front();
    SqliteCommitFailure failure(dbConnection());
    if (!failure.isValid()) {
        GTEST_SKIP() << "Native SQLite commit hook is unavailable";
    }
    QSignalSpy addedSignals(&dao, &TrackDAO::tracksAdded);
    EXPECT_FALSE(internalCollection()->addTrack(track, false).isValid());
    EXPECT_FALSE(track->getId().isValid());
    EXPECT_FALSE(track->getDateAdded().isValid());
    EXPECT_FALSE(cue->getId().isValid());
    EXPECT_TRUE(track->isDirty());
    EXPECT_TRUE(cue->isDirty());
    EXPECT_EQ(0, addedSignals.count());
    EXPECT_EQ(0, readValue(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    EXPECT_EQ(0, readValue(QStringLiteral("SELECT COUNT(*) FROM track_locations")).toInt());
    EXPECT_EQ(1, failure.attempts());
    failure.disable();
    EXPECT_TRUE(internalCollection()->addTrack(track, false).isValid());
    EXPECT_TRUE(track->getId().isValid());
    EXPECT_TRUE(cue->getId().isValid());
    EXPECT_FALSE(track->isDirty());
    EXPECT_EQ(1, addedSignals.count());
}

TEST_F(TrackDAOTest, BatchRollbackDiscardsEarlierSuccessfulTrack) {
    auto& dao = internalCollection()->getTrackDAO();
    const auto first = newTrack(QStringLiteral("batch-first.wav"), QStringLiteral("first"));
    const auto second = newTrack(QStringLiteral("batch-second.wav"), QStringLiteral("reject"));
    ASSERT_TRUE(execute(QStringLiteral(
            "CREATE TRIGGER b_reject_insert BEFORE INSERT ON cues "
            "WHEN NEW.label='reject' BEGIN SELECT RAISE(ABORT,'injected failure'); END")));
    QSignalSpy addedSignals(&dao, &TrackDAO::tracksAdded);
    ASSERT_TRUE(dao.addTracksPrepare());
    const auto provisional = dao.addTracksAddTrack(first, false);
    EXPECT_TRUE(provisional.isValid());
    EXPECT_FALSE(first->getId().isValid());
    EXPECT_FALSE(first->getCuePoints().front()->getId().isValid());
    EXPECT_FALSE(dao.addTracksAddTrack(second, false).isValid());
    EXPECT_FALSE(dao.addTracksFinish());
    EXPECT_FALSE(first->getId().isValid());
    EXPECT_FALSE(second->getId().isValid());
    EXPECT_TRUE(first->isDirty());
    EXPECT_TRUE(second->isDirty());
    EXPECT_EQ(0, addedSignals.count());
    EXPECT_EQ(0, readValue(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());
    EXPECT_EQ(0, readValue(QStringLiteral("SELECT COUNT(*) FROM cues")).toInt());
    EXPECT_EQ(0, readValue(QStringLiteral("SELECT COUNT(*) FROM track_locations")).toInt());
}

TEST_F(TrackDAOTest, SuccessfulBatchPublishesAfterCommit) {
    auto& dao = internalCollection()->getTrackDAO();
    const auto track = newTrack(QStringLiteral("batch-success.wav"), QStringLiteral("cue"));
    const auto cue = track->getCuePoints().front();
    bool signalWasAfterCommit = false;
    const auto connection = QObject::connect(
            &dao, &TrackDAO::tracksAdded, &dao, [&](const QSet<TrackId>& ids) {
                auto database = dbConnection();
                signalWasAfterCommit = database.transaction();
                EXPECT_TRUE(signalWasAfterCommit);
                if (signalWasAfterCommit) {
                    EXPECT_TRUE(database.rollback());
                }
                EXPECT_TRUE(ids.contains(track->getId()));
                EXPECT_FALSE(track->isDirty());
                EXPECT_FALSE(cue->isDirty());
            });
    ASSERT_TRUE(dao.addTracksPrepare());
    const auto id = dao.addTracksAddTrack(track, false);
    ASSERT_TRUE(id.isValid());
    EXPECT_FALSE(track->getId().isValid());
    EXPECT_FALSE(track->getDateAdded().isValid());
    EXPECT_FALSE(cue->getId().isValid());
    EXPECT_TRUE(track->isDirty());
    QList<TrackPointer> added;
    EXPECT_TRUE(dao.addTracksFinish(false, &added));
    EXPECT_TRUE(signalWasAfterCommit);
    EXPECT_EQ(id, track->getId());
    EXPECT_TRUE(track->getDateAdded().isValid());
    EXPECT_TRUE(cue->getId().isValid());
    EXPECT_THAT(added, UnorderedElementsAre(track));
    QObject::disconnect(connection);
}

TEST_F(TrackDAOTest, CachedTrackIdIsPublishedOnlyAfterSuccessfulBatchCommit) {
    auto& dao = internalCollection()->getTrackDAO();
    const auto path = getTestDataDir().filePath(QStringLiteral("cache-commit.wav"));
    ASSERT_TRUE(QFile::copy(getTestDir().filePath(QStringLiteral("sine-30.wav")), path));
    SqliteCommitFailure failure(dbConnection());
    if (!failure.isValid()) {
        GTEST_SKIP() << "Native SQLite commit hook is unavailable";
    }
    ASSERT_TRUE(dao.addTracksPrepare());
    auto track = dao.addTracksAddFile(path, false);
    ASSERT_TRUE(track);
    EXPECT_FALSE(track->getId().isValid());
    const TrackId provisional(readValue(QStringLiteral("SELECT id FROM library")));
    ASSERT_TRUE(provisional.isValid());
    EXPECT_FALSE(GlobalTrackCacheLocker().lookupTrackById(provisional));
    EXPECT_FALSE(dao.addTracksFinish());
    EXPECT_EQ(1, failure.attempts());
    EXPECT_FALSE(track->getId().isValid());
    EXPECT_FALSE(GlobalTrackCacheLocker().lookupTrackById(provisional));
    EXPECT_EQ(0, readValue(QStringLiteral("SELECT COUNT(*) FROM library")).toInt());

    failure.disable();
    ASSERT_TRUE(dao.addTracksPrepare());
    const auto retry = dao.addTracksAddFile(path, false);
    ASSERT_EQ(track, retry);
    EXPECT_FALSE(track->getId().isValid());
    ASSERT_TRUE(dao.addTracksFinish());
    EXPECT_TRUE(track->getId().isValid());
    EXPECT_EQ(track, GlobalTrackCacheLocker().lookupTrackById(track->getId()));
    EXPECT_FALSE(track->isDirty());
}

#ifdef __SQLITE3__
TEST_F(TrackDAOTest, EditDuringSqlWriteRemainsDirtyAndRetries) {
    auto& dao = internalCollection()->getTrackDAO();
    const auto track = newTrack(QStringLiteral("concurrent-edit.wav"), QStringLiteral("cue"));
    const auto id = internalCollection()->addTrack(track, false);
    ASSERT_TRUE(id.isValid());
    auto handle = dbConnection().driver()->handle();
    ASSERT_STREQ("sqlite3*", handle.typeName());
    auto* connection = *static_cast<sqlite3**>(handle.data());
    ASSERT_EQ(SQLITE_OK,
            sqlite3_create_function_v2(
                    connection,
                    "b_edit_track",
                    0,
                    SQLITE_UTF8,
                    track.get(),
                    [](sqlite3_context* context, int, sqlite3_value**) {
                        auto* trackToEdit =
                                static_cast<Track*>(sqlite3_user_data(context));
                        trackToEdit->setTitle(
                                QStringLiteral("edited while saving"));
                        sqlite3_result_int(context, 0);
                    },
                    nullptr,
                    nullptr,
                    nullptr));
    ASSERT_TRUE(execute(QStringLiteral(
            "CREATE TRIGGER b_concurrent_edit BEFORE UPDATE ON library "
            "BEGIN SELECT b_edit_track(); END")));
    track->setTitle(QStringLiteral("snapshot title"));
    EXPECT_FALSE(dao.saveTrack(track.get()));
    EXPECT_TRUE(track->isDirty());
    EXPECT_EQ(QStringLiteral("edited while saving"), track->getTitle());
    EXPECT_EQ(QStringLiteral("snapshot title"),
            readValue(QStringLiteral("SELECT title FROM library WHERE id=?"),
                    {id.toVariant()})
                    .toString());
    ASSERT_TRUE(execute(QStringLiteral("DROP TRIGGER b_concurrent_edit")));
    ASSERT_TRUE(dao.saveTrack(track.get()));
    EXPECT_FALSE(track->isDirty());
    EXPECT_EQ(QStringLiteral("edited while saving"),
            readValue(QStringLiteral("SELECT title FROM library WHERE id=?"),
                    {id.toVariant()})
                    .toString());
    EXPECT_EQ(SQLITE_OK,
            sqlite3_create_function_v2(connection,
                    "b_edit_track",
                    0,
                    SQLITE_UTF8,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr));
}
#endif
// End of ChatGPT-prepared code for human review.
