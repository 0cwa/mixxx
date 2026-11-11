#include "library/rekordbox/rekordboxanlzimporter.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>
#include <limits>

#include "test/rekordboxanlzfixtures.h"
#include "track/track.h"

namespace mixxx::rekordbox {
namespace {

bool writeSource(const QString& path, const std::string& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes.data(), static_cast<qint64>(bytes.size())) ==
            static_cast<qint64>(bytes.size()) &&
            file.flush();
}

class RekordboxAnlzImporterTest : public testing::Test {
  protected:
    TrackPointer track = Track::newTemporary();
    AnlzImportState state;
    const audio::SampleRate rate{48000};

    void SetUp() override {
        track->setAudioProperties(audio::ChannelCount(2),
                rate,
                audio::Bitrate(),
                Duration::fromSeconds(60));
    }

    AnlzApplyResult apply(const AnlzData& data, bool promote = false, double correction = 0) {
        return applyAnlzTrackAnnotations(track,
                rate,
                correction,
                data,
                library::MainCuePromotionPolicy::fromConfigValue(promote),
                &state);
    }

    AnlzApplyResult importSources(
            const QString& path, bool promote = false, double correction = 0) {
        return importAnlzSources(track,
                rate,
                correction,
                path,
                library::MainCuePromotionPolicy::fromConfigValue(promote),
                &state);
    }

    AnlzCue hot(std::uint32_t slot = 0, bool loop = true) {
        AnlzCue cue;
        cue.role = loop ? AnlzCueRole::HotLoop : AnlzCueRole::HotCue;
        cue.hotSlot = slot;
        cue.startMilliseconds = 1000;
        if (loop) {
            cue.endMilliseconds = 2000;
        }
        cue.labelUtf8 = "Loop";
        cue.colorRgb = 0x123456;
        cue.activeLoop = loop ? std::optional<bool>(true) : std::nullopt;
        return cue;
    }

    AnlzCue memory(std::size_t ordinal = 0, bool loop = false) {
        AnlzCue cue = hot(0, loop);
        cue.role = loop ? AnlzCueRole::MemoryLoop : AnlzCueRole::MemoryCue;
        cue.hotSlot.reset();
        cue.provenance.push_back({"source", AnlzSource::Dat, "PCOB", 0, ordinal, {}, {}});
        return cue;
    }
};

TEST_F(RekordboxAnlzImporterTest, LegacyAndExtendedHotLoopsKeepCompleteInterval) {
    for (bool extended : {false, true}) {
        const auto bytes = test::file({test::cueList(extended,
                1,
                {extended ? test::extendedCue() : test::legacyCue()})});
        const auto decoded = decodeAnlz({AnlzSource::Dat, "fixture", bytes});
        ASSERT_TRUE(decoded.usable());
        ASSERT_TRUE(apply(*decoded.data).complete);
        const auto cue = track->findHotcueByIndex(0);
        ASSERT_TRUE(cue);
        EXPECT_EQ(CueType::Loop, cue->getType());
        EXPECT_DOUBLE_EQ(48000, cue->getPosition().value());
        EXPECT_DOUBLE_EQ(96000, cue->getEndPosition().value());
    }
}

TEST_F(RekordboxAnlzImporterTest, OwnedSlotUpdatesPointToLoopAndBack) {
    AnlzData data;
    data.cues = {hot(9, false)};
    ASSERT_TRUE(apply(data).complete);
    const auto cue = track->findHotcueByIndex(9);
    ASSERT_TRUE(cue);
    EXPECT_EQ(CueType::HotCue, cue->getType());
    EXPECT_FALSE(cue->getEndPosition().isValid());
    data.cues = {hot(9)};
    ASSERT_TRUE(apply(data).complete);
    EXPECT_EQ(cue, track->findHotcueByIndex(9));
    EXPECT_EQ(CueType::Loop, cue->getType());
    EXPECT_EQ(audio::FramePos(96000), cue->getEndPosition());
    EXPECT_EQ(std::optional<bool>(true), cue->getActiveLoopIntent());
    data.cues = {hot(9, false)};
    ASSERT_TRUE(apply(data).complete);
    EXPECT_EQ(CueType::HotCue, cue->getType());
    EXPECT_FALSE(cue->getEndPosition().isValid());
    EXPECT_FALSE(cue->getActiveLoopIntent().has_value());
}

TEST_F(RekordboxAnlzImporterTest, PreservesOccupiedUserSlot) {
    const auto userCue = track->createAndAddCue(
            CueType::HotCue, 0, audio::FramePos(123), audio::kInvalidFramePos);
    userCue->setLabel(QStringLiteral("User cue"));
    AnlzData data;
    data.cues = {hot(0), hot(9)};
    EXPECT_FALSE(apply(data).complete);
    EXPECT_EQ(audio::FramePos(123), userCue->getPosition());
    EXPECT_EQ(QStringLiteral("User cue"), userCue->getLabel());
    ASSERT_TRUE(track->findHotcueByIndex(9));
    EXPECT_EQ(CueType::Loop, track->findHotcueByIndex(9)->getType());
}

TEST_F(RekordboxAnlzImporterTest, ReimportPreservesLocalEditsAndDeletions) {
    AnlzData data;
    data.cues = {hot(), memory()};
    ASSERT_TRUE(apply(data).complete);
    const auto hotCue = track->findHotcueByIndex(0);
    const auto memoryCue = track->findCueByType(CueType::Memory);
    hotCue->setLabel(QStringLiteral("Edited locally"));
    track->removeCue(memoryCue);
    data.cues[0].startMilliseconds = 3000;
    data.cues[0].endMilliseconds = 4000;
    EXPECT_FALSE(apply(data).complete);
    EXPECT_EQ(audio::FramePos(48000), hotCue->getPosition());
    EXPECT_EQ(QStringLiteral("Edited locally"), hotCue->getLabel());
    EXPECT_FALSE(track->findCueByType(CueType::Memory));
}

TEST_F(RekordboxAnlzImporterTest, RestartDoesNotAssumeOwnershipOfOccupiedSlots) {
    AnlzData data;
    data.cues = {hot()};
    ASSERT_TRUE(apply(data).complete);
    state = AnlzImportState();
    ASSERT_TRUE(apply(data).complete);
    data.cues[0].startMilliseconds = 3000;
    data.cues[0].endMilliseconds = 4000;
    EXPECT_FALSE(apply(data).complete);
    EXPECT_EQ(audio::FramePos(48000), track->findHotcueByIndex(0)->getPosition());
}

TEST_F(RekordboxAnlzImporterTest, MemoryLoopHasMemoryRoleAndNoHotSlot) {
    AnlzData data;
    data.cues = {memory(0, true)};
    ASSERT_TRUE(apply(data).complete);
    const auto cue = track->findCueByType(CueType::Memory);
    ASSERT_TRUE(cue);
    EXPECT_EQ(Cue::kNoHotCue, cue->getHotCue());
    EXPECT_EQ(audio::FramePos(96000), cue->getEndPosition());
    EXPECT_EQ(std::optional<bool>(true), cue->getActiveLoopIntent());
}

TEST_F(RekordboxAnlzImporterTest, MainCuePromotionPreservesMemoryIdentity) {
    AnlzData data;
    data.cues = {memory()};
    ASSERT_TRUE(apply(data, true).complete);
    const auto memoryCue = track->findCueByType(CueType::Memory);
    const auto mainCue = track->findCueByType(CueType::MainCue);
    ASSERT_TRUE(memoryCue);
    ASSERT_TRUE(mainCue);
    EXPECT_NE(memoryCue, mainCue);
    EXPECT_EQ(memoryCue->getPosition(), mainCue->getPosition());
    EXPECT_EQ(QStringLiteral("Loop"), memoryCue->getLabel());
    ASSERT_TRUE(apply(data, true).complete);
    EXPECT_EQ(2, track->getCuePoints().size());
}

TEST_F(RekordboxAnlzImporterTest, MainCuePromotionAtZeroKeepsMetadataAndOwnership) {
    AnlzData data;
    data.cues = {memory()};
    data.cues.front().startMilliseconds = 0;
    ASSERT_TRUE(apply(data, true).complete);
    const auto memoryCue = track->findCueByType(CueType::Memory);
    const auto mainCue = track->findCueByType(CueType::MainCue);
    ASSERT_TRUE(memoryCue);
    ASSERT_TRUE(mainCue);
    EXPECT_NE(memoryCue, mainCue);
    EXPECT_EQ(audio::FramePos(0), mainCue->getPosition());
    EXPECT_EQ(audio::FramePos(0), track->getMainCuePosition());
    EXPECT_EQ(QStringLiteral("Loop"), mainCue->getLabel());
    EXPECT_EQ(RgbColor(0x123456), mainCue->getColor());
    EXPECT_TRUE(state.promotedMainCue.has_value());
    ASSERT_TRUE(apply(data, true).complete);
    EXPECT_EQ(2, track->getCuePoints().size());
    track->removeCue(mainCue);
    EXPECT_FALSE(apply(data, true).complete);
    EXPECT_FALSE(track->findCueByType(CueType::MainCue));
    EXPECT_EQ(memoryCue, track->findCueByType(CueType::Memory));
}

TEST_F(RekordboxAnlzImporterTest, MainCuePromotionPreservesExplicitOwnedDeletion) {
    AnlzData data;
    data.cues = {memory()};
    ASSERT_TRUE(apply(data, true).complete);
    const auto mainCue = track->findCueByType(CueType::MainCue);
    ASSERT_TRUE(mainCue);
    track->removeCue(mainCue);
    data.cues.front().startMilliseconds = 2000;
    EXPECT_FALSE(apply(data, true).complete);
    EXPECT_FALSE(track->findCueByType(CueType::MainCue));
    ASSERT_TRUE(track->findCueByType(CueType::Memory));
    EXPECT_EQ(audio::FramePos(96000), track->findCueByType(CueType::Memory)->getPosition());
    EXPECT_FALSE(apply(data, true).complete);
    EXPECT_FALSE(track->findCueByType(CueType::MainCue));
}

TEST_F(RekordboxAnlzImporterTest, SkippedFirstMemoryCueDoesNotRetargetMainCue) {
    AnlzData data;
    data.cues = {memory(0), memory(1)};
    data.cues.back().startMilliseconds = 2000;
    ASSERT_TRUE(apply(data, true).complete);
    const auto mainCue = track->findCueByType(CueType::MainCue);
    const auto firstMemory = track->findCueByType(CueType::Memory);
    ASSERT_TRUE(mainCue);
    ASSERT_TRUE(firstMemory);
    firstMemory->setLabel(QStringLiteral("Local edit"));
    EXPECT_FALSE(apply(data, true).complete);
    EXPECT_EQ(audio::FramePos(48000), mainCue->getPosition());
    EXPECT_EQ(QStringLiteral("Local edit"), firstMemory->getLabel());
}

TEST_F(RekordboxAnlzImporterTest, DistinctSameTimeMemoryAnnotationsRemainDistinct) {
    AnlzData data;
    data.cues = {memory(0), memory(1)};
    ASSERT_TRUE(apply(data).complete);
    EXPECT_EQ(2, track->getCuePoints().size());
    ASSERT_TRUE(apply(data).complete);
    EXPECT_EQ(2, track->getCuePoints().size());
}

TEST_F(RekordboxAnlzImporterTest, TimingRetainsZeroAndNegativePreroll) {
    AnlzData data;
    data.cues = {hot(0, false)};
    data.cues[0].startMilliseconds = 0;
    ASSERT_TRUE(apply(data).complete);
    EXPECT_EQ(audio::FramePos(0), track->findHotcueByIndex(0)->getPosition());
    ASSERT_TRUE(apply(data, false, 26).complete);
    EXPECT_EQ(audio::FramePos(-1248), track->findHotcueByIndex(0)->getPosition());
}

TEST_F(RekordboxAnlzImporterTest, VariableGridRetainsBeatInBarAndRespectsBpmLock) {
    AnlzData data;
    data.beats = {{0, 12000, 3}, {500, 12000, 4}, {1100, 10000, 1}};
    ASSERT_TRUE(apply(data).complete);
    ASSERT_TRUE(track->getBeats());
    EXPECT_EQ(2, track->getDownbeatOffset());
    const auto beats = track->getBeats();
    track->setBpmLocked(true);
    data.beats[2].timeMilliseconds = 1200;
    EXPECT_FALSE(apply(data).complete);
    EXPECT_EQ(beats, track->getBeats());
    EXPECT_EQ(2, track->getDownbeatOffset());
}

TEST_F(RekordboxAnlzImporterTest, ReimportPreservesLocallyClearedGrid) {
    AnlzData data;
    data.beats = {{0, 12000, 1}, {500, 12000, 2}};
    ASSERT_TRUE(apply(data).complete);
    ASSERT_TRUE(track->trySetBeats(nullptr));
    EXPECT_FALSE(apply(data).complete);
    EXPECT_FALSE(track->getBeats());
}

TEST_F(RekordboxAnlzImporterTest, SingleBeatQuantizesOnlyDestinationFramePosition) {
    const audio::SampleRate fractionalRate(44100);
    track->setAudioProperties(audio::ChannelCount(2),
            fractionalRate,
            audio::Bitrate(),
            Duration::fromSeconds(60));
    AnlzData data;
    data.beats = {{1001, 12000, 3}};
    ASSERT_TRUE(applyAnlzTrackAnnotations(track,
            fractionalRate,
            0,
            data,
            library::MainCuePromotionPolicy::fromConfigValue(false),
            &state)
                    .complete);
    ASSERT_TRUE(track->getBeats());
    EXPECT_EQ(audio::FramePos(44144), *track->getBeats()->cfirstmarker());
    EXPECT_EQ(2, track->getDownbeatOffset());
    EXPECT_EQ(1001u, data.beats.front().timeMilliseconds);

    data.beats.front().timeMilliseconds = 0;
    ASSERT_TRUE(applyAnlzTrackAnnotations(track,
            fractionalRate,
            26,
            data,
            library::MainCuePromotionPolicy::fromConfigValue(false),
            &state)
                    .complete);
    EXPECT_EQ(audio::FramePos(-1147), *track->getBeats()->cfirstmarker());
    EXPECT_EQ(0u, data.beats.front().timeMilliseconds);
}

TEST_F(RekordboxAnlzImporterTest, InvalidSingleBeatTempoPreservesGridAndAllowsRetry) {
    AnlzData data;
    data.beats = {{1000, 0, 1}};
    data.cues = {hot()};
    const auto invalid = apply(data);
    EXPECT_FALSE(invalid.complete);
    EXPECT_FALSE(invalid.diagnostics.isEmpty());
    EXPECT_FALSE(track->getBeats());
    EXPECT_TRUE(track->findHotcueByIndex(0));

    data.beats.front().tempoHundredths = 12000;
    ASSERT_TRUE(apply(data).complete);
    const auto valid = track->getBeats();
    ASSERT_TRUE(valid);
    data.beats.front().tempoHundredths = 0;
    EXPECT_FALSE(apply(data).complete);
    EXPECT_EQ(valid, track->getBeats());
}

TEST_F(RekordboxAnlzImporterTest, SingleBeatRejectsNonfiniteConvertedTiming) {
    AnlzData data;
    data.beats = {{1000, 12000, 1}};
    const auto invalid = apply(data, false, std::numeric_limits<double>::max());
    EXPECT_FALSE(invalid.complete);
    EXPECT_FALSE(invalid.diagnostics.isEmpty());
    EXPECT_FALSE(track->getBeats());
}

TEST_F(RekordboxAnlzImporterTest, ReimportConvertsUneditedCuesAtNewSampleRate) {
    AnlzData data;
    data.cues = {hot(), memory()};
    ASSERT_TRUE(apply(data, true).complete);
    const audio::SampleRate newRate(96000);
    track->setAudioProperties(audio::ChannelCount(2),
            newRate,
            audio::Bitrate(),
            Duration::fromSeconds(60));
    ASSERT_TRUE(applyAnlzTrackAnnotations(track,
            newRate,
            0,
            data,
            library::MainCuePromotionPolicy::fromConfigValue(true),
            &state)
                    .complete);
    const auto cue = track->findHotcueByIndex(0);
    EXPECT_EQ(audio::FramePos(96000), cue->getPosition());
    EXPECT_EQ(audio::FramePos(192000), cue->getEndPosition());
    EXPECT_EQ(audio::FramePos(96000), track->findCueByType(CueType::MainCue)->getPosition());
}

TEST_F(RekordboxAnlzImporterTest, RepairedCompanionUpdatesMemoryCueWithoutDuplicatingIt) {
    const auto dat = test::file({test::cueList(false, 0, {test::legacyCue(0, 1, 1000, 0, 0)})});
    const auto ext = test::file({test::cueList(true,
            0,
            {test::extendedCue(0,
                    1,
                    1000,
                    0,
                    std::u16string(u"Memory"),
                    std::nullopt,
                    3)})});
    const auto fallback = decodeAnlz({AnlzSource::Dat, "DAT", dat});
    ASSERT_TRUE(fallback.usable());
    ASSERT_TRUE(apply(*fallback.data).complete);
    const auto cue = track->findCueByType(CueType::Memory);
    ASSERT_TRUE(cue);
    EXPECT_TRUE(cue->getLabel().isEmpty());
    const auto repaired = decodeAnlzCompanions(
            {{AnlzSource::Dat, "DAT", dat}, {AnlzSource::Ext, "EXT", ext}});
    ASSERT_TRUE(repaired.usable());
    ASSERT_EQ(1, repaired.data->cues.size());
    ASSERT_TRUE(apply(*repaired.data).complete);
    EXPECT_EQ(1, track->getCuePoints().size());
    EXPECT_EQ(cue, track->findCueByType(CueType::Memory));
    EXPECT_EQ(QStringLiteral("Memory"), cue->getLabel());
    EXPECT_EQ(RgbColor(0xF8A030), cue->getColor());
    ASSERT_TRUE(apply(*fallback.data).complete);
    EXPECT_EQ(1, track->getCuePoints().size());
    EXPECT_EQ(QStringLiteral("Memory"), cue->getLabel());
    ASSERT_TRUE(apply(*repaired.data).complete);
    EXPECT_EQ(1, track->getCuePoints().size());
}

TEST_F(RekordboxAnlzImporterTest, DivergingCompanionsSplitOwnedMemoryAliasesOnce) {
    const auto dat = test::file({test::cueList(false, 0, {test::legacyCue(0, 1, 1000, 0, 0)})});
    const auto originalExt = test::file({test::cueList(true,
            0,
            {test::extendedCue(0,
                    1,
                    1000,
                    0,
                    std::u16string(u"Memory"),
                    std::nullopt,
                    3)})});
    const auto original = decodeAnlzCompanions({{AnlzSource::Dat, "DAT", dat},
            {AnlzSource::Ext, "EXT", originalExt}});
    ASSERT_TRUE(original.usable());
    ASSERT_EQ(1u, original.data->cues.size());
    ASSERT_TRUE(apply(*original.data).complete);
    const auto first = track->findCueByType(CueType::Memory);
    ASSERT_TRUE(first);

    const auto changedExt = test::file({test::cueList(true,
            0,
            {test::extendedCue(0,
                    1,
                    2000,
                    0,
                    std::u16string(u"Memory"),
                    std::nullopt,
                    3)})});
    const auto divergent = decodeAnlzCompanions({{AnlzSource::Dat, "DAT", dat},
            {AnlzSource::Ext, "EXT", changedExt}});
    ASSERT_TRUE(divergent.usable());
    ASSERT_EQ(2u, divergent.data->cues.size());
    ASSERT_TRUE(apply(*divergent.data).complete);
    ASSERT_EQ(2, track->getCuePoints().size());
    EXPECT_EQ(audio::FramePos(48000), first->getPosition());
    CuePointer second;
    for (const auto& cue : track->getCuePoints()) {
        if (cue != first) {
            second = cue;
        }
    }
    ASSERT_TRUE(second);
    EXPECT_EQ(CueType::Memory, second->getType());
    EXPECT_EQ(audio::FramePos(96000), second->getPosition());
    ASSERT_TRUE(apply(*divergent.data).complete);
    EXPECT_EQ(2, track->getCuePoints().size());
    EXPECT_EQ(audio::FramePos(48000), first->getPosition());
    EXPECT_EQ(audio::FramePos(96000), second->getPosition());

    first->setLabel(QStringLiteral("Local edit"));
    EXPECT_FALSE(apply(*divergent.data).complete);
    EXPECT_EQ(2, track->getCuePoints().size());
    EXPECT_EQ(QStringLiteral("Local edit"), first->getLabel());
    EXPECT_EQ(audio::FramePos(96000), second->getPosition());
}

TEST_F(RekordboxAnlzImporterTest, DivergingCompanionsPreserveEditedMergedMemoryCue) {
    const auto dat = test::file({test::cueList(false, 0, {test::legacyCue(0, 1, 1000, 0, 0)})});
    const auto originalExt = test::file({test::cueList(true,
            0,
            {test::extendedCue(0,
                    1,
                    1000,
                    0,
                    std::u16string(u"Memory"),
                    std::nullopt,
                    3)})});
    const auto original = decodeAnlzCompanions({{AnlzSource::Dat, "DAT", dat},
            {AnlzSource::Ext, "EXT", originalExt}});
    ASSERT_TRUE(original.usable());
    ASSERT_TRUE(apply(*original.data).complete);
    const auto cue = track->findCueByType(CueType::Memory);
    ASSERT_TRUE(cue);
    cue->setLabel(QStringLiteral("Local edit"));

    const auto changedExt = test::file({test::cueList(true,
            0,
            {test::extendedCue(0,
                    1,
                    2000,
                    0,
                    std::u16string(u"Memory"),
                    std::nullopt,
                    3)})});
    const auto divergent = decodeAnlzCompanions({{AnlzSource::Dat, "DAT", dat},
            {AnlzSource::Ext, "EXT", changedExt}});
    ASSERT_TRUE(divergent.usable());
    ASSERT_EQ(2u, divergent.data->cues.size());
    for (int attempt = 0; attempt < 2; ++attempt) {
        EXPECT_FALSE(apply(*divergent.data).complete);
        EXPECT_EQ(1, track->getCuePoints().size());
        EXPECT_EQ(QStringLiteral("Local edit"), cue->getLabel());
        EXPECT_EQ(audio::FramePos(48000), cue->getPosition());
    }
}

TEST_F(RekordboxAnlzImporterTest, ReconvergingCompanionsPreserveConflictingOwnedMemoryCues) {
    const auto dat = test::file({test::cueList(false, 0, {test::legacyCue(0, 1, 1000, 0, 0)})});
    const auto splitExt = test::file({test::cueList(true,
            0,
            {test::extendedCue(0,
                    1,
                    2000,
                    0,
                    std::u16string(u"Memory"),
                    std::nullopt,
                    3)})});
    const auto split = decodeAnlzCompanions({{AnlzSource::Dat, "DAT", dat},
            {AnlzSource::Ext, "EXT", splitExt}});
    ASSERT_TRUE(split.usable());
    ASSERT_EQ(2u, split.data->cues.size());
    ASSERT_TRUE(apply(*split.data).complete);
    ASSERT_EQ(2, track->getCuePoints().size());
    const auto cues = track->getCuePoints();
    const auto firstPosition = cues.front()->getPosition();
    const auto secondPosition = cues.back()->getPosition();
    EXPECT_NE(firstPosition, secondPosition);

    const auto mergedExt = test::file({test::cueList(true,
            0,
            {test::extendedCue(0,
                    1,
                    1000,
                    0,
                    std::u16string(u"Memory"),
                    std::nullopt,
                    3)})});
    const auto merged = decodeAnlzCompanions({{AnlzSource::Dat, "DAT", dat},
            {AnlzSource::Ext, "EXT", mergedExt}});
    ASSERT_TRUE(merged.usable());
    ASSERT_EQ(1u, merged.data->cues.size());
    const auto conflict = apply(*merged.data);
    EXPECT_FALSE(conflict.complete);
    EXPECT_FALSE(conflict.diagnostics.isEmpty());
    EXPECT_EQ(cues, track->getCuePoints());
    EXPECT_EQ(firstPosition, cues.front()->getPosition());
    EXPECT_EQ(secondPosition, cues.back()->getPosition());

    cues.back()->setLabel(QStringLiteral("Local edit"));
    EXPECT_FALSE(apply(*merged.data).complete);
    EXPECT_EQ(cues, track->getCuePoints());
    EXPECT_EQ(firstPosition, cues.front()->getPosition());
    EXPECT_EQ(secondPosition, cues.back()->getPosition());
    EXPECT_EQ(QStringLiteral("Local edit"), cues.back()->getLabel());
}

TEST_F(RekordboxAnlzImporterTest, RejectsOutOfRangeSlotWithoutReclassification) {
    AnlzData data;
    data.cues = {hot(std::numeric_limits<std::uint32_t>::max())};
    EXPECT_FALSE(apply(data).complete);
    EXPECT_TRUE(track->getCuePoints().isEmpty());
}

TEST_F(RekordboxAnlzImporterTest, PaletteRejectsOversizedLiteralBeforeMasking) {
    for (std::uint8_t id = 0; id <= 8; ++id) {
        const auto color = anlzPaletteColor(id);
        if (id == 0 || id == 2) {
            EXPECT_FALSE(color);
        } else {
            ASSERT_TRUE(color);
            EXPECT_TRUE(RgbColor::isValidCode(*color));
        }
    }
    EXPECT_FALSE(anlzPaletteColor(255));
}

TEST_F(RekordboxAnlzImporterTest, ExplicitUnassignedColorClearsWhileUnknownPreserves) {
    AnlzData data;
    data.cues = {memory()};
    data.cues[0].colorRgb.reset();
    data.cues[0].paletteId = 1;
    ASSERT_TRUE(apply(data).complete);
    const auto cue = track->findCueByType(CueType::Memory);
    EXPECT_EQ(RgbColor(0xF870F8), cue->getColor());
    data.cues[0].paletteId.reset();
    ASSERT_TRUE(apply(data).complete);
    EXPECT_EQ(RgbColor(0xF870F8), cue->getColor());
    data.cues[0].paletteId = 0;
    ASSERT_TRUE(apply(data).complete);
    EXPECT_EQ(PredefinedColorPalettes::kDefaultCueColor, cue->getColor());
}

TEST_F(RekordboxAnlzImporterTest, FingerprintsDetectSamePathRepairAndCompanionRemoval) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("analysis.DAT"));
    QFile dat(path);
    ASSERT_TRUE(dat.open(QIODevice::WriteOnly));
    ASSERT_EQ(6, dat.write("broken"));
    dat.close();
    const auto corrupt = readAnlzSources(path);
    EXPECT_FALSE(corrupt.decoded.usable());
    const auto bytes = test::file({test::cueList(false, 1, {test::legacyCue()})});
    ASSERT_TRUE(dat.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(static_cast<qint64>(bytes.size()), dat.write(bytes.data(), bytes.size()));
    dat.close();
    const auto repaired = readAnlzSources(path);
    ASSERT_TRUE(repaired.decoded.usable());
    EXPECT_NE(corrupt.fingerprint, repaired.fingerprint);
    EXPECT_EQ(repaired.fingerprint, readAnlzSources(path).fingerprint);
    EXPECT_EQ(repaired.decoded.data, readAnlzSources(path, &repaired).decoded.data);
    QFile ext(directory.filePath(QStringLiteral("analysis.EXT")));
    ASSERT_TRUE(ext.open(QIODevice::WriteOnly));
    ASSERT_EQ(6, ext.write("broken"));
    ext.close();
    const auto fallback = readAnlzSources(path);
    EXPECT_TRUE(fallback.decoded.usable());
    EXPECT_EQ(AnlzStatus::Warning, fallback.decoded.status);
    EXPECT_NE(repaired.fingerprint, fallback.fingerprint);
    ASSERT_TRUE(ext.remove());
    EXPECT_EQ(repaired.fingerprint, readAnlzSources(path).fingerprint);
    ASSERT_TRUE(dat.remove());
    EXPECT_FALSE(readAnlzSources(path).decoded.usable());
}

TEST_F(RekordboxAnlzImporterTest, SourceImportPreservesDeletionAcrossTemporaryAbsence) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("analysis.DAT"));
    const auto bytes = test::file({test::cueList(false, 0, {test::legacyCue(0, 1, 1000, 0, 0)})});
    ASSERT_TRUE(writeSource(path, bytes));
    ASSERT_TRUE(importSources(path).complete);
    const auto cue = track->findCueByType(CueType::Memory);
    ASSERT_TRUE(cue);
    track->removeCue(cue);
    ASSERT_TRUE(QFile::remove(path));
    const auto unavailable = importSources(path);
    EXPECT_FALSE(unavailable.complete);
    EXPECT_FALSE(unavailable.diagnostics.isEmpty());
    EXPECT_EQ(track, state.track.lock());
    EXPECT_FALSE(state.cues.isEmpty());
    ASSERT_TRUE(writeSource(path, bytes));
    EXPECT_TRUE(importSources(path).complete);
    EXPECT_FALSE(track->findCueByType(CueType::Memory));

    ASSERT_TRUE(writeSource(path,
            test::file({test::cueList(false, 0, {test::legacyCue(0, 1, 2000, 0, 0)})})));
    EXPECT_FALSE(importSources(path).complete);
    EXPECT_FALSE(track->findCueByType(CueType::Memory));
    EXPECT_TRUE(state.fingerprint.isEmpty());
}

TEST_F(RekordboxAnlzImporterTest, SourceImportRetriesCorruptInputAndCachesOnlySuccess) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("analysis.DAT"));
    ASSERT_TRUE(writeSource(path, "invalid analysis"));
    const auto corrupt = importSources(path);
    EXPECT_FALSE(corrupt.complete);
    EXPECT_FALSE(corrupt.diagnostics.isEmpty());
    EXPECT_TRUE(state.fingerprint.isEmpty());
    EXPECT_TRUE(track->getCuePoints().isEmpty());
    ASSERT_TRUE(writeSource(path, test::file({test::cueList(false, 1, {test::legacyCue()})})));
    ASSERT_TRUE(importSources(path).complete);
    ASSERT_TRUE(track->findHotcueByIndex(0));
    EXPECT_EQ(state.decodedSource.fingerprint, state.fingerprint);
    EXPECT_FALSE(state.fingerprint.isEmpty());
    const auto decoded = state.decodedSource.decoded.data;
    ASSERT_TRUE(importSources(path).complete);
    EXPECT_EQ(decoded, state.decodedSource.decoded.data);
    EXPECT_EQ(1, track->getCuePoints().size());
}

TEST_F(RekordboxAnlzImporterTest, SourceImportTracksTimingRateAndPromotionParameters) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("analysis.DAT"));
    ASSERT_TRUE(writeSource(path,
            test::file({test::cueList(
                                false, 0, {test::legacyCue(0, 1, 1000, 0, 0)}),
                    test::cueList(false, 1, {test::legacyCue()})})));
    ASSERT_TRUE(importSources(path).complete);
    const auto hotCue = track->findHotcueByIndex(0);
    ASSERT_TRUE(hotCue);
    EXPECT_FALSE(track->findCueByType(CueType::MainCue));
    ASSERT_TRUE(importSources(path, true, 26).complete);
    const auto mainCue = track->findCueByType(CueType::MainCue);
    ASSERT_TRUE(mainCue);
    EXPECT_EQ(audio::FramePos(46752), hotCue->getPosition());
    EXPECT_EQ(audio::FramePos(94752), hotCue->getEndPosition());
    EXPECT_EQ(audio::FramePos(46752), mainCue->getPosition());

    const audio::SampleRate newRate(96000);
    track->setAudioProperties(audio::ChannelCount(2),
            newRate,
            audio::Bitrate(),
            Duration::fromSeconds(60));
    ASSERT_TRUE(importAnlzSources(track,
            newRate,
            26,
            path,
            library::MainCuePromotionPolicy::fromConfigValue(true),
            &state)
                    .complete);
    EXPECT_EQ(audio::FramePos(93504), hotCue->getPosition());
    EXPECT_EQ(audio::FramePos(189504), hotCue->getEndPosition());
    EXPECT_EQ(audio::FramePos(93504), mainCue->getPosition());
    EXPECT_EQ(state.decodedSource.fingerprint, state.fingerprint);
}

TEST_F(RekordboxAnlzImporterTest, SourceImportRetriesIncompleteParameterChangeAfterUnlock) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("analysis.DAT"));
    ASSERT_TRUE(writeSource(path,
            test::file(
                    {test::beatGrid({{1000, 12000, 1}, {1500, 12000, 2}})})));
    ASSERT_TRUE(importSources(path).complete);
    const auto original = track->getBeats();
    ASSERT_TRUE(original);
    EXPECT_FALSE(state.fingerprint.isEmpty());
    track->setBpmLocked(true);
    EXPECT_FALSE(importSources(path, false, 26).complete);
    EXPECT_TRUE(state.fingerprint.isEmpty());
    EXPECT_EQ(original, track->getBeats());
    track->setBpmLocked(false);
    ASSERT_TRUE(importSources(path, false, 26).complete);
    EXPECT_NE(original, track->getBeats());
    EXPECT_EQ(audio::FramePos(46752), *track->getBeats()->cfirstmarker());
    EXPECT_FALSE(state.fingerprint.isEmpty());
}

TEST_F(RekordboxAnlzImporterTest, SourceImportRestartsPreserveOccupiedSlots) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("analysis.DAT"));
    ASSERT_TRUE(writeSource(path, test::file({test::cueList(false, 1, {test::legacyCue()})})));
    ASSERT_TRUE(importSources(path).complete);
    const auto imported = track->findHotcueByIndex(0);
    ASSERT_TRUE(imported);
    state = AnlzImportState();
    ASSERT_TRUE(importSources(path).complete);
    ASSERT_TRUE(writeSource(path,
            test::file({test::cueList(false, 1, {test::legacyCue(1, 2, 3000, 4000)})})));
    EXPECT_FALSE(importSources(path).complete);
    EXPECT_EQ(audio::FramePos(48000), imported->getPosition());

    track = Track::newTemporary();
    track->setAudioProperties(audio::ChannelCount(2),
            rate,
            audio::Bitrate(),
            Duration::fromSeconds(60));
    const auto local = track->createAndAddCue(
            CueType::HotCue, 0, audio::FramePos(123), audio::kInvalidFramePos);
    local->setLabel(QStringLiteral("Local cue"));
    EXPECT_FALSE(importSources(path).complete);
    EXPECT_EQ(local, track->findHotcueByIndex(0));
    EXPECT_EQ(audio::FramePos(123), local->getPosition());
    EXPECT_EQ(QStringLiteral("Local cue"), local->getLabel());
}

TEST_F(RekordboxAnlzImporterTest, SourceImportRetriesCorruptCompanionFallback) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("analysis.DAT"));
    const auto extPath = directory.filePath(QStringLiteral("analysis.EXT"));
    ASSERT_TRUE(writeSource(path, test::file({test::cueList(false, 1, {test::legacyCue()})})));
    ASSERT_TRUE(writeSource(extPath, "invalid companion"));
    const auto fallback = importSources(path);
    EXPECT_FALSE(fallback.complete);
    EXPECT_FALSE(fallback.diagnostics.isEmpty());
    EXPECT_TRUE(state.fingerprint.isEmpty());
    const auto cue = track->findHotcueByIndex(0);
    ASSERT_TRUE(cue);
    ASSERT_TRUE(writeSource(extPath, test::file({test::cueList(true, 1, {test::extendedCue()})})));
    EXPECT_TRUE(importSources(path).complete);
    EXPECT_EQ(cue, track->findHotcueByIndex(0));
    EXPECT_EQ(QStringLiteral("Loop"), cue->getLabel());
    EXPECT_EQ(state.decodedSource.fingerprint, state.fingerprint);
}

TEST_F(RekordboxAnlzImporterTest, SourceImportCachesSupportedDataWithOpaqueCapabilities) {
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto path = directory.filePath(QStringLiteral("analysis.DAT"));
    ASSERT_TRUE(writeSource(path,
            test::file({test::cueList(false, 1, {test::legacyCue()}),
                    test::section("PWAV", 12, "opaque waveform")})));
    const auto imported = importSources(path);
    EXPECT_TRUE(imported.complete);
    EXPECT_TRUE(imported.diagnostics.isEmpty());
    EXPECT_EQ(AnlzStatus::Warning, state.decodedSource.decoded.status);
    EXPECT_EQ(state.decodedSource.fingerprint, state.fingerprint);
    const auto decoded = state.decodedSource.decoded.data;
    EXPECT_TRUE(importSources(path).complete);
    EXPECT_EQ(decoded, state.decodedSource.decoded.data);
}

TEST_F(RekordboxAnlzImporterTest, SourceImportRejectsInvalidTargetStateAndTiming) {
    const auto policy = library::MainCuePromotionPolicy::fromConfigValue(false);
    const QString path = QStringLiteral("unused.DAT");
    EXPECT_FALSE(importAnlzSources(track, rate, 0, path, policy, nullptr).complete);
    EXPECT_FALSE(importAnlzSources({}, rate, 0, path, policy, &state).complete);
    EXPECT_FALSE(importAnlzSources(track, audio::SampleRate(), 0, path, policy, &state).complete);
    EXPECT_FALSE(importAnlzSources(track, rate, 0, {}, policy, &state).complete);
    EXPECT_FALSE(importAnlzSources(track,
            rate,
            std::numeric_limits<double>::quiet_NaN(),
            path,
            policy,
            &state)
                    .complete);
    EXPECT_TRUE(state.fingerprint.isEmpty());
    EXPECT_TRUE(track->getCuePoints().isEmpty());
}

} // namespace
} // namespace mixxx::rekordbox
