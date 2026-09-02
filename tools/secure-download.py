#!/usr/bin/env python3
"""Download an artifact into a regular file and optionally install it."""

import ctypes
import errno
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
    print(
        "         --install-fd FD --install-cwd --install-name NAME",
        file=sys.stderr,
    )


def parse_arguments() -> tuple[
    str,
    list[str],
    int | None,
    str | None,
    int | None,
    bool,
    str | None,
]:
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
    install_cwd = False
    install_name = None
    option_index = 0
    while option_index < len(option_arguments):
        option = option_arguments[option_index]
        if option == "--install-cwd":
            install_cwd = True
            option_index += 1
            continue
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

    if (
        not command
        or (not install_cwd and (install_fd is None) != (install_name is None))
        or (install_fd is not None and install_cwd)
        or (install_cwd and install_name is None)
        or (install_cwd and output_path != "-")
        or (output_path == "-" and not install_cwd)
    ):
        usage()
        raise SystemExit(2)
    return (
        output_path,
        command,
        verify_size,
        verify_sha256,
        install_fd,
        install_cwd,
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
    """Create a no-replace directory entry without reopening source_fd."""
    if sys.platform != "linux":
        raise OSError(
            "secure no-replace descriptor publication is only supported "
            "on Linux"
        )

    libc = ctypes.CDLL(None, use_errno=True)
    try:
        linkat = libc.linkat
    except AttributeError as error:
        raise OSError(
            errno.ENOSYS,
            "secure no-replace descriptor publication primitive is "
            "unavailable",
        ) from error
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


def publish_no_replace_from_file_descriptor(
    source_fd: int, destination_fd: int, destination_name: str
) -> None:
    """Publish source_fd atomically without replacing destination_name."""
    try:
        link_from_file_descriptor(source_fd, destination_fd, destination_name)
    except OSError as error:
        unsupported_errors = {
            errno.EINVAL,
            errno.ENOSYS,
            errno.EOPNOTSUPP,
        }
        if hasattr(errno, "ENOTSUP"):
            unsupported_errors.add(errno.ENOTSUP)
        if error.errno in unsupported_errors or error.errno is None:
            raise OSError(
                error.errno or errno.ENOSYS,
                "secure no-replace descriptor publication is unavailable; "
                "refusing to replace the destination",
            ) from error
        raise


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

    verify_file_descriptor(source_fd, expected_size, expected_sha256)
    source_stat = os.fstat(source_fd)
    existing_fd = None
    try:
        try:
            existing_stat = os.stat(
                destination_name, dir_fd=destination_fd, follow_symlinks=False
            )
        except FileNotFoundError:
            existing_stat = None

        if existing_stat is not None:
            if stat.S_ISLNK(existing_stat.st_mode):
                raise OSError(
                    "secure install destination must not be a symlink"
                )
            if not stat.S_ISREG(existing_stat.st_mode):
                raise OSError(
                    "secure install destination is not a regular file"
                )
            if existing_stat.st_nlink != 1:
                raise OSError(
                    "secure install destination has multiple hard links"
                )
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
            if expected_size is None or expected_sha256 is None:
                raise OSError(
                    "secure install requires expected size and SHA-256 to "
                    "inspect an existing destination"
                )
            verify_file_descriptor(existing_fd, expected_size, expected_sha256)
            current_existing_stat = os.stat(
                destination_name,
                dir_fd=destination_fd,
                follow_symlinks=False,
            )
            if (
                not stat.S_ISREG(current_existing_stat.st_mode)
                or current_existing_stat.st_dev != existing_fd_stat.st_dev
                or current_existing_stat.st_ino != existing_fd_stat.st_ino
                or current_existing_stat.st_nlink != 1
            ):
                raise OSError(
                    "secure install destination changed during install"
                )
            return

        try:
            publish_no_replace_from_file_descriptor(
                source_fd, destination_fd, destination_name
            )
        except OSError as error:
            if error.errno != errno.EEXIST:
                raise
            # A concurrent invocation may have won publication. Accept only a
            # verified regular file that is the exact source inode. A named
            # source remains linked at its pathname after publication, so a
            # legitimate winner may have two links; matching bytes alone is
            # not enough to accept an unrelated hard link.
            try:
                winner_stat = os.stat(
                    destination_name,
                    dir_fd=destination_fd,
                    follow_symlinks=False,
                )
            except FileNotFoundError as missing_error:
                raise OSError(
                    "secure install destination changed during publication"
                ) from missing_error
            if (
                stat.S_ISLNK(winner_stat.st_mode)
                or not stat.S_ISREG(winner_stat.st_mode)
                or winner_stat.st_dev != source_stat.st_dev
                or winner_stat.st_ino != source_stat.st_ino
            ):
                raise OSError(
                    "secure install destination appeared during publication"
                ) from error
            try:
                winner_fd = os.open(
                    destination_name,
                    os.O_RDONLY | os.O_NOFOLLOW,
                    dir_fd=destination_fd,
                )
            except OSError as open_error:
                raise OSError(
                    "secure install destination changed during publication"
                ) from open_error
            try:
                winner_fd_stat = os.fstat(winner_fd)
                if (
                    winner_fd_stat.st_dev != winner_stat.st_dev
                    or winner_fd_stat.st_ino != winner_stat.st_ino
                    or winner_fd_stat.st_dev != source_stat.st_dev
                    or winner_fd_stat.st_ino != source_stat.st_ino
                ):
                    raise OSError(
                        "secure install destination changed during publication"
                    )
                verify_file_descriptor(
                    winner_fd, expected_size, expected_sha256
                )
                current_winner_stat = os.stat(
                    destination_name,
                    dir_fd=destination_fd,
                    follow_symlinks=False,
                )
                if (
                    current_winner_stat.st_dev != winner_fd_stat.st_dev
                    or current_winner_stat.st_ino != winner_fd_stat.st_ino
                    or current_winner_stat.st_dev != source_stat.st_dev
                    or current_winner_stat.st_ino != source_stat.st_ino
                ):
                    raise OSError(
                        "secure install destination changed during publication"
                    )
            finally:
                os.close(winner_fd)
            return

        try:
            published_stat = os.stat(
                destination_name,
                dir_fd=destination_fd,
                follow_symlinks=False,
            )
        except FileNotFoundError as missing_error:
            raise OSError(
                "secure install destination changed during publication"
            ) from missing_error
        if (
            not stat.S_ISREG(published_stat.st_mode)
            or published_stat.st_dev != source_stat.st_dev
            or published_stat.st_ino != source_stat.st_ino
        ):
            raise OSError(
                "secure install destination changed during publication"
            )
        os.fsync(destination_fd)
    finally:
        if existing_fd is not None:
            os.close(existing_fd)


def create_secure_temporary_file(directory_fd: int) -> int:
    if sys.platform != "linux" or not hasattr(os, "O_TMPFILE"):
        raise OSError(
            errno.ENOSYS,
            "secure anonymous temporary file primitive is unavailable; "
            "refusing to install the download",
        )
    try:
        return os.open(
            ".",
            os.O_RDWR | os.O_TMPFILE | os.O_NOFOLLOW,
            0o600,
            dir_fd=directory_fd,
        )
    except OSError as error:
        unsupported_errors = {
            errno.EINVAL,
            errno.ENOSYS,
            errno.EOPNOTSUPP,
        }
        if hasattr(errno, "ENOTSUP"):
            unsupported_errors.add(errno.ENOTSUP)
        if error.errno in unsupported_errors:
            raise OSError(
                error.errno,
                "secure anonymous temporary file primitive is unavailable; "
                "refusing to install the download",
            ) from error
        raise


def main() -> int:
    (
        output_path,
        command,
        verify_size,
        verify_sha256,
        install_fd,
        install_cwd,
        install_name,
    ) = parse_arguments()
    flags = os.O_RDWR | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    output_fd = None
    output_directory_fd = None
    install_directory_fd = install_fd
    try:
        if install_cwd:
            output_directory_fd = os.open(
                ".",
                os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
            )
            install_directory_fd = output_directory_fd

        if output_path == "-":
            assert output_directory_fd is not None
            output_fd = create_secure_temporary_file(output_directory_fd)
        else:
            output_fd = os.open(output_path, flags, 0o600)
    except OSError as error:
        if output_fd is not None:
            os.close(output_fd)
        if output_directory_fd is not None:
            os.close(output_directory_fd)
        if output_path == "-":
            output_description = "the current directory"
        else:
            output_description = output_path
        print(
            f"Could not create secure download file {output_description}: "
            f"{error}",
            file=sys.stderr,
        )
        return 1

    assert output_fd is not None

    try:
        try:
            return_code = subprocess.run(
                command, stdout=output_fd, check=False
            ).returncode
        except OSError as error:
            print(
                f"Could not execute downloader {command[0]}: {error}",
                file=sys.stderr,
            )
            return 127

        if return_code != 0:
            return return_code

        try:
            os.fsync(output_fd)
            verify_file_descriptor(output_fd, verify_size, verify_sha256)
            if install_directory_fd is not None and install_name is not None:
                install_from_file_descriptor(
                    output_fd,
                    install_directory_fd,
                    install_name,
                    verify_size,
                    verify_sha256,
                )
        except OSError as error:
            print(
                f"Secure download verification/install failed: {error}",
                file=sys.stderr,
            )
            return 1
        return 0
    finally:
        os.close(output_fd)
        if output_directory_fd is not None:
            os.close(output_directory_fd)


if __name__ == "__main__":
    raise SystemExit(main())
