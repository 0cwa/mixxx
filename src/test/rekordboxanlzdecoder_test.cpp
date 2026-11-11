#include "library/rekordbox/rekordboxanlzdecoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>

#include "test/rekordboxanlzfixtures.h"

namespace mixxx::rekordbox {
namespace {

using namespace test;

AnlzDecodeResult decode(const std::string& bytes,
        AnlzSource source = AnlzSource::Dat,
        const AnlzDecodeOptions& options = {}) {
    return decodeAnlz({source, "synthetic", bytes}, options);
}

TEST(RekordboxAnlzDecoderTest, LegacyHotLoopKeepsEndpointAndActiveIntent) {
    const auto result = decode(file({cueList(false, 1, {legacyCue()})}));
    ASSERT_EQ(result.status, AnlzStatus::Success);
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 1u);
    const auto& cue = result.data->cues.front();
    EXPECT_EQ(cue.role, AnlzCueRole::HotLoop);
    EXPECT_EQ(cue.hotSlot, 0u);
    EXPECT_EQ(anlzMillisecondsToFrames(cue.startMilliseconds, 48000), 48000);
    ASSERT_TRUE(cue.endMilliseconds);
    EXPECT_EQ(anlzMillisecondsToFrames(*cue.endMilliseconds, 48000), 96000);
    EXPECT_EQ(cue.activeLoop, true);
    EXPECT_FALSE(cue.labelUtf8);
    EXPECT_FALSE(cue.colorRgb);
    ASSERT_EQ(cue.provenance.size(), 1u);
    EXPECT_EQ(cue.provenance.front().section, "PCOB");
}

TEST(RekordboxAnlzDecoderTest, ExtendedHotLoopKeepsEndpointLabelAndColor) {
    const auto result = decode(file({cueList(true, 1, {extendedCue()})}), AnlzSource::Ext);
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 1u);
    const auto& cue = result.data->cues.front();
    EXPECT_EQ(cue.role, AnlzCueRole::HotLoop);
    EXPECT_EQ(cue.hotSlot, 0u);
    EXPECT_EQ(anlzMillisecondsToFrames(cue.startMilliseconds, 48000), 48000);
    ASSERT_TRUE(cue.endMilliseconds);
    EXPECT_EQ(anlzMillisecondsToFrames(*cue.endMilliseconds, 48000), 96000);
    EXPECT_EQ(cue.labelUtf8, "Loop");
    EXPECT_EQ(cue.colorRgb, 0x123456u);
    EXPECT_FALSE(cue.activeLoop);
}

TEST(RekordboxAnlzDecoderTest, PartialExtendedRecordHasUnknownLabelAndColor) {
    const auto result = decode(file({cueList(
            true, 1, {extendedCue(8, 1, 0, 0, std::nullopt, std::nullopt)})}));
    ASSERT_TRUE(result.usable());
    const auto& cue = result.data->cues.front();
    EXPECT_EQ(cue.hotSlot, 7u);
    EXPECT_EQ(cue.startMilliseconds, 0u);
    EXPECT_FALSE(cue.endMilliseconds);
    EXPECT_FALSE(cue.labelUtf8);
    EXPECT_FALSE(cue.colorRgb);
}

TEST(RekordboxAnlzDecoderTest, ExplicitEmptyAndBlackRemainDifferentFromMissing) {
    const auto result = decode(file({cueList(
            true, 1, {extendedCue(1, 1, 0, 0, std::u16string(), 0)})}));
    ASSERT_TRUE(result.usable());
    const auto& cue = result.data->cues.front();
    ASSERT_TRUE(cue.labelUtf8);
    EXPECT_TRUE(cue.labelUtf8->empty());
    ASSERT_TRUE(cue.colorRgb);
    EXPECT_EQ(*cue.colorRgb, 0u);
}

TEST(RekordboxAnlzDecoderTest, MemoryPaletteAndHotRgbHaveSeparateMeaning) {
    const auto result = decode(file(
            {cueList(true, 0, {extendedCue(0, 1, 1000, 0, u"Memory", 0, 2)}),
                    cueList(true,
                            1,
                            {extendedCue(
                                    1, 1, 1000, 0, u"Hot", 0x123456, 8)})}));
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 2u);
    EXPECT_EQ(result.data->cues[0].paletteId, 2u);
    EXPECT_FALSE(result.data->cues[0].colorRgb);
    EXPECT_FALSE(result.data->cues[0].hotColorCode);
    EXPECT_FALSE(result.data->cues[1].paletteId);
    EXPECT_EQ(result.data->cues[1].colorRgb, 0x123456u);
}

TEST(RekordboxAnlzDecoderTest, ExtendedEntryLengthDeterminesNextRecord) {
    auto first = extendedCue();
    first.append(17, '\x7f');
    setU32(&first, 8, static_cast<std::uint32_t>(first.size()));
    const auto result = decode(file({cueList(true,
            1,
            {first, extendedCue(5, 1, 3000, 0, u"Later", std::nullopt)})}));
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 2u);
    EXPECT_EQ(result.data->cues.back().hotSlot, 4u);
    EXPECT_EQ(result.data->cues.back().labelUtf8, "Later");
}

TEST(RekordboxAnlzDecoderTest, Utf16SurrogatesAndTrailingNullAreDecoded) {
    const auto result = decode(file(
            {cueList(true, 1, {extendedCue(1, 1, 0, 0, u"\U0001f3b5 café")})}));
    ASSERT_TRUE(result.usable());
    EXPECT_EQ(result.data->cues.front().labelUtf8, "\xf0\x9f\x8e\xb5 caf\xc3\xa9");
}

TEST(RekordboxAnlzDecoderTest, BeatGridKeepsZeroTimeTempoAndBarPosition) {
    const auto result = decode(file(
            {beatGrid({{0, 12000, 3}, {500, 12000, 4}, {1000, 12800, 1}})}));
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->beats.size(), 3u);
    EXPECT_EQ(result.data->beats.front().timeMilliseconds, 0u);
    EXPECT_EQ(result.data->beats.front().beatInBar, 3u);
    EXPECT_EQ(result.data->beats.back().tempoHundredths, 12800u);
}

TEST(RekordboxAnlzDecoderTest, ExtendedCuesWinAndLegacyActiveLoopSurvives) {
    const auto dat = file({cueList(false, 1, {legacyCue()})});
    const auto ext = file({cueList(true, 1, {extendedCue()})});
    const auto result = decodeAnlzCompanions({{AnlzSource::Ext, "ext", ext},
            {AnlzSource::Dat, "dat", dat}});
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 1u);
    EXPECT_EQ(result.data->cues.front().labelUtf8, "Loop");
    EXPECT_EQ(result.data->cues.front().activeLoop, true);
    EXPECT_EQ(result.data->cues.front().provenance.size(), 2u);
}

TEST(RekordboxAnlzDecoderTest, ExtendedHotSlotCanChangeFromPointToLoop) {
    const auto dat = file({cueList(false, 1, {legacyCue(1, 1, 1000, 0, 0)})});
    const auto ext = file({cueList(true, 1, {extendedCue()})});
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", dat},
            {AnlzSource::Ext, "ext", ext}});
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 1u);
    EXPECT_EQ(result.data->cues.front().role, AnlzCueRole::HotLoop);
    EXPECT_EQ(result.data->cues.front().endMilliseconds, 2000u);
    EXPECT_FALSE(result.data->cues.front().activeLoop);
}

TEST(RekordboxAnlzDecoderTest, ExtendedHotSlotCanChangeFromLoopToPoint) {
    const auto dat = file({cueList(false, 1, {legacyCue()})});
    const auto ext = file({cueList(true, 1, {extendedCue(1, 1, 1000, 0)})});
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", dat},
            {AnlzSource::Ext, "ext", ext}});
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 1u);
    EXPECT_EQ(result.data->cues.front().role, AnlzCueRole::HotCue);
    EXPECT_FALSE(result.data->cues.front().endMilliseconds);
    EXPECT_FALSE(result.data->cues.front().activeLoop);
}

TEST(RekordboxAnlzDecoderTest, SameTimeDistinctMemoryAnnotationsAreRetained) {
    const auto result = decode(file({cueList(true,
            0,
            {extendedCue(0, 1, 1000, 0, u"Same", 0x112233),
                    extendedCue(0, 1, 1000, 0, u"Same", 0x445566),
                    extendedCue(0, 1, 1000, 0, u"Same", 0x112233)})}));
    ASSERT_TRUE(result.usable());
    EXPECT_EQ(result.data->cues.size(), 3u);
    for (const auto& cue : result.data->cues) {
        EXPECT_EQ(cue.role, AnlzCueRole::MemoryCue);
        EXPECT_FALSE(cue.hotSlot);
    }
}

TEST(RekordboxAnlzDecoderTest, MemoryDuplicatesReconcileOneForOneByOrdinal) {
    const auto dat = file({cueList(false, 0, {legacyCue(0, 2), legacyCue(0, 2)})});
    const auto ext = file({cueList(true,
            0,
            {extendedCue(0, 2, 1000, 2000, u"One"),
                    extendedCue(0, 2, 1000, 2000, u"Two")})});
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", dat},
            {AnlzSource::Ext, "ext", ext}});
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 2u);
    EXPECT_EQ(result.data->cues[0].labelUtf8, "One");
    EXPECT_EQ(result.data->cues[1].labelUtf8, "Two");
    EXPECT_EQ(result.data->cues[0].provenance.size(), 2u);
    EXPECT_EQ(result.data->cues[1].provenance.size(), 2u);
}

TEST(RekordboxAnlzDecoderTest, OrderingIsStrictForEveryPresentColorCombination) {
    std::vector<AnlzCue> cues(5);
    cues[0].colorRgb = 0x112233;
    cues[1].colorRgb = 0x445566;
    cues[2].colorRgb = 0;
    cues[4].colorRgb = 0xffffff;
    for (const auto& a : cues) {
        EXPECT_FALSE(anlzCueLess(a, a));
        for (const auto& b : cues) {
            EXPECT_FALSE(anlzCueLess(a, b) && anlzCueLess(b, a));
            for (const auto& c : cues) {
                if (anlzCueLess(a, b) && anlzCueLess(b, c)) {
                    EXPECT_TRUE(anlzCueLess(a, c));
                }
            }
        }
    }
    auto reversed = cues;
    std::reverse(reversed.begin(), reversed.end());
    std::sort(cues.begin(), cues.end(), anlzCueLess);
    std::sort(reversed.begin(), reversed.end(), anlzCueLess);
    for (std::size_t index = 0; index < cues.size(); ++index) {
        EXPECT_EQ(cues[index].colorRgb, reversed[index].colorRgb);
    }
}

TEST(RekordboxAnlzDecoderTest, DatBeatGridHasPriorityRegardlessOfInputOrder) {
    const auto dat = file({beatGrid({{0, 12000, 3}, {500, 12000, 4}})});
    const auto ext = file({beatGrid({{10, 12000, 1}, {510, 12000, 2}})});
    const auto result = decodeAnlzCompanions({{AnlzSource::Ext, "ext", ext},
            {AnlzSource::Dat, "dat", dat}});
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->beats.size(), 2u);
    EXPECT_EQ(result.data->beats.front().timeMilliseconds, 0u);
    EXPECT_EQ(result.data->beats.front().beatInBar, 3u);
    ASSERT_TRUE(result.data->beatProvenance);
    EXPECT_EQ(result.data->beatProvenance->source, AnlzSource::Dat);
}

TEST(RekordboxAnlzDecoderTest, CorruptExtendedCompanionFallsBackWithWarning) {
    const auto dat = file({cueList(false, 1, {legacyCue()})});
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", dat},
            {AnlzSource::Ext, "ext", "truncated"}});
    ASSERT_EQ(result.status, AnlzStatus::Warning);
    ASSERT_TRUE(result.usable());
    EXPECT_EQ(result.data->cues.size(), 1u);
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics.front().status, AnlzStatus::Invalid);
}

TEST(RekordboxAnlzDecoderTest, MissingCompanionIsNotReportedAsCompleteSuccess) {
    const auto dat = file({cueList(false, 1, {legacyCue()})});
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", dat},
            {AnlzSource::Ext, "ext", {}, AnlzStatus::Unavailable}});
    EXPECT_EQ(result.status, AnlzStatus::Warning);
    EXPECT_TRUE(result.usable());
}

TEST(RekordboxAnlzDecoderTest, MissingCompanionDoesNotHideCorruptPrimary) {
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", "invalid"},
            {AnlzSource::Ext, "ext", {}, AnlzStatus::Unavailable}});
    EXPECT_EQ(result.status, AnlzStatus::Invalid);
    EXPECT_FALSE(result.usable());
}

TEST(RekordboxAnlzDecoderTest, UnsupportedCompanionBytesAreRetained) {
    const auto dat = file({cueList(false, 1, {legacyCue()})});
    auto unknown = file({section("NEW!", 12, "opaque")});
    unknown[12] = '\x7f';
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", dat},
            {AnlzSource::TwoEx, "2ex", unknown}});
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->rawSections.size(), 2u);
    EXPECT_EQ(result.data->rawSections.back().bytes, section("NEW!", 12, "opaque"));
    ASSERT_EQ(result.data->fileHeaders.size(), 2u);
    EXPECT_EQ(result.data->fileHeaders.back().bytes, unknown.substr(0, 28));
}

TEST(RekordboxAnlzDecoderTest, DuplicateCompanionTypesAreRejected) {
    const auto bytes = file({});
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "one", bytes},
            {AnlzSource::Dat, "two", bytes}});
    EXPECT_EQ(result.status, AnlzStatus::Invalid);
    EXPECT_FALSE(result.data);
}

TEST(RekordboxAnlzDecoderTest, CorruptThenRepairedInputHasNoHiddenCache) {
    const auto complete = file({cueList(true, 1, {extendedCue()})});
    const auto broken = complete.substr(0, complete.size() - 1);
    EXPECT_EQ(decode(broken).status, AnlzStatus::Invalid);
    const auto result = decode(complete);
    EXPECT_EQ(result.status, AnlzStatus::Success);
    EXPECT_TRUE(result.usable());
}

TEST(RekordboxAnlzDecoderTest, AllTruncationsRejectWithoutPublishingPartialData) {
    const auto complete = file({cueList(false, 1, {legacyCue()}),
            cueList(true, 1, {extendedCue()}),
            beatGrid({{0, 12000, 1}, {500, 12000, 2}})});
    for (std::size_t length = 0; length < complete.size(); ++length) {
        const auto result = decode(complete.substr(0, length));
        EXPECT_EQ(result.status, AnlzStatus::Invalid) << length;
        EXPECT_FALSE(result.data) << length;
    }
}

TEST(RekordboxAnlzDecoderTest, InvalidEnvelopeLengthsAreBounded) {
    const auto valid = file({cueList(true, 1, {extendedCue()})});
    for (const auto offset : {4u, 8u, 32u, 36u, 52u, 56u}) {
        auto invalid = valid;
        setU32(&invalid, offset, 0xffffffff);
        EXPECT_EQ(decode(invalid).status, AnlzStatus::Invalid) << offset;
    }
}

TEST(RekordboxAnlzDecoderTest, TruncatedCueBodiesWithConsistentEnvelopesAreRejected) {
    const auto complete = file({cueList(true, 1, {extendedCue()})});
    for (std::size_t length = 48; length < complete.size(); ++length) {
        auto bytes = complete.substr(0, length);
        setU32(&bytes, 8, static_cast<std::uint32_t>(bytes.size()));
        setU32(&bytes, 36, static_cast<std::uint32_t>(bytes.size() - 28));
        if (length >= 60) {
            setU32(&bytes, 56, static_cast<std::uint32_t>(bytes.size() - 48));
        }
        const auto result = decode(bytes);
        if (length == 88 || (length >= 89 && length < 92) || length >= complete.size() - 4) {
            EXPECT_TRUE(result.usable()) << length;
        } else {
            EXPECT_EQ(result.status, AnlzStatus::Invalid) << length;
            EXPECT_FALSE(result.data) << length;
        }
    }
}

TEST(RekordboxAnlzDecoderTest, MutatedBinaryFieldsNeverLeakPartialInvalidResults) {
    const auto valid = file({cueList(false, 1, {legacyCue()}),
            cueList(true, 1, {extendedCue()}),
            beatGrid({{0, 12000, 1}, {500, 12000, 2}})});
    std::mt19937 random(0x504d4149);
    for (std::size_t iteration = 0; iteration < 2000; ++iteration) {
        auto bytes = valid;
        const auto offset = random() % bytes.size();
        bytes[offset] = static_cast<char>(random());
        const auto result = decode(bytes);
        if (result.status == AnlzStatus::Invalid || result.status == AnlzStatus::Failed) {
            EXPECT_FALSE(result.data) << iteration;
        } else {
            EXPECT_TRUE(result.data) << iteration;
        }
    }
}

TEST(RekordboxAnlzDecoderTest, InvalidCommentLengthCannotConsumeTheNextEntry) {
    auto entry = extendedCue();
    setU32(&entry, 40, 0xffffffff);
    const auto result = decode(file({cueList(true, 1, {entry, extendedCue(2)})}));
    EXPECT_EQ(result.status, AnlzStatus::Invalid);
    EXPECT_FALSE(result.data);
}

TEST(RekordboxAnlzDecoderTest, OddUtf16AndUnpairedSurrogatesAreRejected) {
    auto odd = extendedCue();
    setU32(&odd, 40, 3);
    EXPECT_EQ(decode(file({cueList(true, 1, {odd})})).status, AnlzStatus::Invalid);
    auto surrogate = extendedCue();
    setU16(&surrogate, 44, 0xd800);
    EXPECT_EQ(decode(file({cueList(true, 1, {surrogate})})).status, AnlzStatus::Invalid);
}

TEST(RekordboxAnlzDecoderTest, InvalidLoopIntervalsAndHotSlotZeroAreRejected) {
    EXPECT_EQ(decode(file({cueList(false, 1, {legacyCue(1, 2, 2000, 1000)})})).status,
            AnlzStatus::Invalid);
    EXPECT_EQ(decode(file({cueList(true, 1, {extendedCue(0)})})).status, AnlzStatus::Invalid);
    EXPECT_EQ(decode(file({cueList(false, 1, {legacyCue(), legacyCue()})})).status,
            AnlzStatus::Invalid);
}

TEST(RekordboxAnlzDecoderTest, DecreasingAndDuplicateBeatTimesAreRejected) {
    EXPECT_EQ(decode(file({beatGrid({{500, 12000, 1}, {0, 12000, 2}})})).status,
            AnlzStatus::Invalid);
    EXPECT_EQ(decode(file({beatGrid({{0, 12000, 1}, {0, 12000, 2}})})).status,
            AnlzStatus::Invalid);
}

TEST(RekordboxAnlzDecoderTest, EveryConfiguredLimitRejectsBeforePublication) {
    const auto bytes = file({cueList(true, 1, {extendedCue()}), beatGrid({{0, 12000, 1}})});
    AnlzDecodeOptions options;
    options.limits.maxFileBytes = bytes.size() - 1;
    EXPECT_EQ(decode(bytes, AnlzSource::Dat, options).status, AnlzStatus::Invalid);
    options = {};
    options.limits.maxSections = 1;
    EXPECT_EQ(decode(bytes, AnlzSource::Dat, options).status, AnlzStatus::Invalid);
    options = {};
    options.limits.maxCues = 0;
    EXPECT_EQ(decode(bytes, AnlzSource::Dat, options).status, AnlzStatus::Invalid);
    options = {};
    options.limits.maxBeats = 0;
    EXPECT_EQ(decode(bytes, AnlzSource::Dat, options).status, AnlzStatus::Invalid);
    options = {};
    options.limits.maxStringBytes = 2;
    EXPECT_EQ(decode(bytes, AnlzSource::Dat, options).status, AnlzStatus::Invalid);
    options = {};
    options.limits.maxPreservedBytes = 1;
    EXPECT_EQ(decode(bytes, AnlzSource::Dat, options).status, AnlzStatus::Invalid);
}

TEST(RekordboxAnlzDecoderTest, CancellationAndWorkerExceptionsAreExplicit) {
    const auto bytes = file({cueList(false, 1, {legacyCue()})});
    AnlzDecodeOptions options;
    options.isCancelled = [] { return true; };
    const auto cancelled = decode(bytes, AnlzSource::Dat, options);
    EXPECT_EQ(cancelled.status, AnlzStatus::Cancelled);
    EXPECT_FALSE(cancelled.data);
    options.isCancelled = []() -> bool { throw std::runtime_error("worker failure"); };
    const auto failed = decode(bytes, AnlzSource::Dat, options);
    EXPECT_EQ(failed.status, AnlzStatus::Failed);
    EXPECT_FALSE(failed.data);
}

TEST(RekordboxAnlzDecoderTest, CancellationOrWorkerFailureDiscardsAggregate) {
    const auto dat = file({});
    for (const auto status : {AnlzStatus::Cancelled, AnlzStatus::Failed}) {
        const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", dat},
                {AnlzSource::Ext, "ext", {}, status}});
        EXPECT_EQ(result.status, status);
        EXPECT_FALSE(result.data);
    }
}

TEST(RekordboxAnlzDecoderTest, DeniedUnavailableAndUnsupportedRemainDistinct) {
    EXPECT_EQ(decodeAnlz({AnlzSource::Dat, "dat", {}, AnlzStatus::Denied}).status,
            AnlzStatus::Denied);
    EXPECT_EQ(decodeAnlzCompanions({}).status, AnlzStatus::Unavailable);
    const auto result = decode(file({section("NEW!", 12, "opaque bytes")}));
    EXPECT_EQ(result.status, AnlzStatus::Unsupported);
    ASSERT_TRUE(result.data);
    ASSERT_EQ(result.data->rawSections.size(), 1u);
    EXPECT_EQ(result.data->rawSections.front().bytes, section("NEW!", 12, "opaque bytes"));
}

TEST(RekordboxAnlzDecoderTest, UnsupportedOnlyCueTypesAreNotReportedAsUsable) {
    const auto result = decode(file({cueList(false, 1, {legacyCue(1, 99)})}));
    EXPECT_EQ(result.status, AnlzStatus::Unsupported);
    EXPECT_FALSE(result.usable());
    ASSERT_TRUE(result.data);
    EXPECT_TRUE(result.data->cues.empty());
    EXPECT_EQ(result.data->rawSections.front().capability, AnlzSectionCapability::Unsupported);
}

TEST(RekordboxAnlzDecoderTest, LargeCueListRetainsOrdinalsAndUsesBoundedReconciliation) {
    std::vector<std::string> legacy;
    std::vector<std::string> extended;
    for (std::uint32_t index = 0; index < 10000; ++index) {
        legacy.push_back(legacyCue(0, 1, index * 1000, 0, 0));
        extended.push_back(extendedCue(0, 1, index * 1000, 0, u"Cue"));
    }
    const auto dat = file({cueList(false, 0, legacy)});
    const auto ext = file({cueList(true, 0, extended)});
    const auto result = decodeAnlzCompanions({{AnlzSource::Dat, "dat", dat},
            {AnlzSource::Ext, "ext", ext}});
    ASSERT_TRUE(result.usable());
    ASSERT_EQ(result.data->cues.size(), 10000u);
    EXPECT_EQ(result.data->cues.back().startMilliseconds, 9999000u);
    EXPECT_EQ(result.data->cues.back().provenance.size(), 2u);
}

TEST(RekordboxAnlzDecoderTest, DoubleExtendedWaveformAndPhrasesDeclareCapabilities) {
    const auto result = decode(file({section("PWV6", 20, std::string(8, '\0')),
                                       section("PSSI", 32, std::string(20, '\0'))}),
            AnlzSource::TwoEx);
    ASSERT_EQ(result.status, AnlzStatus::Warning);
    ASSERT_TRUE(result.data);
    ASSERT_EQ(result.data->rawSections.size(), 2u);
    EXPECT_EQ(result.data->rawSections[0].capability, AnlzSectionCapability::RegenerateWaveform);
    EXPECT_EQ(result.data->rawSections[1].capability, AnlzSectionCapability::Unsupported);
}

TEST(RekordboxAnlzDecoderTest, TimingConversionPreservesZeroNegativeAndSourceUnits) {
    EXPECT_EQ(anlzMillisecondsToFrames(0, 48000), 0);
    EXPECT_EQ(anlzMillisecondsToFrames(0, 48000, 10), -480);
    EXPECT_EQ(anlzMillisecondsToFrames(-10, 48000), -480);
    EXPECT_EQ(anlzMillisecondsToFrames(1000, 44100), 44100);
    EXPECT_EQ(anlzMillisecondsToFrames(1000, 96000, 10), 95040);
    EXPECT_TRUE(std::isnan(anlzMillisecondsToFrames(1000, 0)));
    EXPECT_TRUE(std::isnan(anlzMillisecondsToFrames(
            1000, 48000, std::numeric_limits<double>::infinity())));
}

} // namespace
} // namespace mixxx::rekordbox
