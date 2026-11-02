<!-- AI-generated dependency documentation; human review is required before submission. -->

# Signalsmith build dependencies

With `-DSIGNALSMITH=ON`, Mixxx looks for suitable installed headers and falls
back to the bundled dependencies. Configuring or building Signalsmith does not
download anything. The selected source is printed during configuration.

| Dependency | Exact upstream snapshot | License |
| --- | --- | --- |
| Stretch | `57b93f4e9206a089a45387eaa39bdc9f310d3308` | [MIT](../signalsmith-stretch/LICENSE.txt) |
| Linear | `5668673560146a9cfe38c25315071e3fd68c8317` | [MIT](../signalsmith-linear/LICENSE.txt) |

The sources are [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch)
and [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear). Stretch's
snapshot reports version 1.3.2 but is not a release tag. Its upstream CMake file
selects Linear tag 0.3.1, which resolves to the snapshot above. The vendored
headers and license files are unchanged copies of those commits. All upstream
platform headers are retained. Both MIT notices are installed with Mixxx.

`SIGNALSMITH_SOURCE` selects one of three dependency modes:

- `AUTO` (default) uses installed headers when the version and API checks pass,
  otherwise the bundled snapshots.
- `SYSTEM` requires Stretch 1.3.2 or newer and Linear headers from the
  toolchain's normal search paths. A missing or incompatible installation fails
  configuration instead of downloading a replacement or disabling the engine.
- `BUNDLED` always uses the exact snapshots above, including in offline builds.

Distribution and vcpkg builds can select `-DSIGNALSMITH_SOURCE=SYSTEM`.
Nonstandard installations can set `Signalsmith_STRETCH_INCLUDE_DIR` and
`Signalsmith_LINEAR_INCLUDE_DIR`. CMake compiles the API used by the scaler
before accepting installed headers, including `setFormantFactor` and `process`.

Linear's upstream backend options remain available: Accelerate defaults to ON
on Apple platforms; IPP, PFFFT and double-precision PFFFT default to OFF. As in
upstream Linear, PFFFT users must provide the corresponding library themselves.

<!-- End of AI-generated dependency documentation. -->
