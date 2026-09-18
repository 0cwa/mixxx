#pragma once

#include <QMap>
#include <QString>
#include <cstdint>

#include "audio/frame.h"
#include "track/track_decl.h"
#include "util/color/rgbcolor.h"

class QSqlDatabase;

namespace mixxx::rekordbox {

constexpr bool isValidDatabaseId(int id) {
    return id > 0;
}

bool isWritableDatabase(const QSqlDatabase& database);

bool importPlaylistTracks(QSqlDatabase& database,
        int playlistID,
        const QMap<uint32_t, uint32_t>& playlistTracks,
        const QString& device);

void importMemoryCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        const QString& label,
        mixxx::RgbColor::optional_t color);

void importHotCue(TrackPointer track,
        mixxx::audio::FramePos startPosition,
        mixxx::audio::FramePos endPosition,
        int id,
        const QString& label,
        mixxx::RgbColor::optional_t color);

} // namespace mixxx::rekordbox
