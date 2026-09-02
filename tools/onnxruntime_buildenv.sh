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
ONNXRUNTIME_CLEANUP_IDENTITY=

path_identity() {
    stat -c '%d:%i' -- "$1"
}

cleanup() {
    if [[ -n "$ONNXRUNTIME_CLEANUP_DIR" &&
            -n "$ONNXRUNTIME_CLEANUP_IDENTITY" ]]; then
        local cleanup_identity
        cleanup_identity=$(path_identity "$ONNXRUNTIME_CLEANUP_DIR" 2>/dev/null || :)
        if [[ "$cleanup_identity" == "$ONNXRUNTIME_CLEANUP_IDENTITY" ]] &&
                ! path_contains_symlink_component "$ONNXRUNTIME_CLEANUP_DIR"; then
            rm -rf -- "$ONNXRUNTIME_CLEANUP_DIR"
        fi
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

verify_directory_path() {
    local directory_path="$1"

    [[ -d "$directory_path" && ! -L "$directory_path" ]] || return 1
    ! path_contains_symlink_component "$directory_path"
}

remove_owned_path() {
    local path_to_remove="$1"
    local expected_identity="$2"
    local actual_identity

    [[ -e "$path_to_remove" && ! -L "$path_to_remove" ]] || return 0
    actual_identity=$(path_identity "$path_to_remove" 2>/dev/null || :)
    if [[ "$actual_identity" == "$expected_identity" ]]; then
        rm -rf -- "$path_to_remove"
    fi
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

stage_archive() (
    local archive_path="$1"
    local runtime_prefix="$2"
    local parent_dir
    local temporary_prefix
    local targets_file
    local runtime_name
    local parent_identity
    local temporary_prefix_identity
    local temporary_prefix_realpath
    local archive_identity
    local archive_identity_after
    local targets_file_relative

    if [[ ! -f "$archive_path" ]] ||
            path_contains_symlink_component "$archive_path"; then
        echo "Cannot stage missing ONNX Runtime archive: $archive_path" >&2
        return 1
    fi
    archive_identity=$(path_identity "$archive_path") || {
        echo "Cannot identify ONNX Runtime archive: $archive_path" >&2
        return 1
    }
    if ! verify_archive_layout "$archive_path"; then
        return 1
    fi
    if [[ ! -f "$archive_path" ]] ||
            path_contains_symlink_component "$archive_path"; then
        echo "ONNX Runtime archive changed during layout verification: $archive_path" >&2
        return 1
    fi
    archive_identity_after=$(path_identity "$archive_path") || return 1
    if [[ "$archive_identity_after" != "$archive_identity" ]]; then
        echo "ONNX Runtime archive changed during layout verification: $archive_path" >&2
        return 1
    fi

    parent_dir=$(dirname -- "$runtime_prefix")
    if path_contains_symlink_component "$runtime_prefix"; then
        echo "ONNX Runtime prefix contains a symlink path component: $runtime_prefix" >&2
        return 1
    fi
    if ! mkdir -p -- "$parent_dir" || ! verify_directory_path "$parent_dir"; then
        echo "Could not safely create ONNX Runtime prefix parent: $parent_dir" >&2
        return 1
    fi
    parent_dir=$(CDPATH='' cd -P -- "$parent_dir" && pwd -P) || return 1
    if ! CDPATH='' cd -P -- "$parent_dir"; then
        echo "Could not safely enter ONNX Runtime prefix parent: $parent_dir" >&2
        return 1
    fi
    parent_identity=$(path_identity "$parent_dir") || return 1
    runtime_name=$(basename -- "$runtime_prefix")
    runtime_prefix="$parent_dir/$runtime_name"
    if [[ -e "$runtime_name" || -L "$runtime_name" ]]; then
        echo "Refusing to overwrite existing ONNX Runtime prefix: $runtime_prefix" >&2
        return 1
    fi

    temporary_prefix=$(mktemp -d "./.onnxruntime-prefix.XXXXXX") || return 1
    if ! verify_directory_path "$temporary_prefix"; then
        echo "ONNX Runtime temporary prefix has an unsafe path: $temporary_prefix" >&2
        return 1
    fi
    temporary_prefix_identity=$(path_identity "$temporary_prefix") || return 1
    temporary_prefix_realpath=$(CDPATH='' cd -P -- "$temporary_prefix" && pwd -P) || {
        remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
        return 1
    }
    if [[ "$(path_identity .)" != "$parent_identity" ]]; then
        remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
        echo "ONNX Runtime prefix parent changed during staging: $runtime_prefix" >&2
        return 1
    fi

    if ! (
        CDPATH='' cd -P -- "$temporary_prefix" &&
        [[ "$(pwd -P)" == "$temporary_prefix_realpath" ]] &&
        [[ "$(path_identity .)" == "$temporary_prefix_identity" ]] &&
        [[ -f "$archive_path" ]] &&
        ! path_contains_symlink_component "$archive_path" &&
        tar --extract --gzip --file="$archive_path" \
            --strip-components=1 --directory=.
    ); then
        remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
        echo "Could not extract ONNX Runtime archive: $archive_path" >&2
        return 1
    fi
    if ! verify_directory_path "$temporary_prefix" ||
            [[ "$(path_identity "$temporary_prefix")" != "$temporary_prefix_identity" ]]; then
        remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
        echo "ONNX Runtime temporary prefix changed during extraction: $temporary_prefix" >&2
        return 1
    fi

    for targets_file in \
        "$temporary_prefix/lib/cmake/onnxruntime/onnxruntimeTargets.cmake" \
        "$temporary_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"; do
        if [[ ! -f "$targets_file" ]]; then
            remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
            echo "ONNX Runtime archive has incomplete CMake targets: $archive_path" >&2
            return 1
        fi
        targets_file_relative="${targets_file#"$temporary_prefix/"}"
        # The upstream archive's generated target files retain the build-time
        # include/onnxruntime and lib64 paths even though the archive delivers
        # flat headers in include/ and shared libraries in lib/.
        if ! (
            CDPATH='' cd -P -- "$temporary_prefix" &&
            [[ "$(pwd -P)" == "$temporary_prefix_realpath" ]] &&
            [[ "$(path_identity .)" == "$temporary_prefix_identity" ]] &&
            ! path_contains_symlink_component "$targets_file_relative" &&
            sed -i \
                -e 's#/include/onnxruntime#/include#g' \
                -e 's#/lib64/#/lib/#g' \
                "$targets_file_relative"
        ); then
            remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
            echo "ONNX Runtime CMake target changed during normalization: $targets_file" >&2
            return 1
        fi
    done

    if ! (
        CDPATH='' cd -P -- "$temporary_prefix" &&
        [[ "$(pwd -P)" == "$temporary_prefix_realpath" ]] &&
        [[ "$(path_identity .)" == "$temporary_prefix_identity" ]] &&
        verify_prefix .
    ) || ! verify_directory_path "$temporary_prefix" ||
            [[ "$(path_identity "$temporary_prefix")" != "$temporary_prefix_identity" ]]; then
        remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
        return 1
    fi

    if [[ "$(path_identity .)" != "$parent_identity" ]] ||
            path_contains_symlink_component "$runtime_prefix" ||
            [[ -e "$runtime_name" || -L "$runtime_name" ]]; then
        remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
        echo "ONNX Runtime prefix changed before publication: $runtime_prefix" >&2
        return 1
    fi

    # Run from the verified parent and use no-clobber exact-path publication.
    # This keeps a raced final symlink/hard link from being followed or
    # replaced, while the held current directory keeps parent replacement from
    # redirecting the move.
    if ! mv -T --no-clobber -- "$temporary_prefix" "$runtime_name"; then
        remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
        echo "Could not safely install ONNX Runtime at: $runtime_prefix" >&2
        return 1
    fi
    if [[ "$(path_identity .)" != "$parent_identity" ]] ||
            path_contains_symlink_component "$runtime_prefix" ||
            [[ ! -d "$runtime_name" ]] ||
            [[ "$(path_identity "$runtime_name")" != "$temporary_prefix_identity" ]] ||
            ! verify_prefix "$runtime_name"; then
        remove_owned_path "$temporary_prefix" "$temporary_prefix_identity"
        remove_owned_path "$runtime_name" "$temporary_prefix_identity"
        echo "ONNX Runtime prefix changed during publication: $runtime_prefix" >&2
        return 1
    fi
    printf 'Staged ONNX Runtime %s at %s\n' "$ONNXRUNTIME_VERSION" "$runtime_prefix"
)

stage_runtime() (
    local runtime_prefix="${1:-${MIXXX_ONNX_RUNTIME_PREFIX:-}}"
    local parent_dir
    local temporary_dir
    local temporary_directory_path
    local temporary_directory_identity
    local archive_path

    if [[ -z "$runtime_prefix" ]]; then
        echo "Set MIXXX_ONNX_RUNTIME_PREFIX or pass an ONNX Runtime prefix to stage." >&2
        return 1
    fi
    if [[ -e "$runtime_prefix" || -L "$runtime_prefix" ]]; then
        verify_prefix "$runtime_prefix"
        if path_contains_symlink_component "$runtime_prefix"; then
            echo "ONNX Runtime prefix changed while verifying: $runtime_prefix" >&2
            return 1
        fi
        echo "Using existing verified ONNX Runtime prefix: $runtime_prefix"
        return 0
    fi

    parent_dir=$(dirname -- "$runtime_prefix")
    if path_contains_symlink_component "$runtime_prefix"; then
        echo "ONNX Runtime prefix contains a symlink path component: $runtime_prefix" >&2
        return 1
    fi
    if ! mkdir -p -- "$parent_dir" || ! verify_directory_path "$parent_dir"; then
        echo "Could not safely create ONNX Runtime download parent: $parent_dir" >&2
        return 1
    fi
    temporary_dir=$(mktemp -d "$parent_dir/.onnxruntime-download.XXXXXX") || return 1
    if ! verify_directory_path "$temporary_dir"; then
        echo "ONNX Runtime download directory has an unsafe path: $temporary_dir" >&2
        return 1
    fi
    temporary_directory_identity=$(path_identity "$temporary_dir") || return 1
    temporary_directory_path=$(CDPATH='' cd -P -- "$temporary_dir" && pwd -P) || return 1
    if [[ "$(path_identity "$temporary_dir")" != "$temporary_directory_identity" ]]; then
        echo "ONNX Runtime download directory changed after creation: $temporary_dir" >&2
        return 1
    fi
    archive_path="$temporary_directory_path/$ONNXRUNTIME_ARCHIVE_NAME"
    ONNXRUNTIME_CLEANUP_DIR="$temporary_dir"
    ONNXRUNTIME_CLEANUP_IDENTITY="$temporary_directory_identity"
    trap cleanup EXIT HUP INT TERM

    if ! CDPATH='' cd -P -- "$temporary_dir" ||
            [[ "$(pwd -P)" != "$temporary_directory_path" ]] ||
            [[ "$(path_identity .)" != "$temporary_directory_identity" ]]; then
        echo "Could not safely enter the ONNX Runtime download directory." >&2
        return 1
    fi
    python3 "$SECURE_DOWNLOAD_HELPER" - \
        --verify-sha256 "$ONNXRUNTIME_SHA256" \
        --install-cwd \
        --install-name "$ONNXRUNTIME_ARCHIVE_NAME" \
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
    if [[ ! -f "$archive_path" ]] ||
            path_contains_symlink_component "$archive_path" ||
            [[ "$(path_identity "$temporary_dir")" != "$temporary_directory_identity" ]]; then
        echo "ONNX Runtime download path changed during download: $archive_path" >&2
        return 1
    fi
    stage_archive "$archive_path" "$runtime_prefix"
    trap - EXIT HUP INT TERM
    cleanup
    ONNXRUNTIME_CLEANUP_DIR=
    ONNXRUNTIME_CLEANUP_IDENTITY=
)

run_self_tests() {
    local test_dir
    local archive_root
    local archive_path
    local runtime_prefix
    local targets_file
    local actual_sha256
    local fake_bin
    local real_mkdir
    local real_mktemp
    local real_tar
    local real_mv
    local race_prefix
    local race_target
    local race_target_marker
    local mkdir_race_prefix
    local mkdir_race_backup
    local mkdir_race_target
    local mktemp_race_prefix
    local mktemp_race_backup
    local mktemp_race_target
    local tar_race_prefix
    local tar_race_backup
    local tar_race_target
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
    ONNXRUNTIME_CLEANUP_IDENTITY=$(path_identity "$test_dir")
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
    real_mkdir="$(command -v mkdir)"
    real_mktemp="$(command -v mktemp)"
    real_tar="$(command -v tar)"
    real_mv="$(command -v mv)"
    cat >"$fake_bin/mkdir" <<'EOF'
#!/bin/bash

set -euo pipefail

"$REAL_MKDIR" "$@"
if [[ "${FAKE_MKDIR_RACE:-0}" -eq 1 ]]; then
    target="${!#}"
    if [[ "$target" == "${FAKE_MKDIR_PATH:?}" ]]; then
        "$REAL_MV" -- "$target" "${FAKE_MKDIR_BACKUP:?}"
        ln -s -- "${FAKE_MKDIR_TARGET:?}" "$target"
    fi
fi
EOF
    chmod +x -- "$fake_bin/mkdir"
    cat >"$fake_bin/mktemp" <<'EOF'
#!/bin/bash

set -euo pipefail

temporary_path=$("$REAL_MKTEMP" "$@")
printf '%s\n' "$temporary_path"
if [[ "${FAKE_MKTEMP_RACE:-0}" -eq 1 ]]; then
    "$REAL_MV" -- "$temporary_path" "${FAKE_MKTEMP_BACKUP:?}"
    ln -s -- "${FAKE_MKTEMP_TARGET:?}" "$temporary_path"
fi
EOF
    chmod +x -- "$fake_bin/mktemp"
    cat >"$fake_bin/tar" <<'EOF'
#!/bin/bash

set -euo pipefail

"$REAL_TAR" "$@"
if [[ "${FAKE_TAR_RACE:-0}" -eq 1 ]] && [[ "$*" == *'--extract'* ]]; then
    temporary_path=$(pwd -P)
    "$REAL_MV" -- "$temporary_path" "${FAKE_TAR_BACKUP:?}"
    ln -s -- "${FAKE_TAR_TARGET:?}" "$temporary_path"
fi
EOF
    chmod +x -- "$fake_bin/tar"
    cat >"$fake_bin/mv" <<'EOF'
#!/bin/bash

set -euo pipefail

if [[ "${FAKE_MV_RACE:-0}" -eq 1 ]]; then
    [[ "$1" == "-T" && "$2" == "--no-clobber" && "$3" == "--" ]]
    destination="$5"
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
            REAL_MKDIR="$real_mkdir" \
            REAL_MKTEMP="$real_mktemp" \
            REAL_TAR="$real_tar" \
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

    mkdir_race_prefix="$test_dir/runtime-mkdir-race/runtime"
    mkdir_race_backup="$test_dir/runtime-mkdir-race-original"
    mkdir_race_target="$test_dir/runtime-mkdir-race-target"
    mkdir -- "$mkdir_race_target"
    if PATH="$fake_bin:$PATH" \
            FAKE_MKDIR_RACE=1 \
            FAKE_MKDIR_PATH="$(dirname -- "$mkdir_race_prefix")" \
            FAKE_MKDIR_BACKUP="$mkdir_race_backup" \
            FAKE_MKDIR_TARGET="$mkdir_race_target" \
            REAL_MKDIR="$real_mkdir" \
            REAL_MKTEMP="$real_mktemp" \
            REAL_TAR="$real_tar" \
            REAL_MV="$real_mv" \
            stage_archive "$archive_path" "$mkdir_race_prefix"; then
        echo "Self-test failed: ONNX Runtime parent mkdir race was accepted" >&2
        return 1
    fi
    if [[ ! -L "$(dirname -- "$mkdir_race_prefix")" ]]; then
        echo "Self-test failed: ONNX Runtime parent mkdir race was not observed" >&2
        return 1
    fi
    if [[ -e "$mkdir_race_target/runtime" ]]; then
        echo "Self-test failed: ONNX Runtime parent mkdir race was followed" >&2
        return 1
    fi

    mktemp_race_prefix="$test_dir/runtime-mktemp-race"
    mktemp_race_backup="$test_dir/runtime-mktemp-race-original"
    mktemp_race_target="$test_dir/runtime-mktemp-race-target"
    mkdir -- "$mktemp_race_target"
    if PATH="$fake_bin:$PATH" \
            FAKE_MKTEMP_RACE=1 \
            FAKE_MKTEMP_BACKUP="$mktemp_race_backup" \
            FAKE_MKTEMP_TARGET="$mktemp_race_target" \
            REAL_MKDIR="$real_mkdir" \
            REAL_MKTEMP="$real_mktemp" \
            REAL_TAR="$real_tar" \
            REAL_MV="$real_mv" \
            stage_archive "$archive_path" "$mktemp_race_prefix"; then
        echo "Self-test failed: ONNX Runtime mktemp race was accepted" >&2
        return 1
    fi
    if [[ ! -d "$mktemp_race_backup" ]] ||
            [[ -z "$(find "$test_dir" -maxdepth 1 -type l \
                -name '.onnxruntime-prefix.*' -print -quit)" ]]; then
        echo "Self-test failed: ONNX Runtime mktemp race was not observed" >&2
        return 1
    fi
    if [[ -e "$mktemp_race_target/include" ]]; then
        echo "Self-test failed: ONNX Runtime mktemp race was followed" >&2
        return 1
    fi

    tar_race_prefix="$test_dir/runtime-tar-race"
    tar_race_backup="$test_dir/runtime-tar-race-original"
    tar_race_target="$test_dir/runtime-tar-race-target"
    mkdir -- "$tar_race_target"
    if PATH="$fake_bin:$PATH" \
            FAKE_TAR_RACE=1 \
            FAKE_TAR_BACKUP="$tar_race_backup" \
            FAKE_TAR_TARGET="$tar_race_target" \
            REAL_MKDIR="$real_mkdir" \
            REAL_MKTEMP="$real_mktemp" \
            REAL_TAR="$real_tar" \
            REAL_MV="$real_mv" \
            stage_archive "$archive_path" "$tar_race_prefix"; then
        echo "Self-test failed: ONNX Runtime extraction race was accepted" >&2
        return 1
    fi
    if [[ ! -d "$tar_race_backup" ]] ||
            [[ -e "$tar_race_target/include" ]] ||
            [[ -e "$tar_race_prefix" ]]; then
        echo "Self-test failed: ONNX Runtime extraction race was followed" >&2
        return 1
    fi

    trap - EXIT HUP INT TERM
    cleanup
    ONNXRUNTIME_CLEANUP_DIR=
    ONNXRUNTIME_CLEANUP_IDENTITY=
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
