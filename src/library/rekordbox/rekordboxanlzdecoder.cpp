#include "library/rekordbox/rekordboxanlzdecoder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace mixxx::rekordbox {
namespace {

class DecodeError : public std::runtime_error {
  public:
    DecodeError(AnlzStatus status, std::size_t offset, const char* message)
            : std::runtime_error(message),
              status(status),
              offset(offset) {
    }
    AnlzStatus status;
    std::size_t offset;
};

class Bytes {
  public:
    explicit Bytes(std::string_view bytes, std::size_t offset = 0)
            : m_bytes(bytes),
              m_offset(offset) {
    }

    std::size_t size() const {
        return m_bytes.size();
    }

    std::size_t offset() const {
        return m_offset;
    }

    void require(std::size_t offset, std::size_t length) const {
        if (offset > size() || length > size() - offset) {
            throw DecodeError(AnlzStatus::Invalid,
                    m_offset + std::min(offset, size()),
                    "Truncated ANLZ field");
        }
    }

    std::uint8_t u8(std::size_t offset) const {
        require(offset, 1);
        return static_cast<std::uint8_t>(m_bytes[offset]);
    }

    std::uint16_t u16(std::size_t offset) const {
        require(offset, 2);
        return (static_cast<std::uint16_t>(u8(offset)) << 8) | u8(offset + 1);
    }

    std::uint32_t u32(std::size_t offset) const {
        require(offset, 4);
        return (static_cast<std::uint32_t>(u16(offset)) << 16) | u16(offset + 2);
    }

    std::string_view view(std::size_t offset, std::size_t length) const {
        require(offset, length);
        return m_bytes.substr(offset, length);
    }

    Bytes slice(std::size_t offset, std::size_t length) const {
        return Bytes(view(offset, length), m_offset + offset);
    }

  private:
    std::string_view m_bytes;
    std::size_t m_offset;
};

void checkCancelled(const AnlzDecodeOptions& options, std::size_t offset = 0) {
    if (options.isCancelled && options.isCancelled()) {
        throw DecodeError(AnlzStatus::Cancelled, offset, "ANLZ decoding cancelled");
    }
}

void appendUtf8(std::string* output, std::uint32_t codepoint) {
    if (codepoint < 0x80) {
        output->push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
        output->push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint < 0x10000) {
        output->push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else {
        output->push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string decodeUtf16(const Bytes& bytes, const AnlzDecodeOptions& options) {
    if (bytes.size() % 2 != 0 || bytes.size() > options.limits.maxStringBytes) {
        throw DecodeError(AnlzStatus::Invalid,
                bytes.offset(),
                "Invalid or excessive UTF-16 string length");
    }
    std::string result;
    for (std::size_t pos = 0; pos < bytes.size(); pos += 2) {
        auto codepoint = static_cast<std::uint32_t>(bytes.u16(pos));
        if (codepoint == 0 && pos + 2 == bytes.size()) {
            break;
        }
        if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
            if (bytes.size() - pos < 4) {
                throw DecodeError(AnlzStatus::Invalid,
                        bytes.offset() + pos,
                        "Incomplete UTF-16 surrogate pair");
            }
            const auto low = bytes.u16(pos + 2);
            if (low < 0xdc00 || low > 0xdfff) {
                throw DecodeError(AnlzStatus::Invalid,
                        bytes.offset() + pos,
                        "Invalid UTF-16 surrogate pair");
            }
            codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
            pos += 2;
        } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
            throw DecodeError(AnlzStatus::Invalid,
                    bytes.offset() + pos,
                    "Unpaired UTF-16 low surrogate");
        }
        appendUtf8(&result, codepoint);
    }
    return result;
}

void diagnose(std::vector<AnlzDiagnostic>* diagnostics,
        const AnlzInput& input,
        std::size_t offset,
        const char* message,
        AnlzStatus status = AnlzStatus::Warning) {
    diagnostics->push_back({status, input.sourceIdentity, offset, message});
}

bool isWaveform(std::string_view fourcc) {
    return fourcc == "PWAV" || fourcc == "PWV2" || fourcc == "PWV3" ||
            fourcc == "PWV4" || fourcc == "PWV5" || fourcc == "PWV6" ||
            fourcc == "PWV7";
}

bool decodeCues(const Bytes& section,
        bool extended,
        const AnlzInput& input,
        std::size_t sectionIndex,
        const AnlzDecodeOptions& options,
        AnlzData* data,
        std::vector<AnlzDiagnostic>* diagnostics) {
    const auto minimumHeader = extended ? 20u : 24u;
    section.require(0, minimumHeader);
    const auto listType = section.u32(12);
    if (listType > 1) {
        diagnose(diagnostics,
                input,
                section.offset(),
                "Unsupported cue list type",
                AnlzStatus::Unsupported);
        return false;
    }
    const auto count = section.u16(extended ? 16 : 18);
    const auto previousCount = data->cues.size();
    if (count > options.limits.maxCues - std::min(data->cues.size(), options.limits.maxCues)) {
        throw DecodeError(AnlzStatus::Invalid, section.offset(), "ANLZ cue limit exceeded");
    }
    std::size_t pos = section.u32(4);
    if (pos < minimumHeader) {
        throw DecodeError(AnlzStatus::Invalid,
                section.offset() + 4,
                "Invalid cue section header length");
    }
    std::set<std::uint32_t> hotSlots;
    for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
        checkCancelled(options, section.offset() + pos);
        section.require(pos, 12);
        const auto length = section.u32(pos + 8);
        const auto minimumEntry = extended ? 40u : 56u;
        if (length < minimumEntry) {
            throw DecodeError(AnlzStatus::Invalid,
                    section.offset() + pos,
                    "Invalid cue entry length");
        }
        const auto entry = section.slice(pos, length);
        if (entry.view(0, 4) != (extended ? "PCP2" : "PCPT") ||
                entry.u32(4) < 12 || entry.u32(4) > length) {
            throw DecodeError(AnlzStatus::Invalid, entry.offset(), "Invalid cue entry header");
        }
        if (entry.u32(4) != (extended ? 16u : 28u)) {
            diagnose(diagnostics,
                    input,
                    entry.offset(),
                    "Unsupported cue entry header layout",
                    AnlzStatus::Unsupported);
            pos += length;
            continue;
        }
        const auto number = entry.u32(12);
        if ((listType == 0 && number != 0) || (listType == 1 && number == 0)) {
            throw DecodeError(AnlzStatus::Invalid,
                    entry.offset() + 12,
                    "Cue list role and hot slot disagree");
        }
        const auto type = entry.u8(extended ? 16 : 28);
        if (type != 1 && type != 2) {
            diagnose(diagnostics,
                    input,
                    entry.offset(),
                    "Unsupported cue entry type",
                    AnlzStatus::Unsupported);
            pos += length;
            continue;
        }
        AnlzCue cue;
        cue.role = listType == 0
                ? (type == 1 ? AnlzCueRole::MemoryCue : AnlzCueRole::MemoryLoop)
                : (type == 1 ? AnlzCueRole::HotCue : AnlzCueRole::HotLoop);
        if (listType == 1) {
            cue.hotSlot = number - 1;
            if (!hotSlots.insert(*cue.hotSlot).second) {
                throw DecodeError(AnlzStatus::Invalid,
                        entry.offset(),
                        "Duplicate hot slot in one cue list");
            }
        }
        cue.startMilliseconds = entry.u32(extended ? 20 : 32);
        if (type == 2) {
            cue.endMilliseconds = entry.u32(extended ? 24 : 36);
            if (*cue.endMilliseconds <= cue.startMilliseconds) {
                throw DecodeError(AnlzStatus::Invalid,
                        entry.offset(),
                        "Loop end must follow its start");
            }
        }
        AnlzProvenance provenance{input.sourceIdentity,
                input.source,
                extended ? "PCO2" : "PCOB",
                sectionIndex,
                ordinal,
                std::nullopt,
                std::nullopt};
        if (extended) {
            if (listType == 0) {
                cue.paletteId = entry.u8(28);
            }
            cue.loopNumerator = entry.u16(36);
            cue.loopDenominator = entry.u16(38);
            if (length >= 44) {
                const auto commentLength = entry.u32(40);
                cue.labelUtf8 = decodeUtf16(entry.slice(44, commentLength), options);
                const auto colorOffset = 44 + static_cast<std::size_t>(commentLength);
                if (listType == 1 && colorOffset < entry.size()) {
                    cue.hotColorCode = entry.u8(colorOffset);
                }
                if (listType == 1 && entry.size() - colorOffset >= 4) {
                    cue.colorRgb = (static_cast<std::uint32_t>(entry.u8(colorOffset + 1)) << 16) |
                            (static_cast<std::uint32_t>(entry.u8(colorOffset + 2)) << 8) |
                            entry.u8(colorOffset + 3);
                }
            }
        } else {
            const auto status = entry.u32(16);
            if (status == 0 || status == 1 || status == 4) {
                cue.activeLoop = status == 4;
            } else {
                diagnose(diagnostics,
                        input,
                        entry.offset() + 16,
                        "Unknown active-loop status retained in source "
                        "section");
            }
            provenance.orderFirst = entry.u16(24);
            provenance.orderLast = entry.u16(26);
        }
        cue.provenance.push_back(std::move(provenance));
        data->cues.push_back(std::move(cue));
        pos += length;
    }
    return count == 0 || data->cues.size() != previousCount;
}

void decodeBeats(const Bytes& section,
        const AnlzInput& input,
        std::size_t sectionIndex,
        const AnlzDecodeOptions& options,
        AnlzData* data) {
    section.require(0, 24);
    const auto count = section.u32(20);
    const auto start = static_cast<std::size_t>(section.u32(4));
    if (start < 24 || count > options.limits.maxBeats ||
            count > (section.size() - start) / 8) {
        throw DecodeError(AnlzStatus::Invalid, section.offset(), "Invalid or excessive beat count");
    }
    if (data->beatProvenance) {
        throw DecodeError(AnlzStatus::Invalid, section.offset(), "Multiple beat grids in one file");
    }
    data->beats.reserve(count);
    for (std::size_t ordinal = 0; ordinal < count; ++ordinal) {
        checkCancelled(options, section.offset() + start + 8 * ordinal);
        const auto pos = start + 8 * ordinal;
        AnlzBeat beat{section.u32(pos + 4), section.u16(pos + 2), section.u16(pos)};
        if (!data->beats.empty() && data->beats.back().timeMilliseconds >= beat.timeMilliseconds) {
            throw DecodeError(AnlzStatus::Invalid,
                    section.offset() + pos,
                    "Beat timestamps must be strictly increasing");
        }
        data->beats.push_back(beat);
    }
    data->beatProvenance = AnlzProvenance{input.sourceIdentity,
            input.source,
            "PQTZ",
            sectionIndex,
            0,
            std::nullopt,
            std::nullopt};
}

int cuePriority(const AnlzCue& cue) {
    if (cue.provenance.empty()) {
        return 0;
    }
    const auto& provenance = cue.provenance.front();
    return (provenance.section == "PCO2" ? 10 : 0) + static_cast<int>(provenance.source);
}

bool sameInterval(const AnlzCue& left, const AnlzCue& right) {
    return left.role == right.role && left.startMilliseconds == right.startMilliseconds &&
            left.endMilliseconds == right.endMilliseconds;
}

bool sameSection(const AnlzCue& left, const AnlzCue& right) {
    for (const auto& a : left.provenance) {
        for (const auto& b : right.provenance) {
            if (a.sourceIdentity == b.sourceIdentity && a.source == b.source &&
                    a.section == b.section && a.sectionIndex == b.sectionIndex) {
                return true;
            }
        }
    }
    return false;
}

template<typename T>
void fillMissing(std::optional<T>* destination, const std::optional<T>& source) {
    if (!*destination) {
        *destination = source;
    }
}

void mergeCue(AnlzCue* destination, AnlzCue source) {
    if (cuePriority(source) > cuePriority(*destination)) {
        std::swap(*destination, source);
    }
    if (sameInterval(*destination, source)) {
        fillMissing(&destination->activeLoop, source.activeLoop);
        fillMissing(&destination->labelUtf8, source.labelUtf8);
        fillMissing(&destination->colorRgb, source.colorRgb);
        fillMissing(&destination->paletteId, source.paletteId);
        fillMissing(&destination->hotColorCode, source.hotColorCode);
        fillMissing(&destination->loopNumerator, source.loopNumerator);
        fillMissing(&destination->loopDenominator, source.loopDenominator);
    }
    destination->provenance.insert(destination->provenance.end(),
            source.provenance.begin(),
            source.provenance.end());
}

void normalizeCues(AnlzData* data,
        const AnlzDecodeOptions& options,
        std::vector<AnlzDiagnostic>* diagnostics) {
    std::vector<AnlzCue> merged;
    merged.reserve(data->cues.size());
    using MemoryKey = std::tuple<AnlzCueRole,
            std::uint32_t,
            std::optional<std::uint32_t>,
            std::size_t>;
    std::map<MemoryKey, std::vector<std::size_t>> memoryCandidates;
    std::map<std::uint32_t, std::vector<std::size_t>> hotCandidates;
    for (auto& cue : data->cues) {
        checkCancelled(options);
        const auto memoryKey = MemoryKey(cue.role,
                cue.startMilliseconds,
                cue.endMilliseconds,
                cue.provenance.front().ordinal);
        auto& candidates = cue.hotSlot ? hotCandidates[*cue.hotSlot] : memoryCandidates[memoryKey];
        auto match = merged.size();
        bool ambiguous = false;
        for (const auto index : candidates) {
            const auto& existing = merged[index];
            if (sameSection(existing, cue)) {
                continue;
            }
            if (!cue.hotSlot &&
                    ((existing.labelUtf8 && cue.labelUtf8 &&
                             existing.labelUtf8 != cue.labelUtf8) ||
                            (existing.colorRgb && cue.colorRgb &&
                                    existing.colorRgb != cue.colorRgb) ||
                            (existing.paletteId && cue.paletteId &&
                                    existing.paletteId != cue.paletteId))) {
                ambiguous = true;
                continue;
            }
            if (match != merged.size()) {
                ambiguous = true;
                match = merged.size();
                break;
            }
            match = index;
        }
        if (ambiguous) {
            diagnostics->push_back({AnlzStatus::Warning,
                    cue.provenance.front().sourceIdentity,
                    0,
                    "Ambiguous memory-cue identity; distinct annotations "
                    "retained"});
        }
        if (match == merged.size()) {
            candidates.push_back(merged.size());
            merged.push_back(std::move(cue));
        } else {
            mergeCue(&merged[match], std::move(cue));
        }
    }
    std::stable_sort(merged.begin(), merged.end(), anlzCueLess);
    data->cues = std::move(merged);
}

int failurePriority(AnlzStatus status) {
    switch (status) {
    case AnlzStatus::Failed:
        return 5;
    case AnlzStatus::Denied:
        return 4;
    case AnlzStatus::Invalid:
        return 3;
    case AnlzStatus::Unsupported:
        return 2;
    case AnlzStatus::Unavailable:
        return 1;
    default:
        return 0;
    }
}

} // namespace

bool anlzCueLess(const AnlzCue& left, const AnlzCue& right) {
    const auto leftKey = std::tie(left.startMilliseconds,
            left.endMilliseconds,
            left.role,
            left.hotSlot,
            left.labelUtf8,
            left.colorRgb,
            left.paletteId,
            left.activeLoop);
    const auto rightKey = std::tie(right.startMilliseconds,
            right.endMilliseconds,
            right.role,
            right.hotSlot,
            right.labelUtf8,
            right.colorRgb,
            right.paletteId,
            right.activeLoop);
    if (leftKey != rightKey) {
        return leftKey < rightKey;
    }
    if (left.provenance.empty() || right.provenance.empty()) {
        return left.provenance.empty() && !right.provenance.empty();
    }
    const auto& a = left.provenance.front();
    const auto& b = right.provenance.front();
    return std::tie(a.source, a.sourceIdentity, a.section, a.sectionIndex, a.ordinal) <
            std::tie(b.source, b.sourceIdentity, b.section, b.sectionIndex, b.ordinal);
}

double anlzMillisecondsToFrames(
        std::int64_t milliseconds, double sampleRate, double correctionMilliseconds) {
    if (!std::isfinite(sampleRate) || sampleRate <= 0 ||
            !std::isfinite(correctionMilliseconds)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto frames =
            (static_cast<double>(milliseconds) - correctionMilliseconds) * (sampleRate / 1000.0);
    return std::isfinite(frames) ? frames : std::numeric_limits<double>::quiet_NaN();
}

AnlzDecodeResult decodeAnlz(const AnlzInput& input, const AnlzDecodeOptions& options) {
    AnlzDecodeResult result;
    try {
        checkCancelled(options);
        if (input.availability != AnlzStatus::Success) {
            result.status = input.availability;
            diagnose(&result.diagnostics,
                    input,
                    0,
                    "ANLZ input is not available",
                    input.availability);
            return result;
        }
        if (input.bytes.size() > options.limits.maxFileBytes) {
            throw DecodeError(AnlzStatus::Invalid, 0, "ANLZ file size limit exceeded");
        }
        const Bytes file(input.bytes);
        file.require(0, 12);
        if (file.view(0, 4) != "PMAI") {
            throw DecodeError(AnlzStatus::Invalid, 0, "Missing PMAI file signature");
        }
        std::size_t pos = file.u32(4);
        if (pos < 12 || pos > file.size() || file.u32(8) != file.size()) {
            throw DecodeError(AnlzStatus::Invalid, 4, "Invalid ANLZ file length");
        }
        auto data = std::make_shared<AnlzData>();
        std::size_t sectionIndex = 0;
        std::size_t preservedBytes = 0;
        if (options.preserveRawSections) {
            if (pos > options.limits.maxPreservedBytes) {
                throw DecodeError(AnlzStatus::Invalid, 0, "ANLZ preservation limit exceeded");
            }
            preservedBytes = pos;
            data->fileHeaders.push_back({input.source,
                    input.sourceIdentity,
                    std::string(file.view(0, pos))});
        }
        bool recognized = false;
        while (pos < file.size()) {
            checkCancelled(options, pos);
            if (sectionIndex >= options.limits.maxSections) {
                throw DecodeError(AnlzStatus::Invalid, pos, "ANLZ section limit exceeded");
            }
            file.require(pos, 12);
            const auto headerLength = file.u32(pos + 4);
            const auto sectionLength = file.u32(pos + 8);
            if (headerLength < 12 || sectionLength < headerLength) {
                throw DecodeError(AnlzStatus::Invalid, pos, "Invalid ANLZ section length");
            }
            const auto section = file.slice(pos, sectionLength);
            const auto fourcc = section.view(0, 4);
            auto capability = AnlzSectionCapability::Decoded;
            if (fourcc == "PCOB" || fourcc == "PCO2") {
                const bool supported = decodeCues(section,
                        fourcc == "PCO2",
                        input,
                        sectionIndex,
                        options,
                        data.get(),
                        &result.diagnostics);
                recognized = recognized || supported;
                if (!supported) {
                    capability = AnlzSectionCapability::Unsupported;
                }
            } else if (fourcc == "PQTZ") {
                decodeBeats(section, input, sectionIndex, options, data.get());
                recognized = true;
            } else if (fourcc == "PPTH") {
                section.require(0, 16);
                data->mediaPathUtf8 = decodeUtf16(section.slice(16, section.u32(12)), options);
                recognized = true;
            } else {
                const bool waveform = isWaveform(fourcc);
                capability = waveform ? AnlzSectionCapability::RegenerateWaveform
                                      : AnlzSectionCapability::Unsupported;
                recognized = recognized || waveform || fourcc == "PSSI" || fourcc == "PVBR";
                diagnose(&result.diagnostics,
                        input,
                        pos,
                        waveform ? "Waveform reuse unavailable; regenerate "
                                   "from media"
                                 : "Unsupported ANLZ section has no "
                                   "application adapter",
                        AnlzStatus::Unsupported);
            }
            if (options.preserveRawSections) {
                if (sectionLength > options.limits.maxPreservedBytes -
                                std::min(preservedBytes, options.limits.maxPreservedBytes)) {
                    throw DecodeError(AnlzStatus::Invalid, pos, "ANLZ preservation limit exceeded");
                }
                preservedBytes += sectionLength;
                data->rawSections.push_back({{input.sourceIdentity,
                                                     input.source,
                                                     std::string(fourcc),
                                                     sectionIndex,
                                                     0,
                                                     std::nullopt,
                                                     std::nullopt},
                        capability,
                        std::string(section.view(0, section.size()))});
            }
            pos += sectionLength;
            ++sectionIndex;
        }
        normalizeCues(data.get(), options, &result.diagnostics);
        result.status = result.diagnostics.empty() ? AnlzStatus::Success
                : recognized                       ? AnlzStatus::Warning
                                                   : AnlzStatus::Unsupported;
        result.data = std::move(data);
    } catch (const DecodeError& error) {
        result.status = error.status;
        diagnose(&result.diagnostics, input, error.offset, error.what(), error.status);
    } catch (const std::exception&) {
        result.status = AnlzStatus::Failed;
        diagnose(&result.diagnostics, input, 0, "ANLZ decoder failed", AnlzStatus::Failed);
    } catch (...) {
        result.status = AnlzStatus::Failed;
        diagnose(&result.diagnostics, input, 0, "ANLZ decoder failed", AnlzStatus::Failed);
    }
    return result;
}

AnlzDecodeResult decodeAnlzCompanions(
        const std::vector<AnlzInput>& inputs, const AnlzDecodeOptions& options) {
    AnlzDecodeResult result;
    if (inputs.empty()) {
        result.status = AnlzStatus::Unavailable;
        return result;
    }
    if (inputs.size() > 3) {
        result.status = AnlzStatus::Invalid;
        result.diagnostics.push_back({AnlzStatus::Invalid,
                {},
                0,
                "At most DAT, EXT and 2EX companions may be decoded together"});
        return result;
    }
    try {
        auto data = std::make_shared<AnlzData>();
        auto orderedInputs = inputs;
        std::stable_sort(orderedInputs.begin(),
                orderedInputs.end(),
                [](const AnlzInput& a, const AnlzInput& b) {
                    return std::tie(a.source, a.sourceIdentity) <
                            std::tie(b.source, b.sourceIdentity);
                });
        for (std::size_t index = 1; index < orderedInputs.size(); ++index) {
            if (orderedInputs[index - 1].source == orderedInputs[index].source) {
                throw DecodeError(AnlzStatus::Invalid,
                        0,
                        "Duplicate ANLZ companion type is ambiguous");
            }
        }
        bool anyUsable = false;
        bool anyWarning = false;
        result.status = AnlzStatus::Unavailable;
        std::size_t preservedBytes = 0;
        for (const auto& input : orderedInputs) {
            auto decoded = decodeAnlz(input, options);
            result.diagnostics.insert(result.diagnostics.end(),
                    decoded.diagnostics.begin(),
                    decoded.diagnostics.end());
            if (decoded.status == AnlzStatus::Cancelled || decoded.status == AnlzStatus::Failed) {
                result.status = decoded.status;
                return result;
            }
            if (!decoded.usable()) {
                if (failurePriority(decoded.status) > failurePriority(result.status)) {
                    result.status = decoded.status;
                }
                anyWarning = true;
            } else {
                anyUsable = true;
                anyWarning = anyWarning || decoded.status != AnlzStatus::Success;
                if (decoded.data->cues.size() > options.limits.maxCues -
                                std::min(data->cues.size(), options.limits.maxCues)) {
                    throw DecodeError(AnlzStatus::Invalid, 0, "Combined ANLZ cue limit exceeded");
                }
                data->cues.insert(data->cues.end(),
                        decoded.data->cues.begin(),
                        decoded.data->cues.end());
                if (!data->beatProvenance && decoded.data->beatProvenance) {
                    data->beats = decoded.data->beats;
                    data->beatProvenance = decoded.data->beatProvenance;
                }
                if (!data->mediaPathUtf8 && decoded.data->mediaPathUtf8) {
                    data->mediaPathUtf8 = decoded.data->mediaPathUtf8;
                }
            }
            if (!decoded.data) {
                continue;
            }
            for (const auto& header : decoded.data->fileHeaders) {
                if (header.bytes.size() > options.limits.maxPreservedBytes -
                                std::min(preservedBytes, options.limits.maxPreservedBytes)) {
                    throw DecodeError(AnlzStatus::Invalid,
                            0,
                            "Combined ANLZ preservation limit exceeded");
                }
                preservedBytes += header.bytes.size();
                data->fileHeaders.push_back(header);
            }
            for (const auto& section : decoded.data->rawSections) {
                if (section.bytes.size() > options.limits.maxPreservedBytes -
                                std::min(preservedBytes, options.limits.maxPreservedBytes)) {
                    throw DecodeError(AnlzStatus::Invalid,
                            0,
                            "Combined ANLZ preservation limit exceeded");
                }
                preservedBytes += section.bytes.size();
                data->rawSections.push_back(section);
            }
        }
        if (anyUsable) {
            normalizeCues(data.get(), options, &result.diagnostics);
            result.status = anyWarning || !result.diagnostics.empty() ? AnlzStatus::Warning
                                                                      : AnlzStatus::Success;
            result.data = std::move(data);
        } else if (!data->rawSections.empty() || !data->fileHeaders.empty()) {
            result.data = std::move(data);
        }
    } catch (const DecodeError& error) {
        result.status = error.status;
        result.diagnostics.push_back({error.status, {}, error.offset, error.what()});
    } catch (...) {
        result.status = AnlzStatus::Failed;
        result.diagnostics.push_back({AnlzStatus::Failed, {}, 0, "ANLZ companion decoding failed"});
    }
    return result;
}

} // namespace mixxx::rekordbox
