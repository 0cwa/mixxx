# Prepared by ChatGPT: legacy rekordbox reader delivery

This change develops the B lane of the supplied legacy rekordbox handoff on PR26.
It provides bounded PDB and shared ANLZ decoders, a provisional application adapter, cue and beat persistence, and memory/downbeat control fixes.
It does not qualify an exporter/player combination or enable a USB writer.
The PR targets `pr/rekordbox-v1-ci-fixes` so reviewers can inspect this change separately from PR26's existing integration work.

## Revision and scope

- Base: [`78f561f9858cc20c0f36bc5fd6a3468b616f84db`](https://github.com/0cwa/mixxx/commit/78f561f9858cc20c0f36bc5fd6a3468b616f84db), the refreshed head of [fork PR26](https://github.com/0cwa/mixxx/pull/26).
- PR26's target was `mixxx/main` at `6f831813c933d79070600c4b0d47f3318340b0d7`.
- The source identity of this delivery is its Git tree and PR head; [the source manifest](REKORDBOX_B_SOURCES.json) lists every changed implementation, schema, build-registration, and test file with its exact SHA-256.
  Qualification below applies to that source tree, not an earlier donor's reported test results.
- No other orchestrator's conversation, filesystem, or unprovided work was accessed.
- Changes to `rekordboxfeature.*` and central CMake are provisional integration work for A, included here because A was unavailable.
- C's OneLibrary/native adapters, dependency packaging, and qualified exporter corpus remain separate.
- Repository policy files, vendor schemas, generated Kaitai sources, and upstream PR threads were not changed.

## Contract B-read/1 for A and C

The public seams are `rekordboxpdbdecoder.h`, `rekordboxanlzdecoder.h`, and `rekordboxanlzimporter.h`.
The decoders accept caller-owned bounded byte views, return owned immutable data, and perform no file, SQL, Track, or sidebar mutation.
The file/application adapters are separate and are not real-time audio callbacks.

| Concept | Representation and integration rule |
| --- | --- |
| Source identity | Caller-provided source identity and provenance; a path is a locator, not durable device identity. |
| Source generation | PDB's raw database sequence is retained; exact input bytes are SHA-256 fingerprinted separately by the adapter. Neither is a Mixxx TrackId. |
| Media identity | Canonical media location is resolved by the adapter; source track IDs remain unsigned 32-bit values and map separately to temporary SQL IDs. |
| Cue role | Explicit memory cue, memory loop, hot cue, or hot loop; an optional zero-based hot slot is independent of role. High slots remain hot cues. |
| Cue contents | Full interval, optional label, RGB/palette fields, optional active-loop intent, source section/ordinal, and all reconciled provenance aliases. |
| Unknown values | Missing optional metadata differs from explicit empty text, zero, an unassigned color, and inactive loop intent. |
| Timing | Raw ANLZ integer milliseconds and tempo hundredths are retained. Conversion uses sample rate once, without channel count or a minimum-time clamp. |
| Beats | Explicit timestamp/BPM/beat-in-bar entries remain explicit beats. A must retain XML/native tempo-segment anchors separately rather than treating them as sampled beat timestamps. |
| Outcomes | Success, warning, unsupported, invalid, unavailable, denied, cancelled, and failed are explicit. Invalid/cancelled/failed decodes do not publish partial records. |
| Application ownership | A owns durable source identity, provenance, conflict policy, removals, and successful-generation tracking. The included adapter only claims unchanged cues created by its current Track state. |

C should call `decodeAnlzCompanions` rather than copy a second ANLZ implementation.
A should replace the provisional in-memory ownership map with its durable planner while preserving these distinctions.
A process/model restart therefore preserves occupied user slots and existing grids conservatively; it does not claim persisted import ownership.
An ambiguous split/reconvergence of previously reconciled memory provenance remains incomplete for A to resolve, rather than deleting annotations or caching false success.

## Implemented behavior

### B01: bounded decoding and application separation

PDB decoding limits file size, page size, table/page/row counts, index/heap/string extents, decoded record/string storage, retained unknown bytes, and playlist depth.
It rejects cycles, shared page chains, invalid references, malformed UTF-16, and truncated structures before invoking the generated parser on independently validated pages.
The final page's next pointer is not followed beyond the declared table boundary.
Only allocated row offsets and flags consume a partial row-index group; unused offsets may still contain heap data.

ANLZ decoding bounds file/section/cue/beat/string sizes and combined retained data.
It checks cancellation and converts callback exceptions to explicit failures.
Unknown structures and file headers can be retained within limits for future writing; the application cache disables raw-byte retention.
This retained material is not a complete writable PDB image or a USB publication implementation.

The PDB worker reads bytes, decodes immutable records, and commits temporary SQL rows transactionally.
It does not mutate the sidebar; the GUI applies the successful result and builds the playlist hierarchy.
Sandbox permission requests happen before worker launch, and the security token survives the read.
An operation remains pending until its completion handler has accepted the result, including the interval after the worker stops running.
Cancellation or SQL failure leaves no partially committed import; a failed source remains retryable.

### B02: cues, loops, ordering, and colors

Complete legacy `PCOB/PCPT` and extended `PCO2/PCP2` fixtures cover hot number 1, loop type, and 1000–2000 ms.
At 48 kHz and zero correction, both produce slot 0, Loop type, and frames 48000–96000.
Point-to-loop and loop-to-point updates change both type and the complete interval while preserving the owned Cue object.
Sparse/high slots remain hot cues, occupied user slots remain untouched, and memory loops use `CueType::Memory` plus a valid end and no hot slot.
Main Cue promotion retains the original memory cue and respects local edits/deletions.

Cue ordering compares color presence before numeric RGB and obeys strict ordering for equal-time/equal-label records with different present colors.
Companion reconciliation prefers richer records and records all matched provenance without collapsing intentionally distinct same-time annotations.
Split provenance does not update one Cue twice.

The audited oversized palette literal `0xF870900` is rejected before `RgbColor` can mask it to `0x870900`.
No intended replacement red was guessed.
Palette ID 0 is explicit unassigned color; a missing palette value remains unknown.
Issue [#90](https://github.com/0cwa/mixxx/issues/90) still needs trustworthy palette evidence before a replacement value can be qualified.

### B03: precedence, retry, and timing

DAT beat data has priority; richer `PCO2` cue metadata wins over compatible legacy records, with deterministic EXT/2EX precedence and provenance.
A corrupt or missing companion can fall back to usable data with diagnostics.
A cancelled/failed companion aborts the aggregate; unsupported capabilities remain explicit.
Same-path byte changes, corrupt-then-repaired files, companion removal, sample-rate/correction changes, and promotion-policy changes invalidate the appropriate cached result.
Decode reuse and successful application are separate decisions.
Temporarily unavailable files retain live Track ownership snapshots, including local deletion protection.

| ANLZ content | Capability |
| --- | --- |
| `PCOB`, `PCO2` | Decode cue roles, intervals, supported metadata, and provenance. |
| `PQTZ` | Decode explicit beat times, BPM, and beat-in-bar. |
| `PPTH` | Decode media path. |
| `PWAV`, `PWV2` through `PWV7` | Explicit regeneration capability; this change does not reuse vendor waveform data. |
| `PSSI`, `PVBR`, unknown sections | Explicit unsupported capability; bounded raw preservation is available. |

Zero and negative/pre-roll frame positions are valid.
The existing backend-specific MP3/M4A correction table remains in place and is applied once.
A single-beat constant grid validates BPM and rounds the destination marker consistently with Mixxx's beat-position construction; raw source timing remains intact.
**No new PCM/MP3/M4A alignment measurement was performed against a known rekordbox reference across CoreAudio/MAD/FFmpeg.**
Those codec/backend/sample-rate combinations are not newly qualified by arithmetic regression tests.

### B04: durable cue and grid semantics

BeatMap protobuf field 2 stores an optional downbeat offset with default zero, preserving compatibility with older messages.
Equality includes phase and sample rate, and the legacy fallback cannot inherit offset state from a partially parsed protobuf.
Track downbeat updates honor the existing BPM lock, retain the accepted grid's cached phase, handle null/clear/undo, and emit dirty/change state only after acceptance.
The import path does not bypass `trySetBeats`' existing lock enforcement.

Schema 41 adds nullable `cues.active_loop` with a NULL/0/1 constraint.
Cue/CueInfo/DAO preserve unknown, false, and true independently alongside role, slot, label, color, and loop interval.
A file-backed DAO test uses four complete database lifetimes to verify constant/variable grid phase and all four cue roles after close/reopen, subsequent edits, and another reopen.
A and C must coordinate the schema version before combining concurrent migrations.

### B05: performance controls and rendering

Memory controls select genuine memory roles, refresh their cue view on each action, and keep points free of loop endpoints and hot slots.
Explicit memory-loop navigation recalls its interval and enables it only when active intent is known true; loading a track does not activate a loop.
Downbeat changes and memory mutations run on the QObject owner thread.
Cross-thread control callbacks use bounded atomic handoff without Track/database/file operations or GUI mutation.
Stale commands cannot change a replacement track.
Memory commands use 64 total preallocated records and preserve accepted publication order across command kinds/producers; GUI commands share the queue and drain immediately, while worker commands drain on a 20 ms timer.
Commands capture the trigger-time frame and queued-seek snapshots; deferred creation, quantization, deletion, and navigation retain that position even if playback advances before dispatch.
Downbeat deltas combine modulo 16 because their operations commute.

Variable-tempo downbeat renderers use a global beat index across tempo markers instead of restarting the phase at every tempo change.
This change does not import upstream's high-hot-slot classification or add a new skin design.
Controller hardware, real-time latency under load, screenshot review of every skin/renderer, and physical deck/preview operation remain unqualified.

### B06: metadata and hierarchy

The PDB record model retains source IDs, key text/reference identity, track strings/numeric metadata, artwork references, folders, playlist sort positions, and repeated/sparse membership.
The SQL adapter normalizes key notation instead of storing the source key ID as a Mixxx key enum.
It resolves media/analysis/artwork paths within the device root, including existing symlink ancestors of missing targets.
Playlist identity uses source IDs separately from display labels, preserving duplicate labels and empty folders.
Source cleanup compares literal path prefixes, preserves rows still referenced by another source, and removes orphaned temporary rows regardless of their original display label.
Device discovery compares source paths rather than display labels.

The provisional temporary SQL view keeps the first imported metadata/analysis locator for a reused canonical media row; all source records remain available in the pure decoded model.
A must integrate per-source metadata/analysis selection with its durable identity and conflict policy.
History and other unsupported table types produce explicit diagnostics; history import is not implemented.
One canonical-location index is built per import instead of rescanning all rows for each track.
Pure-decoder synthetic measurements covered 10,000 and 100,000 records; they are not full SQL/UI benchmarks or an old-importer baseline.

## Donor audit

The actual latest patches were inspected; historical PR descriptions were not treated as executed evidence.

| Source | Inspected head | Decision |
| --- | --- | --- |
| [Fork #47](https://github.com/0cwa/mixxx/pull/47) | `d765535aa276bf24ff37e2175d0eb22b3397a104` | Recover relevant phase behavior once; it already includes #68/#77. Add persistence/lock/equality regressions. |
| [Fork #35](https://github.com/0cwa/mixxx/pull/35) | `4e8269aee93516d29ac42951e1e292ecd59d8f0f` | Key normalization was already present in PR26; preserve it. |
| [Fork #46](https://github.com/0cwa/mixxx/pull/46) | `9f6fa7d4f2e6e97214f372e04e926de724cee1e8` | Existing guards already present; replace swallowed failures with structured outcomes. |
| [Fork #59](https://github.com/0cwa/mixxx/pull/59) | `7cba229082fc19d9b1d0589ccba6b9f9dea1377f` | Same existing-cue type update as #76; recover once. |
| [Fork #65](https://github.com/0cwa/mixxx/pull/65) | `e756b882f4b1bec4cda0a7268aefe063bc6f2e50` | Color assertions inform tests, not fixture qualification. |
| [Fork #66](https://github.com/0cwa/mixxx/pull/66) | `a7485defada99fce760d978be4fb6d3eb7518da1` | Adapt synthetic PQTZ coverage to the pure reader. |
| [Fork #67](https://github.com/0cwa/mixxx/pull/67) | `3334996d90a0fe3f8aed3c1668ef98c0ef83a846` | Latest patch includes worker exception capture/rethrow; retain those test expectations. |
| [Fork #75](https://github.com/0cwa/mixxx/pull/75) | `5142f15e076869bcd4b19032d499b17ccc6d3708` | Preserve unsigned slot bounds and high-slot identity; replace weak 44-byte fixture with full records. |
| [Fork #76](https://github.com/0cwa/mixxx/pull/76) | `e2ff6f0a4e4d8f233de734b84eb1e13540fa22af` | Recover type-transition behavior and assertions once. |
| [Upstream #17321](https://github.com/mixxxdj/mixxx/pull/17321) | `d43b4b871d89e754092c8b9fef6c93660ff2f6a6` | Do not reclassify high hot slots as memory or duplicate Main Cue preferences. |
| [Upstream #17322](https://github.com/mixxxdj/mixxx/pull/17322) | `9f270a94881dc89066a5147a2c15b3b7ddb169ea` | High-slot marker classification is incompatible; no wholesale renderer transplant. |
| [Upstream #17323](https://github.com/mixxxdj/mixxx/pull/17323) | `7b5dfe3354932d61276a6dbb2137927102960f73` | Optional label styling overlaps #17322; excluded from this correctness change. |
| [Upstream #17441](https://github.com/mixxxdj/mixxx/pull/17441) | `3d469416625d8c22ba994bf763e28f22913a7e34` | Adapt GUI permission request and security-token lifetime. Merged into 2.5 at `34144dc0112102c2feb653e22eecd9ef7a8cc14c`. |

Inspected upstream main was `20f2c218ae1b762ac39fd76ff408c8428056f4f7`; 2.6 was `66c2108f1e4172fb0539b973109a21bf44ecc01f`.
Their identical `rekordboxfeature.cpp` blob `706bf585bfac3cbbbfd53ff6635b9910a47451f8` still contained the audited endpoint loss, time clamp, and invalid palette literal.

## Fixtures and source integrity

Synthetic ANLZ fixtures follow the checked-in schema, and the two complete hot-loop records were independently accepted by the unchanged generated Kaitai reader.
The authoritative comparison used [Crate Digger](https://github.com/Deep-Symmetry/crate-digger/tree/7c9d5358d8a3ce663f566380f2dc79ab1393201e); the inspected ANLZ schema blob was `a3c7a18cb9fb83548cdd0528044858f467bb7b3c`.
No vendor schema/generated-source pair was edited or regenerated.

Two genuine PDB files were read from the MPL-2.0 [rekordcrate corpus](https://github.com/Holzhaus/rekordcrate/tree/68cd702e9c78bde4b9eab477e0a7547a0cb69768/data/complete_export).
No audio, account key, or downloaded fixture binary was committed.

| Fixture | Independent origin/expectation | SHA-256 | Observed decoder result |
| --- | --- | --- | --- |
| Demo export | [Original commit](https://github.com/Holzhaus/rekordcrate/commit/2497e715e8e272997291debb678d639c9fa92f64) describes two tracks and exact Loopmasters paths. | `27d5a84d7688244357c467680b7e62a36a0e6f3f7eb0ab4c37b26aa312e7f8aa` | Two tracks, source IDs 1/2, matching titles and both published paths. |
| Empty export | [Original commit](https://github.com/Holzhaus/rekordcrate/commit/769745ee552380d345958341c4927adfa1271edf) describes an empty device export. | `aa22564888963e1ad1771b20cc858a3dc36b235a4463309da244d6894d605a09` | Zero tracks. |

Both results are usable warnings solely for explicitly unsupported history/other tables.
Their exporter version is not documented; a separate XML fixture's version was not attributed to these PDBs.
They do not establish cue/playlist/ANLZ or named-version compatibility.
Both initially exposed the partial-row-group bound defect; the preceding decoder rejected them at offsets 8220 and 57372, and the corrected decoder accepts both.
The correction is independently supported by [python-prodj-link's reverse index](https://github.com/flesniak/python-prodj-link/blob/b08921e91a72f0f170916f425ad79e8ed4bd153b/prodj/pdblib/page.py) and [rekordcrate's row allocation](https://github.com/Holzhaus/rekordcrate/blob/68cd702e9c78bde4b9eab477e0a7547a0cb69768/src/pdb/mod.rs).

## Executed validation

The final combined run passed **279/279 tests from 19 suites**, with zero failures, errors, disabled tests, or skips (2.799 seconds).
The code, schema, build registration, and test bytes match every entry in the source manifest.
Both `mixxx` and `mixxx-test` built successfully, and `mixxx --version` exited zero reporting `Mixxx 2.7.0-alpha`.
Standalone and integrated results below were executed during this development session, not copied from donor PRs.

| Check | Result and limits |
| --- | --- |
| Final integrated Mixxx tests | 279/279 passed across the suites listed below, including four complete DAO database lifetimes, worker rollback/removal/retry, and trigger-time control snapshots. |
| Build and formatting | `mixxx` and `mixxx-test` build passed; application version smoke passed; all applicable repository pre-commit hooks and `git diff --check` passed. |
| Pure ANLZ ASan + UBSan | 37/37 passed, including 2,000 deterministic mutations, truncations, full loop fixtures, companion fallback, cancellation, callback failures, and a 10,000-cue merge. |
| Pure PDB native | 34/34 passed, including 2,048 truncation prefixes, cycles, malformed counts/strings, partial indexes, metadata, repeated playlists, cancellation, and callback failures. |
| Pure PDB ASan + UBSan | 34/34 passed. LeakSanitizer is unavailable under this executor's ptrace environment; `detect_leaks=0` is explicit. |
| PDB public corpus | Both independently described exports passed the pure decoder; exporter version unknown. |
| Standalone Beats/Cue | 48/48 focused cases passed before final integration. Rebuilding the original PR26 Beats source against the applicable 41 cases produced three expected failures: phase/rate equality, legacy offset contamination, and BeatMap phase persistence. |
| Schema | SQLite migrations 1 through 41 executed; NULL/0/1 active intent accepted and invalid values rejected. Full DAO close/reopen is covered in the integrated suite. |
| Synthetic PDB scale | 10,000 records / 2,561,024 bytes: 56.8458 ms; 100,000 / 25,601,024 bytes: 334.182 ms. In-memory decoder, concurrent build, no SQL/UI or old-importer performance baseline. |

Execution environment: Linux x86_64, GCC 14.2.0, C++20, Qt 6.8.2, CMake 3.31.6, protobuf 3.21.12, GoogleTest 1.17.0 (`52eb8108c5bdec04579160ae17225d66034bd723`), clang-format 19.1.3.
Development dependencies were extracted to a local prefix; the application build used its normal CMake/Ninja targets.

The full build uses Release, Qt6, `BUILD_TESTING=ON`, QML off, and optional ENGINEPRIME/KEYFINDER/BUNGEE/SIGNALSMITH/FFMPEG/FAAD/WAVPACK/PIPEWIRE/LILV features off.
The initial `WARNINGS_FATAL=ON` build stopped on pre-existing GCC float-conversion warnings in `autopaneffect.h`; the integrated build uses `WARNINGS_FATAL=OFF` without changing that unrelated effect.
Changed production sources are separately checked with `-Werror` where recorded.

```sh
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$dependency_prefix" \
  -DCMAKE_BUILD_TYPE=Release -DQT6=ON -DQML=OFF \
  -DBUILD_TESTING=ON -DBUILD_BENCH=OFF -DOPTIMIZE=off \
  -DENGINEPRIME=OFF -DKEYFINDER=OFF -DBUNGEE=OFF -DSIGNALSMITH=OFF \
  -DFFMPEG=OFF -DFAAD=OFF -DWAVPACK=OFF -DPIPEWIRE=OFF -DLILV=OFF \
  -DWARNINGS_FATAL=OFF
cmake --build build --target mixxx mixxx-test --parallel 7
```

The final test command used the extracted Qt library/plugin paths, an isolated temporary/config/cache directory, and this filter (test names were enumerated first to verify registration):

```sh
QT_QPA_PLATFORM=offscreen build/mixxx-test --resource-path "$PWD/res" --log-level info \
  --gtest_filter='Rekordbox*:BeatsTest.*:BeatGridTest.*:BeatMapTest.*:CueTest.*:TrackDAOPersistenceTest.*:TrackDAOTest.*:SchemaManagerTest.*:MainCuePromotionPolicyTest.*:MemoryCueControlTest.*:CueControlTest.*:BpmControlTest.*:HotcueControlTest.*:TrackHotcueOrderByPosTest.*' \
  --gtest_output=xml:integrated-final.xml
```

| Suite | Executed/passed |
| --- | ---: |
| RekordboxAnlzDecoderTest | 37 |
| RekordboxPdbDecoderTest | 34 |
| RekordboxAnlzImporterTest | 34 |
| RekordboxDeviceImporterTest | 9 |
| RekordboxFeatureTest / RekordboxXmlParserTest | 12 / 7 |
| BeatsTest / BeatGridTest / BeatMapTest | 38 / 12 / 7 |
| CueTest / MainCuePromotionPolicyTest | 5 / 3 |
| TrackDAOPersistenceTest / TrackDAOTest / SchemaManagerTest | 1 / 3 / 4 |
| MemoryCueControlTest / CueControlTest | 18 / 21 |
| BpmControlTest / HotcueControlTest / TrackHotcueOrderByPosTest | 2 / 30 / 2 |

The native parser/model and control cases overlap with the sanitizer runs; their counts are not added together as unique tests.
No Windows/macOS runtime, named player/firmware, real USB interruption, or complete sound-backend matrix is claimed.
The worker cancellation test starts already cancelled; late SQL failure proves rollback after writes, but cancellation triggered specifically during SQL application was not independently timed.
The symlink test runs on Linux and explicitly skips its Unix-specific fixture on Windows.

## Provisional output contract B-write/1

Writer implementation/enablement is gated on integrated read qualification, as required by the handoff.
This versioned contract lets B and C prepare independently without making their writer code-start dependencies circular.
It is a proposal for A/C integration, not an already agreed or implemented file format.

| Required field/group | Constraint |
| --- | --- |
| `contractVersion` | `B-write/1`; reject unknown incompatible versions. |
| `targetProfile` | Explicit legacy DeviceSQL/ANLZ and optional OneLibrary variants, filesystem/codec limits, named exporter/player/firmware qualification, supported cue/slot/analysis capabilities. No universal-player default. |
| `sourceSnapshot` | Stable source identity, source generation, exact hashes, original source IDs and opaque input bytes/sections needed for preservation. |
| `targetIds` | Deterministic source-to-output track/playlist/artwork/analysis ID maps, separate from Mixxx TrackId and canonical media identity; complete referential validation. |
| `tracks` | Normalized metadata, canonical media references, codec/sample properties, output media/artwork/analysis paths, known versus unknown values. |
| `annotations` | Explicit role/slot, full cue intervals, labels/colors/active intent, source ordinals/provenance, timing unit/precision and documented correction. |
| `tempo` | Tempo-segment anchors with time/BPM/meter/beat-in-bar separate from explicit per-beat times; preserve phase. |
| `stagingManifest` | Relative target paths, ownership/managed status, bytes/hash/size, dependencies, free-space estimate, verification and publication state. Reject traversal and unsupported targets. |
| `readbackEvidence` | Independent reader results for every written structure and reference, followed by real application/player checks; self-round-trip alone is insufficient. |
| `publicationPlan` | Staging on the target filesystem, recoverable ordered replacement, journal/checkpoint, unmanaged-file preservation, cancellation/full-disk/unplug/power-loss recovery, safe existing-device updates. |

Publishing media, artwork, ANLZ, PDB, and a second database format is not a multi-file atomic operation.
Recovery must identify which generation is complete and safe, retain the prior usable generation until publication is verified, and never silently remove unmanaged files.
Actual rekordbox and named player/firmware evidence is required before any playable-output claim.
No writer, USB publication control, or claimed playable output is added in this PR.

## Integration requests and remaining acceptance

A should integrate the provisional feature/CMake seams, durable source identity and applied generations, conflict/removal policy, XML tempo/timing policy, and the schema 41 migration.
C should reuse the shared ANLZ API, provide licensed versioned exporter fixtures with independently known cue/grid/playlist expectations, and run the platform matrix.
B still needs measured codec alignment, full import performance against a recorded baseline, physical performance/UI qualification, and the qualified read milestone before implementing/enabling the legacy writer.
M1/M2 are not declared accepted merely because these local regressions pass; M3/M4 are not delivered by this PR.

The single next action after review is to run the integrated reader on a licensed, independently described export from a named rekordbox version and player/firmware setup, recording cue, grid, timing, reimport, and control results before enabling writing.

Prepared by ChatGPT for human review.
