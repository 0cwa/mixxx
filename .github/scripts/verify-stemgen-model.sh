#!/bin/sh

set -eu

script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
model_dir=${MIXXX_STEM_MODEL_DIR:-"$script_dir/../../models"}
model_path="$model_dir/htdemucs.onnx"
expected_size=304413278
expected_sha256=db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d
lfs_pointer_header='version https://git-lfs.github.com/spec/v1'

python3 - "$model_path" "$expected_size" "$expected_sha256" "$lfs_pointer_header" <<'PY'
import hashlib
import os
import stat
import sys

model_path, expected_size_text, expected_sha256, lfs_pointer_header = sys.argv[1:]
expected_size = int(expected_size_text)
try:
    model_fd = os.open(model_path, os.O_RDONLY | os.O_NOFOLLOW)
except OSError:
    print(
        f"Stemgen model is missing or is not a regular file: {model_path}",
        file=sys.stderr,
    )
    raise SystemExit(1)

try:
    model_stat = os.fstat(model_fd)
    if not stat.S_ISREG(model_stat.st_mode):
        print(
            f"Stemgen model is missing or is not a regular file: {model_path}",
            file=sys.stderr,
        )
        raise SystemExit(1)

    prefix = os.read(model_fd, len(lfs_pointer_header))
    if prefix == lfs_pointer_header.encode():
        print(
            f"Stemgen model is a Git LFS pointer, not materialized model content: {model_path}",
            file=sys.stderr,
        )
        raise SystemExit(1)

    if model_stat.st_size != expected_size:
        print(
            f"Stemgen model has size {model_stat.st_size} bytes; expected {expected_size} bytes: {model_path}",
            file=sys.stderr,
        )
        raise SystemExit(1)

    os.lseek(model_fd, 0, os.SEEK_SET)
    digest = hashlib.sha256()
    while chunk := os.read(model_fd, 1024 * 1024):
        digest.update(chunk)
    actual_sha256 = digest.hexdigest()
finally:
    os.close(model_fd)

if actual_sha256 != expected_sha256:
    print(
        f"Stemgen model SHA-256 is {actual_sha256}; expected {expected_sha256}: {model_path}",
        file=sys.stderr,
    )
    raise SystemExit(1)

print(
    f"Verified Stemgen model: {model_path} ({expected_size} bytes, SHA-256 {actual_sha256})"
)
PY
