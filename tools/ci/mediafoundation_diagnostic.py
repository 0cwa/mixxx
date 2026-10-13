#!/usr/bin/env python3
"""Record MF provenance and require the reviewed diagnostic cases."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import xml.etree.ElementTree as ET

REVIEWED_HASHES = {
    "src/sources/soundsourcemediafoundation.cpp": (
        "57cba62556de00a982327da4cb4ffd73" "9510f4b129b5a934d3e16118644a6f25"
    ),
    "src/sources/soundsourcemediafoundation.h": (
        "41dc8ba9ca5ec7fa03e36a72d56613b6" "642d5890074f540979e588abb0de332c"
    ),
    "src/test/soundsourcemediafoundation_test.cpp": (
        "0504d3f9ebb6bbcef75c6fe2e701336c" "03c1ebfed98a878ee6fc37f44b5473f4"
    ),
}
BASE_CASES = (
    "StreamTickNullThenSamplePreservesShortGap",
    "TerminalNullStatusesSettleUnknownPosition",
    "TerminalNullStatusesPreserveKnownPosition",
    "FirstPostSeekTickSampleBeforeTargetReachesTarget",
    "FirstPostSeekReadOvershootingTargetIsRejected",
    "FirstPostSeekTickBracketedGapPreservesOutput",
    "FirstPostSeekTickAfterTargetFailsClosed",
    "InvalidPostSeekTickCannotBracketTarget",
    "InvalidPostSeekSampleTimestampFailsClosed",
    "FirstPostSeekTickAtTargetPreservesGap",
    "FirstPostSeekSampleAtTargetPreservesAudio",
    "SeekGapEndingAtTargetDoesNotSilenceTarget",
    "EarlierTickFillsFromKnownRequestedFrame",
    "SeekClearsPendingStreamGap",
    "PublicEmptyFailureMarksFollowingFramesUnreadable",
    "PublicPartialFailureMarksFollowingFramesUnreadable",
)
EXPECTED = {f"SoundSourceMediaFoundationTest.{case}" for case in BASE_CASES}
EXPECTED.update(
    "ExtractionFailures/SoundSourceMediaFoundationSampleFailureTest."
    f"FailedAnchorCannotCompleteSeek/{index}"
    for index in range(12)
)
EXPECTED.update(
    "ExtractionFailures/SoundSourceMediaFoundationBufferedFailureTest."
    f"PreserveOutputPrefixButDiscardBufferedSuffix/{index}"
    for index in range(6)
)
SELECTOR = (
    r"^(SoundSourceMediaFoundationTest|ExtractionFailures/"
    r"SoundSourceMediaFoundation(SampleFailure|BufferedFailure)Test)[.]"
)


def require_cases(names, expected=EXPECTED):
    counts = Counter(names)
    duplicate = sorted(name for name, count in counts.items() if count != 1)
    disabled = sorted(name for name in counts if "DISABLED_" in name)
    missing = sorted(expected - counts.keys())
    unexpected = sorted(counts.keys() - expected)
    if duplicate or disabled or missing or unexpected:
        raise ValueError(
            f"Invalid MF cases: duplicate={duplicate}, disabled={disabled}, "
            f"missing={missing}, unexpected={unexpected}"
        )


def parse_gtest(text):
    suite = None
    names = []
    for line in text.splitlines():
        content = line.split("#", 1)[0].rstrip()
        if not content:
            continue
        if not content[0].isspace():
            suite = content[:-1] if content.endswith(".") else None
        elif suite and "SoundSourceMediaFoundation" in suite:
            case = content.strip()
            if not re.fullmatch(r"[A-Za-z_]\w*(/\d+)?", case):
                raise ValueError(f"Malformed MF discovery line: {line!r}")
            names.append(f"{suite}.{case}")
    require_cases(names)
    return names


def select_ctest(document):
    names = []
    selected = []
    for test in document["tests"]:
        filters = [
            arg.split("=", 1)[1]
            for arg in test.get("command", [])
            if arg.startswith("--gtest_filter=")
        ]
        if "SoundSourceMediaFoundation" not in test["name"] and not any(
            "SoundSourceMediaFoundation" in arg for arg in filters
        ):
            continue
        if len(filters) != 1 or not re.match(SELECTOR, test["name"]):
            raise ValueError(f"Invalid MF CTest registration: {test!r}")
        if any(
            prop["name"] == "DISABLED" and prop["value"]
            for prop in test.get("properties", [])
        ):
            raise ValueError(f"Disabled MF CTest registration: {test['name']}")
        names.extend(filters)
        selected.append(test["name"])
    require_cases(names)
    if len(set(selected)) != len(selected):
        raise ValueError("Duplicate MF CTest display names")
    return selected


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def capture(root, build, phase):
    prefix = build / f"mf-seek-gap-provenance-{phase}"
    status = git(root, "status", "--porcelain=v1", "--untracked-files=all")
    diff = git(root, "diff", "--binary", "--submodule=diff", "HEAD", "--")
    prefix.with_suffix(".status.txt").write_bytes(status)
    prefix.with_suffix(".diff").write_bytes(diff)
    untracked = git(root, "ls-files", "--others", "--exclude-standard", "-z")
    untracked_hashes = {}
    for item in untracked.split(b"\0"):
        if not item:
            continue
        relative = os.fsdecode(item)
        source = root / relative
        destination = build / f"{prefix.name}-untracked" / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
        untracked_hashes[relative] = sha256(source)
    inputs = list(REVIEWED_HASHES) + [
        "CMakeLists.txt",
        ".github/workflows/develop.yml",
        ".github/workflows/build.yml",
        "tools/ci/mediafoundation_diagnostic.py",
        "src/test/mixxxtest.h",
        "src/test/mixxxtest.cpp",
        "src/test/enginebuffertest.cpp",
        "src/test/sine-30.wav",
        "lib/bungee/0001-MSVC-compatibility.patch",
    ]
    commit = git(root, "rev-parse", "HEAD").decode().strip()
    hashes = {name: sha256(root / name) for name in inputs}
    executable = build / "mixxx-test.exe"
    executable_hash = sha256(executable) if executable.is_file() else None
    report = {
        "phase": phase,
        "checked_out_commit": commit,
        "github_sha": os.environ["GITHUB_SHA"],
        "github_ref": os.environ.get("GITHUB_REF"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "dirty_status": status.decode("utf-8", errors="replace").splitlines(),
        "tracked_diff_sha256": hashlib.sha256(diff).hexdigest(),
        "untracked_file_sha256": untracked_hashes,
        "source_input_sha256": hashes,
        "reviewed_mf_sha256": REVIEWED_HASHES,
        "test_executable_sha256": executable_hash,
    }
    write_json(prefix.with_suffix(".json"), report)
    if commit != report["github_sha"]:
        raise ValueError("Checkout commit differs from dispatched GITHUB_SHA")
    for name, expected in REVIEWED_HASHES.items():
        if hashes[name] != expected:
            raise ValueError("MF source bytes differ from the reviewed repair")
    if phase == "after-build" and not executable.is_file():
        raise ValueError("The built mixxx-test.exe is missing")


def run_logged(command, build, stem, timeout):
    with subprocess.Popen(
        command, cwd=build, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    ) as process:
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name == "nt":
                try:
                    subprocess.run(
                        ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        timeout=10,
                        check=False,
                    )
                except (OSError, subprocess.SubprocessError):
                    process.kill()
            process.kill()
            stdout, stderr = process.communicate(timeout=5)
            (build / f"{stem}.stdout.log").write_bytes(stdout)
            (build / f"{stem}.stderr.log").write_bytes(stderr)
            write_json(
                build / f"{stem}.status.json",
                {"command": command, "timeout_seconds": timeout},
            )
            raise subprocess.TimeoutExpired(command, timeout, stdout, stderr)
        result = subprocess.CompletedProcess(
            command, process.returncode, stdout, stderr
        )
    (build / f"{stem}.stdout.log").write_bytes(result.stdout)
    (build / f"{stem}.stderr.log").write_bytes(result.stderr)
    write_json(
        build / f"{stem}.status.json",
        {
            "command": command,
            "exit_code": result.returncode,
            "timeout_seconds": timeout,
        },
    )
    print(result.stdout.decode("utf-8", errors="replace"))
    print(result.stderr.decode("utf-8", errors="replace"))
    result.check_returncode()
    return result.stdout


def run_ctest(build):
    raw = run_logged(
        ["ctest", "--show-only=json-v1"], build, "mf-seek-gap-ctest-list", 60
    )
    (build / "mf-seek-gap-ctest-list.json").write_bytes(raw)
    selected = select_ctest(json.loads(raw))
    write_json(build / "mf-seek-gap-ctest-selection.json", selected)
    (build / "mf-seek-gap-tests.xml").unlink(missing_ok=True)
    run_logged(
        [
            "ctest",
            "--timeout",
            "45",
            "--parallel",
            "1",
            "--no-tests=error",
            "--output-on-failure",
            "--verbose",
            "--tests-regex",
            SELECTOR,
            "--output-junit",
            "mf-seek-gap-tests.xml",
        ],
        build,
        "mf-seek-gap-ctest-run",
        1650,
    )
    cases = ET.parse(build / "mf-seek-gap-tests.xml").findall(".//testcase")
    require_cases([case.attrib["name"] for case in cases], set(selected))
    if any(
        case.find("skipped") is not None
        or case.attrib.get("status") in ("notrun", "disabled")
        for case in cases
    ):
        raise ValueError("CTest skipped a required MF diagnostic case")
    print(f"Executed all {len(cases)} required MF diagnostic cases")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "phase",
        choices=(
            "before-configure",
            "after-configure",
            "after-build",
            "discovery",
            "ctest",
        ),
    )
    args = parser.parse_args()
    root = Path(os.environ["GITHUB_WORKSPACE"])
    build = root / "build"
    if args.phase == "discovery":
        write_json(build / "mf-seek-gap-expected-tests.json", sorted(EXPECTED))
        names = parse_gtest(
            (build / "mf-seek-gap-gtest-list.stdout.log").read_text(
                encoding="utf-8", errors="replace"
            )
        )
        write_json(build / "mf-seek-gap-discovered-tests.json", names)
    elif args.phase == "ctest":
        run_ctest(build)
    else:
        capture(root, build, args.phase)


if __name__ == "__main__":
    main()
