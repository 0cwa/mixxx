// Prepared by ChatGPT for human review.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mixxx::rekordbox {

enum class AnlzOutcome {
    Success,
    Warning,
    Unsupported,
    Invalid,
    Denied,
    Unavailable,
    Cancelled,
    Failed,
};

enum class AnlzSeverity { Info,
    Warning,
    Error };
enum class AnlzCueBank { Memory,
    Hot };
enum class AnlzCueRole { MemoryCue,
    HotCue,
    Loop };

struct AnlzTimestamp {
    std::uint32_t milliseconds;
};

struct AnlzRecordKey {
    std::uint32_t sectionOrdinal;
    std::uint32_t entryOrdinal;
};

struct AnlzRgb {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

struct AnlzCue {
    AnlzRecordKey key;
    std::string sectionTag;
    AnlzCueBank bank;
    AnlzCueRole role;
    std::uint8_t sourceType;
    std::optional<std::uint32_t> hotSlot;
    AnlzTimestamp start;
    std::optional<AnlzTimestamp> end;
    std::optional<std::string> labelUtf8;
    std::optional<AnlzRgb> rgb;
    std::optional<std::uint8_t> sourceColorId;
    std::optional<std::uint8_t> sourceColorCode;
    std::optional<std::uint32_t> sourceStatus;
    std::optional<bool> enabled;
    std::optional<bool> activeLoop;
    std::optional<std::uint16_t> sourceOrderFirst;
    std::optional<std::uint16_t> sourceOrderLast;
    std::optional<std::uint16_t> loopNumerator;
    std::optional<std::uint16_t> loopDenominator;
};

struct AnlzBeat {
    AnlzRecordKey key;
    AnlzTimestamp time;
    std::uint16_t sourceBeatInBar;
    std::uint16_t sourceTempoHundredths;
};

struct AnlzCueList {
    std::uint32_t sectionOrdinal;
    std::string sectionTag;
    AnlzCueBank bank;
    std::uint32_t entryCount;
};

struct DecodedAnlz {
    std::vector<AnlzCue> cues;
    std::vector<AnlzBeat> beats;
    std::vector<AnlzCueList> cueLists;
};

struct AnlzDiagnostic {
    AnlzSeverity severity;
    std::string code;
    std::string message;
    std::optional<std::uint32_t> sectionOrdinal;
    std::optional<std::uint32_t> entryOrdinal;
};

struct AnlzDecodeResult {
    AnlzOutcome outcome;
    std::shared_ptr<const DecodedAnlz> snapshot;
    std::vector<AnlzDiagnostic> diagnostics;
};

AnlzDecodeResult decodeAnlz(
        const std::string& bytes,
        const std::function<bool()>& cancelled = {});

std::optional<std::int64_t> anlzTimestampToPcmFrames(
        AnlzTimestamp sourceTime,
        std::uint32_t sampleRateHz,
        std::int64_t correctionMillis = 0);

} // namespace mixxx::rekordbox
// End of ChatGPT-prepared code for human review.
