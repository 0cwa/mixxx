#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly SCRIPT_DIR
readonly SECURE_DOWNLOAD_HELPER="$SCRIPT_DIR/secure-download.py"
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
        "$runtime_prefix/lib/libonnxruntime.so" \
        "$runtime_prefix/lib/libonnxruntime_providers_shared.so" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeConfig.cmake" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets.cmake" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"; do
        if [[ ! -f "$required_file" ]] ||
                path_contains_symlink_component "$required_file"; then
            echo "ONNX Runtime prefix is incomplete; missing: $required_file" >&2
            return 1
        fi
    done

    for targets_file in \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets.cmake" \
        "$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets-release.cmake"; do
        if grep -Eq '/include/onnxruntime|/lib64/' "$targets_file"; then
            echo "ONNX Runtime CMake targets contain unnormalized paths: $runtime_prefix" >&2
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
    # The generated fixture intentionally contains a literal CMake variable.
    # shellcheck disable=SC2016
    printf 'INTERFACE_INCLUDE_DIRECTORIES "${_IMPORT_PREFIX}/include/onnxruntime"\n' \
        > "$archive_root/lib/cmake/onnxruntime/onnxruntimeTargets.cmake"
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
    targets_file="$runtime_prefix/lib/cmake/onnxruntime/onnxruntimeTargets.cmake"
    if grep -q '/include/onnxruntime' "$targets_file" ||
            ! grep -q '/include"' "$targets_file"; then
        echo "Self-test failed: the ONNX Runtime include target path was not normalized" >&2
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
