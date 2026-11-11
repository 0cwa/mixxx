// Prepared by ChatGPT for human review. Fixtures are synthetic and schema-authored.
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "library/rekordbox/rekordboxanlzdecoder.h"

namespace {

using namespace mixxx::rekordbox;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void setU32(std::string* bytes, std::size_t offset, std::uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        bytes->at(offset + i) = static_cast<char>(value & 0xff);
        value >>= 8;
    }
}

void setU16(std::string* bytes, std::size_t offset, std::uint16_t value) {
    bytes->at(offset) = static_cast<char>(value >> 8);
    bytes->at(offset + 1) = static_cast<char>(value & 0xff);
}

std::string classicCue(std::uint32_t slot,
        std::uint8_t type,
        std::uint32_t start,
        std::uint32_t end,
        std::uint32_t status = 1) {
    std::string bytes(56, '\0');
    bytes.replace(0, 4, "PCPT");
    setU32(&bytes, 4, 28);
    setU32(&bytes, 8, 56);
    setU32(&bytes, 12, slot);
    setU32(&bytes, 16, status);
    setU32(&bytes, 20, 0x10000);
    setU16(&bytes, 24, 0xffff);
    setU16(&bytes, 26, 0xffff);
    bytes[28] = static_cast<char>(type);
    setU32(&bytes, 32, start);
    setU32(&bytes, 36, end);
    return bytes;
}

std::string extendedCue(std::uint32_t slot,
        std::uint8_t type,
        std::uint32_t start,
        std::uint32_t end,
        const std::string& labelUtf16Be = std::string("\0\0", 2),
        std::optional<AnlzRgb> color = std::nullopt) {
    std::string bytes(44 + labelUtf16Be.size() + (color ? 4 : 0), '\0');
    bytes.replace(0, 4, "PCP2");
    setU32(&bytes, 4, 16);
    setU32(&bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    setU32(&bytes, 12, slot);
    bytes[16] = static_cast<char>(type);
    setU32(&bytes, 20, start);
    setU32(&bytes, 24, end);
    bytes[28] = 2;
    setU16(&bytes, 36, type == 2 ? 4 : 0);
    setU16(&bytes, 38, type == 2 ? 1 : 0);
    setU32(&bytes, 40, static_cast<std::uint32_t>(labelUtf16Be.size()));
    bytes.replace(44, labelUtf16Be.size(), labelUtf16Be);
    if (color) {
        const auto offset = 44 + labelUtf16Be.size();
        bytes[offset] = 0x22;
        bytes[offset + 1] = static_cast<char>(color->red);
        bytes[offset + 2] = static_cast<char>(color->green);
        bytes[offset + 3] = static_cast<char>(color->blue);
    }
    return bytes;
}

std::string cueSection(bool extended,
        std::uint32_t listKind,
        const std::vector<std::string>& cues) {
    std::string bytes(extended ? 20 : 24, '\0');
    bytes.replace(0, 4, extended ? "PCO2" : "PCOB");
    setU32(&bytes, 4, static_cast<std::uint32_t>(bytes.size()));
    setU32(&bytes, 12, listKind);
    setU16(&bytes, extended ? 16 : 18, static_cast<std::uint16_t>(cues.size()));
    for (const auto& cue : cues) {
        bytes += cue;
    }
    setU32(&bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    return bytes;
}

struct BeatFixture {
    std::uint16_t beatInBar;
    std::uint16_t tempoHundredths;
    std::uint32_t milliseconds;
};

std::string beatSection(const std::vector<BeatFixture>& beats) {
    std::string bytes(24 + beats.size() * 8, '\0');
    bytes.replace(0, 4, "PQTZ");
    setU32(&bytes, 4, 24);
    setU32(&bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    setU32(&bytes, 16, 0x80000);
    setU32(&bytes, 20, static_cast<std::uint32_t>(beats.size()));
    for (std::size_t i = 0; i < beats.size(); ++i) {
        setU16(&bytes, 24 + i * 8, beats[i].beatInBar);
        setU16(&bytes, 26 + i * 8, beats[i].tempoHundredths);
        setU32(&bytes, 28 + i * 8, beats[i].milliseconds);
    }
    return bytes;
}

std::string file(const std::vector<std::string>& sections) {
    std::string bytes(28, '\0');
    bytes.replace(0, 4, "PMAI");
    setU32(&bytes, 4, 28);
    for (const auto& section : sections) {
        bytes += section;
    }
    setU32(&bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    return bytes;
}

void expectOutcome(const AnlzDecodeResult& result, AnlzOutcome outcome) {
    std::string diagnostic;
    for (const auto& item : result.diagnostics) {
        diagnostic += item.code + ": " + item.message + "; ";
    }
    expect(result.outcome == outcome, "Unexpected decode outcome: " + diagnostic);
    const bool usable = outcome == AnlzOutcome::Success || outcome == AnlzOutcome::Warning;
    expect(static_cast<bool>(result.snapshot) == usable, "Snapshot publication is not atomic");
}

void preservesClassicHotLoop() {
    const auto result = decodeAnlz(file({cueSection(false, 1, {classicCue(3, 2, 4000, 6000, 4)})}));
    expectOutcome(result, AnlzOutcome::Success);
    const auto& cue = result.snapshot->cues.at(0);
    expect(cue.role == AnlzCueRole::Loop && cue.bank == AnlzCueBank::Hot, "Loop role lost");
    expect(cue.hotSlot == 2, "Hot slot is not zero-based");
    expect(cue.end.has_value() && cue.end->milliseconds == 6000, "Loop endpoint lost");
    expect(cue.activeLoop == true && cue.enabled == true, "Active-loop intent lost");
    expect(anlzTimestampToPcmFrames(cue.start, 48000) == 192000, "Loop start is not PCM frames");
    expect(anlzTimestampToPcmFrames(*cue.end, 48000) == 288000, "Loop end is not PCM frames");
}

void preservesExtendedHotLoopAndUnicode() {
    const std::string label("\x00\xc5\x00\x20\xd8\x3c\xdf\xb5\x00\x00", 10);
    const auto result = decodeAnlz(file({cueSection(true,
            1,
            {extendedCue(8, 2, 4000, 6000, label, AnlzRgb{16, 32, 48})})}));
    expectOutcome(result, AnlzOutcome::Success);
    const auto& cue = result.snapshot->cues.at(0);
    expect(cue.hotSlot == 7 && cue.end->milliseconds == 6000, "Extended hot loop lost");
    expect(cue.labelUtf8 == std::string("\xc3\x85 \xf0\x9f\x8e\xb5"), "UTF-16 label changed");
    expect(cue.rgb && cue.rgb->red == 16 && cue.rgb->green == 32 && cue.rgb->blue == 48,
            "Explicit RGB was lost");
    expect(!cue.activeLoop && !cue.sourceStatus, "Extended cue invented active-loop intent");
    expect(cue.loopNumerator == 4 && cue.loopDenominator == 1, "Quantized loop fields lost");
}

void preservesMemoryIdentityAndZero() {
    const auto result = decodeAnlz(file({cueSection(false,
            0,
            {classicCue(0, 1, 0, 0), classicCue(0, 2, 4000, 6000)})}));
    expectOutcome(result, AnlzOutcome::Success);
    const auto& cues = result.snapshot->cues;
    expect(cues.size() == 2 && cues[0].role == AnlzCueRole::MemoryCue, "Memory role lost");
    expect(!cues[0].hotSlot && !cues[1].hotSlot, "Memory cue acquired a hot slot");
    expect(cues[0].start.milliseconds == 0 && !cues[0].end, "Zero clamped or point became loop");
    expect(cues[0].key.entryOrdinal == 0 && cues[1].key.entryOrdinal == 1, "Identity lost");
    expect(cues[0].sourceOrderFirst == 0xffff, "Source memory order lost");
}

void preservesSameTimeDifferentColors() {
    const auto result = decodeAnlz(file({cueSection(true,
            0,
            {extendedCue(0, 1, 1000, 0, {}, AnlzRgb{255, 0, 0}),
                    extendedCue(0, 1, 1000, 0, {}, AnlzRgb{0, 0, 255})})}));
    expectOutcome(result, AnlzOutcome::Success);
    const auto& cues = result.snapshot->cues;
    expect(cues.size() == 2 && cues[0].rgb->red == 255 && cues[1].rgb->blue == 255,
            "Same-time cues were merged");
}

void preservesDuplicatePhysicalRecords() {
    const auto cue = classicCue(0, 1, 1000, 0);
    const auto result = decodeAnlz(file({cueSection(false, 0, {cue, cue}),
            cueSection(false, 0, {cue})}));
    expectOutcome(result, AnlzOutcome::Success);
    expect(result.snapshot->cues.size() == 3, "Physical cue records were collapsed");
    expect(result.snapshot->cues[2].key.sectionOrdinal == 1, "Section identity lost");
}

void preservesExplicitBeatTimingAndPhase() {
    const auto result = decodeAnlz(file({beatSection({{3, 12000, 500},
            {4, 12000, 1000},
            {1, 11999, 1501},
            {3, 15000, 30500},
            {4, 15000, 30900},
            {1, 15000, 31300}})}));
    expectOutcome(result, AnlzOutcome::Success);
    const auto& beats = result.snapshot->beats;
    expect(beats.size() == 6 && beats[2].time.milliseconds == 1501,
            "Explicit beat spacing changed");
    expect(beats[0].sourceBeatInBar == 3 && beats[4].sourceBeatInBar == 4 &&
                    beats[5].sourceBeatInBar == 1,
            "Beat phase lost");
    expect(beats[2].sourceTempoHundredths == 11999, "Source tempo precision lost");
    expect(anlzTimestampToPcmFrames(beats[4].time, 44100) == 1362690, "Beat frame timing wrong");
}

void rejectsPartialSnapshot() {
    auto bad = cueSection(false, 0, {classicCue(0, 1, 1000, 0)});
    bad.resize(bad.size() - 1);
    const auto result = decodeAnlz(file({beatSection({{1, 12000, 0}}), bad}));
    expectOutcome(result, AnlzOutcome::Invalid);
    expect(result.diagnostics.back().sectionOrdinal == 1, "Error section context missing");
}

void refusesUnknownCueType() {
    expectOutcome(decodeAnlz(file({cueSection(false, 0, {classicCue(0, 7, 1000, 0)})})),
            AnlzOutcome::Unsupported);
}

void reportsUnknownStatus() {
    const auto result = decodeAnlz(file({cueSection(false, 0, {classicCue(0, 1, 1000, 0, 7)})}));
    expectOutcome(result, AnlzOutcome::Warning);
    expect(result.snapshot->cues[0].sourceStatus == 7 && !result.snapshot->cues[0].activeLoop,
            "Unknown source status was invented");
}

void rejectsCountOverflow() {
    auto section = beatSection({{1, 12000, 0}});
    setU32(&section, 20, 0xffffffff);
    expectOutcome(decodeAnlz(file({section})), AnlzOutcome::Invalid);
}

void rejectsEntryCrossingSection() {
    auto section = cueSection(false, 0, {classicCue(0, 1, 1000, 0)});
    setU32(&section, 24 + 8, 0xffffffff);
    expectOutcome(decodeAnlz(file({section})), AnlzOutcome::Invalid);
}

void rejectsCommentCrossingEntry() {
    auto cue = extendedCue(1, 1, 1000, 0);
    setU32(&cue, 40, 0xffffffff);
    expectOutcome(decodeAnlz(file({cueSection(true, 1, {cue})})), AnlzOutcome::Invalid);
}

void rejectsMalformedUnicode() {
    const auto cue = extendedCue(1, 1, 1000, 0, std::string("\xd8\x00\x00\x00", 4));
    expectOutcome(decodeAnlz(file({cueSection(true, 1, {cue})})), AnlzOutcome::Invalid);
}

void distinguishesAbsentAndEmptyLabel() {
    const auto result = decodeAnlz(file({cueSection(false, 0, {classicCue(0, 1, 1000, 0)}),
            cueSection(true, 0, {extendedCue(0, 1, 1000, 0, {})})}));
    expectOutcome(result, AnlzOutcome::Success);
    expect(!result.snapshot->cues[0].labelUtf8, "Absent label became empty");
    expect(result.snapshot->cues[1].labelUtf8 == std::string(), "Empty label became absent");
}

void preservesShortExtendedCueSafely() {
    auto cue = extendedCue(1, 1, 1000, 0);
    cue.resize(40);
    setU32(&cue, 8, 40);
    const auto result = decodeAnlz(file({cueSection(true, 1, {cue})}));
    expectOutcome(result, AnlzOutcome::Success);
    const auto& decoded = result.snapshot->cues.at(0);
    expect(decoded.hotSlot == 0 && decoded.start.milliseconds == 1000,
            "Short extended cue lost its timing or slot");
    expect(!decoded.labelUtf8 && !decoded.rgb, "Short cue invented optional fields");
}

void refusesInconsistentSlot() {
    expectOutcome(decodeAnlz(file({cueSection(false, 1, {classicCue(0, 1, 1000, 0)})})),
            AnlzOutcome::Invalid);
}

void rejectsNonpositiveLoop() {
    expectOutcome(decodeAnlz(file({cueSection(false, 1, {classicCue(1, 2, 1000, 1000)})})),
            AnlzOutcome::Invalid);
}

void rejectsUnorderedBeats() {
    expectOutcome(decodeAnlz(file({beatSection({{1, 12000, 500}, {2, 12000, 500}})})),
            AnlzOutcome::Invalid);
}

void rejectsExtraTrailingContent() {
    auto bytes = file({});
    bytes += "x";
    expectOutcome(decodeAnlz(bytes), AnlzOutcome::Invalid);
    setU32(&bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    expectOutcome(decodeAnlz(bytes), AnlzOutcome::Invalid);
}

void supportsEmptySections() {
    const auto result = decodeAnlz(file({cueSection(false, 0, {}),
            cueSection(true, 1, {}),
            beatSection({})}));
    expectOutcome(result, AnlzOutcome::Success);
    expect(result.snapshot->cues.empty() && result.snapshot->beats.empty(), "Empty source changed");
    expect(result.snapshot->cueLists.size() == 2 &&
                    result.snapshot->cueLists[0].bank == AnlzCueBank::Memory &&
                    result.snapshot->cueLists[0].entryCount == 0 &&
                    result.snapshot->cueLists[1].bank == AnlzCueBank::Hot,
            "Declared empty cue banks became indistinguishable from absent banks");
}

void cancellationNeverPublishes() {
    std::uint32_t checks = 0;
    const auto result = decodeAnlz(
            file({cueSection(false,
                    0,
                    {classicCue(0, 1, 1000, 0), classicCue(0, 1, 2000, 0)})}),
            [&checks] { return ++checks == 4; });
    expectOutcome(result, AnlzOutcome::Cancelled);
    expect(checks == 4, "Cancellation did not occur during decoding");
}

void sourceBytesStayUnchanged() {
    const auto bytes = file({cueSection(false, 0, {classicCue(0, 1, 1000, 0)})});
    const auto before = bytes;
    expectOutcome(decodeAnlz(bytes), AnlzOutcome::Success);
    expect(bytes == before, "Source data was mutated");
}

void checkedFrameConversion() {
    expect(anlzTimestampToPcmFrames({5}, 44100) == 221, "Half-frame rounding is wrong");
    expect(anlzTimestampToPcmFrames({0}, 48000) == 0, "Zero position changed");
    expect(anlzTimestampToPcmFrames({4000}, 48000, 26) == 190752,
            "Correction was not applied once");
    expect(anlzTimestampToPcmFrames({0}, 48000, -1) == 48, "Negative correction is wrong");
    expect(!anlzTimestampToPcmFrames({0}, 48000, 1), "Negative final time was silently clamped");
    expect(!anlzTimestampToPcmFrames({0}, 0), "Invalid sample rate accepted");
    expect(!anlzTimestampToPcmFrames({1}, 48000, std::numeric_limits<std::int64_t>::min()),
            "Signed correction overflow accepted");
    expect(!anlzTimestampToPcmFrames(
                   {0}, 0xffffffff, -std::numeric_limits<std::int64_t>::max()),
            "Frame overflow accepted");
}

void unknownSectionsAreExplicitlyOutsideCapability() {
    std::string section(12, '\0');
    section.replace(0, 4, "ZZZZ");
    setU32(&section, 4, 12);
    setU32(&section, 8, 12);
    const auto result = decodeAnlz(file({section}));
    expectOutcome(result, AnlzOutcome::Success);
    expect(result.diagnostics.size() == 1 &&
                    result.diagnostics[0].code == "section-outside-capability",
            "Unknown section disappeared without a diagnostic");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
            {"PreservesClassicHotLoop", preservesClassicHotLoop},
            {"PreservesExtendedHotLoopAndUnicode",
                    preservesExtendedHotLoopAndUnicode},
            {"PreservesMemoryIdentityAndZero", preservesMemoryIdentityAndZero},
            {"PreservesSameTimeDifferentColors",
                    preservesSameTimeDifferentColors},
            {"PreservesDuplicatePhysicalRecords",
                    preservesDuplicatePhysicalRecords},
            {"PreservesExplicitBeatTimingAndPhase",
                    preservesExplicitBeatTimingAndPhase},
            {"RejectsPartialSnapshot", rejectsPartialSnapshot},
            {"RefusesUnknownCueType", refusesUnknownCueType},
            {"ReportsUnknownStatus", reportsUnknownStatus},
            {"RejectsCountOverflow", rejectsCountOverflow},
            {"RejectsEntryCrossingSection", rejectsEntryCrossingSection},
            {"RejectsCommentCrossingEntry", rejectsCommentCrossingEntry},
            {"RejectsMalformedUnicode", rejectsMalformedUnicode},
            {"DistinguishesAbsentAndEmptyLabel",
                    distinguishesAbsentAndEmptyLabel},
            {"PreservesShortExtendedCueSafely",
                    preservesShortExtendedCueSafely},
            {"RefusesInconsistentSlot", refusesInconsistentSlot},
            {"RejectsNonpositiveLoop", rejectsNonpositiveLoop},
            {"RejectsUnorderedBeats", rejectsUnorderedBeats},
            {"RejectsExtraTrailingContent", rejectsExtraTrailingContent},
            {"SupportsEmptySections", supportsEmptySections},
            {"CancellationNeverPublishes", cancellationNeverPublishes},
            {"SourceBytesStayUnchanged", sourceBytesStayUnchanged},
            {"CheckedFrameConversion", checkedFrameConversion},
            {"UnknownSectionsAreExplicitlyOutsideCapability",
                    unknownSectionsAreExplicitlyOutsideCapability},
    };
    std::size_t passed = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            ++passed;
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << "Executed " << tests.size() << "; passed " << passed
              << "; failed " << tests.size() - passed << "; skipped 0\n";
    return passed == tests.size() ? 0 : 1;
}
// End of ChatGPT-prepared code for human review.
