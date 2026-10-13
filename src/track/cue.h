#pragma once

#include <QColor>
#include <QMutex>
#include <QObject>
#include <atomic>
#include <memory>
#include <type_traits> // static_assert

#include "audio/frame.h"
#include "audio/types.h"
#include "track/cueinfo.h"
#include "util/color/rgbcolor.h"
#include "util/db/dbid.h"

#ifdef BUILD_TESTING
class F001ProbeForTest;
#endif

class Cue : public QObject {
    Q_OBJECT

  public:
    /// A position value for the cue that signals its position is not set
    static constexpr double kNoPosition = -1.0;

    /// Invalid hot cue index
    static constexpr int kNoHotCue = -1;

    static_assert(kNoHotCue != mixxx::kFirstHotCueIndex,
            "Conflicting definitions of invalid and first hot cue index");

    struct StartAndEndPositions {
        mixxx::audio::FramePos startPosition;
        mixxx::audio::FramePos endPosition;
    };

    Cue() = delete;

    /// For roundtrips during tests
    Cue(
            const mixxx::CueInfo& cueInfo,
            mixxx::audio::SampleRate sampleRate,
            bool setDirty);

    /// Load entity from database.
    Cue(
            DbId id,
            mixxx::CueType type,
            mixxx::audio::FramePos position,
            mixxx::audio::FrameDiff_t length,
            int hotCue,
            const QString& label,
            mixxx::RgbColor color);

    /// Initialize new cue points
    Cue(
            mixxx::CueType type,
            int hotCueIndex,
            mixxx::audio::FramePos startPosition,
            mixxx::audio::FramePos endPosition,
            mixxx::RgbColor color);

    ~Cue() override = default;

    bool isDirty() const;
    DbId getId() const;

    mixxx::CueType getType() const;
    void setType(mixxx::CueType type);

    mixxx::audio::FramePos getPosition() const;
    void setStartPosition(mixxx::audio::FramePos position);
    void setEndPosition(mixxx::audio::FramePos position);
    void setStartAndEndPosition(
            mixxx::audio::FramePos startPosition,
            mixxx::audio::FramePos endPosition);
    void shiftPositionFrames(mixxx::audio::FrameDiff_t frameOffset);

    mixxx::audio::FrameDiff_t getLengthFrames() const;

    void setHotCue(int n);
    int getHotCue() const;

    QString getLabel() const;
    void setLabel(const QString& label);

    mixxx::RgbColor getColor() const;
    void setColor(mixxx::RgbColor color);

    mixxx::audio::FramePos getEndPosition() const;

    StartAndEndPositions getStartAndEndPosition() const;

#ifdef BUILD_TESTING
    void setF001ProbeForTest(F001ProbeForTest* pProbe) {
        m_pF001ProbeForTest.store(pProbe, std::memory_order_release);
    }
#endif

    mixxx::CueInfo getCueInfo(
            mixxx::audio::SampleRate sampleRate) const;

  signals:
    void updated();

  private:
    void setDirty(bool dirty);

    void setId(DbId dbId);

    mutable QMutex m_mutex;
#ifdef BUILD_TESTING
    std::atomic<F001ProbeForTest*> m_pF001ProbeForTest{nullptr};
#endif

    bool m_bDirty;
    DbId m_dbId;
    mixxx::CueType m_type;
    mixxx::audio::FramePos m_startPosition;
    mixxx::audio::FramePos m_endPosition;
    int m_iHotCue;
    QString m_label;
    mixxx::RgbColor m_color;

    friend class Track;
    friend class CueDAO;
};

static_assert(mixxx::audio::FramePos::kLegacyInvalidEnginePosition ==
                Cue::kNoPosition,
        "Invalid engine position value mismatch");

class CuePointer : public std::shared_ptr<Cue> {
  public:
    CuePointer() = default;
    explicit CuePointer(Cue* pCue)
            : std::shared_ptr<Cue>(pCue, deleteLater) {
    }

  private:
    static void deleteLater(Cue* pCue);
};
