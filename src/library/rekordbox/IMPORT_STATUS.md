# Prepared by ChatGPT: Rekordbox import integration status

Prepared at the repository owner’s request for review.

Updated 11 November 2026. This change is a follow-up to fork PR26, based on
`78f561f9858cc20c0f36bc5fd6a3468b616f84db` (`pr/rekordbox-v1-ci-fixes`).
It is prepared for the owner’s fork, not an upstream submission or a main-branch
merge. M1 acceptance remains open until the remaining implementation and
validation gates below are done.

## What this change connects

The XML parser, legacy PDB/ANLZ readers, source catalogue and canonical Mixxx
collection now have a shared import boundary. Displaying source listings
publishes source-specific external views without admitting canonical tracks.
Mixxx requests canonical tracks for selection previews, deck loading and the
existing playlist/crate import actions. These requests admit that track through
a planned SQL transaction. This includes new media rows, supported metadata, cues, grids,
ownership, provenance and the successful per-track revision.

The relevant implementation is split by responsibility:

- `rekordboxxmlparser` reads XML into frozen source records. It preserves field
  presence, raw attributes, numeric cue roles, memory versus hot slots, tempo
  anchors, ID/location playlist keys and repeated membership.
- `rekordboxsource` owns source sessions and exact source-scoped external SQL
  views. Playlist keys use opaque UUIDs rather than names or display paths.
- `rekordboxsourceparser` reads XML or legacy `PIONEER`/`.PIONEER` PDB sources
  without accessing GUI objects or the canonical database.
- `rekordboxanlzdecoder` validates bounded binary sections into immutable physical
  records; `rekordboxanlzimporter` reconciles DAT/EXT and prepares canonical fields.
- `rekordboxxmlimporter` prepares XML fields and provides the canonical SQL
  adapter shared by both import paths.
- `rekordboximportregistry` seals plans, records ownership and source revisions,
  rechecks expected values inside the transaction, and rejects stale plans.
- `rekordboxfeature` provides add, refresh, locate and remove actions. Workers
  retain access leases and immutable requests. Only a current source generation
  may publish a result on the GUI thread.

CueDAO and TrackDAO now check writes and commits before publishing IDs, clean
state, cache entries or added-track notifications. Caller rollback and failed
commits leave proposed objects uncommitted. Scanner publication uses the same
post-commit rule. Derived waveform analysis cache writes remain best effort
following the annotation transaction.

## Import policy and limits

The default policy preserves local values and edits. Occupied hot slots, edited
source-owned fields and unsupported semantics produce reviewable losses. Memory
cues remain independent physical annotations, including cues at the same time.
Black is a real RGB value; hot slot zero and frame zero are valid. XML tempo
anchors are interpreted as segments, while ANLZ PQTZ records are explicit beat
observations. Decoder timing correction is applied once in PCM-frame conversion.

Requested automatic memory-cue promotion currently requires explicit loss
consent. The importer preserves the existing Main Cue and independent memory
records. A new canonical track receives Mixxx’s normal cue position of zero;
this does not create or promote a Main Cue annotation.

Supported XML grids require continuous 4/4 phase and representable intervals.
Grid anchors must fit the existing signed 32-bit serialized PCM-frame field;
out-of-range anchors produce a reviewed loss instead of truncation. Artwork
application accepts absolute local references; relative and remote references
still require a resolution design.

New or changed imports require an exclusive canonical-cache lease. The actual
cache mutex is released before a consent dialog and reacquired afterward; the
sealed plan and transaction checks reject intervening changes. An unchanged
persisted input/policy revision can return an already cached canonical track
without reapplying annotations. Changed imports into loaded/cached targets are
refused until that target is available for safe mutation.

The model also prevents a nested GUI track request from applying a second
import while a modal review is open. Cancellation and stale-plan failures
explain why that request was not committed.

New rows retain the actually observed audio-metadata synchronization timestamp
and a valid DateAdded. Existing unparsed or outdated metadata requires refresh;
legacy parsed rows with an unknown timestamp keep their valid legacy behavior.
New rows receive probed physical facts and explicit source metadata. Broad
audio-tag fallback for fields omitted from XML/PDB is not implemented.

A per-track import does not stamp the entire source as imported. It preserves
other track mappings and playlist membership. A failed read, rejected review,
cancellation or failed database operation never records success. Removing a
source removes its external view and provenance bindings, while keeping
canonical tracks and audio files.

Automatic USB reconnection requires a matching verified volume identity. Linux
filesystem UUIDs are supported where available. Unknown or ambiguous identities
require explicit reconnection; the same mount path is insufficient proof. XML
sources remain visible during USB discovery and removal.

New or changed canonical applications are refused when external collection synchronization is
enabled, because the direct transaction must not silently bypass that manager’s
synchronization contract. Source files are read-only. Native desktop databases,
OneLibrary, playable USB writing and native writeback are not advertised.

## Executed validation

The PR description and execution evidence identify the candidate commit and
exact binaries. All 225 selected integrated tests passed across 16 suites, with
zero failures, disabled cases or skips.

| Check | Result |
| --- | --- |
| Actual Mixxx application and `mixxx-test` build, Qt 6.8.2/GCC 14.2 | Both targets built and linked |
| Integrated selected tests | 225 passed across 16 suites, including new-track cue-position and reentrancy regressions |
| XML compatibility suites, Qt 6.4.2 and 6.8.2 | 41 passed on each version with production-style math flags |
| Current ANLZ decoder ASan/UBSan suite | 24 passed; leak detection excluded by the sandbox environment |
| Source/session/external-view SQLite suite | 16 passed within the integrated selection |
| Native application smoke/screenshots | Fresh database: repeated order, single consent, Deck 1 load, canonical Tracks(1), clean exit; zero debug assertions |
| Focused repository lint | gersemi 0.17.1, cmakelint 1.4.3, codespell 2.4.3 and markdownlint-cli2 0.23.2 passed; whitespace/JSON checks passed |

The local configuration requests Debug with `-O0 -g1` and BUILD_TESTING. The
actual application compile commands append `-O3 -ffast-math`, so these paths run
with optimization and debug assertions. Finite-number checks use Mixxx’s
`util_isfinite`; its implementation receives `-fno-fast-math` separately.
Optional audio and integration capabilities not needed for these paths are disabled locally;
this is not a claim that every release configuration or supported OS was built.
The dependency and configuration manifests identify the exact local settings.

The final native workflow retained the XML title/artist and 120 BPM after deck
loading, preserved both same-time memory cues and the black hot loop, and left
the XML/WAV bytes unchanged through shutdown. The environment had no audio
output device, so this establishes application loading and persistence rather
than audible playback or hardware qualification. The six screenshots show
states of the current candidate; a true base-versus-candidate comparison remains
open below.

A standard developer environment can reproduce the relevant targets with:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target mixxx-test rekordbox-anlz-decoder-test
QT_QPA_PLATFORM=offscreen build/mixxx-test --resource-path res \
  --gtest_filter='Rekordbox*.*:ImportRegistryTest.*:CueDAOTest.*:TrackDAOTest.*:CueTest.*:BeatsTest.*:BeatsPersistence.*:BeatsTempoAnchors.*:BeatsGlobalPhase.*'
ctest --test-dir build -R rekordbox-anlz-decoder --output-on-failure
```

The ten model tests use generated 48 kHz WAV audio and exercise the actual
`RekordboxPlaylistModel::getTrack` method, audio probing, database admission,
rollback, cached repeats, tagged-audio metadata and nested consent events. Their USB case injects a
frozen track DTO into a source decoded from an empty valid PDB, then reads the
real synthetic DAT/EXT files; it does not qualify populated PDB track decoding.
The file-backed adapter tests exercise the actual Mixxx schema and Cue/Beats
serialization, including close/reopen and failure recovery.

## Fixture provenance

`src/test/rekordboxfixtures/README.md` records the XML fixture origins and scope.
The synthetic ANLZ pair has byte counts, SHA-256 hashes and independently stated
numerical expectations in `anlz-synthetic/provenance.json`. It includes two
same-time memory records, black hot slot zero, a hot loop and explicit variable
beat timing with phase. Synthetic fixtures do not establish exporter or hardware
compatibility.

The first handoff packet also contains externally sourced XML labeled rekordbox
5.8.6 and 6.6.2, and six public ANLZ files. Their retrieval hashes were checked;
the captures were not independently attested as genuine exports, and those ANLZ
files have no cue records. They cannot satisfy the missing genuine cue/loop
acceptance gate.

## Remaining work and acceptance gates

| Work | Required completion evidence |
| --- | --- |
| Genuine M1 exports | Capture legal, attributable exports from named rekordbox versions. Compare required metadata, repeated/location-key playlists, memory/hot cues and loops, RGB extensions, and fixed/variable grid phase against independently recorded expectations. Include populated legacy PDB pages and DAT/EXT disagreements. |
| Real DJ and device validation | Run the intended loading, playback, cue/loop/grid and restart workflow on actual media and hardware. Include MP3/M4A decoder-offset cases, unplug during read, remount, duplicate IDs and two devices referencing the same audio. Qualify scanner/device concurrency and large collections. Human review remains required before a main-branch submission. |
| Platform and configuration qualification | Run Windows and macOS builds/tests; exercise actual sandbox permission denial/grant and retained access-token lifetime on macOS. Verify automatic identity where available and explicit reconnection elsewhere. Run the project’s normal CI configuration on the exact PR head. |
| Full UI comparison | Capture and review an actual base-commit versus candidate GUI comparison. Current-state workflow screenshots, if supplied, are labeled as those states and do not substitute for the base comparison. |
| Remaining durable metadata capabilities | Mixxx’s current library schema has no matching durable fields for label, remixer or disc number, and no matching semantics for source modification dates. Keep their explicit loss/provenance behavior until a separately reviewed persistence design is implemented. Design audio-tag fallback and relative/remote artwork resolution with clear ownership rules. |
| Remaining cue/grid capabilities | Implement any future Main Cue promotion and automatic active-loop behavior through faithful canonical representations. Qualify disabled cues, indexed-only colors without a verified RGB mapping, ambiguous memory records, non-four-beat/discontinuous phase, and grid positions outside the current serialized range; retain explicit losses/refusals until supported. |
| External collection synchronization | Design and test an approved manager-level transaction/synchronization path before allowing changed imports with external collection synchronization enabled. |
| Clean current-main series | Port the focused changes onto the selected current fork/upstream base, resolve the feature/preferences conflicts, preserve `.PIONEER` and access-lifetime fixes, and exclude PR26’s inherited unrelated audio work. This stacked follow-up deliberately targets the existing PR26 branch. |
| M1 decision | Review all preceding evidence and explicitly accept or reject M1. Passing synthetic and local joined tests alone is insufficient. |
| A08 XML export | Start after M1 acceptance. Use a separate export model, stable IDs, capability/loss reporting and safe publication, then import output into a named rekordbox version and compare independent expectations. |
| M2 modern/native sources | Continue independently with version-specific profiles, sealed snapshots, core tracks/playlists, cue/loop/grid semantics, persistence and source-integrity checks. No modern/native version is enabled by this PR. |
| USB writing/native writeback | Keep behind their own acceptance and authorization gates; XML export would not qualify either capability. |

The next action after code and CI review is to obtain an attributable genuine
export set and execute the M1 acceptance checklist. Do not retire PR26 or submit
this combined history to main until the clean-series and human validation gates
are resolved.

Prepared by ChatGPT for human review and submission.
