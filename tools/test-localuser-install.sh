#!/usr/bin/env bash
set -euo pipefail

script_dir=${BASH_SOURCE[0]%/*}
ROOT=$(cd -- "$script_dir/.." && pwd -P)
INSTALLER=$ROOT/tools/localuser-install.sh
LOCALUSER=$ROOT/localuser.sh
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/mixxx-localuser-install.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

assert_fails() {
    local label=$1
    shift
    if "$@" >"$TEST_ROOT/$label.log" 2>&1; then
        fail "$label unexpectedly succeeded"
    fi
}

assert_succeeds() {
    local label=$1
    shift
    if ! "$@" >"$TEST_ROOT/$label.log" 2>&1; then
        cat "$TEST_ROOT/$label.log" >&2
        fail "$label failed"
    fi
}

test_home=$TEST_ROOT/home
live_prefix=$test_home/.local/mixxx
build_dir=$TEST_ROOT/build
mkdir -p "$test_home" "$build_dir"

# Prefix validation must reject roots, traversal, and the private Mixxx state
# directory before creating a lock or touching the target.
for unsafe_prefix in / ../../../../ "$test_home/../.." "$test_home/.mixxx"; do
    assert_fails "localuser-prefix-${#unsafe_prefix}" \
        env HOME="$test_home" MIXXX_INSTALL_PREFIX="$unsafe_prefix" \
        MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
        "$LOCALUSER" help
done
assert_fails localuser-container-root \
    env HOME="$test_home" MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX=/ "$LOCALUSER" help

assert_succeeds localuser-prefix-normal \
    env HOME="$test_home" MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" "$LOCALUSER" help

for unsafe_prefix in / ../../../../ "$test_home/../.." "$test_home/.mixxx"; do
    assert_fails "installer-prefix-${#unsafe_prefix}" \
        env HOME="$test_home" "$INSTALLER" "$build_dir" "$unsafe_prefix" lib
done

case "$HOME" in
    /home/*|/var/home/*)
        user_name=${HOME##*/}
        for normal_alias in "/home/$user_name/.local" "/var/home/$user_name/.local"; do
            assert_succeeds "localuser-alias-${#normal_alias}" \
                env HOME="$HOME" MIXXX_INSTALL_PREFIX="$normal_alias" \
                MIXXX_CONTAINER_INSTALL_PREFIX="/home/$user_name/.local" \
                "$LOCALUSER" help
        done
        ;;
esac

cat >"$TEST_ROOT/fake-cmake" <<'FAKE_CMAKE'
#!/usr/bin/env bash
set -euo pipefail

printf 'invoked\n' >>"${FAKE_INVOCATION_LOG:?}"
stage=
while (($#)); do
    case "$1" in
        --prefix)
            stage=$2
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done
: "${stage:?missing staging prefix}"

mkdir -p "$stage/bin"
cp -- "${FAKE_MIXXX:?}" "$stage/bin/mixxx"

if [[ -n ${FAKE_EXTRA:-} ]]; then
    mkdir -p "$stage/lib/mixxx"
    cp -- "$FAKE_EXTRA" "$stage/lib/mixxx/libfixture.so.1.0.0"
fi

if [[ ${FAKE_LAYOUT:-0} == 1 ]]; then
    mkdir -p "$stage/lib/mixxx"
    cp -- "${FAKE_LIBRARY:?}" "$stage/lib/mixxx/libfixture.so.1.0.0"
    ln -s libfixture.so.1.0.0 "$stage/lib/mixxx/libfixture.so.1"
    ln -s libfixture.so.1 "$stage/lib/mixxx/libfixture.so"
fi

if [[ ${FAKE_MODE:-success} == fail ]]; then
    exit 42
fi
FAKE_CMAKE
chmod +x "$TEST_ROOT/fake-cmake"

fake_cmake=$TEST_ROOT/fake-cmake
invocation_log=$TEST_ROOT/fake-cmake.invocations
: >"$invocation_log"

# User install arguments may not replace the disposable prefix. This check is
# deliberately made before the fake CMake command can write anything.
assert_fails prefix-override \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX=/bin/false \
    "$INSTALLER" "$build_dir" "$live_prefix" lib \
    --prefix "$TEST_ROOT/escape"
assert_fails prefix-override-inline \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX=/bin/false \
    "$INSTALLER" "$build_dir" "$live_prefix" lib \
    "--prefix=$TEST_ROOT/escape"
assert_fails cmake-prefix-override \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX=/bin/false \
    "$INSTALLER" "$build_dir" "$live_prefix" lib \
    "-DCMAKE_INSTALL_PREFIX=$TEST_ROOT/escape"
[[ ! -s "$invocation_log" ]] || fail "prefix override invoked CMake"
[[ ! -e "$TEST_ROOT/escape" ]] || fail "prefix override created an escape path"

# A simulated dependency-scan failure must leave the existing live install
# byte-for-byte unchanged and remove all disposable state.
mkdir -p "$live_prefix/bin"
printf 'existing-install\n' >"$live_prefix/bin/mixxx"
cp -- "$live_prefix/bin/mixxx" "$TEST_ROOT/live-before"
assert_fails dependency-failure \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX=/bin/false \
    FAKE_MODE=fail "$INSTALLER" "$build_dir" "$live_prefix" lib
cmp -- "$TEST_ROOT/live-before" "$live_prefix/bin/mixxx" || \
    fail "dependency failure changed the live executable"
if find "$live_prefix" -maxdepth 1 -name '.mixxx-*' -print -quit | grep -q .; then
    fail "dependency failure left disposable install state"
fi

if ! command -v cc >/dev/null 2>&1; then
    fail "cc is required for ELF RUNPATH regression tests"
fi
printf 'int main(void) { return 0; }\n' >"$TEST_ROOT/good.c"
cc -Wl,-rpath,"\$ORIGIN" "$TEST_ROOT/good.c" -o "$TEST_ROOT/good"
printf 'int main(void) { return 0; }\n' >"$TEST_ROOT/bad-executable.c"
cc -Wl,-rpath,/usr/lib "$TEST_ROOT/bad-executable.c" \
    -o "$TEST_ROOT/bad-executable"
printf 'int fixture(void) { return 0; }\n' >"$TEST_ROOT/fixture.c"
cc -shared -fPIC -Wl,-rpath,"\$ORIGIN/../../.." "$TEST_ROOT/fixture.c" \
    -o "$TEST_ROOT/bad-library.so"
cc -shared -fPIC "$TEST_ROOT/fixture.c" -o "$TEST_ROOT/good-library.so"

run_install() {
    local label=$1
    shift
    assert_succeeds "$label" \
        env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
        FAKE_INVOCATION_LOG="$invocation_log" "$@" \
        "$INSTALLER" "$build_dir" "$live_prefix" lib
}

run_install good-elf FAKE_MIXXX="$TEST_ROOT/good" FAKE_MODE=success

# The executable scan must reject an absolute host RUNPATH.
assert_fails bad-executable-runpath \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX="$TEST_ROOT/bad-executable" \
    "$INSTALLER" "$build_dir" "$live_prefix" lib
[[ -x "$live_prefix/bin/mixxx" ]] || fail "bad executable removed the live install"

# The same validation must cover copied shared libraries, including an
# $ORIGIN path that escapes the disposable prefix.
assert_fails bad-library-runpath \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX="$TEST_ROOT/good" \
    FAKE_EXTRA="$TEST_ROOT/bad-library.so" "$INSTALLER" "$build_dir" \
    "$live_prefix" lib
[[ -x "$live_prefix/bin/mixxx" ]] || fail "bad library removed the live install"

# Repeated installs must converge to the same canonical SONAME symlink layout.
run_install first-layout FAKE_MIXXX="$TEST_ROOT/good" \
    FAKE_LIBRARY="$TEST_ROOT/good-library.so" FAKE_LAYOUT=1
find "$live_prefix" -mindepth 1 -printf '%P %y %l\n' | sort >"$TEST_ROOT/layout-before"
run_install second-layout FAKE_MIXXX="$TEST_ROOT/good" \
    FAKE_LIBRARY="$TEST_ROOT/good-library.so" FAKE_LAYOUT=1
find "$live_prefix" -mindepth 1 -printf '%P %y %l\n' | sort >"$TEST_ROOT/layout-after"
cmp -- "$TEST_ROOT/layout-before" "$TEST_ROOT/layout-after" || \
    fail "repeated install changed the canonical layout"

# Verify the wrapper-only CMake install guard with a tiny disposable project.
mkdir -p "$TEST_ROOT/guard-source"
printf '%s\n' 'payload' >"$TEST_ROOT/guard-source/payload"
printf '%s\n' \
    'cmake_minimum_required(VERSION 3.24)' \
    'project(mixxx-runtime-install-guard NONE)' \
    "install(SCRIPT \"$ROOT/cmake/CheckMixxxRuntimeInstall.cmake.in\")" \
    "install(FILES \"\${CMAKE_CURRENT_SOURCE_DIR}/payload\" DESTINATION .)" \
    >"$TEST_ROOT/guard-source/CMakeLists.txt"
cmake -S "$TEST_ROOT/guard-source" -B "$TEST_ROOT/guard-build" \
    >"$TEST_ROOT/guard-configure.log"
assert_fails direct-cmake-install \
    env -u MIXXX_LOCALUSER_INSTALL_STAGING cmake --install \
    "$TEST_ROOT/guard-build" --prefix "$TEST_ROOT/direct-prefix"
[[ ! -e "$TEST_ROOT/direct-prefix/payload" ]] || \
    fail "unguarded direct CMake install copied payload"
assert_fails direct-cmake-install-marker \
    env MIXXX_LOCALUSER_INSTALL_STAGING=1 cmake --install \
    "$TEST_ROOT/guard-build" --prefix "$TEST_ROOT/direct-prefix"
[[ ! -e "$TEST_ROOT/direct-prefix/payload" ]] || \
    fail "marked direct CMake install copied payload"
assert_succeeds wrapped-cmake-install \
    env MIXXX_LOCALUSER_INSTALL_STAGING=1 cmake --install \
    "$TEST_ROOT/guard-build" --prefix "$TEST_ROOT/.mixxx-install.test"
[[ -e "$TEST_ROOT/.mixxx-install.test/payload" ]] || \
    fail "guarded CMake install did not copy payload"

printf 'PASS: localuser installer safety, rollback, ELF paths, guard, and idempotence\n'
