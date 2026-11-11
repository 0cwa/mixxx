// Prepared by ChatGPT for human review.
#pragma once

#include <QDateTime>
#include <QHash>
#include <QSqlDatabase>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <memory>
#include <optional>

#include "library/rekordbox/rekordboximportregistry.h"
#include "library/rekordbox/rekordboxxmlparser.h"

namespace mixxx::rekordbox {

enum class SourceKind { Usb,
    Xml };

struct SourceDescriptor {
    QString id;
    SourceKind kind = SourceKind::Usb;
    QString locator;
    QString displayName;
    QString volumeIdentity;
};

struct SourceRequest {
    SourceDescriptor source;
    qint64 generation = 0;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::shared_ptr<const void> accessLease;

    bool isCancelled() const {
        return cancelled && cancelled->load(std::memory_order_relaxed);
    }
};

struct ExternalTrack {
    QString sourceTrackId;
    Track metadata;
    QString analyzePath;
    std::optional<quint32> color;
};

struct ExternalPlaylist {
    QString id;
    QString parentId;
    QString name;
    bool folder = false;
    QVector<QString> trackIds;
};

struct SourceSnapshot {
    SourceDescriptor source;
    qint64 generation = 0;
    QString fingerprint;
    QString sourcePath;
    qint64 sourceFileSize = -1;
    QDateTime sourceModified;
    QVector<ExternalTrack> tracks;
    QVector<ExternalPlaylist> playlists;
    std::shared_ptr<const Library> xml;
    std::shared_ptr<const void> accessLease;
};

struct SourceResult {
    SourceRequest request;
    ImportOutcome outcome = ImportOutcome::Failed;
    QStringList diagnostics;
    std::shared_ptr<const SourceSnapshot> snapshot;
};

struct SourceSession {
    SourceDescriptor source;
    qint64 generation = 0;
    bool available = false;
    bool loading = false;
    ImportOutcome lastOutcome = ImportOutcome::Unavailable;
    QStringList diagnostics;
    std::shared_ptr<std::atomic_bool> cancelled;
    std::shared_ptr<const SourceSnapshot> snapshot;
};

// A USB mount path cannot prove device continuity. Unknown or conflicting
// filesystem identities require an explicit user binding.
bool matchesDiscoveredSource(const SourceSession& existing, const SourceDescriptor& discovered);

// Owned by the GUI thread. Workers receive only copyable requests and snapshots.
class SourceSessions final {
  public:
    bool add(const SourceDescriptor& source, bool available);
    std::optional<SourceRequest> begin(const QString& sourceId,
            std::shared_ptr<const void> accessLease = {},
            qint64 generation = 0);
    bool current(const SourceRequest& request) const;
    bool finish(const SourceResult& result);
    bool relocate(const QString& sourceId, const QString& locator);
    void unavailable(const QString& sourceId);
    void remove(const QString& sourceId);
    void cancelAll();
    const SourceSession* get(const QString& sourceId) const;
    QVector<SourceSession> all() const;
    QVector<QString> missingUsbSources(const QVector<QString>& presentIds) const;
    bool loading() const;

  private:
    QHash<QString, SourceSession> m_sources;
};

QString externalPlaylistKey(const QString& sourceId, const QString& playlistId = {});
bool createExternalSourceTables(QSqlDatabase database, QString* error);
bool publishExternalSource(QSqlDatabase database,
        const SourceSnapshot& snapshot,
        QString* error);
bool removeExternalSource(QSqlDatabase database, const QString& sourceId, QString* error);

} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
