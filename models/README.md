# Bundled Stemgen model

This directory contains the package-owned `htdemucs.onnx` model used by the
Stemgen ONNX runtime path. Its exact provenance and integrity data are in
`htdemucs.onnx.manifest.json`; the matching distribution notice is in
`NOTICE.md`.

The model is the single non-fine-tuned four-source HTDemucs v4 export. Mixxx
passes stereo, 44.1 kHz, float32 chunks with shape `[1, 2, 343980]` and expects
four stereo source outputs for drums, bass, other, and vocals. This is the
runtime contract implemented by `StemConverter`.

The repository stores the file as a Git LFS pointer. A checkout used for
packaging must materialize the model before configuring CMake; a pointer is not
a usable model. Model-enabled CI materializes the pinned release asset from
the manifest provenance and verifies its size and SHA-256 before the build.

The `.github/scripts/download-stemgen-model.sh` helper requires
`MIXXX_STEM_MODEL_DIR` to be set explicitly to a staging directory outside the
source checkout. The Flatpak workflow uses the runner's temporary directory;
the helper does not select the tracked `models/` directory by default.
