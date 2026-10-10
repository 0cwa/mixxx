# Bungee Keylock Engine Integration

This document describes how Mixxx integrates the [Bungee][bungee] real-time
audio time-stretching library as an optional keylock (pitch-independent tempo)
engine.

[bungee]: https://github.com/kupix/bungee

---

## Architecture overview

<!-- AI-generated ownership clarification begins. -->

```text
EngineBuffer
  ├── owns ReadAheadManager
  ├── owns m_pBungeeWorker (EngineBufferBungeeWorker)
  │     ├── owns current and retired EngineBufferScaleBungee instances
  │     │     ├── borrows ReadAheadManager
  │     │     └── owns Bungee::Stretcher
  │     └── owns current and retired published state records
  └── audio callback borrows state/scaler pointers within a reader slot
```

`EngineBufferScaleBungee` implements the `EngineBufferScale` interface.
When the user picks *"Bungee (high quality)"* from
**Preferences → Sound → Keylock engine**, callback selection resolves a prepared
scaler from the worker's publication. The callback uses a borrowed `m_pScale`
pointer; `m_pScaleKeylock` also serves as a diagnostic/test mirror and supports
test overrides.

The worker owns both scalers and their immutable publication records. A record
contains a non-owning scaler pointer, sample rate, and channel count. The scaler
borrows the `ReadAheadManager` owned by `EngineBuffer`. Replacement preparation
and worker-owned object destruction happen outside the audio callback. See
[Immutable publication, acknowledgement, and retry](#immutable-publication-acknowledgement-and-retry).

<!-- End AI-generated ownership clarification. -->

---

## Immutable publication, acknowledgement, and retry

<!-- AI-generated publication and retry clarification begins. -->

The callback acquires a reader slot before accessing published Bungee state and
releases that slot at its boundary. It can load the publication more than once,
including during layout handling, keylock selection, and boundary acknowledgement.
The reader slot protects its borrowed pointers while the worker retains the
current and retired objects.

The worker retains at most one retired state/scaler pair. Reclamation requires
both a zero callback-reader count and an acknowledged state pointer different
from the retired state. At the callback boundary, `finishBungeeCallback()` keeps
acknowledging the borrowed state while `m_pScale` still uses its scaler; otherwise
it acknowledges the current publication. It then releases the reader slot and
marks the worker ready. The acknowledgement uses state-pointer identity; the
configuration generation counter separately rejects stale replacements before publication.
While reclamation is blocked, the worker returns without repeatedly waking itself.

The Bungee publication and acknowledgement protocol uses lock-free atomics and
does not make the callback wait for preparation or destroy worker-owned objects.
This describes that protocol: the broader `EngineBuffer` callback still uses its
existing `m_pause.tryLock()`/`unlock()` around track processing.

Read-ahead misses use a scaler-local retry state. A retryable miss keeps the
pending input range and returns muted output without calling `next()` or advancing
the Bungee request cursor. A later callback retries the same request. Reset can
occur on the audio path: `clear()` calls `completePendingGrainForReset()`, which
cancels that scaler's retry and completes the pending grain with muted input from
preallocated storage before resetting the request.

Worker-owned reclamation and `EngineBuffer` teardown occur outside the callback.
The scaler destructor is defaulted; it has no separate teardown retry-cancellation
call. `EngineBuffer` stops and destroys the worker before deleting the borrowed
`ReadAheadManager`.

<!-- End AI-generated publication and retry clarification. -->
