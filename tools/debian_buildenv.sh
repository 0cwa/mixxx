#!/bin/bash
# This script works with Debian, Ubuntu, and derivatives.
# shellcheck disable=SC1091

# Fail if not executed with bash
if [ -z "$BASH_VERSION" ]; then
    echo "Error: This script must be called as executable: ./debian_buildenv.sh setup" >&2
    exit 1
fi

set -o pipefail

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

        sudo apt-get update

        # If jackd2 is installed as per dpkg database, install libjack-jackd2-dev.
        # This avoids a package deadlock, resulting in jackd2 being removed, and jackd1 being installed,
        # to satisfy portaudio19-dev's need for a jackd dev package. In short, portaudio19-dev needs a
        # jackd dev library, so let's give it one..
        if [ "$(dpkg-query -W -f='${Status}' jackd2 2>/dev/null | grep -c "ok installed")" -eq 1 ];
        then
            sudo apt-get install libjack-jackd2-dev;
        fi

        # Stem conversion setup is opt-in through an environment variable so
        # this script remains safe for unattended CI and packaging jobs.
        INSTALL_DEMUCS="${MIXXX_INSTALL_STEM_CONVERSION:-false}"
        if [[ "$INSTALL_DEMUCS" == "true" ]]; then
            INSTALL_DEMUCS=true

            # Add demucs dependencies if user wants it
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
            sudo apt-get install -y --no-install-recommends mold
        else
            if apt-cache show lld 2>/dev/null >/dev/null;
            then
                sudo apt-get install -y --no-install-recommends lld
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

        sudo apt-get install -y --no-install-recommends -- \
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
            "${PACKAGES_EXTRA[@]}"

        if [[ "${FONTS_UBUNTU_AVAILABLE:-false}" == true ]]; then
            sudo apt-get install -y --no-install-recommends fonts-ubuntu
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

            # Download every model selectable in the stem conversion UI.
            echo ""
            echo "Downloading Mixxx HTDemucs ONNX models..."
            echo ""

            MODEL_PATH="${MIXXX_STEM_MODEL_DIR:-$ACTUAL_HOME/.local/mixxx_models}"
            MODEL_URL_BASE="https://github.com/mixxxdj/demucs/releases/download/v4.0.1-19-gd182d42-onnxmodel"
            MODEL_FILES=(htdemucs.onnx htdemucs_ft.onnx)

            # Create directory with proper ownership
            if sudo -u "$ACTUAL_USER" mkdir -p "$MODEL_PATH"; then
                for MODEL_NAME in "${MODEL_FILES[@]}"; do
                    MODEL_FILE="$MODEL_PATH/$MODEL_NAME"
                    if sudo -u "$ACTUAL_USER" wget -c "$MODEL_URL_BASE/$MODEL_NAME" -O "$MODEL_FILE"; then
                        echo "Model downloaded successfully to $MODEL_FILE"
                        sudo chown "$ACTUAL_USER:$ACTUAL_USER" "$MODEL_FILE"
                    else
                        echo "Failed to download Mixxx HTDemucs model $MODEL_NAME"
                        exit 1
                    fi
                done
            else
                echo "Failed to create model path: $MODEL_PATH"
                exit 1
            fi

            # Check if MP4Box is already installed system-wide
            echo ""
            if command -v MP4Box &> /dev/null; then
                echo "✓ MP4Box is already installed at: $(command -v MP4Box)"
                MP4BOX_PATH=$(command -v MP4Box)
            else
                echo "MP4Box not found. Compiling GPAC v2.4.0 from source..."

                GPAC_BUILD_DIR="/tmp/gpac_build"

                # Create build directory
                mkdir -p "$GPAC_BUILD_DIR"
                cd "$GPAC_BUILD_DIR" || exit 1

                # Download GPAC v2.4.0
                echo "Downloading GPAC v2.4.0..."
                wget -q https://github.com/gpac/gpac/archive/refs/tags/v2.4.0.tar.gz -O gpac-2.4.0.tar.gz

                if [ ! -f "gpac-2.4.0.tar.gz" ]; then
                    echo "Failed to download GPAC"
                    exit 1
                fi

                # Extract
                echo "Extracting GPAC..."
                tar -xzf gpac-2.4.0.tar.gz
                cd gpac-2.4.0 || exit 1

                # Configure and compile for system installation
                echo "Configuring GPAC..."
                if ! ./configure --prefix=/usr/local; then
                    echo "GPAC configuration failed"
                    exit 1
                fi

                echo "Compiling GPAC (this may take a few minutes)..."
                if ! make -j"$(nproc)"; then
                    echo "GPAC compilation failed"
                    exit 1
                fi

                echo "Installing GPAC to system..."
                if sudo make install; then
                    echo "GPAC v2.4.0 compiled and installed successfully"
                    echo "Location: /usr/local/bin/MP4Box"

                    # Verify MP4Box
                    if [ -f "/usr/local/bin/MP4Box" ]; then
                        echo "MP4Box executable found"
                        /usr/local/bin/MP4Box -version
                        MP4BOX_PATH="/usr/local/bin/MP4Box"
                    else
                        echo "MP4Box executable not found after installation"
                        exit 1
                    fi
                else
                    echo "Failed to install GPAC"
                    exit 1
                fi

                # Clean up build directory
                cd /
                rm -rf "$GPAC_BUILD_DIR"
            fi

            echo ""
            echo "✓ Mixxx Python environment setup complete!"
            echo "Virtual environment location: $VENV_PATH"
            echo "Demucs: $VENV_PATH/bin/demucs"
            echo "MP4Box: $MP4BOX_PATH"
        fi
        ;;
    *)
        echo "Usage: $0 [options]"
        echo ""
        echo "options:"
        echo "   help       Displays this help."
        echo "   name       Displays the name of the required build environment."
        echo "   setup      Installs the build environment."
        ;;
esac
