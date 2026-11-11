#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mixxx::rekordbox {

enum class AnlzStatus {
    Success,
    Warning,
    Unsupported,
    Invalid,
    Unavailable,
    Denied,
    Cancelled,
    Failed,
};

enum class AnlzSource { Dat,
    Ext,
    TwoEx };
enum class AnlzCueRole { MemoryCue,
    MemoryLoop,
    HotCue,
    HotLoop };
enum class AnlzSectionCapability { Decoded,
    RegenerateWaveform,
    Unsupported };

struct AnlzProvenance {
    std::string sourceIdentity;
    AnlzSource source{AnlzSource::Dat};
    std::string section;
    std::size_t sectionIndex{0};
    std::size_t ordinal{0};
    std::optional<std::uint16_t> orderFirst;
    std::optional<std::uint16_t> orderLast;
};

struct AnlzCue {
    AnlzCueRole role{AnlzCueRole::MemoryCue};
    std::optional<std::uint32_t> hotSlot;
    std::uint32_t startMilliseconds{0};
    std::optional<std::uint32_t> endMilliseconds;
    std::optional<std::string> labelUtf8;
    std::optional<std::uint32_t> colorRgb;
    std::optional<std::uint8_t> paletteId;
    std::optional<std::uint8_t> hotColorCode;
    std::optional<bool> activeLoop;
    std::optional<std::uint16_t> loopNumerator;
    std::optional<std::uint16_t> loopDenominator;
    std::vector<AnlzProvenance> provenance;
};

struct AnlzBeat {
    std::uint32_t timeMilliseconds{0};
    std::uint16_t tempoHundredths{0};
    std::uint16_t beatInBar{0};
};

struct AnlzRawSection {
    AnlzProvenance provenance;
    AnlzSectionCapability capability{AnlzSectionCapability::Unsupported};
    std::string bytes;
};

struct AnlzRawFileHeader {
    AnlzSource source{AnlzSource::Dat};
    std::string sourceIdentity;
    std::string bytes;
};

struct AnlzData {
    std::vector<AnlzCue> cues;
    std::vector<AnlzBeat> beats;
    std::optional<AnlzProvenance> beatProvenance;
    std::optional<std::string> mediaPathUtf8;
    std::vector<AnlzRawFileHeader> fileHeaders;
    std::vector<AnlzRawSection> rawSections;
};

struct AnlzDiagnostic {
    AnlzStatus status{AnlzStatus::Warning};
    std::string sourceIdentity;
    std::size_t byteOffset{0};
    std::string message;
};

struct AnlzInput {
    AnlzSource source{AnlzSource::Dat};
    std::string sourceIdentity;
    std::string_view bytes;
    AnlzStatus availability{AnlzStatus::Success};
};

struct AnlzDecodeLimits {
    std::size_t maxFileBytes{64 * 1024 * 1024};
    std::size_t maxSections{4096};
    std::size_t maxCues{65536};
    std::size_t maxBeats{1000000};
    std::size_t maxStringBytes{65536};
    std::size_t maxPreservedBytes{64 * 1024 * 1024};
};

struct AnlzDecodeOptions {
    AnlzDecodeLimits limits;
    std::function<bool()> isCancelled;
    bool preserveRawSections{true};
};

struct AnlzDecodeResult {
    AnlzStatus status{AnlzStatus::Failed};
    std::shared_ptr<const AnlzData> data;
    std::vector<AnlzDiagnostic> diagnostics;

    bool usable() const {
        return data && (status == AnlzStatus::Success || status == AnlzStatus::Warning);
    }
};

// Prepared by ChatGPT.
// Inputs are bounded byte views owned by the caller, with uncorrected integer
// milliseconds. Decode results own their data and never apply Track/UI changes.
// The application fingerprints the exact inputs before recording applied generations.
// End of ChatGPT-generated documentation.
AnlzDecodeResult decodeAnlz(
        const AnlzInput& input, const AnlzDecodeOptions& options = {});
AnlzDecodeResult decodeAnlzCompanions(
        const std::vector<AnlzInput>& inputs, const AnlzDecodeOptions& options = {});

bool anlzCueLess(const AnlzCue& left, const AnlzCue& right);
double anlzMillisecondsToFrames(std::int64_t milliseconds,
        double sampleRate,
        double correctionMilliseconds = 0.0);

} // namespace mixxx::rekordbox
