# Bungee Keylock Engine Integration

This document describes how Mixxx integrates the [Bungee][bungee] real-time
audio time-stretching library as an optional keylock (pitch-independent tempo)
engine.

[bungee]: https://github.com/kupix/bungee

---

## Architecture overview

```text
EngineBuffer
  ├── m_pBungeeWorker  (EngineBufferBungeeWorker)
  │     └── owns EngineBufferScaleBungee and its published state record
  │           ├── ReadAheadManager   — pulls decoded audio frames
  │           └── Bungee::Stretcher  — time-stretch / pitch-shift
  └── audio callback — borrows the scaler through the published state
```

`EngineBufferScaleBungee` implements the `EngineBufferScale` interface.
`EngineBuffer` selects it as `m_pScaleKeylock` when the user picks
*"Bungee (high quality)"* from **Preferences → Sound → Keylock engine**.
The worker prepares replacements outside the audio callback and publishes an
immutable state record. The callback acknowledges the state it used; the worker
retains the previous state and scaler until that acknowledgement permits safe
reclamation. See [Immutable publication, acknowledgement, and retry](#immutable-publication-acknowledgement-and-retry).

---

## Immutable publication, acknowledgement, and retry

Sample-rate and channel-count changes are prepared outside the audio callback.
The preparation worker owns each Bungee scaler and publishes an immutable state
record containing the scaler and its signal format. The callback reads one
published state for the duration of a callback; it never allocates, locks, or
destroys a scaler while rendering.

When a replacement is published, the worker retains the previous state until
the callback has acknowledged the state epoch it used. This acknowledgement
keeps the old scaler alive across the callback boundary and makes replacement
safe even when a callback observes a configuration change at the same time as
the worker publishes it. A generation counter causes a stale in-flight
configuration to be discarded before publication.

Read-ahead misses use a scaler-local retry state. A retryable read keeps the
pending input range and returns muted output for that callback without calling
`next()` or advancing the Bungee request cursor. The same request is retried on
the next callback; reset and teardown acknowledge or cancel the pending retry
off the audio path before releasing its state.
