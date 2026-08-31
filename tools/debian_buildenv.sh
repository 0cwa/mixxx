#!/bin/bash
# This script works with Debian, Ubuntu, and derivatives.
# shellcheck disable=SC1091

# Fail if not executed with bash
if [ -z "$BASH_VERSION" ]; then
    echo "Error: This script must be called as executable: ./debian_buildenv.sh setup" >&2
    exit 1
fi

set -e -o pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly SCRIPT_DIR
readonly ONNXRUNTIME_HELPER="$SCRIPT_DIR/onnxruntime_buildenv.sh"
readonly SECURE_DOWNLOAD_HELPER="$SCRIPT_DIR/secure-download.py"
readonly HTDEMUCS_MODEL_NAME="htdemucs.onnx"
readonly HTDEMUCS_MODEL_URL="https://github.com/mixxxdj/demucs/releases/download/v4.0.1-19-gd182d42-onnxmodel/htdemucs.onnx"
readonly HTDEMUCS_MODEL_SIZE=304413278
readonly HTDEMUCS_MODEL_SHA256="db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d"
readonly GPAC_PACKAGE_NAME="gpac"

canonicalize_model_staging_directory() {
    local model_path="$1"
    local checkout_path
    local canonical_model_path

    if ! checkout_path="$(realpath -m -- "$SCRIPT_DIR/..")" ||
            ! canonical_model_path="$(realpath -m -- "$model_path")"; then
        echo "Could not resolve MIXXX_STEM_MODEL_DIR: $model_path" >&2
        return 1
    fi

    case "$canonical_model_path/" in
        "$checkout_path/"*)
            echo "MIXXX_STEM_MODEL_DIR must resolve to a staging directory outside the source checkout." >&2
            return 1
            ;;
    esac

    printf '%s\n' "$canonical_model_path"
}

validate_model_file_destination() {
    local model_file_path="$1"

    if [[ -L "$model_file_path" ]]; then
        echo "The final Stemgen model destination must not be a symlink." >&2
        return 1
    fi
    if [[ -e "$model_file_path" && ! -f "$model_file_path" ]]; then
        echo "The final Stemgen model destination must be a regular file or absent." >&2
        return 1
    fi
}

validate_model_staging_destination() {
    local model_path="$1"
    local canonical_model_path
    local model_file_path

    if ! canonical_model_path="$(canonicalize_model_staging_directory "$model_path")"; then
        return 1
    fi
    model_file_path="$canonical_model_path/$HTDEMUCS_MODEL_NAME"
    if ! validate_model_file_destination "$model_file_path"; then
        return 1
    fi

    printf '%s\n' "$canonical_model_path"
}

assert_model_staging_directory_outside_checkout() {
    local current_model_path
    local checkout_path

    if ! current_model_path="$(pwd -P)" ||
            ! checkout_path="$(realpath -m -- "$SCRIPT_DIR/..")"; then
        return 1
    fi
    case "$current_model_path/" in
        "$checkout_path/"*)
            echo "MIXXX_STEM_MODEL_DIR must resolve to a staging directory outside the source checkout." >&2
            return 1
            ;;
    esac
}

enter_model_staging_directory() {
    local model_path="$1"
    local download_user="$2"
    local canonical_model_path
    local staging_anchor_path
    local staging_remaining_suffix
    local staging_parent_path
    local staging_component

    if ! canonical_model_path="$(validate_model_staging_destination "$model_path")"; then
        return 1
    fi

    # Anchor the shell in the first existing directory. Every missing path
    # component is then created and entered relative to that directory. This
    # prevents a replacement of the checked path from redirecting later file
    # operations through a symlink.
    staging_anchor_path="$canonical_model_path"
    while [[ ! -e "$staging_anchor_path" ]]; do
        staging_parent_path="${staging_anchor_path%/*}"
        if [[ -z "$staging_parent_path" ]]; then
            staging_parent_path=/
        fi
        staging_anchor_path="$staging_parent_path"
    done
    if [[ ! -d "$staging_anchor_path" ]] ||
            ! cd -P -- "$staging_anchor_path" ||
            ! assert_model_staging_directory_outside_checkout; then
        return 1
    fi

    staging_remaining_suffix="${canonical_model_path#"$staging_anchor_path"}"
    while [[ -n "$staging_remaining_suffix" ]]; do
        staging_remaining_suffix="${staging_remaining_suffix#/}"
        if [[ "$staging_remaining_suffix" == */* ]]; then
            staging_component="${staging_remaining_suffix%%/*}"
            staging_remaining_suffix="/${staging_remaining_suffix#*/}"
        else
            staging_component="$staging_remaining_suffix"
            staging_remaining_suffix=
        fi

        [[ -n "$staging_component" ]] || continue
        if [[ ! -e "$staging_component" ]]; then
            if ! sudo -u "$download_user" mkdir -- "$staging_component" &&
                    [[ ! -d "$staging_component" ]]; then
                return 1
            fi
        elif [[ ! -d "$staging_component" ]]; then
            return 1
        fi
        if ! cd -P -- "$staging_component" ||
                ! assert_model_staging_directory_outside_checkout; then
            return 1
        fi
    done

    validate_model_file_destination "$HTDEMUCS_MODEL_NAME"
}

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

verify_file_size() {
    local expected_size="$1"
    local artifact_path="$2"
    local actual_size

    if [[ ! "$expected_size" =~ ^[0-9]+$ ]]; then
        echo "Invalid expected size for $artifact_path" >&2
        return 1
    fi
    if [[ ! -f "$artifact_path" ]]; then
        echo "Integrity check failed: artifact is missing: $artifact_path" >&2
        return 1
    fi
    if ! actual_size="$(stat -c '%s' -- "$artifact_path")"; then
        echo "Integrity check failed: could not determine size: $artifact_path" >&2
        return 1
    fi
    if [[ "$actual_size" != "$expected_size" ]]; then
        echo "Integrity check failed: expected $expected_size bytes, got $actual_size: $artifact_path" >&2
        return 1
    fi
}

download_verified_file() {
    local download_user="$1"
    local url="$2"
    local expected_size="$3"
    local expected_sha256="$4"
    local destination="$5"
    local temporary_dir
    local temporary_dir_name

    if ! validate_model_file_destination "$destination"; then
        return 1
    fi

    (
        local staging_directory_path
        local staging_fd_path
        local temporary_directory_path

        exec 9<.
        staging_fd_path="/proc/$BASHPID/fd/9"

        temporary_dir="$(sudo -u "$download_user" mktemp -d "./.${destination}.tmp.XXXXXX")" || {
            echo "Failed to create a temporary download for $destination" >&2
            exit 1
        }
        temporary_dir_name=${temporary_dir#./}
        staging_directory_path="$(cd -P -- "$staging_fd_path" && pwd -P)"
        temporary_directory_path="$staging_directory_path/$temporary_dir_name"

        # shellcheck disable=SC2329 # Invoked indirectly by the EXIT trap.
        cleanup_temporary_download() {
            rm -rf -- "${staging_fd_path:?}/${temporary_dir_name:?}" 2>/dev/null || :
            exec 9<&-
        }
        trap cleanup_temporary_download EXIT HUP INT TERM

        if ! cd -P -- "$staging_fd_path/$temporary_dir_name" ||
                [[ "$(pwd -P)" != "$temporary_directory_path" ]] ||
                ! assert_model_staging_directory_outside_checkout; then
            echo "Could not safely enter the temporary download directory for $destination" >&2
            exit 1
        fi
        if ! sudo --preserve-fds=9 -u "$download_user" \
                python3 "$SECURE_DOWNLOAD_HELPER" "./$HTDEMUCS_MODEL_NAME" \
                --verify-size "$expected_size" \
                --verify-sha256 "$expected_sha256" \
                --install-fd 9 \
                --install-name "$destination" \
                -- \
                wget \
                    --no-verbose \
                    --output-document=- \
                    --tries=3 \
                    --timeout=30 \
                    --waitretry=5 \
                    "$url";
        then
            echo "Failed to download $url" >&2
            exit 1
        fi
    )
}

find_mp4box() {
    local mp4box_path
    local dpkg_query_path
    local package_status

    if ! dpkg_query_path="$(find_dpkg_query)"; then
        print_dpkg_query_unavailable
        return 1
    fi

    if ! package_status="$("$dpkg_query_path" -W -f="\${Status}" "$GPAC_PACKAGE_NAME" 2>/dev/null)" ||
            [[ "$package_status" != "install ok installed" ]]; then
        print_gpac_install_guidance
        echo "The signed distro package '$GPAC_PACKAGE_NAME' is not installed on" >&2
        echo "${PRETTY_NAME:-this Debian-based distribution}." >&2
        echo "Refusing to use an MP4Box binary found only through PATH." >&2
        return 1
    fi

    if ! mp4box_path="$(find_dpkg_owned_mp4box "$dpkg_query_path")"; then
        return 1
    fi

    printf '%s\n' "$mp4box_path"
}

print_dpkg_query_unavailable() {
    echo "Cannot verify signed '$GPAC_PACKAGE_NAME' ownership: the dpkg runtime" >&2
    echo "does not provide /usr/bin/dpkg-query or /bin/dpkg-query on this system." >&2
    echo "Use a Debian/Ubuntu environment with the dpkg runtime; this backend refuses" >&2
    echo "to use an arbitrary PATH binary." >&2
}

find_dpkg_query() {
    local dpkg_query_path

    # Do not resolve this trust anchor through PATH: a caller-controlled
    # dpkg-query must not be able to manufacture package ownership.
    for dpkg_query_path in /usr/bin/dpkg-query /bin/dpkg-query; do
        if [[ -x "$dpkg_query_path" ]]; then
            printf '%s\n' "$dpkg_query_path"
            return 0
        fi
    done
    return 1
}

find_dpkg_owned_mp4box() {
    # Production callers pass only the absolute path returned by
    # find_dpkg_query. The explicit parameter also makes the ownership logic
    # testable with a deterministic package database fixture.
    local dpkg_query_path="$1"
    local mp4box_path
    local package_files

    if ! package_files="$("$dpkg_query_path" -L "$GPAC_PACKAGE_NAME" 2>/dev/null)"; then
        echo "Could not query the installed '$GPAC_PACKAGE_NAME' package contents." >&2
        echo "Verify the dpkg database and reinstall the signed package if necessary." >&2
        return 1
    fi

    while IFS= read -r mp4box_path; do
        if [[ "$mp4box_path" != */MP4Box || ! -f "$mp4box_path" ||
                ! -x "$mp4box_path" ]]; then
            continue
        fi
        if "$dpkg_query_path" -S "$mp4box_path" 2>/dev/null |
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

print_gpac_install_guidance() {
    if [[ "${ID:-}" == "ubuntu" ]]; then
        echo "Ubuntu publishes the signed '$GPAC_PACKAGE_NAME' package in the universe component." >&2
        echo "If APT cannot find it, enable universe, refresh metadata, and install it:" >&2
        echo "  sudo add-apt-repository -y universe" >&2
        echo "  sudo apt-get update --error-on=any" >&2
        echo "  sudo apt-get install --no-install-recommends '$GPAC_PACKAGE_NAME'" >&2
        echo "If add-apt-repository is unavailable, install software-properties-common first." >&2
        echo "If the package remains unavailable, leave MIXXX_INSTALL_STEM_CONVERSION=false." >&2
    else
        echo "Install the signed distro package '$GPAC_PACKAGE_NAME' with APT, then retry." >&2
        echo "If it is unavailable for this release, leave MIXXX_INSTALL_STEM_CONVERSION=false." >&2
    fi
}

print_gpac_unavailable_guidance() {
    if [[ "${ID:-}" == "ubuntu" ]]; then
        echo "APT cannot find the signed '$GPAC_PACKAGE_NAME' package in the configured Ubuntu repositories." >&2
        print_gpac_install_guidance
    elif [[ "${ID:-}" == "debian" &&
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
    local fake_path
    local package_path
    local fake_mp4box_path
    local package_mp4box_path
    local dpkg_query_path
    local query_mode_path
    local original_path
    local mp4box_result
    local selected_dpkg_query_path
    local trusted_dpkg_query_path
    local poison_marker_path
    local actual_test_size
    local checkout_model_path
    local checkout_model_symlink
    local normalized_checkout_model_path
    local external_model_path
    local checkout_model_file
    local external_model_file
    local non_regular_model_path
    local race_model_path
    local race_model_backup_path
    local race_payload_path
    local race_payload_size
    local race_payload_sha256
    local checkout_model_before
    local sudo_path
    local wget_path
    local final_race_model_path
    local temporary_race_model_path
    local temporary_race_backup_path

    test_dir="$(mktemp -d /tmp/mixxx-debian-buildenv-test.XXXXXX)" || return 1
    test_file="$test_dir/verified-artifact"
    printf 'verified test artifact\n' > "$test_file"
    expected_sha256="$(sha256sum "$test_file")"
    expected_sha256="${expected_sha256%% *}"

    if ! verify_sha256 "$expected_sha256" "$test_file"; then
        rm -rf -- "$test_dir"
        return 1
    fi
    actual_test_size="$(stat -c '%s' -- "$test_file")"
    if ! verify_file_size "$actual_test_size" "$test_file"; then
        echo "Self-test failed: matching size was rejected" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if verify_file_size "$((actual_test_size + 1))" "$test_file"; then
        echo "Self-test failed: size mismatch was accepted" >&2
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
    original_path="$PATH"
    fake_path="$test_dir/fake-bin"
    mkdir "$fake_path"
    fake_mp4box_path="$fake_path/MP4Box"
    printf '#!/bin/sh\nexit 0\n' > "$fake_mp4box_path"
    chmod +x "$fake_mp4box_path"
    poison_marker_path="$test_dir/dpkg-query-used"

    package_path="$test_dir/package-root/usr/bin"
    mkdir -p "$package_path"
    package_mp4box_path="$package_path/MP4Box"
    printf '#!/bin/sh\nexit 0\n' > "$package_mp4box_path"
    chmod +x "$package_mp4box_path"

    query_mode_path="$test_dir/dpkg-query-mode"
    printf 'unowned\n' > "$query_mode_path"
    dpkg_query_path="$fake_path/dpkg-query"
    # The generated fixture intentionally contains literal shell variables.
    # shellcheck disable=SC2016
    printf '%s\n' \
        '#!/bin/sh' \
        "query_mode_path='$query_mode_path'" \
        "fake_mp4box_path='$fake_mp4box_path'" \
        "package_mp4box_path='$package_mp4box_path'" \
        "poison_marker_path='$poison_marker_path'" \
        'touch "$poison_marker_path"' \
        'case "$1" in' \
        '    -W)' \
        '        printf "%s\\n" "install ok installed"' \
        '        ;;' \
        '    -L)' \
        '        if [ "$(cat "$query_mode_path")" = "unowned" ]; then' \
        '            printf "%s\\n" "$fake_mp4box_path"' \
        '        else' \
        '            printf "%s\\n" "$package_mp4box_path"' \
        '        fi' \
        '        ;;' \
        '    -S)' \
        '        if [ "$2" = "$package_mp4box_path" ]; then' \
        '            printf "gpac:amd64: %s\\n" "$2"' \
        '        else' \
        '            printf "untrusted: %s\\n" "$2"' \
        '        fi' \
        '        ;;' \
        '    *)' \
        '        exit 1' \
        '        ;;' \
        'esac' > "$dpkg_query_path"
    chmod +x "$dpkg_query_path"

    if ! trusted_dpkg_query_path="$(find_dpkg_query 2>/dev/null)"; then
        trusted_dpkg_query_path=""
    fi
    if ! selected_dpkg_query_path="$(PATH="$fake_path:$original_path" find_dpkg_query 2>/dev/null)"; then
        selected_dpkg_query_path=""
    fi
    if [[ -n "$trusted_dpkg_query_path" ]]; then
        if [[ "$selected_dpkg_query_path" != "$trusted_dpkg_query_path" ]]; then
            echo "Self-test failed: PATH poisoning changed the trusted dpkg-query helper" >&2
            rm -rf -- "$test_dir"
            return 1
        fi
    elif [[ -n "$selected_dpkg_query_path" ]]; then
        echo "Self-test failed: PATH dpkg-query bypassed fail-closed selection" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if PATH="$fake_path:$original_path" find_dpkg_owned_mp4box "$dpkg_query_path" >/dev/null 2>&1; then
        echo "Self-test failed: an unowned fake MP4Box was accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    printf 'owned\n' > "$query_mode_path"
    if ! mp4box_result="$(PATH="$fake_path:$original_path" \
            find_dpkg_owned_mp4box "$dpkg_query_path")" ||
            [[ "$mp4box_result" != "$package_mp4box_path" ]]; then
        echo "Self-test failed: package-owned MP4Box layout was not accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if [[ -n "$trusted_dpkg_query_path" ]]; then
        rm -f -- "$poison_marker_path"
        if PATH="$fake_path:$original_path" find_mp4box >/dev/null 2>&1; then
            :
        fi
        if [[ -e "$poison_marker_path" ]]; then
            echo "Self-test failed: production MP4Box lookup executed PATH dpkg-query" >&2
            rm -rf -- "$test_dir"
            return 1
        fi
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
    # Keep this independent from HTDEMUCS_MODEL_SHA256 so the self-test detects
    # accidental changes to the production trust anchor.
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

    checkout_model_path="$SCRIPT_DIR/../models"
    normalized_checkout_model_path="$SCRIPT_DIR//../models/./../models"
    checkout_model_symlink="$test_dir/checkout-models-link"
    external_model_path="$test_dir/external-models"
    checkout_model_file="$checkout_model_path/$HTDEMUCS_MODEL_NAME"
    external_model_file="$external_model_path/$HTDEMUCS_MODEL_NAME"
    non_regular_model_path="$test_dir/non-regular-models"
    ln -s -- "$checkout_model_path" "$checkout_model_symlink"
    if validate_model_staging_destination "$checkout_model_path" >/dev/null ||
            validate_model_staging_destination "$normalized_checkout_model_path" >/dev/null ||
            validate_model_staging_destination "$checkout_model_symlink" >/dev/null; then
        echo "Self-test failed: checkout-contained model staging path was accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if [[ "$(validate_model_staging_destination "$external_model_path")" != \
            "$external_model_path" ]]; then
        echo "Self-test failed: external model staging path was not canonicalized" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    mkdir -p -- "$external_model_path"
    ln -s -- "$checkout_model_file" "$external_model_file"
    if validate_model_staging_destination "$external_model_path" >/dev/null; then
        echo "Self-test failed: symlinked final model destination was accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    rm -- "$external_model_file"
    mkdir -p -- "$non_regular_model_path/$HTDEMUCS_MODEL_NAME"
    if validate_model_staging_destination "$non_regular_model_path" >/dev/null; then
        echo "Self-test failed: non-regular final model destination was accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi

    # Replace the validated pathname after the script has entered the staging
    # directory. All temporary and final writes must stay in the directory
    # that was entered, even though its pathname now points at the checkout.
    race_model_path="$test_dir/race-models"
    race_model_backup_path="$test_dir/race-models-original"
    mkdir -p -- "$race_model_path"
    race_payload_path="$test_dir/race-payload"
    printf '%s\n' 'race-model' > "$race_payload_path"
    race_payload_size="$(stat -c '%s' -- "$race_payload_path")"
    race_payload_sha256="$(sha256sum "$race_payload_path")"
    race_payload_sha256="${race_payload_sha256%% *}"
    checkout_model_before="$(sha256sum "$checkout_model_file")"

    sudo_path="$fake_path/sudo"
    printf '%s\n' \
        '#!/bin/sh' \
        'set -eu' \
        "if [ \"\$1\" = \"--preserve-fds=9\" ]; then shift; fi" \
        "[ \"\$1\" = \"-u\" ]" \
        'shift 2' \
        'exec "$@"' > "$sudo_path"
    chmod +x "$sudo_path"
    wget_path="$fake_path/wget"
    # The generated fixture intentionally contains literal shell variables.
    # shellcheck disable=SC2016
    printf '%s\n' \
        '#!/bin/sh' \
        'set -eu' \
        'current_temp_directory=$(pwd -P)' \
        'if [ "${FAKE_WGET_REPLACE_TEMP:-0}" -eq 1 ]; then' \
        '    mv -- "$current_temp_directory" "${FAKE_WGET_TEMP_BACKUP:?}"' \
        '    ln -s -- "${FAKE_WGET_TEMP_TARGET:?}" "$current_temp_directory"' \
        'fi' \
        'if [ "${FAKE_WGET_REPLACE_FINAL:-0}" -eq 1 ]; then' \
        '    ln -s -- "${FAKE_WGET_FINAL_TARGET:?}" "${FAKE_WGET_FINAL_PATH:?}"' \
        'fi' \
        'printf "%s\\n" "${FAKE_WGET_CONTENT:?wget content was not provided}"' > "$wget_path"
    chmod +x "$wget_path"

    if ! (
        enter_model_staging_directory "$race_model_path" "$USER"
        mv -- "$race_model_path" "$race_model_backup_path"
        ln -s -- "$checkout_model_path" "$race_model_path"
        PATH="$fake_path:$original_path"
        export PATH
        FAKE_WGET_CONTENT='race-model'
        export FAKE_WGET_CONTENT
        download_verified_file \
            "$USER" \
            "$HTDEMUCS_MODEL_URL" \
            "$race_payload_size" \
            "$race_payload_sha256" \
            "$HTDEMUCS_MODEL_NAME"
    ); then
        echo "Self-test failed: staging directory replacement race was not handled safely" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if [[ ! -L "$race_model_path" ]] ||
            [[ "$(sha256sum "$checkout_model_file")" != "$checkout_model_before" ]] ||
            ! grep -Fqx 'race-model' "$race_model_backup_path/$HTDEMUCS_MODEL_NAME"; then
        echo "Self-test failed: staging directory replacement race modified the checkout" >&2
        rm -rf -- "$test_dir"
        return 1
    fi

    temporary_race_model_path="$test_dir/temporary-race-models"
    temporary_race_backup_path="$test_dir/temporary-race-models-original"
    mkdir -p -- "$temporary_race_model_path"
    if ! (
        enter_model_staging_directory "$temporary_race_model_path" "$USER"
        PATH="$fake_path:$original_path"
        export PATH
        FAKE_WGET_CONTENT='race-model'
        export FAKE_WGET_CONTENT
        FAKE_WGET_REPLACE_TEMP=1
        export FAKE_WGET_REPLACE_TEMP
        FAKE_WGET_TEMP_BACKUP="$temporary_race_backup_path"
        export FAKE_WGET_TEMP_BACKUP
        FAKE_WGET_TEMP_TARGET="$checkout_model_path"
        export FAKE_WGET_TEMP_TARGET
        download_verified_file \
            "$USER" \
            "$HTDEMUCS_MODEL_URL" \
            "$race_payload_size" \
            "$race_payload_sha256" \
            "$HTDEMUCS_MODEL_NAME"
    ); then
        echo "Self-test failed: temporary download pathname race was not handled safely" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if [[ "$(sha256sum "$checkout_model_file")" != "$checkout_model_before" ]] ||
            ! grep -Fqx 'race-model' \
                "$temporary_race_model_path/$HTDEMUCS_MODEL_NAME"; then
        echo "Self-test failed: temporary download pathname race modified the checkout" >&2
        rm -rf -- "$test_dir"
        return 1
    fi

    final_race_model_path="$test_dir/final-race-models"
    mkdir -p -- "$final_race_model_path"
    if (
        enter_model_staging_directory "$final_race_model_path" "$USER"
        PATH="$fake_path:$original_path"
        export PATH
        FAKE_WGET_CONTENT='race-model'
        export FAKE_WGET_CONTENT
        FAKE_WGET_REPLACE_FINAL=1
        export FAKE_WGET_REPLACE_FINAL
        FAKE_WGET_FINAL_PATH="$final_race_model_path/$HTDEMUCS_MODEL_NAME"
        export FAKE_WGET_FINAL_PATH
        FAKE_WGET_FINAL_TARGET="$checkout_model_path"
        export FAKE_WGET_FINAL_TARGET
        download_verified_file \
            "$USER" \
            "$HTDEMUCS_MODEL_URL" \
            "$race_payload_size" \
            "$race_payload_sha256" \
            "$HTDEMUCS_MODEL_NAME"
    ); then
        echo "Self-test failed: final destination symlink race was accepted" >&2
        rm -rf -- "$test_dir"
        return 1
    fi
    if [[ ! -L "$final_race_model_path/$HTDEMUCS_MODEL_NAME" ]] ||
            [[ "$(sha256sum "$checkout_model_file")" != "$checkout_model_before" ]] ||
            [[ -e "$final_race_model_path/$HTDEMUCS_MODEL_NAME" &&
                    ! -L "$final_race_model_path/$HTDEMUCS_MODEL_NAME" ]]; then
        echo "Self-test failed: final destination symlink race was not rejected safely" >&2
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

        if ! sudo apt-get update --error-on=any; then
            echo "Failed to update signed APT repository metadata; aborting setup." >&2
            exit 1
        fi

        if ! dpkg_query_path="$(find_dpkg_query)"; then
            print_dpkg_query_unavailable
            exit 1
        fi

        # If jackd2 is installed as per dpkg database, install libjack-jackd2-dev.
        # This avoids a package deadlock, resulting in jackd2 being removed, and jackd1 being installed,
        # to satisfy portaudio19-dev's need for a jackd dev package. In short, portaudio19-dev needs a
        # jackd dev library, so let's give it one..
        if [ "$("$dpkg_query_path" -W -f="\${Status}" jackd2 2>/dev/null |
                grep -c "ok installed")" -eq 1 ];
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
            if [[ -n "${MIXXX_ONNX_RUNTIME_PREFIX:-}" ]]; then
                if ! "$ONNXRUNTIME_HELPER" verify "$MIXXX_ONNX_RUNTIME_PREFIX"; then
                    echo "The externally staged ONNX Runtime is missing or incomplete; aborting setup." >&2
                    exit 1
                fi
                echo "Using the externally staged ONNX Runtime at $MIXXX_ONNX_RUNTIME_PREFIX."
            else
                PACKAGES_EXTRA+=(libonnxruntime-dev)
            fi
            PACKAGES_EXTRA+=(
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
            # htdemucs_ft.onnx is deliberately disabled. Its release URL is
            # unavailable and no authoritative digest exists. Do not add it
            # until a maintainer publishes a verifiable artifact and digest.
            MODEL_FILES=("$HTDEMUCS_MODEL_NAME")

            echo "The unsupported htdemucs_ft.onnx model is intentionally disabled."
            echo "Maintainer action: publish a verifiable artifact and digest before re-enabling it."

            if ! (
                if ! enter_model_staging_directory "$MODEL_PATH" "$ACTUAL_USER"; then
                    echo "Refusing to stage Mixxx HTDemucs models at $MODEL_PATH" >&2
                    exit 1
                fi
                MODEL_PATH="$(pwd -P)"

                for MODEL_NAME in "${MODEL_FILES[@]}"; do
                    MODEL_FILE="$MODEL_NAME"
                    if ! MODEL_SHA256="$(get_model_sha256 "$MODEL_NAME")"; then
                        echo "Unsupported model $MODEL_NAME has no trusted digest; aborting." >&2
                        exit 1
                    fi
                    if ! download_verified_file \
                            "$ACTUAL_USER" \
                            "$HTDEMUCS_MODEL_URL" \
                            "$HTDEMUCS_MODEL_SIZE" \
                            "$MODEL_SHA256" \
                            "$MODEL_FILE"; then
                        echo "Failed to download Mixxx HTDemucs model $MODEL_NAME"
                        exit 1
                    fi
                    echo "Verified model downloaded successfully to $MODEL_PATH/$MODEL_NAME"
                done
            ); then
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
