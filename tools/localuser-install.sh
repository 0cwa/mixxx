#!/usr/bin/env bash
set -eEuo pipefail

if [[ $# -lt 3 ]]; then
    printf 'Usage: %s BUILD_DIR INSTALL_PREFIX INSTALL_LIBDIR [cmake-install args...]\n' "$0" >&2
    exit 2
fi

BUILD_DIR=$1
LIVE_PREFIX_INPUT=$2
INSTALL_LIBDIR=$3
shift 3

STAGE=
ROLLBACK_DIR=
LOCK_DIR=
LOCK_ACQUIRED=0
ACTIVATED_PATHS=()
BACKED_UP_PATHS=()
ROLLBACK_NEEDED=1
CLEANUP_DONE=0

if ! command -v realpath >/dev/null 2>&1; then
    printf 'realpath is required to validate the user-local install prefix\n' >&2
    exit 2
fi

user_home_scopes=()
add_user_home_scope() {
    local scope=$1
    local canonical_scope
    canonical_scope=$(realpath -m -- "$scope") || {
        printf 'Unable to canonicalize user home scope: %s\n' "$scope" >&2
        exit 2
    }
    user_home_scopes+=("$canonical_scope")
}

add_user_home_scope "$HOME"
case "$HOME" in
    /home/*)
        add_user_home_scope "/var/home/${HOME#/home/}"
        ;;
    /var/home/*)
        add_user_home_scope "/home/${HOME#/var/home/}"
        ;;
esac

canonical_user_prefix() {
    local input=$1
    local canonical_prefix
    local scope

    case "$input" in
        /*) ;;
        *)
            printf 'Install prefix must be an absolute path: %s\n' "$input" >&2
            exit 2
            ;;
    esac

    canonical_prefix=$(realpath -m -- "$input") || {
        printf 'Unable to canonicalize install prefix: %s\n' "$input" >&2
        exit 2
    }

    for scope in "${user_home_scopes[@]}"; do
        if [[ "$scope" == "/" && "$canonical_prefix" != "/" ]] || \
            [[ "$scope" != "/" && "$canonical_prefix" == "$scope"/* ]]; then
            case "$canonical_prefix" in
                */.mixxx|*/.mixxx/*)
                    break
                    ;;
                *)
                    printf '%s\n' "$canonical_prefix"
                    return 0
                    ;;
            esac
        fi
    done

    printf 'Install prefix must be a user-local prefix inside the user home: %s\n' \
        "$input" >&2
    exit 2
}

LIVE_PREFIX=$(canonical_user_prefix "$LIVE_PREFIX_INPUT")

reject_prefix_overrides() {
    local argument
    while (($#)); do
        argument=$1
        shift
        case "$argument" in
            --prefix|--prefix=*|-DCMAKE_INSTALL_PREFIX|-DCMAKE_INSTALL_PREFIX=*|\
                -DCMAKE_INSTALL_PREFIX:*)
                printf 'Do not override the staging prefix with install argument: %s\n' \
                    "$argument" >&2
                exit 2
                ;;
            -D)
                if (($#)); then
                    case "$1" in
                        CMAKE_INSTALL_PREFIX|CMAKE_INSTALL_PREFIX=*|\
                            CMAKE_INSTALL_PREFIX:*)
                            printf 'Do not override the staging prefix with install argument: -D %s\n' \
                                "$1" >&2
                            exit 2
                            ;;
                    esac
                    shift
                fi
                ;;
        esac
    done
}

reject_prefix_overrides "$@"

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
LOCK_DIR=${LIVE_PREFIX}.mixxx-install-lock

cleanup() {
    local status=$?
    set +e

    if ((CLEANUP_DONE)); then
        return "$status"
    fi
    CLEANUP_DONE=1

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

    if [[ -n "$STAGE" ]]; then
        rm -rf -- "$STAGE"
    fi
    if [[ -n "$ROLLBACK_DIR" ]]; then
        rm -rf -- "$ROLLBACK_DIR"
    fi
    if ((LOCK_ACQUIRED)); then
        rm -rf -- "$LOCK_DIR"
        LOCK_ACQUIRED=0
    fi
    exit "$status"
}
trap cleanup EXIT

mkdir -p "$LIVE_PARENT"

if ! mkdir "$LOCK_DIR" 2>/dev/null; then
    printf 'Mixxx install is already active: %s\n' "$LIVE_PREFIX" >&2
    exit 1
fi
LOCK_ACQUIRED=1

STAGE=$(mktemp -d "$LIVE_PARENT/.mixxx-install.XXXXXX")
ROLLBACK_DIR=$(mktemp -d "$LIVE_PARENT/.mixxx-rollback.XXXXXX")

cmake_command=${MIXXX_CMAKE_COMMAND:-cmake}
"$cmake_command" --install "$BUILD_DIR" "$@" --prefix "$STAGE"

if [[ ! -x "$STAGE/bin/mixxx" ]]; then
    printf 'Staged Mixxx executable is missing: %s\n' "$STAGE/bin/mixxx" >&2
    exit 1
fi

runtime_dependency_script=$BUILD_DIR/InstallMixxxRuntimeDependencies.cmake
if [[ -f "$runtime_dependency_script" ]]; then
    "$cmake_command" \
        -DCMAKE_INSTALL_PREFIX="$STAGE" \
        -DMIXXX_RUNTIME_EXECUTABLE="$STAGE/bin/mixxx" \
        -P "$runtime_dependency_script"
fi

if ! command -v readelf >/dev/null 2>&1; then
    printf 'readelf is required to validate installed ELF RUNPATH/RPATH\n' >&2
    exit 1
fi

validate_elf_runpaths() {
    local staged_file=$1
    local origin_dir=${staged_file%/*}
    local runpath
    local runpath_entry
    local relative_runpath
    local resolved_runpath
    local -a runpath_entries

    if ! readelf -h "$staged_file" >/dev/null 2>&1; then
        return 0
    fi

    while IFS= read -r runpath; do
        IFS=: read -r -a runpath_entries <<< "$runpath"
        for runpath_entry in "${runpath_entries[@]}"; do
            case "$runpath_entry" in
                "\$ORIGIN") ;;
                "\$ORIGIN/"*)
                    relative_runpath=${runpath_entry#"\$ORIGIN"}
                    case "$relative_runpath" in
                        *'\$'*)
                            printf 'Staged ELF has an unsupported RUNPATH/RPATH token: %s -> %s\n' \
                                "$staged_file" "$runpath_entry" >&2
                            exit 1
                            ;;
                    esac
                    resolved_runpath=$(realpath -m -- \
                        "$origin_dir/$relative_runpath")
                    case "$resolved_runpath" in
                        "$STAGE"|"$STAGE"/*) ;;
                        *)
                            printf 'Staged ELF RUNPATH/RPATH escapes the stage: %s -> %s\n' \
                                "$staged_file" "$runpath_entry" >&2
                            exit 1
                            ;;
                    esac
                    ;;
                *)
                    printf '%s\n' \
                        "Staged ELF RUNPATH/RPATH is not a safe \$ORIGIN path: $staged_file -> $runpath_entry" \
                        >&2
                    exit 1
                    ;;
            esac
        done
    done < <(
        readelf -d "$staged_file" 2>/dev/null |
            sed -n -E 's/.*\((RUNPATH|RPATH)\).*: \[(.*)\]/\2/p'
    )
}

mapfile -d '' staged_regular_files < <(find "$STAGE" -type f -print0)
for staged_regular_file in "${staged_regular_files[@]}"; do
    validate_elf_runpaths "$staged_regular_file"
done

mapfile -d '' staged_symlinks < <(find "$STAGE" -type l -print0)
for staged_symlink in "${staged_symlinks[@]}"; do
    symlink_target=$(readlink -- "$staged_symlink")
    if [[ "$symlink_target" = /* ]]; then
        printf 'Staged symlink is not relative: %s -> %s\n' \
            "$staged_symlink" "$symlink_target" >&2
        exit 1
    fi
    resolved_symlink=$(realpath -m -- \
        "${staged_symlink%/*}/$symlink_target")
    case "$resolved_symlink" in
        "$STAGE"|"$STAGE"/*) ;;
        *)
            printf 'Staged symlink escapes the stage: %s -> %s\n' \
                "$staged_symlink" "$symlink_target" >&2
            exit 1
            ;;
    esac
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
