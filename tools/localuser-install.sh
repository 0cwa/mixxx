#!/usr/bin/env bash
set -eEuo pipefail

if [[ $# -lt 3 ]]; then
    printf 'Usage: %s BUILD_DIR INSTALL_PREFIX INSTALL_LIBDIR [cmake-install args...]\n' "$0" >&2
    exit 2
fi

BUILD_DIR=$1
LIVE_PREFIX=${2%/}
INSTALL_LIBDIR=$3
shift 3

case "$INSTALL_LIBDIR" in
    ''|.|..|../*|*/..|*/../*|/*)
        printf 'Install library directory must be relative: %s\n' "$INSTALL_LIBDIR" >&2
        exit 2
        ;;
esac

case "$LIVE_PREFIX" in
    /|/bin|/etc|/lib|/lib32|/lib64|/opt|/sbin|/tmp|/usr|/usr/*|/var|/var/cache|/var/lib|/var/log|/var/run|/var/spool|/var/tmp|*/.mixxx|*/.mixxx/*)
        printf 'Install prefix must be a user-local prefix: %s\n' "$LIVE_PREFIX" >&2
        exit 2
        ;;
esac

LIVE_PARENT=${LIVE_PREFIX%/*}
if [[ "$LIVE_PARENT" == "$LIVE_PREFIX" ]]; then
    LIVE_PARENT=.
fi
mkdir -p "$LIVE_PARENT"

LOCK_DIR=${LIVE_PREFIX}.mixxx-install-lock
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
    printf 'Mixxx install is already active: %s\n' "$LIVE_PREFIX" >&2
    exit 1
fi

STAGE=$(mktemp -d "$LIVE_PARENT/.mixxx-install.XXXXXX")
ROLLBACK_DIR=$(mktemp -d "$LIVE_PARENT/.mixxx-rollback.XXXXXX")
ACTIVATED_PATHS=()
BACKED_UP_PATHS=()
ROLLBACK_NEEDED=1

cleanup() {
    local status=$?
    set +e

    if ((ROLLBACK_NEEDED)); then
        local index
        for ((index = ${#ACTIVATED_PATHS[@]} - 1; index >= 0; index--)); do
            local relative_path=${ACTIVATED_PATHS[index]}
            local destination="$LIVE_PREFIX/$relative_path"
            if [[ -e "$destination" || -L "$destination" ]]; then
                rm -rf -- "$destination"
            fi
        done
        for ((index = ${#BACKED_UP_PATHS[@]} - 1; index >= 0; index--)); do
            local relative_path=${BACKED_UP_PATHS[index]}
            local destination="$LIVE_PREFIX/$relative_path"
            local backup="$ROLLBACK_DIR/$relative_path"
            if [[ -e "$backup" || -L "$backup" ]]; then
                mkdir -p "$(dirname "$destination")"
                mv -- "$backup" "$destination"
            fi
        done
    fi

    rm -rf -- "$STAGE" "$ROLLBACK_DIR" "$LOCK_DIR"
    exit "$status"
}
trap cleanup EXIT

cmake_command=${MIXXX_CMAKE_COMMAND:-cmake}
"$cmake_command" --install "$BUILD_DIR" --prefix "$STAGE" "$@"

if [[ ! -x "$STAGE/bin/mixxx" ]]; then
    printf 'Staged Mixxx executable is missing: %s\n' "$STAGE/bin/mixxx" >&2
    exit 1
fi

# The CMake scanner performs architecture/ABI-aware dependency resolution.
# Keep the installed ELF relocatable as an additional guard against an
# accidental host path in the generated native install.
if command -v readelf >/dev/null 2>&1; then
    runpath=$(readelf -d "$STAGE/bin/mixxx" 2>/dev/null | sed -n \
        -e 's/.*(RUNPATH).*: \[\(.*\)\]/\1/p' \
        -e 's/.*(RPATH).*: \[\(.*\)\]/\1/p' || true)
    if [[ -n "$runpath" ]]; then
        IFS=: read -ra runpath_entries <<< "$runpath"
        for runpath_entry in "${runpath_entries[@]}"; do
            case "$runpath_entry" in
                "\$ORIGIN"|"\$ORIGIN/"*) ;;
                *)
                    printf 'Staged Mixxx RUNPATH is not relative: %s\n' "$runpath" >&2
                    exit 1
                    ;;
            esac
        done
    fi
fi

mapfile -d '' staged_symlinks < <(find "$STAGE" -type l -print0)
for staged_symlink in "${staged_symlinks[@]}"; do
    symlink_target=$(readlink -- "$staged_symlink")
    if [[ "$symlink_target" = /* ]]; then
        printf 'Staged symlink is not relative: %s -> %s\n' \
            "$staged_symlink" "$symlink_target" >&2
        exit 1
    fi
done

move_to_backup() {
    local relative_path=$1
    local destination="$LIVE_PREFIX/$relative_path"
    local backup="$ROLLBACK_DIR/$relative_path"
    if [[ -e "$destination" || -L "$destination" ]]; then
        mkdir -p "$(dirname "$backup")"
        mv -- "$destination" "$backup"
        BACKED_UP_PATHS+=("$relative_path")
    fi
}

activate_path() {
    local relative_path=$1
    case "$relative_path" in
        ''|.|..|../*|*/..|*/../*|/*)
            printf 'Invalid staged relative path: %s\n' "$relative_path" >&2
            exit 1
            ;;
    esac

    local source="$STAGE/$relative_path"
    local destination="$LIVE_PREFIX/$relative_path"
    if [[ ! -e "$source" && ! -L "$source" ]]; then
        return 0
    fi
    move_to_backup "$relative_path"
    mkdir -p "$(dirname "$destination")"
    mv -- "$source" "$destination"
    ACTIVATED_PATHS+=("$relative_path")
}

# These paths are wholly owned by Mixxx. Swapping the dependency directory as
# one unit removes stale SONAME files and makes repeated installs converge to
# the same versioned-target-plus-symlink-chain layout.
activate_path "bin/mixxx"
if [[ -d "$STAGE/$INSTALL_LIBDIR/mixxx" || -d "$LIVE_PREFIX/$INSTALL_LIBDIR/mixxx" ]]; then
    if [[ ! -d "$STAGE/$INSTALL_LIBDIR/mixxx" ]]; then
        mkdir -p "$STAGE/$INSTALL_LIBDIR/mixxx"
    fi
    activate_path "$INSTALL_LIBDIR/mixxx"
fi
activate_path "share/mixxx"
activate_path "share/doc/mixxx"

mapfile -d '' staged_files < <(
    find "$STAGE" -mindepth 1 \( -type f -o -type l \) -print0
)
for staged_file in "${staged_files[@]}"; do
    relative_path=${staged_file#"$STAGE/"}
    case "$relative_path" in
        bin/mixxx|"$INSTALL_LIBDIR/mixxx"|"$INSTALL_LIBDIR/mixxx"/*|share/mixxx|share/mixxx/*|share/doc/mixxx|share/doc/mixxx/*)
            continue
            ;;
    esac
    activate_path "$relative_path"
done

ROLLBACK_NEEDED=0
printf 'Activated staged Mixxx install at %s\n' "$LIVE_PREFIX"
