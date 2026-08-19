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
SIGNALSMITH=${MIXXX_SIGNALSMITH:-ON}

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
        cmake --install "$CONTAINER_BUILD_DIR" \
        --prefix "$CONTAINER_INSTALL_PREFIX" \
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
