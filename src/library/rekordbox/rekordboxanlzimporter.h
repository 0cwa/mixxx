#pragma once

#include <QHash>
#include <QStringList>

#include "library/memorycuepromotionpolicy.h"
#include "library/rekordbox/rekordboxanlzdecoder.h"
#include "track/beats.h"
#include "track/cue.h"
#include "track/track_decl.h"

namespace mixxx::rekordbox {

struct AnlzAppliedCue {
    CuePointer cue;
    CueInfo snapshot;
    audio::SampleRate sampleRate;
};

struct AnlzSourceSnapshot {
    AnlzDecodeResult decoded;
    QByteArray fingerprint;
};

struct AnlzImportState {
    TrackWeakPointer track;
    QHash<QString, std::shared_ptr<AnlzAppliedCue>> cues;
    BeatsPointer beats;
    std::optional<CueInfo> promotedMainCue;
    audio::SampleRate promotedMainCueSampleRate;
    QByteArray fingerprint;
    audio::SampleRate sampleRate;
    double timingCorrectionMilliseconds{0.0};
    bool promoteFirstMemoryCue{false};
    AnlzSourceSnapshot decodedSource;
};

struct AnlzApplyResult {
    bool complete{true};
    QStringList diagnostics;
};

AnlzSourceSnapshot readAnlzSources(
        const QString& anlzPath, const AnlzSourceSnapshot* cached = nullptr);

// Prepared by ChatGPT.
// This compatibility adapter only updates cues created by this import state
// while their saved snapshot still matches. Durable provenance and conflict
// decisions belong to the shared import planner; a new state preserves occupied
// slots and existing annotations. Invoke outside the real-time audio callback.
// End of ChatGPT-generated documentation.
AnlzApplyResult applyAnlzTrackAnnotations(const TrackPointer& track,
        audio::SampleRate sampleRate,
        double timingCorrectionMilliseconds,
        const AnlzData& data,
        const library::MainCuePromotionPolicy& promotionPolicy,
        AnlzImportState* state);

// Prepared by ChatGPT.
// Read and fingerprint the available companions on every call. Preserve owned
// annotation snapshots through unavailable or invalid sources, and record an
// applied fingerprint only after all supported annotations were accepted.
// End of ChatGPT-generated documentation.
AnlzApplyResult importAnlzSources(const TrackPointer& track,
        audio::SampleRate sampleRate,
        double timingCorrectionMilliseconds,
        const QString& anlzPath,
        const library::MainCuePromotionPolicy& promotionPolicy,
        AnlzImportState* state);

RgbColor::optional_t anlzPaletteColor(std::uint8_t paletteId);

} // namespace mixxx::rekordbox
