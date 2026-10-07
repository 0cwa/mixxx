#pragma once

#include <QtGlobal>

namespace mixxx {

// Read this opt-in only while constructing diagnostic owners. Never call it
// from an audio callback.
inline bool isAudioCallbackDiagnosticsEnabled() {
    return qgetenv("MIXXX_AUDIO_CALLBACK_DIAGNOSTICS") == "1";
}

} // namespace mixxx
