// Prepared by ChatGPT for human review.
#pragma once

#include "library/rekordbox/rekordboxsource.h"

namespace mixxx::rekordbox {
QString rekordboxPdbPath(const QString& root);
QString xmlSourceTrackId(const Library& library, int trackIndex);
SourceResult decodeSource(const SourceRequest& request);
} // namespace mixxx::rekordbox
// End of code prepared by ChatGPT for human review and submission.
