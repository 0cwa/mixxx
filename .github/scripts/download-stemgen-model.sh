#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd -P)
model_dir=${MIXXX_STEM_MODEL_DIR:-}
if [ -z "$model_dir" ]; then
    printf '%s\n' \
        'MIXXX_STEM_MODEL_DIR must be set to a staging directory outside the source checkout.' \
        >&2
    exit 2
fi
checkout_dir=$(CDPATH='' cd -- "$script_dir/../.." && pwd -P)
model_url=https://github.com/mixxxdj/demucs/releases/download/v4.0.1-19-gd182d42-onnxmodel/htdemucs.onnx

mkdir -p -- "$model_dir"
model_dir=$(CDPATH='' cd -- "$model_dir" && pwd -P)
case "$model_dir/" in
    "$checkout_dir/"*)
        printf '%s\n' \
            'MIXXX_STEM_MODEL_DIR must resolve to a staging directory outside the source checkout.' \
            >&2
        exit 2
        ;;
esac
model_path="$model_dir/htdemucs.onnx"

# Reuse an already materialized, verified model when one is present. This also
# makes local and CI runs idempotent without replacing a valid artifact.
if [ -f "$model_path" ] &&
        MIXXX_STEM_MODEL_DIR="$model_dir" "$script_dir/verify-stemgen-model.sh"; then
    exit 0
fi

temporary_dir=$(mktemp -d "$model_dir/.stemgen-model.XXXXXX")
temporary_path="$temporary_dir/htdemucs.onnx"
cleanup() {
    rm -rf -- "$temporary_dir"
}
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
    --output "$temporary_path" \
    "$model_url"

MIXXX_STEM_MODEL_DIR="$temporary_dir" "$script_dir/verify-stemgen-model.sh"
mv -- "$temporary_path" "$model_path"

printf 'Materialized verified Stemgen model at %s\n' "$model_path"
