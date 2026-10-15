#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QString>
#include <functional>
#include <memory>
#include <utility>

#include "effects/defs.h"
#include "util/compatibility/qmutex.h"

namespace AudioUnitManifestDiscovery {

class State final {
  public:
    void storeManifest(QString effectId, EffectManifestPointer manifest) {
        auto locker = lockMutex(&m_mutex);
        m_manifestsById.insert(std::move(effectId), std::move(manifest));
    }

    QList<QString> effectIds() const {
        auto locker = lockMutex(&m_mutex);
        return m_manifestsById.keys();
    }

    EffectManifestPointer manifest(const QString& effectId) const {
        auto locker = lockMutex(&m_mutex);
        return m_manifestsById.value(effectId);
    }

    QList<EffectManifestPointer> manifests() const {
        auto locker = lockMutex(&m_mutex);
        return m_manifestsById.values();
    }

  private:
    QHash<QString, EffectManifestPointer> m_manifestsById;
    mutable QMutex m_mutex;
};

using StatePointer = std::shared_ptr<State>;
using ManifestPublisher = std::function<void(EffectManifestPointer)>;

inline ManifestPublisher makeManifestPublisher(
        const StatePointer& state, QString effectId) {
    return [state, effectId = std::move(effectId)](
                   EffectManifestPointer manifest) {
        state->storeManifest(effectId, std::move(manifest));
    };
}

} // namespace AudioUnitManifestDiscovery
