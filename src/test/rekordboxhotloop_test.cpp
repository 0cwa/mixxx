#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <limits>

#include "library/rekordbox/rekordboxparser_test.h"
#include "rekordbox_hotloop_test_fixtures.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

class RekordboxHotLoopParserTest : public ::testing::TestWithParam<bool> {
  protected:
    QByteArray fixture(quint8 type = 2,
            quint32 endTime = 2000,
            quint32 number = 1,
            quint32 listType = 1) {
        return mixxx::rekordbox::test::hotloop::makeAnlzCueFixture(GetParam(),
                listType,
                number,
                type,
                1000,
                endTime,
                QStringLiteral("fixture"),
                qRgb(0x12, 0x34, 0x56));
    }

    void parse(const TrackPointer& track,
            const QByteArray& bytes,
            int offset = 0,
            bool ignoreCues = false,
            mixxx::audio::SampleRate rate = mixxx::audio::SampleRate(48000)) {
        ASSERT_TRUE(m_directory.isValid());
        const QString path = m_directory.filePath(QStringLiteral("synthetic.anlz"));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(bytes.size(), file.write(bytes));
        file.close();
        EXPECT_NO_THROW(mixxx::rekordbox::test::readAnalyzeForTest(
                track, rate, offset, ignoreCues, path));
    }

    CuePointer seed(const TrackPointer& track, mixxx::CueType type = mixxx::CueType::Loop) {
        auto cue = track->createAndAddCue(type,
                0,
                mixxx::audio::FramePos(123),
                mixxx::audio::FramePos(456));
        cue->setLabel(QStringLiteral("seed"));
        cue->setColor(mixxx::RgbColor(0xabcdef));
        return cue;
    }

    void expectUnchanged(const TrackPointer& track, const CuePointer& cue) {
        ASSERT_EQ(track->getCuePoints().size(), 1);
        EXPECT_EQ(track->getCuePoints().front(), cue);
        EXPECT_EQ(cue->getType(), mixxx::CueType::Loop);
        EXPECT_EQ(cue->getHotCue(), 0);
        EXPECT_EQ(cue->getPosition(), mixxx::audio::FramePos(123));
        EXPECT_EQ(cue->getEndPosition(), mixxx::audio::FramePos(456));
        EXPECT_EQ(cue->getLabel(), QStringLiteral("seed"));
        EXPECT_EQ(cue->getColor(), mixxx::RgbColor(0xabcdef));
    }

  private:
    QTemporaryDir m_directory;
};

TEST_P(RekordboxHotLoopParserTest, CreatesHotLoopWithBoundsAndSlot) {
    const auto track = Track::newTemporary();
    parse(track, fixture());
    ASSERT_EQ(track->getCuePoints().size(), 1);
    const auto cue = track->getCuePoints().front();
    EXPECT_EQ(cue->getType(), mixxx::CueType::Loop);
    EXPECT_EQ(cue->getHotCue(), 0);
    EXPECT_EQ(cue->getPosition(), mixxx::audio::FramePos(48000));
    EXPECT_EQ(cue->getEndPosition(), mixxx::audio::FramePos(96000));
    EXPECT_EQ(cue->getLabel(), GetParam() ? QStringLiteral("fixture") : QString());
    if (GetParam()) {
        EXPECT_EQ(cue->getColor(), mixxx::RgbColor(0x123456));
    }
}

TEST_P(RekordboxHotLoopParserTest, ReimportsExistingCueWithoutReordering) {
    for (const auto type : {mixxx::CueType::HotCue, mixxx::CueType::Loop}) {
        SCOPED_TRACE(static_cast<int>(type));
        const auto track = Track::newTemporary();
        const auto first = seed(track, type);
        const auto duplicate = track->createAndAddCue(mixxx::CueType::HotCue,
                0,
                mixxx::audio::FramePos(789),
                mixxx::audio::kInvalidFramePos);
        parse(track, fixture());
        ASSERT_EQ(track->getCuePoints().size(), 2);
        EXPECT_EQ(track->getCuePoints().at(0), first);
        EXPECT_EQ(track->getCuePoints().at(1), duplicate);
        EXPECT_EQ(first->getType(), mixxx::CueType::Loop);
        EXPECT_EQ(first->getPosition(), mixxx::audio::FramePos(48000));
        EXPECT_EQ(first->getEndPosition(), mixxx::audio::FramePos(96000));
        EXPECT_EQ(first->getLabel(), GetParam() ? QStringLiteral("fixture") : QString());
        EXPECT_EQ(first->getColor(), mixxx::RgbColor(GetParam() ? 0x123456 : 0xabcdef));
        EXPECT_EQ(duplicate->getType(), mixxx::CueType::HotCue);
        EXPECT_EQ(duplicate->getPosition(), mixxx::audio::FramePos(789));
        EXPECT_FALSE(duplicate->getEndPosition().isValid());
    }
}

TEST_P(RekordboxHotLoopParserTest, PlainCueIgnoresEndpointAndUpdatesType) {
    const auto track = Track::newTemporary();
    const auto cue = seed(track);
    parse(track, fixture(1));
    ASSERT_EQ(track->getCuePoints().size(), 1);
    EXPECT_EQ(track->getCuePoints().front(), cue);
    EXPECT_EQ(cue->getType(), mixxx::CueType::HotCue);
    EXPECT_EQ(cue->getHotCue(), 0);
    EXPECT_EQ(cue->getPosition(), mixxx::audio::FramePos(48000));
    EXPECT_FALSE(cue->getEndPosition().isValid());
    EXPECT_EQ(cue->getColor(), mixxx::RgbColor(GetParam() ? 0x123456 : 0xabcdef));
}

TEST_P(RekordboxHotLoopParserTest, AppliesOffsetAndOneMillisecondClampToBothBounds) {
    struct Expected {
        int offset;
        double start;
        double end;
    };
    for (const auto& expected : {Expected{250, 36000, 84000},
                 Expected{-250, 60000, 108000},
                 Expected{1500, 48, 24000}}) {
        SCOPED_TRACE(expected.offset);
        const auto track = Track::newTemporary();
        parse(track, fixture(), expected.offset);
        ASSERT_EQ(track->getCuePoints().size(), 1);
        const auto cue = track->getCuePoints().front();
        EXPECT_EQ(cue->getType(), mixxx::CueType::Loop);
        EXPECT_EQ(cue->getPosition(), mixxx::audio::FramePos(expected.start));
        EXPECT_EQ(cue->getEndPosition(), mixxx::audio::FramePos(expected.end));
    }
}

TEST_P(RekordboxHotLoopParserTest, InvalidEndpointsPreserveExistingState) {
    for (const quint32 end : {0u, 999u, 1000u, std::numeric_limits<quint32>::max()}) {
        SCOPED_TRACE(end);
        const auto track = Track::newTemporary();
        const auto cue = seed(track);
        parse(track, fixture(2, end));
        expectUnchanged(track, cue);
        const auto fresh = Track::newTemporary();
        parse(fresh, fixture(2, end));
        EXPECT_TRUE(fresh->getCuePoints().isEmpty());
    }
}

TEST_P(RekordboxHotLoopParserTest, OffsetCollapsedLoopPreservesExistingState) {
    const auto track = Track::newTemporary();
    const auto cue = seed(track);
    parse(track, fixture(), 2000);
    expectUnchanged(track, cue);
}

TEST_P(RekordboxHotLoopParserTest, TruncatedEndpointPreservesExistingState) {
    auto bytes = fixture();
    bytes.resize(GetParam() ? 59 : 75);
    QByteArray length;
    mixxx::rekordbox::test::hotloop::appendU32Be(&length, bytes.size());
    bytes.replace(8, 4, length);
    length.clear();
    mixxx::rekordbox::test::hotloop::appendU32Be(&length, bytes.size() - 12);
    bytes.replace(20, 4, length);
    const auto track = Track::newTemporary();
    const auto cue = seed(track);
    parse(track, bytes);
    expectUnchanged(track, cue);
    const auto fresh = Track::newTemporary();
    parse(fresh, bytes);
    EXPECT_TRUE(fresh->getCuePoints().isEmpty());
}

TEST_P(RekordboxHotLoopParserTest, UnsignedEndpointDoesNotWrapToNegativeTime) {
    const auto track = Track::newTemporary();
    parse(track, fixture(2, 0x80000000u));
    ASSERT_EQ(track->getCuePoints().size(), 1);
    const auto cue = track->getCuePoints().front();
    EXPECT_EQ(cue->getType(), mixxx::CueType::Loop);
    EXPECT_EQ(cue->getEndPosition(), mixxx::audio::FramePos(48.0 * 0x80000000u));
}

TEST_P(RekordboxHotLoopParserTest, InvalidNumbersAreSkippedAndNinthSlotIsRetained) {
    for (const quint32 number : {0u, 0x80000001u}) {
        const auto track = Track::newTemporary();
        parse(track, fixture(2, 2000, number));
        EXPECT_TRUE(track->getCuePoints().isEmpty());
    }
    const auto track = Track::newTemporary();
    parse(track, fixture(2, 2000, 9));
    ASSERT_EQ(track->getCuePoints().size(), 1);
    EXPECT_EQ(track->getCuePoints().front()->getHotCue(), 8);
    EXPECT_EQ(track->getCuePoints().front()->getType(), mixxx::CueType::Loop);
}

TEST_P(RekordboxHotLoopParserTest, MemoryLoopZeroNumberKeepsItsExistingBehavior) {
    const auto track = Track::newTemporary();
    parse(track, fixture(2, 2000, 0, 0));
    ASSERT_EQ(track->getCuePoints().size(), 1);
    const auto cue = track->getCuePoints().front();
    EXPECT_EQ(cue->getType(), mixxx::CueType::Loop);
    EXPECT_EQ(cue->getHotCue(), Cue::kNoHotCue);
    EXPECT_EQ(cue->getPosition(), mixxx::audio::FramePos(48000));
    EXPECT_EQ(cue->getEndPosition(), mixxx::audio::FramePos(96000));
}

TEST_P(RekordboxHotLoopParserTest, IgnoreAndInvalidRatePreserveExistingState) {
    const auto track = Track::newTemporary();
    const auto cue = seed(track);
    parse(track, fixture(), 0, true);
    expectUnchanged(track, cue);
    parse(track, fixture(), 0, false, mixxx::audio::SampleRate());
    expectUnchanged(track, cue);
    parse(TrackPointer(), fixture());
}

INSTANTIATE_TEST_SUITE_P(AnlzFormats,
        RekordboxHotLoopParserTest,
        ::testing::Values(false, true),
        [](const auto& info) { return info.param ? "Extended" : "Legacy"; });

} // namespace
