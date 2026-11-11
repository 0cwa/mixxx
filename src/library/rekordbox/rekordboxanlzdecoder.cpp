// Prepared by ChatGPT for human review.
#include "library/rekordbox/rekordboxanlzdecoder.h"

#include <kaitai/kaitaistream.h>
#include <rekordbox_anlz.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace mixxx::rekordbox {
namespace {

constexpr std::size_t kMaximumFileBytes = 64 * 1024 * 1024;

struct DecodeError : std::runtime_error {
    AnlzOutcome outcome;
    std::string code;

    DecodeError(AnlzOutcome result, std::string diagnosticCode, const std::string& message)
            : std::runtime_error(message),
              outcome(result),
              code(std::move(diagnosticCode)) {
    }
};

void require(bool valid, const std::string& code, const std::string& message) {
    if (!valid) {
        throw DecodeError(AnlzOutcome::Invalid, code, message);
    }
}

void requireSupported(bool supported, const std::string& code, const std::string& message) {
    if (!supported) {
        throw DecodeError(AnlzOutcome::Unsupported, code, message);
    }
}

void checkCancellation(const std::function<bool()>& cancelled) {
    if (cancelled && cancelled()) {
        throw DecodeError(AnlzOutcome::Cancelled, "cancelled", "ANLZ decoding was cancelled");
    }
}

std::uint32_t u32(const std::string& bytes, std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 4,
            "truncated",
            "Missing 32-bit field");
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    }
    return value;
}

std::uint16_t u16(const std::string& bytes, std::size_t offset) {
    require(offset <= bytes.size() && bytes.size() - offset >= 2,
            "truncated",
            "Missing 16-bit field");
    return static_cast<std::uint16_t>(
            (static_cast<unsigned char>(bytes[offset]) << 8) |
            static_cast<unsigned char>(bytes[offset + 1]));
}

void appendUtf8(std::string* output, std::uint32_t codePoint) {
    if (codePoint < 0x80) {
        output->push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        output->push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
        output->push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else if (codePoint < 0x10000) {
        output->push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
        output->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    } else {
        output->push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
        output->push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    }
}

std::string decodeUtf16Be(const std::string& bytes) {
    require(bytes.size() % 2 == 0, "invalid-utf16", "Cue label has an odd UTF-16 byte length");
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        std::uint32_t codePoint = u16(bytes, i);
        if (codePoint == 0 && i + 2 == bytes.size()) {
            break;
        }
        if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
            require(i + 4 <= bytes.size(), "invalid-utf16", "Unpaired high surrogate");
            const auto low = u16(bytes, i + 2);
            require(low >= 0xdc00 && low <= 0xdfff,
                    "invalid-utf16",
                    "Unpaired high surrogate");
            codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
            i += 2;
        } else {
            require(codePoint < 0xdc00 || codePoint > 0xdfff,
                    "invalid-utf16",
                    "Unpaired low surrogate");
        }
        appendUtf8(&result, codePoint);
    }
    return result;
}

AnlzCue makeCue(AnlzRecordKey key,
        const std::string& tag,
        std::uint32_t listKind,
        std::uint8_t sourceType,
        std::uint32_t hotNumber,
        std::uint32_t start,
        std::uint32_t end) {
    if (listKind > 1 || (sourceType != 1 && sourceType != 2)) {
        throw DecodeError(AnlzOutcome::Unsupported,
                "unsupported-cue-kind",
                "Unknown ANLZ cue list or entry type");
    }
    require((listKind == 0 && hotNumber == 0) || (listKind == 1 && hotNumber > 0),
            "inconsistent-hot-slot",
            "Cue list kind disagrees with its hot-cue number");
    require(sourceType != 2 || end > start,
            "invalid-loop",
            "Loop end must follow its start");
    AnlzCue cue{};
    cue.key = key;
    cue.sectionTag = tag;
    cue.bank = listKind == 0 ? AnlzCueBank::Memory : AnlzCueBank::Hot;
    cue.role = sourceType == 2 ? AnlzCueRole::Loop
                               : (listKind == 0 ? AnlzCueRole::MemoryCue : AnlzCueRole::HotCue);
    cue.sourceType = sourceType;
    if (hotNumber > 0) {
        cue.hotSlot = hotNumber - 1;
    }
    cue.start = {start};
    if (sourceType == 2) {
        cue.end = AnlzTimestamp{end};
    }
    return cue;
}

void decodeClassicCue(const std::string& bytes,
        AnlzRecordKey key,
        std::uint32_t listKind,
        DecodedAnlz* decoded,
        std::vector<AnlzDiagnostic>* diagnostics) {
    require(bytes.size() >= 56, "truncated-pcpt", "PCPT entry is shorter than its fixed fields");
    requireSupported(bytes.size() == 56,
            "unsupported-pcpt-size",
            "PCPT entry extension is not supported");
    auto cue = makeCue(key,
            "PCOB",
            listKind,
            static_cast<std::uint8_t>(bytes[28]),
            u32(bytes, 12),
            u32(bytes, 32),
            u32(bytes, 36));
    kaitai::kstream stream(bytes);
    rekordbox_anlz_t::cue_entry_t source(&stream);
    const auto status = u32(bytes, 16);
    cue.sourceStatus = status;
    if (status == 0 || status == 1 || status == 4) {
        cue.enabled = status != 0;
        cue.activeLoop = status == 4;
    } else {
        diagnostics->push_back({AnlzSeverity::Warning,
                "unknown-cue-status",
                "Cue status is preserved without inferring active-loop intent",
                key.sectionOrdinal,
                key.entryOrdinal});
    }
    cue.sourceOrderFirst = source.order_first();
    cue.sourceOrderLast = source.order_last();
    decoded->cues.push_back(std::move(cue));
}

void decodeExtendedCue(const std::string& bytes,
        AnlzRecordKey key,
        std::uint32_t listKind,
        DecodedAnlz* decoded) {
    require(bytes.size() >= 40, "truncated-pcp2", "PCP2 entry is shorter than its fixed fields");
    auto cue = makeCue(key,
            "PCO2",
            listKind,
            static_cast<std::uint8_t>(bytes[16]),
            u32(bytes, 12),
            u32(bytes, 20),
            u32(bytes, 24));
    cue.sourceColorId = static_cast<std::uint8_t>(bytes[28]);
    cue.loopNumerator = u16(bytes, 36);
    cue.loopDenominator = u16(bytes, 38);
    if (bytes.size() < 44) {
        requireSupported(bytes.size() == 40,
                "short-pcp2-profile",
                "PCP2 extension shorter than a comment-length field is "
                "unsupported");
        decoded->cues.push_back(std::move(cue));
        return;
    }
    const auto commentLength = u32(bytes, 40);
    require(commentLength <= bytes.size() - 44,
            "truncated-comment",
            "Cue comment exceeds its entry");
    const auto label = decodeUtf16Be(bytes.substr(44, commentLength));
    kaitai::kstream stream(bytes);
    rekordbox_anlz_t::cue_extended_entry_t source(&stream);
    cue.labelUtf8 = label;
    cue.sourceColorId = source.color_id();
    cue.loopNumerator = source.loop_numerator();
    cue.loopDenominator = source.loop_denominator();
    const auto tailLength = bytes.size() - 44 - commentLength;
    if (tailLength >= 1) {
        cue.sourceColorCode = source.color_code();
    }
    if (tailLength >= 4) {
        cue.rgb = AnlzRgb{source.color_red(), source.color_green(), source.color_blue()};
    }
    decoded->cues.push_back(std::move(cue));
}

} // namespace

AnlzDecodeResult decodeAnlz(const std::string& bytes,
        const std::function<bool()>& cancelled) {
    AnlzDecodeResult result{AnlzOutcome::Success, {}, {}};
    auto decoded = std::make_shared<DecodedAnlz>();
    std::optional<std::uint32_t> sectionOrdinal;
    std::optional<std::uint32_t> entryOrdinal;
    try {
        checkCancellation(cancelled);
        require(bytes.size() <= kMaximumFileBytes,
                "file-too-large",
                "ANLZ file exceeds the 64 MiB decoding limit");
        require(bytes.size() >= 12 && bytes.compare(0, 4, "PMAI") == 0,
                "invalid-header",
                "Expected a complete PMAI header");
        const auto headerLength = u32(bytes, 4);
        require(headerLength >= 12 && headerLength <= bytes.size(),
                "invalid-header-length",
                "ANLZ header length is outside the file");
        require(u32(bytes, 8) == bytes.size(),
                "invalid-file-length",
                "ANLZ declared file length does not match its bytes");
        std::size_t offset = headerLength;
        std::uint32_t sectionIndex = 0;
        while (offset < bytes.size()) {
            checkCancellation(cancelled);
            sectionOrdinal = sectionIndex;
            entryOrdinal.reset();
            require(bytes.size() - offset >= 12,
                    "truncated-section",
                    "ANLZ section header is incomplete");
            const auto tag = bytes.substr(offset, 4);
            const auto sectionHeaderLength = u32(bytes, offset + 4);
            const auto sectionLength = u32(bytes, offset + 8);
            require(sectionHeaderLength >= 12 && sectionLength >= sectionHeaderLength &&
                            sectionLength <= bytes.size() - offset,
                    "invalid-section-length",
                    "ANLZ section lengths exceed their enclosing file");
            const auto sectionEnd = offset + sectionLength;
            if (tag == "PQTZ") {
                require(sectionHeaderLength >= 24,
                        "truncated-pqtz-header",
                        "PQTZ header is incomplete");
                requireSupported(sectionHeaderLength == 24,
                        "unsupported-pqtz-header",
                        "PQTZ header must contain 24 bytes");
                const auto count = u32(bytes, offset + 20);
                require(count == (sectionLength - 24) / 8 && (sectionLength - 24) % 8 == 0,
                        "invalid-beat-count",
                        "PQTZ beat count disagrees with section length");
                std::optional<std::uint32_t> previousTime;
                for (std::uint32_t i = 0; i < count; ++i) {
                    checkCancellation(cancelled);
                    entryOrdinal = i;
                    kaitai::kstream stream(bytes.substr(offset + 24 + i * 8, 8));
                    rekordbox_anlz_t::beat_grid_beat_t source(&stream);
                    require(!previousTime || source.time() > *previousTime,
                            "unordered-beats",
                            "PQTZ timestamps must strictly increase within a grid");
                    require(source.beat_number() > 0 && source.tempo() > 0,
                            "invalid-beat",
                            "PQTZ beat phase and tempo must be positive");
                    decoded->beats.push_back({{sectionIndex, i},
                            {source.time()},
                            source.beat_number(),
                            source.tempo()});
                    previousTime = source.time();
                }
            } else if (tag == "PCOB" || tag == "PCO2") {
                const bool extended = tag == "PCO2";
                const std::size_t expectedHeader = extended ? 20 : 24;
                require(sectionHeaderLength >= expectedHeader,
                        "truncated-cue-header",
                        "Cue section header is incomplete");
                requireSupported(sectionHeaderLength == expectedHeader,
                        "unsupported-cue-header",
                        "Cue section has an unsupported header size");
                const auto listKind = u32(bytes, offset + 12);
                if (listKind > 1) {
                    throw DecodeError(AnlzOutcome::Unsupported,
                            "unsupported-cue-kind",
                            "Unknown ANLZ cue list type");
                }
                const auto count = u16(bytes, offset + (extended ? 16 : 18));
                decoded->cueLists.push_back({sectionIndex,
                        tag,
                        listKind == 0 ? AnlzCueBank::Memory : AnlzCueBank::Hot,
                        count});
                std::size_t entryOffset = offset + expectedHeader;
                for (std::uint32_t i = 0; i < count; ++i) {
                    checkCancellation(cancelled);
                    entryOrdinal = i;
                    require(sectionEnd - entryOffset >= 12,
                            "truncated-entry",
                            "Cue entry header is incomplete");
                    require(bytes.compare(entryOffset, 4, extended ? "PCP2" : "PCPT") == 0,
                            "invalid-entry-magic",
                            "Cue entry tag disagrees with its list");
                    const auto entryHeader = u32(bytes, entryOffset + 4);
                    const auto entryLength = u32(bytes, entryOffset + 8);
                    require(entryHeader >= 12 && entryLength >= entryHeader &&
                                    entryLength <= sectionEnd - entryOffset,
                            "invalid-entry-length",
                            "Cue entry exceeds its enclosing section");
                    const auto entry = bytes.substr(entryOffset, entryLength);
                    if (extended) {
                        decodeExtendedCue(entry, {sectionIndex, i}, listKind, decoded.get());
                    } else {
                        decodeClassicCue(entry,
                                {sectionIndex, i},
                                listKind,
                                decoded.get(),
                                &result.diagnostics);
                    }
                    entryOffset += entryLength;
                }
                require(entryOffset == sectionEnd,
                        "unconsumed-cue-data",
                        "Cue count leaves unexplained data in its section");
            } else {
                result.diagnostics.push_back({AnlzSeverity::Info,
                        "section-outside-capability",
                        "Section " + tag +
                                " is outside this decoder's cue and beat capabilities",
                        sectionIndex,
                        {}});
            }
            offset = sectionEnd;
            ++sectionIndex;
        }
        checkCancellation(cancelled);
        for (const auto& diagnostic : result.diagnostics) {
            if (diagnostic.severity == AnlzSeverity::Warning) {
                result.outcome = AnlzOutcome::Warning;
            }
        }
        result.snapshot = std::move(decoded);
    } catch (const DecodeError& error) {
        result.outcome = error.outcome;
        result.diagnostics.push_back({AnlzSeverity::Error,
                error.code,
                error.what(),
                sectionOrdinal,
                entryOrdinal});
    } catch (const std::exception& error) {
        result.outcome = AnlzOutcome::Failed;
        result.diagnostics.push_back({AnlzSeverity::Error,
                "decoder-failure",
                error.what(),
                sectionOrdinal,
                entryOrdinal});
    } catch (...) {
        result.outcome = AnlzOutcome::Failed;
        result.diagnostics.push_back({AnlzSeverity::Error,
                "decoder-failure",
                "Unknown ANLZ decoder failure",
                sectionOrdinal,
                entryOrdinal});
    }
    return result;
}

std::optional<std::int64_t> anlzTimestampToPcmFrames(
        AnlzTimestamp sourceTime, std::uint32_t sampleRateHz, std::int64_t correctionMillis) {
    constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
    const auto sourceMillis = static_cast<std::int64_t>(sourceTime.milliseconds);
    if (sampleRateHz == 0 || correctionMillis > sourceMillis ||
            (correctionMillis < 0 && sourceMillis > kMaximum + correctionMillis)) {
        return std::nullopt;
    }
    const auto correctedMillis = sourceMillis - correctionMillis;
    const auto wholeSeconds = correctedMillis / 1000;
    const auto remainder = correctedMillis % 1000;
    if (wholeSeconds > kMaximum / sampleRateHz) {
        return std::nullopt;
    }
    const auto wholeFrames = wholeSeconds * sampleRateHz;
    const auto fractionalFrames = (remainder * sampleRateHz + 500) / 1000;
    if (fractionalFrames > kMaximum - wholeFrames) {
        return std::nullopt;
    }
    return wholeFrames + fractionalFrames;
}

} // namespace mixxx::rekordbox
// End of ChatGPT-prepared code for human review.
