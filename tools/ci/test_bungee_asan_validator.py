#!/usr/bin/env python3
"""Exercise the exact embedded Bungee workflow validator with synthetic XML."""

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest
import xml.etree.ElementTree as ET

WORKFLOW = (
    Path(__file__).resolve().parents[2] / ".github/workflows/bungee-asan.yml"
)
FILTER = (
    "*Bungee*:SettingsManagerTest.*KeylockEngine*:"
    "ReadAheadManagerTest.*:CachingReader*.*"
)
SKIP_ID = (
    "SettingsManagerTest.RejectsUnavailableRubberBandR3GlobalKeylockEngine"
)
REASON = "RubberBand R3 is available"
IDS = [
    "EngineBufferBungeeTest.Alignment",
    "SettingsManagerTest.PreservesBungeeGlobalKeylockEngine",
    SKIP_ID,
    "ReadAheadManagerTest.Retry",
    "CachingReaderTest.Retry",
]


def listing(ids):
    return "".join(
        f"{test.rsplit('.', 1)[0]}.\n  {test.rsplit('.', 1)[1]}\n"
        for test in ids
    )


def cases(root):
    return list(root.iter("testcase"))


def remove_case(root, index):
    target = cases(root)[index]
    for suite in root.findall("testsuite"):
        if target in list(suite):
            suite.remove(target)
            return


def results(skip=False, terminal_newline=False, line_ending="\n"):
    root = ET.Element(
        "testsuites",
        tests=str(len(IDS)),
        failures="0",
        errors="0",
        disabled="0",
        skipped=str(int(skip)),
    )
    suites = {}
    for test in IDS:
        suite_name, name = test.rsplit(".", 1)
        if suite_name not in suites:
            suites[suite_name] = ET.SubElement(
                root,
                "testsuite",
                name=suite_name,
                tests=str(
                    sum(test.startswith(suite_name + ".") for test in IDS)
                ),
                failures="0",
                errors="0",
                disabled="0",
                skipped=str(int(skip and suite_name == "SettingsManagerTest")),
            )
        case = ET.SubElement(
            suites[suite_name],
            "testcase",
            classname=suite_name,
            name=name,
            status="run",
            result="completed",
        )
        if skip and test == SKIP_ID:
            case.set("result", "skipped")
            message = (
                "/home/runner/src/test/settingsmanager_test.cpp:195\n" + REASON
            )
            if terminal_newline:
                message += "\n"
            message = message.replace("\n", line_ending)
            ET.SubElement(case, "skipped", message=message).text = message
    return root


class ValidatorTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        workflow = WORKFLOW.read_text()
        embedded = workflow.split(
            "          cat > verify_bungee_asan_tests.py <<'PY'\n", 1
        )[1].split("          PY\n", 1)[0]
        self.script = self.directory / "verifier.py"
        self.script.write_text(textwrap.dedent(embedded))

    def run_validator(self, *arguments):
        return subprocess.run(
            [sys.executable, str(self.script), *map(str, arguments)],
            capture_output=True,
            text=True,
            check=False,
        )

    def before(self, ids=IDS, test_filter=FILTER):
        source = self.directory / "list.txt"
        source.write_text(listing(ids))
        selection = self.directory / "selection.json"
        return self.run_validator("before", source, test_filter, selection)

    def after(self, root, native=0, cdata=False):
        self.assertEqual(self.before().returncode, 0)
        xml = self.directory / "results.xml"
        # Preserve CR as an XML character reference so parser newline
        # normalization cannot hide a message/text mismatch in the fixtures.
        encoded = ET.tostring(root, encoding="utf-8")
        if cdata:
            message = cases(root)[2].find("skipped").text.encode("utf-8")
            encoded = encoded.replace(
                b">" + message + b"</skipped>",
                b"><![CDATA[" + message + b"]]></skipped>",
            )
            self.assertIn(b"<![CDATA[", encoded)
        xml.write_bytes(encoded.replace(b"\r", b"&#13;"))
        evidence = self.directory / "evidence.json"
        process = self.run_validator(
            "after", self.directory / "selection.json", xml, native, evidence
        )
        return process, json.loads(evidence.read_text())

    def test_all_passed(self):
        process, evidence = self.after(results())
        self.assertEqual(
            process.returncode, 0, process.stdout + process.stderr
        )
        self.assertEqual(evidence["passed_count"], 5)
        self.assertEqual(evidence["conditional_skipped_tests"], [])

    def test_valid_conditional_skip(self):
        process, evidence = self.after(results(skip=True))
        self.assertEqual(
            process.returncode, 0, process.stdout + process.stderr
        )
        self.assertEqual(evidence["passed_count"], 4)
        self.assertEqual(evidence["skipped"], 1)
        self.assertEqual(len(evidence["conditional_skipped_tests"]), 1)
        self.assertEqual(
            evidence["conditional_skipped_tests"][0]["id"], SKIP_ID
        )
        self.assertEqual(
            evidence["conditional_skipped_tests"][0]["reason"], REASON
        )

    def test_gtest_114_conditional_skip(self):
        root = results(skip=True, terminal_newline=True)
        message = cases(root)[2].find("skipped").get("message")
        self.assertTrue(message.endswith(REASON + "\n"))
        self.assertFalse(message.endswith("\n\n"))
        process, evidence = self.after(root, cdata=True)
        self.assertEqual(
            process.returncode, 0, process.stdout + process.stderr
        )
        self.assertEqual(evidence["passed_count"], 4)
        self.assertEqual(evidence["skipped"], 1)
        self.assertEqual(
            evidence["conditional_skipped_tests"][0]["reason"], REASON
        )

    def test_matching_crlf_conditional_skip(self):
        process, evidence = self.after(
            results(skip=True, terminal_newline=True, line_ending="\r\n")
        )
        self.assertEqual(
            process.returncode, 0, process.stdout + process.stderr
        )
        self.assertEqual(evidence["passed_count"], 4)
        self.assertEqual(
            evidence["conditional_skipped_tests"][0]["reason"], REASON
        )

    def test_rejected_list_controls(self):
        for name, ids, test_filter in [
            ("empty", [], FILTER),
            ("duplicate", IDS + [IDS[0]], FILTER),
            ("wrong_filter", IDS, "*Bungee*"),
            ("disabled", IDS + ["BungeeTest.DISABLED_Alignment"], FILTER),
            (
                "disabled_suite",
                IDS + ["DISABLED_BungeeTest.Alignment"],
                FILTER,
            ),
            *[
                (
                    "zero_" + family,
                    [test for test in IDS if family not in test],
                    FILTER,
                )
                for family in (
                    "Bungee",
                    "SettingsManagerTest",
                    "ReadAheadManagerTest",
                    "CachingReaderTest",
                )
            ],
        ]:
            with self.subTest(name=name):
                self.assertNotEqual(
                    self.before(ids, test_filter).returncode, 0
                )

    def test_rejected_xml_controls(self):
        valid = results(skip=True, terminal_newline=True)
        controls = {}

        def change(name, mutate):
            root = copy.deepcopy(valid)
            mutate(root)
            controls[name] = (root, 0)

        def skip_node(root):
            return cases(root)[2].find("skipped")

        def replace_reason(root, reason):
            skip_node(root).set("message", reason)
            skip_node(root).text = reason

        change("wrong_reason", lambda r: replace_reason(r, "wrong reason"))
        change(
            "wrong_text",
            lambda r: setattr(skip_node(r), "text", "wrong reason"),
        )
        change(
            "wrong_source",
            lambda r: replace_reason(r, "other.cpp:195\n" + REASON),
        )
        change(
            "extra_reason",
            lambda r: replace_reason(r, REASON + "\nother reason"),
        )
        change("double_newline", lambda r: replace_reason(r, REASON + "\n\n"))
        change("trailing_space", lambda r: replace_reason(r, REASON + " \n"))
        change(
            "leading_space", lambda r: replace_reason(r, " " + REASON + "\n")
        )
        change("bare_cr", lambda r: replace_reason(r, REASON + "\r"))
        change("double_crlf", lambda r: replace_reason(r, REASON + "\r\n\r\n"))
        change(
            "newline_mismatch",
            lambda r: setattr(skip_node(r), "text", skip_node(r).text[:-1]),
        )
        change(
            "crlf_mismatch",
            lambda r: setattr(
                skip_node(r), "text", skip_node(r).text.replace("\n", "\r\n")
            ),
        )
        change(
            "wrong_id", lambda r: cases(r)[2].set("name", "OtherKeylockEngine")
        )
        change(
            "extra_skipped",
            lambda r: ET.SubElement(cases(r)[0], "skipped", message=REASON),
        )
        change(
            "duplicate_skip_node",
            lambda r: ET.SubElement(cases(r)[2], "skipped", message=REASON),
        )
        change("disabled_state", lambda r: cases(r)[0].set("status", "notrun"))
        change("suppressed", lambda r: cases(r)[0].set("result", "suppressed"))
        change("skip_notrun", lambda r: cases(r)[2].set("status", "notrun"))
        change(
            "skip_completed", lambda r: cases(r)[2].set("result", "completed")
        )
        change("missing_skip_node", lambda r: cases(r)[2].remove(skip_node(r)))
        change("missing_state", lambda r: cases(r)[0].attrib.pop("status"))
        change("unknown_state", lambda r: cases(r)[0].set("status", "unknown"))
        change("disabled_count", lambda r: r.set("disabled", "1"))
        change("skips_count", lambda r: r.set("skipped", "2"))
        change("failure_count", lambda r: r.set("failures", "1"))
        change("error_count", lambda r: r.set("errors", "1"))
        change("failure_node", lambda r: ET.SubElement(cases(r)[0], "failure"))
        change("error_node", lambda r: ET.SubElement(cases(r)[0], "error"))
        change("missing_id", lambda r: remove_case(r, 0))
        change(
            "duplicate_id",
            lambda r: cases(r)[0].attrib.update(cases(r)[1].attrib),
        )
        change("extra_id", lambda r: r[0].append(copy.deepcopy(cases(r)[0])))
        change("wrong_count", lambda r: r.set("tests", "0"))
        change("zero_family", lambda r: remove_case(r, -1))
        for attribute in ("failures", "errors", "skipped", "disabled"):
            change(
                "nested_suite_" + attribute,
                lambda r, attribute=attribute: r[0].set(attribute, "1"),
            )
        controls["native_failure"] = (valid, 1)
        for name, (root, native) in controls.items():
            with self.subTest(name=name):
                process, evidence = self.after(root, native)
                self.assertNotEqual(process.returncode, 0)
                self.assertTrue(evidence["problems"])
                if name in ("failure_node", "error_node"):
                    self.assertEqual(evidence["passed_count"], 3)


if __name__ == "__main__":
    unittest.main()
