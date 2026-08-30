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

canonicalize_path() {
    case "$1" in
        /*)
            absolute_path=$1
            ;;
        *)
            absolute_path="$(pwd -P)/$1"
            ;;
    esac

    existing_path=$absolute_path
    missing_suffix=
    while [ ! -d "$existing_path" ]; do
        parent_path=$(dirname -- "$existing_path")
        missing_suffix="/$(basename -- "$existing_path")$missing_suffix"
        if [ "$parent_path" = "$existing_path" ]; then
            return 1
        fi
        existing_path=$parent_path
    done

    canonical_existing_path=$(CDPATH='' cd -- "$existing_path" && pwd -P)
    printf '%s%s\n' "$canonical_existing_path" "$missing_suffix" | awk '
        {
            count = 0
            number = split($0, components, "/")
            for (component_index = 1; component_index <= number; component_index++) {
                if (components[component_index] == "" || components[component_index] == ".") {
                    continue
                }
                if (components[component_index] == "..") {
                    if (count > 0) {
                        count--
                    }
                    continue
                }
                components[++count] = components[component_index]
            }
            if (substr($0, 1, 2) == "//") {
                printf "//"
            } else {
                printf "/"
            }
            for (component_index = 1; component_index <= count; component_index++) {
                printf "%s%s", components[component_index], component_index < count ? "/" : ""
            }
            printf "\n"
        }'
}

checkout_dir=$(canonicalize_path "$script_dir/../..")
canonical_model_dir=$(canonicalize_path "$model_dir")
case "$canonical_model_dir/" in
    "$checkout_dir/"*)
        printf '%s\n' \
            'MIXXX_STEM_MODEL_DIR must resolve to a staging directory outside the source checkout.' \
            >&2
        exit 2
        ;;
esac
model_dir=$canonical_model_dir
model_url=https://github.com/mixxxdj/demucs/releases/download/v4.0.1-19-gd182d42-onnxmodel/htdemucs.onnx

mkdir -p -- "$model_dir"
model_path="$model_dir/htdemucs.onnx"
if [ -L "$model_path" ]; then
    printf '%s\n' \
        'The final Stemgen model destination must not be a symlink.' \
        >&2
    exit 2
fi
if [ -e "$model_path" ] && [ ! -f "$model_path" ]; then
    printf '%s\n' \
        'The final Stemgen model destination must be a regular file or absent.' \
        >&2
    exit 2
fi

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
