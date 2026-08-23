#!/usr/bin/env bash
set -euo pipefail

script_path=${BASH_SOURCE[0]}
case "$script_path" in
    */*) script_dir=${script_path%/*} ;;
    *) script_dir=. ;;
esac
case "$script_dir" in
    /*) PROJECT_DIR=$script_dir ;;
    .) PROJECT_DIR=$PWD ;;
    *) PROJECT_DIR=$PWD/$script_dir ;;
esac

TOOLBOX_NAME=${MIXXX_TOOLBOX:-mixxx-build}
BUILD_DIR=${MIXXX_BUILD_DIR:-$PROJECT_DIR/build}
BUILD_TYPE=${MIXXX_BUILD_TYPE:-RelWithDebInfo}
INSTALL_PREFIX=${MIXXX_INSTALL_PREFIX:-$HOME/.local}
INSTALL_LIBDIR=${MIXXX_INSTALL_LIBDIR:-lib}
SIGNALSMITH=${MIXXX_SIGNALSMITH:-ON}

case "$INSTALL_LIBDIR" in
    ''|.|..|../*|*/..|*/../*|/*)
        printf 'MIXXX_INSTALL_LIBDIR must be relative: %s\n' "$INSTALL_LIBDIR" >&2
        exit 2
        ;;
esac

case "$INSTALL_PREFIX" in
    /|/bin|/etc|/lib|/lib32|/lib64|/opt|/sbin|/usr|/usr/*|/var|/var/cache|/var/lib|/var/log|/var/run|/var/spool|/var/tmp|*/.mixxx|*/.mixxx/*)
        printf 'MIXXX_INSTALL_PREFIX must be a user-local prefix: %s\n' "$INSTALL_PREFIX" >&2
        exit 2
        ;;
esac

# Keep user-local native installs relocatable. The second entry also lets
# future installed shared libraries find sibling libraries in lib/mixxx.
INSTALL_RPATH="\$ORIGIN/../${INSTALL_LIBDIR}/mixxx;\$ORIGIN"

container_path() {
    case "$1" in
        /var/home/*) printf '/home/%s\n' "${1#/var/home/}" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

CONTAINER_PROJECT_DIR=${MIXXX_CONTAINER_PROJECT_DIR:-$(container_path "$PROJECT_DIR")}
CONTAINER_BUILD_DIR=${MIXXX_CONTAINER_BUILD_DIR:-$(container_path "$BUILD_DIR")}
CONTAINER_INSTALL_PREFIX=${MIXXX_CONTAINER_INSTALL_PREFIX:-$(container_path "$INSTALL_PREFIX")}

HOST_COMMAND_PREFIX=()
if [[ -e /.flatpak-info ]] && command -v flatpak-spawn >/dev/null 2>&1; then
    HOST_COMMAND_PREFIX=(flatpak-spawn --host)
fi

usage() {
    cat <<USAGE
Usage: ./localuser.sh [command] [args]

Drive the local Mixxx build through the existing "$TOOLBOX_NAME" toolbox.
When running from a Flatpak-style sandbox, host access is executed with:
  flatpak-spawn --host toolbox run --container "$TOOLBOX_NAME" ...
When already running on the host, the script uses toolbox directly.

Commands:
  build [cmake-build args]     Configure, then build. Default command.
  configure [cmake args]       Configure $BUILD_DIR.
  install [cmake-install args] Install from $BUILD_DIR to $INSTALL_PREFIX.
  all                          Configure, build, then install.
  run [mixxx args]             Run ./mixxx from $BUILD_DIR.
  help                         Show this help.

Defaults can be overridden with:
  MIXXX_TOOLBOX=$TOOLBOX_NAME
  MIXXX_BUILD_DIR=$BUILD_DIR
  MIXXX_BUILD_TYPE=$BUILD_TYPE
  MIXXX_INSTALL_PREFIX=$INSTALL_PREFIX
  MIXXX_INSTALL_LIBDIR=$INSTALL_LIBDIR
  MIXXX_SIGNALSMITH=$SIGNALSMITH
  MIXXX_JOBS=<parallel build jobs>
  MIXXX_CONTAINER_PROJECT_DIR=$CONTAINER_PROJECT_DIR
  MIXXX_CONTAINER_BUILD_DIR=$CONTAINER_BUILD_DIR
  MIXXX_CONTAINER_INSTALL_PREFIX=$CONTAINER_INSTALL_PREFIX

No images are pulled and host CMake is not used.
USAGE
}

strip_separator() {
    if [[ ${1-} == -- ]]; then
        shift
    fi
    if [[ $# -eq 0 ]]; then
        return 0
    fi
    printf '%s\0' "$@"
}

run_in_toolbox_at() {
    local cwd=$1
    shift
    local nested_script="set -euo pipefail; cd \"\$1\"; shift; exec \"\$@\""
    "${HOST_COMMAND_PREFIX[@]}" toolbox run --container "$TOOLBOX_NAME" \
        bash -lc "$nested_script" \
        bash "$cwd" "$@"
}

configure() {
    local args=()
    while IFS= read -r -d '' arg; do
        args+=("$arg")
    done < <(strip_separator "$@")

    run_in_toolbox_at "$CONTAINER_PROJECT_DIR" \
        cmake -S "$CONTAINER_PROJECT_DIR" -B "$CONTAINER_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_INSTALL_PREFIX="$CONTAINER_INSTALL_PREFIX" \
        -DCMAKE_INSTALL_LIBDIR="$INSTALL_LIBDIR" \
        -DCMAKE_INSTALL_RPATH="$INSTALL_RPATH" \
        -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=FALSE \
        -DMIXXX_INSTALL_RUNTIME_DEPENDENCIES=ON \
        -DSIGNALSMITH="$SIGNALSMITH" \
        "${args[@]}"
}

build_only() {
    local args=()
    while IFS= read -r -d '' arg; do
        args+=("$arg")
    done < <(strip_separator "$@")

    local parallel_args=()
    if [[ -n ${MIXXX_JOBS:-} ]]; then
        parallel_args=(--parallel "$MIXXX_JOBS")
    fi

    run_in_toolbox_at "$CONTAINER_PROJECT_DIR" \
        cmake --build "$CONTAINER_BUILD_DIR" \
        "${parallel_args[@]}" \
        "${args[@]}"
}

install_local() {
    local args=()
    while IFS= read -r -d '' arg; do
        args+=("$arg")
    done < <(strip_separator "$@")

    run_in_toolbox_at "$CONTAINER_PROJECT_DIR" \
        bash "$CONTAINER_PROJECT_DIR/tools/localuser-install.sh" \
        "$CONTAINER_BUILD_DIR" "$CONTAINER_INSTALL_PREFIX" "$INSTALL_LIBDIR" \
        "${args[@]}"
}

run_mixxx() {
    local args=()
    while IFS= read -r -d '' arg; do
        args+=("$arg")
    done < <(strip_separator "$@")

    run_in_toolbox_at "$CONTAINER_BUILD_DIR" ./mixxx "${args[@]}"
}

command=${1:-build}
if [[ $# -gt 0 ]]; then
    shift
fi

case "$command" in
    help|-h|--help)
        usage
        ;;
    configure)
        configure "$@"
        ;;
    build)
        configure
        build_only "$@"
        ;;
    build-only)
        build_only "$@"
        ;;
    install)
        install_local "$@"
        ;;
    all)
        configure
        build_only
        install_local
        ;;
    run)
        run_mixxx "$@"
        ;;
    *)
        printf 'Unknown command: %s\n\n' "$command" >&2
        usage >&2
        exit 2
        ;;
esac
