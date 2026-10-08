#pragma once

#include <QString>

#include "util/assert.h"

// Test-only Event tracing replacement. The real scheduler calls Event::end
// after the worker scan and outside its worker-list mutex.
void schedulerScanFinishedForTest();

class Event {
  public:
    static bool start(const QString&) {
        return true;
    }
    static bool end(const QString&) {
        schedulerScanFinishedForTest();
        return true;
    }
};
