#!/bin/bash
# This script works with Debian, Ubuntu, and derivatives.
# shellcheck disable=SC1091

# Fail if not executed with bash
if [ -z "$BASH_VERSION" ]; then
    echo "Error: This script must be called as executable: ./debian_buildenv.sh setup" >&2
    exit 1
fi

set -e -o pipefail

readonly HTDEMUCS_MODEL_NAME="htdemucs.onnx"
readonly HTDEMUCS_MODEL_SHA256="db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d"
readonly GPAC_PACKAGE_NAME="gpac"

get_model_sha256() {
    local model_name="$1"

    case "$model_name" in
        "$HTDEMUCS_MODEL_NAME")
            printf '%s\n' "$HTDEMUCS_MODEL_SHA256"
            ;;
        *)
            return 1
            ;;
    esac
}

verify_sha256() {
    local expected_sha256="$1"
    local artifact_path="$2"

    if [[ ! "$expected_sha256" =~ ^[[:xdigit:]]{64}$ ]]; then
        echo "Invalid SHA-256 value for $artifact_path" >&2
        return 1
    fi
    if [[ ! -f "$artifact_path" ]]; then
        echo "Integrity check failed: artifact is missing: $artifact_path" >&2
        return 1
    fi
    if ! printf '%s  %s\n' "$expected_sha256" "$artifact_path" |
            sha256sum --check --status -; then
        echo "Integrity check failed: SHA-256 mismatch: $artifact_path" >&2
        return 1
    fi
}

download_verified_file() {
    local download_user="$1"
    local url="$2"
    local expected_sha256="$3"
    local destination="$4"
    local temporary_path

    temporary_path="$(sudo -u "$download_user" mktemp "${destination}.tmp.XXXXXX")" || {
        echo "Failed to create a temporary download for $destination" >&2
        return 1
    }

    if ! sudo -u "$download_user" wget --quiet --output-document="$temporary_path" "$url";
    then
        echo "Failed to download $url" >&2
        sudo -u "$download_user" rm -f -- "$temporary_path"
        return 1
    fi

    if ! verify_sha256 "$expected_sha256" "$temporary_path"; then
        sudo -u "$download_user" rm -f -- "$temporary_path"
        return 1
    fi

    if ! sudo -u "$download_user" mv -- "$temporary_path" "$destination"; then
        echo "Failed to install verified artifact at $destination" >&2
        sudo -u "$download_user" rm -f -- "$temporary_path"
        return 1
    fi
}

find_mp4box() {
    local mp4box_path
    local package_files
    local package_status

    if ! command -v dpkg-query >/dev/null 2>&1; then
        echo "Cannot verify signed '$GPAC_PACKAGE_NAME' ownership because dpkg-query is unavailable on" >&2
        echo "${PRETTY_NAME:-this Debian-based distribution}; refusing to use an arbitrary PATH binary." >&2
        echo "Use a Debian-based environment with signed '$GPAC_PACKAGE_NAME', or leave" >&2
        echo "MIXXX_INSTALL_STEM_CONVERSION=false." >&2
        return 1
    fi
    if ! package_status="$(dpkg-query -W -f='${Status}' "$GPAC_PACKAGE_NAME" 2>/dev/null)" ||
            [[ "$package_status" != "install ok installed" ]]; then
        print_gpac_unavailable_guidance
        echo "The signed distro package '$GPAC_PACKAGE_NAME' is not installed on" >&2
        echo "${PRETTY_NAME:-this Debian-based distribution}." >&2
        echo "Refusing to use an MP4Box binary found only through PATH." >&2
        return 1
    fi

    if ! package_files="$(dpkg-query -L "$GPAC_PACKAGE_NAME" 2>/dev/null)"; then
        echo "Could not query the installed '$GPAC_PACKAGE_NAME' package contents." >&2
        echo "Verify the dpkg database and reinstall the signed package if necessary." >&2
        return 1
    fi

    while IFS= read -r mp4box_path; do
        if [[ "$mp4box_path" != */MP4Box || ! -f "$mp4box_path" ||
                ! -x "$mp4box_path" ]]; then
            continue
        fi
        if dpkg-query -S "$mp4box_path" 2>/dev/null |
                awk -F': ' -v package="$GPAC_PACKAGE_NAME" '
                    { split($1, package_name, ":"); if (package_name[1] == package) found = 1 }
                    END { exit !found }'; then
            printf '%s\n' "$mp4box_path"
            return 0
        fi
    done <<< "$package_files"

    echo "The installed signed '$GPAC_PACKAGE_NAME' package on" >&2
    echo "${PRETTY_NAME:-this Debian-based distribution} does not provide an executable MP4Box." >&2
    echo "Verify the package version and architecture; do not use an arbitrary PATH binary." >&2
    return 1
}

print_gpac_unavailable_guidance() {
    if [[ "${ID:-}" == "debian" &&
            ("${VERSION_CODENAME:-}" == "bookworm" ||
                    "${VERSION_CODENAME:-}" == "trixie") ]]; then
        echo "Debian ${VERSION_CODENAME} may not publish '$GPAC_PACKAGE_NAME' in its configured repositories." >&2
        echo "Stem conversion setup is unavailable on this release without a signed distro package." >&2
        echo "Use a Debian/Ubuntu release that publishes signed '$GPAC_PACKAGE_NAME', or leave" >&2
        echo "MIXXX_INSTALL_STEM_CONVERSION=false; this script will not build GPAC from source." >&2
    else
        echo "The signed distro package '$GPAC_PACKAGE_NAME' is unavailable in the configured APT repositories." >&2
        echo "Enable a repository for this distribution that publishes it, or leave" >&2
        echo "MIXXX_INSTALL_STEM_CONVERSION=false; this script will not build GPAC from source." >&2
    fi
}

run_self_tests() {
    local test_dir
    local test_file
    local expected_sha256
    local expected_publisher_sha256
    local model_path
    local empty_path
    local fake_path

    test_dir="$(mktemp -d /tmp/mixxx-debian-buildenv-test.XXXXXX)" || return 1
    test_file="$test_dir/verified-artifact"
    printf 'verified test artifact\n' > "$test_file"
    expected_sha256="$(sha256sum "$test_file")"
    expected_sha256="${expected_sha256%% *}"

    if ! verify_sha256 "$expected_sha256" "$test_file"; then
        rm -rf -- "$test_dir"
        return 1
    fi
    if verify_sha256 \
            "0000000000000000000000000000000000000000000000000000000000000000" \
            "$test_file"; then
        echo "Self-test failed: checksum mismatch was accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    model_path="$test_dir/$HTDEMUCS_MODEL_NAME"
    if verify_sha256 "$HTDEMUCS_MODEL_SHA256" "$model_path"; then
        echo "Self-test failed: missing $HTDEMUCS_MODEL_NAME was accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    empty_path="$test_dir/empty-bin"
    mkdir "$empty_path"
    fake_path="$test_dir/fake-bin"
    mkdir "$fake_path"
    printf '#!/bin/sh\n' > "$fake_path/MP4Box"
    chmod +x "$fake_path/MP4Box"
    if PATH="$fake_path:$empty_path" find_mp4box >/dev/null 2>&1; then
        echo "Self-test failed: failed GPAC package lookup was accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if get_model_sha256 "htdemucs_ft.onnx" >/dev/null; then
        echo "Self-test failed: unsupported model has a trusted digest" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if [[ "$HTDEMUCS_MODEL_NAME" != "htdemucs.onnx" ]]; then
        echo "Self-test failed: unsupported model became the installer default" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    expected_publisher_sha256="db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d"
    if ! [[ "$expected_publisher_sha256" =~ ^[[:xdigit:]]{64}$ ]]; then
        echo "Self-test failed: publisher model digest is not a SHA-256 value" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if [[ "$(get_model_sha256 "$HTDEMUCS_MODEL_NAME")" != "$expected_publisher_sha256" ]]; then
        echo "Self-test failed: model digest is not the publisher-authoritative value" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if [[ "$HTDEMUCS_MODEL_SHA256" != "$expected_publisher_sha256" ]]; then
        echo "Self-test failed: installer model digest differs from publisher value" >&2
        rm -rf -- "$test_dir"
        return 1
    fi

    rm -rf -- "$test_dir"
    echo "Debian build environment security self-tests passed."
}

case "$1" in
    name)
        echo "No build environment name required for Debian based distros." >&2
        echo "This script installs the build dependencies via apt using the \"setup\" option." >&2
        ;;

    setup)
        source /etc/os-release 2>/dev/null
        case "${VERSION_CODENAME}" in
            jammy|bullseye|victoria|vera|vanessa|virginia) # <= Ubuntu 22.04.5 LTS
                PACKAGES_EXTRA=(
                    libqt6shadertools6-dev \
                    libqt6core5compat6-dev
                )
                ;;
            *)
                PACKAGES_EXTRA=(
                    qt6-shadertools-dev \
                    qt6-5compat-dev
                )
        esac

        case "${VERSION_CODENAME}" in
            jammy|noble|oracular|bullseye|bookworm|victoria|vera|vanessa|virginia|wilma|wildflower) # <= Ubuntu 24.10
                # qt6-svg-plugins not available
                ;;
            *)
                PACKAGES_EXTRA+=(
                    qt6-svg-plugins
                )
        esac

        if ! sudo apt-get update; then
            echo "Failed to update signed APT repository metadata; aborting setup." >&2
            exit 1
        fi

        # If jackd2 is installed as per dpkg database, install libjack-jackd2-dev.
        # This avoids a package deadlock, resulting in jackd2 being removed, and jackd1 being installed,
        # to satisfy portaudio19-dev's need for a jackd dev package. In short, portaudio19-dev needs a
        # jackd dev library, so let's give it one..
        if [ "$(dpkg-query -W -f='${Status}' jackd2 2>/dev/null | grep -c "ok installed")" -eq 1 ];
        then
            if ! sudo apt-get install -y libjack-jackd2-dev; then
                echo "Failed to install libjack-jackd2-dev; aborting setup." >&2
                exit 1
            fi
        fi

        # Stem conversion setup is opt-in through an environment variable so
        # this script remains safe for unattended CI and packaging jobs.
        INSTALL_DEMUCS="${MIXXX_INSTALL_STEM_CONVERSION:-false}"
        if [[ "$INSTALL_DEMUCS" == "true" ]]; then
            INSTALL_DEMUCS=true

            if ! apt-cache show "$GPAC_PACKAGE_NAME" 2>/dev/null |
                    grep -q "^Package: $GPAC_PACKAGE_NAME$"; then
                print_gpac_unavailable_guidance
                exit 1
            fi

            # Install GPAC/MP4Box from the existing signed distro repositories.
            # There is intentionally no source-build fallback without an
            # authoritative GPAC artifact checksum.
            if ! sudo apt-get install -y --no-install-recommends -- "$GPAC_PACKAGE_NAME"; then
                echo "Failed to install signed '$GPAC_PACKAGE_NAME' from APT; aborting setup." >&2
                exit 1
            fi
            if ! MP4BOX_PATH="$(find_mp4box)"; then
                echo "GPAC installation did not provide a package-owned executable MP4Box." >&2
                exit 1
            fi
            PACKAGES_EXTRA+=(
                libonnxruntime-dev
                libsndfile1-dev
                ffmpeg
                sox
                wget
            )
        else
            INSTALL_DEMUCS=false
        fi

        # Install a faster linker. Prefer mold, fall back to lld
        if apt-cache show mold 2>/dev/null >/dev/null;
        then
            if ! sudo apt-get install -y --no-install-recommends mold; then
                echo "Failed to install mold; aborting setup." >&2
                exit 1
            fi
        else
            if apt-cache show lld 2>/dev/null >/dev/null;
            then
                if ! sudo apt-get install -y --no-install-recommends lld; then
                    echo "Failed to install lld; aborting setup." >&2
                    exit 1
                fi
            fi
        fi

        # Check if fonts-ubuntu is available (from non-free repository)
        if apt-cache show fonts-ubuntu 2>/dev/null | grep -q "Package: fonts-ubuntu"; then
            FONTS_UBUNTU_AVAILABLE=true
        else
            echo ""
            echo "WARNING: The package 'fonts-ubuntu' is not available."
            echo "This package is required for Mixxx and is located in the Debian non-free repository."
            echo ""
            echo "See also: https://wiki.debian.org/SourcesList"
            echo ""
            if [[ "${MIXXX_REQUIRE_UBUNTU_FONTS:-false}" == "true" ]]; then
                echo "The package 'fonts-ubuntu' is unavailable; enable Debian non-free and retry."
                exit 1
            fi
        fi

        if ! sudo apt-get install -y --no-install-recommends -- \
            ccache \
            cmake \
            clazy \
            clang-tidy \
            debhelper \
            devscripts \
            docbook-to-man \
            dput \
            fonts-open-sans \
            g++ \
            lcov \
            libavformat-dev \
            libbenchmark-dev \
            libchromaprint-dev \
            libdistro-info-perl \
            libebur128-dev \
            libfaad-dev \
            libfftw3-dev \
            libflac-dev \
            libgmock-dev \
            libgtest-dev \
            libgl1-mesa-dev \
            libhidapi-dev \
            libid3tag0-dev \
            liblilv-dev \
            libmad0-dev \
            libmodplug-dev \
            libmp3lame-dev \
            libmsgsl-dev \
            libopus-dev \
            libopusfile-dev \
            libpipewire-0.3-dev \
            libportmidi-dev \
            libprotobuf-dev \
            libqt6opengl6-dev \
            libqt6sql6-sqlite \
            libqt6svg6-dev \
            librubberband-dev \
            libshout-idjc-dev \
            libsndfile1-dev \
            libsoundtouch-dev \
            libspa-0.2-dev \
            libsqlite3-dev \
            libssl-dev \
            libtag1-dev \
            libudev-dev \
            libupower-glib-dev \
            libusb-1.0-0-dev \
            libwavpack-dev \
            lv2-dev \
            markdown \
            portaudio19-dev \
            protobuf-compiler \
            qtkeychain-qt6-dev \
            qt6-declarative-private-dev \
            qt6-base-private-dev \
            qt6-multimedia-dev \
            qml6-module-qt5compat-graphicaleffects \
            qml6-module-qtcore \
            qml6-module-qtqml-workerscript \
            qml6-module-qtquick-controls \
            qml6-module-qtquick-dialogs \
            qml6-module-qtquick-layouts \
            qml6-module-qtquick-shapes \
            qml6-module-qtquick-templates \
            qml6-module-qtquick-window \
            qml6-module-qt-labs-qmlmodels \
            qml6-module-qtquick-dialogs \
            qml6-module-qt-labs-folderlistmodel \
            qml6-module-qtmultimedia \
            "${PACKAGES_EXTRA[@]}"; then
            echo "Failed to install the Debian build dependencies; aborting setup." >&2
            exit 1
        fi

        if [[ "${FONTS_UBUNTU_AVAILABLE:-false}" == true ]]; then
            if ! sudo apt-get install -y --no-install-recommends fonts-ubuntu; then
                echo "Failed to install fonts-ubuntu; aborting setup." >&2
                exit 1
            fi
        fi

        # Install demucs if user requested it
        if [ "$INSTALL_DEMUCS" = true ]; then
            # Get the actual user running sudo
            ACTUAL_USER="${SUDO_USER:-$USER}"
            ACTUAL_HOME="$(getent passwd "$ACTUAL_USER" | cut -d: -f6)"
            if [ -z "$ACTUAL_HOME" ]; then
                echo "Failed to determine the home directory for $ACTUAL_USER"
                exit 1
            fi

            # Download only models with an authoritative digest.
            echo ""
            echo "Downloading Mixxx HTDemucs ONNX models..."
            echo ""

            MODEL_PATH="${MIXXX_STEM_MODEL_DIR:-$ACTUAL_HOME/.local/mixxx_models}"
            MODEL_URL_BASE="https://github.com/mixxxdj/demucs/releases/download/v4.0.1-19-gd182d42-onnxmodel"
            # htdemucs_ft.onnx is deliberately disabled. Its release URL is
            # unavailable and no authoritative digest exists. Do not add it
            # until a maintainer publishes a verifiable artifact and digest.
            MODEL_FILES=("$HTDEMUCS_MODEL_NAME")

            echo "The unsupported htdemucs_ft.onnx model is intentionally disabled."
            echo "Maintainer action: publish a verifiable artifact and digest before re-enabling it."

            # Create directory with proper ownership
            if sudo -u "$ACTUAL_USER" mkdir -p "$MODEL_PATH"; then
                for MODEL_NAME in "${MODEL_FILES[@]}"; do
                    MODEL_FILE="$MODEL_PATH/$MODEL_NAME"
                    if ! MODEL_SHA256="$(get_model_sha256 "$MODEL_NAME")"; then
                        echo "Unsupported model $MODEL_NAME has no trusted digest; aborting." >&2
                        exit 1
                    fi
                    if ! download_verified_file \
                            "$ACTUAL_USER" \
                            "$MODEL_URL_BASE/$MODEL_NAME" \
                            "$MODEL_SHA256" \
                            "$MODEL_FILE"; then
                        echo "Failed to download Mixxx HTDemucs model $MODEL_NAME"
                        exit 1
                    fi
                    echo "Verified model downloaded successfully to $MODEL_FILE"
                done
            else
                echo "Failed to create model path: $MODEL_PATH"
                exit 1
            fi

            # Check if MP4Box is already installed system-wide
            echo ""
            if ! MP4BOX_PATH="$(find_mp4box)"; then
                exit 1
            fi
            echo "✓ MP4Box is installed at: $MP4BOX_PATH"

            echo ""
            echo "✓ Mixxx Python environment setup complete!"
            echo "Virtual environment location: $VENV_PATH"
            echo "Demucs: $VENV_PATH/bin/demucs"
            echo "MP4Box: $MP4BOX_PATH"
        fi
        ;;
    test)
        run_self_tests
        ;;
    *)
        echo "Usage: $0 [options]"
        echo ""
        echo "options:"
        echo "   help       Displays this help."
        echo "   name       Displays the name of the required build environment."
        echo "   setup      Installs the build environment."
        echo "   test       Runs download integrity self-tests without installing anything."
        ;;
esac
