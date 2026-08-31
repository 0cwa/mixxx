#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly SCRIPT_DIR
readonly SECURE_DOWNLOAD_HELPER="$SCRIPT_DIR/secure-download.py"
readonly ONNXRUNTIME_VERSION="1.26.0"
readonly ONNXRUNTIME_ARCHIVE_NAME="onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz"
readonly ONNXRUNTIME_ARCHIVE_ROOT="onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}"
readonly ONNXRUNTIME_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/${ONNXRUNTIME_ARCHIVE_NAME}"
readonly ONNXRUNTIME_SHA256="1254da24fb389cf39dc0ff3451ab48301740ffbfcbaf646849df92f80ee92c57"
ONNXRUNTIME_CLEANUP_DIR=

cleanup() {
    if [[ -n "$ONNXRUNTIME_CLEANUP_DIR" ]]; then
        rm -rf -- "$ONNXRUNTIME_CLEANUP_DIR"
    fi
}

verify_sha256() {
    local expected_sha256="$1"
    local artifact_path="$2"

    if [[ ! "$expected_sha256" =~ ^[[:xdigit:]]{64}$ ]]; then
        echo "Invalid SHA-256 value for $artifact_path" >&2
        return 1
    fi
    if [[ ! -f "$artifact_path" ]]; then
        echo "ONNX Runtime archive is missing: $artifact_path" >&2
        return 1
    fi
    if ! printf '%s  %s\n' "$expected_sha256" "$artifact_path" |
            sha256sum --check --status -; then
        echo "ONNX Runtime archive SHA-256 mismatch: $artifact_path" >&2
        return 1
    fi
}

path_contains_symlink_component() {
    local path_to_check="$1"
    local parent_path

    while [[ -n "$path_to_check" && "$path_to_check" != "/" &&
            "$path_to_check" != "." ]]; do
        if [[ -L "$path_to_check" ]]; then
            return 0
        fi
        parent_path=$(dirname -- "$path_to_check")
        if [[ "$parent_path" == "$path_to_check" ]]; then
            break
        fi
        path_to_check="$parent_path"
    done
    return 1
}

path_contains_symlink_parent_component() {
    path_contains_symlink_component "$(dirname -- "$1")"
}

verify_internal_library_symlink() {
    local symlink_path="$1"
    local runtime_prefix="$2"
    local canonical_prefix
    local current_path="$symlink_path"
    local symlink_target
    local resolved_path
    local link_count=0

    canonical_prefix=$(realpath -e -- "$runtime_prefix") || return 1
    while [[ -L "$current_path" ]]; do
        if path_contains_symlink_parent_component "$current_path"; then
            return 1
        fi
        symlink_target=$(readlink -- "$current_path") || return 1
        if [[ -z "$symlink_target" || "$symlink_target" = /* ]]; then
            return 1
        fi
        current_path="$(dirname -- "$current_path")/$symlink_target"
        link_count=$((link_count + 1))
        if ((link_count > 16)); then
            return 1
        fi
    done

    resolved_path=$(realpath -e -- "$current_path") || return 1
    case "$resolved_path/" in
        "$canonical_prefix/"*) ;;
        *) return 1 ;;
    esac
    [[ -f "$resolved_path" ]]
}

verify_required_file() {
    local required_file="$1"
    local runtime_prefix="$2"
    local allow_internal_symlink="${3:-0}"

    if path_contains_symlink_parent_component "$required_file"; then
        return 1
    fi
    if [[ -L "$required_file" ]]; then
        [[ "$allow_internal_symlink" -eq 1 ]] || return 1
        verify_internal_library_symlink "$required_file" "$runtime_prefix"
        return $?
    fi
    [[ -f "$required_file" ]]
}

verify_prefix() {
    local runtime_prefix="$1"
    local required_file
    local targets_file

    if [[ -z "$runtime_prefix" || ! -d "$runtime_prefix" ]]; then
        echo "ONNX Runtime prefix is missing: $runtime_prefix" >&2
        return 1
    fi
    if path_contains_symlink_component "$runtime_prefix"; then
        echo "ONNX Runtime prefix contains a symlink path component: $runtime_prefix" >&2
        return 1
    fi

    for required_file in \
        "$runtime_prefix/include/onnxruntime_cxx_api.h" \
        "$runtime_prefix/lib/libonnxruntime_providers_shared.so" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeConfig.cmake" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeConfigVersion.cmake" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets.cmake" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"; do
        if ! verify_required_file "$required_file" "$runtime_prefix"; then
            echo "ONNX Runtime prefix is incomplete; missing: $required_file" >&2
            return 1
        fi
    done

    required_file="$runtime_prefix/lib/libonnxruntime.so"
    if ! verify_required_file "$required_file" "$runtime_prefix" 1; then
        echo "ONNX Runtime prefix is incomplete; missing: $required_file" >&2
        return 1
    fi

    for targets_file in \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets.cmake" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"; do
        if grep -Eq '/include/onnxruntime|/lib64/' "$targets_file"; then
            echo "ONNX Runtime CMake targets contain unnormalized paths: $runtime_prefix" >&2
            return 1
        fi
    done
}

verify_archive_layout() {
    local archive_path="$1"
    local archive_listing
    local archive_entry
    local relative_entry
    local required_entry

    if ! archive_listing=$(tar --list --gzip --file="$archive_path"); then
        echo "Could not inspect ONNX Runtime archive: $archive_path" >&2
        return 1
    fi

    while IFS= read -r archive_entry; do
        [[ -n "$archive_entry" ]] || continue
        case "$archive_entry" in
            "$ONNXRUNTIME_ARCHIVE_ROOT"|"$ONNXRUNTIME_ARCHIVE_ROOT/"*)
                ;;
            *)
                echo "ONNX Runtime archive has an unexpected top-level entry: $archive_entry" >&2
                return 1
                ;;
        esac
        relative_entry="${archive_entry#"$ONNXRUNTIME_ARCHIVE_ROOT"}"
        case "$relative_entry" in
            *"/../"*|*"/.."|"../"*|".."|*"/./"*|*"/."|"./"*)
                echo "ONNX Runtime archive has an unsafe relative entry: $archive_entry" >&2
                return 1
                ;;
        esac
    done <<< "$archive_listing"

    for required_entry in \
        include/onnxruntime_cxx_api.h \
        lib/libonnxruntime.so \
        lib/libonnxruntime.so.1 \
        lib/libonnxruntime.so.1.26.0 \
        lib/libonnxruntime_providers_shared.so \
        lib/cmake/onnxruntime/onnxruntimeConfig.cmake \
        lib/cmake/onnxruntime/onnxruntimeConfigVersion.cmake \
        lib/cmake/onnxruntime/onnxruntimeTargets.cmake \
        lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake; do
        if ! grep -Fqx "$ONNXRUNTIME_ARCHIVE_ROOT/$required_entry" <<< "$archive_listing" &&
                ! grep -Fqx "$ONNXRUNTIME_ARCHIVE_ROOT/$required_entry/" <<< "$archive_listing"; then
            echo "ONNX Runtime archive is missing required entry: $required_entry" >&2
            return 1
        fi
    done
}

stage_archive() {
    local archive_path="$1"
    local runtime_prefix="$2"
    local parent_dir
    local temporary_prefix
    local targets_file

    if [[ ! -f "$archive_path" ]] ||
            path_contains_symlink_component "$archive_path"; then
        echo "Cannot stage missing ONNX Runtime archive: $archive_path" >&2
        return 1
    fi
    if ! verify_archive_layout "$archive_path"; then
        return 1
    fi

    parent_dir=$(dirname -- "$runtime_prefix")
    if path_contains_symlink_component "$runtime_prefix"; then
        echo "ONNX Runtime prefix contains a symlink path component: $runtime_prefix" >&2
        return 1
    fi
    mkdir -p -- "$parent_dir"
    if [[ -e "$runtime_prefix" || -L "$runtime_prefix" ]]; then
        echo "Refusing to overwrite existing ONNX Runtime prefix: $runtime_prefix" >&2
        return 1
    fi

    temporary_prefix=$(mktemp -d "$parent_dir/.onnxruntime-prefix.XXXXXX")
    if ! tar --extract --gzip --file="$archive_path" \
            --strip-components=1 --directory="$temporary_prefix"; then
        rm -rf -- "$temporary_prefix"
        echo "Could not extract ONNX Runtime archive: $archive_path" >&2
        return 1
    fi

    for targets_file in \
        "$temporary_prefix/lib/cmake/onnxruntime/onnxruntimeTargets.cmake" \
        "$temporary_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"; do
        if [[ ! -f "$targets_file" ]]; then
            rm -rf -- "$temporary_prefix"
            echo "ONNX Runtime archive has incomplete CMake targets: $archive_path" >&2
            return 1
        fi
        # The upstream archive's generated target files retain the build-time
        # include/onnxruntime and lib64 paths even though the archive delivers
        # flat headers in include/ and shared libraries in lib/.
        sed -i \
            -e 's#/include/onnxruntime#/include#g' \
            -e 's#/lib64/#/lib/#g' \
            "$targets_file"
    done

    if ! verify_prefix "$temporary_prefix"; then
        rm -rf -- "$temporary_prefix"
        return 1
    fi

    # GNU mv -T treats the destination as an exact path and never descends
    # into a directory symlink; a raced destination is rejected safely.
    if ! mv -T -- "$temporary_prefix" "$runtime_prefix"; then
        rm -rf -- "$temporary_prefix"
        echo "Could not safely install ONNX Runtime at: $runtime_prefix" >&2
        return 1
    fi
    printf 'Staged ONNX Runtime %s at %s\n' "$ONNXRUNTIME_VERSION" "$runtime_prefix"
}

stage_runtime() {
    local runtime_prefix="${1:-${MIXXX_ONNX_RUNTIME_PREFIX:-}}"
    local parent_dir
    local temporary_dir
    local archive_path

    if [[ -z "$runtime_prefix" ]]; then
        echo "Set MIXXX_ONNX_RUNTIME_PREFIX or pass an ONNX Runtime prefix to stage." >&2
        return 1
    fi
    if [[ -e "$runtime_prefix" || -L "$runtime_prefix" ]]; then
        verify_prefix "$runtime_prefix"
        echo "Using existing verified ONNX Runtime prefix: $runtime_prefix"
        return 0
    fi

    parent_dir=$(dirname -- "$runtime_prefix")
    if path_contains_symlink_component "$runtime_prefix"; then
        echo "ONNX Runtime prefix contains a symlink path component: $runtime_prefix" >&2
        return 1
    fi
    mkdir -p -- "$parent_dir"
    temporary_dir=$(mktemp -d "$parent_dir/.onnxruntime-download.XXXXXX")
    archive_path="$temporary_dir/$ONNXRUNTIME_ARCHIVE_NAME"
    ONNXRUNTIME_CLEANUP_DIR="$temporary_dir"
    trap cleanup EXIT HUP INT TERM

    python3 "$SECURE_DOWNLOAD_HELPER" "$archive_path" \
        --verify-sha256 "$ONNXRUNTIME_SHA256" \
        -- \
        curl \
        --fail \
        --location \
        --proto '=https' \
        --retry 3 \
        --retry-all-errors \
        --silent \
        --show-error \
        --tlsv1.2 \
        "$ONNXRUNTIME_URL"
    stage_archive "$archive_path" "$runtime_prefix"
    trap - EXIT HUP INT TERM
    cleanup
    ONNXRUNTIME_CLEANUP_DIR=
}

run_self_tests() {
    local test_dir
    local archive_root
    local archive_path
    local runtime_prefix
    local targets_file
    local actual_sha256
    local fake_bin
    local real_mv
    local race_prefix
    local race_target
    local race_target_marker
    local symlink_prefix
    local symlink_required_file
    local symlink_required_target
    local library_link
    local library_link_target
    local external_library_target
    local cmake_project
    local cmake_output
    local incomplete_archive_dir
    local incomplete_archive_root
    local incomplete_archive_path
    local unexpected_archive_dir
    local unexpected_archive_root
    local unexpected_archive_path

    test_dir=$(mktemp -d "${TMPDIR:-/tmp}/mixxx-onnxruntime-test.XXXXXX")
    ONNXRUNTIME_CLEANUP_DIR="$test_dir"
    trap cleanup EXIT HUP INT TERM

    archive_root="$test_dir/onnxruntime-linux-x64-$ONNXRUNTIME_VERSION"
    mkdir -p \
        "$archive_root/include" \
        "$archive_root/lib/cmake/onnxruntime"
    printf 'test header\n' > "$archive_root/include/onnxruntime_cxx_api.h"
    : > "$archive_root/lib/libonnxruntime.so.1.26.0"
    ln -s -- libonnxruntime.so.1.26.0 "$archive_root/lib/libonnxruntime.so.1"
    ln -s -- libonnxruntime.so.1 "$archive_root/lib/libonnxruntime.so"
    : > "$archive_root/lib/libonnxruntime_providers_shared.so"
    cat >"$archive_root/lib/cmake/onnxruntime/onnxruntimeConfig.cmake" <<'EOF'
include("${CMAKE_CURRENT_LIST_DIR}/onnxruntimeTargets.cmake")
EOF
    printf 'set(PACKAGE_VERSION "1.26.0")\n' > \
        "$archive_root/lib/cmake/onnxruntime/onnxruntimeConfigVersion.cmake"
    # The generated fixture intentionally contains literal CMake variables.
    # Keep the target files close enough to the real export for the CMake
    # probe below to exercise the imported target and its resolved paths.
    cat >"$archive_root/lib/cmake/onnxruntime/onnxruntimeTargets.cmake" <<'EOF'
get_filename_component(_IMPORT_PREFIX "${CMAKE_CURRENT_LIST_FILE}" PATH)
get_filename_component(_IMPORT_PREFIX "${_IMPORT_PREFIX}" PATH)
get_filename_component(_IMPORT_PREFIX "${_IMPORT_PREFIX}" PATH)
get_filename_component(_IMPORT_PREFIX "${_IMPORT_PREFIX}" PATH)
add_library(onnxruntime::onnxruntime SHARED IMPORTED)
set_target_properties(
    onnxruntime::onnxruntime
    PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include/onnxruntime"
)
include("${CMAKE_CURRENT_LIST_DIR}/onnxruntimeTargets-release.cmake")
EOF
    cat >"$archive_root/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake" <<'EOF'
set_target_properties(
    onnxruntime::onnxruntime
    PROPERTIES
    IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib64/libonnxruntime.so.1.26.0"
)
EOF

    archive_path="$test_dir/$ONNXRUNTIME_ARCHIVE_NAME"
    tar --create --gzip --file="$archive_path" \
        --directory="$test_dir" "onnxruntime-linux-x64-$ONNXRUNTIME_VERSION"
    actual_sha256=$(sha256sum "$archive_path")
    actual_sha256="${actual_sha256%% *}"
    verify_sha256 "$actual_sha256" "$archive_path"
    if verify_sha256 \
            "0000000000000000000000000000000000000000000000000000000000000000" \
            "$archive_path"; then
        echo "Self-test failed: checksum mismatch was accepted" >&2
        return 1
    fi

    incomplete_archive_dir="$test_dir/incomplete-archive"
    incomplete_archive_root="$incomplete_archive_dir/$ONNXRUNTIME_ARCHIVE_ROOT"
    mkdir -p -- "$incomplete_archive_root/include"
    cp -a "$archive_root/." "$incomplete_archive_root/"
    rm -- "$incomplete_archive_root/include/onnxruntime_cxx_api.h"
    incomplete_archive_path="$test_dir/incomplete-$ONNXRUNTIME_ARCHIVE_NAME"
    tar --create --gzip --file="$incomplete_archive_path" \
        --directory="$incomplete_archive_dir" "$ONNXRUNTIME_ARCHIVE_ROOT"
    if stage_archive "$incomplete_archive_path" "$test_dir/incomplete-runtime"; then
        echo "Self-test failed: incomplete ONNX Runtime archive was accepted" >&2
        return 1
    fi

    unexpected_archive_dir="$test_dir/unexpected-archive"
    unexpected_archive_root="$unexpected_archive_dir/unexpected-root"
    mkdir -p -- "$unexpected_archive_root/include"
    printf 'test header\n' > "$unexpected_archive_root/include/onnxruntime_cxx_api.h"
    unexpected_archive_path="$test_dir/unexpected-$ONNXRUNTIME_ARCHIVE_NAME"
    tar --create --gzip --file="$unexpected_archive_path" \
        --directory="$unexpected_archive_dir" unexpected-root
    if stage_archive "$unexpected_archive_path" "$test_dir/unexpected-runtime"; then
        echo "Self-test failed: unexpected ONNX Runtime archive root was accepted" >&2
        return 1
    fi

    runtime_prefix="$test_dir/runtime"
    stage_archive "$archive_path" "$runtime_prefix"
    verify_prefix "$runtime_prefix"
    targets_file="$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"
    if ! grep -q '/lib/libonnxruntime.so' "$targets_file"; then
        echo "Self-test failed: the lib64 CMake target path was not normalized" >&2
        return 1
    fi
    if [[ "$(readlink -- "$runtime_prefix/lib/libonnxruntime.so")" != \
            'libonnxruntime.so.1' ]] ||
            [[ "$(readlink -- "$runtime_prefix/lib/libonnxruntime.so.1")" != \
            'libonnxruntime.so.1.26.0' ]]; then
        echo "Self-test failed: the ONNX Runtime library symlink chain changed" >&2
        return 1
    fi
    targets_file="$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets.cmake"
    if grep -q '/include/onnxruntime' "$targets_file" ||
            ! grep -q '/include"' "$targets_file"; then
        echo "Self-test failed: the ONNX Runtime include target path was not normalized" >&2
        return 1
    fi

    cmake_project="$test_dir/cmake-project"
    cmake_output="$test_dir/cmake-output"
    mkdir -- "$cmake_project"
    cat >"$cmake_project/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.22)
project(onnxruntime_target_probe NONE)
find_package(onnxruntime CONFIG REQUIRED)
if(NOT TARGET onnxruntime::onnxruntime)
    message(FATAL_ERROR "ONNX Runtime imported target is missing")
endif()
get_target_property(probe_include onnxruntime::onnxruntime INTERFACE_INCLUDE_DIRECTORIES)
get_target_property(probe_location onnxruntime::onnxruntime IMPORTED_LOCATION_RELEASE)
if(NOT probe_include STREQUAL "${CMAKE_PREFIX_PATH}/include")
    message(FATAL_ERROR "ONNX Runtime imported target has the wrong include path")
endif()
if(NOT probe_location STREQUAL "${CMAKE_PREFIX_PATH}/lib/libonnxruntime.so.1.26.0")
    message(FATAL_ERROR "ONNX Runtime imported target has the wrong library path")
endif()
EOF
    if ! cmake -S "$cmake_project" -B "$test_dir/cmake-build" \
            -DCMAKE_PREFIX_PATH="$runtime_prefix" >"$cmake_output" 2>&1; then
        cat "$cmake_output" >&2
        echo "Self-test failed: normalized ONNX Runtime CMake target did not load" >&2
        return 1
    fi

    symlink_prefix="$test_dir/runtime-link"
    ln -s -- "$runtime_prefix" "$symlink_prefix"
    if verify_prefix "$symlink_prefix"; then
        echo "Self-test failed: symlinked ONNX Runtime prefix was accepted" >&2
        return 1
    fi
    symlink_required_file="$runtime_prefix/include/onnxruntime_cxx_api.h"
    symlink_required_target="$runtime_prefix/include/onnxruntime_cxx_api.real.h"
    mv -- "$symlink_required_file" "$symlink_required_target"
    ln -s -- "$symlink_required_target" "$symlink_required_file"
    if verify_prefix "$runtime_prefix"; then
        echo "Self-test failed: symlinked ONNX Runtime file was accepted" >&2
        return 1
    fi
    rm -- "$symlink_required_file"
    mv -- "$symlink_required_target" "$symlink_required_file"

    library_link="$runtime_prefix/lib/libonnxruntime.so"
    external_library_target="$test_dir/external-libonnxruntime.so"
    library_link_target=$(readlink -- "$library_link")
    : > "$external_library_target"
    rm -- "$library_link"
    ln -s -- "$external_library_target" "$library_link"
    if verify_prefix "$runtime_prefix"; then
        echo "Self-test failed: external ONNX Runtime library symlink was accepted" >&2
        return 1
    fi
    rm -- "$library_link"
    ln -s -- "$library_link_target" "$library_link"

    fake_bin="$test_dir/fake-bin"
    mkdir -- "$fake_bin"
    real_mv="$(command -v mv)"
    cat >"$fake_bin/mv" <<'EOF'
#!/bin/bash

set -euo pipefail

if [[ "${FAKE_MV_RACE:-0}" -eq 1 ]]; then
    [[ "$1" == "-T" && "$2" == "--" ]]
    destination="$4"
    ln -s -- "${FAKE_MV_TARGET:?}" "$destination"
fi
exec "$REAL_MV" "$@"
EOF
    chmod +x -- "$fake_bin/mv"
    race_prefix="$test_dir/runtime-race"
    race_target="$test_dir/runtime-race-target"
    race_target_marker="$race_target/marker"
    mkdir -p -- "$race_target"
    printf '%s\n' 'untouched' >"$race_target_marker"
    if ! PATH="$fake_bin:$PATH" \
            FAKE_MV_RACE=1 \
            FAKE_MV_TARGET="$race_target" \
            REAL_MV="$real_mv" \
            stage_archive "$archive_path" "$race_prefix"; then
        :
    else
        echo "Self-test failed: ONNX Runtime destination race was accepted" >&2
        return 1
    fi
    if [[ ! -L "$race_prefix" ]] ||
            [[ "$(cat "$race_target_marker")" != 'untouched' ]] ||
            [[ -e "$race_target/runtime-race" ]]; then
        echo "Self-test failed: ONNX Runtime destination race modified the target" >&2
        return 1
    fi

    trap - EXIT HUP INT TERM
    cleanup
    ONNXRUNTIME_CLEANUP_DIR=
    echo "ONNX Runtime staging self-tests passed."
}

case "${1:-}" in
    stage)
        stage_runtime "${2:-}"
        ;;
    verify)
        verify_prefix "${2:-${MIXXX_ONNX_RUNTIME_PREFIX:-}}"
        ;;
    test)
        run_self_tests
        ;;
    *)
        echo "Usage: $0 {stage [prefix]|verify [prefix]|test}" >&2
        exit 2
        ;;
esac
