#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "library/rekordbox/rekordboxanlzdecoder.h"

// Prepared by ChatGPT.
// Synthetic complete ANLZ fixtures follow the checked-in rekordbox_anlz.ksy.
// They are parser regression inputs, not independently exported rekordbox data.
// End of ChatGPT-generated documentation.
namespace mixxx::rekordbox::test {

inline void setU16(std::string* bytes, std::size_t offset, std::uint16_t value) {
    bytes->at(offset) = static_cast<char>(value >> 8);
    bytes->at(offset + 1) = static_cast<char>(value);
}

inline void setU32(std::string* bytes, std::size_t offset, std::uint32_t value) {
    setU16(bytes, offset, static_cast<std::uint16_t>(value >> 16));
    setU16(bytes, offset + 2, static_cast<std::uint16_t>(value));
}

inline std::string section(const char* tag, std::uint32_t headerLength, const std::string& body) {
    std::string result(tag, 4);
    result.resize(12);
    result += body;
    setU32(&result, 4, headerLength);
    setU32(&result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

inline std::string file(const std::vector<std::string>& sections) {
    std::string result("PMAI");
    result.resize(28);
    setU32(&result, 4, 28);
    for (const auto& value : sections) {
        result += value;
    }
    setU32(&result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

inline std::string legacyCue(std::uint32_t number = 1,
        std::uint8_t type = 2,
        std::uint32_t start = 1000,
        std::uint32_t end = 2000,
        std::uint32_t status = 4) {
    std::string result("PCPT");
    result.resize(56);
    setU32(&result, 4, 28);
    setU32(&result, 8, 56);
    setU32(&result, 12, number);
    setU32(&result, 16, status);
    setU32(&result, 20, 0x10000);
    setU16(&result, 24, 0xffff);
    setU16(&result, 26, 0xffff);
    result[28] = static_cast<char>(type);
    setU16(&result, 30, 1000);
    setU32(&result, 32, start);
    setU32(&result, 36, end);
    return result;
}

inline std::string extendedCue(std::uint32_t number = 1,
        std::uint8_t type = 2,
        std::uint32_t start = 1000,
        std::uint32_t end = 2000,
        std::optional<std::u16string> label = std::u16string(u"Loop", 4),
        std::optional<std::uint32_t> color = 0x123456,
        std::uint8_t palette = 0) {
    std::string result("PCP2");
    result.resize(40);
    setU32(&result, 4, 16);
    setU32(&result, 12, number);
    result[16] = static_cast<char>(type);
    setU16(&result, 18, 1000);
    setU32(&result, 20, start);
    setU32(&result, 24, end);
    result[28] = static_cast<char>(palette);
    if (label) {
        const auto length = label->empty() ? 0 : (label->size() + 1) * 2;
        result.resize(44 + length);
        setU32(&result, 40, static_cast<std::uint32_t>(length));
        for (std::size_t index = 0; index < label->size(); ++index) {
            setU16(&result, 44 + 2 * index, (*label)[index]);
        }
        if (color) {
            result.push_back(1);
            result.push_back(static_cast<char>(*color >> 16));
            result.push_back(static_cast<char>(*color >> 8));
            result.push_back(static_cast<char>(*color));
        }
    }
    setU32(&result, 8, static_cast<std::uint32_t>(result.size()));
    return result;
}

inline std::string cueList(bool extended,
        std::uint32_t type,
        const std::vector<std::string>& cues) {
    std::string body(extended ? 8 : 12, '\0');
    setU32(&body, 0, type);
    setU16(&body, extended ? 4 : 6, static_cast<std::uint16_t>(cues.size()));
    for (const auto& cue : cues) {
        body += cue;
    }
    return section(extended ? "PCO2" : "PCOB", extended ? 20 : 24, body);
}

inline std::string beatGrid(const std::vector<AnlzBeat>& beats) {
    std::string body(12 + 8 * beats.size(), '\0');
    setU32(&body, 4, 0x80000);
    setU32(&body, 8, static_cast<std::uint32_t>(beats.size()));
    for (std::size_t index = 0; index < beats.size(); ++index) {
        setU16(&body, 12 + 8 * index, beats[index].beatInBar);
        setU16(&body, 14 + 8 * index, beats[index].tempoHundredths);
        setU32(&body, 16 + 8 * index, beats[index].timeMilliseconds);
    }
    return section("PQTZ", 24, body);
}

} // namespace mixxx::rekordbox::test
