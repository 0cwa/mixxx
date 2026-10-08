<!-- AI-generated source/fixture diagnostic review begins. -->

# Source-classified headless diagnostic profile

Manual `develop.yml` dispatch with `headless_tests: true` and the exact reviewed
`expected_head_sha` is the only hosted opt-in. It defaults to false; ordinary
push/PR and manual full-suite behavior retain their existing configuration.
Publishing and hosted execution remain subject to separate human approval.

The fixed profile assigns explicit policy to all 144 test-source files and 1,138
source definitions at its pinned source tree. There is no permissive default.
Per-source fixture and call-site review conservatively excludes visual/QML,
widget, rendering, image, inherited library/cover-art/controller integration,
unknown legacy fixture, hardware/mapping validation and network families. The
alignment renderer family and waveform-factory config fixture are excluded;
marker taxonomy/control-order tests and numeric VisualPlayPosition oracles remain.
The shared test QApplication infrastructure remains. This is a source/fixture
classification audit, not independent human certification or GUI acceptance.

TrackExporter has an active plain testing::Test fixture, a present header,
temporary directories and QObject signal/slot overwrite and cancel answers.
Its seven cases remain an explicit file-export integration scope exclusion;
their exclusion does not assert missing source, inactivity or GUI behavior.

The public Forest `interfaceqml_test.cpp` blob is absent from this candidate.
Its two public cases would be rejected as unknown if discovered. Any change to
the pinned src tree, CMake bytes, file hashes, definitions or discovered commands
requires a new classification review. Conditional/platform absent cases are
recorded; a Linux discovery list is never treated as Windows discovery.

macOS headless builds pass `AU_EFFECTS=OFF`. Actual macOS execution also checks its
CMake cache and refuses missing/ON AudioUnit configuration: EffectsManager and
EffectsBackendManager fixtures can reach installed AudioUnits. This diagnostic
has no AudioUnit integration coverage. Ordinary configuration keeps its existing
default. LV2 enumeration remains enabled; this does not claim physical/device or
external plug-in runtime acceptance.

Windows retains MEDIAFOUNDATION=ON for x64 and ARM64. Its 12 additional mandatory
cases comprise three original MF stream-status regressions, three explicit
provider diagnostics, the public-read late-post-seek tick diagnostic, and five
source/proxy open/seek/read definitions. Both shutdown tests, provider chunk-jump,
five representative reader cases and all 15 public parser cases remain mandatory.
The seven HID report-descriptor data cases are also mandatory on every native
diagnostic platform. The common hosted Configure step already passes HID=ON;
an absent or disabled HID data case now blocks the diagnostic rather than
silently reducing coverage. Cross-compiled and packaging-only lanes retain
their explicit no-test scope.
A provider loop that continues on unsupported input is insufficient evidence.
The new direct MF source, forced registered MF proxy, and forced proxy-to-cache
chunk tests require the local WAV to open and reject provider fallback. They
assert forward/backward ranges and sample equivalence and record provider/read
properties. The chunk test exercises CachingReaderChunk buffering; it does not
prove the asynchronous CachingReaderWorker/ReadAheadManager chain.

Windows diagnostic execution sets `MIXXX_HEADLESS_MF_DIAGNOSTICS=1`. The new cases
otherwise skip as explicit diagnostics; in this profile every mandatory case
must run without skips. The late-post-seek tick test uses the public read entry,
with a real MF source-reader seek and injected F<T<S<D events. It requires
successful target-frame/sample recovery. This is an unresolved native runtime
success requirement and may expose the existing conservative overshoot failure;
no production repair or native pass is claimed. The original three status tests
alone do not close this real-caller success contract.

On Windows only ExistingUnreadableAnalyzeFilePreservesTrackState may have its
existing qualified permission-fixture skip: it must be present/enabled, carry one
of the two exact filesystem permission limitations, and contain no failure/error.
Its receipt records a qualified skip rather than a pass. The other 14 parser
cases and all other mandatory cases require passes without skips. Linux requires
all 15 parser cases without skips. Windows stays serial and preserves the ARM64
AutoDJ exclusion (the conservative source policy also excludes its library fixture).

Selection uses actual runner CTest discovery, exact GTest definitions, finite
45-second process timeouts and fixture-expansion suppression. JUnit must match
the selected case set; missing, duplicate, failed or unqualified mandatory skips
fail. SHA/tree/profile/config/discovery/selection/results and logs are retained.

AppImage product-startup smoke remains disabled. Flatpak diagnostic staging
sets run-tests:false and preserves ordinary inputs; its diagnostic coverage is
packaging only. Instrumented diagnostics produce no LCOV/Coveralls baseline.
Cross-compiled Android/macOS ARM64 keep their existing no-test behavior. The
full existing matrix remains. No real DJ/audio acceptance or crash-causality
claim is made, and no prior cross-SHA binary receipt is substituted here.

<!-- End AI-generated source/fixture diagnostic review. -->
