#pragma once

#include <array>
#include <cstddef>

#include "util/color/rgbcolor.h"

namespace mixxx::rekordbox::detail {

// ID 2 historically used QColor(248, 0, 0) in Mixxx before the palette
// constants were added. This preserves Mixxx's #F80000 choice; it does not
// assert an official Rekordbox RGB triplet.
constexpr std::array<RgbColor::code_t, 8> kTrackColorCodes{
        0xF870F8,
        0xF80000,
        0xF8A030,
        0xF8E331,
        0x1EE000,
        0x16C0F8,
        0x0150F8,
        0x9808F8,
};

constexpr bool areTrackColorCodesValid() {
    for (const auto code : kTrackColorCodes) {
        if (code > 0x00FFFFFF) {
            return false;
        }
    }
    return true;
}

static_assert(areTrackColorCodesValid(), "Rekordbox track colors must be 24-bit RGB codes");

constexpr RgbColor colorFromID(int colorID) {
    if (colorID < 1 || colorID > static_cast<int>(kTrackColorCodes.size())) {
        return RgbColor(0);
    }
    return RgbColor(kTrackColorCodes[static_cast<std::size_t>(colorID - 1)]);
}

} // namespace mixxx::rekordbox::detail
