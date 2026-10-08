// AI-generated test API adaptation begins.
#pragma once

#include "waveform/visualplayposition.h"

namespace mixxx::test {

inline double playPositionAtNextVSync(
        VisualPlayPosition& position, VSyncTimeProvider* pVSyncTimeProvider) {
    double playPosition = 0.0;
    double slipPosition = 0.0;
    return position.getPlaySlipAtNextVSync(
                   pVSyncTimeProvider, &playPosition, &slipPosition)
            ? playPosition
            : -1.0;
}

} // namespace mixxx::test
// End AI-generated test API adaptation.
