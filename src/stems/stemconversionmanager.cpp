#include "stems/stemconversionmanager.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRunnable>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVariant>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("StemConversionManager");
constexpr int kMaxHistoryItems = 100;
} // namespace

class StemConversionTask : public QRunnable {
  public:
    StemConversionTask(StemConverterPointer pConverter,
            TrackPointer pTrack,
            StemConverter::Resolution resolution)
            : m_pConverter(pConverter),
              m_pTrack(pTrack),
              m_resolution(resolution) {
    }

    void run() override {
        if (!m_pConverter || !m_pTrack) {
            return;
        }

        // Execute the conversion in the thread pool with the specified resolution
        m_pConverter->convertTrack(m_pTrack, m_resolution);
    }

  private:
    StemConverterPointer m_pConverter;
    TrackPointer m_pTrack;
    StemConverter::Resolution m_resolution;
};

StemConversionManager::StemConversionManager(QObject* parent)
        : QObject(parent),
          m_pThreadPool(std::make_unique<QThreadPool>()),
          m_currentTrackId(TrackId()) {
    // Create thread pool for asynchronous conversions
    m_pThreadPool->setMaxThreadCount(1); // One conversion at a time

    // Define history file path
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir configDir(configPath);
    if (!configDir.exists()) {
        configDir.mkpath(".");
    }
    m_historyFilePath = configDir.filePath("stem_conversion_history.json");

    loadHistory();

    kLogger.info() << "StemConversionManager initialized";
}

StemConversionManager::~StemConversionManager() {
    // Conversion tasks hold raw signal receivers and must finish before this
    // manager is destroyed. QThreadPool owns and deletes its QRunnables.
    m_pThreadPool->waitForDone();
    {
        QMutexLocker locker(&m_statusMutex);
        m_pCurrentConverter.reset();
    }
    saveHistory();
}

void StemConversionManager::convertTrack(const TrackPointer& pTrack,
        StemConverter::Resolution resolution) {
    if (!StemConverter::isValidConversionInput(pTrack)) {
        kLogger.warning() << "Cannot convert null or location-less track";
        return;
    }

    bool startNext = false;
    int pendingCount = 0;
    {
        QMutexLocker locker(&m_statusMutex);
        m_conversionQueue.append(PendingConversion{pTrack, resolution});
        pendingCount = m_conversionQueue.size();
        startNext = !m_pCurrentConverter;
    }
    emit queueChanged(pendingCount);

    // If no conversion is in progress, start the next one
    if (startNext) {
        processNextInQueue();
    }
}

void StemConversionManager::processNextInQueue() {
    PendingConversion pending;
    int pendingCount = 0;
    {
        QMutexLocker locker(&m_statusMutex);
        if (m_conversionQueue.isEmpty()) {
            m_currentTrackId = TrackId();
            m_currentTrackPath.clear();
            m_pCurrentConverter.reset();
        } else {
            pending = m_conversionQueue.takeFirst();
            m_currentTrackId = pending.pTrack->getId();
            m_currentTrackPath = pending.pTrack->getLocation();
            pendingCount = m_conversionQueue.size();
        }
    }
    if (!pending.pTrack) {
        emit queueChanged(0);
        return;
    }

    // Get the first track from the queue
    const TrackPointer pTrack = pending.pTrack;
    const StemConverter::Resolution resolution = pending.resolution;

    // Create the converter
    StemConverterPointer pConverter = std::make_shared<StemConverter>();

    // Connect the converter signals
    connect(pConverter.get(),
            &StemConverter::conversionStarted,
            this,
            &StemConversionManager::onConversionStarted);
    connect(pConverter.get(),
            &StemConverter::conversionProgress,
            this,
            &StemConversionManager::onConversionProgress);
    connect(pConverter.get(),
            &StemConverter::conversionCompleted,
            this,
            &StemConversionManager::onConversionCompleted);
    connect(pConverter.get(),
            &StemConverter::conversionFailed,
            this,
            &StemConversionManager::onConversionFailed);

    // Store the converter before starting the task so an invalid TrackId does
    // not make a temporary-track conversion appear idle.
    {
        QMutexLocker locker(&m_statusMutex);
        m_pCurrentConverter = pConverter;
    }

    // Execute the conversion in a separate thread (asynchronous)
    auto pTask = std::make_unique<StemConversionTask>(pConverter, pTrack, resolution);
    m_pThreadPool->start(pTask.release());

    emit queueChanged(pendingCount);
}

void StemConversionManager::onConversionStarted(TrackId trackId, const QString& trackTitle) {
    {
        QMutexLocker locker(&m_statusMutex);
        m_currentTrackId = trackId;
        if (m_pCurrentConverter) {
            m_currentTrackPath = m_pCurrentConverter->getTrackPath();
        }
    }
    emit conversionStarted(trackId, trackTitle);
}

void StemConversionManager::onConversionProgress(
        TrackId trackId, float progress, const QString& message) {
    emit conversionProgress(trackId, progress, message);
}

void StemConversionManager::onConversionCompleted(TrackId trackId) {
    kLogger.info() << "Conversion completed for track:" << trackId;
    const QString trackTitle = recordFinishedConversion(
            trackId, StemConverter::ConversionState::Completed, 1.0f);

    emit conversionCompleted(trackId, trackTitle);

    // Process next in queue
    {
        QMutexLocker locker(&m_statusMutex);
        m_currentTrackId = TrackId();
        m_currentTrackPath.clear();
        m_pCurrentConverter.reset();
    }
    processNextInQueue();
}

void StemConversionManager::onConversionFailed(TrackId trackId, const QString& errorMessage) {
    kLogger.warning() << "Conversion failed for track:" << trackId << "Error:" << errorMessage;
    const QString trackTitle = recordFinishedConversion(
            trackId, StemConverter::ConversionState::Failed, 0.0f);

    emit conversionFailed(trackId, trackTitle, errorMessage);

    // Process next in queue
    {
        QMutexLocker locker(&m_statusMutex);
        m_currentTrackId = TrackId();
        m_currentTrackPath.clear();
        m_pCurrentConverter.reset();
    }
    processNextInQueue();
}

std::optional<StemConversionManager::ConversionInfo>
StemConversionManager::getCurrentConversion() const {
    QMutexLocker locker(&m_statusMutex);
    if (!m_pCurrentConverter) {
        return std::nullopt;
    }

    ConversionInfo info;
    info.trackId = m_currentTrackId;
    info.trackPath = m_currentTrackPath;
    info.trackTitle = m_pCurrentConverter->getTrackTitle();
    info.progress = m_pCurrentConverter->getProgress();

    return info;
}

QList<StemConversionManager::ConversionStatus> StemConversionManager::getConversionHistory() const {
    QMutexLocker locker(&m_statusMutex);
    return m_conversionHistory;
}

void StemConversionManager::clearConversionHistory() {
    {
        QMutexLocker locker(&m_statusMutex);
        m_conversionHistory.clear();
    }
    saveHistory();
}

QString StemConversionManager::recordFinishedConversion(
        TrackId trackId, StemConverter::ConversionState state, float progress) {
    QString trackTitle = QStringLiteral("Unknown");
    QString trackPath;

    {
        QMutexLocker locker(&m_statusMutex);
        if (m_pCurrentConverter) {
            trackTitle = m_pCurrentConverter->getTrackTitle();
            trackPath = m_pCurrentConverter->getTrackPath();
        }

        ConversionStatus status;
        status.trackId = trackId;
        status.trackPath = trackPath;
        status.trackTitle = trackTitle;
        status.state = state;
        status.progress = progress;
        m_conversionHistory.prepend(status);

        if (m_conversionHistory.size() > kMaxHistoryItems) {
            m_conversionHistory.removeLast();
        }
    }
    saveHistory();
    return trackTitle;
}

void StemConversionManager::loadHistory() {
    QFile historyFile(m_historyFilePath);
    if (!historyFile.open(QIODevice::ReadOnly)) {
        kLogger.info() << "No conversion history file found. Starting fresh.";
        return;
    }

    QByteArray data = historyFile.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isArray()) {
        kLogger.warning() << "Conversion history file is corrupted.";
        return;
    }

    m_conversionHistory.clear();
    QJsonArray historyArray = doc.array();
    for (const QJsonValue& value : historyArray) {
        QJsonObject obj = value.toObject();
        ConversionStatus status;
        const int trackId = obj["track_id"].toInt(-1);
        if (trackId >= 0) {
            status.trackId = TrackId(QVariant(trackId));
        }
        status.trackPath = obj["path"].toString();
        status.trackTitle = obj["title"].toString();
        const int state = obj["state"].toInt(-1);
        if (state < static_cast<int>(StemConverter::ConversionState::Idle) ||
                state > static_cast<int>(StemConverter::ConversionState::Failed)) {
            continue;
        }
        status.state = static_cast<StemConverter::ConversionState>(state);
        status.progress = static_cast<float>(obj["progress"].toDouble());
        m_conversionHistory.append(status);
    }
    kLogger.info() << "Loaded" << m_conversionHistory.size() << "items from conversion history.";
}

void StemConversionManager::saveHistory() {
    QList<ConversionStatus> history;
    {
        QMutexLocker locker(&m_statusMutex);
        history = m_conversionHistory;
    }

    QJsonArray historyArray;
    for (const auto& status : history) {
        QJsonObject obj;
        if (status.trackId.isValid()) {
            obj["track_id"] = QJsonValue::fromVariant(status.trackId.toVariant());
        }
        obj["path"] = status.trackPath;
        obj["title"] = status.trackTitle;
        obj["state"] = static_cast<int>(status.state);
        obj["progress"] = status.progress;
        historyArray.append(obj);
    }

    const QByteArray serializedHistory = QJsonDocument(historyArray).toJson();
    QSaveFile historyFile(m_historyFilePath);
    if (!historyFile.open(QIODevice::WriteOnly)) {
        kLogger.warning() << "Could not write to conversion history file:" << m_historyFilePath;
        return;
    }
    if (historyFile.write(serializedHistory) != serializedHistory.size()) {
        kLogger.warning() << "Could not write conversion history file:" << m_historyFilePath;
        historyFile.cancelWriting();
        return;
    }
    if (!historyFile.commit()) {
        kLogger.warning() << "Could not commit conversion history file:" << m_historyFilePath;
    }
}

#include "moc_stemconversionmanager.cpp"
