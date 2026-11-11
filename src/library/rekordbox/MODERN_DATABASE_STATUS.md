# Prepared by ChatGPT: Modern and native database read status

Prepared at the repository owner's request for a focused follow-up to PR #101.
This implements Orchestrator C's independent database read work. **M2 acceptance
remains open.** The new readers are not registered in Mixxx's main build or source
sidebar, and no native application version or player is advertised as supported.

## Baseline and ownership

The exact base is `68293f35393af00a2615afca9df44b7a34e51ce4`, branch
`work/rekordbox-import-integration-20261111`, published in
[fork PR #101](https://github.com/0cwa/mixxx/pull/101). Its parent is PR #26's
`78f561f9858cc20c0f36bc5fd6a3468b616f84db`. This follow-up contains only C's unique
delta. It does not supersede either PR or qualify their inherited changes.
The PR description binds the final head SHA to the validation results.

| Owner | Delivered boundary |
| --- | --- |
| C, this change | Versioned database records, snapshots, OneLibrary/native mappings, isolated dependency module, tests, fixture manifests and packaging proposal. |
| A, existing base | Source bindings/generations, GUI permission prompts, source/sidebar publication, canonical transactions, conflict policy and durable import completion. Central build and CI registration remain A's integration step. |
| B, existing base | The existing ANLZ decoder and canonical cue/grid semantics. The composed test calls that decoder; no second decoder is added. |

No counterpart conversation or unpublished checkout was assumed. The published
PR #101 interfaces and decoder are the available integration evidence. The
Qt-neutral database record contract is version **1** and remains a proposed
extension for A/B acceptance, rather than a replacement for their current types.

## Implemented read path

`readDatabaseSource()` is the sole exported entry point of the proposed private
shared module. It accepts a source format, separately selected cipher profile,
key-provider callback, snapshot limits, original asset roots, durable source
binding, generation, cancellation flag and retained permission lease. It returns
`shared_ptr<const DatabaseLibrary>` plus structured outcomes/issues. No canonical
Track, Cue, Beats, sidebar model or import-completion record is mutated.

The context fingerprint describes the completed private snapshot. It is separate
from the source binding and source track IDs. A caller-provided binding survives
a remount; identical track IDs from different bindings remain distinct. Native
`DBID` is recorded separately. OneLibrary `masterDbId` is provenance for the
originating collection, not a USB identity. Empty devices still need a caller
binding.

The record graph preserves SQL NULL versus explicit empty values, raw properties,
opaque IDs, playlist parentage/order/repeated membership, original media and asset
references, and capability limits. Media is distinguished as local, missing,
streaming or unavailable. Schema, data, path, key, access and cancellation
failures never publish a partial collection.

### Safe database snapshots

The source is never opened by libdjinterop. A source-only VFS strips create,
write and delete flags from database/journal/WAL opens, rejects write/truncate/
delete operations, disables shared-memory growth, and forces SQLite's read-only
SHM behavior. `readonly_shm=1` comes from the exact qualified SQLite source, with
a runtime source-ID check; a runtime with different behavior is refused.

An explicit read transaction fixes the committed view, including WAL-only rows.
Plaintext databases use SQLite's backup API. The actual SQLCipher 4.16 probe
rejects that API for encrypted databases, so encrypted profiles instead use
`sqlcipher_export()` inside the same source read transaction. Export writes only
to private storage, preserves `user_version`/`application_id`, and limits the
destination page count. This choice follows the
[SQLite WAL](https://www.sqlite.org/wal.html),
[backup](https://www.sqlite.org/backup.html), and
[SQLCipher design](https://www.zetetic.net/sqlcipher/design/) guidance together
with observed behavior of the pinned implementation.

Some cleanly closed WAL databases have no sidecars. If the protected online read
cannot work, the fallback first obtains SQLite's **exclusive main-file lock**.
Existing WAL connections retain shared main-file locks, so this proves that no
cooperating database user remains. The service copies the main database through
the locked handle together with any WAL and rollback journal. It rebuilds/replays
sidecars only in private storage. The lock handle requires write access to obtain
the exclusive lock on POSIX, but no write, truncate or sync is issued through it.
The service does not change source permissions to obtain that access.

If the lock cannot be obtained or a read-only source needs unavailable sidecars,
the service refuses safely. There is no unlocked raw-copy fallback, source
checkpoint, source journal-mode change, or `immutable=1` claim. Source device/inode
changes, removal, cancellation and timeouts prevent publication. WAL/journal
copying is bounded and checks companion metadata. SQLite's local locking
requirements still apply; network-filesystem WAL use is not qualified.

Private directories are created with owner-only POSIX permissions. The completed
database is plaintext and opens read-only; its directory and connection are
removed when the last record owner releases them. The original access leases
remain held through later media/ANLZ access. Key bytes are passed through the
format-specific callback and scrubbed from temporary buffers. They are not
logged, embedded in fixtures, or put in the record graph.

The Windows snapshot entry point returns Unsupported until private-directory
ACLs and Windows behavior are implemented and qualified. POSIX construction is
implemented, but only Linux was executed here; macOS permission/token behavior
still requires A's platform integration and runtime tests.

### OneLibrary

The reader accepts the structural `onelibrary-v1-db1000` profile and uses
libdjinterop's maintained low-level API for corroboration. Checked raw queries
preserve fields that API omits or normalizes, including empty text, artwork,
color identity and raw membership positions. The adapter validates references,
ordinary tables, numeric bounds, hierarchy/order, repeated entries, path
containment and direct-source/hardlink alias rejection.

When a device contains both legacy `export.pdb` and modern `exportLibrary.db`,
`selectDeviceDatabase()` requires an explicit choice. This policy covers matching
and divergent pairs without assuming they match. A sole available format may be
selected automatically. A requested missing format is not replaced silently.
Legacy parsing remains with A/B.

OneLibrary cues, histories and advanced database records are retained as opaque
data with capability limits. An analysis locator is not a performance-data
acceptance result. Cues, loops, grids, phase and native database/ANLZ precedence
must still be qualified with B before canonical application.

### Native desktop

The native reader accepts only the inspected structural
`native-dbversion-6000-v1` profile. It maps metadata, ordered/repeated playlist and
history membership, native annotations, original media paths, artwork and ANLZ
references. Database cues preserve memory/hot identity, optional zero-based hot
slot, loop endpoints, optional label/indexed color/active-loop intent, milliseconds
and independent 1/150-second fields. No field is assumed to contain PCM frames.

The independent encrypted/plaintext fixture establishes six metadata rows and
the cipher/schema path. Its original application version is not attested, and
it contains no cues or populated playlists. The adjacent XML's 6.6.2 label is
not assigned to that native database. Native v7 and v5 are not advertised.
See [NATIVE_DATABASE_PROFILE.md](NATIVE_DATABASE_PROFILE.md) for exact mapping,
provenance, unsupported alternate cue payloads and the independent oracle.

## Dependency and module isolation

The source pin is libdjinterop
`d61727c5a3369f79d11b70afba6c925ddeb7f54c`, declared 0.28.0 but unreleased at
qualification. The downstream private-snapshot patch has SHA-256
`5cb112963f8c30025eef8ae416f129211deb2d6db96cf8281785e242cb72fa65`.
It permits an explicit empty private passphrase; source cipher profiles still
require their own keys. SQLCipher is 4.16.0 on SQLite 3.53.1.

Directly linking the SQLCipher archive into Mixxx failed the ordinary SQLite
isolation check. Mixxx calls SQLite C functions on Qt-owned handles, so a simple
QSQLITE query pass is insufficient. The candidate therefore statically contains
its own hidden dependency copies in a private shared module. Only
`readDatabaseSource()` is exported, and no SQLite handle or libdjinterop object
crosses the boundary. The main application's existing Engine DJ dependency
remains unchanged. The composed consumer checks direct functions/collations on
Qt-owned handles before and after reads.

The standalone project and focused recipes live in
[`../../../tools/rekordbox/database_readers`](../../../tools/rekordbox/database_readers).
The dependency probe, pin, patch, upstream comparison, license/runtime details
and Linux/macOS/Windows/Flatpak proposals are in
[`../../../tools/rekordbox/onelibrary_probe/README.md`](../../../tools/rekordbox/onelibrary_probe/README.md).
These are unregistered proposals. A must serialize central CMake/CI and packaging
changes, preserving the existing feature-off application build. Disabling
SQLCipher in the isolated test project still uses OpenSSL for snapshot hashing;
it is not a claim that this test module has no crypto runtime dependencies.

## Corpus and validation

The common corpus index is
[`../../test/rekordboxfixtures/database/corpus-manifest.json`](../../test/rekordboxfixtures/database/corpus-manifest.json).
It references existing XML/legacy/ANLZ evidence and new modern/native manifests.
It explicitly records that no attributable synchronized collection across all
four formats is available. Synthetic expectations are separate from independent
fixture observations and hardware/application evidence.

The reproducible commands and final pass/fail/blocked results are recorded in the
standalone project's README and validation manifest. The composed test exercises
snapshot → adapter → existing shared ANLZ decoder, source binding/generation,
remounts, overlapping IDs, access retention, cleanup, profile mismatch,
cancellation, unsupported schemas and encrypted/closed-WAL variants. It preserves
deliberately disagreeing native and ANLZ data separately; it does not apply either
to canonical tracks or claim that precedence is resolved.

The independent encrypted native probe compares the entire 45-entry source tree,
including directory entries, sizes, permissions, modification times and hashes.
Snapshot tests compare complete source-file manifests before/after quiescent
reads; concurrent-source tests distinguish intentional writer commits from the
importer's behavior. All test counts in this change refer to actual runs, with
the baseline PR's earlier application results kept separate.

## Acceptance matrix and next action

| Work package | Delivered | Remaining gate |
| --- | --- | --- |
| C01 | Pinned dependency probe, API gap checks, downstream patch and actual runtime evidence. | Suitable released package and supported-platform packaging acceptance. |
| C02 | Protected online snapshots and verified quiescent fallback; bounded cancellation/cleanup and separate cipher profiles. | Windows ACL implementation, macOS access tests and target filesystem/runtime qualification. |
| C03 | OneLibrary db1000 reader, explicit dual-format policy, metadata/hierarchy/order/assets and raw unsupported records. | Genuine OneLibrary exports, A source/application integration and B performance qualification. |
| C04 | Native DBVersion6000 structural reader and independent encrypted metadata fixture. | Named-version support evidence, native7 fixtures, real smart/history observations and source UI/discovery. |
| C05 | Native annotation mapping, explicit units, original asset roots and tests retaining database/ANLZ disagreements. | Authoritative precedence and genuine cue/loop/grid/color/phase fixtures; canonical persistence. |
| C06 | Isolated module, ordinary/Qt SQLite checks, Engine regression probe and focused recipes. | A's central registration, production package builds and Windows/macOS/Flatpak runtime tests. |
| C07 | Checksummed synthetic manifests and independently maintained native fixture evidence. | Attributable equivalent XML/PDB/OneLibrary/native collection, genuine performance records and player observations. |
| C08 | Actual base composition at the record/decoder boundary. | Tracks/cues/grids saved and reloaded through A/B, idempotent reimport, local-edit preservation, failure atomicity, UI/runtime and every advertised platform/version. M2 is not complete. |
| C09–C11 | Kept separate. | OneLibrary USB writing, native writeback and native v5 require their own implementation and acceptance gates after M2. |

**Next action:** A/B should review and accept this record/module boundary, then
connect it to the canonical source/application path using attributable modern
and native cue/loop/grid fixtures. The M1 XML/legacy milestone must not wait for
that work. No PR is merged or promoted to a completed M2 claim by this delivery.

Prepared by ChatGPT for human review and submission.
