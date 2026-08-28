# Bundled Stemgen model

This directory contains the package-owned `htdemucs.onnx` model used by the
Stemgen ONNX runtime path. Its exact provenance and integrity data are in
`htdemucs.onnx.manifest.json`; the matching distribution notice is in
`NOTICE.md`.

The model is the single non-fine-tuned four-source HTDemucs v4 export. Mixxx
passes stereo, 44.1 kHz, float32 chunks with shape `[1, 2, 343980]` and expects
four stereo source outputs for drums, bass, other, and vocals. This is the
runtime contract implemented by `StemConverter`.

The file is Git LFS content. A checkout used for packaging must materialize
the LFS object before configuring CMake; a pointer is not a usable model.
