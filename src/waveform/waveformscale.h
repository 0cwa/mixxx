#pragma once

#include <algorithm>
#include <cmath>

namespace mixxx {
namespace waveform {

inline constexpr int kDefaultVisualSampleRate = 441;

inline double getDefaultAudioVisualRatio(double audioSampleRate) noexcept {
    if (!std::isfinite(audioSampleRate) || audioSampleRate <= 0.0) {
        return 0.0;
    }
    return audioSampleRate /
            std::min(audioSampleRate, static_cast<double>(kDefaultVisualSampleRate));
}

inline double getAudioSamplePerPixel(
        double visualSamplePerPixel, double audioVisualRatio) noexcept {
    if (!std::isfinite(visualSamplePerPixel) || visualSamplePerPixel <= 0.0 ||
            !std::isfinite(audioVisualRatio) || audioVisualRatio <= 0.0) {
        return 0.0;
    }
    const double audioSamplePerPixel = visualSamplePerPixel * audioVisualRatio;
    return std::isfinite(audioSamplePerPixel) && audioSamplePerPixel > 0.0
            ? audioSamplePerPixel
            : 0.0;
}

} // namespace waveform
} // namespace mixxx
