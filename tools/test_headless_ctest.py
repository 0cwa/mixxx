"""AI-generated selection guard tests for review begin.

End AI-generated description.
"""

import copy
import json
import pathlib
import subprocess
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

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


class WorkingSourceGuardTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        self.git("init", "-q")
        for path in (
            "src/engine/example.cpp",
            "src/test/signalpathtest.h",
            "cmake/example.cmake",
            "CMakeLists.txt",
            "tools/headless_ctest.py",
            "tools/headless_ctest_profile.json",
            ".github/workflows/build.yml",
            ".gitignore",
        ):
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("original\n")
        (self.root / "tools/headless_ctest_profile.json").write_text(
            json.dumps(PROFILE)
        )
        self.git("add", ".")
        self.git(
            "-c",
            "user.name=Guard Test",
            "-c",
            "user.email=guard@example.invalid",
            "commit",
            "-qm",
            "Fixture baseline",
        )

    def git(self, *args):
        return subprocess.check_output(
            ["git", "-C", str(self.root), *args], text=True
        )

    def test_clean_source_is_accepted(self):
        result = headless_ctest.validate_source_checkout(self.root)
        self.assertEqual("clean", result["status"])

    def test_dirty_production_and_fixture_headers_are_rejected(self):
        for path in ("src/engine/example.cpp", "src/test/signalpathtest.h"):
            with self.subTest(path=path):
                (self.root / path).write_text("changed\n")
                with self.assertRaisesRegex(
                    ValueError, "classification requires review"
                ):
                    headless_ctest.validate_source_checkout(self.root)
                self.git("restore", path)

    def test_staged_source_is_rejected(self):
        (self.root / "src/engine/example.cpp").write_text("changed\n")
        self.git("add", "src/engine/example.cpp")
        with self.assertRaises(ValueError):
            headless_ctest.validate_source_checkout(self.root)

    def test_untracked_source_is_rejected(self):
        (self.root / "src/test/interfaceqml_test.cpp").write_text("new\n")
        with self.assertRaises(ValueError):
            headless_ctest.validate_source_checkout(self.root)

    def test_ignored_source_is_rejected(self):
        (self.root / ".gitignore").write_text("src/test/ignored.h\n")
        self.git("add", ".gitignore")
        self.git(
            "-c",
            "user.name=Guard Test",
            "-c",
            "user.email=guard@example.invalid",
            "commit",
            "-qm",
            "Ignore fixture",
        )
        (self.root / "src/test/ignored.h").write_text("new\n")
        with self.assertRaises(ValueError):
            headless_ctest.validate_source_checkout(self.root)

    def test_changed_build_policy_helper_and_workflow_are_rejected(self):
        for path in (
            "cmake/example.cmake",
            "CMakeLists.txt",
            "tools/headless_ctest.py",
            "tools/headless_ctest_profile.json",
            ".github/workflows/build.yml",
        ):
            with self.subTest(path=path):
                (self.root / path).write_text("changed\n")
                with self.assertRaises(ValueError):
                    headless_ctest.validate_source_checkout(self.root)
                self.git("restore", path)

    def test_untracked_receipts_outside_source_are_allowed(self):
        (self.root / "receipt.json").write_text("{}\n")
        self.assertEqual(
            "clean",
            headless_ctest.validate_source_checkout(self.root)["status"],
        )

    def test_run_plan_rejects_dirty_header_before_discovery(self):
        (self.root / "src/test/signalpathtest.h").write_text("changed\n")
        args = SimpleNamespace(
            output=self.root / "receipts",
            expected_head=self.git("rev-parse", "HEAD").strip(),
        )
        with mock.patch.object(
            headless_ctest,
            "__file__",
            str(self.root / "tools/headless_ctest.py"),
        ):
            with self.assertRaisesRegex(
                ValueError, "uncommitted or ignored changes"
            ):
                headless_ctest.run_plan(args)


class HeadlessSelectionTest(unittest.TestCase):
    def test_active_track_exporter_remains_explicitly_excluded(self):
        path = "src/test/trackexport_test.cpp"
        self.assertTrue((ROOT / "src/test/trackexport_test.h").is_file())
        policies = PROFILE["sources"][path]["cases"]
        self.assertEqual(7, len(policies))
        plan = self.select([case(name) for name in policies])
        self.assertEqual(7, len(plan["excluded"]))
        for policy in policies.values():
            self.assertIn(
                "Source and fixture header are present", policy["reason"]
            )
            self.assertEqual("excluded", policy["classification"])

    def test_all_native_platforms_require_hid_data_cases(self):
        names = PROFILE["sources"][
            "src/test/controller_hid_reportdescriptor_test.cpp"
        ]["cases"]
        self.assertEqual(7, len(names))
        self.assertTrue(set(names).issubset(PROFILE["mandatory_common"]))
        for system in ("Linux", "Darwin", "Windows"):
            with self.subTest(system=system):
                tests = [
                    t for t in required_tests(system) if t["name"] not in names
                ]
                plan = headless_ctest.select_tests(
                    {"tests": tests}, PROFILE, system
                )
                self.assertEqual(sorted(names), plan["missing_required"])

    def test_disabled_hid_data_case_is_not_coverage(self):
        name = next(
            iter(
                PROFILE["sources"][
                    "src/test/controller_hid_reportdescriptor_test.cpp"
                ]["cases"]
            )
        )
        tests = required_tests()
        next(t for t in tests if t["name"] == name)["properties"][0][
            "value"
        ] = True
        plan = headless_ctest.select_tests({"tests": tests}, PROFILE, "Linux")
        self.assertEqual([name], plan["missing_required"])

    def test_macos_rejects_enabled_or_unproven_audio_units(self):
        for cache in ("", "AU_EFFECTS:BOOL=ON\n"):
            with self.assertRaises(ValueError):
                headless_ctest.validate_diagnostic_config("Darwin", cache)
        headless_ctest.validate_diagnostic_config(
            "Darwin", "AU_EFFECTS:BOOL=OFF\n"
        )

    def test_public_interface_qml_omission_fails_closed(self):
        extras = [
            case(
                "InterfaceQmlTest."
                "EditResetCancelAndSaveKeepMaxZoomOutSynchronized"
            ),
            case(
                "InterfaceQmlTest."
                "LoweringMaxZoomOutReclampsExistingWaveformDisplay"
            ),
        ]
        plan = self.select(extras)
        self.assertEqual(2, len(plan["unknown"]))
        self.assertTrue(
            all("InterfaceQmlTest" not in t["name"] for t in plan["selected"])
        )

    def test_transitive_visual_fixtures_and_renderer_are_excluded(self):
        paths = (
            "src/test/controllerscriptenginelegacy_test.cpp",
            "src/test/playermanagertest.cpp",
            "src/test/enginebufferalignmenttest.cpp",
            "src/test/trackupdate_test.cpp",
        )
        for path in paths:
            policies = PROFILE["sources"][path]["cases"]
            plan = self.select([case(name) for name in policies])
            self.assertEqual(len(policies), len(plan["excluded"]))

    def test_factory_fixture_excluded_marker_data_retained(self):
        plan = self.select(
            [
                case(
                    "WaveformCueCountdownConfigTest."
                    "DefaultsAreMigratedToWaveformConfig"
                ),
                case(
                    "WaveformMarkSetTest."
                    "CountdownSelectionHonorsIndependentCategories"
                ),
            ]
        )
        self.assertEqual(1, len(plan["excluded"]))
        self.assertIn(
            "WaveformMarkSetTest."
            "CountdownSelectionHonorsIndependentCategories",
            [t["name"] for t in plan["selected"]],
        )

    def select(self, extra=(), system="Linux", exclude=""):
        discovery = {"tests": required_tests(system) + list(extra)}
        return headless_ctest.select_tests(discovery, PROFILE, system, exclude)

    def test_windows_requires_status_and_provider_diagnostics(self):
        plan = self.select(system="Windows")
        self.assertEqual([], plan["missing_required"])
        self.assertEqual(
            4,
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
            sorted(PROFILE["mandatory_windows"]), plan["missing_required"]
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

    def permission_skip(self):
        result = next(
            r
            for r in self.results
            if r["name"]
            == (
                "RekordboxImportTest."
                "ExistingUnreadableAnalyzeFilePreservesTrackState"
            )
        )
        result["skipped"] = [{"message": "SKIP_RETURN_CODE"}]
        result["attributes"]["status"] = "notrun"
        result["output"] = (
            "Filesystem or current user still permits reading the fixture"
        )
        return result

    def test_windows_permission_skip_is_qualified_not_pass(self):
        result = self.permission_skip()
        policy = self.plan["qualified_skips"][result["name"]]
        self.assertTrue(headless_ctest.qualified_skip(result, policy))
        self.assertEqual("notrun", result["attributes"]["status"])
        self.assertEqual(
            ([], []), headless_ctest.validate_results(self.results, self.plan)
        )

    def test_linux_permission_skip_is_not_qualified(self):
        result = self.permission_skip()
        plan = headless_ctest.select_tests(
            {"tests": required_tests()}, PROFILE, "Linux"
        )
        results = [r for r in self.results if r["name"] in plan["required"]]
        errors, required = headless_ctest.validate_results(results, plan)
        self.assertEqual([], errors)
        self.assertEqual([result["name"]], required)

    def test_windows_unrelated_skip_reason_is_not_qualified(self):
        result = self.permission_skip()
        result["output"] = "missing provider"
        _, required = headless_ctest.validate_results(self.results, self.plan)
        self.assertEqual([result["name"]], required)

    def test_windows_other_parser_skip_is_not_qualified(self):
        result = next(
            r
            for r in self.results
            if r["name"] == "RekordboxImportTest.CreatesHotCueAtFirstIndex"
        )
        result["skipped"] = [{"message": "SKIP_RETURN_CODE"}]
        result["attributes"]["status"] = "notrun"
        result["output"] = (
            "Filesystem or current user still permits reading the fixture"
        )
        _, required = headless_ctest.validate_results(self.results, self.plan)
        self.assertEqual([result["name"]], required)

    def test_windows_permission_skip_with_failure_is_not_qualified(self):
        result = self.permission_skip()
        result["failures"] = [{"message": "failure"}]
        errors, required = headless_ctest.validate_results(
            self.results, self.plan
        )
        self.assertTrue(errors)
        self.assertEqual([result["name"]], required)

    def test_windows_permission_case_still_required_and_enabled(self):
        name = self.permission_skip()["name"]
        tests = [t for t in required_tests("Windows") if t["name"] != name]
        missing = headless_ctest.select_tests(
            {"tests": tests}, PROFILE, "Windows"
        )
        self.assertEqual([name], missing["missing_required"])
        tests.append(case(name, disabled=True))
        disabled = headless_ctest.select_tests(
            {"tests": tests}, PROFILE, "Windows"
        )
        self.assertEqual([name], disabled["missing_required"])

    def test_junit_error_cannot_qualify_as_permission_skip(self):
        result = self.permission_skip()
        result["errors"] = [{"message": "error"}]
        errors, required = headless_ctest.validate_results(
            self.results, self.plan
        )
        self.assertTrue(errors)
        self.assertEqual([result["name"]], required)


class OptionalFeatureRequiredTest(unittest.TestCase):
    # AI-generated QML configuration regressions begin.
    def selection(self, extra, cache):
        return headless_ctest.select_tests(
            {"tests": required_tests() + extra},
            PROFILE,
            "Linux",
            cache_text=cache,
        )

    def test_qml_off_retains_all_common_requirements(self):
        self.assertEqual(
            [], self.selection([], "QML:BOOL=OFF\n")["missing_required"]
        )

    def test_qml_on_requires_every_enabled_model_case(self):
        names = PROFILE["mandatory_features"]["QML"]
        self.assertEqual(
            [],
            self.selection([case(n) for n in names], "QML:BOOL=ON\n")[
                "missing_required"
            ],
        )

    def test_qml_on_rejects_missing_model_group(self):
        names = PROFILE["mandatory_features"]["QML"]
        self.assertEqual(
            sorted(names),
            self.selection([], "QML:BOOL=ON\n")["missing_required"],
        )

    def test_qml_on_rejects_disabled_model_case(self):
        names = PROFILE["mandatory_features"]["QML"]
        entries = [case(n, disabled=(i == 0)) for i, n in enumerate(names)]
        self.assertEqual(
            [names[0]],
            self.selection(entries, "QML:BOOL=ON\n")["missing_required"],
        )

    def test_actual_cache_must_name_feature(self):
        with self.assertRaisesRegex(
            ValueError, "Missing explicit feature configuration"
        ):
            self.selection([], "QT6:BOOL=ON\n")

    # End AI-generated QML configuration regressions.


if __name__ == "__main__":
    unittest.main()
