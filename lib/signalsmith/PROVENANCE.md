# Signalsmith vendored dependencies

Mixxx vendors the header-only portions of these Signalsmith Audio snapshots so
native and Flatpak builds do not need network-enabled CMake dependency
acquisition for this dependency.

## Stretch

- Source: https://github.com/Signalsmith-Audio/signalsmith-stretch
- Snapshot: `57b93f4e9206a089a45387eaa39bdc9f310d3308`
- Archive: `signalsmith-stretch-57b93f4e9206a089a45387eaa39bdc9f310d3308.tar.gz`
- SHA-256: `ad02e24334438b203e81d44f6c9906f3c6773e90a4ea923bb3e73d15697187d6`
- License: [MIT](../signalsmith-stretch/LICENSE.txt)

## Linear

- Source: https://github.com/Signalsmith-Audio/linear
- Snapshot: `5668673560146a9cfe38c25315071e3fd68c8317`
- Archive: `linear-5668673560146a9cfe38c25315071e3fd68c8317.tar.gz`
- SHA-256: `91d09ff4924c6958c70b2d182ec2553526fc301176652111b27cb08fc03f532e`
- License: [MIT](../signalsmith-linear/LICENSE.txt)

The archive digests above are the verification inputs for these vendored
snapshots. The source files are retained under their upstream include layout;
the small Mixxx CMake wrapper in this directory only creates `INTERFACE`
targets and does not modify the dependency sources.
