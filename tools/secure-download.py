#!/usr/bin/env python3
"""Run a downloader with stdout bound to a newly-created regular file."""

import os
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(
            f"Usage: {sys.argv[0]} OUTPUT_PATH COMMAND [ARGUMENT ...]",
            file=sys.stderr,
        )
        return 2

    output_path = sys.argv[1]
    command = sys.argv[2:]
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW
    try:
        output_fd = os.open(output_path, flags, 0o600)
    except OSError as error:
        print(
            f"Could not create secure download file {output_path}: {error}",
            file=sys.stderr,
        )
        return 1

    stdout_fd = sys.stdout.fileno()
    if output_fd != stdout_fd:
        os.dup2(output_fd, stdout_fd)
        os.close(output_fd)

    try:
        os.execvp(command[0], command)
    except OSError as error:
        print(
            f"Could not execute downloader {command[0]}: {error}",
            file=sys.stderr,
        )
        return 127

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
