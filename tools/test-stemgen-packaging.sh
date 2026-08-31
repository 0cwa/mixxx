#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repository_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
readonly repository_root

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

test_download_staging_safety() {
    local checkout_root
    local script_dir
    local fake_bin
    local curl_log
    local missing_destination
    local double_slash_destination
    local inside_destination
    local inside_symlink_destination
    local final_symlink_destination
    local non_regular_destination
    local external_destination
    local checkout_symlink_destination
    local pointer_path
    local failure_destination
    local output
    local status
    local inside_pointer_before
    local race_destination
    local race_backup_destination
    local real_mktemp
    local final_race_destination
    local temporary_race_destination
    local temporary_race_backup_destination
    local reuse_race_destination
    local reuse_race_backup_destination
    local reuse_race_target
    local verify_symlink_destination
    local verify_symlink_target
    local fake_model_size
    local fake_model_sha256

    test_download_root=$(mktemp -d "${TMPDIR:-/tmp}/mixxx-stemgen-model-test.XXXXXX")
    cleanup_download_test() {
        rm -rf -- "$test_download_root"
    }
    trap cleanup_download_test EXIT

    checkout_root="$test_download_root/checkout"
    script_dir="$checkout_root/.github/scripts"
    fake_bin="$test_download_root/bin"
    curl_log="$test_download_root/curl.log"
    mkdir -p -- "$script_dir" "$checkout_root/models" "$checkout_root/tools" "$fake_bin"
    cp -- "$download_script" "$script_dir/download-stemgen-model.sh"
    cp -- "$repository_root/tools/secure-download.py" \
        "$checkout_root/tools/secure-download.py"
    fake_model_size=$(printf '%s\n' 'materialized-model' | wc -c)
    fake_model_sha256=$(printf '%s\n' 'materialized-model' | sha256sum)
    fake_model_sha256=${fake_model_sha256%% *}
    cat >"$checkout_root/tools/secure-download.py" <<EOF
#!/usr/bin/env python3

import os
import sys

arguments = sys.argv[1:]
separator_index = arguments.index("--")
command = arguments[separator_index + 1 :]
os.execv(
    sys.executable,
    [
        sys.executable,
        "$repository_root/tools/secure-download.py",
        arguments[0],
        "--verify-size",
        "$fake_model_size",
        "--verify-sha256",
        "$fake_model_sha256",
        "--install-fd",
        "9",
        "--install-name",
        "htdemucs.onnx",
        "--",
        *command,
    ],
)
EOF
    chmod +x -- "$script_dir/download-stemgen-model.sh"

    cat >"$script_dir/verify-stemgen-model.sh" <<'EOF'
#!/bin/sh

set -eu

model_path="${MIXXX_STEM_MODEL_DIR:?}/htdemucs.onnx"
if [ "${FAKE_VERIFY_REPLACE_MODEL:-0}" -eq 1 ]; then
    mv -- "$model_path" "${FAKE_VERIFY_MODEL_BACKUP:?}"
    ln -s -- "${FAKE_VERIFY_MODEL_TARGET:?}" "$model_path"
fi
if [ ! -f "$model_path" ]; then
    exit 1
fi
grep -Fqx 'materialized-model' "$model_path"
EOF
    chmod +x -- "$script_dir/verify-stemgen-model.sh"

    cat >"$fake_bin/curl" <<'EOF'
#!/bin/sh

set -eu

printf '%s\n' "$(pwd -P)" >>"${FAKE_CURL_LOG:?}"
if [ "${FAKE_CURL_REPLACE_TEMP:-0}" -eq 1 ]; then
    current_temp_directory=$(pwd -P)
    mv -- "$current_temp_directory" "${FAKE_CURL_TEMP_BACKUP:?}"
    ln -s -- "${FAKE_CURL_TEMP_TARGET:?}" "$current_temp_directory"
fi
if [ "${FAKE_CURL_FAIL:-0}" -eq 1 ]; then
    printf '%s\n' 'partial-model'
    exit 17
fi
if [ "${FAKE_CURL_REPLACE_FINAL:-0}" -eq 1 ]; then
    ln -s -- "${FAKE_CURL_FINAL_TARGET:?}" "${FAKE_CURL_FINAL_PATH:?}"
fi
printf '%s\n' 'materialized-model'
EOF
    chmod +x -- "$fake_bin/curl"

    inside_destination="$checkout_root/models"
    printf '%s\n' 'checkout-pointer' >"$inside_destination/htdemucs.onnx"
    inside_pointer_before=$(sha256sum "$inside_destination/htdemucs.onnx")

    missing_destination="$checkout_root/new/nonexistent/staging"
    output="$test_download_root/missing.out"
    status=0
    if MIXXX_STEM_MODEL_DIR="$missing_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 2 || fail 'nonexistent checkout destination was not rejected'
    test ! -e "$checkout_root/new" || \
        fail 'rejected checkout destination created directories'
    test ! -e "$missing_destination" || \
        fail 'rejected checkout destination was created'
    test ! -s "$curl_log" || \
        fail 'curl ran for a rejected nonexistent checkout destination'
    grep -Fq 'outside the source checkout' "$output" || \
        fail 'nonexistent checkout rejection did not identify the containment contract'

    double_slash_destination="//${missing_destination#/}"
    output="$test_download_root/double-slash.out"
    status=0
    if MIXXX_STEM_MODEL_DIR="$double_slash_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 2 || fail 'repeated-slash checkout destination was not rejected'
    test ! -e "$checkout_root/new" || \
        fail 'repeated-slash checkout destination created directories'
    test ! -e "$double_slash_destination" || \
        fail 'repeated-slash checkout destination was created'
    test "$inside_pointer_before" = \
        "$(sha256sum "$inside_destination/htdemucs.onnx")" || \
        fail 'repeated-slash checkout destination mutated checkout bytes'
    test ! -s "$curl_log" || \
        fail 'curl ran for a rejected repeated-slash checkout destination'
    grep -Fq 'outside the source checkout' "$output" || \
        fail 'repeated-slash rejection did not identify the containment contract'

    output="$test_download_root/inside.out"
    status=0
    if MIXXX_STEM_MODEL_DIR="$inside_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 2 || fail 'checkout destination was not rejected'
    test "$inside_pointer_before" = \
        "$(sha256sum "$inside_destination/htdemucs.onnx")" || \
        fail 'checkout model was replaced'
    test ! -s "$curl_log" || fail 'curl ran for a rejected checkout destination'
    grep -Fq 'outside the source checkout' "$output" || \
        fail 'checkout rejection did not identify the containment contract'

    inside_symlink_destination="$test_download_root/checkout-models-link"
    ln -s -- "$checkout_root/models" "$inside_symlink_destination"
    status=0
    if MIXXX_STEM_MODEL_DIR="$inside_symlink_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >/dev/null 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 2 || fail 'symlinked checkout destination was not rejected'
    test "$inside_pointer_before" = \
        "$(sha256sum "$inside_destination/htdemucs.onnx")" || \
        fail 'symlinked checkout model was replaced'
    test ! -s "$curl_log" || fail 'curl ran for a rejected symlinked destination'

    final_symlink_destination="$test_download_root/final-symlink-models"
    mkdir -p -- "$final_symlink_destination"
    ln -s -- "$checkout_root/models" "$final_symlink_destination/htdemucs.onnx"
    : >"$curl_log"
    output="$test_download_root/final-symlink.out"
    status=0
    if MIXXX_STEM_MODEL_DIR="$final_symlink_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 2 || fail 'final model symlink was not rejected'
    test "$inside_pointer_before" = \
        "$(sha256sum "$inside_destination/htdemucs.onnx")" || \
        fail 'final model symlink replaced the checkout model'
    test -L "$final_symlink_destination/htdemucs.onnx" || \
        fail 'final model symlink was removed'
    test ! -s "$curl_log" || fail 'curl ran for a rejected final symlink'
    grep -Fq 'must not be a symlink' "$output" || \
        fail 'final symlink rejection did not identify the destination type'

    non_regular_destination="$test_download_root/non-regular-models"
    mkdir -p -- "$non_regular_destination/htdemucs.onnx"
    : >"$curl_log"
    output="$test_download_root/non-regular.out"
    status=0
    if MIXXX_STEM_MODEL_DIR="$non_regular_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 2 || fail 'non-regular final model destination was not rejected'
    test -d "$non_regular_destination/htdemucs.onnx" || \
        fail 'non-regular final model destination was replaced'
    test ! -s "$curl_log" || fail 'curl ran for a non-regular final destination'
    grep -Fq 'must be a regular file or absent' "$output" || \
        fail 'non-regular destination rejection did not identify the destination type'

    external_destination="$test_download_root/external-models"
    : >"$curl_log"
    MIXXX_STEM_MODEL_DIR="$external_destination" \
        PATH="$fake_bin:$PATH" \
        FAKE_CURL_LOG="$curl_log" \
        "$script_dir/download-stemgen-model.sh" >/dev/null
    test -f "$external_destination/htdemucs.onnx" || \
        fail 'valid external destination did not receive a model'
    grep -Fq '.stemgen-model.' "$curl_log" || \
        fail 'valid external destination did not use a temporary download path'
    test -z "$(find "$external_destination" -mindepth 1 -maxdepth 1 \
        -name '.stemgen-model.*' -print -quit)" || \
        fail 'temporary download directory was not cleaned after success'

    checkout_symlink_destination="$checkout_root/external-models-link"
    ln -s -- "$external_destination" "$checkout_symlink_destination"
    : >"$curl_log"
    MIXXX_STEM_MODEL_DIR="$checkout_symlink_destination" \
        PATH="$fake_bin:$PATH" \
        FAKE_CURL_LOG="$curl_log" \
        "$script_dir/download-stemgen-model.sh" >/dev/null
    test -f "$external_destination/htdemucs.onnx" || \
        fail 'external symlink destination was not allowed'
    test ! -s "$curl_log" || \
        fail 'verified model was not reused through an external symlink'

    pointer_path="$external_destination/htdemucs.onnx"
    printf '%s\n' \
        'version https://git-lfs.github.com/spec/v1' \
        'oid sha256:0123456789abcdef' \
        'size 123' >"$pointer_path"
    : >"$curl_log"
    MIXXX_STEM_MODEL_DIR="$external_destination" \
        PATH="$fake_bin:$PATH" \
        FAKE_CURL_LOG="$curl_log" \
        "$script_dir/download-stemgen-model.sh" >/dev/null
    grep -Fqx 'materialized-model' "$pointer_path" || \
        fail 'existing LFS pointer was not replaced with downloaded content'
    test -s "$curl_log" || fail 'existing LFS pointer was incorrectly reused'

    reuse_race_destination="$test_download_root/reuse-race-models"
    reuse_race_backup_destination="$test_download_root/reuse-race-model"
    reuse_race_target="$test_download_root/reuse-race-target"
    mkdir -p -- "$reuse_race_destination"
    printf '%s\n' 'materialized-model' >"$reuse_race_destination/htdemucs.onnx"
    printf '%s\n' 'materialized-model' >"$reuse_race_target"
    : >"$curl_log"
    status=0
    if FAKE_VERIFY_REPLACE_MODEL=1 \
            FAKE_VERIFY_MODEL_BACKUP="$reuse_race_backup_destination" \
            FAKE_VERIFY_MODEL_TARGET="$reuse_race_target" \
            MIXXX_STEM_MODEL_DIR="$reuse_race_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >/dev/null 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 2 || \
        fail 'verified model reuse race was accepted'
    test -L "$reuse_race_destination/htdemucs.onnx" || \
        fail 'verified model reuse race did not inject its symlink'
    test -f "$reuse_race_backup_destination" || \
        fail 'verified model reuse race lost the original model'
    test ! -s "$curl_log" || \
        fail 'verified model reuse race started a download'

    verify_symlink_destination="$test_download_root/verify-symlink-models"
    verify_symlink_target="$test_download_root/verify-symlink-target"
    mkdir -p -- "$verify_symlink_destination"
    printf '%s\n' 'materialized-model' >"$verify_symlink_target"
    ln -s -- "$verify_symlink_target" \
        "$verify_symlink_destination/htdemucs.onnx"
    if MIXXX_STEM_MODEL_DIR="$verify_symlink_destination" \
            "$repository_root/.github/scripts/verify-stemgen-model.sh" \
            >/dev/null 2>&1; then
        fail 'model verifier accepted a symlink'
    fi

    # Replace the validated pathname after the script has entered the staging
    # directory. All temporary and final writes must stay in the directory
    # that was entered, even though its pathname now points at the checkout.
    race_destination="$test_download_root/race-models"
    race_backup_destination="$test_download_root/race-models-original"
    mkdir -p -- "$race_destination"
    real_mktemp=$(command -v mktemp)
    cat >"$fake_bin/mktemp" <<'EOF'
#!/bin/sh

set -eu

mv -- "$RACE_DESTINATION" "$RACE_BACKUP_DESTINATION"
ln -s -- "$RACE_CHECKOUT_DESTINATION" "$RACE_DESTINATION"
exec "$REAL_MKTEMP" "$@"
EOF
    chmod +x -- "$fake_bin/mktemp"
    : >"$curl_log"
    if ! RACE_DESTINATION="$race_destination" \
            RACE_BACKUP_DESTINATION="$race_backup_destination" \
            RACE_CHECKOUT_DESTINATION="$checkout_root/models" \
            REAL_MKTEMP="$real_mktemp" \
            MIXXX_STEM_MODEL_DIR="$race_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >/dev/null 2>&1; then
        fail 'staging directory replacement race was not handled safely'
    fi
    test -L "$race_destination" || \
        fail 'race harness did not replace the staging directory with a symlink'
    test "$inside_pointer_before" = \
        "$(sha256sum "$inside_destination/htdemucs.onnx")" || \
        fail 'staging directory replacement race modified the checkout model'
    grep -Fqx 'materialized-model' \
        "$race_backup_destination/htdemucs.onnx" || \
        fail 'race-safe write did not remain in the original staging directory'
    rm -f -- "$fake_bin/mktemp"

    temporary_race_destination="$test_download_root/temporary-race-models"
    temporary_race_backup_destination="$test_download_root/temporary-race-models-original"
    mkdir -p -- "$temporary_race_destination"
    : >"$curl_log"
    if ! FAKE_CURL_REPLACE_TEMP=1 \
            FAKE_CURL_TEMP_BACKUP="$temporary_race_backup_destination" \
            FAKE_CURL_TEMP_TARGET="$checkout_root/models" \
            MIXXX_STEM_MODEL_DIR="$temporary_race_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >/dev/null 2>&1; then
        fail 'temporary download pathname race was not handled safely'
    fi
    test "$inside_pointer_before" = \
        "$(sha256sum "$inside_destination/htdemucs.onnx")" || \
        fail 'temporary download pathname race modified the checkout model'
    grep -Fqx 'materialized-model' \
        "$temporary_race_destination/htdemucs.onnx" || \
        fail 'temporary download pathname race lost the downloaded model'

    final_race_destination="$test_download_root/final-race-models"
    mkdir -p -- "$final_race_destination"
    : >"$curl_log"
    status=0
    if FAKE_CURL_REPLACE_FINAL=1 \
            FAKE_CURL_FINAL_PATH="$final_race_destination/htdemucs.onnx" \
            FAKE_CURL_FINAL_TARGET="$checkout_root/models" \
            MIXXX_STEM_MODEL_DIR="$final_race_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >/dev/null 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -ne 0 || \
        fail 'final destination symlink race was accepted'
    test -L "$final_race_destination/htdemucs.onnx" || \
        fail 'final destination symlink race was removed instead of rejected'
    test "$inside_pointer_before" = \
        "$(sha256sum "$inside_destination/htdemucs.onnx")" || \
        fail 'final destination directory race modified the checkout model'
    test -z "$(find "$checkout_root/models" -mindepth 1 -maxdepth 1 \
        ! -name 'htdemucs.onnx' -print -quit)" || \
        fail 'final destination directory race left a checkout artifact'

    failure_destination="$test_download_root/failed-models"
    : >"$curl_log"
    status=0
    if FAKE_CURL_FAIL=1 \
            MIXXX_STEM_MODEL_DIR="$failure_destination" \
            PATH="$fake_bin:$PATH" \
            FAKE_CURL_LOG="$curl_log" \
            "$script_dir/download-stemgen-model.sh" >/dev/null 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 17 || fail 'download failure did not propagate its status'
    test ! -e "$failure_destination/htdemucs.onnx" || \
        fail 'failed download left a final model behind'
    test -z "$(find "$failure_destination" -mindepth 1 -maxdepth 1 \
        -name '.stemgen-model.*' -print -quit)" || \
        fail 'temporary download directory was not cleaned after failure'
}

test_cmake_install_staging_safety() {
    local test_root
    local project_dir
    local build_dir
    local staged_parent
    local staged_model
    local staged_parent_backup
    local staged_symlink_target
    local staged_parent_target
    local target_before
    local model_size
    local model_sha256
    local install_root
    local installed_model
    local failed_install_root
    local destination_symlink_root
    local destination_symlink_target
    local output
    local status

    test_root=$(mktemp -d "${TMPDIR:-/tmp}/mixxx-stemgen-cmake-test.XXXXXX")
    project_dir="$test_root/project"
    build_dir="$test_root/build"
    staged_parent="$test_root/staged-models"
    staged_model="$staged_parent/htdemucs.onnx"
    staged_parent_backup="$test_root/staged-models-original"
    staged_symlink_target="$test_root/staged-model-target"
    staged_parent_target="$test_root/staged-model-parent-target"
    install_root="$test_root/install"
    failed_install_root="$test_root/failed-install"
    destination_symlink_root="$test_root/destination-symlink-install"
    destination_symlink_target="$test_root/destination-symlink-target"
    output="$test_root/output"
    mkdir -p -- "$project_dir" "$staged_parent"
    printf '%s\n' 'trusted-model' >"$staged_model"
    model_size=$(stat -c '%s' -- "$staged_model")
    model_sha256=$(sha256sum "$staged_model")
    model_sha256=${model_sha256%% *}

    # Configure the actual install-script template used by CMakeLists.txt so
    # this test covers its generated verification and install semantics.
    cat >"$project_dir/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.20)
project(stemgen_install_fixture NONE)
set(MIXXX_STEM_MODEL_STAGED_FILE [==[${staged_model}]==])
set(MIXXX_STEM_MODEL_NAME [==[htdemucs.onnx]==])
set(MIXXX_STEM_MODEL_SIZE ${model_size})
set(MIXXX_STEM_MODEL_SHA256 [==[${model_sha256}]==])
set(MIXXX_INSTALL_DATADIR [==[share/mixxx]==])
configure_file(
    [==[${repository_root}/cmake/InstallStemgenModel.cmake.in]==]
    "\${CMAKE_CURRENT_BINARY_DIR}/InstallStemgenModel.cmake"
    @ONLY
)
install(SCRIPT "\${CMAKE_CURRENT_BINARY_DIR}/InstallStemgenModel.cmake")
EOF
    status=0
    if cmake -S "$project_dir" -B "$build_dir" >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 0 || fail 'CMake install fixture did not configure'

    status=0
    if cmake --install "$build_dir" --prefix "$install_root" >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -eq 0 || fail 'generated CMake install script rejected a valid model'
    installed_model="$install_root/share/mixxx/models/htdemucs.onnx"
    grep -Fqx 'trusted-model' "$installed_model" || \
        fail 'generated CMake install script did not install the verified model'

    # Replace the staged artifact after configure. The generated install
    # script must reject the symlink before file(INSTALL) can follow it.
    printf '%s\n' 'trusted-model' >"$staged_symlink_target"
    target_before=$(sha256sum "$staged_symlink_target")
    rm -- "$staged_model"
    ln -s -- "$staged_symlink_target" "$staged_model"
    status=0
    if cmake --install "$build_dir" --prefix "$failed_install_root" \
            >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -ne 0 || fail 'generated CMake install script accepted a staged symlink'
    test -L "$staged_model" || fail 'CMake symlink fixture lost its staged symlink'
    test "$target_before" = "$(sha256sum "$staged_symlink_target")" || \
        fail 'generated CMake install script modified the symlink target'
    test ! -e "$failed_install_root" || \
        fail 'generated CMake install script created an artifact for a symlink'
    grep -Fq 'symlink component' "$output" || \
        fail 'generated CMake install script did not identify the staged symlink'

    mkdir -p -- "$destination_symlink_root/share"
    mkdir -- "$destination_symlink_target"
    ln -s -- "$destination_symlink_target" \
        "$destination_symlink_root/share/mixxx"
    status=0
    if cmake --install "$build_dir" --prefix "$destination_symlink_root" \
            >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -ne 0 || \
        fail 'generated CMake install script followed an install parent symlink'
    test ! -e "$destination_symlink_target/htdemucs.onnx" || \
        fail 'generated CMake install script installed through a parent symlink'
    grep -Fq 'symlink component' "$output" || \
        fail 'generated CMake install script did not identify an install parent symlink'

    # Replace the staged directory itself after configure. Its parent-chain
    # check must prevent file(INSTALL) from following a redirected directory.
    rm -- "$staged_model"
    mv -- "$staged_parent" "$staged_parent_backup"
    mkdir -- "$staged_parent_target"
    printf '%s\n' 'trusted-model' >"$staged_parent_target/htdemucs.onnx"
    ln -s -- "$staged_parent_target" "$staged_parent"
    status=0
    if cmake --install "$build_dir" --prefix "$failed_install_root" \
            >"$output" 2>&1; then
        status=0
    else
        status=$?
    fi
    test "$status" -ne 0 || \
        fail 'generated CMake install script followed a staged parent symlink'
    test -L "$staged_parent" || fail 'CMake parent symlink fixture was lost'
    test ! -e "$failed_install_root" || \
        fail 'generated CMake install script created an artifact through a parent symlink'
    grep -Fq 'symlink component' "$output" || \
        fail 'generated CMake install script did not identify a parent symlink'

    rm -rf -- "$test_root"
}

download_script="$repository_root/.github/scripts/download-stemgen-model.sh"
model_pointer="$repository_root/models/htdemucs.onnx"

if ! "$repository_root/tools/onnxruntime_buildenv.sh" test; then
    fail 'ONNX Runtime staging self-test failed'
fi
if ! "$repository_root/tools/debian_buildenv.sh" test; then
    fail 'Debian model staging self-test failed'
fi

test_download_staging_safety
test_cmake_install_staging_safety
pointer_before=$(sha256sum "$model_pointer")
if env -u MIXXX_STEM_MODEL_DIR "$download_script" >/dev/null 2>&1; then
    fail 'unparameterized model download unexpectedly succeeded'
fi
test "$pointer_before" = "$(sha256sum "$model_pointer")" || \
    fail 'unparameterized model download changed the tracked LFS pointer'

python3 - \
    "$repository_root/models/htdemucs.onnx.manifest.json" \
    "$repository_root/models/NOTICE.md" \
    "$repository_root/models/README.md" \
    "$repository_root/packaging/flatpak/modules/onnxruntime.yaml" \
    "$repository_root/packaging/flatpak/modules/stemgen-model.yaml" \
    "$repository_root/src/stems/stemconverter.h" \
    "$repository_root/.github/scripts/download-stemgen-model.sh" \
    "$repository_root/.github/scripts/verify-stemgen-model.sh" \
    "$repository_root/tools/debian_buildenv.sh" \
    "$repository_root/tools/onnxruntime_buildenv.sh" \
    "$repository_root/tools/secure-download.py" \
    "$repository_root/CMakeLists.txt" \
    "$repository_root/cmake/InstallStemgenModel.cmake.in" \
    "$repository_root/.github/workflows/build.yml" <<'PY'
import json
import sys
from pathlib import Path

(
    manifest_path,
    notice_path,
    readme_path,
    onnxruntime_module_path,
    stemgen_model_module_path,
    stemconverter_path,
    download_script_path,
    verifier_path,
    debian_buildenv_path,
    onnxruntime_buildenv_path,
    secure_download_path,
    cmake_lists_path,
    cmake_install_script_path,
    workflow_path,
) = map(Path, sys.argv[1:])

manifest = json.loads(manifest_path.read_text())
model_name = manifest["filename"]
model_url = manifest["source"]["asset_url"]
model_size = str(manifest["size_bytes"])
model_sha256 = manifest["sha256"]

assert model_name == "htdemucs.onnx"
assert model_size == "304413278"
assert model_sha256 == (
    "db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d"
)

notice = " ".join(notice_path.read_text().split())
assert "does not claim that the weights are MIT licensed" in notice

readme = " ".join(readme_path.read_text().split())
assert "requires `MIXXX_STEM_MODEL_DIR`" in readme
assert "outside the source checkout" in readme

onnxruntime_module = onnxruntime_module_path.read_text()
assert 'cp -a include/. "${FLATPAK_DEST}/include/"' in onnxruntime_module
assert 'cp -a include/. "${FLATPAK_DEST}/include/onnxruntime/"' not in onnxruntime_module

stemconverter = stemconverter_path.read_text()
assert "#include <onnxruntime_cxx_api.h>" in stemconverter

for path in (
    download_script_path,
    verifier_path,
    debian_buildenv_path,
    stemgen_model_module_path,
):
    contents = path.read_text()
    if path in (download_script_path, debian_buildenv_path, stemgen_model_module_path):
        assert model_url in contents, path
    if path in (verifier_path, debian_buildenv_path, stemgen_model_module_path):
        assert model_sha256 in contents, path

download_script = download_script_path.read_text()
assert "model_dir=${MIXXX_STEM_MODEL_DIR:-}" in download_script
assert "$script_dir/../../models" not in download_script
assert "--verify-size" in download_script
assert "--verify-sha256" in download_script
assert "--install-fd" in download_script

secure_download = secure_download_path.read_text()
assert "os.O_NOFOLLOW" in secure_download
assert "os.fstat" in secure_download
assert "os.fsync" in secure_download
assert "--install-fd" in secure_download

onnxruntime_buildenv = onnxruntime_buildenv_path.read_text()
assert 'python3 "$SECURE_DOWNLOAD_HELPER" "$archive_path"' in onnxruntime_buildenv
assert '--output "$archive_path"' not in onnxruntime_buildenv

cmake_lists = cmake_lists_path.read_text()
assert 'IS_SYMLINK "${MIXXX_STEM_MODEL_FILE}"' in cmake_lists
assert 'COPY_FILE' in cmake_lists
assert 'MIXXX_STEM_MODEL_STAGED_FILE' in cmake_lists
assert 'InstallStemgenModel.cmake.in' in cmake_lists
assert 'install(SCRIPT "${MIXXX_STEM_MODEL_INSTALL_SCRIPT}")' in cmake_lists

cmake_install_script = cmake_install_script_path.read_text()
assert 'if(IS_SYMLINK "${_mixxx_stem_model_path_to_check}")' in cmake_install_script
assert 'file(SHA256 "${_mixxx_stem_model_path}"' in cmake_install_script
assert 'file(\n  INSTALL "${_mixxx_stem_model_path}"' in cmake_install_script
assert 'file(\n  SHA256\n  "${_mixxx_stem_model_destination_path}"' in cmake_install_script

workflow = workflow_path.read_text()
# runner.* is unavailable in a job-level env mapping. Keep runner.temp in
# step-level env mappings so the workflow can be expanded before a runner is
# allocated.
job_env_runner_context = [
    line
    for line in workflow.splitlines()
    if line.startswith("      ")
    and not line.startswith("        ")
    and "${{ runner." in line
]
assert not job_env_runner_context, job_env_runner_context

flatpak_start = workflow.index("  build-flatpak:")
smoke_start = workflow.index("  build-stemgen-ubuntu:")
regular_build = workflow[workflow.index("  build:"):flatpak_start]
assert "stem_conversion: true" not in regular_build
assert "stem_conversion: false" in regular_build
flatpak = workflow[flatpak_start:smoke_start]
staging_path = "MIXXX_STEM_MODEL_DIR: ${{ runner.temp }}/mixxx-stemgen-model"
assert staging_path in flatpak
download_position = flatpak.index("run: .github/scripts/download-stemgen-model.sh")
clean_position = flatpak.index("Confirm clean checkout after model staging")
version_position = flatpak.index('GIT_DESC=$(git describe --always --first-parent')
assert download_position < clean_position < version_position

stemgen_start = workflow.index("  build-stemgen-ubuntu:")
manifest_start = workflow.index("  update_manifest:")
stemgen = workflow[stemgen_start:manifest_start]
assert "MIXXX_ONNX_RUNTIME_PREFIX: ${{ runner.temp }}/mixxx-onnxruntime" in stemgen
assert staging_path in stemgen
assert stemgen.count("MIXXX_ONNX_RUNTIME_PREFIX: ${{ runner.temp }}/mixxx-onnxruntime") >= 4
assert stemgen.count(staging_path) >= 4
assert "MIXXX_INSTALL_STEM_CONVERSION: true" in stemgen
assert "-DSTEM_CONVERSION=ON" in stemgen
tag_fetch_position = stemgen.index("run: git fetch origin --force --tags")
confirm_position = stemgen.index('git describe --always --first-parent --dirty=-modified')
assert tag_fetch_position < confirm_position

print(
    "Stemgen packaging contracts passed: "
    "Flatpak headers, model manifest, rights notice, and staging order"
)
PY
