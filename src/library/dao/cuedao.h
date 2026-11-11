#pragma once

#include <optional>
#include <vector>

#include "library/dao/dao.h"
#include "track/cue.h"
#include "track/trackid.h"

#define CUE_TABLE "cues"

class Cue;

class CueDAO : public DAO {
  private:
    struct CueSnapshot {
        DbId id;
        mixxx::CueType type;
        mixxx::audio::FramePos start;
        mixxx::audio::FramePos end;
        int hotCue;
        QString label;
        mixxx::RgbColor color;
        bool dirty;
    };

  public:
    // Prepared by ChatGPT for human review.
    // SQL writes do not change Cue state until the owning transaction commits.
    class PendingSave {
      public:
        PendingSave() = default;
        PendingSave(PendingSave&&) = default;
        PendingSave& operator=(PendingSave&&) = default;
        PendingSave(const PendingSave&) = delete;
        PendingSave& operator=(const PendingSave&) = delete;

        // Call only after a successful commit. Concurrent edits remain dirty.
        bool finalize();

      private:
        struct SavedCue {
            CuePointer cue;
            CueSnapshot snapshot;
            DbId savedId;
        };
        std::vector<SavedCue> m_savedCues;
        friend class CueDAO;
    };
    // End of ChatGPT-prepared documentation for human review.

    ~CueDAO() override = default;

    QList<CuePointer> getCuesForTrack(TrackId trackId) const;

    // Prepared by ChatGPT for human review.
    // Owns a transaction. Failure leaves the database and all Cue state unchanged.
    [[nodiscard]] bool saveTrackCues(TrackId trackId, const QList<CuePointer>& cueList) const;

    // The caller owns the transaction and must roll back on failure. Finalize
    // the returned changes only after commit. No Cue object is mutated here.
    [[nodiscard]] std::optional<PendingSave> prepareTrackCues(
            TrackId trackId, const QList<CuePointer>& cueList) const;

    // Writes one cue without deleting others or mutating the Cue. Existing IDs
    // must belong to this track. The returned ID may be used for provenance in
    // the same transaction; its caller is responsible for commit or rollback.
    [[nodiscard]] std::optional<DbId> writeCue(TrackId trackId, const Cue& cue) const;
    // End of ChatGPT-prepared documentation for human review.
    bool deleteCuesForTrack(TrackId trackId) const;
    bool deleteCuesForTracks(const QList<TrackId>& trackIds) const;

  private:
    static std::optional<PendingSave> snapshotCues(const QList<CuePointer>& cueList);
    bool writeTrackCues(TrackId trackId, PendingSave* pending) const;
    static CueSnapshot snapshotCue(const Cue& cue);
    static bool finalizeCue(const PendingSave::SavedCue& saved);
    std::optional<DbId> writeCueSnapshot(TrackId trackId, const CueSnapshot& snapshot) const;
    friend class TrackDAO;
};
