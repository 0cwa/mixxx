#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
model_dir=${MIXXX_STEM_MODEL_DIR:-"$script_dir/../../models"}
model_path="$model_dir/htdemucs.onnx"
expected_size=304413278
expected_sha256=db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d
lfs_pointer_header='version https://git-lfs.github.com/spec/v1'

if [ ! -f "$model_path" ]; then
    printf '%s\n' "Stemgen model is missing or is not a regular file: $model_path" >&2
    exit 1
fi

prefix=$(LC_ALL=C head -c "${#lfs_pointer_header}" "$model_path")
if [ "$prefix" = "$lfs_pointer_header" ]; then
    printf '%s\n' "Stemgen model is a Git LFS pointer, not materialized model content: $model_path" >&2
    exit 1
fi

actual_size=$(stat -c '%s' "$model_path")
if [ "$actual_size" -ne "$expected_size" ]; then
    printf '%s\n' \
        "Stemgen model has size ${actual_size} bytes; expected ${expected_size} bytes: $model_path" \
        >&2
    exit 1
fi

checksum_output=$(sha256sum "$model_path")
actual_sha256=${checksum_output%%[[:space:]]*}
if [ "$actual_sha256" != "$expected_sha256" ]; then
    printf '%s\n' \
        "Stemgen model SHA-256 is ${actual_sha256}; expected ${expected_sha256}: $model_path" \
        >&2
    exit 1
fi

printf 'Verified Stemgen model: %s (%s bytes, SHA-256 %s)\n' \
    "$model_path" "$actual_size" "$actual_sha256"
