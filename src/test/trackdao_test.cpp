#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "library/coverartcache.h"
#include "library/library_prefs.h"
#include "test/librarytest.h"
#include "track/globaltrackcache.h"
#include "track/track.h"

using ::testing::UnorderedElementsAre;

class TrackDAOTest : public LibraryTest {
};

class TrackDAOPersistenceTest : public MixxxTest, SoundSourceProviderRegistration {
  protected:
    TrackDAOPersistenceTest()
            : m_keyNotationCO(mixxx::library::prefs::kKeyNotationConfigKey) {
        CoverArtCache::createInstance();
    }
    ~TrackDAOPersistenceTest() override {
        CoverArtCache::destroy();
    }

  private:
    ControlObject m_keyNotationCO;
};

TEST_F(TrackDAOPersistenceTest, AnnotationsSurviveDatabaseCloseAndReopen) {
    using namespace mixxx;
    const QStringList locations{
            getTestDataDir().filePath(QStringLiteral("constant.wav")),
            getTestDataDir().filePath(QStringLiteral("variable.wav"))};
    for (const auto& location : locations) {
        ASSERT_TRUE(QFile::copy(getTestDir().filePath(QStringLiteral("sine-30.wav")), location));
    }
    QVector<TrackId> trackIds;

    for (int pass = 0; pass < 4; ++pass) {
        QString connectionName;
        {
            MixxxDb database(config(), false);
            DbConnectionPooler pooler(database.connectionPool());
            const QSqlDatabase connection = DbConnectionPooled(pooler);
            ASSERT_TRUE(connection.isOpen());
            connectionName = connection.connectionName();
            ASSERT_TRUE(MixxxDb::initDatabaseSchema(connection));
            TrackCollectionManager manager(nullptr, config(), pooler, [](Track* pTrack) {
                delete pTrack;
            });

            for (int i = 0; i < locations.size(); ++i) {
                auto pTrack = pass == 0
                        ? manager.getOrAddTrack(TrackRef::fromFilePath(locations[i]))
                        : manager.getTrackById(trackIds[i]);
                ASSERT_TRUE(pTrack);
                if (pass == 0) {
                    trackIds.append(pTrack->getId());
                    ASSERT_TRUE(trackIds.back().isValid());
                    const auto sampleRate = pTrack->getSampleRate();
                    ASSERT_TRUE(sampleRate.isValid());
                    const auto pBeats = i == 0
                            ? Beats::fromConstTempo(sampleRate,
                                      audio::kStartFramePos,
                                      Bpm(120.0),
                                      QString(),
                                      3)
                            : Beats::fromBeatPositions(sampleRate,
                                      {audio::FramePos(0),
                                              audio::FramePos(10000),
                                              audio::FramePos(25000)},
                                      QString(),
                                      3);
                    ASSERT_TRUE(pTrack->trySetBeats(pBeats));
                    pTrack->setCuePoints({});
                    const auto pMemoryCue = pTrack->createAndAddCue(CueType::Memory,
                            Cue::kNoHotCue,
                            audio::FramePos(48000),
                            audio::kInvalidFramePos,
                            RgbColor(0x123456));
                    pMemoryCue->setLabel(QStringLiteral("memory cue"));
                    const auto pMemoryLoop = pTrack->createAndAddCue(CueType::Memory,
                            Cue::kNoHotCue,
                            audio::FramePos(48000),
                            audio::FramePos(96000),
                            RgbColor(0x234567));
                    pMemoryLoop->setLabel(QStringLiteral("memory loop"));
                    pMemoryLoop->setActiveLoopIntent(true);
                    const auto pHotLoop = pTrack->createAndAddCue(CueType::Loop,
                            0,
                            audio::FramePos(48000),
                            audio::FramePos(96000),
                            RgbColor(0x345678));
                    pHotLoop->setLabel(QStringLiteral("hot loop"));
                    pHotLoop->setActiveLoopIntent(false);
                    const auto pHotCue = pTrack->createAndAddCue(CueType::HotCue,
                            7,
                            audio::FramePos(72000),
                            audio::kInvalidFramePos,
                            RgbColor(0x456789));
                    pHotCue->setLabel(QStringLiteral("hot cue"));
                } else {
                    EXPECT_EQ(trackIds[i], pTrack->getId());
                    if (pass < 3) {
                        ASSERT_TRUE(pTrack->getBeats());
                        EXPECT_EQ(pass == 1 ? 3 : 2, pTrack->getDownbeatOffset());
                        EXPECT_EQ(pTrack->getDownbeatOffset(),
                                pTrack->getBeats()->getDownbeatsOffset());
                        EXPECT_EQ(i == 0, pTrack->getBeats()->hasConstantTempo());
                        EXPECT_EQ(pass == 2, pTrack->isBpmLocked());
                    } else {
                        EXPECT_FALSE(pTrack->getBeats());
                        EXPECT_EQ(0, pTrack->getDownbeatOffset());
                    }

                    const auto cues = pTrack->getCuePoints();
                    ASSERT_EQ(4, cues.size());
                    int memoryCues = 0;
                    for (const auto& pCue : cues) {
                        if (pCue->getType() != CueType::Memory) {
                            continue;
                        }
                        ++memoryCues;
                        EXPECT_EQ(Cue::kNoHotCue, pCue->getHotCue());
                        EXPECT_EQ(audio::FramePos(48000), pCue->getPosition());
                        if (pCue->getEndPosition().isValid()) {
                            EXPECT_EQ(audio::FramePos(96000), pCue->getEndPosition());
                            EXPECT_EQ(QStringLiteral("memory loop"), pCue->getLabel());
                            EXPECT_EQ(RgbColor(0x234567), pCue->getColor());
                            EXPECT_EQ(std::optional<bool>(true), pCue->getActiveLoopIntent());
                        } else {
                            EXPECT_EQ(QStringLiteral("memory cue"), pCue->getLabel());
                            EXPECT_EQ(RgbColor(0x123456), pCue->getColor());
                            EXPECT_FALSE(pCue->getActiveLoopIntent().has_value());
                        }
                    }
                    EXPECT_EQ(2, memoryCues);

                    const auto pSlot0 = pTrack->findHotcueByIndex(0);
                    const auto pSlot7 = pTrack->findHotcueByIndex(7);
                    ASSERT_TRUE(pSlot0);
                    ASSERT_TRUE(pSlot7);
                    EXPECT_FALSE(pTrack->findHotcueByIndex(1));
                    EXPECT_EQ(CueType::Loop, pSlot0->getType());
                    EXPECT_EQ(CueType::HotCue, pSlot7->getType());
                    EXPECT_EQ(audio::FramePos(pass == 1 ? 48000 : 72000), pSlot0->getPosition());
                    EXPECT_EQ(audio::FramePos(96000), pSlot0->getEndPosition());
                    EXPECT_EQ(std::optional<bool>(false), pSlot0->getActiveLoopIntent());
                    EXPECT_EQ(audio::FramePos(pass == 1 ? 72000 : 48000), pSlot7->getPosition());
                    EXPECT_FALSE(pSlot7->getEndPosition().isValid());
                    EXPECT_FALSE(pSlot7->getActiveLoopIntent().has_value());
                    EXPECT_EQ(QStringLiteral("hot loop"),
                            pass == 1 ? pSlot0->getLabel() : pSlot7->getLabel());
                    EXPECT_EQ(QStringLiteral("hot cue"),
                            pass == 1 ? pSlot7->getLabel() : pSlot0->getLabel());
                    EXPECT_EQ(RgbColor(0x345678),
                            pass == 1 ? pSlot0->getColor() : pSlot7->getColor());
                    EXPECT_EQ(RgbColor(0x456789),
                            pass == 1 ? pSlot7->getColor() : pSlot0->getColor());

                    if (pass == 1) {
                        pTrack->swapHotcues(0, 7);
                        pSlot0->setType(CueType::HotCue);
                        pSlot0->setEndPosition(audio::kInvalidFramePos);
                        pSlot0->setActiveLoopIntent(std::nullopt);
                        pSlot7->setType(CueType::Loop);
                        pSlot7->setEndPosition(audio::FramePos(96000));
                        pSlot7->setActiveLoopIntent(false);
                        pTrack->setDownbeatOffset(2);
                        pTrack->setBpmLocked(true);
                    } else if (pass == 2) {
                        pTrack->setBpmLocked(false);
                        ASSERT_TRUE(pTrack->trySetBeats(nullptr));
                    }
                }
                if (pass < 3) {
                    EXPECT_EQ(TrackCollectionManager::SaveTrackResult::Saved,
                            manager.saveTrack(pTrack));
                }
            }
            EXPECT_TRUE(GlobalTrackCacheLocker().isEmpty());
        }
        EXPECT_FALSE(QSqlDatabase::contains(connectionName));
    }
}

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
