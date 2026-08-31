#!/usr/bin/env python3
"""Download an artifact into a regular file and optionally install it."""

import ctypes
import errno
import hashlib
import os
import secrets
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


def link_from_file_descriptor(
    source_fd: int, destination_fd: int, destination_name: str
) -> None:
    """Create a directory entry for source_fd without reopening its path."""
    if sys.platform != "linux":
        raise OSError(
            "secure descriptor installation is only supported on Linux"
        )

    libc = ctypes.CDLL(None, use_errno=True)
    linkat = libc.linkat
    linkat.argtypes = [
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
        ctypes.c_char_p,
        ctypes.c_int,
    ]
    linkat.restype = ctypes.c_int
    if (
        linkat(
            source_fd,
            b"",
            destination_fd,
            os.fsencode(destination_name),
            0x1000,  # Linux AT_EMPTY_PATH
        )
        != 0
    ):
        error_number = ctypes.get_errno()
        raise OSError(error_number, os.strerror(error_number))


def link_from_file_descriptor_with_unique_name(
    source_fd: int, destination_fd: int, prefix: str
) -> str:
    for _ in range(100):
        candidate_name = (
            f".secure-download.{prefix}.{os.getpid()}.{secrets.token_hex(16)}"
        )
        try:
            link_from_file_descriptor(
                source_fd, destination_fd, candidate_name
            )
        except OSError as error:
            if error.errno == errno.EEXIST:
                continue
            raise
        return candidate_name
    raise OSError("could not create secure install backup link")


def unlink_temporary_name_if_owned(
    destination_fd: int,
    temporary_name: str,
    temporary_stat: os.stat_result,
) -> None:
    try:
        path_stat = os.stat(
            temporary_name, dir_fd=destination_fd, follow_symlinks=False
        )
    except FileNotFoundError:
        return
    if (
        stat.S_ISREG(path_stat.st_mode)
        and path_stat.st_dev == temporary_stat.st_dev
        and path_stat.st_ino == temporary_stat.st_ino
    ):
        try:
            os.unlink(temporary_name, dir_fd=destination_fd)
        except FileNotFoundError:
            pass


def restore_existing_destination(
    existing_fd: int,
    destination_fd: int,
    destination_name: str,
    existing_backup_name: str,
    existing_fd_stat: os.stat_result,
    installed_stat: os.stat_result,
) -> bool:
    try:
        current_stat = os.stat(
            destination_name, dir_fd=destination_fd, follow_symlinks=False
        )
    except FileNotFoundError:
        current_stat = None

    if current_stat is not None:
        if (
            stat.S_ISREG(current_stat.st_mode)
            and current_stat.st_dev == existing_fd_stat.st_dev
            and current_stat.st_ino == existing_fd_stat.st_ino
        ):
            unlink_temporary_name_if_owned(
                destination_fd, existing_backup_name, existing_fd_stat
            )
            return True
        if (
            not stat.S_ISREG(current_stat.st_mode)
            or current_stat.st_dev != installed_stat.st_dev
            or current_stat.st_ino != installed_stat.st_ino
            or current_stat.st_nlink != 1
        ):
            # Do not remove or replace an entry that was not created by this
            # install. Leave the old artifact linked by existing_backup_name.
            return False
        os.unlink(destination_name, dir_fd=destination_fd)

    link_from_file_descriptor(existing_fd, destination_fd, destination_name)
    unlink_temporary_name_if_owned(
        destination_fd, existing_backup_name, existing_fd_stat
    )
    return True


def install_from_file_descriptor(
    source_fd: int,
    destination_fd: int,
    destination_name: str,
    expected_size: int | None = None,
    expected_sha256: str | None = None,
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

    try:
        existing_stat = os.stat(
            destination_name, dir_fd=destination_fd, follow_symlinks=False
        )
    except FileNotFoundError:
        existing_stat = None

    if existing_stat is not None:
        if stat.S_ISLNK(existing_stat.st_mode):
            raise OSError("secure install destination must not be a symlink")
        if not stat.S_ISREG(existing_stat.st_mode):
            raise OSError("secure install destination is not a regular file")
        if existing_stat.st_nlink != 1:
            raise OSError("secure install destination has multiple hard links")

    temporary_name = None
    installed_fd = None
    for _ in range(100):
        candidate_name = (
            f".secure-download.{os.getpid()}.{secrets.token_hex(16)}"
        )
        try:
            installed_fd = os.open(
                candidate_name,
                os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                0o600,
                dir_fd=destination_fd,
            )
        except FileExistsError:
            continue
        temporary_name = candidate_name
        break
    if installed_fd is None or temporary_name is None:
        raise OSError("could not create secure install temporary file")

    existing_fd = None
    existing_fd_stat = None
    existing_backup_name = None
    temporary_stat = os.fstat(installed_fd)
    try:
        if existing_stat is not None:
            try:
                existing_fd = os.open(
                    destination_name,
                    os.O_RDONLY | os.O_NOFOLLOW,
                    dir_fd=destination_fd,
                )
            except OSError as error:
                raise OSError(
                    "secure install destination changed during install"
                ) from error
            existing_fd_stat = os.fstat(existing_fd)
            if (
                not stat.S_ISREG(existing_fd_stat.st_mode)
                or existing_fd_stat.st_dev != existing_stat.st_dev
                or existing_fd_stat.st_ino != existing_stat.st_ino
                or existing_fd_stat.st_nlink != 1
            ):
                raise OSError(
                    "secure install destination changed during install"
                )

        os.lseek(source_fd, 0, os.SEEK_SET)
        while chunk := os.read(source_fd, 1024 * 1024):
            chunk_offset = 0
            while chunk_offset < len(chunk):
                chunk_offset += os.write(installed_fd, chunk[chunk_offset:])
        temporary_stat = os.fstat(installed_fd)
        if (
            not stat.S_ISREG(temporary_stat.st_mode)
            or temporary_stat.st_nlink != 1
        ):
            raise OSError(
                "secure install temporary file changed during install"
            )
        os.fsync(installed_fd)
        verify_file_descriptor(installed_fd, expected_size, expected_sha256)
        temporary_final_stat = os.fstat(installed_fd)
        if (
            not stat.S_ISREG(temporary_final_stat.st_mode)
            or temporary_final_stat.st_dev != temporary_stat.st_dev
            or temporary_final_stat.st_ino != temporary_stat.st_ino
            or temporary_final_stat.st_nlink > 1
        ):
            raise OSError(
                "secure install temporary file changed during install"
            )

        # Remove the old entry without following it, then create the new entry
        # directly from the held descriptor. A pathname-based rename would let
        # a concurrent process replace temporary_name after the descriptor was
        # verified and before the rename reopened that pathname.
        if existing_fd is not None:
            try:
                current_existing_stat = os.stat(
                    destination_name,
                    dir_fd=destination_fd,
                    follow_symlinks=False,
                )
            except FileNotFoundError as error:
                raise OSError(
                    "secure install destination changed during install"
                ) from error
            if (
                not stat.S_ISREG(current_existing_stat.st_mode)
                or current_existing_stat.st_dev != existing_fd_stat.st_dev
                or current_existing_stat.st_ino != existing_fd_stat.st_ino
                or current_existing_stat.st_nlink != 1
            ):
                raise OSError(
                    "secure install destination changed during install"
                )
            # Keep a descriptor-bound link to the old artifact while replacing
            # the destination. This lets a failed linkat restore the old file
            # without relying on linking an already-unlinked inode.
            existing_backup_name = link_from_file_descriptor_with_unique_name(
                existing_fd, destination_fd, "existing"
            )
            os.unlink(destination_name, dir_fd=destination_fd)
        link_from_file_descriptor(
            installed_fd, destination_fd, destination_name
        )
        unlink_temporary_name_if_owned(
            destination_fd, temporary_name, temporary_stat
        )
        installed_path_stat = os.stat(
            destination_name, dir_fd=destination_fd, follow_symlinks=False
        )
        if (
            not stat.S_ISREG(installed_path_stat.st_mode)
            or installed_path_stat.st_dev != temporary_stat.st_dev
            or installed_path_stat.st_ino != temporary_stat.st_ino
            or installed_path_stat.st_nlink != 1
        ):
            raise OSError("secure install destination changed during install")
        os.fsync(destination_fd)
        if existing_backup_name is not None:
            unlink_temporary_name_if_owned(
                destination_fd, existing_backup_name, existing_fd_stat
            )
            existing_backup_name = None
    except OSError:
        if existing_backup_name is not None:
            try:
                if restore_existing_destination(
                    existing_fd,
                    destination_fd,
                    destination_name,
                    existing_backup_name,
                    existing_fd_stat,
                    temporary_stat,
                ):
                    existing_backup_name = None
            except OSError as restore_error:
                raise OSError(
                    "secure install failed and the existing destination "
                    "could not be preserved"
                ) from restore_error
        raise
    finally:
        # Retain an unverified rollback link if a competing path prevented a
        # safe restoration. The old artifact is then still recoverable by name.
        if existing_fd is not None:
            os.close(existing_fd)
        os.close(installed_fd)
        if temporary_stat is not None:
            unlink_temporary_name_if_owned(
                destination_fd, temporary_name, temporary_stat
            )


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
            install_from_file_descriptor(
                output_fd,
                install_fd,
                install_name,
                verify_size,
                verify_sha256,
            )
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
