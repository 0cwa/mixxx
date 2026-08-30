#!/bin/bash

set -euo pipefail

readonly ONNXRUNTIME_VERSION="1.26.0"
readonly ONNXRUNTIME_ARCHIVE_NAME="onnxruntime-linux-x64-${ONNXRUNTIME_VERSION}.tgz"
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

verify_prefix() {
    local runtime_prefix="$1"
    local required_file

    if [[ -z "$runtime_prefix" || ! -d "$runtime_prefix" ]]; then
        echo "ONNX Runtime prefix is missing: $runtime_prefix" >&2
        return 1
    fi

    for required_file in \
        "$runtime_prefix/include/onnxruntime_cxx_api.h" \
        "$runtime_prefix/lib/libonnxruntime.so" \
        "$runtime_prefix/lib/libonnxruntime_providers_shared.so" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeConfig.cmake" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"; do
        if [[ ! -f "$required_file" ]]; then
            echo "ONNX Runtime prefix is incomplete; missing: $required_file" >&2
            return 1
        fi
    done

    if grep -q '/lib64/' \
            "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"; then
        echo "ONNX Runtime CMake targets still reference lib64: $runtime_prefix" >&2
        return 1
    fi
}

stage_archive() {
    local archive_path="$1"
    local runtime_prefix="$2"
    local parent_dir
    local temporary_prefix
    local targets_file

    if [[ ! -f "$archive_path" ]]; then
        echo "Cannot stage missing ONNX Runtime archive: $archive_path" >&2
        return 1
    fi

    parent_dir=$(dirname -- "$runtime_prefix")
    mkdir -p -- "$parent_dir"
    if [[ -e "$runtime_prefix" ]]; then
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

    targets_file="$temporary_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"
    if [[ ! -f "$targets_file" ]]; then
        rm -rf -- "$temporary_prefix"
        echo "ONNX Runtime archive has no release CMake targets: $archive_path" >&2
        return 1
    fi
    # The upstream archive's generated target file contains its build-time
    # lib64 path even though the shared libraries are delivered in lib/.
    sed -i 's#/lib64/#/lib/#g' "$targets_file"

    if ! verify_prefix "$temporary_prefix"; then
        rm -rf -- "$temporary_prefix"
        return 1
    fi

    mv -- "$temporary_prefix" "$runtime_prefix"
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
    if [[ -e "$runtime_prefix" ]]; then
        verify_prefix "$runtime_prefix"
        echo "Using existing verified ONNX Runtime prefix: $runtime_prefix"
        return 0
    fi

    parent_dir=$(dirname -- "$runtime_prefix")
    mkdir -p -- "$parent_dir"
    temporary_dir=$(mktemp -d "$parent_dir/.onnxruntime-download.XXXXXX")
    archive_path="$temporary_dir/$ONNXRUNTIME_ARCHIVE_NAME"
    ONNXRUNTIME_CLEANUP_DIR="$temporary_dir"
    trap cleanup EXIT HUP INT TERM

    curl \
        --fail \
        --location \
        --proto '=https' \
        --retry 3 \
        --retry-all-errors \
        --silent \
        --show-error \
        --tlsv1.2 \
        --output "$archive_path" \
        "$ONNXRUNTIME_URL"
    verify_sha256 "$ONNXRUNTIME_SHA256" "$archive_path"
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

    test_dir=$(mktemp -d "${TMPDIR:-/tmp}/mixxx-onnxruntime-test.XXXXXX")
    ONNXRUNTIME_CLEANUP_DIR="$test_dir"
    trap cleanup EXIT HUP INT TERM

    archive_root="$test_dir/onnxruntime-linux-x64-$ONNXRUNTIME_VERSION"
    mkdir -p \
        "$archive_root/include" \
        "$archive_root/lib/cmake/onnxruntime"
    printf 'test header\n' > "$archive_root/include/onnxruntime_cxx_api.h"
    : > "$archive_root/lib/libonnxruntime.so"
    : > "$archive_root/lib/libonnxruntime_providers_shared.so"
    printf 'test config\n' > "$archive_root/lib/cmake/onnxruntime/onnxruntimeConfig.cmake"
    printf 'IMPORTED_LOCATION "/build/lib64/libonnxruntime.so"\n' \
        > "$archive_root/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"

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

    runtime_prefix="$test_dir/runtime"
    stage_archive "$archive_path" "$runtime_prefix"
    verify_prefix "$runtime_prefix"
    targets_file="$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"
    if ! grep -q '/lib/libonnxruntime.so' "$targets_file"; then
        echo "Self-test failed: the lib64 CMake target path was not normalized" >&2
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
