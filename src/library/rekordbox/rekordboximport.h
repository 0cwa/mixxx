#pragma once

#include <QMap>
#include <QSqlDatabase>
#include <QString>
#include <cstdint>

#include "audio/frame.h"
#include "track/track_decl.h"
#include "util/color/rgbcolor.h"

namespace mixxx::rekordbox {

bool isWritableDatabase(const QSqlDatabase& database);

constexpr bool isValidDatabaseId(int id) {
    return id > 0;
}

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
