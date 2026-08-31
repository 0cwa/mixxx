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

normalize_path_for_comparison() {
    normalized_path=$1
    case "$path_platform" in
        MINGW*|MSYS*|CYGWIN*)
            ;;
        *)
            while [ "${normalized_path#//}" != "$normalized_path" ]; do
                normalized_path=${normalized_path#/}
            done
            ;;
    esac
    printf '%s\n' "$normalized_path"
}

assert_staging_directory_outside_checkout() {
    staging_current_path=$(pwd -P) || return 1
    staging_current_path_for_comparison=$(normalize_path_for_comparison "$staging_current_path")
    case "$staging_current_path_for_comparison/" in
        "$checkout_dir_for_comparison/"*)
            printf '%s\n' \
                'MIXXX_STEM_MODEL_DIR must resolve to a staging directory outside the source checkout.' \
                >&2
            return 1
            ;;
    esac
}

enter_staging_directory() {
    staging_canonical_path=$1
    staging_anchor_path=$staging_canonical_path
    staging_missing_suffix=

    # Anchor the shell in the first existing directory. Every missing path
    # component is then created and entered relative to that directory. This
    # prevents a replacement of the checked path from redirecting later file
    # operations through a symlink.
    while [ ! -e "$staging_anchor_path" ]; do
        staging_parent_path=$(dirname -- "$staging_anchor_path")
        staging_component=${staging_anchor_path##*/}
        staging_missing_suffix="/$staging_component$staging_missing_suffix"
        if [ "$staging_parent_path" = "$staging_anchor_path" ]; then
            return 1
        fi
        staging_anchor_path=$staging_parent_path
    done
    if [ ! -d "$staging_anchor_path" ] ||
            ! CDPATH='' cd -P -- "$staging_anchor_path" ||
            ! assert_staging_directory_outside_checkout; then
        return 1
    fi

    staging_remaining_suffix=${staging_canonical_path#"$staging_anchor_path"}
    while [ -n "$staging_remaining_suffix" ]; do
        staging_remaining_suffix=${staging_remaining_suffix#/}
        case "$staging_remaining_suffix" in
            */*)
                staging_component=${staging_remaining_suffix%%/*}
                staging_remaining_suffix="/${staging_remaining_suffix#*/}"
                ;;
            *)
                staging_component=$staging_remaining_suffix
                staging_remaining_suffix=
                ;;
        esac
        [ -n "$staging_component" ] || continue

        if [ ! -e "$staging_component" ]; then
            if ! mkdir -- "$staging_component" && [ ! -d "$staging_component" ]; then
                return 1
            fi
        elif [ ! -d "$staging_component" ]; then
            return 1
        fi
        if ! CDPATH='' cd -P -- "$staging_component" ||
                ! assert_staging_directory_outside_checkout; then
            return 1
        fi
    done
}

path_platform=$(uname -s)
checkout_dir=$(canonicalize_path "$script_dir/../..")
checkout_dir_for_comparison=$(normalize_path_for_comparison "$checkout_dir")
canonical_model_dir=$(canonicalize_path "$model_dir")
canonical_model_dir_for_comparison=$(normalize_path_for_comparison "$canonical_model_dir")
case "$canonical_model_dir_for_comparison/" in
    "$checkout_dir_for_comparison/"*)
        printf '%s\n' \
            'MIXXX_STEM_MODEL_DIR must resolve to a staging directory outside the source checkout.' \
            >&2
        exit 2
        ;;
esac

if ! enter_staging_directory "$canonical_model_dir"; then
    printf '%s\n' \
        'Could not safely enter the Stemgen model staging directory.' \
        >&2
    exit 2
fi

# Keep all subsequent operations relative to the directory that was entered
# above. The shell's current directory remains bound to that directory even
# if its original pathname is replaced with a symlink by another process.
model_path=./htdemucs.onnx
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
        MIXXX_STEM_MODEL_DIR=. "$script_dir/verify-stemgen-model.sh"; then
    exit 0
fi

model_url=https://github.com/mixxxdj/demucs/releases/download/v4.0.1-19-gd182d42-onnxmodel/htdemucs.onnx
temporary_dir=$(mktemp -d './.stemgen-model.XXXXXX')
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
# GNU mv -T uses rename semantics and will not descend into a destination
# directory that appears after the final-destination checks above. The
# packaging callers run in GNU/Linux environments.
mv -T -- "$temporary_path" "$model_path"

printf 'Materialized verified Stemgen model at %s\n' "$model_path"
