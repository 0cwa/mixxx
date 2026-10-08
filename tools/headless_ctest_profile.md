<!-- AI-generated validation profile documentation begins. -->

# Headless CI validation profile

This local review branch offers source-classified headless validation in
`develop.yml`, for both reusable build tests and instrumented coverage tests.
It is intended for the current Forest candidate, not a general change to
upstream test policy.
Publishing or dispatching this branch requires separate approval.

Only manual dispatch with `headless_tests: true` selects this diagnostic profile.
The input defaults to false; ordinary push/PR events and manual dispatch without
the opt-in retain full tests. This is not an upstream release/test-policy change.
The full existing build/check matrix remains; there is no Windows-only option.

Headless dispatch requires `expected_head_sha`, the full reviewed commit SHA.
Each pre-commit/build/check job immediately rejects a missing, invalid, or
different SHA after checkout, including cross-compiled builds, clazy, and
clang-tidy. Diagnostic runs also disable the download-manifest publishing job.
Each test job records its actual commit, tree,
source tree, profile checksum, platform, and GitHub run identity.

`headless_ctest_profile.json` classifies source definitions, including conditional
Windows Media Foundation and macOS definitions. The helper discovers the actual
CTest cases on each runner and resolves parameterized cases through their exact
GTest filters. It does not reuse a Linux count or fixed Linux list. Unknown
definitions, wildcard commands, unexpected executables, changed source bytes,
and a changed source tree or CMake test definitions block execution.

The profile retains the broad Linux headless baseline. Its additional exclusion
is the inherited, source-disabled CoreServices initialization case. Visual QML,
window, screenshot, widget, skin, image, controller-screen, mapping/hardware, and
network-dependent families remain excluded. Core application initialization and
external AudioUnit plug-in instantiation are also excluded. The macOS dispatch
group regression remains selected without instantiating a plug-in.

The test harness still creates its shared QApplication infrastructure. Selecting
the offscreen platform is supplementary; source classification excludes GUI
test bodies. This profile does not claim real GUI or audio/device acceptance.

The Windows x64 and ARM64 configurations retain `MEDIAFOUNDATION=ON` and native
test compilation. Windows testing stays serial, and the inherited ARM64 AutoDJ
exclusion remains. All three Media Foundation regressions, both shutdown tests,
the provider caching-reader regression, representative reader tests, and all
15 public parser regressions must be discovered, selected, and enabled. The
Linux parser cases must pass without skips. On Windows, only the unreadable-file
permission fixture may have a qualified skip: Qt file permissions do not enforce
an NTFS unreadable ACL. The recorded output must contain one of that fixture's
two explicit permission limitations, and there must be no failure/error. Its
receipt reports a qualified skip, never a pass. The other 14 parser cases, all
three MF cases, both shutdown cases, and required provider/reader cases must pass
without skips. A missing or disabled permission fixture still fails selection.
Other available provider and caching-reader definitions remain
selected. Conditional provider availability and skips are recorded per runner.

The helper writes complete discovery, selected/excluded/unknown cases, source
coverage, disabled cases, and CTest's verified selection before execution. It
uses a CTest index file, avoiding Windows command-line limits and supporting
CMake 3.22. Fixture setup cannot automatically expand the selection. Empty or
incomplete mandatory selections fail. Each process retains the 45-second CTest
timeout. JUnit results must match the selected case set; unqualified required skips, missing
results, duplicates, or failures fail validation. The workflow preserves logs,
JUnit, per-case skip/failure output, and receipts even when testing fails.

The AppImage product-startup smoke test is disabled for this profile. Flatpak's
staged manifest sets `run-tests: false`; the action's existing default input
remains unchanged. Its builds are packaging-only and carry no Flatpak
test/runtime coverage claim. Non-headless callers use the original manifest
and the original action inputs. Cross-compiled macOS ARM64 and
Android continue to skip tests, as before. Other callers retain the reusable
workflows' default full test behavior; approval for this profile must identify
`develop.yml` and the exact reviewed ref/commit.

The coverage build remains instrumented, but the diagnostic profile does not
generate or upload an LCOV/Coveralls report. Its selected test results do not
replace the ordinary full-suite coverage baseline.

Local selector checks and saved/synthetic discovery prove selection behavior;
they are not Windows compilation, native discovery, or matrix pass receipts.
The earlier normal build/unit receipts remain bound to production/test source
at `1c0b415cead1b26e37a37b196e82a8c6ab30fb0b`. This CI-only branch leaves those
source bytes unchanged. No additional crash-causality claim is made.

<!-- End AI-generated validation profile documentation. -->
