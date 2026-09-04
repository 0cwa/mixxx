// Deterministic end-to-end alignment coverage for the real EngineBuffer path.
//
// The reader is synchronous and serves a preloaded source. The test therefore
// exercises ReadAheadManager, the normal scaler, EngineBuffer::m_playPos,
// VisualPlayPosition, and the production waveform coordinate transform without
// depending on a reader worker or wall-clock readiness.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

#include "control/controlobject.h"
#ifdef __BUNGEE__
#include "engine/bufferscalers/enginebufferscalebungee.h"
#endif
#ifdef __SIGNALSMITH__
#include "engine/bufferscalers/enginebufferscalesignalsmith.h"
#endif
#include "engine/bufferscalers/enginebufferscalelinear.h"
#include "engine/bufferscalers/enginebufferscalest.h"
#include "engine/cachingreader/cachingreader.h"
#include "engine/controls/enginecontrol.h"
#include "engine/engine.h"
#include "engine/enginebuffer.h"
#include "engine/readaheadmanager.h"
#include "test/signalpathtest.h"
#include "track/track.h"
#include "util/performancetimer.h"
#include "util/sample.h"
#include "waveform/isynctimeprovider.h"
#include "waveform/renderers/waveformwidgetrenderer.h"
#include "waveform/visualplayposition.h"

namespace {

constexpr int kChannels = 2;
constexpr int kSampleRate = 48000;
constexpr int kTrackSeconds = 10;
constexpr int kTrackFrames = kSampleRate * kTrackSeconds;
constexpr int kBufferFrames = 960;
constexpr int kBufferSamples = kBufferFrames * kChannels;
constexpr int kVSyncOffsetMicros = 5000;
constexpr int kVSyncOffsetFrames = 240; // 5000 us at 48 kHz
constexpr int kMarkerFrames = 16;
constexpr int kMarkerSourceFrame =
        kBufferFrames + kVSyncOffsetFrames + 16 * kBufferFrames;
constexpr int kEngineMarkerFrames = 128;
constexpr int kEngineMarkerSourceFrame = 15 * kBufferFrames + 400;
// The impulse is intentionally detected on onset, not peak: the two engines
// have different smoothing kernels, while the first threshold crossing is a
// stable emitted-output clock edge in the deterministic source.
#ifdef __SIGNALSMITH__
constexpr double kSignalSmithMarkerOnsetThreshold = 0.3;
#endif
#ifdef __BUNGEE__
constexpr double kBungeeMarkerOnsetThreshold = 0.8;
#endif
constexpr int kMarkerNeighbourFrames = 8;
constexpr int kRendererWidth = 1000;
const QString kTracePath =
        QDir(QDir::tempPath())
                .filePath(QStringLiteral("mixxx-enginebuffer-alignment.trace"));
const QString kEngineMarkerTracePath =
        QDir(QDir::tempPath())
                .filePath(
                        QStringLiteral("mixxx-engine-marker-alignment.trace"));

static_assert(kMarkerSourceFrame + kMarkerFrames < kTrackFrames);
static_assert(kEngineMarkerSourceFrame + kEngineMarkerFrames < kTrackFrames);

const std::array<CSAMPLE, kMarkerFrames * kChannels> kMarkerCode = {
        -0.731f,
        0.419f,
        0.913f,
        -0.277f,
        -0.151f,
        -0.887f,
        0.643f,
        0.752f,
        -0.962f,
        0.083f,
        0.307f,
        -0.538f,
        0.844f,
        0.194f,
        -0.406f,
        0.971f,
        0.118f,
        -0.674f,
        -0.819f,
        0.365f,
        0.556f,
        -0.923f,
        0.278f,
        0.607f,
        -0.489f,
        -0.126f,
        0.786f,
        -0.348f,
        -0.052f,
        0.895f,
        0.472f,
        -0.764f,
};

CSAMPLE engineMarkerSample(int markerFrame, int channel) {
    if (markerFrame == 0) {
        return channel == 0 ? 1.0f : -1.0f;
    }
    if (markerFrame < kMarkerFrames) {
        return kMarkerCode[markerFrame * kChannels + channel];
    }
    constexpr std::array<CSAMPLE, 4> kLevels = {0.92f, -0.74f, 0.56f, -0.38f};
    const CSAMPLE level = kLevels[(markerFrame - kMarkerFrames) / 28];
    return channel == 0 ? level : -level;
}

struct ReadObservation {
    SINT startSample = 0;
    SINT numSamples = 0;
    bool reverse = false;
    std::size_t generation = 0;
};

struct DeterministicSource {
    static constexpr std::size_t kMaxReadObservations = 65536;

    std::array<CSAMPLE, kTrackFrames * kChannels> samples{};
    mutable std::array<ReadObservation, kMaxReadObservations> readObservations{};
    mutable std::size_t readObservationCount = 0;

    DeterministicSource() {
        for (int frame = 0; frame < kTrackFrames; ++frame) {
            const double phase = 0.017 * static_cast<double>(frame);
            samples[frame * kChannels] = static_cast<CSAMPLE>(
                    0.18 * std::sin(phase) + 0.07 * std::sin(phase * 0.37));
            samples[frame * kChannels + 1] =
                    static_cast<CSAMPLE>(0.16 * std::cos(phase * 0.71) -
                            0.05 * std::sin(phase * 0.19));
        }

        std::copy(kMarkerCode.begin(),
                kMarkerCode.end(),
                samples.begin() + kMarkerSourceFrame * kChannels);
        for (int frame = 0; frame < kEngineMarkerFrames; ++frame) {
            for (int channel = 0; channel < kChannels; ++channel) {
                samples[(kEngineMarkerSourceFrame + frame) * kChannels + channel] =
                        engineMarkerSample(frame, channel);
            }
        }
    }

    void resetReadObservations() const {
        readObservationCount = 0;
    }

    void recordRead(SINT startSample, SINT numSamples, bool reverse) const {
        if (readObservationCount < readObservations.size()) {
            readObservations[readObservationCount++] =
                    ReadObservation{startSample, numSamples, reverse};
        }
    }
};

CSAMPLE markerForRead(std::size_t readGeneration) {
    return static_cast<CSAMPLE>(
            0.25 + 0.0001 * static_cast<double>(readGeneration));
}

class GenerationMarkedSource {
  public:
    static constexpr std::size_t kMaxReadObservations = 65536;

    mutable std::array<ReadObservation, kMaxReadObservations> readObservations{};
    mutable std::size_t readObservationCount = 0;
    mutable std::size_t generation = 0;

    void resetReadObservations() const {
        readObservationCount = 0;
        generation = 0;
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) const {
        if (numSamples == 0) {
            return CachingReader::ReadResult::AVAILABLE;
        }

        const std::size_t readGeneration = ++generation;
        if (readObservationCount < readObservations.size()) {
            readObservations[readObservationCount++] =
                    ReadObservation{startSample,
                            numSamples,
                            reverse,
                            readGeneration};
        }

        const SINT sourceStart = reverse ? startSample - numSamples : startSample;
        const SINT sourceEnd = sourceStart + numSamples;
        const SINT sourceSize = kTrackFrames * kChannels;
        if (channelCount != mixxx::audio::ChannelCount::stereo() ||
                sourceStart < 0 || sourceEnd > sourceSize) {
            SampleUtil::clear(buffer, numSamples);
            return CachingReader::ReadResult::PARTIALLY_AVAILABLE;
        }

        // Every reader call gets a unique constant stereo marker. The marker
        // survives linear interpolation and ties recovered output to one
        // exact ReadObservation instead of to an absolute-position heuristic.
        const CSAMPLE marker = markerForRead(readGeneration);
        for (SINT sample = 0; sample < numSamples; ++sample) {
            buffer[sample] = sample % kChannels == 0 ? marker : -marker;
        }
        return CachingReader::ReadResult::AVAILABLE;
    }
};

struct DeterministicReaderContext {
    DeterministicSource normalSource;
    GenerationMarkedSource recoverySource;
    bool useRecoverySource = false;
};

DeterministicReaderContext g_readerContext;
DeterministicSource& g_source = g_readerContext.normalSource;
GenerationMarkedSource& g_recoverySource = g_readerContext.recoverySource;

class DeterministicCachingReader final : public CachingReader {
  public:
    DeterministicCachingReader(const QString& group,
            UserSettingsPointer pConfig,
            mixxx::audio::ChannelCount maxSupportedChannel,
            const DeterministicReaderContext* pContext)
            : CachingReader(group, pConfig, maxSupportedChannel),
              m_pContext(pContext) {
    }

    CachingReader::ReadResult read(SINT startSample,
            SINT numSamples,
            bool reverse,
            CSAMPLE* buffer,
            mixxx::audio::ChannelCount channelCount) override {
        if (numSamples == 0) {
            return CachingReader::ReadResult::AVAILABLE;
        }

        if (m_pContext->useRecoverySource) {
            return m_pContext->recoverySource.read(
                    startSample, numSamples, reverse, buffer, channelCount);
        }

        const DeterministicSource* const pSource =
                &m_pContext->normalSource;
        pSource->recordRead(startSample, numSamples, reverse);

        // This test only enables forward playback. Keep the implementation
        // exact for both directions so an accidental reverse request cannot
        // silently turn into a different source sequence.
        const SINT sourceStart = reverse ? startSample - numSamples : startSample;
        const SINT sourceEnd = sourceStart + numSamples;
        const SINT sourceSize = static_cast<SINT>(pSource->samples.size());
        if (channelCount != mixxx::audio::ChannelCount::stereo() ||
                sourceStart < 0 || sourceEnd > sourceSize) {
            SampleUtil::clear(buffer, numSamples);
            return CachingReader::ReadResult::PARTIALLY_AVAILABLE;
        }

        if (!reverse) {
            std::copy_n(pSource->samples.data() + sourceStart,
                    numSamples,
                    buffer);
        } else {
            for (SINT sample = 0; sample < numSamples; ++sample) {
                buffer[sample] = pSource->samples[sourceEnd - sample - 1];
            }
        }
        return CachingReader::ReadResult::AVAILABLE;
    }

  private:
    const DeterministicReaderContext* const m_pContext;
};

CachingReader* makeDeterministicReader(const QString& group,
        UserSettingsPointer pConfig,
        mixxx::audio::ChannelCount maxSupportedChannel,
        void* pContext) {
    return new DeterministicCachingReader(
            group,
            pConfig,
            maxSupportedChannel,
            static_cast<const DeterministicReaderContext*>(pContext));
}

class RecoverySourceScope {
  public:
    explicit RecoverySourceScope(DeterministicReaderContext* pContext)
            : m_pContext(pContext),
              m_previous(pContext->useRecoverySource) {
        m_pContext->useRecoverySource = true;
    }

    ~RecoverySourceScope() {
        m_pContext->useRecoverySource = m_previous;
    }

  private:
    DeterministicReaderContext* const m_pContext;
    const bool m_previous;
};

bool containsGenerationMarker(std::span<const CSAMPLE> samples,
        std::size_t generation) {
    const CSAMPLE marker = markerForRead(generation);
    for (std::size_t sample = 0; sample + 1 < samples.size(); sample += 2) {
        if (std::abs(samples[sample] - marker) < 1e-5f &&
                std::abs(samples[sample + 1] + marker) < 1e-5f) {
            return true;
        }
    }
    return false;
}

std::size_t findGenerationMarkedRead(
        const GenerationMarkedSource& source,
        std::size_t firstObservation,
        std::span<const CSAMPLE> samples) {
    for (std::size_t i = firstObservation;
            i < source.readObservationCount;
            ++i) {
        if (containsGenerationMarker(samples,
                    source.readObservations[i].generation)) {
            return i;
        }
    }
    return std::numeric_limits<std::size_t>::max();
}

bool matchesMarker(const CSAMPLE* pSamples) {
    return std::equal(kMarkerCode.begin(), kMarkerCode.end(), pSamples);
}

int countSourceMarkerOccurrences(const DeterministicSource& source) {
    int count = 0;
    for (int frame = 0; frame + kMarkerFrames <= kTrackFrames; ++frame) {
        if (matchesMarker(source.samples.data() + frame * kChannels)) {
            ++count;
        }
    }
    return count;
}

struct ReadAheadSnapshot {
    double startFrames = 0.0;
    double endFrames = 0.0;
    std::size_t observationCount = 0;
};

ReadAheadSnapshot latestReadAheadSnapshot(const DeterministicSource& source) {
    ReadAheadSnapshot snapshot;
    snapshot.observationCount = source.readObservationCount;
    if (source.readObservationCount == 0) {
        return snapshot;
    }

    const auto& read = source.readObservations[source.readObservationCount - 1];
    const SINT sourceStart = read.reverse
            ? read.startSample - read.numSamples
            : read.startSample;
    const SINT sourceEnd = read.reverse
            ? read.startSample
            : read.startSample + read.numSamples;
    snapshot.startFrames = static_cast<double>(sourceStart) / kChannels;
    snapshot.endFrames = static_cast<double>(sourceEnd) / kChannels;
    return snapshot;
}

ReadAheadSnapshot readAheadSnapshotSince(
        const DeterministicSource& source,
        std::size_t firstObservation) {
    ReadAheadSnapshot snapshot;
    const std::size_t first = std::min(
            firstObservation, source.readObservationCount);
    for (std::size_t i = first; i < source.readObservationCount; ++i) {
        const auto& read = source.readObservations[i];
        const SINT sourceStart = read.reverse
                ? read.startSample - read.numSamples
                : read.startSample;
        const SINT sourceEnd = read.reverse
                ? read.startSample
                : read.startSample + read.numSamples;
        const double startFrames =
                static_cast<double>(sourceStart) / kChannels;
        const double endFrames =
                static_cast<double>(sourceEnd) / kChannels;
        if (snapshot.observationCount == 0) {
            snapshot.startFrames = startFrames;
            snapshot.endFrames = endFrames;
        } else {
            snapshot.startFrames = std::min(snapshot.startFrames, startFrames);
            snapshot.endFrames = std::max(snapshot.endFrames, endFrames);
        }
        ++snapshot.observationCount;
    }
    return snapshot;
}

struct MarkerMatch {
    bool found = false;
    int outputFrame = -1;
};

MarkerMatch correlateMarker(const std::array<CSAMPLE, kBufferSamples>& output) {
    for (int frame = 0; frame + kMarkerFrames <= kBufferFrames; ++frame) {
        if (matchesMarker(output.data() + frame * kChannels)) {
            return MarkerMatch{true, frame};
        }
    }
    return {};
}

struct MarkerSimilarity {
    double correlation = -1.0;
    double normalizedError = std::numeric_limits<double>::max();
    int outputFrame = -1;
};

MarkerSimilarity findBestEngineMarkerSimilarity(
        std::span<const CSAMPLE> output) {
    double markerEnergy = 0.0;
    double markerSum = 0.0;
    for (int frame = 0; frame < kEngineMarkerFrames; ++frame) {
        for (int channel = 0; channel < kChannels; ++channel) {
            const double sample = engineMarkerSample(frame, channel);
            markerEnergy += sample * sample;
            markerSum += sample;
        }
    }

    MarkerSimilarity best;
    const double sampleCount = kEngineMarkerFrames * kChannels;
    const double markerMean = markerSum / sampleCount;
    double markerVariance = 0.0;
    for (int frame = 0; frame < kEngineMarkerFrames; ++frame) {
        for (int channel = 0; channel < kChannels; ++channel) {
            const double markerDelta =
                    engineMarkerSample(frame, channel) - markerMean;
            markerVariance += markerDelta * markerDelta;
        }
    }

    const int outputFrames = static_cast<int>(output.size() / kChannels);
    for (int frame = 0;
            frame + kEngineMarkerFrames <= outputFrames;
            ++frame) {
        double outputSum = 0.0;
        for (int markerFrame = 0;
                markerFrame < kEngineMarkerFrames;
                ++markerFrame) {
            for (int channel = 0; channel < kChannels; ++channel) {
                outputSum += output[(frame + markerFrame) * kChannels + channel];
            }
        }
        const double outputMean = outputSum / sampleCount;
        double covariance = 0.0;
        double outputVariance = 0.0;
        double error = 0.0;
        for (int markerFrame = 0;
                markerFrame < kEngineMarkerFrames;
                ++markerFrame) {
            for (int channel = 0; channel < kChannels; ++channel) {
                const double outputSample =
                        output[(frame + markerFrame) * kChannels + channel];
                const double markerSample = engineMarkerSample(markerFrame, channel);
                const double outputDelta = outputSample - outputMean;
                const double markerDelta = markerSample - markerMean;
                const double difference = outputSample - markerSample;
                covariance += outputDelta * markerDelta;
                outputVariance += outputDelta * outputDelta;
                error += difference * difference;
            }
        }
        const double correlation = markerVariance > 0.0 && outputVariance > 0.0
                ? covariance / std::sqrt(outputVariance * markerVariance)
                : 0.0;
        if (correlation > best.correlation) {
            best.correlation = correlation;
            best.normalizedError = error / markerEnergy;
            best.outputFrame = frame;
        }
    }
    return best;
}

#if defined(__SIGNALSMITH__) || defined(__BUNGEE__)
MarkerSimilarity findEngineMarkerOnset(std::span<const CSAMPLE> output,
        double sourceRate,
        double threshold) {
    const int expectedOutputFrame = static_cast<int>(std::round(
            kEngineMarkerSourceFrame / sourceRate));
    const int outputFrames = static_cast<int>(output.size() / kChannels);
    // The source marker cannot be audible before its nominal source-to-output
    // time. Starting at that boundary avoids mistaking the deterministic
    // background waveform for the marker onset.
    const int firstFrame = std::max(0, expectedOutputFrame);
    const int lastFrame = std::min(outputFrames - 1, expectedOutputFrame + 1400);
    for (int frame = firstFrame; frame <= lastFrame; ++frame) {
        const double score = std::abs(static_cast<double>(
                                              output[frame * kChannels]) -
                static_cast<double>(output[frame * kChannels + 1]));
        if (score >= threshold) {
            MarkerSimilarity onset;
            onset.correlation = score;
            onset.normalizedError = 0.0;
            onset.outputFrame = frame;
            return onset;
        }
    }
    return {};
}
#endif

class FixedVSyncProvider final : public VSyncTimeProvider {
  public:
    explicit FixedVSyncProvider(int outputOffsetFrames = kVSyncOffsetFrames)
            : m_offset(std::chrono::microseconds(static_cast<long long>(
                      std::llround(static_cast<double>(outputOffsetFrames) *
                              1000000.0 / kSampleRate)))) {
    }

    std::chrono::microseconds fromTimerToNextSync(
            const PerformanceTimer&) override {
        return m_offset;
    }

    std::chrono::microseconds getSyncInterval() const override {
        return std::chrono::microseconds(16667);
    }

  private:
    const std::chrono::microseconds m_offset;
};

#if defined(__SIGNALSMITH__) || defined(__BUNGEE__)
struct MarkerPlayheadPixelResult {
    bool rendererInitialized = false;
    double markerPixel = 0.0;
    double playheadPixel = 0.0;
    double playheadSample = 0.0;
};

MarkerPlayheadPixelResult replayMarkerAtSyntheticVSync(
        EngineBuffer* pEngineBuffer,
        const QString& group,
        const TrackPointer& track,
        int callbackIndex,
        int markerOutputFrame) {
    MarkerPlayheadPixelResult result;
    ControlObject::set(ConfigKey(group, QStringLiteral("play")), 0.0);
    pEngineBuffer->loadFakeTrack(track, false);
    pEngineBuffer->seekExact(mixxx::audio::kStartFramePos);
    ControlObject::set(ConfigKey(group, QStringLiteral("play")), 1.0);

    const auto visualPlayPosition =
            VisualPlayPosition::getVisualPlayPosition(group);
    FixedVSyncProvider vsync(markerOutputFrame);
    WaveformWidgetRenderer renderer(group);
    result.rendererInitialized = renderer.init();
    renderer.setTrack(track);
    renderer.resizeRenderer(kRendererWidth, 100, 1.0f);
    std::array<CSAMPLE, kBufferSamples> output{};
    for (int callback = 0; callback <= callbackIndex; ++callback) {
        if (callback == callbackIndex && result.rendererInitialized &&
                visualPlayPosition->isValid()) {
            renderer.onPreRender(&vsync);
            result.playheadSample = renderer.getTruePosSample();
            result.playheadPixel = renderer.transformSamplePositionInRendererWorld(
                    result.playheadSample);
            result.markerPixel = renderer.transformSamplePositionInRendererWorld(
                    static_cast<double>(kEngineMarkerSourceFrame * kChannels));
        }
        pEngineBuffer->process(output.data(), kBufferSamples);
        pEngineBuffer->postProcess(kBufferSamples);
    }
    return result;
}
#endif

struct StretchedMarkerProbeResult {
    MarkerSimilarity similarity;
    int callbackIndex = -1;
    double playPosBeforeFrames = 0.0;
    double playPosAfterFrames = 0.0;
    double visualPlayPosBeforeFrames = 0.0;
    double visualVSyncPosBeforeFrames = 0.0;
    double effectiveRate = 0.0;
    double requestedOutputFrames = 0.0;
    double returnedSourceFrames = 0.0;
    double readAheadStartFrames = 0.0;
    double readAheadEndFrames = 0.0;
    std::size_t readAheadObservationCount = 0;
    double maximumOutput = 0.0;
};

void configureAlignmentControls(const QString& group,
        EngineBuffer::KeylockEngine engine,
        double rateRatio) {
    ControlObject::set(ConfigKey(QStringLiteral("[App]"),
                               QStringLiteral("samplerate")),
            kSampleRate);
    ControlObject::set(ConfigKey(group, QStringLiteral("keylock_engine")),
            static_cast<double>(engine));
    ControlObject::set(ConfigKey(group, QStringLiteral("play")), 0.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("rate")), 0.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("rateSearch")), 0.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("rate_dir")), 1.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("rate_ratio")), rateRatio);
    ControlObject::set(ConfigKey(group, QStringLiteral("pitch")), 0.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("pitch_adjust")), 0.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("keylock")), 1.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("reverse")), 0.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("slip_enabled")), 0.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("repeat")), 0.0);
    ControlObject::set(ConfigKey(group, QStringLiteral("passthrough")), 0.0);
}

const char* keylockEngineTraceName(EngineBuffer::KeylockEngine engine) {
    switch (engine) {
    case EngineBuffer::KeylockEngine::SoundTouch:
        return "SoundTouch";
#ifdef __RUBBERBAND__
    case EngineBuffer::KeylockEngine::RubberBandFaster:
        return "RubberBandFaster";
    case EngineBuffer::KeylockEngine::RubberBandFiner:
        return "RubberBandR3";
#endif
#ifdef __BUNGEE__
    case EngineBuffer::KeylockEngine::Bungee:
        return "Bungee";
#endif
#ifdef __SIGNALSMITH__
    case EngineBuffer::KeylockEngine::SignalSmith:
        return "SignalSmith";
#endif
    default:
        return "Unknown";
    }
}

struct CommonScalerPositionTraceRecord {
    const char* engine = "";
    const char* activeEngine = "";
    const char* scenario = "";
    int trackSampleRateHz = 0;
    int outputSampleRateHz = 0;
    double tempoRatio = 0.0;
    double pitchRatio = 0.0;
    const char* direction = "forward";
    int callback = 0;
    int markerSourceFrame = kEngineMarkerSourceFrame;
    int markerOutputFrameAbsolute = -1;
    int markerFound = 0;
    double markerCorrelation = 0.0;
    double markerNormalizedError = 0.0;
    double playPosBeforeFrames = 0.0;
    double playPosAfterFrames = 0.0;
    double returnedSourceFrames = 0.0;
    double effectiveRate = 0.0;
    double readAheadStartFrames = 0.0;
    double readAheadEndFrames = 0.0;
    std::size_t readAheadObservationCount = 0;
    double scalerVisualOffsetSourceFrames = 0.0;
    double visualEnginePlayBeforeFrames = 0.0;
    double visualVSyncBeforeFrames = 0.0;
    double outputMaxAbs = 0.0;
    int outputAllFinite = 1;
};

double tracePlayPositionValue(mixxx::audio::FramePos position) {
    // FramePos::isValid() intentionally accepts every finite value. The
    // engine's finite kInitialPlayPosition is nevertheless a semantic
    // sentinel used until the initial seek has been processed.
    if (!position.isValid() || position == kInitialPlayPosition) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return position.value();
}

void writeCommonScalerPositionTrace(
        const QString& traceDirectory,
        const std::vector<CommonScalerPositionTraceRecord>& records) {
    QDir outputDirectory(traceDirectory);
    ASSERT_TRUE(outputDirectory.exists() || outputDirectory.mkpath(QStringLiteral(".")))
            << "Could not create trace directory " << traceDirectory.toStdString();

    const QString tracePath = outputDirectory.filePath(
            QStringLiteral("engine-position-trace-%1.tsv")
                    .arg(QCoreApplication::applicationPid()));
    QSaveFile traceFile(tracePath);
    ASSERT_TRUE(traceFile.open(QIODevice::WriteOnly | QIODevice::Text))
            << "Could not open trace file " << tracePath.toStdString();

    QTextStream trace(&traceFile);
    trace.setRealNumberPrecision(17);
    trace << "# Synthetic test trace: visual_engine_play_before_frames and "
             "visual_vsync_before_frames are VisualPlayPosition predictions, "
             "not physical DAC or VSync captures.\n";
    trace << "# Follow-up gaps: reverse, seek, loop, and rate-transition scenarios "
             "are not included because this marker matcher is forward-oriented "
             "and this bounded trace does not claim physical device timing.\n";
    trace << "engine\tactive_engine\tscenario\ttrack_sample_rate_hz\t"
             "output_sample_rate_hz\ttempo_ratio\tpitch_ratio\tdirection\t"
             "callback\tmarker_source_frame\tmarker_output_frame_absolute\t"
             "marker_found\tmarker_correlation\tmarker_normalized_error\t"
             "m_playPos_before_frames\tm_playPos_after_frames\t"
             "returned_source_frames\teffective_rate\tread_ahead_start_frames\t"
             "read_ahead_end_frames\tread_ahead_observation_count\t"
             "scaler_visual_offset_source_frames\t"
             "visual_engine_play_before_frames\tvisual_vsync_before_frames\t"
             "output_max_abs\toutput_all_finite\n";

    for (const auto& record : records) {
        trace << record.engine << '\t'
              << record.activeEngine << '\t'
              << record.scenario << '\t'
              << record.trackSampleRateHz << '\t'
              << record.outputSampleRateHz << '\t'
              << record.tempoRatio << '\t'
              << record.pitchRatio << '\t'
              << record.direction << '\t'
              << record.callback << '\t'
              << record.markerSourceFrame << '\t'
              << record.markerOutputFrameAbsolute << '\t'
              << record.markerFound << '\t'
              << record.markerCorrelation << '\t'
              << record.markerNormalizedError << '\t'
              << record.playPosBeforeFrames << '\t'
              << record.playPosAfterFrames << '\t'
              << record.returnedSourceFrames << '\t'
              << record.effectiveRate << '\t'
              << record.readAheadStartFrames << '\t'
              << record.readAheadEndFrames << '\t'
              << record.readAheadObservationCount << '\t'
              << record.scalerVisualOffsetSourceFrames << '\t'
              << record.visualEnginePlayBeforeFrames << '\t'
              << record.visualVSyncBeforeFrames << '\t'
              << record.outputMaxAbs << '\t'
              << record.outputAllFinite << '\n';
    }
    trace.flush();
    ASSERT_TRUE(traceFile.commit())
            << "Could not commit trace file " << tracePath.toStdString();
    GTEST_LOG_(INFO) << "Common scaler position trace: "
                     << tracePath.toStdString()
                     << " (" << records.size() << " records)";
}

#if defined(__SIGNALSMITH__) || defined(__BUNGEE__)
StretchedMarkerProbeResult runStretchedMarkerProbe(
        EngineBuffer* pEngineBuffer,
        const QString& group,
        const TrackPointer& track,
        double sourceRate,
        double markerThreshold) {
    pEngineBuffer->loadFakeTrack(track, false);
    pEngineBuffer->seekExact(mixxx::audio::kStartFramePos);
    ControlObject::set(ConfigKey(group, QStringLiteral("play")), 1.0);

    std::array<CSAMPLE, kBufferSamples> output{};
    std::vector<CSAMPLE> emitted;
    emitted.reserve(kBufferSamples * 80);
    std::array<double, 80> playPositionsBefore{};
    std::array<double, 80> playPositionsAfter{};
    std::array<double, 80> visualPlayPositionsBefore{};
    std::array<double, 80> visualVSyncPositionsBefore{};
    std::array<double, 80> effectiveRates{};
    std::array<double, 80> readAheadStarts{};
    std::array<double, 80> readAheadEnds{};
    std::array<std::size_t, 80> readAheadCounts{};
    FixedVSyncProvider vsync;
    const auto visualPlayPosition = VisualPlayPosition::getVisualPlayPosition(group);
    const double engineTrackFrames = pEngineBuffer->getTrackEndPosition().value();
    double maximumOutput = 0.0;
    g_source.resetReadObservations();
    for (int callback = 0; callback < 80; ++callback) {
        playPositionsBefore[callback] = pEngineBuffer->getPlayPos().value();
        visualPlayPositionsBefore[callback] =
                visualPlayPosition->getEnginePlayPos() * engineTrackFrames;
        visualVSyncPositionsBefore[callback] =
                visualPlayPosition->getAtNextVSync(&vsync) * engineTrackFrames;
        pEngineBuffer->process(output.data(), kBufferSamples);
        pEngineBuffer->postProcess(kBufferSamples);
        playPositionsAfter[callback] = pEngineBuffer->getPlayPos().value();
        effectiveRates[callback] = pEngineBuffer->getSpeed();
        const ReadAheadSnapshot readAhead = latestReadAheadSnapshot(g_source);
        readAheadStarts[callback] = readAhead.startFrames;
        readAheadEnds[callback] = readAhead.endFrames;
        readAheadCounts[callback] = readAhead.observationCount;
        emitted.insert(emitted.end(), output.begin(), output.end());
        for (CSAMPLE sample : output) {
            maximumOutput = std::max(maximumOutput,
                    std::abs(static_cast<double>(sample)));
        }
    }

    StretchedMarkerProbeResult result;
    result.similarity = findEngineMarkerOnset(
            emitted, sourceRate, markerThreshold);
    result.callbackIndex = result.similarity.outputFrame >= 0
            ? result.similarity.outputFrame / kBufferFrames
            : -1;
    if (result.callbackIndex >= 0 && result.callbackIndex < 80) {
        result.playPosBeforeFrames = playPositionsBefore[result.callbackIndex];
        result.playPosAfterFrames = playPositionsAfter[result.callbackIndex];
        result.visualPlayPosBeforeFrames =
                visualPlayPositionsBefore[result.callbackIndex];
        result.visualVSyncPosBeforeFrames =
                visualVSyncPositionsBefore[result.callbackIndex];
        result.effectiveRate = effectiveRates[result.callbackIndex];
        result.requestedOutputFrames = kBufferFrames;
        result.returnedSourceFrames = result.playPosAfterFrames -
                result.playPosBeforeFrames;
        result.readAheadStartFrames = readAheadStarts[result.callbackIndex];
        result.readAheadEndFrames = readAheadEnds[result.callbackIndex];
        result.readAheadObservationCount = readAheadCounts[result.callbackIndex];
    }
    result.maximumOutput = maximumOutput;
    return result;
}
#endif

struct AlignmentObservation {
    int callbackIndex = -1;
    int markerOutputFrame = -1;
    int markerSourceFrame = -1;
    int markerFound = 0;
    int firstDivergentClock = 0;
    double playPosBeforeFrames = 0.0;
    double playPosAfterFrames = 0.0;
    double effectiveRate = 0.0;
    double visualEnginePlayPosBefore = 0.0;
    double visualAtNextVSyncBefore = 0.0;
    double expectedVisualAtNextVSync = 0.0;
    double rendererTruePosSample = 0.0;
    double rendererMarkerPixel = 0.0;
    double rendererNeighbourPixel = 0.0;
    double requestedOutputFrames = 0.0;
    double returnedSourceFrames = 0.0;
    double readAheadStartFrames = 0.0;
    double readAheadEndFrames = 0.0;
    std::size_t readAheadObservationCount = 0;
};

static_assert(std::is_trivially_copyable_v<AlignmentObservation>);

void writeFailureTrace(const AlignmentObservation* observations,
        std::size_t observationCount,
        int sourceMarkerOccurrences) {
    std::ofstream trace(kTracePath.toStdString(), std::ios::trunc);
    if (!trace) {
        return;
    }
    trace << std::setprecision(17);
    trace << "reproduction_command=mixxx-test --gtest_color=no "
             "--gtest_filter=EngineBufferAlignmentTest.RealProcessReadAheadVisualMarkerChain\n";
    trace << "source_marker_occurrences=" << sourceMarkerOccurrences << '\n';
    trace << "marker_source_frame=" << kMarkerSourceFrame << '\n';
    trace << "buffer_frames=" << kBufferFrames << '\n';
    trace << "vsync_offset_micros=" << kVSyncOffsetMicros << '\n';
    trace << "first_divergent_clock=1 source->output, 2 output->m_playPos, "
             "3 m_playPos->VisualPlayPosition, 4 visual->renderer\n";
    for (std::size_t i = 0; i < observationCount; ++i) {
        const AlignmentObservation& observation = observations[i];
        trace << "callback=" << observation.callbackIndex
              << " first_divergent_clock=" << observation.firstDivergentClock
              << " marker_found=" << observation.markerFound
              << " marker_output_frame=" << observation.markerOutputFrame
              << " marker_source_frame=" << observation.markerSourceFrame
              << " play_before_frames=" << observation.playPosBeforeFrames
              << " play_after_frames=" << observation.playPosAfterFrames
              << " effective_rate=" << observation.effectiveRate
              << " visual_engine_play_before="
              << observation.visualEnginePlayPosBefore
              << " visual_next_vsync_before="
              << observation.visualAtNextVSyncBefore
              << " expected_visual_next_vsync="
              << observation.expectedVisualAtNextVSync
              << " renderer_true_pos_sample="
              << observation.rendererTruePosSample
              << " renderer_marker_pixel="
              << observation.rendererMarkerPixel
              << " renderer_neighbour_pixel="
              << observation.rendererNeighbourPixel
              << " requested_output_frames="
              << observation.requestedOutputFrames
              << " returned_source_frames="
              << observation.returnedSourceFrames
              << " read_ahead_start_frames="
              << observation.readAheadStartFrames
              << " read_ahead_end_frames="
              << observation.readAheadEndFrames
              << " read_ahead_observation_count="
              << observation.readAheadObservationCount << '\n';
    }
}

#if defined(__SIGNALSMITH__) || defined(__BUNGEE__)
void writeEngineMarkerFailureTrace(const char* engine,
        double correlation,
        double normalizedError,
        int outputFrame,
        int callbackIndex,
        double playPosBeforeFrames,
        double playPosAfterFrames,
        double visualPlayPosBeforeFrames,
        double visualVSyncPosBeforeFrames,
        double rendererPosBeforeSamples,
        double markerPixel,
        double rendererPlayheadPixel,
        double requestedOutputFrames,
        double returnedSourceFrames,
        double readAheadStartFrames,
        double readAheadEndFrames,
        std::size_t readAheadObservationCount,
        int firstDivergentClock,
        double maximumOutput,
        const char* scenario = "EngineMarkerTracksEnginePosition") {
    std::ofstream trace(kEngineMarkerTracePath.toStdString(), std::ios::trunc);
    if (!trace) {
        return;
    }
    trace << std::setprecision(17);
    trace << "reproduction_command=mixxx-test --gtest_color=no "
             "--gtest_filter=EngineBufferAlignmentTest."
          << engine
          << scenario << '\n';
    trace << "engine=" << engine << '\n';
    trace << "clock_map=1 source->output, 2 output->m_playPos, "
             "3 m_playPos->VisualPlayPosition, 4 visual->renderer\n";
    trace << "first_divergent_clock=" << firstDivergentClock << '\n';
    trace << "callback_index=" << callbackIndex << '\n';
    trace << "marker_source_frame=" << kEngineMarkerSourceFrame << '\n';
    trace << "marker_output_frame=" << outputFrame << '\n';
    trace << "marker_correlation=" << correlation << '\n';
    trace << "marker_normalized_error=" << normalizedError << '\n';
    trace << "play_before_frames=" << playPosBeforeFrames << '\n';
    trace << "play_after_frames=" << playPosAfterFrames << '\n';
    trace << "visual_play_before_frames=" << visualPlayPosBeforeFrames << '\n';
    trace << "visual_vsync_before_frames=" << visualVSyncPosBeforeFrames << '\n';
    trace << "renderer_pos_before_samples=" << rendererPosBeforeSamples << '\n';
    trace << "marker_pixel=" << markerPixel << '\n';
    trace << "renderer_playhead_pixel=" << rendererPlayheadPixel << '\n';
    trace << "requested_output_frames=" << requestedOutputFrames << '\n';
    trace << "returned_source_frames=" << returnedSourceFrames << '\n';
    trace << "read_ahead_start_frames=" << readAheadStartFrames << '\n';
    trace << "read_ahead_end_frames=" << readAheadEndFrames << '\n';
    trace << "read_ahead_observation_count=" << readAheadObservationCount << '\n';
    trace << "maximum_output=" << maximumOutput << '\n';
}
#endif

} // namespace

class EngineBufferAlignmentTest : public BaseSignalPathTest {
  protected:
    static void SetUpTestSuite() {
        EngineBuffer::setTestReaderFactory(
                &makeDeterministicReader,
                &g_readerContext);
    }

    static void TearDownTestSuite() {
        EngineBuffer::setTestReaderFactory(nullptr);
    }
};

TEST_F(EngineBufferAlignmentTest, CommonScalerPositionTrace) {
    const QString traceDirectory = qEnvironmentVariable(
            "MIXXX_ENGINE_POSITION_TRACE_DIR");
    if (traceDirectory.trimmed().isEmpty()) {
        GTEST_SKIP() << "Set MIXXX_ENGINE_POSITION_TRACE_DIR to opt in";
    }

    constexpr int kTraceCallbacks = 80;
    struct TraceScenario {
        const char* name;
        double tempoRatio;
    };
    constexpr std::array<TraceScenario, 2> kTraceScenarios = {{
            {"forward-unity", 1.0},
            {"forward-stretched-1.25", 1.25},
    }};

    TrackPointer track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::kEngineChannelOutputCount,
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));

    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    const auto visualPlayPosition =
            VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    FixedVSyncProvider vsync;
    std::vector<CommonScalerPositionTraceRecord> records;
    records.reserve(EngineBuffer::kKeylockEngines.size() *
            kTraceScenarios.size() * kTraceCallbacks);

    for (const auto engine : EngineBuffer::kKeylockEngines) {
        if (!EngineBuffer::isKeylockEngineAvailable(engine)) {
            GTEST_LOG_(INFO) << "Skipping unavailable keylock engine "
                             << keylockEngineTraceName(engine);
            continue;
        }

        for (const auto& scenario : kTraceScenarios) {
            configureAlignmentControls(
                    m_sGroup1, engine, scenario.tempoRatio);
            pEngineBuffer->loadFakeTrack(track, false);
            pEngineBuffer->seekExact(mixxx::audio::kStartFramePos);
            ControlObject::set(
                    ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);

            ASSERT_EQ(pEngineBuffer->m_iKeylockEngine.loadAcquire(),
                    static_cast<int>(engine))
                    << "Requested engine " << keylockEngineTraceName(engine)
                    << " was not active";
            EngineBufferScale* expectedKeylockScaler = nullptr;
            switch (engine) {
            case EngineBuffer::KeylockEngine::SoundTouch:
                expectedKeylockScaler = pEngineBuffer->m_pScaleST;
                break;
#ifdef __RUBBERBAND__
            case EngineBuffer::KeylockEngine::RubberBandFaster:
            case EngineBuffer::KeylockEngine::RubberBandFiner:
                expectedKeylockScaler = pEngineBuffer->m_pScaleRB;
                break;
#endif
#ifdef __BUNGEE__
            case EngineBuffer::KeylockEngine::Bungee:
                if (const auto* pState = pEngineBuffer->m_pBungeePublishedState.load(
                            std::memory_order_seq_cst)) {
                    expectedKeylockScaler = pState->pScaler;
                }
                break;
#endif
#ifdef __SIGNALSMITH__
            case EngineBuffer::KeylockEngine::SignalSmith:
                expectedKeylockScaler = pEngineBuffer->m_pScaleSignalSmith;
                break;
#endif
            default:
                break;
            }
            ASSERT_NE(nullptr, expectedKeylockScaler)
                    << "The requested keylock scaler is unavailable for "
                    << keylockEngineTraceName(engine);

            const char* const engineName = keylockEngineTraceName(engine);
            const std::size_t firstRecord = records.size();
            std::array<CSAMPLE, kBufferSamples> output{};
            std::vector<CSAMPLE> emitted;
            emitted.reserve(kBufferSamples * kTraceCallbacks);
            const double engineTrackFrames =
                    pEngineBuffer->getTrackEndPosition().value();
            g_source.resetReadObservations();

            for (int callback = 0; callback < kTraceCallbacks; ++callback) {
                CommonScalerPositionTraceRecord record;
                record.engine = engineName;
                record.activeEngine = engineName;
                record.scenario = scenario.name;
                record.trackSampleRateHz = kSampleRate;
                record.outputSampleRateHz = kSampleRate;
                record.tempoRatio = scenario.tempoRatio;
                record.pitchRatio = 1.0;
                record.direction = "forward";
                record.callback = callback;
                const auto playPosBefore = pEngineBuffer->getPlayPos();
                record.playPosBeforeFrames =
                        tracePlayPositionValue(playPosBefore);
                record.visualEnginePlayBeforeFrames =
                        visualPlayPosition->getEnginePlayPos() *
                        engineTrackFrames;
                record.visualVSyncBeforeFrames =
                        visualPlayPosition->getAtNextVSync(&vsync) *
                        engineTrackFrames;
                const std::size_t firstObservation =
                        g_source.readObservationCount;

                pEngineBuffer->process(output.data(), kBufferSamples);
                pEngineBuffer->postProcess(kBufferSamples);

                const auto playPosAfter = pEngineBuffer->getPlayPos();
                record.playPosAfterFrames = tracePlayPositionValue(playPosAfter);
                record.returnedSourceFrames =
                        std::isfinite(record.playPosBeforeFrames) &&
                                std::isfinite(record.playPosAfterFrames)
                        ? record.playPosAfterFrames - record.playPosBeforeFrames
                        : std::numeric_limits<double>::quiet_NaN();
                record.effectiveRate = pEngineBuffer->getSpeed();
                const ReadAheadSnapshot readAhead =
                        readAheadSnapshotSince(g_source, firstObservation);
                record.readAheadObservationCount = readAhead.observationCount;
                record.readAheadStartFrames = readAhead.startFrames;
                record.readAheadEndFrames = readAhead.endFrames;
                record.scalerVisualOffsetSourceFrames =
                        pEngineBuffer->m_pScale->getVisualPlayPositionOffset();
                for (const CSAMPLE sample : output) {
                    record.outputMaxAbs = std::max(
                            record.outputMaxAbs,
                            std::abs(static_cast<double>(sample)));
                    record.outputAllFinite = record.outputAllFinite &&
                            std::isfinite(static_cast<double>(sample));
                }
                EXPECT_EQ(record.outputAllFinite, 1)
                        << "Non-finite output from " << engineName
                        << " in scenario " << scenario.name
                        << " callback " << callback;
                emitted.insert(emitted.end(), output.begin(), output.end());
                records.push_back(record);
            }

            const MarkerSimilarity marker =
                    findBestEngineMarkerSimilarity(emitted);
            const bool markerFound = marker.outputFrame >= 0 &&
                    marker.correlation >= 0.3;
            EXPECT_TRUE(markerFound)
                    << "Marker was not reliably observed for " << engineName
                    << " in scenario " << scenario.name
                    << "; best correlation=" << marker.correlation;
            if (markerFound) {
                const int callback = marker.outputFrame / kBufferFrames;
                if (callback >= 0 && callback < kTraceCallbacks) {
                    auto& record = records[firstRecord + callback];
                    record.markerFound = 1;
                    record.markerOutputFrameAbsolute = marker.outputFrame;
                    record.markerCorrelation = marker.correlation;
                    record.markerNormalizedError = marker.normalizedError;
                }
            }
        }
    }

    ASSERT_FALSE(records.empty()) << "No keylock engines were available";
    writeCommonScalerPositionTrace(traceDirectory, records);
}

TEST_F(EngineBufferAlignmentTest, RealProcessReadAheadVisualMarkerChain) {
    constexpr double kExpectedMarkerPixel = kRendererWidth * 0.5;

    TrackPointer track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::kEngineChannelOutputCount,
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));

    // Establish every transport input used by RateControl and EngineBuffer.
    ControlObject::set(ConfigKey(QStringLiteral("[App]"), QStringLiteral("samplerate")),
            kSampleRate);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rateSearch")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate_dir")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate_ratio")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("pitch")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("pitch_adjust")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("reverse")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("slip_enabled")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("repeat")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("passthrough")), 0.0);

    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    g_source.resetReadObservations();
    pEngineBuffer->loadFakeTrack(track, false);
    pEngineBuffer->seekExact(mixxx::audio::kStartFramePos);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);

    std::array<CSAMPLE, kBufferSamples> output{};
    pEngineBuffer->process(output.data(), kBufferSamples);
    pEngineBuffer->postProcess(kBufferSamples);
    const double engineTrackFrames = pEngineBuffer->getTrackEndPosition().value();

    const auto visualPlayPosition =
            VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    FixedVSyncProvider vsync;
    WaveformWidgetRenderer renderer(m_sGroup1);
    const bool rendererInitialized = renderer.init();
    renderer.setTrack(track);
    renderer.resizeRenderer(kRendererWidth, 100, 1.0f);

    std::array<AlignmentObservation, 20> observations{};
    std::size_t observationCount = 0;
    int firstDivergentClock = 0;
    bool allChecksPassed = rendererInitialized;
    bool markerObserved = false;
    const int sourceMarkerOccurrences = countSourceMarkerOccurrences(g_source);

    auto check = [&](bool condition, int clock, AlignmentObservation* observation) {
        if (!condition) {
            allChecksPassed = false;
            if (firstDivergentClock == 0) {
                firstDivergentClock = clock;
            }
            if (observation->firstDivergentClock == 0) {
                observation->firstDivergentClock = clock;
            }
        }
    };

    // The first callback establishes the initial m_playPos published to the
    // visual clock. The marker is placed exactly 240 frames into a later
    // callback, which is the fixed 5 ms VSync prediction from that state.
    for (int callback = 0;
            callback < static_cast<int>(observations.size()) && !markerObserved;
            ++callback) {
        AlignmentObservation& observation = observations[observationCount++];
        observation.callbackIndex = callback;
        observation.playPosBeforeFrames = pEngineBuffer->getPlayPos().value();
        observation.visualEnginePlayPosBefore =
                visualPlayPosition->getEnginePlayPos();
        observation.visualAtNextVSyncBefore =
                visualPlayPosition->getAtNextVSync(&vsync);
        observation.expectedVisualAtNextVSync =
                (observation.playPosBeforeFrames + kVSyncOffsetFrames) /
                engineTrackFrames;

        if (rendererInitialized) {
            renderer.onPreRender(&vsync);
            observation.rendererTruePosSample = renderer.getTruePosSample();
        }

        pEngineBuffer->process(output.data(), kBufferSamples);
        pEngineBuffer->postProcess(kBufferSamples);

        observation.playPosAfterFrames = pEngineBuffer->getPlayPos().value();
        observation.effectiveRate = pEngineBuffer->getSpeed();
        observation.requestedOutputFrames = kBufferFrames;
        observation.returnedSourceFrames =
                observation.playPosAfterFrames - observation.playPosBeforeFrames;
        const ReadAheadSnapshot readAhead = latestReadAheadSnapshot(g_source);
        observation.readAheadStartFrames = readAhead.startFrames;
        observation.readAheadEndFrames = readAhead.endFrames;
        observation.readAheadObservationCount = readAhead.observationCount;

        const MarkerMatch markerMatch = correlateMarker(output);
        observation.markerFound = markerMatch.found ? 1 : 0;
        observation.markerOutputFrame = markerMatch.outputFrame;
        if (markerMatch.found) {
            markerObserved = true;
            observation.markerSourceFrame = kMarkerSourceFrame;
            observation.rendererMarkerPixel =
                    renderer.transformSamplePositionInRendererWorld(
                            static_cast<double>(kMarkerSourceFrame * kChannels));
            observation.rendererNeighbourPixel =
                    renderer.transformSamplePositionInRendererWorld(
                            static_cast<double>((kMarkerSourceFrame +
                                                        kMarkerNeighbourFrames) *
                                    kChannels));
        }

        check(sourceMarkerOccurrences == 1, 1, &observation);
        check(observation.requestedOutputFrames == kBufferFrames, 1, &observation);
        check(observation.returnedSourceFrames >= 0.0, 2, &observation);
        check(observation.readAheadObservationCount > 0, 1, &observation);
        check(observation.readAheadEndFrames >= observation.readAheadStartFrames,
                1,
                &observation);
        if (markerMatch.found) {
            check(markerMatch.outputFrame == kVSyncOffsetFrames, 1, &observation);
            check(std::abs(observation.markerSourceFrame -
                          (observation.playPosBeforeFrames +
                                  markerMatch.outputFrame)) < 1e-9,
                    2,
                    &observation);
            check(std::abs(observation.playPosAfterFrames -
                          (observation.playPosBeforeFrames +
                                  kBufferFrames)) < 1e-9,
                    2,
                    &observation);
            check(std::abs(observation.effectiveRate - 1.0) < 1e-9,
                    2,
                    &observation);
            check(std::abs(observation.visualAtNextVSyncBefore -
                          observation.expectedVisualAtNextVSync) < 1e-9,
                    3,
                    &observation);
            check(std::abs(observation.visualEnginePlayPosBefore -
                          observation.playPosBeforeFrames /
                                  engineTrackFrames) < 1e-9,
                    3,
                    &observation);
            check(std::abs(observation.rendererTruePosSample -
                          kMarkerSourceFrame * kChannels) < 1e-9,
                    4,
                    &observation);
            check(std::abs(observation.rendererMarkerPixel -
                          kExpectedMarkerPixel) < 1e-9,
                    4,
                    &observation);
            check(std::abs(observation.rendererNeighbourPixel -
                          (kExpectedMarkerPixel +
                                  kMarkerNeighbourFrames)) < 1e-9,
                    4,
                    &observation);
        }
    }

    if (!markerObserved) {
        allChecksPassed = false;
        if (firstDivergentClock == 0) {
            firstDivergentClock = 1;
        }
    }

    if (!allChecksPassed) {
        for (std::size_t i = 0; i < observationCount; ++i) {
            if (observations[i].firstDivergentClock == 0) {
                observations[i].firstDivergentClock = firstDivergentClock;
            }
        }
        writeFailureTrace(observations.data(), observationCount, sourceMarkerOccurrences);
    }

    EXPECT_TRUE(allChecksPassed)
            << "first divergent clock=" << firstDivergentClock
            << "; failure trace=" << kTracePath.toStdString();
}

TEST_F(EngineBufferAlignmentTest, ProcessRecoversAfterReadAheadLogCapacity) {
    constexpr int kSetupStartFrame = 128;
    constexpr double kTempoRatio = 1.25;
    constexpr std::size_t kReadLogCapacity =
            ReadAheadManager::kMaxReadAheadLogEntries + 2;
    constexpr SINT kForwardMappingSamples = kChannels * 2;
    constexpr SINT kReverseMappingSamples = kChannels;
    constexpr int kFallbackCallbackFrames = kBufferFrames * 2;
    constexpr int kFallbackCallbackSamples =
            kFallbackCallbackFrames * kChannels;
    constexpr CSAMPLE kStaleSample = -12345.0f;

    // Existing alignment tests use the normal waveform fixture. This test
    // temporarily selects a separate source that labels every reader call so
    // recovery output can be tied to the exact read that produced it.
    RecoverySourceScope recoverySourceScope(&g_readerContext);

    TrackPointer track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::kEngineChannelOutputCount,
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));

    // Keep this callback test on the linear scaler. Disabling keylock while
    // keeping pitch and tempo equal selects it independently of optional
    // time-stretching engines.
    configureAlignmentControls(
            m_sGroup1, EngineBuffer::KeylockEngine::SoundTouch, kTempoRatio);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock")), 0.0);
    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    pEngineBuffer->loadFakeTrack(track, false);
    pEngineBuffer->seekExact(mixxx::audio::FramePos(kSetupStartFrame));
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);

    g_recoverySource.resetReadObservations();
    std::array<CSAMPLE, kBufferSamples> primeOutput{};

    // Prime the actual EngineBuffer/scaler once. The linear scaler retains its
    // first read-ahead mapping internally; the manager then starts with one
    // mapping that can be followed by the bounded alternating setup below.
    const std::size_t primeReadStart = g_recoverySource.readObservationCount;
    pEngineBuffer->process(primeOutput.data(), primeOutput.size());
    pEngineBuffer->postProcess(primeOutput.size());

    ReadAheadManager* const pReadAheadManager =
            pEngineBuffer->m_pReadAheadManager;
    ASSERT_EQ(pEngineBuffer->m_pScale,
            static_cast<EngineBufferScale*>(pEngineBuffer->m_pScaleLinear));
    ASSERT_EQ(1u, pReadAheadManager->m_readAheadLogSize);
    ASSERT_EQ(0u, pReadAheadManager->m_readAheadLogOverflowSize);
    ASSERT_TRUE(std::all_of(
            primeOutput.begin(),
            primeOutput.end(),
            [](CSAMPLE sample) { return sample != kStaleSample; }));
    const std::size_t primeRead = findGenerationMarkedRead(
            g_recoverySource, primeReadStart, primeOutput);
    ASSERT_NE(std::numeric_limits<std::size_t>::max(), primeRead);
    const auto& primeObservation = g_recoverySource.readObservations[primeRead];

    // Alternate directions so every request is a distinct mapping. Starting
    // opposite the primed forward entry and ending reverse ensures the next
    // forward scaler refill cannot merge with either spill entry.
    const bool primedForward = pReadAheadManager
                                       ->m_readAheadLog[pReadAheadManager->m_readAheadLogStart]
                                       .direction();
    ASSERT_TRUE(primedForward);
    bool reverse = !primedForward;
    std::array<CSAMPLE, kForwardMappingSamples> mappingBuffer{};
    for (std::size_t mapping = 0; mapping < kReadLogCapacity; ++mapping) {
        const std::size_t occupiedEntries =
                pReadAheadManager->m_readAheadLogSize +
                pReadAheadManager->m_readAheadLogOverflowSize;
        const SINT requestedSamples =
                reverse ? kReverseMappingSamples : kForwardMappingSamples;
        std::fill(mappingBuffer.begin(), mappingBuffer.end(), kStaleSample);
        ASSERT_EQ(requestedSamples,
                pReadAheadManager->getNextSamples(
                        reverse ? -1.0 : 1.0,
                        mappingBuffer.data(),
                        requestedSamples,
                        mixxx::audio::ChannelCount::stereo()));
        ASSERT_TRUE(std::all_of(
                mappingBuffer.begin(),
                mappingBuffer.begin() + requestedSamples,
                [](CSAMPLE sample) { return sample != kStaleSample; }));
        ASSERT_EQ(occupiedEntries + 1,
                pReadAheadManager->m_readAheadLogSize +
                        pReadAheadManager->m_readAheadLogOverflowSize);
        reverse = !reverse;
        if (pReadAheadManager->m_readAheadLogSize ==
                        ReadAheadManager::kMaxReadAheadLogEntries &&
                pReadAheadManager->m_readAheadLogOverflowSize == 2) {
            break;
        }
    }

    ASSERT_EQ(ReadAheadManager::kMaxReadAheadLogEntries,
            pReadAheadManager->m_readAheadLogSize);
    ASSERT_EQ(2u, pReadAheadManager->m_readAheadLogOverflowSize);
    ASSERT_FALSE(pReadAheadManager
                    ->m_readAheadLogOverflow[1]
                    .direction());

    const std::size_t oldestLogStart = pReadAheadManager->m_readAheadLogStart;
    const double oldestMappingFrames =
            pReadAheadManager->m_readAheadLog[oldestLogStart].length() /
            kChannels;
    const double readAheadPositionBefore = pReadAheadManager->getPlaypos();
    const double playPosBefore = pEngineBuffer->getPlayPos().value();
    const std::size_t readsBeforeFallback = g_recoverySource.readObservationCount;

    std::array<CSAMPLE, kFallbackCallbackSamples> blockedOutput{};
    std::fill(blockedOutput.begin(), blockedOutput.end(), kStaleSample);
    // This is the real callback path while the manager is full. The forward
    // linear refill is non-mergeable with the reverse final spill mapping, so
    // capacity fallback must return zero without calling the reader.
    pEngineBuffer->process(blockedOutput.data(), blockedOutput.size());
    pEngineBuffer->postProcess(blockedOutput.size());

    const double playPosAfterFallback = pEngineBuffer->getPlayPos().value();
    EXPECT_TRUE(std::all_of(
            blockedOutput.begin(),
            blockedOutput.end(),
            [](CSAMPLE sample) { return sample != kStaleSample; }));
    EXPECT_TRUE(std::all_of(
            blockedOutput.begin(),
            blockedOutput.end(),
            [](CSAMPLE sample) { return sample == 0.0f; }));
    EXPECT_GT(playPosAfterFallback, playPosBefore);
    EXPECT_GT(playPosAfterFallback - playPosBefore, oldestMappingFrames);
    EXPECT_GT(pReadAheadManager->m_readAheadLogStart, oldestLogStart);
    EXPECT_LT(pReadAheadManager->m_readAheadLogSize +
                    pReadAheadManager->m_readAheadLogOverflowSize,
            kReadLogCapacity);
    EXPECT_DOUBLE_EQ(readAheadPositionBefore,
            pReadAheadManager->getPlaypos());
    EXPECT_EQ(readsBeforeFallback, g_recoverySource.readObservationCount);

    const std::size_t readsBeforeRecovery = g_recoverySource.readObservationCount;
    std::array<CSAMPLE, kFallbackCallbackSamples> recoveryOutput{};
    std::fill(recoveryOutput.begin(), recoveryOutput.end(), kStaleSample);
    const std::size_t recoveryReadStart = g_recoverySource.readObservationCount;
    pEngineBuffer->process(recoveryOutput.data(), recoveryOutput.size());
    pEngineBuffer->postProcess(recoveryOutput.size());

    ASSERT_GT(g_recoverySource.readObservationCount, readsBeforeRecovery);
    ASSERT_TRUE(std::all_of(
            recoveryOutput.begin(),
            recoveryOutput.end(),
            [](CSAMPLE sample) { return sample != kStaleSample; }));
    const std::size_t recoveryRead = findGenerationMarkedRead(
            g_recoverySource, recoveryReadStart, recoveryOutput);
    ASSERT_NE(std::numeric_limits<std::size_t>::max(), recoveryRead);
    const auto& recoveryObservation =
            g_recoverySource.readObservations[recoveryRead];
    EXPECT_GT(recoveryObservation.generation, primeObservation.generation);
    EXPECT_FALSE(recoveryObservation.reverse);
    EXPECT_EQ(recoveryObservation.startSample,
            static_cast<SINT>(readAheadPositionBefore));
    EXPECT_GT(recoveryObservation.numSamples, 0);
    EXPECT_TRUE(containsGenerationMarker(
            recoveryOutput, recoveryObservation.generation));
    EXPECT_FALSE(containsGenerationMarker(
            recoveryOutput, primeObservation.generation));
    EXPECT_FALSE(std::all_of(
            recoveryOutput.begin(),
            recoveryOutput.end(),
            [](CSAMPLE sample) { return sample == 0.0f; }));
    EXPECT_FALSE(std::equal(
            recoveryOutput.begin(), recoveryOutput.end(), blockedOutput.begin()));
    EXPECT_FALSE(std::equal(
            recoveryOutput.begin(),
            recoveryOutput.begin() + kBufferSamples,
            primeOutput.begin()));
}

#ifdef __SIGNALSMITH__
TEST_F(EngineBufferAlignmentTest, SignalSmithEngineSelectedAndProcesses) {
    TrackPointer track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::kEngineChannelOutputCount,
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));

    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::SignalSmith));
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rateSearch")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate_dir")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate_ratio")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("pitch")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("pitch_adjust")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("reverse")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("slip_enabled")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("repeat")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("passthrough")), 0.0);

    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    pEngineBuffer->loadFakeTrack(track, false);
    pEngineBuffer->seekExact(mixxx::audio::kStartFramePos);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);
    std::array<CSAMPLE, kBufferSamples> output{};
    pEngineBuffer->process(output.data(), kBufferSamples);
    pEngineBuffer->postProcess(kBufferSamples);

    EXPECT_EQ(pEngineBuffer->m_pScaleSignalSmith,
            pEngineBuffer->m_pScaleKeylock.loadAcquire());
    EXPECT_EQ(pEngineBuffer->m_pScaleSignalSmith, pEngineBuffer->m_pScale);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](CSAMPLE sample) {
        return std::isfinite(sample);
    }));
}

TEST_F(EngineBufferAlignmentTest, SignalSmithEngineMarkerTracksEnginePosition) {
    TrackPointer track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::kEngineChannelOutputCount,
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));

    ControlObject::set(ConfigKey(QStringLiteral("[App]"),
                               QStringLiteral("samplerate")),
            kSampleRate);

    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::SignalSmith));
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rateSearch")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate_dir")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate_ratio")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("pitch")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("pitch_adjust")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("reverse")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("slip_enabled")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("repeat")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("passthrough")), 0.0);

    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    pEngineBuffer->loadFakeTrack(track, false);
    pEngineBuffer->seekExact(mixxx::audio::kStartFramePos);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);
    const double engineTrackFrames = pEngineBuffer->getTrackEndPosition().value();

    std::array<CSAMPLE, kBufferSamples> output{};
    std::vector<CSAMPLE> emitted;
    emitted.reserve(kBufferSamples * 80);
    std::array<double, 80> playPositionsBefore{};
    std::array<double, 80> playPositionsAfter{};
    const auto visualPlayPosition =
            VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    FixedVSyncProvider vsync;
    WaveformWidgetRenderer renderer(m_sGroup1);
    const bool rendererInitialized = renderer.init();
    renderer.setTrack(track);
    renderer.resizeRenderer(kRendererWidth, 100, 1.0f);
    std::array<double, 80> visualPlayPositionsBefore{};
    std::array<double, 80> visualVSyncPositionsBefore{};
    std::array<double, 80> rendererPositionsBefore{};
    std::array<double, 80> requestedOutputFrames{};
    std::array<double, 80> returnedSourceFrames{};
    std::array<double, 80> readAheadStartFrames{};
    std::array<double, 80> readAheadEndFrames{};
    std::array<std::size_t, 80> readAheadObservationCounts{};
    double maximumOutput = 0.0;
    g_source.resetReadObservations();
    for (int callback = 0; callback < 80; ++callback) {
        playPositionsBefore[callback] = pEngineBuffer->getPlayPos().value();
        visualPlayPositionsBefore[callback] =
                visualPlayPosition->getEnginePlayPos() * engineTrackFrames;
        visualVSyncPositionsBefore[callback] =
                visualPlayPosition->getAtNextVSync(&vsync) * engineTrackFrames;
        if (rendererInitialized && visualPlayPosition->isValid()) {
            renderer.onPreRender(&vsync);
            rendererPositionsBefore[callback] = renderer.getTruePosSample();
        }
        pEngineBuffer->process(output.data(), kBufferSamples);
        pEngineBuffer->postProcess(kBufferSamples);
        playPositionsAfter[callback] = pEngineBuffer->getPlayPos().value();
        requestedOutputFrames[callback] = kBufferFrames;
        returnedSourceFrames[callback] =
                playPositionsAfter[callback] - playPositionsBefore[callback];
        const ReadAheadSnapshot readAhead = latestReadAheadSnapshot(g_source);
        readAheadStartFrames[callback] = readAhead.startFrames;
        readAheadEndFrames[callback] = readAhead.endFrames;
        readAheadObservationCounts[callback] = readAhead.observationCount;
        emitted.insert(emitted.end(), output.begin(), output.end());
        for (CSAMPLE sample : output) {
            maximumOutput = std::max(maximumOutput,
                    std::abs(static_cast<double>(sample)));
        }
    }

    const MarkerSimilarity bestSimilarity =
            findBestEngineMarkerSimilarity(emitted);
    const int bestCallback = bestSimilarity.outputFrame >= 0
            ? bestSimilarity.outputFrame / kBufferFrames
            : -1;
    const int markerOutputFrame = bestSimilarity.outputFrame >= 0
            ? bestSimilarity.outputFrame % kBufferFrames
            : -1;
    const double markerPlayPosBefore = bestCallback >= 0 && bestCallback < 80
            ? playPositionsBefore[bestCallback]
            : 0.0;
    const double markerPlayPosAfter = bestCallback >= 0 && bestCallback < 80
            ? playPositionsAfter[bestCallback]
            : 0.0;
    const double markerVisualPlayPosBefore = bestCallback >= 0 && bestCallback < 80
            ? visualPlayPositionsBefore[bestCallback]
            : 0.0;
    const double markerVisualVSyncPosBefore = bestCallback >= 0 && bestCallback < 80
            ? visualVSyncPositionsBefore[bestCallback]
            : 0.0;
    const double markerRendererPosBefore = bestCallback >= 0 && bestCallback < 80
            ? rendererPositionsBefore[bestCallback]
            : 0.0;
    const double markerRequestedOutputFrames = bestCallback >= 0 && bestCallback < 80
            ? requestedOutputFrames[bestCallback]
            : 0.0;
    const double markerReturnedSourceFrames = bestCallback >= 0 && bestCallback < 80
            ? returnedSourceFrames[bestCallback]
            : 0.0;
    const double markerReadAheadStartFrames = bestCallback >= 0 && bestCallback < 80
            ? readAheadStartFrames[bestCallback]
            : 0.0;
    const double markerReadAheadEndFrames = bestCallback >= 0 && bestCallback < 80
            ? readAheadEndFrames[bestCallback]
            : 0.0;
    const std::size_t markerReadAheadObservationCount = bestCallback >= 0 && bestCallback < 80
            ? readAheadObservationCounts[bestCallback]
            : 0;
    const double markerPixel = renderer.transformSamplePositionInRendererWorld(
            static_cast<double>(kEngineMarkerSourceFrame * kChannels));
    const double rendererPlayheadPixel = renderer.transformSamplePositionInRendererWorld(
            markerRendererPosBefore);
    const bool markerObserved = bestSimilarity.outputFrame >= 0 &&
            bestSimilarity.correlation >= 0.3;
    const double bestPlayPos = markerPlayPosBefore;
    const int firstDivergentClock = !markerObserved
            ? 1
            : (std::abs(kEngineMarkerSourceFrame -
                       (markerVisualPlayPosBefore + markerOutputFrame)) > 2.0
                              ? 3
                              : (std::abs(markerRendererPosBefore -
                                         markerVisualVSyncPosBefore * kChannels) > 1e-9
                                                ? 4
                                                : 0));

    if (!markerObserved) {
        writeEngineMarkerFailureTrace(
                "SignalSmith",
                bestSimilarity.correlation,
                bestSimilarity.normalizedError,
                bestSimilarity.outputFrame,
                bestCallback,
                markerPlayPosBefore,
                markerPlayPosAfter,
                markerVisualPlayPosBefore,
                markerVisualVSyncPosBefore,
                markerRendererPosBefore,
                markerPixel,
                rendererPlayheadPixel,
                markerRequestedOutputFrames,
                markerReturnedSourceFrames,
                markerReadAheadStartFrames,
                markerReadAheadEndFrames,
                markerReadAheadObservationCount,
                firstDivergentClock,
                maximumOutput);
    }
    ASSERT_TRUE(markerObserved)
            << "SignalSmith marker was not emitted in 80 callbacks; best correlation="
            << bestSimilarity.correlation
            << ", normalized marker error=" << bestSimilarity.normalizedError
            << " at callback=" << bestCallback
            << " play position=" << bestPlayPos
            << " at output frame=" << bestSimilarity.outputFrame
            << ", maximum output=" << maximumOutput;
    const bool markerPositionAligned =
            std::abs(kEngineMarkerSourceFrame -
                    (markerVisualPlayPosBefore + markerOutputFrame)) <= 2.0;
    if (!markerPositionAligned) {
        writeEngineMarkerFailureTrace(
                "SignalSmith",
                bestSimilarity.correlation,
                bestSimilarity.normalizedError,
                bestSimilarity.outputFrame,
                bestCallback,
                markerPlayPosBefore,
                markerPlayPosAfter,
                markerVisualPlayPosBefore,
                markerVisualVSyncPosBefore,
                markerRendererPosBefore,
                markerPixel,
                rendererPlayheadPixel,
                markerRequestedOutputFrames,
                markerReturnedSourceFrames,
                markerReadAheadStartFrames,
                markerReadAheadEndFrames,
                markerReadAheadObservationCount,
                firstDivergentClock,
                maximumOutput);
    }
    EXPECT_NEAR(kEngineMarkerSourceFrame,
            markerVisualPlayPosBefore + markerOutputFrame,
            2.0)
            << "marker output frame=" << markerOutputFrame
            << " play position before=" << markerPlayPosBefore
            << " play position after=" << markerPlayPosAfter
            << " visual play position before=" << markerVisualPlayPosBefore;
    EXPECT_NEAR(kBufferFrames,
            markerPlayPosAfter - markerPlayPosBefore,
            0.5);
    ASSERT_TRUE(rendererInitialized);
    EXPECT_NEAR(markerVisualPlayPosBefore + kVSyncOffsetFrames,
            markerVisualVSyncPosBefore,
            1e-9);
    EXPECT_NEAR(markerVisualVSyncPosBefore * kChannels,
            markerRendererPosBefore,
            1e-9);
    const double markerNeighbourPixel = renderer.transformSamplePositionInRendererWorld(
            static_cast<double>((kEngineMarkerSourceFrame + kMarkerNeighbourFrames) *
                    kChannels));
    EXPECT_NEAR(markerNeighbourPixel - markerPixel,
            kMarkerNeighbourFrames / renderer.getAudioSamplePerPixel(),
            1e-9);
    const MarkerPlayheadPixelResult markerPixelReplay =
            replayMarkerAtSyntheticVSync(
                    pEngineBuffer,
                    m_sGroup1,
                    track,
                    bestCallback,
                    markerOutputFrame);
    ASSERT_TRUE(markerPixelReplay.rendererInitialized);
    if (std::abs(markerPixelReplay.playheadPixel -
                markerPixelReplay.markerPixel) > 1.0) {
        writeEngineMarkerFailureTrace(
                "SignalSmith",
                bestSimilarity.correlation,
                bestSimilarity.normalizedError,
                bestSimilarity.outputFrame,
                bestCallback,
                markerPlayPosBefore,
                markerPlayPosAfter,
                markerVisualPlayPosBefore,
                markerVisualVSyncPosBefore,
                markerPixelReplay.playheadSample,
                markerPixelReplay.markerPixel,
                markerPixelReplay.playheadPixel,
                markerRequestedOutputFrames,
                markerReturnedSourceFrames,
                markerReadAheadStartFrames,
                markerReadAheadEndFrames,
                markerReadAheadObservationCount,
                4,
                maximumOutput);
    }
    EXPECT_NEAR(markerPixelReplay.playheadPixel,
            markerPixelReplay.markerPixel,
            1.0);
}

TEST_F(EngineBufferAlignmentTest, SignalSmithStretchedMarkerTracksEnginePosition) {
    TrackPointer track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::kEngineChannelOutputCount,
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));

    constexpr double kRateRatio = 1.25;
    configureAlignmentControls(
            m_sGroup1,
            EngineBuffer::KeylockEngine::SignalSmith,
            kRateRatio);
    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    const StretchedMarkerProbeResult probe = runStretchedMarkerProbe(
            pEngineBuffer,
            m_sGroup1,
            track,
            kRateRatio,
            kSignalSmithMarkerOnsetThreshold);
    if (probe.callbackIndex < 0 || probe.similarity.correlation < 0.3) {
        writeEngineMarkerFailureTrace(
                "SignalSmith",
                probe.similarity.correlation,
                probe.similarity.normalizedError,
                probe.similarity.outputFrame,
                probe.callbackIndex,
                probe.playPosBeforeFrames,
                probe.playPosAfterFrames,
                probe.visualPlayPosBeforeFrames,
                probe.visualVSyncPosBeforeFrames,
                0.0,
                0.0,
                0.0,
                probe.requestedOutputFrames,
                probe.returnedSourceFrames,
                probe.readAheadStartFrames,
                probe.readAheadEndFrames,
                probe.readAheadObservationCount,
                1,
                probe.maximumOutput,
                "SignalSmithStretchedMarkerTracksEnginePosition");
    }
    ASSERT_GE(probe.callbackIndex, 0);
    ASSERT_GE(probe.similarity.correlation, 0.3)
            << "SignalSmith stretched marker was not emitted; correlation="
            << probe.similarity.correlation
            << ", normalized error=" << probe.similarity.normalizedError
            << ", output frame=" << probe.similarity.outputFrame
            << ", maximum output=" << probe.maximumOutput;

    const int markerOutputFrame = probe.similarity.outputFrame >= 0
            ? probe.similarity.outputFrame % kBufferFrames
            : -1;
    qDebug() << "SignalSmith stretched probe" << probe.callbackIndex
             << probe.similarity.outputFrame << markerOutputFrame
             << probe.visualPlayPosBeforeFrames << probe.effectiveRate
             << probe.similarity.correlation;
    EXPECT_NEAR(kEngineMarkerSourceFrame,
            probe.visualPlayPosBeforeFrames +
                    markerOutputFrame * probe.effectiveRate,
            2.0);
    EXPECT_NEAR(kBufferFrames * probe.effectiveRate,
            probe.returnedSourceFrames,
            0.5);
    EXPECT_DOUBLE_EQ(kBufferFrames, probe.requestedOutputFrames);
    EXPECT_GT(probe.readAheadObservationCount, 0u);
    EXPECT_GE(probe.readAheadEndFrames, probe.readAheadStartFrames);
    EXPECT_NEAR(probe.visualPlayPosBeforeFrames +
                    kVSyncOffsetFrames * probe.effectiveRate,
            probe.visualVSyncPosBeforeFrames,
            1e-9);

    const MarkerPlayheadPixelResult pixel = replayMarkerAtSyntheticVSync(
            pEngineBuffer,
            m_sGroup1,
            track,
            probe.callbackIndex,
            markerOutputFrame);
    ASSERT_TRUE(pixel.rendererInitialized);
    const bool stretchedAlignmentPassed =
            std::abs(kEngineMarkerSourceFrame -
                    (probe.visualPlayPosBeforeFrames +
                            markerOutputFrame * probe.effectiveRate)) <= 2.0 &&
            std::abs(pixel.playheadPixel - pixel.markerPixel) <= 1.0;
    if (!stretchedAlignmentPassed) {
        writeEngineMarkerFailureTrace(
                "SignalSmith",
                probe.similarity.correlation,
                probe.similarity.normalizedError,
                probe.similarity.outputFrame,
                probe.callbackIndex,
                probe.playPosBeforeFrames,
                probe.playPosAfterFrames,
                probe.visualPlayPosBeforeFrames,
                probe.visualVSyncPosBeforeFrames,
                pixel.playheadSample,
                pixel.markerPixel,
                pixel.playheadPixel,
                probe.requestedOutputFrames,
                probe.returnedSourceFrames,
                probe.readAheadStartFrames,
                probe.readAheadEndFrames,
                probe.readAheadObservationCount,
                3,
                probe.maximumOutput,
                "SignalSmithStretchedMarkerTracksEnginePosition");
    }
    EXPECT_NEAR(pixel.playheadPixel, pixel.markerPixel, 1.0);
}
#endif

#ifdef __BUNGEE__
TEST_F(EngineBufferAlignmentTest, BungeeEngineMarkerTracksEnginePosition) {
    TrackPointer track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::kEngineChannelOutputCount,
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));

    ControlObject::set(ConfigKey(QStringLiteral("[App]"),
                               QStringLiteral("samplerate")),
            kSampleRate);

    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock_engine")),
            static_cast<double>(EngineBuffer::KeylockEngine::Bungee));
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rateSearch")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate_dir")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("rate_ratio")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("pitch")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("pitch_adjust")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("keylock")), 1.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("reverse")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("slip_enabled")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("repeat")), 0.0);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("passthrough")), 0.0);

    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    pEngineBuffer->loadFakeTrack(track, false);
    pEngineBuffer->seekExact(mixxx::audio::kStartFramePos);
    ControlObject::set(ConfigKey(m_sGroup1, QStringLiteral("play")), 1.0);
    const double engineTrackFrames = pEngineBuffer->getTrackEndPosition().value();

    std::array<CSAMPLE, kBufferSamples> output{};
    std::vector<CSAMPLE> emitted;
    emitted.reserve(kBufferSamples * 80);
    std::array<double, 80> playPositionsBefore{};
    std::array<double, 80> playPositionsAfter{};
    const auto visualPlayPosition =
            VisualPlayPosition::getVisualPlayPosition(m_sGroup1);
    FixedVSyncProvider vsync;
    WaveformWidgetRenderer renderer(m_sGroup1);
    const bool rendererInitialized = renderer.init();
    renderer.setTrack(track);
    renderer.resizeRenderer(kRendererWidth, 100, 1.0f);
    std::array<double, 80> visualPlayPositionsBefore{};
    std::array<double, 80> visualVSyncPositionsBefore{};
    std::array<double, 80> rendererPositionsBefore{};
    std::array<double, 80> requestedOutputFrames{};
    std::array<double, 80> returnedSourceFrames{};
    std::array<double, 80> readAheadStartFrames{};
    std::array<double, 80> readAheadEndFrames{};
    std::array<std::size_t, 80> readAheadObservationCounts{};
    double maximumOutput = 0.0;
    g_source.resetReadObservations();
    for (int callback = 0; callback < 80; ++callback) {
        playPositionsBefore[callback] = pEngineBuffer->getPlayPos().value();
        visualPlayPositionsBefore[callback] =
                visualPlayPosition->getEnginePlayPos() * engineTrackFrames;
        visualVSyncPositionsBefore[callback] =
                visualPlayPosition->getAtNextVSync(&vsync) * engineTrackFrames;
        if (rendererInitialized && visualPlayPosition->isValid()) {
            renderer.onPreRender(&vsync);
            rendererPositionsBefore[callback] = renderer.getTruePosSample();
        }
        pEngineBuffer->process(output.data(), kBufferSamples);
        pEngineBuffer->postProcess(kBufferSamples);
        playPositionsAfter[callback] = pEngineBuffer->getPlayPos().value();
        requestedOutputFrames[callback] = kBufferFrames;
        returnedSourceFrames[callback] =
                playPositionsAfter[callback] - playPositionsBefore[callback];
        const ReadAheadSnapshot readAhead = latestReadAheadSnapshot(g_source);
        readAheadStartFrames[callback] = readAhead.startFrames;
        readAheadEndFrames[callback] = readAhead.endFrames;
        readAheadObservationCounts[callback] = readAhead.observationCount;
        emitted.insert(emitted.end(), output.begin(), output.end());
        for (CSAMPLE sample : output) {
            maximumOutput = std::max(maximumOutput,
                    std::abs(static_cast<double>(sample)));
        }
    }

    const MarkerSimilarity bestSimilarity =
            findBestEngineMarkerSimilarity(emitted);
    const int bestCallback = bestSimilarity.outputFrame >= 0
            ? bestSimilarity.outputFrame / kBufferFrames
            : -1;
    const int markerOutputFrame = bestSimilarity.outputFrame >= 0
            ? bestSimilarity.outputFrame % kBufferFrames
            : -1;
    const double markerPlayPosBefore = bestCallback >= 0 && bestCallback < 80
            ? playPositionsBefore[bestCallback]
            : 0.0;
    const double markerPlayPosAfter = bestCallback >= 0 && bestCallback < 80
            ? playPositionsAfter[bestCallback]
            : 0.0;
    const double markerVisualPlayPosBefore = bestCallback >= 0 && bestCallback < 80
            ? visualPlayPositionsBefore[bestCallback]
            : 0.0;
    const double markerVisualVSyncPosBefore = bestCallback >= 0 && bestCallback < 80
            ? visualVSyncPositionsBefore[bestCallback]
            : 0.0;
    const double markerRendererPosBefore = bestCallback >= 0 && bestCallback < 80
            ? rendererPositionsBefore[bestCallback]
            : 0.0;
    const double markerRequestedOutputFrames = bestCallback >= 0 && bestCallback < 80
            ? requestedOutputFrames[bestCallback]
            : 0.0;
    const double markerReturnedSourceFrames = bestCallback >= 0 && bestCallback < 80
            ? returnedSourceFrames[bestCallback]
            : 0.0;
    const double markerReadAheadStartFrames = bestCallback >= 0 && bestCallback < 80
            ? readAheadStartFrames[bestCallback]
            : 0.0;
    const double markerReadAheadEndFrames = bestCallback >= 0 && bestCallback < 80
            ? readAheadEndFrames[bestCallback]
            : 0.0;
    const std::size_t markerReadAheadObservationCount = bestCallback >= 0 && bestCallback < 80
            ? readAheadObservationCounts[bestCallback]
            : 0;
    const double markerPixel = renderer.transformSamplePositionInRendererWorld(
            static_cast<double>(kEngineMarkerSourceFrame * kChannels));
    const double rendererPlayheadPixel = renderer.transformSamplePositionInRendererWorld(
            markerRendererPosBefore);
    const bool markerObserved = bestSimilarity.outputFrame >= 0 &&
            bestSimilarity.correlation >= 0.3;
    const double bestPlayPos = markerPlayPosBefore;
    const int firstDivergentClock = !markerObserved
            ? 1
            : (std::abs(kEngineMarkerSourceFrame -
                       (markerVisualPlayPosBefore + markerOutputFrame)) > 2.0
                              ? 3
                              : (std::abs(markerRendererPosBefore -
                                         markerVisualVSyncPosBefore * kChannels) > 1e-9
                                                ? 4
                                                : 0));

    if (!markerObserved) {
        writeEngineMarkerFailureTrace(
                "Bungee",
                bestSimilarity.correlation,
                bestSimilarity.normalizedError,
                bestSimilarity.outputFrame,
                bestCallback,
                markerPlayPosBefore,
                markerPlayPosAfter,
                markerVisualPlayPosBefore,
                markerVisualVSyncPosBefore,
                markerRendererPosBefore,
                markerPixel,
                rendererPlayheadPixel,
                markerRequestedOutputFrames,
                markerReturnedSourceFrames,
                markerReadAheadStartFrames,
                markerReadAheadEndFrames,
                markerReadAheadObservationCount,
                firstDivergentClock,
                maximumOutput);
    }
    ASSERT_TRUE(markerObserved)
            << "Bungee marker was not emitted in 80 callbacks; best correlation="
            << bestSimilarity.correlation
            << ", normalized marker error=" << bestSimilarity.normalizedError
            << " at callback=" << bestCallback
            << " play position=" << bestPlayPos
            << " at output frame=" << bestSimilarity.outputFrame
            << ", maximum output=" << maximumOutput;
    const bool markerPositionAligned =
            std::abs(kEngineMarkerSourceFrame -
                    (markerVisualPlayPosBefore + markerOutputFrame)) <= 2.0;
    if (!markerPositionAligned) {
        writeEngineMarkerFailureTrace(
                "Bungee",
                bestSimilarity.correlation,
                bestSimilarity.normalizedError,
                bestSimilarity.outputFrame,
                bestCallback,
                markerPlayPosBefore,
                markerPlayPosAfter,
                markerVisualPlayPosBefore,
                markerVisualVSyncPosBefore,
                markerRendererPosBefore,
                markerPixel,
                rendererPlayheadPixel,
                markerRequestedOutputFrames,
                markerReturnedSourceFrames,
                markerReadAheadStartFrames,
                markerReadAheadEndFrames,
                markerReadAheadObservationCount,
                firstDivergentClock,
                maximumOutput);
    }
    EXPECT_NEAR(kEngineMarkerSourceFrame,
            markerVisualPlayPosBefore + markerOutputFrame,
            2.0)
            << "marker output frame=" << markerOutputFrame
            << " play position before=" << markerPlayPosBefore
            << " play position after=" << markerPlayPosAfter
            << " visual play position before=" << markerVisualPlayPosBefore;
    EXPECT_NEAR(kBufferFrames,
            markerPlayPosAfter - markerPlayPosBefore,
            0.5);
    ASSERT_TRUE(rendererInitialized);
    EXPECT_NEAR(markerVisualPlayPosBefore + kVSyncOffsetFrames,
            markerVisualVSyncPosBefore,
            1e-9);
    EXPECT_NEAR(markerVisualVSyncPosBefore * kChannels,
            markerRendererPosBefore,
            1e-9);
    const double markerNeighbourPixel = renderer.transformSamplePositionInRendererWorld(
            static_cast<double>((kEngineMarkerSourceFrame + kMarkerNeighbourFrames) *
                    kChannels));
    EXPECT_NEAR(markerNeighbourPixel - markerPixel,
            kMarkerNeighbourFrames / renderer.getAudioSamplePerPixel(),
            1e-9);
    const MarkerPlayheadPixelResult markerPixelReplay =
            replayMarkerAtSyntheticVSync(
                    pEngineBuffer,
                    m_sGroup1,
                    track,
                    bestCallback,
                    markerOutputFrame);
    ASSERT_TRUE(markerPixelReplay.rendererInitialized);
    if (std::abs(markerPixelReplay.playheadPixel -
                markerPixelReplay.markerPixel) > 1.0) {
        writeEngineMarkerFailureTrace(
                "Bungee",
                bestSimilarity.correlation,
                bestSimilarity.normalizedError,
                bestSimilarity.outputFrame,
                bestCallback,
                markerPlayPosBefore,
                markerPlayPosAfter,
                markerVisualPlayPosBefore,
                markerVisualVSyncPosBefore,
                markerPixelReplay.playheadSample,
                markerPixelReplay.markerPixel,
                markerPixelReplay.playheadPixel,
                markerRequestedOutputFrames,
                markerReturnedSourceFrames,
                markerReadAheadStartFrames,
                markerReadAheadEndFrames,
                markerReadAheadObservationCount,
                4,
                maximumOutput);
    }
    EXPECT_NEAR(markerPixelReplay.playheadPixel,
            markerPixelReplay.markerPixel,
            1.0);
}

TEST_F(EngineBufferAlignmentTest, BungeeStretchedMarkerTracksEnginePosition) {
    TrackPointer track = Track::newTemporary();
    track->setAudioProperties(
            mixxx::kEngineChannelOutputCount,
            mixxx::audio::SampleRate(kSampleRate),
            mixxx::audio::Bitrate(),
            mixxx::Duration::fromSeconds(kTrackSeconds));

    constexpr double kRateRatio = 1.25;
    configureAlignmentControls(
            m_sGroup1,
            EngineBuffer::KeylockEngine::Bungee,
            kRateRatio);
    EngineBuffer* const pEngineBuffer = m_pChannel1->getEngineBuffer();
    const StretchedMarkerProbeResult probe = runStretchedMarkerProbe(
            pEngineBuffer,
            m_sGroup1,
            track,
            kRateRatio,
            kBungeeMarkerOnsetThreshold);
    if (probe.callbackIndex < 0 || probe.similarity.correlation < 0.3) {
        writeEngineMarkerFailureTrace(
                "Bungee",
                probe.similarity.correlation,
                probe.similarity.normalizedError,
                probe.similarity.outputFrame,
                probe.callbackIndex,
                probe.playPosBeforeFrames,
                probe.playPosAfterFrames,
                probe.visualPlayPosBeforeFrames,
                probe.visualVSyncPosBeforeFrames,
                0.0,
                0.0,
                0.0,
                probe.requestedOutputFrames,
                probe.returnedSourceFrames,
                probe.readAheadStartFrames,
                probe.readAheadEndFrames,
                probe.readAheadObservationCount,
                1,
                probe.maximumOutput,
                "BungeeStretchedMarkerTracksEnginePosition");
    }
    ASSERT_GE(probe.callbackIndex, 0);
    ASSERT_GE(probe.similarity.correlation, 0.3)
            << "Bungee stretched marker was not emitted; correlation="
            << probe.similarity.correlation
            << ", normalized error=" << probe.similarity.normalizedError
            << ", output frame=" << probe.similarity.outputFrame
            << ", maximum output=" << probe.maximumOutput;

    const int markerOutputFrame = probe.similarity.outputFrame >= 0
            ? probe.similarity.outputFrame % kBufferFrames
            : -1;
    qDebug() << "Bungee stretched probe" << probe.callbackIndex
             << probe.similarity.outputFrame << markerOutputFrame
             << probe.visualPlayPosBeforeFrames << probe.effectiveRate
             << probe.similarity.correlation;
    EXPECT_NEAR(kEngineMarkerSourceFrame,
            probe.visualPlayPosBeforeFrames +
                    markerOutputFrame * probe.effectiveRate,
            2.0);
    EXPECT_NEAR(kBufferFrames * probe.effectiveRate,
            probe.returnedSourceFrames,
            0.5);
    EXPECT_DOUBLE_EQ(kBufferFrames, probe.requestedOutputFrames);
    EXPECT_GT(probe.readAheadObservationCount, 0u);
    EXPECT_GE(probe.readAheadEndFrames, probe.readAheadStartFrames);
    EXPECT_NEAR(probe.visualPlayPosBeforeFrames +
                    kVSyncOffsetFrames * probe.effectiveRate,
            probe.visualVSyncPosBeforeFrames,
            1e-9);

    const MarkerPlayheadPixelResult pixel = replayMarkerAtSyntheticVSync(
            pEngineBuffer,
            m_sGroup1,
            track,
            probe.callbackIndex,
            markerOutputFrame);
    ASSERT_TRUE(pixel.rendererInitialized);
    const bool stretchedAlignmentPassed =
            std::abs(kEngineMarkerSourceFrame -
                    (probe.visualPlayPosBeforeFrames +
                            markerOutputFrame * probe.effectiveRate)) <= 2.0 &&
            std::abs(pixel.playheadPixel - pixel.markerPixel) <= 1.0;
    if (!stretchedAlignmentPassed) {
        writeEngineMarkerFailureTrace(
                "Bungee",
                probe.similarity.correlation,
                probe.similarity.normalizedError,
                probe.similarity.outputFrame,
                probe.callbackIndex,
                probe.playPosBeforeFrames,
                probe.playPosAfterFrames,
                probe.visualPlayPosBeforeFrames,
                probe.visualVSyncPosBeforeFrames,
                pixel.playheadSample,
                pixel.markerPixel,
                pixel.playheadPixel,
                probe.requestedOutputFrames,
                probe.returnedSourceFrames,
                probe.readAheadStartFrames,
                probe.readAheadEndFrames,
                probe.readAheadObservationCount,
                3,
                probe.maximumOutput,
                "BungeeStretchedMarkerTracksEnginePosition");
    }
    EXPECT_NEAR(pixel.playheadPixel, pixel.markerPixel, 1.0);
}
#endif
