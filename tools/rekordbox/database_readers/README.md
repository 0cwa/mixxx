# Prepared by ChatGPT: Isolated modern/native database readers

This standalone project builds the proposed reader boundary and its tests.
It does not register a feature in Mixxx's central build or publish a sidebar
source. A owns those integration steps.

## Why the boundary is shared

Mixxx passes Qt-owned SQLite handles to direct SQLite C API functions, including
function and collation registration in `src/util/db/dbconnection.cpp`. A static
SQLCipher linked into the main executable replaces those symbols and can receive
handles created by a different SQLite implementation. The dependency probe
demonstrated that symbol collision.

`rekordbox-database-readers` therefore contains its own **private static copies**
of the pinned, patched libdjinterop and SQLCipher inside a shared library. It does
not depend on a public `libdjinterop.so.0` or change the application's existing
0.27.1 Engine DJ dependency. Source requests and immutable result records pass
through `readDatabaseSource()`; database handles, statements and libdjinterop
objects stay inside the module.

The build applies hidden C/C++ visibility and hidden inline visibility to the
dependency archives and reader. On ELF platforms, `--exclude-libs,ALL` and a
strict export map leave only `readDatabaseSource()` exported. The macOS recipe
uses a matching exported-symbol list. Windows uses an explicit `dllexport` on
that boundary and disables automatic exports. macOS/Windows exports and runtime
behavior still require validation on those platforms. The snapshot service
currently refuses Windows pending private-directory ACL qualification.

The caller and module must use compatible C++ standard libraries, runtimes,
compiler ABI settings and record headers. Keep the module loaded for the entire
lifetime of returned records and access leases. This is an internal application
boundary, not a promise of a stable third-party binary ABI.

`rekordbox-database-test-core` exposes internals only to isolated test programs.
**Never link that static test library into Mixxx.**

## Build and test

First obtain the exact dependency and apply the downstream snapshot patch as
described in [the dependency probe](../onelibrary_probe/README.md). Configuration
checks both the source commit and the complete tracked diff against that patch.
No installed DjInterop package is used by this project.

```sh
mixxx_checkout=/absolute/path/to/mixxx
dependency_checkout=/absolute/path/to/patched/libdjinterop
reader_build=/absolute/path/to/reader-build
cmake -S "$mixxx_checkout/tools/rekordbox/database_readers" -B "$reader_build" \
    -DLIBDJINTEROP_SOURCE_DIR="$dependency_checkout" \
    -DCMAKE_BUILD_TYPE=Release \
    -DREKORDBOX_DATABASE_QT_ISOLATION_TEST=ON
cmake --build "$reader_build" --parallel 2
ctest --test-dir "$reader_build" --output-on-failure
```

Qt Core/SQL development files and the QSQLITE runtime plugin are needed for the
Qt isolation check. With the option enabled, the shared consumer links only the
reader module, ordinary SQLite and Qt. It verifies that its direct SQLite C API
remains ordinary, registers functions and collations on Qt-owned handles, and
checks them before and after every source read. The source fixture is created
with that independent ordinary SQLite. Synthetic ANLZ bytes reach B's existing
decoder through the returned asset locator; no second decoder is built.

The live-WAL test launches the importer and SQLite writer in separate processes
and records their distinct process IDs. It checks committed WAL-only rows, a
writer commit during a pinned reader transaction, the next snapshot seeing that
commit, and uncommitted writes remaining invisible. Complete source manifests
are compared while the writer is paused, and private snapshot cleanup is
checked. Its optional syscall trace reports when tracing is unavailable instead
of treating that as observed evidence.

Use a separate build directory with
`-DREKORDBOX_DATABASE_WITH_SQLCIPHER=OFF` to test explicit OneLibrary rejection,
native plaintext reading and ordinary-SQLite snapshot behavior. That validation
build still uses OpenSSL for snapshot SHA-256 fingerprints; it does not claim
runtime behavior when OpenSSL itself is missing. The feature-off Mixxx
application build remains unchanged until A registers a module dependency.

`-DREKORDBOX_DATABASE_SANITIZERS=ON` instruments the reader and test code with
AddressSanitizer and UndefinedBehaviorSanitizer on supported compilers. It does
not instrument the entire vendored dependency.

On Linux, inspect the resulting boundary as part of the acceptance check:

```sh
nm -D --defined-only "$reader_build/librekordbox-database-readers.so" | c++filt
ldd "$reader_build/librekordbox-database-readers.so"
ldd "$reader_build/rekordbox-database-shared-consumer"
```

The first command must show only `mixxx::rekordbox::readDatabaseSource(...)`.
The module must have no dynamic dependency on a public DjInterop or ordinary
SQLite library. The consumer must retain its ordinary SQLite dependency and
must pass the actual function/collation checks; symbol inspection alone is
insufficient.

## Executed qualification

Linux x86-64 builds used GCC 14.2.0, CMake 4.4.4, OpenSSL 3.5.7 and the exact
source/patch above. SQLCipher-enabled builds use SQLite 3.53.1 inside SQLCipher
4.16.0; feature-off builds use the pinned checkout's ordinary SQLite 3.53.4.
The final consumer was compiled against Qt 6.8.2 and exercised
its matching QSQLITE plugin with ordinary system SQLite. Matching Debian Qt
development files and the plugin were extracted into a workspace prefix; no
system packages were replaced. An earlier crypto-enabled run also passed with
Qt 5.15.15.

| Test group | SQLCipher enabled | SQLCipher disabled |
| --- | --- | --- |
| Snapshot service | 24 cases passed | 19 cases passed |
| Native record reader | 45 cases passed | 45 cases passed |
| Composed source request | 14 cases passed | 8 cases passed |
| Shared consumer with ordinary SQLite and actual Qt 6 handles | 9 cases passed | 8 cases passed |
| OneLibrary record reader and format selection | 34 cases passed | Explicit rejection covered by the composed tests |
| Separate-process live WAL | 4 scenarios passed | 4 scenarios passed |
| Aggregate CTest result | 6/6 passed | 5/5 passed |

Both module builds export exactly `readDatabaseSource()` and have no dynamic
DjInterop or SQLite dependency. Both consumers retain their ordinary SQLite and
Qt 6 Core/SQL dynamic dependencies. CMake emits `compile_commands.json` for the
exact build inputs. The live-WAL source manifests, consistency checks and
cleanup checks passed; syscall tracing could not run because this environment
denied `ptrace`, so no syscall-level nonmutation result is claimed.

The Debug build with Qt 5.15.15 and AddressSanitizer/UndefinedBehaviorSanitizer
also passed all six CTests and 130 cases/scenarios. The default LeakSanitizer
run failed at process teardown because its tracing is unsupported in this
sandbox. The successful run excluded leak detection explicitly:

```sh
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir "$reader_build" --output-on-failure
```

This establishes the instrumented reader/test result, with leak detection and
vendored-library instrumentation excluded. The machine-readable
[validation manifest](validation-manifest.json) records configuration, case
counts, observed failed experiments, open gates and input checksums. The PR
description binds the final commit to the committed-head verification.

The fixtures are synthetic or identified reference fixtures, and the tests
deliberately keep canonical application and qualified performance capabilities
false. Native v7, genuine OneLibrary exports, physical hardware, platform GUI
permissions and the full M2 import acceptance gate remain separate work.

Prepared by ChatGPT for human review.
