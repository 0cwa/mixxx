$ErrorActionPreference = "Stop"

$modelPath = Join-Path $env:GITHUB_WORKSPACE "models/htdemucs.onnx"
$expectedSize = 304413278
$expectedSha256 = "db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d"
$lfsPointerHeader = "version https://git-lfs.github.com/spec/v1"

$model = Get-Item -LiteralPath $modelPath -Force -ErrorAction SilentlyContinue
if ($null -eq $model -or $model.PSIsContainer -or
        (($model.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw "Stemgen model is missing, a reparse point, or is not a regular file: $modelPath"
}

$stream = [System.IO.FileStream]::new(
    $modelPath,
    [System.IO.FileMode]::Open,
    [System.IO.FileAccess]::Read,
    [System.IO.FileShare]::Read,
    1048576,
    [System.IO.FileOptions]::SequentialScan
)
$digest = [System.Security.Cryptography.SHA256]::Create()
try {
    $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($lfsPointerHeader)
    $prefixBytes = [byte[]]::new($headerBytes.Length)
    $bytesRead = $stream.Read($prefixBytes, 0, $prefixBytes.Length)
    if ($bytesRead -eq $headerBytes.Length -and
            [System.Text.Encoding]::ASCII.GetString($prefixBytes) -eq $lfsPointerHeader) {
        throw "Stemgen model is a Git LFS pointer, not materialized model content: $modelPath"
    }

    $modelSize = $stream.Length
    if ($modelSize -ne $expectedSize) {
        throw "Stemgen model has size $modelSize bytes; expected $expectedSize bytes: $modelPath"
    }

    $stream.Position = 0
    $actualSha256 = [System.Convert]::ToHexString(
        $digest.ComputeHash($stream)
    ).ToLowerInvariant()
}
finally {
    $digest.Dispose()
    $stream.Dispose()
}

if ($actualSha256 -ne $expectedSha256) {
    throw "Stemgen model SHA-256 is $actualSha256; expected $expectedSha256: $modelPath"
}

Write-Host "Verified Stemgen model: $modelPath ($modelSize bytes, SHA-256 $actualSha256)"
