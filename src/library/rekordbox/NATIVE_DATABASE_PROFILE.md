# Prepared by ChatGPT: Native database reader profile

This is an isolated read adapter for the structurally inspected
`djmdProperty.DBVersion = 6000` profile. It does not enable native sources in the
Mixxx sidebar or advertise compatibility with every rekordbox 6/7 release.
The C++ reader consumes an already unlocked, stable private database supplied by
`snapshotDatabase`; it never opens a live native database itself.

## Evidence and version boundary

The independent reference is
[pyrekordbox revision `02a208837434e4c05c0a222e8fe6583e6ac0547f`](https://github.com/dylanljones/pyrekordbox/tree/02a208837434e4c05c0a222e8fe6583e6ac0547f).
Its `master_locked.db` and `master_unlocked.db` test fixtures contain DBVersion
6000, database ID `3546525630`, six tracks, one regular empty playlist, one
internal playlist record, and no cue or history records. Values were inspected
with Python's standard SQLite reader before executing this adapter. The native
fixture's producer application version is not independently attested.
An adjacent XML declares rekordbox 6.6.2 but has different track IDs and analysis
references; it cannot establish the native database's producer version or an
equivalent synchronized collection.

The repository's MIT license was inspected. External binary databases and media
are not redistributed here. Retrieval hashes, provenance limits, expected
metadata, and the separately authored SQL fixture are recorded in
[`../../test/rekordboxfixtures/native-db6000/provenance.json`](../../test/rekordboxfixtures/native-db6000/provenance.json).
The external fixture does not qualify populated playlist order, cues, loops,
history, native 7, a vendor application reopen, or physical hardware.

## Record mapping

The field definitions are based on the pinned reference's
[native format documentation](https://github.com/dylanljones/pyrekordbox/blob/02a208837434e4c05c0a222e8fe6583e6ac0547f/docs/source/formats/db6.md)
and [database models](https://github.com/dylanljones/pyrekordbox/blob/02a208837434e4c05c0a222e8fe6583e6ac0547f/pyrekordbox/masterdb/models.py).

| Input | Output and boundary |
| --- | --- |
| Caller `sourceId`, generation, fingerprint | Preserved separately from native `DBID`, mount path, media location and source track ID. An explicit source binding and snapshot provenance are required. |
| `djmdContent.ID` | Opaque string, including IDs outside signed 32-bit range. Raw row columns remain in `properties`. |
| Title, comments, dates, lookup names | SQL NULL stays absent in typed fields; an empty string stays explicitly empty. The raw property map distinguishes a missing column from a present NULL. |
| Artist, album, album artist, composer, remixer, genre, key and label IDs | Resolved through their native tables. Unresolved active references refuse the complete result. Key spelling remains the stored `ScaleName`. |
| `BPM`, `Length` | BPM divided by 100; integer seconds converted to milliseconds with overflow checks. No beat grid is invented from average BPM. |
| `djmdPlaylist` / `djmdSongPlaylist` | Opaque IDs, parentage, sibling `Seq`, membership IDs and one-based `TrackNo` preserved. Repeated track membership remains repeated. Duplicate positions, missing references and cyclic parents refuse the result. |
| `djmdHistory` / `djmdSongHistory` | Preserved in a separate `history:` identifier namespace. These mappings currently have synthetic evidence only. |
| Smart playlists and unknown/internal playlist types | Definition and raw properties preserved; no smart query is evaluated. A structured issue records the unsupported behavior. |
| `djmdCue.Kind` and `OutMsec` | Memory versus hot and point versus loop remain separate. Positive `Kind` becomes a zero-based hot slot; `OutMsec = -1` means no loop end. |
| `InMsec` / `OutMsec` | Milliseconds, retained independently from `InFrame` / `OutFrame`, which use 1/150-second units. They are never interpreted as PCM frames. |
| Cue comment, color and `ActiveLoop` | Optional labels and active-loop intent preserved. Indexed colors remain indexed; the reader does not claim an unverified RGB palette. The raw `-1` no-color sentinel remains in provenance. |
| `CueMicrosec`, color-table and other unknown fields | Raw values retained without guessing precedence or converting them into canonical annotations. |
| Nonempty `contentCue.Cues` alternate payload | Entire read returns Unsupported. Authority relative to `djmdCue` and ANLZ has not been qualified. |
| Active censor, cue-bank, tag, related-track, sampler and mixer records | Preserved as opaque ancillary rows with an explicit issue. They are not applied to canonical Tracks or performance controls. |
| Artist, album, genre, key, label and color lookup rows | Retained in raw form as well as typed mappings, so references from unmapped fields keep their source data. |
| `rb_local_deleted` | Deleted records excluded. An unknown deletion flag is unsupported. |

All published libraries are immutable shared records. Failure or cancellation
returns no partial library. The adapter bounds row count and individual value
size, validates ordinary schema tables, checks cancellation while reading and
mapping, and rejects invalid numeric values or embedded null bytes.

## Original paths and access lifetime

`context.originalDatabasePath` is the original absolute database location;
`context.assetRoot` is the original native library directory and defaults to that
database's parent. The reference reader resolves `AnalysisDataPath` under the
library's `share` directory, even when the stored path begins `/PIONEER/`.
Artwork paths resolve under the library directory. These semantics follow the
pinned [native database reader](https://github.com/dylanljones/pyrekordbox/blob/02a208837434e4c05c0a222e8fe6583e6ac0547f/pyrekordbox/masterdb/database.py).
The private snapshot directory never replaces either original root.

Asset traversal and symlinks that escape the supplied root are refused. Missing
assets retain their original locator with a structured issue. Relative media
requires an explicit absolute `mediaRoot`; foreign Windows paths on Linux and
non-local locators remain unavailable pending explicit relocation. Streaming
service records are distinguished from ordinary missing local files. The
adapter does not infer that streaming cache files are playable local media.
The immutable result retains the caller's access lease for later asset access.

## Validation and integration gate

The standalone test reads the committed synthetic SQL fixture and can optionally
read a private copy of the pinned external plaintext fixture. It covers metadata,
NULL versus empty fields, Unicode and moved roots, ordered/repeated memberships,
separate history IDs, cue roles/timing, cancellation, empty collections,
duplicate/orphan/cyclic data, bounds, unsafe paths, unsupported schemas, and
unchanged private database write counters.

`tools/rekordbox/native_fixture_probe.cpp` additionally drives the production
snapshot service with the pinned encrypted fixture. It accepts key bytes only
on standard input, checks the ciphertext checksum, verifies known metadata,
tests an incorrect key, checks plaintext snapshot cleanup, and compares the
complete source-directory manifest before and after. The probe contains no key.
Its successful Linux run observed 45 unchanged source-tree entries. It establishes
the tested native cipher/schema path, not native performance-data or application
compatibility. Final candidate build commands and test counts belong in the
combined qualification report.

The next integration action is to qualify native database versus shared ANLZ
authority using attributable cue/loop/grid fixtures, then connect this frozen
record boundary to A's source/application lifecycle. Canonical application,
smart-playlist evaluation, exact native application-version support, native 7,
macOS permissions, Windows runtime support, and M2 acceptance remain open.

Prepared by ChatGPT for human review and submission.
