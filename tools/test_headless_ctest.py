"""AI-generated selection guard tests for review begin.

End AI-generated description.
"""

import copy
import json
import pathlib
import unittest

import headless_ctest

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROFILE = json.loads((ROOT / "tools/headless_ctest_profile.json").read_text())


def case(name, disabled=False):
    return {
        "name": name,
        "command": ["mixxx-test.exe", "--gtest_filter=" + name],
        "properties": [{"name": "DISABLED", "value": disabled}],
    }


def required_tests(system="Linux"):
    names = PROFILE["mandatory_common"] + (
        PROFILE["mandatory_windows"] if system == "Windows" else []
    )
    return [case(name) for name in names]


class HeadlessSelectionTest(unittest.TestCase):
    def select(self, extra=(), system="Linux", exclude=""):
        discovery = {"tests": required_tests(system) + list(extra)}
        return headless_ctest.select_tests(discovery, PROFILE, system, exclude)

    def test_windows_requires_three_mf_cases(self):
        plan = self.select(system="Windows")
        self.assertEqual([], plan["missing_required"])
        self.assertEqual(
            3,
            sum(
                t["definition"].startswith("SoundSourceMediaFoundationTest.")
                for t in plan["selected"]
            ),
        )

    def test_missing_windows_mf_blocks(self):
        plan = headless_ctest.select_tests(
            {"tests": required_tests()}, PROFILE, "Windows"
        )
        self.assertEqual(
            PROFILE["mandatory_windows"], plan["missing_required"]
        )

    def test_source_defined_provider_cases_remain(self):
        extras = [
            case("SoundSourceProxyTest.open"),
            case("CachingReaderRetryTest.AcceptsMaximumStemRequest"),
        ]
        plan = self.select(extras, "Windows")
        self.assertEqual([], plan["unknown"])
        self.assertTrue(
            {t["name"] for t in extras}.issubset(
                {t["name"] for t in plan["selected"]}
            )
        )

    def test_parameterized_gui_excluded(self):
        gui = case(
            "QmlSkins/QmlStartupSmokeTest.StartsLateNightWithoutWarnings/0"
        )
        # Use an actual source case, retaining the parameterized suite prefix.
        names = PROFILE["sources"]["src/test/qml/qmlstartupsmoketest.cpp"][
            "cases"
        ]
        gui = case("QmlSkins/" + next(iter(names)) + "/0")
        plan = self.select([gui])
        self.assertEqual([], plan["unknown"])
        self.assertEqual(gui["name"], plan["excluded"][0]["name"])

    def test_unknown_case_blocks(self):
        plan = self.select([case("FutureWindowTest.OpensWindow")])
        self.assertEqual(1, len(plan["unknown"]))

    def test_wildcard_command_blocks(self):
        plan = self.select([case("EngineMixerTest.*")])
        self.assertEqual(1, len(plan["unknown"]))

    def test_unexpected_executable_blocks(self):
        item = copy.deepcopy(required_tests()[0])
        item["command"][0] = "mixxx.exe"
        plan = self.select([item])
        self.assertEqual(1, len(plan["unknown"]))

    def test_disabled_mandatory_blocks(self):
        tests = required_tests("Windows")
        tests[-1]["properties"][0]["value"] = True
        plan = headless_ctest.select_tests(
            {"tests": tests}, PROFILE, "Windows"
        )
        self.assertEqual([tests[-1]["name"]], plan["missing_required"])

    def test_arm_exclusion_retained(self):
        source = PROFILE["sources"]["src/test/autodjprocessor_test.cpp"][
            "cases"
        ]
        item = case(next(iter(source)))
        plan = self.select([item], "Windows", "^AutoDJProcessorTest.*$")
        self.assertEqual(item["name"], plan["excluded"][0]["name"])

    def test_plugin_instantiation_excluded_but_dispatch_kept(self):
        extras = [
            case(
                "AudioUnitManagerTest.AsyncOutOfProcessCompletesWithinTimeout"
            ),
            case("AudioUnitManagerTest.DispatchGroupLeaveMustBeCalledOnError"),
        ]
        plan = self.select(extras, "Darwin")
        self.assertEqual(
            [extras[0]["name"]], [t["name"] for t in plan["excluded"]]
        )
        self.assertIn(extras[1]["name"], [t["name"] for t in plan["selected"]])


class ResultGuardTest(unittest.TestCase):
    def setUp(self):
        self.plan = headless_ctest.select_tests(
            {"tests": required_tests("Windows")}, PROFILE, "Windows"
        )
        self.results = [
            {
                "name": t["name"],
                "attributes": {"status": "run"},
                "skipped": [],
                "failures": [],
            }
            for t in self.plan["selected"]
        ]

    def test_success_matches_selected_cases(self):
        self.assertEqual(
            ([], []), headless_ctest.validate_results(self.results, self.plan)
        )

    def test_mf_skip_is_not_pass(self):
        self.results[-1]["skipped"] = [{"message": "missing provider"}]
        self.results[-1]["attributes"]["status"] = "notrun"
        errors, required = headless_ctest.validate_results(
            self.results, self.plan
        )
        self.assertEqual([], errors)
        self.assertEqual([self.results[-1]["name"]], required)

    def test_missing_results_not_accepted(self):
        errors, required = headless_ctest.validate_results(
            self.results[:-1], self.plan
        )
        self.assertTrue(errors)
        self.assertEqual([self.results[-1]["name"]], required)

    def test_duplicate_results_not_accepted(self):
        errors, _ = headless_ctest.validate_results(
            self.results + [self.results[0]], self.plan
        )
        self.assertTrue(errors)

    def test_failure_not_accepted_even_if_ctest_exit_zero(self):
        self.results[0]["failures"] = [{"message": "failure"}]
        errors, required = headless_ctest.validate_results(
            self.results, self.plan
        )
        self.assertTrue(errors)
        self.assertEqual([self.results[0]["name"]], required)


if __name__ == "__main__":
    unittest.main()
