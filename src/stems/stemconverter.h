#pragma once

#include <QByteArray>
#include <QMutex>
#include <QObject>
#include <QString>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#ifdef __STEM_CONVERSION__
#include <onnxruntime_cxx_api.h>
#endif

#include "track/track.h"

/// Converts audio tracks to STEM format using ONNX Runtime and mp4box
class StemConverter : public QObject {
    Q_OBJECT

    friend class StemgenMasterConversionTest;

  public:
    enum class Resolution {
        High, // Unsupported until a verified htdemucs_ft artifact is published.
        Low,  // 44.1 kHz, verified base model (htdemucs).
    };

    /// Return the only model currently supported by the installer.
    static constexpr Resolution defaultResolution() {
        return Resolution::Low;
    }

    /// Return whether a model has a verified artifact available to Mixxx.
    static constexpr bool isResolutionSupported(Resolution resolution) {
        return resolution == Resolution::Low;
    }

    enum class ConversionState {
        Idle,
        Processing,
        Completed,
        Failed,
    };

    explicit StemConverter(QObject* parent = nullptr);
    ~StemConverter() override = default;

    /// Convert a track to STEM format. Model validation failures are reported
    /// before conversion starts and leave progress at 0.0.
    /// @param pTrack Track to convert
    /// @param resolution Output resolution (only Low is currently supported)
    void convertTrack(const TrackPointer& pTrack,
            Resolution resolution = defaultResolution());

    /// Get current conversion progress (0.0 - 1.0)
    float getProgress() const;

    /// Get the title of the track being converted
    QString getTrackTitle() const;

    /// Get the source path used as the conversion identity for temporary tracks.
    QString getTrackPath() const;

    /// Get the current conversion state
    ConversionState getState() const;

    /// Convert interleaved audio frames to the planar layout used by Demucs.
    static bool interleavedToPlanar(const std::vector<float>& interleaved,
            int channels,
            std::size_t frames,
            std::vector<float>* pPlanar);

    /// Convert planar model output to interleaved audio frames for Mixxx/files.
    static bool planarToInterleaved(const std::vector<float>& planar,
            int channels,
            std::size_t frames,
            std::vector<float>* pInterleaved);

    /// Temporary tracks are not database records, so their path is their identity.
    static bool isValidConversionInput(const TrackPointer& pTrack);

    /// Return whether decoded audio must be converted for HTDemucs.
    static bool requiresAudioFormatConversion(int sampleRate, int channels);

    /// Return whether an executable is the package-owned MP4Box in a Flatpak
    /// sandbox prefix.
    static bool isTrustedFlatpakMP4BoxPath(const QString& mp4boxPath,
            const QString& flatpakPrefix);

#ifdef __STEM_CONVERSION__
    /// Find a model in an explicit override, the installed resources, or the user model directory.
    static QString findModelPath(const QString& modelFileName,
            const QString& modelDirectoryOverride,
            const QString& resourcePath,
            const QString& userModelDirectory);

    /// Verify a model file against the expected size and SHA-256 digest.
    static bool isVerifiedModelFile(const QString& modelPath,
            qint64 expectedSize,
            const QByteArray& expectedSha256);
#endif

    /// Return whether an overlapping inference window starts after this one.
    static bool hasNextChunk(
            std::size_t frameOffset,
            std::size_t chunkStepFrames,
            std::size_t totalFrames);

    /// Return the blend weight for a frame in an inference window.
    static float getChunkFrameWeight(
            std::size_t frameOffset,
            std::size_t frame,
            std::size_t chunkWindowFrames,
            std::size_t chunkStepFrames,
            std::size_t totalFrames);

    /// Return a collision-resistant directory for the converted stems.
    static QString getStemsDirectory(const QString& trackFilePath);

    /// Return the final STEM container path for a stems directory.
    static QString getStemOutputPath(const QString& stemsDir);

  signals:
    /// Emitted when conversion starts
    void conversionStarted(TrackId trackId, const QString& trackTitle);

    /// Emitted when conversion progress updates
    void conversionProgress(TrackId trackId, float progress, const QString& message);

    /// Emitted when conversion completes successfully
    void conversionCompleted(TrackId trackId);

    /// Emitted when conversion fails
    void conversionFailed(TrackId trackId, const QString& errorMessage);

  private:
#ifdef __STEM_CONVERSION__
    static constexpr int kModelChannels = 2;
    static constexpr std::size_t kChunkFrames = 343980;
    static constexpr std::size_t kChunkOverlapFrames = kChunkFrames / 4;
    static constexpr std::size_t kChunkStepFrames = kChunkFrames - kChunkOverlapFrames;
    static constexpr int kModelSampleRate = 44100;

    struct ModelConfig {
        const char* fileName;
        int sampleRate;
    };

    static ModelConfig getModelConfig(Resolution resolution);

    /// to save number of track original frames
    std::size_t m_originalFrames = 0;

    /// Step 1: Load the selected ONNX model from the configured model directory.
    bool loadOnnxModel();

    /// Step 2: Run ONNX inference on audio file
    bool runInference(const std::vector<float>& inputAudio,
            int sampleRate,
            std::vector<std::vector<float>>& outputStems);

    /// Step 3: Decode audio file to WAV using ffmpeg
    bool decodeAudioFile(const QString& inputPath,
            int targetSampleRate,
            std::vector<float>& audioData,
            int& sampleRate,
            int& channels,
            std::size_t& originalFrames);

    /// Step 4: Save stem to WAV file using libsndfile
    bool saveStemToWav(const std::vector<float>& audioData,
            const QString& outputPath,
            int sampleRate,
            int channels,
            std::size_t originalFrames);

    /// Main ONNX separation function
    bool runOnnxSeparation(const QString& trackFilePath, const QString& outputDir);
#endif

    void setProgress(float progress);
    void setState(ConversionState state);
    void setTrackTitle(const QString& trackTitle);
    void setTrackPath(const QString& trackPath);

    /// Convert separated stems (WAV) to M4A format
    bool convertStemsToM4A(const QString& stemsDir);

    /// Create STEM container using ffmpeg and mp4box
    bool createStemContainer(const QString& trackFilePath, const QString& stemsDir);

    /// Create STEM manifest JSON
    QString createStemManifest();

    /// Find MP4Box executable in system. Flatpak builds accept only the
    /// package-owned executable under their sandbox prefix. Other Linux builds
    /// require the dpkg-managed gpac package; unsupported package managers fail
    /// closed, while other platforms use PATH lookup.
    QString findMP4Box();

    /// Add STEM metadata atom using MP4Box
    bool addStemMetadata(const QString& outputPath);

    /// Convert a single track to M4A format
    bool convertTrackToM4A(const QString& inputPath, const QString& outputPath);

#ifdef __STEM_CONVERSION__
    // ONNX Runtime members
    std::unique_ptr<Ort::Env> m_pOrtEnv;
    std::unique_ptr<Ort::Session> m_pOrtSession;
    Ort::AllocatorWithDefaultOptions m_allocator;
    std::optional<Resolution> m_loadedResolution;
#endif

    TrackPointer m_pTrack;
    Resolution m_resolution{defaultResolution()};
    ConversionState m_state;
    float m_progress;
    QString m_trackTitle;
    QString m_trackPath;
    mutable QMutex m_statusMutex;
};

using StemConverterPointer = std::shared_ptr<StemConverter>;
