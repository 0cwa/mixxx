#!/usr/bin/env python3
"""AI-generated headless CI selection for human review begins.

Select source-classified GTest cases from actual platform CTest discovery.
Unknown cases or changed source block execution. Preserve result evidence.

End AI-generated description.
"""

import argparse
import collections
import hashlib
import json
import os
import pathlib
import platform
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def validate_source_checkout(source):
    paths = [
        "src",
        "CMakeLists.txt",
        "cmake",
        "tools/headless_ctest.py",
        "tools/headless_ctest_profile.json",
        ".github/workflows",
    ]
    status = subprocess.check_output(
        [
            "git",
            "-C",
            str(source),
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
            "--ignored=matching",
            "--ignore-submodules=none",
            "--",
            *paths,
        ],
        text=True,
    )
    if status:
        raise ValueError(
            "Source checkout has uncommitted or ignored changes: "
            "classification requires review"
        )
    return {"status": "clean", "paths": paths, "includes_ignored": True}


def case_definition(test, definitions):
    command = test.get("command", [])
    if not command or pathlib.PurePath(
        command[0].replace("\\", "/")
    ).name not in ("mixxx-test", "mixxx-test.exe"):
        raise ValueError(f"Unexpected test executable: {test['name']}")
    filters = [
        arg.split("=", 1)[1]
        for arg in test.get("command", [])
        if arg.startswith("--gtest_filter=")
    ]
    if len(filters) != 1 or re.search(r"[*?:]", filters[0]):
        raise ValueError(f"Not one exact GTest case: {test['name']}")
    suite, case = filters[0].split(".", 1)
    case = case.split("/", 1)[0]
    matches = [
        part + "." + case
        for part in suite.split("/")
        if part + "." + case in definitions
    ]
    if len(matches) != 1:
        raise ValueError(f"Unclassified source definition: {filters[0]}")
    return matches[0]


def select_tests(
    discovery, profile, system, exclude_regex="", cache_text=None
):
    definitions = {}
    for source, entry in profile["sources"].items():
        for name, policy in entry["cases"].items():
            if name in definitions:
                raise ValueError(f"Ambiguous source definition: {name}")
            definitions[name] = policy | {"source": source}
    selected, excluded, unknown = [], [], []
    for index, test in enumerate(discovery["tests"], 1):
        try:
            definition = case_definition(test, definitions)
        except ValueError as error:
            unknown.append({"name": test["name"], "error": str(error)})
            continue
        policy = definitions[definition]
        properties = {
            p["name"]: p["value"] for p in test.get("properties", [])
        }
        item = {
            "index": index,
            "name": test["name"],
            "definition": definition,
            "source": policy["source"],
            "disabled": bool(properties.get("DISABLED", False)),
            "reason": policy["reason"],
        }
        if policy["classification"] != "headless":
            excluded.append(item)
        elif exclude_regex and re.search(exclude_regex, test["name"]):
            item["reason"] = "Existing platform exclusion: " + exclude_regex
            excluded.append(item)
        else:
            selected.append(item)
    required = profile["mandatory_common"] + (
        profile["mandatory_windows"] if system == "Windows" else []
    )
    # AI-generated optional-feature guard begins.
    # Actual execution supplies its CMake cache.
    # Saved discovery remains a plan.
    if cache_text is not None:
        for feature, cases in profile.get("mandatory_features", {}).items():
            match = re.search(
                rf"^{re.escape(feature)}:BOOL=(ON|OFF)$",
                cache_text,
                re.MULTILINE,
            )
            if not match:
                raise ValueError(
                    f"Missing explicit feature configuration: {feature}"
                )
            if match.group(1) == "ON":
                required += cases
    # End AI-generated optional-feature guard.
    available = {t["definition"] for t in selected if not t["disabled"]}
    missing = sorted(set(required) - available)
    coverage = {}
    for source, entry in profile["sources"].items():
        included = [t for t in selected if t["source"] == source]
        omitted = [t for t in excluded if t["source"] == source]
        observed = {t["definition"] for t in included + omitted}
        coverage[source] = {
            "source_definitions": len(entry["cases"]),
            "discovered_instances": len(included) + len(omitted),
            "selected_instances": len(included),
            "disabled_selected": sum(t["disabled"] for t in included),
            "excluded_instances": len(omitted),
            "definitions_not_discovered": sorted(
                set(entry["cases"]) - observed
            ),
        }
    return {
        "system": system,
        "discovered": len(discovery["tests"]),
        "selected": selected,
        "excluded": excluded,
        "unknown": unknown,
        "required": required,
        "qualified_skips": profile.get("qualified_skips", {}).get(system, {}),
        "missing_required": missing,
        "coverage_by_source": coverage,
        "disabled_selected": sum(t["disabled"] for t in selected),
        "excluded_by_reason": dict(
            collections.Counter(t["reason"] for t in excluded)
        ),
    }


def qualified_skip(case, policy):
    return bool(
        policy
        and case.get("skipped")
        and not case.get("failures")
        and not case.get("errors")
        and case.get("attributes", {}).get("status") == "notrun"
        and any(
            message in case.get("output", "")
            for message in policy["allowed_output_messages"]
        )
    )


def validate_diagnostic_config(system, cache_text):
    if system == "Darwin" and not re.search(
        r"^AU_EFFECTS:BOOL=OFF$", cache_text, re.MULTILINE
    ):
        raise ValueError(
            "Headless macOS diagnostics require AU_EFFECTS=OFF; "
            "effects fixtures can instantiate installed AudioUnits"
        )


def validate_results(results, plan):
    errors = []
    names = {r["name"] for r in results}
    if names != {t["name"] for t in plan["selected"]} or len(results) != len(
        plan["selected"]
    ):
        errors.append("JUnit case set differs from selection")
    if any(r["failures"] or r.get("errors") for r in results):
        errors.append("JUnit contains failed cases")
    by_name = {r["name"]: r for r in results}
    required_errors = []
    for item in plan["selected"]:
        if item["definition"] in plan["required"]:
            case = by_name.get(item["name"], {})
            policy = plan.get("qualified_skips", {}).get(item["definition"])
            if qualified_skip(case, policy):
                continue
            if (
                case.get("skipped")
                or case.get("failures")
                or case.get("errors")
                or case.get("attributes", {}).get("status") != "run"
            ):
                required_errors.append(item["name"])
    return errors, required_errors


def run_plan(args):
    source = pathlib.Path(__file__).resolve().parents[1]
    output = pathlib.Path(args.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    profile_path = source / "tools/headless_ctest_profile.json"
    profile = json.loads(profile_path.read_text(encoding="utf-8"))

    def git(*args):
        return subprocess.check_output(
            ["git", "-C", str(source), *args], text=True
        ).strip()

    receipt = {
        "head": git("rev-parse", "HEAD"),
        "tree": git("rev-parse", "HEAD^{tree}"),
        "src_tree": git("rev-parse", "HEAD:src"),
        "profile_SHA256": hashlib.sha256(
            profile_path.read_bytes()
        ).hexdigest(),
        "platform": platform.platform(),
        "system": platform.system(),
        "github_sha": os.environ.get("GITHUB_SHA"),
        "github_run_id": os.environ.get("GITHUB_RUN_ID"),
        "scope": "Source-classified headless tests; no GUI/runtime acceptance",
        "execution": "not started",
    }
    write_json(output / "receipt.json", receipt)
    if args.expected_head and (
        not re.fullmatch(r"[0-9a-f]{40}", args.expected_head)
        or args.expected_head != receipt["head"]
    ):
        raise ValueError("Checkout does not match the exact reviewed commit")
    receipt["expected_head"] = args.expected_head or None
    receipt["working_source_checkout"] = validate_source_checkout(source)
    if receipt["src_tree"] != profile["src_tree"]:
        raise ValueError("Source tree changed: classification requires review")
    if (
        hashlib.sha256((source / "CMakeLists.txt").read_bytes()).hexdigest()
        != profile["cmake_SHA256"]
    ):
        raise ValueError(
            "CMake test definitions changed: classification requires review"
        )
    if any(
        name in os.environ
        for name in ("GTEST_TOTAL_SHARDS", "GTEST_SHARD_INDEX")
    ):
        raise ValueError(
            "GTest sharding would invalidate per-case execution receipts"
        )
    for path, policy in profile["sources"].items():
        if (
            hashlib.sha256((source / path).read_bytes()).hexdigest()
            != policy["sha256"]
        ):
            raise ValueError(f"Source bytes changed: {path}")
    ctest = [args.ctest, "--test-dir", str(pathlib.Path(args.build).resolve())]
    if args.config:
        ctest += ["-C", args.config]
    cache_text = None
    if args.discovery_json:
        if not args.plan_only:
            raise ValueError(
                "Saved discovery is allowed only for plan-only validation"
            )
        discovery = json.loads(pathlib.Path(args.discovery_json).read_text())
        receipt["discovery_mode"] = (
            "saved discovery; no platform execution claim"
        )
    else:
        cache = pathlib.Path(args.build).resolve() / "CMakeCache.txt"
        cache_text = cache.read_text(encoding="utf-8")
        validate_diagnostic_config(platform.system(), cache_text)
        receipt["CMakeCache_SHA256"] = hashlib.sha256(
            cache.read_bytes()
        ).hexdigest()
        receipt["AU_effects_coverage"] = (
            "disabled for macOS diagnostic; no AU integration coverage"
            if platform.system() == "Darwin"
            else "not a macOS runner"
        )
        discovery = json.loads(
            subprocess.check_output(ctest + ["--show-only=json-v1"], text=True)
        )
        receipt["discovery_mode"] = "actual platform CTest discovery"
    write_json(output / "discovered.json", discovery)
    plan = select_tests(
        discovery, profile, platform.system(), args.exclude_regex, cache_text
    )
    write_json(output / "selection.json", plan)
    receipt["counts"] = {
        "discovered": plan["discovered"],
        "selected": len(plan["selected"]),
        "excluded": len(plan["excluded"]),
        "disabled_selected": plan["disabled_selected"],
        "unknown": len(plan["unknown"]),
    }
    receipt["missing_required"] = plan["missing_required"]
    write_json(output / "receipt.json", receipt)
    if plan["unknown"] or plan["missing_required"] or not plan["selected"]:
        raise ValueError(
            "Unknown cases, missing mandatory cases, or empty selection"
        )
    index_file = output / "selected-indices.txt"
    index_file.write_text(
        "0,0,1," + ",".join(str(t["index"]) for t in plan["selected"]) + "\n"
    )
    (output / "selected.txt").write_text(
        "\n".join(t["name"] for t in plan["selected"]) + "\n", encoding="utf-8"
    )
    if args.plan_only and args.discovery_json:
        receipt["execution"] = "plan-only; saved discovery"
        write_json(output / "receipt.json", receipt)
        return 0
    selection = ["-I", str(index_file), "-FA", ".*"]
    verified = json.loads(
        subprocess.check_output(
            ctest + selection + ["--show-only=json-v1"], text=True
        )
    )
    write_json(output / "verified-selection.json", verified)
    if [t["name"] for t in verified["tests"]] != [
        t["name"] for t in plan["selected"]
    ]:
        raise ValueError(
            "CTest selection differs from approved source-classified plan"
        )
    if args.plan_only:
        receipt["execution"] = "plan-only; actual discovery/selection verified"
        write_json(output / "receipt.json", receipt)
        return 0
    command = (
        ctest
        + selection
        + [
            "--timeout",
            str(args.timeout),
            "--output-on-failure",
            "--output-junit",
            str(output / "results.xml"),
        ]
    )
    receipt["command"] = command
    receipt["execution"] = "started"
    (output / "results.xml").unlink(missing_ok=True)
    write_json(output / "receipt.json", receipt)
    with (output / "ctest.log").open("w", encoding="utf-8") as log:
        execution_env = os.environ.copy()
        if plan["system"] == "Windows":
            execution_env["MIXXX_HEADLESS_MF_DIAGNOSTICS"] = "1"
            receipt["MF_diagnostic_opt_in"] = True
            write_json(output / "receipt.json", receipt)
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=execution_env,
        )
        for line in process.stdout:
            print(line, end="", flush=True)
            log.write(line)
        code = process.wait()
    receipt["exit"] = code
    receipt["execution"] = "finished"
    if (output / "results.xml").exists():
        root = ET.parse(output / "results.xml").getroot()
        results = []
        for case in root.iter("testcase"):
            results.append(
                {
                    "name": case.get("name"),
                    "attributes": case.attrib,
                    "skipped": [e.attrib for e in case.findall("skipped")],
                    "failures": [e.attrib for e in case.findall("failure")],
                    "errors": [e.attrib for e in case.findall("error")],
                    "output": case.findtext("system-out", default=""),
                }
            )
        write_json(output / "case-results.json", results)
        receipt["qualified_skipped_cases"] = [
            {
                "name": item["name"],
                "definition": item["definition"],
                "reason": plan["qualified_skips"][item["definition"]][
                    "reason"
                ],
                "status": "qualified skip; not a test pass",
            }
            for item in plan["selected"]
            for case in results
            if case["name"] == item["name"]
            and qualified_skip(
                case, plan["qualified_skips"].get(item["definition"])
            )
        ]
        receipt["junit"] = root.attrib
        receipt["statuses"] = dict(
            collections.Counter(r["attributes"].get("status") for r in results)
        )
        result_errors, required_errors = validate_results(results, plan)
        if result_errors:
            receipt["result_error"] = result_errors
            code = code or 1
        if required_errors:
            receipt["required_case_errors"] = required_errors
            code = code or 1
    else:
        receipt["result_error"] = "Missing JUnit results"
        code = code or 1
    receipt["validation_exit"] = code
    write_json(output / "receipt.json", receipt)
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build")
    parser.add_argument("--ctest", default="ctest")
    parser.add_argument("--config", default="")
    parser.add_argument("--expected-head", default="")
    parser.add_argument("--output", default="build/headless-evidence")
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--exclude-regex", default="")
    parser.add_argument("--plan-only", action="store_true")
    parser.add_argument("--discovery-json")
    args = parser.parse_args()
    try:
        return run_plan(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Headless validation blocked: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
