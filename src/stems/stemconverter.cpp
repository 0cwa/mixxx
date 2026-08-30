#include "stems/stemconverter.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <algorithm>
#include <cstring>
#include <limits>

#ifdef __STEM_CONVERSION__
#include <sndfile.h>
#endif

#include "preferences/configobject.h"
#include "sources/soundsourceproxy.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("StemConverter");
constexpr int kStemModelSampleRate = 44100;
constexpr int kStemModelChannels = 2;
constexpr auto kStemModelResourceDirectory = "models";
constexpr auto kStemModelDirectoryEnvironmentVariable = "MIXXX_STEM_MODEL_DIR";
constexpr auto kStemModelLfsPointerHeader = "version https://git-lfs.github.com/spec/v1";
constexpr qint64 kStemModelHashChunkSize = 1024 * 1024;
constexpr int kExternalProcessTimeoutMs = 10 * 60 * 1000;
constexpr int kProcessTerminateGracePeriodMs = 1000;

bool isGitLfsPointer(const QFileInfo& modelFile) {
    QFile file(modelFile.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    return file.readLine().trimmed() == QByteArray(kStemModelLfsPointerHeader);
}

bool waitForProcess(QProcess* pProcess, const QString& description) {
    if (!pProcess->waitForStarted(5000)) {
        kLogger.warning() << "Failed to start" << description << pProcess->errorString();
        return false;
    }
    if (pProcess->waitForFinished(kExternalProcessTimeoutMs)) {
        return true;
    }

    kLogger.warning() << "Timed out waiting for" << description;
    pProcess->terminate();
    if (!pProcess->waitForFinished(kProcessTerminateGracePeriodMs)) {
        pProcess->kill();
        if (!pProcess->waitForFinished(kProcessTerminateGracePeriodMs)) {
            kLogger.warning() << "Could not stop" << description;
        }
    }
    return false;
}

#ifdef Q_OS_LINUX
bool runDpkgQuery(const QStringList& arguments, QString* pOutput) {
    const QString dpkgQueryPath = QStandardPaths::findExecutable(
            QStringLiteral("dpkg-query"),
            {QStringLiteral("/usr/bin"), QStringLiteral("/bin")});
    if (dpkgQueryPath.isEmpty()) {
        return false;
    }

    QProcess dpkgQuery;
    dpkgQuery.start(dpkgQueryPath, arguments);
    if (!dpkgQuery.waitForStarted(5000) || !dpkgQuery.waitForFinished(5000) ||
            dpkgQuery.exitStatus() != QProcess::NormalExit || dpkgQuery.exitCode() != 0) {
        return false;
    }

    if (pOutput) {
        *pOutput = QString::fromLocal8Bit(dpkgQuery.readAllStandardOutput());
    }
    return true;
}

QString findDpkgOwnedMp4Box() {
    QString packageStatus;
    if (!runDpkgQuery(
                {QStringLiteral("-W"),
                        QStringLiteral("-f=${Status}"),
                        QStringLiteral("gpac")},
                &packageStatus) ||
            packageStatus.trimmed() != QStringLiteral("install ok installed")) {
        return {};
    }

    QString packageFiles;
    if (!runDpkgQuery(
                {QStringLiteral("-L"), QStringLiteral("gpac")},
                &packageFiles)) {
        return {};
    }

    const QStringList packagePaths = packageFiles.split(QChar('\n'), Qt::SkipEmptyParts);
    for (const QString& packagePath : packagePaths) {
        const QFileInfo mp4boxInfo(packagePath.trimmed());
        if (mp4boxInfo.fileName() != QStringLiteral("MP4Box") ||
                !mp4boxInfo.isFile() || !mp4boxInfo.isExecutable()) {
            continue;
        }

        QString packageOwners;
        if (!runDpkgQuery(
                    {QStringLiteral("-S"), mp4boxInfo.absoluteFilePath()},
                    &packageOwners)) {
            continue;
        }
        for (const QString& owner :
                packageOwners.split(QChar('\n'), Qt::SkipEmptyParts)) {
            if (owner.section(QChar(':'), 0, 0) == QStringLiteral("gpac")) {
                return mp4boxInfo.absoluteFilePath();
            }
        }
    }
    return {};
}
#endif
} // namespace

bool StemConverter::interleavedToPlanar(const std::vector<float>& interleaved,
        int channels,
        std::size_t frames,
        std::vector<float>* pPlanar) {
    if (!pPlanar || channels <= 0 ||
            frames > std::numeric_limits<std::size_t>::max() /
                            static_cast<std::size_t>(channels) ||
            interleaved.size() != frames * static_cast<std::size_t>(channels)) {
        return false;
    }

    pPlanar->assign(interleaved.size(), 0.0f);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            (*pPlanar)[static_cast<std::size_t>(channel) * frames + frame] =
                    interleaved[frame * static_cast<std::size_t>(channels) +
                            static_cast<std::size_t>(channel)];
        }
    }
    return true;
}

bool StemConverter::planarToInterleaved(const std::vector<float>& planar,
        int channels,
        std::size_t frames,
        std::vector<float>* pInterleaved) {
    if (!pInterleaved || channels <= 0 ||
            frames > std::numeric_limits<std::size_t>::max() /
                            static_cast<std::size_t>(channels) ||
            planar.size() != frames * static_cast<std::size_t>(channels)) {
        return false;
    }

    pInterleaved->assign(planar.size(), 0.0f);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            (*pInterleaved)[frame * static_cast<std::size_t>(channels) +
                    static_cast<std::size_t>(channel)] =
                    planar[static_cast<std::size_t>(channel) * frames + frame];
        }
    }
    return true;
}

StemConverter::StemConverter(QObject* parent)
        : QObject(parent),
          m_state(ConversionState::Idle),
          m_progress(0.0f),
          m_trackTitle("Unknown") {
#ifdef __STEM_CONVERSION__
    // Suppress ONNX Runtime verbose output
    qputenv("ORT_DISABLE_TELEMETRY", QByteArrayLiteral("1"));
    qputenv("ORT_DISABLE_LOGGING", QByteArrayLiteral("1"));

    try {
        m_pOrtEnv = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "StemConverter");
        kLogger.info() << "ONNX Runtime environment initialized";
    } catch (const std::exception& e) {
        kLogger.warning() << "Failed to initialize ONNX Runtime:" << e.what();
    }
#endif
}

bool StemConverter::isValidConversionInput(const TrackPointer& pTrack) {
    return pTrack && !pTrack->getLocation().isEmpty();
}

bool StemConverter::requiresAudioFormatConversion(int sampleRate, int channels) {
    return sampleRate != kStemModelSampleRate || channels != kStemModelChannels;
}

bool StemConverter::isTrustedFlatpakMP4BoxPath(
        const QString& mp4boxPath, const QString& flatpakPrefix) {
    if (flatpakPrefix.isEmpty()) {
        return false;
    }

    const QFileInfo prefixInfo(flatpakPrefix);
    const QFileInfo mp4boxInfo(mp4boxPath);
    if (!prefixInfo.isDir() || !mp4boxInfo.isFile() ||
            !mp4boxInfo.isExecutable() || mp4boxInfo.isSymLink()) {
        return false;
    }

    const QString canonicalPrefix = prefixInfo.canonicalFilePath();
    const QString canonicalMp4Box = mp4boxInfo.canonicalFilePath();
    if (canonicalPrefix.isEmpty() || canonicalMp4Box.isEmpty()) {
        return false;
    }

    const QString expectedMp4Box =
            QFileInfo(QDir(canonicalPrefix).filePath(QStringLiteral("bin/MP4Box")))
                    .canonicalFilePath();
    return !expectedMp4Box.isEmpty() && canonicalMp4Box == expectedMp4Box;
}

bool StemConverter::hasNextChunk(
        std::size_t frameOffset,
        std::size_t chunkStepFrames,
        std::size_t totalFrames) {
    return chunkStepFrames > 0 && frameOffset < totalFrames &&
            chunkStepFrames < totalFrames - frameOffset;
}

float StemConverter::getChunkFrameWeight(
        std::size_t frameOffset,
        std::size_t frame,
        std::size_t chunkWindowFrames,
        std::size_t chunkStepFrames,
        std::size_t totalFrames) {
    if (chunkWindowFrames == 0 || frameOffset >= totalFrames) {
        return 0.0f;
    }

    const std::size_t chunkFrames = std::min(
            chunkWindowFrames, totalFrames - frameOffset);
    if (frame >= chunkFrames) {
        return 0.0f;
    }

    const std::size_t chunkEnd = frameOffset + chunkFrames;
    const std::size_t overlapFrames = chunkWindowFrames > chunkStepFrames
            ? chunkWindowFrames - chunkStepFrames
            : 0;
    if (overlapFrames == 0) {
        return 1.0f;
    }

    const std::size_t outputFrame = frameOffset + frame;
    float weight = 1.0f;

    if (frameOffset >= chunkStepFrames) {
        const std::size_t previousStart = frameOffset - chunkStepFrames;
        const std::size_t previousFrames = std::min(
                chunkWindowFrames, totalFrames - previousStart);
        const std::size_t previousEnd = previousStart + previousFrames;
        const std::size_t fadeInFrames = previousEnd > frameOffset
                ? std::min(chunkFrames, previousEnd - frameOffset)
                : 0;
        if (frame < fadeInFrames) {
            weight *= static_cast<float>(frame + 1) /
                    static_cast<float>(overlapFrames);
        }
    }

    if (hasNextChunk(frameOffset, chunkStepFrames, totalFrames)) {
        const std::size_t nextStart = frameOffset + chunkStepFrames;
        const std::size_t nextFrames = std::min(
                chunkWindowFrames, totalFrames - nextStart);
        const std::size_t nextEnd = nextStart + nextFrames;
        const std::size_t fadeOutStart = nextStart;
        const std::size_t fadeOutEnd = std::min(chunkEnd, nextEnd);
        if (outputFrame >= fadeOutStart && outputFrame < fadeOutEnd) {
            const std::size_t fadeOutFrame = outputFrame - fadeOutStart;
            weight *= static_cast<float>(overlapFrames - fadeOutFrame - 1) /
                    static_cast<float>(overlapFrames);
        }
    }

    return weight;
}

void StemConverter::setProgress(float progress) {
    QMutexLocker locker(&m_statusMutex);
    m_progress = progress;
}

void StemConverter::setState(ConversionState state) {
    QMutexLocker locker(&m_statusMutex);
    m_state = state;
}

void StemConverter::setTrackTitle(const QString& trackTitle) {
    QMutexLocker locker(&m_statusMutex);
    m_trackTitle = trackTitle;
}

void StemConverter::setTrackPath(const QString& trackPath) {
    QMutexLocker locker(&m_statusMutex);
    m_trackPath = trackPath;
}

void StemConverter::convertTrack(const TrackPointer& pTrack, Resolution resolution) {
    if (!pTrack) {
        qWarning() << "Cannot convert null track";
        emit conversionFailed(TrackId(), QStringLiteral("Track is null"));
        setState(ConversionState::Failed);
        return;
    }

    const TrackId trackId = pTrack->getId();
    if (!isValidConversionInput(pTrack)) {
        qWarning() << "Cannot convert track without a source path";
        emit conversionFailed(trackId, QStringLiteral("Track file path is empty"));
        setState(ConversionState::Failed);
        return;
    }

    m_pTrack = pTrack;
    m_resolution = resolution;

    const QString trackFilePath = QFileInfo(pTrack->getLocation()).absoluteFilePath();
    QFileInfo fileInfo(trackFilePath);
    setTrackTitle(fileInfo.fileName());
    setTrackPath(QDir::cleanPath(trackFilePath));

    if (!isResolutionSupported(resolution)) {
        const QString errorMsg = QStringLiteral(
                "Fine-tuned HTDemucs is unavailable until a verified model artifact is published");
        kLogger.warning() << errorMsg;
        emit conversionFailed(trackId, errorMsg);
        setState(ConversionState::Failed);
        return;
    }

    setProgress(0.0f);

#ifdef __STEM_CONVERSION__
    // Validate the verified model before entering Processing or announcing
    // conversion progress. A missing or unloadable model is an early
    // configuration failure, not a failed audio processing step.
    if (!loadOnnxModel()) {
        const QString errorMsg = QStringLiteral("Verified HTDemucs model unavailable");
        kLogger.warning() << errorMsg;
        emit conversionFailed(trackId, errorMsg);
        setState(ConversionState::Failed);
        return;
    }
#endif

    setState(ConversionState::Processing);

    if (trackFilePath.isEmpty()) {
        qWarning() << "Track file path is empty";
        emit conversionFailed(trackId, QStringLiteral("Track file path is empty"));
        setState(ConversionState::Failed);
        return;
    }

    emit conversionStarted(trackId, getTrackTitle());
    setProgress(0.1f);
    emit conversionProgress(trackId, 0.1f, QStringLiteral("Starting ONNX separation..."));

#ifdef __STEM_CONVERSION__
    QString stemsDir = getStemsDirectory(trackFilePath);
    if (!runOnnxSeparation(trackFilePath, stemsDir)) {
        const QString errorMsg = QStringLiteral("ONNX separation failed");
        kLogger.warning() << errorMsg;
        emit conversionFailed(trackId, errorMsg);
        setState(ConversionState::Failed);
        return;
    }
#else
    const QString errorMsg =
            QStringLiteral("STEM conversion not available (ONNX Runtime not compiled)");
    kLogger.warning() << errorMsg;
    emit conversionFailed(trackId, errorMsg);
    setState(ConversionState::Failed);
    return;
#endif

    setProgress(0.5f);
    emit conversionProgress(trackId, 0.5f, QStringLiteral("Converting stems to M4A..."));

    if (!convertStemsToM4A(stemsDir)) {
        const QString errorMsg = QStringLiteral("Stem conversion to M4A failed");
        kLogger.warning() << errorMsg;
        emit conversionFailed(trackId, errorMsg);
        setState(ConversionState::Failed);
        return;
    }

    setProgress(0.7f);
    emit conversionProgress(trackId, 0.7f, QStringLiteral("Creating STEM container..."));

    if (!createStemContainer(trackFilePath, stemsDir)) {
        const QString errorMsg = QStringLiteral("STEM container creation failed");
        kLogger.warning() << errorMsg;
        emit conversionFailed(trackId, errorMsg);
        setState(ConversionState::Failed);
        return;
    }

    setProgress(1.0f);
    emit conversionProgress(trackId, 1.0f, QStringLiteral("Conversion completed successfully!"));
    emit conversionCompleted(trackId);

    setState(ConversionState::Completed);
    kLogger.info() << "Track conversion completed:" << trackFilePath;
}

float StemConverter::getProgress() const {
    QMutexLocker locker(&m_statusMutex);
    return m_progress;
}

QString StemConverter::getTrackTitle() const {
    QMutexLocker locker(&m_statusMutex);
    return m_trackTitle;
}

QString StemConverter::getTrackPath() const {
    QMutexLocker locker(&m_statusMutex);
    return m_trackPath;
}

StemConverter::ConversionState StemConverter::getState() const {
    QMutexLocker locker(&m_statusMutex);
    return m_state;
}

#ifdef __STEM_CONVERSION__
StemConverter::ModelConfig StemConverter::getModelConfig(Resolution resolution) {
    if (!isResolutionSupported(resolution)) {
        return {nullptr, 0};
    }

    return {MIXXX_STEM_MODEL_NAME, kStemModelSampleRate};
}

QString StemConverter::findModelPath(const QString& modelFileName,
        const QString& modelDirectoryOverride,
        const QString& resourcePath,
        const QString& userModelDirectory) {
    const auto findInDirectory = [&modelFileName](const QString& directory) {
        if (directory.isEmpty()) {
            return QString();
        }

        const QFileInfo modelFile(QDir(directory).filePath(modelFileName));
        if (!modelFile.isFile()) {
            return QString();
        }
        if (isGitLfsPointer(modelFile)) {
            kLogger.warning()
                    << "Ignoring unmaterialized Git LFS pointer instead of an ONNX model:"
                    << modelFile.absoluteFilePath();
            return QString();
        }
        return modelFile.absoluteFilePath();
    };

    // Preserve the environment variable as an explicit override. In particular,
    // do not silently fall back to a package model when the override is missing.
    if (!modelDirectoryOverride.isEmpty()) {
        return findInDirectory(modelDirectoryOverride);
    }

    const QString packagedModelPath = findInDirectory(
            QDir(resourcePath).filePath(QString::fromLatin1(kStemModelResourceDirectory)));
    if (!packagedModelPath.isEmpty()) {
        return packagedModelPath;
    }

    // CMake embeds the source resource directory for development builds. Only
    // use its sibling model directory when the active resource path is that
    // exact directory; an installed resource path must not infer a source tree
    // from an unrelated parent directory.
    const QString sourceResourcePath =
            QStringLiteral(MIXXX_STEM_MODEL_SOURCE_RESOURCE_PATH);
    if (QDir::cleanPath(QFileInfo(resourcePath).absoluteFilePath()) ==
            QDir::cleanPath(QFileInfo(sourceResourcePath).absoluteFilePath())) {
        const QString sourceTreeModelPath = findInDirectory(
                QDir(sourceResourcePath).filePath(QStringLiteral("../models")));
        if (!sourceTreeModelPath.isEmpty()) {
            return sourceTreeModelPath;
        }
    }

    return findInDirectory(userModelDirectory);
}

bool StemConverter::isVerifiedModelFile(const QString& modelPath,
        qint64 expectedSize,
        const QByteArray& expectedSha256) {
    const QFileInfo modelFile(modelPath);
    if (!modelFile.isFile()) {
        kLogger.warning() << "Stem model is missing or not a regular file:" << modelPath;
        return false;
    }

    if (isGitLfsPointer(modelFile)) {
        kLogger.warning()
                << "Stem model is an unmaterialized Git LFS pointer, not model content:"
                << modelPath;
        return false;
    }

    if (modelFile.size() != expectedSize) {
        kLogger.warning() << "Stem model has size" << modelFile.size() << "bytes; expected"
                          << expectedSize << "bytes:" << modelPath;
        return false;
    }

    QFile model(modelPath);
    if (!model.open(QIODevice::ReadOnly)) {
        kLogger.warning() << "Failed to open Stem model for SHA-256 verification:"
                          << modelPath << model.errorString();
        return false;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!model.atEnd()) {
        const QByteArray chunk = model.read(kStemModelHashChunkSize);
        if (chunk.isEmpty()) {
            if (model.error() != QFileDevice::NoError) {
                kLogger.warning() << "Failed to read Stem model for SHA-256 verification:"
                                  << modelPath << model.errorString();
                return false;
            }
            break;
        }
        hash.addData(chunk);
    }

    const QByteArray actualSha256 = hash.result().toHex();
    if (actualSha256 != expectedSha256.toLower()) {
        kLogger.warning() << "Stem model SHA-256 is" << actualSha256 << "; expected"
                          << expectedSha256 << ":" << modelPath;
        return false;
    }

    return true;
}

bool StemConverter::loadOnnxModel() {
    if (m_pOrtSession && m_loadedResolution && *m_loadedResolution == m_resolution) {
        return true; // Already loaded
    }

    if (!m_pOrtEnv) {
        kLogger.warning() << "ONNX Runtime environment not initialized";
        return false;
    }

    try {
        const ModelConfig modelConfig = getModelConfig(m_resolution);
        if (!modelConfig.fileName) {
            kLogger.warning() << "Selected HTDemucs model is not supported";
            return false;
        }
        const QString modelPath = findModelPath(
                QString::fromLatin1(modelConfig.fileName),
                qEnvironmentVariable(kStemModelDirectoryEnvironmentVariable),
                ConfigObject<ConfigValue>::computeResourcePath(),
                QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                        .filePath(QStringLiteral(".local/mixxx_models")));

        if (modelPath.isEmpty()) {
            kLogger.warning() << "ONNX model not found in the configured, "
                                 "installed, or user model directories";
            return false;
        }

        if (!isVerifiedModelFile(
                    modelPath,
                    MIXXX_STEM_MODEL_EXPECTED_SIZE,
                    QByteArrayLiteral(MIXXX_STEM_MODEL_EXPECTED_SHA256))) {
            kLogger.warning() << "Refusing to load unverified Stem model:" << modelPath;
            return false;
        }

        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(4);
        sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        m_pOrtSession = std::make_unique<Ort::Session>(
                *m_pOrtEnv,
                modelPath.toStdString().c_str(),
                sessionOptions);
        m_loadedResolution = m_resolution;

        kLogger.info() << "ONNX model loaded successfully from:" << modelPath;
        return true;
    } catch (const std::exception& e) {
        kLogger.warning() << "Failed to load ONNX model:" << e.what();
        return false;
    }
}

bool StemConverter::runInference(const std::vector<float>& inputAudio,
        int sampleRate,
        std::vector<std::vector<float>>& outputStems) {
    if (!m_pOrtSession) {
        kLogger.warning() << "ONNX session not initialized";
        return false;
    }

    try {
        const ModelConfig modelConfig = getModelConfig(m_resolution);
        if (sampleRate != modelConfig.sampleRate) {
            kLogger.warning() << "Unexpected sample rate for ONNX model:" << sampleRate
                              << "expected" << modelConfig.sampleRate;
            return false;
        }

        if (inputAudio.size() != kChunkFrames * kModelChannels) {
            kLogger.warning() << "Unexpected interleaved input size:" << inputAudio.size();
            return false;
        }

        std::vector<float> planarInput;
        if (!interleavedToPlanar(
                    inputAudio, kModelChannels, kChunkFrames, &planarInput)) {
            kLogger.warning() << "Could not convert interleaved model input to planar layout";
            return false;
        }

        const std::vector<int64_t> inputShape = {
                1, kModelChannels, static_cast<int64_t>(kChunkFrames)};

        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                m_allocator.GetInfo(),
                planarInput.data(),
                planarInput.size(),
                inputShape.data(),
                inputShape.size());

        auto inputName = m_pOrtSession->GetInputNameAllocated(0, m_allocator);
        auto outputName = m_pOrtSession->GetOutputNameAllocated(0, m_allocator);
        const char* inputNames[] = {inputName.get()};
        const char* outputNames[] = {outputName.get()};

        // Run inference
        auto outputTensors = m_pOrtSession->Run(
                Ort::RunOptions{nullptr},
                inputNames,
                &inputTensor,
                1,
                outputNames,
                1);

        // Extract output stems from the single output tensor
        // Output shape is expected to be [1, 4, 2, 343980] (batch, stems, channels, samples)
        if (outputTensors.empty()) {
            kLogger.warning() << "ONNX inference returned no output tensor";
            return false;
        }

        auto& outputTensor = outputTensors.front();
        float* outputData = outputTensor.GetTensorMutableData<float>();
        auto tensorInfo = outputTensor.GetTensorTypeAndShapeInfo();
        auto outputShape = tensorInfo.GetShape();

        // HTDemucs output is [batch, stems, channels, samples].
        if (outputShape.size() != 4 || outputShape[0] != 1 || outputShape[1] != 4 ||
                outputShape[2] != kModelChannels || outputShape[3] <= 0 ||
                outputShape[3] > static_cast<int64_t>(kChunkFrames)) {
            kLogger.warning() << "Unexpected output tensor shape:" << outputShape.size();
            return false;
        }

        int64_t numStems = outputShape[1];
        int64_t numChannels = outputShape[2];
        int64_t numSamples = outputShape[3];

        outputStems.resize(numStems);
        for (int i = 0; i < numStems; ++i) {
            int64_t stemSize = numChannels * numSamples;
            float* stemStart = outputData + (i * stemSize);
            std::vector<float> planarStem(stemStart, stemStart + stemSize);
            if (!planarToInterleaved(
                        planarStem,
                        static_cast<int>(numChannels),
                        static_cast<std::size_t>(numSamples),
                        &outputStems[i])) {
                kLogger.warning() << "Could not convert ONNX output to interleaved layout";
                return false;
            }
        }

        kLogger.info() << "ONNX inference completed successfully";
        return true;
    } catch (const std::exception& e) {
        kLogger.warning() << "ONNX inference failed:" << e.what();
        return false;
    }
}

bool StemConverter::decodeAudioFile(const QString& inputPath,
        int targetSampleRate,
        std::vector<float>& audioData,
        int& sampleRate,
        int& channels,
        std::size_t& originalFrames) {
    if (targetSampleRate <= 0) {
        kLogger.warning() << "Invalid target sample rate:" << targetSampleRate;
        return false;
    }

    auto pTrack = Track::newTemporary(inputPath);
    if (!isValidConversionInput(pTrack)) {
        kLogger.warning() << "Cannot decode audio without a source path:" << inputPath;
        return false;
    }

    auto pProxy = std::make_unique<SoundSourceProxy>(pTrack);
    mixxx::AudioSourcePointer pAudioSource = pProxy->openAudioSource(
            mixxx::AudioSource::OpenParams(
                    mixxx::audio::ChannelCount(kModelChannels),
                    mixxx::audio::SampleRate(targetSampleRate)));
    if (!pAudioSource) {
        kLogger.warning() << "Failed to open audio file:" << inputPath;
        return false;
    }

    sampleRate = pAudioSource->getSignalInfo().getSampleRate();
    channels = pAudioSource->getSignalInfo().getChannelCount();
    QTemporaryDir convertedInputDir;
    if (requiresAudioFormatConversion(sampleRate, channels)) {
        pAudioSource->close();
        if (!convertedInputDir.isValid()) {
            kLogger.warning() << "Failed to create temporary directory for audio conversion";
            return false;
        }

        const QString convertedInputPath = convertedInputDir.filePath("stem-input.wav");
        const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (ffmpegPath.isEmpty()) {
            kLogger.warning() << "ffmpeg executable not found for audio conversion";
            return false;
        }

        QProcess ffmpegProcess;
        const QStringList arguments{
                QStringLiteral("-nostdin"),
                QStringLiteral("-hide_banner"),
                QStringLiteral("-loglevel"),
                QStringLiteral("error"),
                QStringLiteral("-i"),
                inputPath,
                QStringLiteral("-vn"),
                QStringLiteral("-ac"),
                QString::number(kModelChannels),
                QStringLiteral("-ar"),
                QString::number(targetSampleRate),
                QStringLiteral("-c:a"),
                QStringLiteral("pcm_f32le"),
                QStringLiteral("-y"),
                convertedInputPath};
        ffmpegProcess.start(ffmpegPath, arguments);
        if (!waitForProcess(&ffmpegProcess, QStringLiteral("ffmpeg audio conversion")) ||
                ffmpegProcess.exitStatus() != QProcess::NormalExit ||
                ffmpegProcess.exitCode() != 0 || !QFile::exists(convertedInputPath)) {
            kLogger.warning() << "ffmpeg could not convert audio to the model format"
                              << ffmpegProcess.readAllStandardError();
            return false;
        }

        pTrack = Track::newTemporary(convertedInputPath);
        pProxy = std::make_unique<SoundSourceProxy>(pTrack);
        pAudioSource = pProxy->openAudioSource(mixxx::AudioSource::OpenParams(
                mixxx::audio::ChannelCount(kModelChannels),
                mixxx::audio::SampleRate(targetSampleRate)));
        if (!pAudioSource) {
            kLogger.warning() << "Failed to open converted audio file:" << convertedInputPath;
            return false;
        }
        sampleRate = pAudioSource->getSignalInfo().getSampleRate();
        channels = pAudioSource->getSignalInfo().getChannelCount();
    }

    if (sampleRate != targetSampleRate || channels != kModelChannels) {
        kLogger.warning() << "Audio source is not in the model format after conversion."
                          << "Sample rate:" << sampleRate << "channels:" << channels;
        return false;
    }

    const SINT totalFrames = pAudioSource->frameLength();
    if (totalFrames <= 0) {
        kLogger.warning() << "Audio source has no readable frames:" << inputPath;
        return false;
    }

    audioData.clear();
    audioData.reserve(static_cast<std::size_t>(totalFrames) * kModelChannels);
    mixxx::SampleBuffer sampleBuffer(static_cast<SINT>(kChunkFrames * kModelChannels));
    SINT frameOffset = 0;
    while (frameOffset < totalFrames) {
        const SINT requestedFrames = std::min<SINT>(
                static_cast<SINT>(kChunkFrames), totalFrames - frameOffset);
        sampleBuffer.clear();
        const auto readableFrames = pAudioSource->readSampleFrames(
                mixxx::WritableSampleFrames(
                        mixxx::IndexRange::between(frameOffset, frameOffset + requestedFrames),
                        mixxx::SampleBuffer::WritableSlice(sampleBuffer)));
        const SINT decodedFrames = readableFrames.frameIndexRange().length();
        if (decodedFrames <= 0 || decodedFrames > requestedFrames) {
            kLogger.warning() << "Audio source returned an invalid frame count:" << decodedFrames;
            return false;
        }

        const std::size_t sampleCount = static_cast<std::size_t>(decodedFrames) * channels;
        if (readableFrames.readableLength() < static_cast<SINT>(sampleCount)) {
            kLogger.warning() << "Audio source returned too few samples for its frame count";
            return false;
        }
        audioData.insert(audioData.end(),
                readableFrames.readableData(),
                readableFrames.readableData() + sampleCount);
        frameOffset += decodedFrames;
        if (decodedFrames < requestedFrames) {
            break;
        }
    }

    originalFrames = audioData.size() / static_cast<std::size_t>(channels);
    if (originalFrames == 0) {
        kLogger.warning() << "Audio source produced no samples:" << inputPath;
        return false;
    }

    kLogger.info() << "Audio decoded at model sample rate:" << sampleRate
                   << "frames:" << originalFrames;

    pAudioSource->close();
    return true;
}

bool StemConverter::saveStemToWav(const std::vector<float>& audioData,
        const QString& outputPath,
        int sampleRate,
        int channels,
        std::size_t originalFrames) {
    if (channels != kModelChannels || originalFrames == 0 ||
            originalFrames > audioData.size() / static_cast<std::size_t>(channels) ||
            originalFrames > static_cast<std::size_t>(std::numeric_limits<sf_count_t>::max())) {
        kLogger.warning() << "Invalid stem audio dimensions for WAV output";
        return false;
    }

    SF_INFO sfInfo;
    memset(&sfInfo, 0, sizeof(sfInfo));

    sfInfo.samplerate = sampleRate;
    sfInfo.channels = channels;
    sfInfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;

    SNDFILE* outfile = sf_open(outputPath.toStdString().c_str(), SFM_WRITE, &sfInfo);
    if (!outfile) {
        kLogger.warning() << "Failed to create WAV file:" << sf_strerror(nullptr);
        return false;
    }

    // Only write the original frames, not the padding
    const sf_count_t framesToWrite = static_cast<sf_count_t>(originalFrames);
    sf_count_t writtenFrames = sf_writef_float(outfile, audioData.data(), framesToWrite);

    if (writtenFrames != framesToWrite) {
        kLogger.warning() << "Failed to write all frames to WAV file";
        sf_close(outfile);
        return false;
    }

    sf_close(outfile);
    kLogger.info() << "Stem saved to WAV:" << outputPath;
    return true;
}

bool StemConverter::runOnnxSeparation(const QString& trackFilePath, const QString& outputDir) {
    kLogger.info() << "Starting ONNX separation for:" << trackFilePath;

    if (!loadOnnxModel()) {
        kLogger.warning() << "Failed to load ONNX model";
        return false;
    }

    const ModelConfig modelConfig = getModelConfig(m_resolution);
    std::vector<float> audioData;
    int sampleRate = 0;
    int channels = 0;

    if (!decodeAudioFile(
                trackFilePath,
                modelConfig.sampleRate,
                audioData,
                sampleRate,
                channels,
                m_originalFrames)) {
        kLogger.warning() << "Failed to decode audio file";
        return false;
    }

    QStringList stemNames = {"drums", "bass", "other", "vocals"};
    const std::size_t samplesPerFrame = static_cast<std::size_t>(channels);
    std::vector<std::vector<float>> outputStems(
            stemNames.size(),
            std::vector<float>(m_originalFrames * samplesPerFrame, 0.0f));
    std::vector<float> outputWeights(m_originalFrames, 0.0f);

    // HTDemucs accepts one fixed-size window. Process overlapping windows so
    // boundaries are blended instead of concatenated, while short tracks are
    // still padded and long tracks are never silently truncated.
    for (std::size_t frameOffset = 0; frameOffset < m_originalFrames;
            frameOffset += kChunkStepFrames) {
        const std::size_t chunkFrames = std::min(
                kChunkFrames, m_originalFrames - frameOffset);
        std::vector<float> inputChunk(kChunkFrames * samplesPerFrame, 0.0f);
        std::copy_n(audioData.begin() + frameOffset * samplesPerFrame,
                chunkFrames * samplesPerFrame,
                inputChunk.begin());

        std::vector<std::vector<float>> chunkStems;
        if (!runInference(inputChunk, sampleRate, chunkStems) ||
                chunkStems.size() != stemNames.size()) {
            kLogger.warning() << "Failed to run ONNX inference for chunk at frame:"
                              << frameOffset;
            return false;
        }

        const bool hasNextChunk = StemConverter::hasNextChunk(
                frameOffset, kChunkStepFrames, m_originalFrames);
        std::vector<float> chunkWeights(chunkFrames, 0.0f);
        for (std::size_t frame = 0; frame < chunkFrames; ++frame) {
            const std::size_t outputFrame = frameOffset + frame;
            const float weight = StemConverter::getChunkFrameWeight(
                    frameOffset,
                    frame,
                    kChunkFrames,
                    kChunkStepFrames,
                    m_originalFrames);
            chunkWeights[frame] = weight;
            outputWeights[outputFrame] += weight;
        }

        for (std::size_t stem = 0; stem < chunkStems.size(); ++stem) {
            const std::size_t chunkSamples = chunkFrames * samplesPerFrame;
            if (chunkStems[stem].size() < chunkSamples) {
                kLogger.warning() << "ONNX output is shorter than the input chunk";
                return false;
            }
            for (std::size_t frame = 0; frame < chunkFrames; ++frame) {
                const std::size_t outputFrame = frameOffset + frame;
                for (std::size_t channel = 0; channel < samplesPerFrame; ++channel) {
                    outputStems[stem][outputFrame * samplesPerFrame + channel] +=
                            chunkStems[stem][frame * samplesPerFrame + channel] *
                            chunkWeights[frame];
                }
            }
        }

        if (!hasNextChunk) {
            break;
        }
    }

    for (std::size_t frame = 0; frame < m_originalFrames; ++frame) {
        if (outputWeights[frame] <= 0.0f) {
            kLogger.warning() << "No inferred samples available at frame:" << frame;
            return false;
        }
        for (auto& outputStem : outputStems) {
            for (std::size_t channel = 0; channel < samplesPerFrame; ++channel) {
                outputStem[frame * samplesPerFrame + channel] /= outputWeights[frame];
            }
        }
    }

    if (!QDir().mkpath(outputDir)) {
        kLogger.warning() << "Failed to create stem output directory:" << outputDir;
        return false;
    }

    for (size_t i = 0; i < outputStems.size(); ++i) {
        QString stemPath = outputDir + "/" + stemNames[i] + ".wav";
        if (!saveStemToWav(outputStems[i], stemPath, sampleRate, channels, m_originalFrames)) {
            kLogger.warning() << "Failed to save stem:" << stemNames[i];
            return false;
        }
    }

    kLogger.info() << "ONNX separation completed successfully";
    return true;
}
#endif

QString StemConverter::getStemsDirectory(const QString& trackFilePath) {
    QFileInfo fileInfo(trackFilePath);
    const QDir trackDir(fileInfo.absolutePath());
    const QString baseName = fileInfo.baseName();

    // Keep the legacy basename when it is available. If either the temporary
    // directory or the final container already exists, add a deterministic
    // suffix so queued conversions never overwrite an earlier result.
    for (int collisionIndex = 0;; ++collisionIndex) {
        const QString collisionSuffix = collisionIndex == 0
                ? QString()
                : QStringLiteral("-%1").arg(collisionIndex);
        const QString outputBaseName = baseName + collisionSuffix;
        const QString stemsDir = trackDir.filePath(outputBaseName);
        const QString outputPath = trackDir.filePath(
                outputBaseName + QStringLiteral(".stem.m4a"));

        if (!QFileInfo(stemsDir).exists() && !QFileInfo(outputPath).exists()) {
            return stemsDir;
        }
    }
}

QString StemConverter::getStemOutputPath(const QString& stemsDir) {
    const QFileInfo stemsDirInfo(stemsDir);
    return stemsDirInfo.absoluteDir().filePath(
            stemsDirInfo.fileName() + QStringLiteral(".stem.m4a"));
}

bool StemConverter::convertStemsToM4A(const QString& stemsDir) {
    kLogger.info() << "Converting stems to M4A:" << stemsDir;

    QDir dir(stemsDir);
    if (!dir.exists()) {
        kLogger.warning() << "Stems directory does not exist:" << stemsDir;
        return false;
    }

    QStringList stemNames = {"drums", "bass", "other", "vocals"};

    const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpegPath.isEmpty()) {
        kLogger.warning() << "ffmpeg executable not found";
        return false;
    }

    for (const QString& stemName : stemNames) {
        QString wavPath = stemsDir + "/" + stemName + ".wav";
        QString m4aPath = stemsDir + "/" + stemName + ".m4a";

        if (!QFile::exists(wavPath)) {
            kLogger.warning() << "WAV file not found:" << wavPath;
            return false;
        }

        QProcess ffmpegProcess;
        QStringList args;
        args << "-i" << wavPath
             << "-c:a" << "alac"
             << "-y" << m4aPath;

        ffmpegProcess.start(ffmpegPath, args);
        if (!waitForProcess(&ffmpegProcess, QStringLiteral("ffmpeg stem conversion"))) {
            return false;
        }

        if (ffmpegProcess.exitStatus() != QProcess::NormalExit ||
                ffmpegProcess.exitCode() != 0) {
            kLogger.warning() << "ffmpeg failed for:" << stemName;
            return false;
        }
    }

    kLogger.info() << "All stems converted to M4A";
    return true;
}

bool StemConverter::convertTrackToM4A(const QString& inputPath, const QString& outputPath) {
    kLogger.info() << "Converting to M4A:" << inputPath;

    const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpegPath.isEmpty()) {
        kLogger.warning() << "ffmpeg executable not found";
        return false;
    }

    QProcess process;
    QStringList arguments;

    arguments << "-i" << inputPath;
    arguments << "-ar" << QString::number(kStemModelSampleRate);
    arguments << "-c:a" << "alac";
    arguments << "-y" << outputPath;

    process.start(ffmpegPath, arguments);

    if (!waitForProcess(&process, QStringLiteral("ffmpeg track conversion"))) {
        return false;
    }

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        QString errorOutput = process.readAllStandardError();
        kLogger.warning() << "ffmpeg error:" << errorOutput;
        return false;
    }

    kLogger.info() << "Converted to M4A:" << outputPath;
    return true;
}

bool StemConverter::createStemContainer(const QString& trackFilePath, const QString& stemsDir) {
    kLogger.info() << "Creating STEM container";

    const QString outputPath = getStemOutputPath(stemsDir);

    // Create mixdown M4A from original track
    QString mixdownM4A = stemsDir + "/mixdown.m4a";
    if (!convertTrackToM4A(trackFilePath, mixdownM4A)) {
        kLogger.warning() << "Failed to create mixdown M4A";
        return false;
    }

    // Create multi-track M4A with ffmpeg
    // Combine: mixdown.m4a + drums.m4a + bass.m4a + other.m4a + vocals.m4a
    const QString ffmpegPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpegPath.isEmpty()) {
        kLogger.warning() << "ffmpeg executable not found";
        return false;
    }

    QProcess ffmpegProcess;
    QStringList ffmpegArgs;

    ffmpegArgs << "-y";
    ffmpegArgs << "-i" << mixdownM4A;
    ffmpegArgs << "-i" << (stemsDir + "/drums.m4a");
    ffmpegArgs << "-i" << (stemsDir + "/bass.m4a");
    ffmpegArgs << "-i" << (stemsDir + "/other.m4a");
    ffmpegArgs << "-i" << (stemsDir + "/vocals.m4a");

    for (const QString& stemName : {QStringLiteral("drums"),
                 QStringLiteral("bass"),
                 QStringLiteral("other"),
                 QStringLiteral("vocals")}) {
        if (!QFile::exists(stemsDir + "/" + stemName + ".m4a")) {
            kLogger.warning() << "Stem M4A file not found:" << stemName;
            return false;
        }
    }

    // Map all audio tracks
    ffmpegArgs << "-map" << "0:a:0"; // mixdown
    ffmpegArgs << "-map" << "1:a:0"; // drums
    ffmpegArgs << "-map" << "2:a:0"; // bass
    ffmpegArgs << "-map" << "3:a:0"; // other
    ffmpegArgs << "-map" << "4:a:0"; // vocals

    ffmpegArgs << "-c:a" << "copy";
    ffmpegArgs << "-movflags" << "+faststart";
    ffmpegArgs << "-fflags" << "+bitexact";
    ffmpegArgs << outputPath;

    kLogger.info() << "Running ffmpeg to create multi-track M4A...";
    kLogger.info() << "Command: ffmpeg" << ffmpegArgs.join(" ");

    ffmpegProcess.start(ffmpegPath, ffmpegArgs);

    if (!waitForProcess(&ffmpegProcess, QStringLiteral("ffmpeg STEM container creation"))) {
        return false;
    }

    if (ffmpegProcess.exitStatus() != QProcess::NormalExit ||
            ffmpegProcess.exitCode() != 0) {
        QString errorOutput = ffmpegProcess.readAllStandardError();
        kLogger.warning() << "ffmpeg error:" << errorOutput;
        return false;
    }

    if (!QFile::exists(outputPath)) {
        kLogger.warning() << "ffmpeg reported success but did not create:" << outputPath;
        return false;
    }

    kLogger.info() << "Multi-track M4A created:" << outputPath;

    // Add STEM metadata atom
    if (!addStemMetadata(outputPath)) {
        kLogger.warning() << "Failed to add STEM metadata";
        return false;
    }

    kLogger.info() << "STEM container created:" << outputPath;
    return true;
}

QString StemConverter::createStemManifest() {
    QJsonObject masteringDsp;

    // Compressor settings
    QJsonObject compressor;
    compressor["enabled"] = false;
    compressor["ratio"] = 3;
    compressor["output_gain"] = 0.5;
    compressor["release"] = 0.3;
    compressor["attack"] = 0.003;
    compressor["input_gain"] = 0.5;
    compressor["threshold"] = 0;
    compressor["hp_cutoff"] = 300;
    compressor["dry_wet"] = 50;

    // Limiter settings
    QJsonObject limiter;
    limiter["enabled"] = false;
    limiter["release"] = 0.05;
    limiter["threshold"] = 0;
    limiter["ceiling"] = -0.35;

    masteringDsp["compressor"] = compressor;
    masteringDsp["limiter"] = limiter;

    // Stems metadata
    QJsonObject drums;
    drums["name"] = "Drums";
    drums["color"] = "#009E73";

    QJsonObject bass;
    bass["name"] = "Bass";
    bass["color"] = "#D55E00";

    QJsonObject other;
    other["name"] = "Other";
    other["color"] = "#CC79A7";

    QJsonObject vox;
    vox["name"] = "Vox";
    vox["color"] = "#56B4E9";

    QJsonArray stems;
    stems.append(drums);
    stems.append(bass);
    stems.append(other);
    stems.append(vox);

    QJsonObject root;
    root["mastering_dsp"] = masteringDsp;
    root["version"] = 1;
    root["stems"] = stems;

    QJsonDocument doc(root);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

QString StemConverter::findMP4Box() {
#ifdef Q_OS_LINUX
#ifdef MIXXX_FLATPAK
    const QString flatpakPrefix = QStringLiteral("/app");
    const QString flatpakMp4boxPath =
            QDir(flatpakPrefix).filePath(QStringLiteral("bin/MP4Box"));
    if (isTrustedFlatpakMP4BoxPath(flatpakMp4boxPath, flatpakPrefix)) {
        kLogger.info() << "Found package-owned Flatpak MP4Box:"
                       << flatpakMp4boxPath;
        return flatpakMp4boxPath;
    }

    kLogger.warning() << "Package-owned Flatpak MP4Box was not found under"
                      << flatpakPrefix;
    return {};
#else
    // tools/debian_buildenv.sh provides this dpkg-backed backend on Debian and
    // Ubuntu. Other Linux package managers intentionally fail closed.
    const QString mp4boxPath = findDpkgOwnedMp4Box();
    if (!mp4boxPath.isEmpty()) {
        kLogger.info() << "Found package-owned MP4Box:" << mp4boxPath;
        return mp4boxPath;
    }

    kLogger.warning() << "Package-owned MP4Box from the signed gpac package was not found";
    return {};
#endif
#else
    // The signed APT package contract applies only to the Debian/Linux setup.
    // Keep the existing platform-specific lookup for non-Linux builds.
    const QString mp4boxPath = QStandardPaths::findExecutable(QStringLiteral("MP4Box"));
    if (!mp4boxPath.isEmpty()) {
        kLogger.info() << "Found MP4Box in PATH:" << mp4boxPath;
        return mp4boxPath;
    }

    kLogger.warning() << "MP4Box not found in system";
    return "";
#endif
}

bool StemConverter::addStemMetadata(const QString& outputPath) {
    kLogger.info() << "Adding STEM metadata to:" << outputPath;

    QString mp4boxPath = findMP4Box();
    if (mp4boxPath.isEmpty()) {
        kLogger.warning() << "MP4Box not found";
        return false;
    }

    if (!QFile::exists(outputPath)) {
        kLogger.warning() << "Cannot add metadata to missing output:" << outputPath;
        return false;
    }

    // Create temporary JSON file with STEM manifest
    QString stemManifest = createStemManifest();

    QTemporaryFile tempJsonFile;
    if (!tempJsonFile.open()) {
        kLogger.warning() << "Failed to create temporary JSON file";
        return false;
    }

    const QByteArray manifestBytes = stemManifest.toUtf8();
    if (tempJsonFile.write(manifestBytes) != manifestBytes.size()) {
        kLogger.warning() << "Failed to write temporary STEM manifest";
        return false;
    }
    tempJsonFile.close();

    QString tempJsonPath = tempJsonFile.fileName();
    kLogger.info() << "Created temporary JSON at:" << tempJsonPath;

    // Run MP4Box to add STEM metadata
    QProcess mp4boxProcess;
    QStringList mp4boxArgs;

    mp4boxArgs << "-udta" << QString("0:type=stem:src=%1").arg(tempJsonPath);
    mp4boxArgs << outputPath;

    kLogger.info() << "Running MP4Box:" << mp4boxPath << mp4boxArgs.join(" ");
    mp4boxProcess.start(mp4boxPath, mp4boxArgs);

    if (!waitForProcess(&mp4boxProcess, QStringLiteral("MP4Box metadata update"))) {
        QFile::remove(tempJsonPath);
        return false;
    }

    if (mp4boxProcess.exitStatus() != QProcess::NormalExit ||
            mp4boxProcess.exitCode() != 0) {
        QString errorOutput = mp4boxProcess.readAllStandardError();
        kLogger.warning() << "MP4Box error:" << errorOutput;
        QFile::remove(tempJsonPath);
        return false;
    }

    QString standardOutput = mp4boxProcess.readAllStandardOutput();
    kLogger.info() << "MP4Box output:" << standardOutput;

    if (!QFile::exists(outputPath)) {
        kLogger.warning() << "MP4Box reported success but output is missing:" << outputPath;
        return false;
    }

    kLogger.info() << "STEM metadata added successfully";
    return true;
}

#include "moc_stemconverter.cpp"
