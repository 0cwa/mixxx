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

mkdir -p "$TEST_ROOT/fake-bin"
toolbox_invocation_log=$TEST_ROOT/toolbox.invocations
cat >"$TEST_ROOT/fake-bin/toolbox" <<'FAKE_TOOLBOX'
#!/usr/bin/env bash
set -euo pipefail
printf 'invoked\n' >>"${FAKE_TOOLBOX_INVOCATION_LOG:?}"
FAKE_TOOLBOX
chmod +x "$TEST_ROOT/fake-bin/toolbox"

# Configure arguments may not replace the enforced install prefix. Check the
# separated, inline, typed, and `-D`-separator forms before toolbox/CMake is
# invoked. CMake's `--install-prefix` configure option must be rejected in
# both its separated and inline forms as well.
assert_fails localuser-configure-prefix-separated \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure --prefix "$TEST_ROOT/escape"
assert_fails localuser-configure-prefix-inline \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure "--prefix=$TEST_ROOT/escape"
assert_fails localuser-configure-install-prefix-separated \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure --install-prefix "$TEST_ROOT/escape"
assert_fails localuser-configure-install-prefix-inline \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure "--install-prefix=$TEST_ROOT/escape"
assert_fails localuser-configure-cmake-prefix-separated \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure -DCMAKE_INSTALL_PREFIX "$TEST_ROOT/escape"
assert_fails localuser-configure-cmake-prefix-inline \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure "-DCMAKE_INSTALL_PREFIX=$TEST_ROOT/escape"
assert_fails localuser-configure-cmake-prefix-typed \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure "-DCMAKE_INSTALL_PREFIX:PATH=$TEST_ROOT/escape"
assert_fails localuser-configure-d-separator \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure -D "CMAKE_INSTALL_PREFIX=$TEST_ROOT/escape"
assert_fails localuser-configure-d-separator-typed \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure -D "CMAKE_INSTALL_PREFIX:PATH=$TEST_ROOT/escape"
[[ ! -s "$toolbox_invocation_log" ]] || \
    fail "configure prefix override reached the toolbox"
for rejected_configure_prefix in \
    localuser-configure-install-prefix-separated \
    localuser-configure-install-prefix-inline; do
    rg -q -- 'Do not override the enforced install prefix: --install-prefix' \
        "$TEST_ROOT/$rejected_configure_prefix.log" || \
        fail "$rejected_configure_prefix did not explain the rejected prefix"
done

assert_succeeds localuser-configure-allowed \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_TOOLBOX_INVOCATION_LOG="$toolbox_invocation_log" \
    MIXXX_INSTALL_PREFIX="$test_home/.local" \
    MIXXX_CONTAINER_INSTALL_PREFIX="$test_home/.local" \
    "$LOCALUSER" configure -DSIGNALSMITH=OFF
[[ -s "$toolbox_invocation_log" ]] || fail "allowed configure did not reach toolbox"

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
        -P)
            printf 'runtime\n' >>"${FAKE_INVOCATION_LOG:?}"
            exit 0
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
printf '# disposable runtime dependency script\n' \
    >"$build_dir/InstallMixxxRuntimeDependencies.cmake"

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
assert_fails cmake-prefix-override-separated \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX=/bin/false \
    "$INSTALLER" "$build_dir" "$live_prefix" lib \
    -DCMAKE_INSTALL_PREFIX "$TEST_ROOT/escape"
assert_fails cmake-prefix-override-typed \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX=/bin/false \
    "$INSTALLER" "$build_dir" "$live_prefix" lib \
    "-DCMAKE_INSTALL_PREFIX:PATH=$TEST_ROOT/escape"
assert_fails cmake-prefix-override-d-separator \
    env HOME="$test_home" MIXXX_CMAKE_COMMAND="$fake_cmake" \
    FAKE_INVOCATION_LOG="$invocation_log" FAKE_MIXXX=/bin/false \
    "$INSTALLER" "$build_dir" "$live_prefix" lib \
    -D "CMAKE_INSTALL_PREFIX=$TEST_ROOT/escape"
[[ ! -s "$invocation_log" ]] || fail "prefix override invoked CMake"
[[ ! -e "$TEST_ROOT/escape" ]] || fail "prefix override created an escape path"

# A failure while allocating the rollback directory must remove the already
# acquired lock and staging directory.
real_mktemp=$(command -v mktemp)
mktemp_fail_count=$TEST_ROOT/mktemp-fail.count
printf '0\n' >"$mktemp_fail_count"
cat >"$TEST_ROOT/fake-bin/mktemp" <<'FAKE_MKTEMP'
#!/usr/bin/env bash
set -euo pipefail
count=$(<"${FAKE_MKTEMP_COUNT:?}")
count=$((count + 1))
printf '%s\n' "$count" >"$FAKE_MKTEMP_COUNT"
if ((count == 2)); then
    exit 73
fi
exec "$REAL_MKTEMP" "$@"
FAKE_MKTEMP
chmod +x "$TEST_ROOT/fake-bin/mktemp"
setup_failure_prefix=$test_home/.local/setup-failure
assert_fails setup-failure \
    env HOME="$test_home" PATH="$TEST_ROOT/fake-bin:$PATH" \
    FAKE_MKTEMP_COUNT="$mktemp_fail_count" REAL_MKTEMP="$real_mktemp" \
    MIXXX_CMAKE_COMMAND="$fake_cmake" FAKE_INVOCATION_LOG="$invocation_log" \
    FAKE_MIXXX=/bin/false "$INSTALLER" "$build_dir" \
    "$setup_failure_prefix" lib
[[ ! -e "${setup_failure_prefix}.mixxx-install-lock" ]] || \
    fail "setup failure left the install lock"
if find "$test_home/.local" -maxdepth 1 \
    \( -name '.mixxx-install.*' -o -name '.mixxx-rollback.*' \) \
    -print -quit | grep -q .; then
    fail "setup failure left disposable install state"
fi

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

# Direct CMake installs must not enter the wrapper-only runtime staging path,
# even when supplied with the old marker and a spoof-shaped prefix. The
# generated runtime script is deliberately present but is not an install rule.
mkdir -p "$TEST_ROOT/guard-source"
printf '%s\n' 'payload' >"$TEST_ROOT/guard-source/payload"
printf '%s\n' \
    'cmake_minimum_required(VERSION 3.24)' \
    'project(mixxx-runtime-install-guard NONE)' \
    "install(FILES \"\${CMAKE_CURRENT_SOURCE_DIR}/payload\" DESTINATION .)" \
    >"$TEST_ROOT/guard-source/CMakeLists.txt"
cmake -S "$TEST_ROOT/guard-source" -B "$TEST_ROOT/guard-build" \
    >"$TEST_ROOT/guard-configure.log"
printf '%s\n' \
    "file(WRITE \"$TEST_ROOT/runtime-was-invoked\" yes)" \
    >"$TEST_ROOT/guard-build/InstallMixxxRuntimeDependencies.cmake"
assert_succeeds direct-cmake-install-spoof \
    env MIXXX_LOCALUSER_INSTALL_STAGING=1 cmake --install \
    "$TEST_ROOT/guard-build" --prefix "$TEST_ROOT/.mixxx-install.spoof"
[[ -e "$TEST_ROOT/.mixxx-install.spoof/payload" ]] || \
    fail "direct CMake install did not copy ordinary payload"
[[ ! -e "$TEST_ROOT/runtime-was-invoked" ]] || \
    fail "direct CMake install invoked the runtime dependency script"
if rg -q 'CheckMixxxRuntimeInstall' "$ROOT/CMakeLists.txt"; then
    fail "CMakeLists.txt still registers the obsolete direct-install guard"
fi

printf 'PASS: localuser installer safety, rollback, ELF paths, spoof, and idempotence\n'
