# Prepared by ChatGPT: OneLibrary dependency qualification

This standalone probe qualifies the dependency used by the modern/native
rekordbox work. It creates disposable synthetic databases and never accepts a
user database or a user key. It is independent of the Mixxx GUI and central build.
Passing it establishes dependency behavior, not the M2 import acceptance gate.

## Revisions and scope

Evidence was collected on **11 November 2026** against:

| Item | Exact revision |
| --- | --- |
| Integration baseline, fork PR #101 | `68293f35393af00a2615afca9df44b7a34e51ce4` |
| Historical fork PR #26 | `78f561f9858cc20c0f36bc5fd6a3468b616f84db` |
| libdjinterop candidate | `d61727c5a3369f79d11b70afba6c925ddeb7f54c` |
| Latest libdjinterop tag found, `0.27.1` | `a3daec8642a985c5fe35a99c5f7967077d69f544` |
| Upstream Mixxx `main` | `20f2c218ae1b762ac39fd76ff408c8428056f4f7` |
| Upstream Mixxx `2.6` | `66c2108f1e4172fb0539b973109a21bf44ecc01f` |
| Upstream Mixxx `2.5` | `4aaf412249f1cd11b554b4e86355677b8a9606e2` |

The candidate declares version **0.28.0**, but no 0.28.0 tag or GitHub release
was available. It is an unreleased source pin, not a qualified released package.
Keep the current 0.27.1 Engine DJ dependency unchanged until A integrates a
qualified replacement or a private module boundary. Different libdjinterop
versions must not share a public ABI or interposable symbol namespace.

The maintained reader is used rather than a second OneLibrary implementation.
Its [guide](https://github.com/xsco/libdjinterop/blob/d61727c5a3369f79d11b70afba6c925ddeb7f54c/GUIDE.md)
and [build configuration](https://github.com/xsco/libdjinterop/blob/d61727c5a3369f79d11b70afba6c925ddeb7f54c/CMakeLists.txt)
require C++20 and the experimental SQLCipher build for encrypted OneLibrary.
The OneLibrary API does not implement native rekordbox `master.db` reading.

## Required private-snapshot patch

The upstream SQLCipher opener unconditionally calls `sqlite3_key()`. The actual
probe showed SQLCipher 4.16 rejects its zero-length key with `SQLITE_MISUSE`, so
an unmodified library cannot open a decrypted private snapshot with an empty
private key.

`patches/libdjinterop-private-snapshot.patch` changes that one call site to skip
keying only when the supplied passphrase is empty. Nonempty keys and read-only
open flags are unchanged. The Mixxx adapter must use this path only for the
private snapshot made by the snapshot service. Source encryption profiles must
still reject absent keys. The patch also permits the probe to create disposable
plaintext fixture databases; it is not a source writeback API.

Patch SHA-256:
`5cb112963f8c30025eef8ae416f129211deb2d6db96cf8281785e242cb72fa65`.
This is a downstream qualification patch, not an upstream submission.

## Reproduce

The commands use an isolated working directory and an installed dependency
package. Supply the paths for the local checkouts; no keys are needed.

```sh
mixxx_checkout=/absolute/path/to/mixxx
probe_work=/absolute/path/to/probe-work
mkdir -p "$probe_work"
git clone https://github.com/xsco/libdjinterop.git "$probe_work/libdjinterop"
git -C "$probe_work/libdjinterop" checkout --detach d61727c5a3369f79d11b70afba6c925ddeb7f54c
git -C "$probe_work/libdjinterop" apply --check \
    "$mixxx_checkout/tools/rekordbox/onelibrary_probe/patches/libdjinterop-private-snapshot.patch"
git -C "$probe_work/libdjinterop" apply \
    "$mixxx_checkout/tools/rekordbox/onelibrary_probe/patches/libdjinterop-private-snapshot.patch"
cmake -S "$probe_work/libdjinterop" -B "$probe_work/dependency-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$probe_work/dependency-install" \
    -DEXPERIMENTAL_ENABLE_SQLCIPHER=ON -DSYSTEM_SQLITE=OFF \
    -DBUILD_SHARED_LIBS=ON -DBUILD_EXAMPLES=ON -DBUILD_TESTING=OFF -DBUILD_DOC=OFF
cmake --build "$probe_work/dependency-build" --parallel 2
cmake --install "$probe_work/dependency-build"
cmake -S "$mixxx_checkout/tools/rekordbox/onelibrary_probe" \
    -B "$probe_work/probe-build" \
    -DLIBDJINTEROP_SOURCE_DIR="$probe_work/libdjinterop" \
    -DCMAKE_PREFIX_PATH="$probe_work/dependency-install" \
    -DPROBE_WITH_QT_SQLITE=ON
cmake --build "$probe_work/probe-build" --parallel 2
ctest --test-dir "$probe_work/probe-build" --output-on-failure
```

The source checkout pin is checked at configuration; the installed package must
have been built from that checkout with the supplied patch. The public version
number alone cannot establish that provenance. `PROBE_WITH_QT_SQLITE=ON` requires
Qt Core and SQL development packages and the QSQLITE runtime plugin. Turn it off
only to mark that check skipped. `PROBE_WITH_SYSTEM_SQLITE=OFF` similarly disables
the separate ordinary-SQLite coexistence check when that library is unavailable.
Set `TMPDIR` to a writable private directory on systems without a usable `/tmp`.

For the crypto-disabled check, build and install a second dependency with
`EXPERIMENTAL_ENABLE_SQLCIPHER=OFF`, keeping `SYSTEM_SQLITE=OFF`, then configure a
fresh probe build using its install prefix. The expected result is a structured
OneLibrary rejection while Engine DJ continues working. The GCC 14.2 build of
upstream's ordinary SQLite amalgamation required
`-DCMAKE_C_FLAGS=-Wno-error=stringop-overread`: the unmodified configuration failed
on a vendor `strlen` warning promoted to an error. This flag was not needed for
the SQLCipher build, whose upstream target already relaxes vendor warnings.

## Observed results

Linux x86-64 was exercised with GCC 14.2.0, CMake 4.4.4, Qt 5.15.15's QSQLITE
plugin, OpenSSL 3.5.7 and zlib 1.3.1. SQLCipher is vendored **4.16.0** on SQLite
**3.53.1**, as declared by the pinned dependency. These are runtime/build
observations from this environment, not a new project minimum.

| Check | Result |
| --- | --- |
| Unpatched SQLCipher dependency and three examples compile | Passed |
| Unpatched empty-key private-snapshot open | Failed with `SQLITE_MISUSE`; patch required |
| Patched shared dependency and three examples compile | Passed |
| Patched shared probe, with ordinary SQLite and Qt QSQLITE | 35 checks passed, 1 CTest passed |
| Crypto-disabled shared probe, with ordinary SQLite and Qt QSQLITE | 13 checks passed, 1 CTest passed after the vendor-warning workaround |
| Patched static dependency build and installed CMake consumer | Passed |
| Static probe with direct ordinary SQLite C API | Failed the isolation check: its direct `sqlite3_*` calls resolve to the statically linked SQLCipher |
| Static probe with Qt QSQLITE, direct ordinary C API check disabled | 29 checks passed, 1 CTest passed |
| Non-WAL fixture directory contents before/after reads | Byte-for-byte equal, including names and directories |
| Direct read-only open of a WAL-mode fixture with absent sidecars | Creates sidecars; intentionally demonstrated as a limitation |
| `nm -D --defined-only` on shared libdjinterop | No exported `sqlite3` or `sqlcipher` symbols |
| Engine DJ smoke test | Track creation, playlist membership and close/reload passed |
| Upstream Boost-based unit tests | Skipped: Boost filesystem/system development packages unavailable |
| Full Mixxx application, macOS, Windows, Flatpak runtime and physical players | Not exercised by this probe |

The fixture is schema-derived and synthetic. Known expected values exercise
Unicode metadata, raw key notation, color, integer tempo, second-based duration,
lookups, source IDs, nested playlist order, repeated membership, missing media,
empty collections, wrong keys, empty fields and unsupported version behavior.
The fixture manifest records its provenance and checksum. The same dependency
creates and reads its encrypted fixtures, so these checks are not independent
evidence of rekordbox interoperability.

The root snapshot service separately qualifies committed-WAL reads and source
integrity. SQLCipher 4.16 rejects `sqlite3_backup_init()` on encrypted databases
with "backup is not supported with encrypted databases". Its encrypted snapshot
path therefore uses `sqlcipher_export()` into private storage inside a held read
transaction, under the source-protecting VFS. Ordinary SQLite backup guidance
must not be assumed to apply unchanged to this encrypted runtime.

The linked shared library requires `libcrypto.so.3`, `libz.so.1` and the platform
C/C++ runtime. OpenSSL's CMake targets include SSL and Crypto; this Linux link
discarded the unused SSL library. Missing crypto DLL/dylib behavior on the other
platforms remains an acceptance check.

The static test uses a separate dependency build with `BUILD_SHARED_LIBS=OFF`
and a probe build with `PROBE_WITH_SYSTEM_SQLITE=OFF`. This is a distinct linkage
profile: direct SQLite C calls in that executable cannot select a separate
ordinary SQLite merely by linking both libraries with the same symbol names.
Qt's dynamically loaded QSQLITE plugin did retain its own ordinary SQLite and
passed before and after encrypted reads. However, Mixxx's
`src/util/db/dbconnection.cpp` passes Qt connection handles to direct
`sqlite3_create_function()` and `sqlite3_create_collation()` calls. Resolving those
calls to SQLCipher is a concrete integration blocker. **Do not link this
SQLCipher archive into the main Mixxx executable.**

The integration candidate is a private shared C++ module containing hidden
SQLCipher and libdjinterop symbols. Its public boundary exposes only the source
request and immutable records; no `sqlite3*`, statement or dependency object may
cross it. The composed module consumer must link ordinary SQLite, prove that
its SQLite symbols remain ordinary before/after a read, and repeat the Qt-handle
function/collation checks. The dependency probe alone does not qualify that
separate boundary.

The [isolated reader project](../database_readers/README.md) now builds that
boundary with private static archives. Its Linux Qt 6.8.2 consumer passed the
direct function/collation checks on Qt-owned handles with SQLCipher enabled and
disabled, and symbol inspection found only `readDatabaseSource()` exported.
Those results qualify the standalone module candidate; central application
registration and the full M2 acceptance gate remain with A.

## Fields and limitations the adapter must handle

| Requirement | Dependency behavior / adapter responsibility |
| --- | --- |
| Metadata, key spelling, track color, original paths | Available through `onelibrary::v1::content_table` |
| Tempo and duration | `bpm_x100` is integer hundredths; duration is whole seconds |
| Tree order and repeated membership | `root_ids()`, `child_ids()` and `track_ids()` preserve order and repeated entries |
| Empty versus absent text | Low-level API collapses empty strings; use checked raw snapshot queries for this distinction |
| Source identity | `database::uuid()` uses first `masterDbId`; it is an originating desktop identity, not a device identity, and is empty for empty sources |
| Schema profile | Upstream checks tables/indexes and a nonempty version; Mixxx must explicitly accept only its qualified `dbVersion=1000` profile |
| Artwork, playlist kind and original membership positions | Require raw snapshot fields; not all are exposed by the public abstraction |
| ANLZ | Analysis locator is available; cue/grid/waveform accessors return empty; B owns decoding |
| Permissions and path resolution | Hold A's access lease and resolve original asset paths against the real device, not the private database directory |
| Native v6/v7 | Separate adapter and separately qualified key/schema profile; OneLibrary evidence does not establish native coverage |

## Packaging proposal for A

The standalone build is intentionally unregistered in central CMake/CI. A owns
that registration. Use the private shared module described above behind an
explicit experimental feature gate until the composed M2 acceptance checks
pass. The normal feature-off application build should retain its current Engine
dependency and should not require crypto. Any future dependency unification
must retain the SQLite/Qt handle boundary.

| Platform | Concrete recipe / gate |
| --- | --- |
| Linux | Use the commands above with system OpenSSL/zlib for the standalone probe. The application candidate must use the private shared module, hide all dependency/SQLite symbols and run the composed consumer with the exact Qt plugin shipped. Inspect `ldd` and symbol exports. |
| Windows | Configure the same source/patch with the project's vcpkg toolchain and matching triplet/runtime; set `EXPERIMENTAL_ENABLE_SQLCIPHER=ON`, `SYSTEM_SQLITE=OFF`. Place DjInterop/SQLCipher inside the private DLL, export only its record API, and bundle matching crypto runtime DLLs where dynamically linked. Inspect exports with `dumpbin` and test with missing DLLs and the shipped Qt plugin. Not executed here; the snapshot service currently refuses Windows pending private-directory ACL qualification. |
| macOS | Build the private shared module for each supported architecture with matching OpenSSL and zlib, using `CMAKE_OSX_ARCHITECTURES` and `OPENSSL_ROOT_DIR`. Inspect symbol visibility, `otool -L`, install names, bundled dylibs and signing. Run sandbox permission/lifetime checks on the composed app; dependency compilation is insufficient. Not executed here. |
| Flatpak | `libdjinterop-flatpak-proposal.yaml` is a standalone probe candidate only. It installs the pinned dependency under `/app/libexec/mixxx-onelibrary-probe`, preserving the application's existing Engine dependency. Do not add that private prefix to the application build or its library path. Run only the dependency probe with that prefix in the actual SDK/sandbox; production qualification must embed static dependency archives inside the private reader module instead. Not included by the production manifest and not executed here. |

The installed CMake config exports SQLCipher and OpenSSL dependencies. The
candidate `.pc` file lacks the extra static crypto dependencies, so a pkg-config
fallback cannot substitute for that config without a separately tested fix.
libdjinterop identifies its source as LGPL-3.0-or-later; SQLCipher's amalgamation
carries its own BSD-style notice, and OpenSSL 3 carries Apache-2.0 terms. Preserve
source/patch provenance and dependency notices in the packaging work.

## Upstream comparison

The actual rekordbox source on the pinned upstream `main` and `2.6` revisions
still discovers `export.pdb` under `PIONEER` and `.PIONEER`, and does not contain
an OneLibrary or native `master.db` adapter. Their `parseDeviceDB()` still calls
`Sandbox::askForAccess()` in its worker path.

The open/closed upstream PR searches for `OneLibrary`, `exportLibrary.db`,
`master.db`, `sqlcipher` and `rekordbox native` found no implementation of this
adapter. The `OneLibrary` search returned changelog PR
[#17445](https://github.com/mixxxdj/mixxx/pull/17445); its inspected patch describes
Engine DJ support through libdjinterop 0.27.1, not a modern rekordbox reader.
The feature request [#15556](https://github.com/mixxxdj/mixxx/issues/15556) remains
open. These are search observations at the recorded date, not a claim that no
unpublished competing implementation exists.

[Upstream #17441](https://github.com/mixxxdj/mixxx/pull/17441) is merged to `2.5`
at `34144dc0112102c2feb653e22eecd9ef7a8cc14c`. Its inspected patch moves the device
permission request to activation on the main thread and holds security tokens
while reading PDB and ANLZ files. It does not supply modern/native reading.
Any shared UI backport belongs to A; no parallel UI change is included here.

[libdjinterop #196](https://github.com/xsco/libdjinterop/pull/196) and
[#213](https://github.com/xsco/libdjinterop/pull/213) are merged at
`597be49121abf175f5347662e425ae28a463d922` and
`3740b07c06d505c3d39ad04d857e7d7176ed8522` respectively.
[#214](https://github.com/xsco/libdjinterop/pull/214) remains open at
`96a6bee8223e8dc2b39081fe56a75b5557f96f78` and concerns empty database creation;
it is not a complete USB exporter.

**Next action:** integrate the pinned candidate and snapshot-only patch through
A's central dependency gate, then run the composed import acceptance corpus on
each platform before advertising M2 support.

Prepared by ChatGPT for human review.
