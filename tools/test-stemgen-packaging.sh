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
    local inside_destination
    local inside_symlink_destination
    local external_destination
    local checkout_symlink_destination
    local pointer_path
    local failure_destination
    local output
    local status
    local inside_pointer_before

    test_download_root=$(mktemp -d "${TMPDIR:-/tmp}/mixxx-stemgen-model-test.XXXXXX")
    cleanup_download_test() {
        rm -rf -- "$test_download_root"
    }
    trap cleanup_download_test EXIT

    checkout_root="$test_download_root/checkout"
    script_dir="$checkout_root/.github/scripts"
    fake_bin="$test_download_root/bin"
    curl_log="$test_download_root/curl.log"
    mkdir -p -- "$script_dir" "$checkout_root/models" "$fake_bin"
    cp -- "$download_script" "$script_dir/download-stemgen-model.sh"
    chmod +x -- "$script_dir/download-stemgen-model.sh"

    cat >"$script_dir/verify-stemgen-model.sh" <<'EOF'
#!/bin/sh

set -eu

model_path="${MIXXX_STEM_MODEL_DIR:?}/htdemucs.onnx"
if [ ! -f "$model_path" ]; then
    exit 1
fi
grep -Fqx 'materialized-model' "$model_path"
EOF
    chmod +x -- "$script_dir/verify-stemgen-model.sh"

    cat >"$fake_bin/curl" <<'EOF'
#!/bin/sh

set -eu

output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output)
            output=$2
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

: "${output:?curl output path was not provided}"
printf '%s\n' "$output" >>"${FAKE_CURL_LOG:?}"
if [ "${FAKE_CURL_FAIL:-0}" -eq 1 ]; then
    printf '%s\n' 'partial-model' >"$output"
    exit 17
fi
printf '%s\n' 'materialized-model' >"$output"
EOF
    chmod +x -- "$fake_bin/curl"

    inside_destination="$checkout_root/models"
    printf '%s\n' 'checkout-pointer' >"$inside_destination/htdemucs.onnx"
    inside_pointer_before=$(sha256sum "$inside_destination/htdemucs.onnx")
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

    external_destination="$test_download_root/external-models"
    : >"$curl_log"
    MIXXX_STEM_MODEL_DIR="$external_destination" \
        PATH="$fake_bin:$PATH" \
        FAKE_CURL_LOG="$curl_log" \
        "$script_dir/download-stemgen-model.sh" >/dev/null
    test -f "$external_destination/htdemucs.onnx" || \
        fail 'valid external destination did not receive a model'
    grep -Fq "$external_destination/.stemgen-model." "$curl_log" || \
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

download_script="$repository_root/.github/scripts/download-stemgen-model.sh"
model_pointer="$repository_root/models/htdemucs.onnx"
test_download_staging_safety
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
