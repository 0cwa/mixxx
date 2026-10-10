#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <string>

#include "library/rekordbox/rekordboxparser_test.h"
#include "rekordbox_test_fixtures.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track.h"

namespace {

// AI-generated public Kaitai/parser guard regression tests begin.
namespace parser = mixxx::rekordbox::test;

TEST(RekordboxParserGuardTest, NullPdbStringReturnsEmptyText) {
    EXPECT_TRUE(parser::nullPdbStringForTest().isEmpty());
}

TEST(RekordboxParserGuardTest, ReadsActualKaitaiShortAndLongAsciiStrings) {
    EXPECT_EQ(QStringLiteral("C"), parser::textFromPdbStringForTest(std::string("\x05"
                                                                                "C",
                                           2)));
    EXPECT_EQ(QStringLiteral("long"),
            parser::textFromPdbStringForTest(std::string("\x40\x08\x00\x00long", 8)));
    EXPECT_TRUE(parser::textFromPdbStringForTest(std::string("\x03", 1)).isEmpty());
}

TEST(RekordboxParserGuardTest, BigEndianTextHandlesShortAndTerminatedInputs) {
    EXPECT_TRUE(parser::utf16BeTextForTest({}).isEmpty());
    EXPECT_TRUE(parser::utf16BeTextForTest(std::string("\0", 1)).isEmpty());
    EXPECT_TRUE(parser::utf16BeTextForTest(std::string("\0\0", 2)).isEmpty());
    EXPECT_EQ(QStringLiteral("A"), parser::utf16BeTextForTest(std::string("\0A", 2)));
    EXPECT_EQ(QStringLiteral("A"), parser::utf16BeTextForTest(std::string("\0A\0\0", 4)));
}

class RekordboxAnalyzeGuardTest : public testing::Test {
  protected:
    QString writeFile(const QByteArray& bytes) {
        EXPECT_TRUE(directory.isValid());
        const QString path = directory.filePath("public.dat");
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(bytes.size(), file.write(bytes));
        file.close();
        return path;
    }

    QTemporaryDir directory;
    const mixxx::audio::SampleRate sampleRate{48000};
};

TEST_F(RekordboxAnalyzeGuardTest, ValidBeatGridImportsActualBeatPositions) {
    const auto path = writeFile(parser::makeAnlzBeatGridFixture());
    const auto track = Track::newTemporary();
    ASSERT_NO_THROW(parser::readAnalyzeForTest(track, sampleRate, 0, true, path));
    const auto beats = track->getBeats();
    ASSERT_TRUE(beats);
    EXPECT_EQ(sampleRate, beats->getSampleRate());
    EXPECT_EQ(mixxx::audio::FramePos(48000), beats->findNextBeat(mixxx::audio::FramePos(48000)));
    EXPECT_EQ(mixxx::audio::FramePos(72000), beats->findNextBeat(mixxx::audio::FramePos(48001)));
    EXPECT_DOUBLE_EQ(120.0,
            beats->getBpmInRange(mixxx::audio::FramePos(48000),
                         mixxx::audio::FramePos(72000))
                    .value());
}

TEST_F(RekordboxAnalyzeGuardTest, CuePassDoesNotImportBeatGrid) {
    const auto path = writeFile(parser::makeAnlzBeatGridFixture());
    const auto track = Track::newTemporary();
    ASSERT_NO_THROW(parser::readAnalyzeForTest(track, sampleRate, 0, false, path));
    EXPECT_FALSE(track->getBeats());
    EXPECT_TRUE(track->getCuePoints().isEmpty());
}

TEST_F(RekordboxAnalyzeGuardTest, InvalidTrackAndSampleRateReturnBeforeParsing) {
    const auto path = writeFile(parser::makeAnlzBeatGridFixture());
    ASSERT_NO_THROW(parser::readAnalyzeForTest({}, sampleRate, 0, true, path));
    const auto track = Track::newTemporary();
    ASSERT_NO_THROW(parser::readAnalyzeForTest(track, mixxx::audio::SampleRate{}, 0, true, path));
    EXPECT_FALSE(track->getBeats());
    EXPECT_TRUE(track->getCuePoints().isEmpty());
}

TEST_F(RekordboxAnalyzeGuardTest, MissingFilePreservesState) {
    const auto track = Track::newTemporary();
    const auto beats = mixxx::Beats::fromConstTempo(
            sampleRate, mixxx::audio::FramePos(0), mixxx::Bpm(90));
    ASSERT_TRUE(track->trySetBeats(beats));
    ASSERT_NO_THROW(parser::readAnalyzeForTest(
            track, sampleRate, 0, true, directory.filePath("missing.dat")));
    EXPECT_EQ(beats, track->getBeats());
}

class RekordboxMalformedAnalyzeTest : public RekordboxAnalyzeGuardTest,
                                      public testing::WithParamInterface<int> {
};

TEST_P(RekordboxMalformedAnalyzeTest, PreservesSeededAndFreshTrackStateInBothPasses) {
    auto bytes = parser::makeAnlzBeatGridFixture();
    switch (GetParam()) {
    case 0:
        bytes.clear();
        break;
    case 1:
        bytes.chop(1);
        break;
    case 2:
        bytes[0] = 'X';
        break;
    case 3:
        bytes.append("PCOB", 4);
        parser::appendU32Be(&bytes, 12);
        parser::appendU32Be(&bytes, 24);
        for (int i = 0; i < 4; ++i) {
            bytes[8 + i] = static_cast<char>((bytes.size() >> (24 - 8 * i)) & 0xff);
        }
        break;
    }
    const auto path = writeFile(bytes);
    for (const bool ignoreCues : {false, true}) {
        SCOPED_TRACE(ignoreCues);
        const auto track = Track::newTemporary();
        const auto beats = mixxx::Beats::fromConstTempo(
                sampleRate, mixxx::audio::FramePos(0), mixxx::Bpm(90));
        ASSERT_TRUE(track->trySetBeats(beats));
        const auto cue = track->createAndAddCue(mixxx::CueType::HotCue,
                1,
                mixxx::audio::FramePos(123),
                mixxx::audio::kInvalidFramePos);
        ASSERT_TRUE(cue);
        ASSERT_NO_THROW(parser::readAnalyzeForTest(track, sampleRate, 0, ignoreCues, path));
        EXPECT_EQ(beats, track->getBeats());
        ASSERT_EQ(1, track->getCuePoints().size());
        EXPECT_EQ(cue, track->getCuePoints().front());
        EXPECT_EQ(mixxx::audio::FramePos(123), cue->getPosition());

        const auto fresh = Track::newTemporary();
        ASSERT_NO_THROW(parser::readAnalyzeForTest(fresh, sampleRate, 0, ignoreCues, path));
        EXPECT_FALSE(fresh->getBeats());
        EXPECT_TRUE(fresh->getCuePoints().isEmpty());
    }
}

INSTANTIATE_TEST_SUITE_P(PublicMalformedFiles,
        RekordboxMalformedAnalyzeTest,
        testing::Values(0, 1, 2, 3));
// End AI-generated public Kaitai/parser guard regression tests.

} // namespace
