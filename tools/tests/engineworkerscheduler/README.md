<!-- AI-generated review documentation begins. -->

# Standalone scheduler regression

This bounded native target compiles the checkout's actual `EngineWorker` and
`EngineWorkerScheduler` with QtCore and GoogleTest. Only Event tracing and
assertion support are replaced by test stubs. It does not launch Mixxx, use an
audio device, or exercise the full mixer or reader integration.

```sh
cmake -S tools/tests/engineworkerscheduler -B build-scheduler \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-scheduler
ctest --test-dir build-scheduler --output-on-failure
```

The tests force a notification after the worker scan and before the scheduler
parks, repeat 10,000 ready cycles with duplicate notifications, and stop the
scheduler while a worker remains blocked. Shutdown also accepts late
notifications and a repeated stop.

For the negative control, extract the worker and scheduler sources plus
`src/util/compatibility/qmutex.h` from Forest commit
`06a324bd8239f97d9c9a5360a3d81607c73b3194` into a separate source directory.
Configure another build with `-DMIXXX_SOURCE_ROOT=/path/to/extracted-source`
and run only `EngineWorkerSchedulerTest.RetainsNotificationAfterWorkerScan`.
The old scheduler loses the forced notification and fails both wake assertions;
the guard releases the scan barrier and joins the scheduler on failure.

The production fix is adapted from public commit
`655929d0f25838b73d2d32c8d74b6b8d7b13d26a` (also present in
`53138f490e9525ee586bac40e8d61a7aa40c8dba`). The original forced-gap harness
comes from `8693f19e77953c07e65947dd232466334099734d`.

These checks do not certify real-time performance, the full Forest feature
set, or native Windows/macOS behavior. The existing headless profile retains
its independent runtime gates, including the unresolved Media Foundation
late-seek counterexample. No hosted workflow is added or invoked.

<!-- End AI-generated review documentation. -->
