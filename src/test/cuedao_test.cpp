// Prepared by ChatGPT for human review.
#include "library/dao/cuedao.h"

#include <gtest/gtest.h>

#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <memory>
#include <stdexcept>

#include "util/db/sqltransaction.h"

namespace {
using mixxx::CueType;
using mixxx::RgbColor;
using mixxx::audio::FramePos;

class CueDatabase {
  public:
    CueDatabase()
            : name(QUuid::createUuid().toString()) {
        if (!directory.isValid() || !open() ||
                !exec(QStringLiteral("PRAGMA foreign_keys=ON")) ||
                !exec(QStringLiteral("CREATE TABLE tracks(id INTEGER PRIMARY KEY)")) ||
                !exec(QStringLiteral("INSERT INTO tracks VALUES(10),(11)")) ||
                !exec(QStringLiteral("CREATE TABLE cues(id INTEGER PRIMARY KEY,track_id INTEGER,"
                                     "type INTEGER,position REAL,length REAL,hotcue INTEGER,"
                                     "label TEXT NOT NULL,color INTEGER,FOREIGN KEY(track_id) "
                                     "REFERENCES tracks(id) DEFERRABLE INITIALLY DEFERRED)"))) {
            throw std::runtime_error(error.toStdString());
        }
    }

    ~CueDatabase() {
        close();
    }

    bool exec(const QString& sql) {
        QSqlQuery query(db);
        const bool ok = query.exec(sql);
        error = query.lastError().text();
        return ok;
    }

    int count(const QString& table = QStringLiteral("cues")) const {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM ") + table) || !query.next()) {
            return -1;
        }
        return query.value(0).toInt();
    }

    bool reopen() {
        close();
        return open() && exec(QStringLiteral("PRAGMA foreign_keys=ON"));
    }

    QSqlDatabase db;
    std::unique_ptr<CueDAO> dao;
    QString error;

  private:
    bool open() {
        db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(directory.filePath(QStringLiteral("cues.sqlite")));
        if (!db.open()) {
            error = db.lastError().text();
            return false;
        }
        dao = std::make_unique<CueDAO>();
        dao->initialize(db);
        return true;
    }

    void close() {
        dao.reset();
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }

    QTemporaryDir directory;
    const QString name;
};

TrackId trackId() {
    return TrackId(QVariant(10));
}

CuePointer memoryCue(const QString& label) {
    CuePointer cue(new Cue(CueType::Memory,
            Cue::kNoHotCue,
            FramePos(48000),
            mixxx::audio::kInvalidFramePos,
            RgbColor(0x123456)));
    cue->setLabel(label);
    return cue;
}

TEST(CueDAOTest, BlackHotCueAtFrameZeroSurvivesCloseReopen) {
    CueDatabase database;
    CuePointer cue(new Cue(CueType::HotCue,
            0,
            FramePos(0),
            mixxx::audio::kInvalidFramePos,
            RgbColor(0)));
    cue->setLabel(QString::fromUtf8("黒 / intentional zero"));
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {cue}));
    ASSERT_TRUE(database.reopen());
    const auto restored = database.dao->getCuesForTrack(trackId());
    ASSERT_EQ(1, restored.size());
    EXPECT_EQ(CueType::HotCue, restored[0]->getType());
    EXPECT_EQ(0, restored[0]->getHotCue());
    EXPECT_EQ(FramePos(0), restored[0]->getPosition());
    EXPECT_EQ(RgbColor(0), restored[0]->getColor());
    EXPECT_EQ(cue->getLabel(), restored[0]->getLabel());
}

TEST(CueDAOTest, FailedSecondInsertRollsBackFirstInsertAndKeepsOriginalCues) {
    CueDatabase database;
    const auto original = memoryCue(QStringLiteral("original"));
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {original}));
    const auto first = memoryCue(QStringLiteral("first"));
    const auto second = memoryCue(QStringLiteral("reject"));
    ASSERT_TRUE(database.exec(QStringLiteral(
            "CREATE TRIGGER reject_insert BEFORE INSERT ON cues "
            "WHEN NEW.label='reject' BEGIN SELECT RAISE(ABORT,'injected insert failure'); END")));
    EXPECT_FALSE(database.dao->saveTrackCues(trackId(), {first, second}));
    EXPECT_FALSE(first->getId().isValid());
    EXPECT_FALSE(second->getId().isValid());
    EXPECT_TRUE(first->isDirty());
    EXPECT_TRUE(second->isDirty());
    const auto restored = database.dao->getCuesForTrack(trackId());
    ASSERT_EQ(1, restored.size());
    EXPECT_EQ(original->getId(), restored[0]->getId());
    EXPECT_EQ(QStringLiteral("original"), restored[0]->getLabel());
    ASSERT_TRUE(database.exec(QStringLiteral("DROP TRIGGER reject_insert")));
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {first, second}));
    ASSERT_TRUE(database.reopen());
    EXPECT_EQ(2, database.dao->getCuesForTrack(trackId()).size());
}

TEST(CueDAOTest, FailedUpdateDoesNotDeleteOmittedCues) {
    CueDatabase database;
    const auto changed = memoryCue(QStringLiteral("before"));
    const auto retained = memoryCue(QStringLiteral("retain"));
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {changed, retained}));
    const auto originalId = changed->getId();
    changed->setLabel(QStringLiteral("after"));
    ASSERT_TRUE(database.exec(QStringLiteral(
            "CREATE TRIGGER reject_update BEFORE UPDATE ON cues "
            "BEGIN SELECT RAISE(ABORT,'injected update failure'); END")));
    EXPECT_FALSE(database.dao->saveTrackCues(trackId(), {changed}));
    EXPECT_EQ(originalId, changed->getId());
    EXPECT_TRUE(changed->isDirty());
    EXPECT_EQ(2, database.count());
    const auto restored = database.dao->getCuesForTrack(trackId());
    ASSERT_EQ(2, restored.size());
    EXPECT_EQ(QStringLiteral("before"), restored[0]->getLabel());
}

TEST(CueDAOTest, FailedOrphanDeletionRollsBackEarlierWrites) {
    CueDatabase database;
    const auto changed = memoryCue(QStringLiteral("before"));
    const auto retained = memoryCue(QStringLiteral("retain"));
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {changed, retained}));
    changed->setLabel(QStringLiteral("after"));
    const auto added = memoryCue(QStringLiteral("added"));
    ASSERT_TRUE(database.exec(QStringLiteral(
            "CREATE TRIGGER reject_delete BEFORE DELETE ON cues "
            "WHEN OLD.label='retain' BEGIN SELECT RAISE(ABORT,'injected delete failure'); END")));
    EXPECT_FALSE(database.dao->saveTrackCues(trackId(), {changed, added}));
    EXPECT_TRUE(changed->isDirty());
    EXPECT_TRUE(added->isDirty());
    EXPECT_FALSE(added->getId().isValid());
    ASSERT_TRUE(database.reopen());
    const auto restored = database.dao->getCuesForTrack(trackId());
    ASSERT_EQ(2, restored.size());
    EXPECT_EQ(QStringLiteral("before"), restored[0]->getLabel());
    EXPECT_EQ(QStringLiteral("retain"), restored[1]->getLabel());
}

TEST(CueDAOTest, FailedCommitKeepsCueIdsDirtyFlagsAndDatabaseUnchanged) {
    CueDatabase database;
    const auto original = memoryCue(QStringLiteral("before"));
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {original}));
    const auto originalId = original->getId();
    original->setLabel(QStringLiteral("after"));
    const auto added = memoryCue(QStringLiteral("added"));
    ASSERT_TRUE(database.exec(QStringLiteral(
            "CREATE TABLE commit_guard(value INTEGER,"
            "FOREIGN KEY(value) REFERENCES tracks(id) DEFERRABLE INITIALLY DEFERRED)")));
    ASSERT_TRUE(database.exec(QStringLiteral(
            "CREATE TRIGGER fail_commit AFTER UPDATE ON cues "
            "BEGIN INSERT INTO commit_guard VALUES(999); END")));
    EXPECT_FALSE(database.dao->saveTrackCues(trackId(), {original, added}));
    EXPECT_EQ(originalId, original->getId());
    EXPECT_TRUE(original->isDirty());
    EXPECT_FALSE(added->getId().isValid());
    EXPECT_TRUE(added->isDirty());
    EXPECT_EQ(0, database.count(QStringLiteral("commit_guard")));
    ASSERT_TRUE(database.reopen());
    const auto restored = database.dao->getCuesForTrack(trackId());
    ASSERT_EQ(1, restored.size());
    EXPECT_EQ(QStringLiteral("before"), restored[0]->getLabel());
}

TEST(CueDAOTest, CallerRollbackDoesNotPublishPendingCueState) {
    CueDatabase database;
    const auto cue = memoryCue(QStringLiteral("pending"));
    SqlTransaction transaction(database.db);
    ASSERT_TRUE(transaction);
    auto pending = database.dao->prepareTrackCues(trackId(), {cue});
    ASSERT_TRUE(pending);
    EXPECT_EQ(1, database.count());
    EXPECT_FALSE(cue->getId().isValid());
    EXPECT_TRUE(cue->isDirty());
    ASSERT_TRUE(transaction.rollback());
    pending.reset();
    EXPECT_EQ(0, database.count());
    EXPECT_FALSE(cue->getId().isValid());
    EXPECT_TRUE(cue->isDirty());
}

TEST(CueDAOTest, CallerFinalizesOnlyAfterCommit) {
    CueDatabase database;
    const auto cue = memoryCue(QStringLiteral("pending"));
    SqlTransaction transaction(database.db);
    ASSERT_TRUE(transaction);
    auto pending = database.dao->prepareTrackCues(trackId(), {cue});
    ASSERT_TRUE(pending);
    ASSERT_TRUE(transaction.commit());
    EXPECT_FALSE(cue->getId().isValid());
    EXPECT_TRUE(cue->isDirty());
    EXPECT_TRUE(pending->finalize());
    EXPECT_TRUE(cue->getId().isValid());
    EXPECT_FALSE(cue->isDirty());
}

TEST(CueDAOTest, ConcurrentEditWhileSavePendingRemainsDirty) {
    CueDatabase database;
    const auto cue = memoryCue(QStringLiteral("saved snapshot"));
    {
        SqlTransaction transaction(database.db);
        ASSERT_TRUE(transaction);
        auto pending = database.dao->prepareTrackCues(trackId(), {cue});
        ASSERT_TRUE(pending);
        cue->setLabel(QStringLiteral("later user edit"));
        ASSERT_TRUE(transaction.commit());
        EXPECT_FALSE(pending->finalize());
        EXPECT_TRUE(cue->getId().isValid());
        EXPECT_TRUE(cue->isDirty());
        EXPECT_EQ(QStringLiteral("later user edit"), cue->getLabel());
        const auto restored = database.dao->getCuesForTrack(trackId());
        ASSERT_EQ(1, restored.size());
        EXPECT_EQ(QStringLiteral("saved snapshot"), restored[0]->getLabel());
    }
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {cue}));
    ASSERT_TRUE(database.reopen());
    EXPECT_EQ(QStringLiteral("later user edit"),
            database.dao->getCuesForTrack(trackId())[0]->getLabel());
}

TEST(CueDAOTest, SingleCueWriteDoesNotDeleteOthersOrPublishState) {
    CueDatabase database;
    const auto first = memoryCue(QStringLiteral("first"));
    const auto second = memoryCue(QStringLiteral("second"));
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {first, second}));
    const auto added = memoryCue(QStringLiteral("adapter"));
    SqlTransaction transaction(database.db);
    ASSERT_TRUE(transaction);
    const auto assigned = database.dao->writeCue(trackId(), *added);
    ASSERT_TRUE(assigned);
    EXPECT_EQ(3, database.count());
    EXPECT_FALSE(added->getId().isValid());
    EXPECT_TRUE(added->isDirty());
    ASSERT_TRUE(transaction.rollback());
    EXPECT_EQ(2, database.count());
}

TEST(CueDAOTest, MissingOrOtherTrackCueIdIsAnExplicitWriteFailure) {
    CueDatabase database;
    const auto cue = memoryCue(QStringLiteral("owned by10"));
    ASSERT_TRUE(database.dao->saveTrackCues(trackId(), {cue}));
    EXPECT_FALSE(database.dao->writeCue(TrackId(QVariant(11)), *cue));
    Cue missing(DbId(QVariant(999)),
            CueType::HotCue,
            FramePos(48000),
            0,
            1,
            QStringLiteral("missing"),
            RgbColor(0));
    EXPECT_FALSE(database.dao->writeCue(trackId(), missing));
    EXPECT_EQ(1, database.count());
    EXPECT_EQ(QStringLiteral("owned by10"),
            database.dao->getCuesForTrack(trackId())[0]->getLabel());
}

TEST(CueDAOTest, NestedStandaloneSaveDoesNotCommitCallerTransaction) {
    CueDatabase database;
    const auto cue = memoryCue(QStringLiteral("pending"));
    SqlTransaction transaction(database.db);
    ASSERT_TRUE(transaction);
    EXPECT_FALSE(database.dao->saveTrackCues(trackId(), {cue}));
    EXPECT_FALSE(cue->getId().isValid());
    EXPECT_EQ(0, database.count());
    EXPECT_TRUE(transaction.rollback());
}

TEST(CueDAOTest, DuplicatePointersAreRejectedBeforeWriting) {
    CueDatabase database;
    const auto cue = memoryCue(QStringLiteral("duplicate"));
    EXPECT_FALSE(database.dao->saveTrackCues(trackId(), {cue, cue}));
    EXPECT_FALSE(cue->getId().isValid());
    EXPECT_EQ(0, database.count());
}
} // namespace
// End of ChatGPT-prepared tests for human review.
