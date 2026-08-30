#!/usr/bin/env bash

set -euo pipefail

script_dir=$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repository_root=$(CDPATH='' cd -- "$script_dir/.." && pwd)
readonly repository_root

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

download_script="$repository_root/.github/scripts/download-stemgen-model.sh"
model_pointer="$repository_root/models/htdemucs.onnx"
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
confirm_position = stemgen.index('git describe --dirty=-modified')
assert tag_fetch_position < confirm_position

print(
    "Stemgen packaging contracts passed: "
    "Flatpak headers, model manifest, rights notice, and staging order"
)
PY
