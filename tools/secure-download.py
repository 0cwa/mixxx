#!/usr/bin/env python3
"""Download an artifact into a regular file and optionally install it."""

import hashlib
import os
import stat
import subprocess
import sys


def usage() -> None:
    print(
        f"Usage: {sys.argv[0]} OUTPUT_PATH [OPTIONS] [-- COMMAND ...]",
        file=sys.stderr,
    )
    print(
        "Options: --verify-size BYTES --verify-sha256 SHA256", file=sys.stderr
    )
    print("         --install-fd FD --install-name NAME", file=sys.stderr)


def parse_arguments() -> (
    tuple[str, list[str], int | None, str | None, int | None, str | None]
):
    arguments = sys.argv[1:]
    if len(arguments) < 2:
        usage()
        raise SystemExit(2)

    output_path = arguments[0]
    option_arguments = arguments[1:]
    command = option_arguments
    if "--" in option_arguments:
        separator_index = option_arguments.index("--")
        option_arguments = option_arguments[:separator_index]
        command = command[separator_index + 1 :]

    verify_size = None
    verify_sha256 = None
    install_fd = None
    install_name = None
    option_index = 0
    while option_index < len(option_arguments):
        option = option_arguments[option_index]
        if option in (
            "--verify-size",
            "--verify-sha256",
            "--install-fd",
            "--install-name",
        ):
            if option_index + 1 >= len(option_arguments):
                usage()
                raise SystemExit(2)
            value = option_arguments[option_index + 1]
            option_index += 2
        elif option.startswith("--verify-size="):
            option = "--verify-size"
            value = option_arguments[option_index][len(option) + 1 :]
            option_index += 1
        elif option.startswith("--verify-sha256="):
            option = "--verify-sha256"
            value = option_arguments[option_index][len(option) + 1 :]
            option_index += 1
        elif option.startswith("--install-fd="):
            option = "--install-fd"
            value = option_arguments[option_index][len(option) + 1 :]
            option_index += 1
        elif option.startswith("--install-name="):
            option = "--install-name"
            value = option_arguments[option_index][len(option) + 1 :]
            option_index += 1
        else:
            usage()
            raise SystemExit(2)

        if option == "--verify-size":
            try:
                verify_size = int(value)
            except ValueError:
                usage()
                raise SystemExit(2) from None
            if verify_size < 0:
                usage()
                raise SystemExit(2)
        elif option == "--verify-sha256":
            if len(value) != hashlib.sha256().digest_size * 2 or any(
                character not in "0123456789abcdefABCDEF"
                for character in value
            ):
                usage()
                raise SystemExit(2)
            verify_sha256 = value.lower()
        elif option == "--install-fd":
            try:
                install_fd = int(value)
            except ValueError:
                usage()
                raise SystemExit(2) from None
            if install_fd < 0:
                usage()
                raise SystemExit(2)
        elif option == "--install-name":
            install_name = value

    if not command or (install_fd is None) != (install_name is None):
        usage()
        raise SystemExit(2)
    return (
        output_path,
        command,
        verify_size,
        verify_sha256,
        install_fd,
        install_name,
    )


def verify_file_descriptor(
    file_descriptor: int,
    expected_size: int | None,
    expected_sha256: str | None,
) -> None:
    file_stat = os.fstat(file_descriptor)
    if not stat.S_ISREG(file_stat.st_mode):
        raise OSError("download output is not a regular file")
    if expected_size is not None and file_stat.st_size != expected_size:
        raise OSError(
            f"download output has size {file_stat.st_size}; "
            f"expected {expected_size}"
        )

    if expected_sha256 is not None:
        os.lseek(file_descriptor, 0, os.SEEK_SET)
        digest = hashlib.sha256()
        while chunk := os.read(file_descriptor, 1024 * 1024):
            digest.update(chunk)
        if digest.hexdigest() != expected_sha256:
            raise OSError(
                "download output SHA-256 does not match the expected value"
            )
        os.lseek(file_descriptor, 0, os.SEEK_SET)


def install_from_file_descriptor(
    source_fd: int, destination_fd: int, destination_name: str
) -> None:
    if (
        not destination_name
        or os.path.basename(destination_name) != destination_name
        or destination_name in (".", "..")
    ):
        raise OSError("secure install destination must be a file name")
    if not stat.S_ISDIR(os.fstat(destination_fd).st_mode):
        raise OSError(
            "secure install destination descriptor is not a directory"
        )

    destination_flags = os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW
    try:
        installed_fd = os.open(
            destination_name, destination_flags, 0o600, dir_fd=destination_fd
        )
    except OSError as error:
        raise OSError(
            f"could not open secure install destination {destination_name}: "
            f"{error}"
        ) from error

    try:
        installed_stat = os.fstat(installed_fd)
        if not stat.S_ISREG(installed_stat.st_mode):
            raise OSError("secure install destination is not a regular file")
        if installed_stat.st_nlink != 1:
            raise OSError("secure install destination has multiple hard links")
        os.ftruncate(installed_fd, 0)
        os.lseek(source_fd, 0, os.SEEK_SET)
        while chunk := os.read(source_fd, 1024 * 1024):
            chunk_offset = 0
            while chunk_offset < len(chunk):
                chunk_offset += os.write(installed_fd, chunk[chunk_offset:])
        os.fsync(installed_fd)
        installed_path_stat = os.stat(
            destination_name, dir_fd=destination_fd, follow_symlinks=False
        )
        if (
            not stat.S_ISREG(installed_path_stat.st_mode)
            or installed_path_stat.st_dev != installed_stat.st_dev
            or installed_path_stat.st_ino != installed_stat.st_ino
        ):
            raise OSError("secure install destination changed during install")
    finally:
        os.close(installed_fd)


def main() -> int:
    (
        output_path,
        command,
        verify_size,
        verify_sha256,
        install_fd,
        install_name,
    ) = parse_arguments()
    flags = os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    try:
        output_fd = os.open(output_path, flags, 0o600)
    except OSError as error:
        print(
            f"Could not create secure download file {output_path}: {error}",
            file=sys.stderr,
        )
        return 1

    try:
        return_code = subprocess.run(
            command, stdout=output_fd, check=False
        ).returncode
    except OSError as error:
        print(
            f"Could not execute downloader {command[0]}: {error}",
            file=sys.stderr,
        )
        os.close(output_fd)
        return 127

    if return_code != 0:
        os.close(output_fd)
        return return_code

    try:
        verify_file_descriptor(output_fd, verify_size, verify_sha256)
        if install_fd is not None and install_name is not None:
            install_from_file_descriptor(output_fd, install_fd, install_name)
    except OSError as error:
        print(
            f"Secure download verification/install failed: {error}",
            file=sys.stderr,
        )
        os.close(output_fd)
        return 1

    os.close(output_fd)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
