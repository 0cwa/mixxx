#pragma once

namespace mixxx::library {

/// Policy for promoting an imported memory cue to the track's MainCue.
///
/// This value is read outside the engine thread and passed into import code.
/// Keeping the decision here avoids making import or engine code read
/// preferences directly.
struct MainCuePromotionPolicy {
    /// Preserve the existing behavior unless explicitly disabled.
    bool promoteFirstMemoryCue{true};

    static MainCuePromotionPolicy currentBehavior();
    static MainCuePromotionPolicy fromConfigValue(bool value);

    /// Return whether this candidate should become MainCue.
    ///
    /// Candidates are expected to be in chronological order. A candidate with
    /// an end position is a loop and must not be promoted.
    bool shouldPromote(bool mainCueAlreadyFound, bool candidateIsLoop) const;
};

} // namespace mixxx::library
