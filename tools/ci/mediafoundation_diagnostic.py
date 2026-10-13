#!/usr/bin/env python3
"""Record MF provenance and require the reviewed diagnostic cases."""

import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
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
REVIEWED_BLOBS = dict(
    zip(
        REVIEWED_HASHES,
        (
            "2af32ea415df0089d4b0db934fdf3fa13935f9fa",
            "21e168e48427ff322f835e5e5082a1e19285faf7",
            "856b46521ad39ddf0996252b6f7735a8b6f88a16",
        ),
    )
)
BUNGEE_POSTPATCH = {
    "lib/bungee/src/Assert.cpp": (
        "95162f7dd5792896a3914d7b541f20d6" "3c55fed44ce3938e1ec0b27bdf4b1aa1"
    ),
    "lib/bungee/src/Resample.h": (
        "d3b1c688b0834c784c7168c48c2d3bd2" "f8461fdb7f65dad0f29b1eab459e87c3"
    ),
    "lib/bungee/src/Platform.h": (
        "01e60e8fbd3ef203d08590f49f6d1c92" "83dcefbaed958f48ed04c3939db1913a"
    ),
}
GENERATED_PATH = "lib/bungee/src/Platform.h"
ALLOWED_DIRTY = set(BUNGEE_POSTPATCH) - {GENERATED_PATH}


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


def contained_path(root, relative):
    root = root.resolve()
    relative = Path(relative)
    if relative.is_absolute() or ".." in relative.parts:
        raise ValueError("Provenance path must remain inside the checkout")
    candidate = root
    for part in relative.parts:
        candidate = candidate / part
        if candidate.is_symlink():
            raise ValueError("Symlinks are not allowed in provenance paths")
    if not candidate.resolve().is_relative_to(root):
        raise ValueError("Provenance path escapes the checkout")
    return candidate


def capture(root, build, phase):
    prefix = contained_path(root, f"build/mf-seek-gap-provenance-{phase}")
    relative = prefix.relative_to(root)
    status_path = contained_path(root, relative.with_suffix(".status.txt"))
    diff_path = contained_path(root, relative.with_suffix(".diff"))
    report_path = contained_path(root, relative.with_suffix(".json"))
    status = git(root, "status", "--porcelain=v1", "--untracked-files=all")
    dirty = git(
        root,
        "diff",
        "--no-ext-diff",
        "--no-textconv",
        "--name-only",
        "-z",
        "HEAD",
        "--",
    )
    untracked = git(root, "ls-files", "--others", "--exclude-standard", "-z")
    dirty_names = [os.fsdecode(item) for item in dirty.split(b"\0") if item]
    new_paths = [os.fsdecode(item) for item in untracked.split(b"\0") if item]
    unexpected = set(dirty_names) - ALLOWED_DIRTY
    unexpected |= set(new_paths) - {GENERATED_PATH}
    unexpected = sorted(unexpected)
    report = {
        "phase": phase,
        "checked_out_commit": git(root, "rev-parse", "HEAD").decode().strip(),
        "github_sha": os.environ["GITHUB_SHA"],
        "github_ref": os.environ.get("GITHUB_REF"),
        "run_id": os.environ.get("GITHUB_RUN_ID"),
        "dirty_status": status.decode("utf-8", errors="replace").splitlines(),
        "dirty_tracked_paths": dirty_names,
        "untracked_paths": new_paths,
        "unexpected_paths": unexpected,
        "capture_status": "inventory_only",
    }
    status_path.write_bytes(status)
    write_json(report_path, report)
    if unexpected:
        raise ValueError("Unexpected dirty provenance paths; review inventory")
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
    paths = {name: contained_path(root, name) for name in inputs}
    for name in dirty_names + new_paths:
        contained_path(root, name)
    executable = contained_path(root, "build/mixxx-test.exe")
    generated_output = contained_path(
        root, f"build/{prefix.name}-untracked/{GENERATED_PATH}"
    )
    if report["checked_out_commit"] != report["github_sha"]:
        raise ValueError("Checkout commit differs from dispatched GITHUB_SHA")
    blobs = {
        name: git(root, "hash-object", f"--path={name}", str(paths[name]))
        .decode()
        .strip()
        for name in REVIEWED_BLOBS
    }
    eol_config = subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "config",
            "--show-origin",
            "--get-regexp",
            r"^core\.(autocrlf|eol|safecrlf)$",
        ],
        stdout=subprocess.PIPE,
        check=False,
    )
    if eol_config.returncode not in (0, 1):
        eol_config.check_returncode()
    hashes = {name: sha256(path) for name, path in paths.items()}
    executable_hash = sha256(executable) if executable.is_file() else None
    report.update(
        source_input_sha256=hashes,
        mf_git_normalized_blobs=blobs,
        reviewed_mf_git_blobs=REVIEWED_BLOBS,
        reviewed_mf_sha256=REVIEWED_HASHES,
        eol_attributes=git(root, "check-attr", "--all", "--", *inputs)
        .decode()
        .splitlines(),
        eol_configuration=eol_config.stdout.decode().splitlines(),
        test_executable_sha256=executable_hash,
    )
    write_json(report_path, report)
    if blobs != REVIEWED_BLOBS:
        raise ValueError("MF normalized blob differs from reviewed source")
    generated_data = None
    for name in dirty_names + new_paths:
        data = contained_path(root, name).read_bytes()
        digest = hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()
        if digest != BUNGEE_POSTPATCH[name]:
            raise ValueError("Bungee bytes differ from the reviewed patch")
        if name == GENERATED_PATH:
            generated_data = data
    diff = git(
        root,
        "diff",
        "--no-ext-diff",
        "--no-textconv",
        "--binary",
        "HEAD",
        "--",
        *sorted(ALLOWED_DIRTY),
    )
    diff_path.write_bytes(diff)
    if generated_data is not None:
        generated_output.parent.mkdir(parents=True, exist_ok=True)
        generated_output.write_bytes(generated_data)
    report.update(
        capture_status="captured",
        tracked_diff_sha256=hashlib.sha256(diff).hexdigest(),
        untracked_file_sha256=(
            {GENERATED_PATH: hashlib.sha256(generated_data).hexdigest()}
            if generated_data is not None
            else {}
        ),
    )
    write_json(report_path, report)
    if phase == "after-build" and not executable.is_file():
        raise ValueError("The built mixxx-test.exe is missing")


def run_logged(command, build, stem, timeout):
    process = subprocess.Popen(
        command, cwd=build, stdout=subprocess.PIPE, stderr=subprocess.PIPE
    )
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as initial:
        stdout, stderr = initial.output or b"", initial.stderr or b""
        evidence = {
            "command": command,
            "timeout_seconds": timeout,
            "status": "timeout",
            "stdio_closed": False,
            "child_termination_confirmed": False,
        }
        try:
            if os.name == "nt":
                try:
                    killed = subprocess.run(
                        ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        timeout=10,
                        check=False,
                    )
                    evidence["taskkill_exit_code"] = killed.returncode
                except (OSError, subprocess.SubprocessError) as error:
                    evidence["taskkill_error"] = type(error).__name__
            process.kill()
            latest_stdout, latest_stderr = process.communicate(timeout=5)
            stdout, stderr = latest_stdout or stdout, latest_stderr or stderr
            evidence["stdio_closed"] = True
        except subprocess.TimeoutExpired as cleanup:
            stdout = cleanup.output or stdout
            stderr = cleanup.stderr or stderr
            evidence["cleanup_error"] = "TimeoutExpired"
        except (OSError, subprocess.SubprocessError) as error:
            evidence["cleanup_error"] = type(error).__name__
        finally:
            evidence["root_exit_code"] = process.poll()
            (build / f"{stem}.stdout.log").write_bytes(stdout)
            (build / f"{stem}.stderr.log").write_bytes(stderr)
            write_json(build / f"{stem}.status.json", evidence)
        raise subprocess.TimeoutExpired(command, timeout, stdout, stderr)
    return_code = process.returncode
    result = subprocess.CompletedProcess(command, return_code, stdout, stderr)
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


def validate_junit(path, selected):
    report = ET.parse(path)
    cases = report.findall(".//testcase")
    require_cases([case.attrib["name"] for case in cases], set(selected))
    for element in report.iter():
        if element.tag in ("failure", "error", "skipped"):
            raise ValueError("JUnit contains a failing or skipped MF result")
        if element.tag in ("testsuite", "testsuites"):
            for key in ("failures", "errors", "skipped", "disabled"):
                if int(element.get(key, "0")) != 0:
                    raise ValueError("JUnit unsuccessful total is nonzero")
            declared = element.get("tests")
            actual = len(element.findall(".//testcase"))
            if declared is not None and int(declared) != actual:
                raise ValueError("JUnit declared test count is inconsistent")
        if element.tag in ("testcase", "testsuite", "testsuites"):
            for key in ("status", "result"):
                if element.get(key, "").strip().lower() not in (
                    "",
                    "run",
                    "passed",
                    "pass",
                    "success",
                    "completed",
                ):
                    raise ValueError("JUnit status is not successful")
    return len(cases)


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
    count = validate_junit(build / "mf-seek-gap-tests.xml", selected)
    print(f"Executed all {count} required MF diagnostic cases")


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
