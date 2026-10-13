#pragma once

#include "util/color/rgbcolor.h"

namespace mixxx::rekordbox {

// Indexed track/memory colors are raw RGB, unlike hot-cue qRgb values that
// include alpha. Check the literal before RgbColor can mask its high bits.
template<RgbColor::code_t Code>
constexpr RgbColor checkedIndexedColor() {
    static_assert(Code == RgbColor::validateCode(Code),
            "Rekordbox indexed colors must be 24-bit RGB");
    return RgbColor(Code);
}

} // namespace mixxx::rekordbox
