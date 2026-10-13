#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>
#include <array>

#include "library/rekordbox/rekordboxparser_test.h"
#include "track/cue.h"
#include "track/track.h"
#include "util/color/predefinedcolorpalettes.h"

namespace {

// Independent expected values: ID 2 restores the original QColor(248, 0, 0).
// The other seven indexed colors retain their b919 values.
constexpr std::array<mixxx::RgbColor::code_t, 8> kExpectedIndexedColors{
        0xF870F8, 0xF80000, 0xF8A030, 0xF8E331, 0x1EE000, 0x16C0F8, 0x0150F8, 0x9808F8};

void appendU32Be(QByteArray* bytes, quint32 value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        bytes->append(static_cast<char>((value >> shift) & 0xff));
    }
}

// Synthetic PCO2 layout adapted from issue89's fixture. Indexed memory colors
// and custom hot-cue RGB occupy separate fields, intentionally set differently.
QByteArray makeExtendedCue(quint8 colorID, bool loop, bool hotCue = false) {
    QByteArray entry("PCP2");
    appendU32Be(&entry, 0x20);
    appendU32Be(&entry, 48);
    appendU32Be(&entry, hotCue ? 1 : 0);
    entry.append(loop ? '\2' : '\1');
    entry.append(QByteArray(3, '\0'));
    appendU32Be(&entry, 1000);
    appendU32Be(&entry, 2000);
    entry.append(static_cast<char>(colorID));
    entry.append(QByteArray(7, '\0'));
    entry.append(QByteArray(4, '\0'));             // loop numerator/denominator
    appendU32Be(&entry, 0);                        // comment length
    entry.append(QByteArray::fromHex("00123456")); // color code and custom RGB

    QByteArray section("PCO2");
    appendU32Be(&section, 12);
    appendU32Be(&section, 20 + entry.size());
    appendU32Be(&section, hotCue ? 1 : 0);
    section.append(QByteArray::fromHex("00010000")); // one entry
    section.append(entry);

    QByteArray file("PMAI");
    appendU32Be(&file, 12);
    appendU32Be(&file, 12 + section.size());
    file.append(section);
    return file;
}

class RekordboxIndexedColorTest : public testing::TestWithParam<int> {
  protected:
    void parse(const TrackPointer& track, quint8 id, bool loop, bool hotCue = false) {
        QTemporaryDir directory;
        ASSERT_TRUE(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("synthetic.anlz"));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = makeExtendedCue(id, loop, hotCue);
        ASSERT_EQ(file.write(bytes), bytes.size());
        file.close();
        mixxx::rekordbox::test::readAnalyzeForTest(
                track, mixxx::audio::SampleRate(48000), 0, false, path);
    }

    void expectMemoryColor(int id, bool loop, mixxx::RgbColor expected) {
        const auto track = Track::newTemporary();
        parse(track, static_cast<quint8>(id), loop);
        ASSERT_EQ(track->getCuePoints().size(), 1);
        const auto cue = track->getCuePoints().front();
        EXPECT_EQ(cue->getType(), loop ? mixxx::CueType::Loop : mixxx::CueType::MainCue);
        EXPECT_EQ(cue->getColor(), expected);
    }
};

TEST_P(RekordboxIndexedColorTest, ProductionMappingMatchesSelectablePalette) {
    const int id = GetParam();
    const auto expected = mixxx::RgbColor(kExpectedIndexedColors.at(id - 1));
    const auto imported = mixxx::rekordbox::test::colorFromIDForTest(id);
    const auto& palette = mixxx::PredefinedColorPalettes::kRekordboxTrackColorPalette;
    ASSERT_EQ(palette.size(), 8);
    EXPECT_EQ(imported, expected);
    EXPECT_EQ(palette.at(id - 1), expected);
    EXPECT_EQ(palette.at(id - 1), imported);
}

TEST_P(RekordboxIndexedColorTest, MemoryCueUsesIndexedColor) {
    expectMemoryColor(GetParam(),
            false,
            mixxx::RgbColor(kExpectedIndexedColors.at(GetParam() - 1)));
}

TEST_P(RekordboxIndexedColorTest, MemoryLoopUsesIndexedColor) {
    expectMemoryColor(GetParam(), true, mixxx::RgbColor(kExpectedIndexedColors.at(GetParam() - 1)));
}

INSTANTIATE_TEST_SUITE_P(IDs1Through8, RekordboxIndexedColorTest, testing::Range(1, 9));

TEST(RekordboxColorTest, ZeroAndUnknownWireIDsMapToBlack) {
    EXPECT_EQ(mixxx::rekordbox::test::colorFromIDForTest(0), mixxx::RgbColor(0));
    for (int id = 9; id <= 255; ++id) {
        SCOPED_TRACE(id);
        EXPECT_EQ(mixxx::rekordbox::test::colorFromIDForTest(id), mixxx::RgbColor(0));
    }
}

TEST(RekordboxColorTest, IndexedPaletteIsSelectable) {
    const auto& palette = mixxx::PredefinedColorPalettes::kRekordboxTrackColorPalette;
    EXPECT_EQ(palette.size(), 8);
    EXPECT_TRUE(mixxx::PredefinedColorPalettes::kPalettes.contains(palette));
}

class RekordboxUnknownColorTest : public RekordboxIndexedColorTest {};

TEST_P(RekordboxUnknownColorTest, MemoryCueRemainsBlack) {
    expectMemoryColor(GetParam(), false, mixxx::RgbColor(0));
}

TEST_P(RekordboxUnknownColorTest, MemoryLoopRemainsBlack) {
    expectMemoryColor(GetParam(), true, mixxx::RgbColor(0));
}

INSTANTIATE_TEST_SUITE_P(ZeroAndUnknown,
        RekordboxUnknownColorTest,
        testing::Values(0, 9, 255));

class RekordboxCustomColorTest : public RekordboxIndexedColorTest {};

TEST_P(RekordboxCustomColorTest, HotCueUsesCustomRGB) {
    const auto track = Track::newTemporary();
    parse(track, static_cast<quint8>(GetParam()), false, true);
    const auto cue = track->findHotcueByIndex(0);
    ASSERT_TRUE(cue);
    EXPECT_EQ(cue->getType(), mixxx::CueType::HotCue);
    EXPECT_EQ(cue->getColor(), mixxx::RgbColor(0x123456));
}

INSTANTIATE_TEST_SUITE_P(ZeroKnownAndUnknown,
        RekordboxCustomColorTest,
        testing::Values(0, 2, 255));

} // namespace
