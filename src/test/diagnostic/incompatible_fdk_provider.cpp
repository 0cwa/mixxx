/*
 * Diagnostic-only provider for issue #83 native acceptance.
 * Copied from tmp/issue83-fdk-null-rejection/fixtures/incompatible_fdk_provider.cpp
 * (original SHA-256 cb82d015b5087f04268f64a9260c6340ec32464b295073737bd5a617d3a9cd52).
 * The fixture intentionally omits aacEncInfo; CI builds it into a private
 * temporary library path. It is not linked into or injected into Mixxx.
 */
// Minimal symbol-presence fixture for the EncoderFdkAac rejection path.
// The signatures are immaterial: the encoder resolves these names, then rejects
// the provider before calling any function because aacEncInfo is absent.
extern "C" {
void aacEncGetLibInfo() {}
void aacEncOpen() {}
void aacEncClose() {}
void aacEncEncode() {}
void aacEncoder_SetParam() {}
// Intentionally omit aacEncInfo.
}
