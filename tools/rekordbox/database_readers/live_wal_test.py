# Prepared by ChatGPT for human review.
"""Qualify snapshots while another process holds a live SQLite WAL."""

import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def manifest(directory):
    result = {}
    for path in [directory, *sorted(directory.rglob("*"))]:
        status = path.lstat()
        require(not path.is_symlink(), "Unexpected fixture symlink")
        record = {
            "mode": status.st_mode,
            "mtime_ns": status.st_mtime_ns,
            "inode": status.st_ino,
        }
        if path.is_file():
            record["bytes"] = status.st_size
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            record["sha256"] = digest.hexdigest()
        else:
            require(path.is_dir(), "Unexpected fixture special file")
        result[path.relative_to(directory).as_posix()] = record
    return result


def summary(records):
    encoded = json.dumps(
        records, sort_keys=True, separators=(",", ":")
    ).encode()
    return {
        "entries": len(records),
        "sha256": hashlib.sha256(encoded).hexdigest(),
    }


def wait_event(process, timeout):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        require(selector.select(timeout), "Snapshot handshake timed out")
    line = process.stdout.readline()
    require(line, "Snapshot process ended before its handshake")
    return json.loads(line)


def inspect_trace(path, source_directory):
    source = str(source_directory) + "/"
    writes = []
    shm_locks = 0
    source_opens = 0
    for line in path.read_text().splitlines():
        if source not in line:
            continue
        if "openat(" in line or "open(" in line:
            source_opens += 1
            if "O_CREAT" in line or "O_TRUNC" in line:
                writes.append(line)
        descriptor_write = re.search(
            r"\b(?:write|writev|pwrite64|pwritev|pwritev2|ftruncate|fallocate)"
            r"\(\d+<([^>]+)>",
            line,
        )
        if descriptor_write and descriptor_write.group(1).startswith(source):
            writes.append(line)
        if re.search(
            r"\b(?:truncate|unlink|unlinkat|rename|renameat|renameat2)\(", line
        ):
            writes.append(line)
        if "mmap(" in line and "PROT_WRITE" in line and "MAP_SHARED" in line:
            writes.append(line)
        if "fcntl(" in line and "-shm>" in line and "F_SETLK" in line:
            shm_locks += 1
    require(
        not writes,
        "Importer attempted source mutation in syscall trace:\n"
        + "\n".join(writes),
    )
    require(
        source_opens > 0 and shm_locks > 0,
        "Trace did not observe source opens and interprocess SHM locks",
    )
    return {
        "source_open_calls": source_opens,
        "source_shm_lock_calls": shm_locks,
        "source_mutation_calls": 0,
    }


def run(driver, source, private, expected_count, expected_epoch):
    before = manifest(source.parent)
    completed = subprocess.run(
        [str(driver), str(source), str(private)],
        capture_output=True,
        text=True,
        timeout=20,
    )
    require(
        completed.returncode == 0, completed.stderr or "Snapshot driver failed"
    )
    record = json.loads(completed.stdout)
    require(
        record["importer_pid"] != os.getpid(),
        "Writer and importer share a process",
    )
    require(
        record["count"] == expected_count
        and record["epoch"] == expected_epoch,
        "Snapshot omitted committed WAL rows or observed an "
        "uncommitted generation",
    )
    require(
        before == manifest(source.parent),
        "Importer changed a paused writer's source tree",
    )
    record["source_manifest_unchanged"] = summary(before)
    return record


def main():
    require(
        len(sys.argv) == 3, "Usage: live_wal_test.py DRIVER WORK_DIRECTORY"
    )
    driver = Path(sys.argv[1]).resolve()
    work = Path(sys.argv[2]).resolve()
    work.mkdir(parents=True, exist_ok=True)
    case = Path(tempfile.mkdtemp(prefix="cross-process-", dir=work))
    source_directory = case / "source"
    private = case / "private"
    control = case / "control"
    for directory in (source_directory, private, control):
        directory.mkdir()
    source = source_directory / "library.db"
    report = {
        "writer_pid": os.getpid(),
        "writer_sqlite_version": sqlite3.sqlite_version,
    }
    process = None
    writer = sqlite3.connect(source, timeout=5)
    try:
        writer.execute("PRAGMA journal_mode=WAL")
        writer.execute("PRAGMA synchronous=FULL")
        writer.execute("PRAGMA wal_autocheckpoint=0")
        writer.executescript(
            "CREATE TABLE tracks(id INTEGER PRIMARY KEY, "
            "generation INTEGER NOT NULL, payload TEXT);"
            "CREATE TABLE library_state(epoch INTEGER NOT NULL, "
            "expected_count INTEGER NOT NULL);"
            "INSERT INTO library_state VALUES(0, 0);"
        )
        writer.execute("PRAGMA wal_checkpoint(TRUNCATE)")
        count = 2048
        with writer:
            writer.executemany(
                "INSERT INTO tracks VALUES(?, 1, ?)",
                (
                    (number, "synthetic-" + "x" * 1024)
                    for number in range(1, count + 1)
                ),
            )
            writer.execute(
                "UPDATE library_state SET epoch=1, expected_count=?", (count,)
            )
        require(
            Path(str(source) + "-wal").stat().st_size > 32,
            "Fixture has no committed WAL",
        )
        main_only = control / "main-only.sqlite"
        shutil.copyfile(source, main_only)
        with sqlite3.connect(
            main_only.as_uri() + "?mode=ro&immutable=1", uri=True
        ) as oracle:
            require(
                oracle.execute("SELECT count(*) FROM tracks").fetchone()[0]
                == 0,
                "Fixture did not keep all content rows exclusively in WAL",
            )
        report["wal_only_rows"] = count
        report["committed_wal"] = run(driver, source, private, count, 1)
        print(
            "PASS separate-process committed WAL rows and source integrity",
            flush=True,
        )

        gate = control / "continue"
        before = manifest(source_directory)
        command = [str(driver), str(source), str(private), str(gate)]
        trace = work / "latest-importer.strace"
        strace = shutil.which("strace")
        trace_enabled = False
        if strace:
            availability = subprocess.run(
                [
                    strace,
                    "-o",
                    str(control / "strace-availability.log"),
                    sys.executable,
                    "-c",
                    "pass",
                ],
                capture_output=True,
                text=True,
                timeout=10,
            )
            if availability.returncode == 0:
                command = [
                    strace,
                    "-f",
                    "-yy",
                    "-o",
                    str(trace),
                    "-e",
                    "trace=open,openat,write,writev,pwrite64,pwritev,pwritev2,"
                    "ftruncate,truncate,unlink,unlinkat,"
                    "rename,renameat,renameat2,"
                    "mmap,mprotect,msync,fallocate,fcntl",
                    *command,
                ]
                trace_enabled = True
            else:
                report["syscall_trace"] = {
                    "status": "unavailable",
                    "reason": availability.stderr.strip(),
                }
        else:
            report["syscall_trace"] = {
                "status": "unavailable",
                "reason": "strace not installed",
            }
        process = subprocess.Popen(
            command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        report["importer_launcher_pid"] = process.pid
        event = wait_event(process, 10)
        require(
            event == {"event": "read-pinned"}, "Unexpected snapshot handshake"
        )
        require(
            before == manifest(source_directory),
            "Importer changed source files before writer activity",
        )
        started = time.monotonic()
        with writer:
            writer.execute("UPDATE tracks SET generation=2")
            writer.execute(
                "INSERT INTO tracks VALUES(?, 2, 'committed during import')",
                (count + 1,),
            )
            writer.execute(
                "UPDATE library_state SET epoch=2, expected_count=?",
                (count + 1,),
            )
        commit_seconds = time.monotonic() - started
        after_writer = manifest(source_directory)
        require(
            before != after_writer,
            "Concurrent writer did not change the source",
        )
        gate.touch()
        stdout, stderr = process.communicate(timeout=20)
        require(
            process.returncode == 0, stderr or "Coordinated snapshot failed"
        )
        record = json.loads(stdout)
        require(
            record["importer_pid"] != os.getpid(),
            "Writer and importer share a process",
        )
        require(
            record["coordinated"]
            and record["count"] == count
            and record["epoch"] == 1,
            "Concurrent commit changed or mixed the pinned read generation",
        )
        require(
            after_writer == manifest(source_directory),
            "Importer changed source files after writer activity",
        )
        record.update(
            {
                "concurrent_commit_seconds": commit_seconds,
                "before_writer_manifest": summary(before),
                "after_writer_manifest": summary(after_writer),
                "source_changes_only_in_writer_phase": True,
            }
        )
        report["concurrent_commit"] = record
        if trace_enabled:
            report["syscall_trace"] = {
                "status": "passed",
                **inspect_trace(trace, source_directory),
            }
        print(
            "PASS separate-process commit during pinned read "
            "with consistent prior generation",
            flush=True,
        )
        report["after_commit"] = run(driver, source, private, count + 1, 2)
        print(
            "PASS next snapshot observes the later committed generation",
            flush=True,
        )

        writer.execute("BEGIN IMMEDIATE")
        writer.execute("UPDATE tracks SET generation=3")
        writer.execute("UPDATE library_state SET epoch=3")
        report["uncommitted_writer"] = run(
            driver, source, private, count + 1, 2
        )
        writer.rollback()
        print(
            "PASS separate-process uncommitted writes remain outside snapshot",
            flush=True,
        )
        require(
            not list(private.iterdir()),
            "Private snapshot directory leaked files",
        )
        report["passed_cases"] = 4
        report["failed_cases"] = 0
        (work / "latest-report.json").write_text(
            json.dumps(report, indent=2) + "\n"
        )
        print(
            "PASS private cleanup and source manifests in all paused phases",
            flush=True,
        )
        print("Syscall trace:", report["syscall_trace"]["status"], flush=True)
        return 0
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.communicate()
        writer.close()
        shutil.rmtree(case)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print("FAIL", error, file=sys.stderr)
        raise SystemExit(1) from error

# End of code prepared by ChatGPT for human review and submission.
