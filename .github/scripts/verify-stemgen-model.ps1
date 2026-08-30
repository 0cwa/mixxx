$ErrorActionPreference = "Stop"

$modelPath = Join-Path $env:GITHUB_WORKSPACE "models/htdemucs.onnx"
$expectedSize = 304413278
$expectedSha256 = "db37d1314ac1e1051e7978d25ef45b3f1d3f43c837678752f592c0f2deca752d"
$lfsPointerHeader = "version https://git-lfs.github.com/spec/v1"

if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) {
    throw "Stemgen model is missing or is not a regular file: $modelPath"
}

$model = Get-Item -LiteralPath $modelPath
$headerBytes = [System.Text.Encoding]::ASCII.GetBytes($lfsPointerHeader)
$prefixBytes = [byte[]]::new($headerBytes.Length)
$stream = [System.IO.File]::OpenRead($modelPath)
try {
    $bytesRead = $stream.Read($prefixBytes, 0, $prefixBytes.Length)
}
finally {
    $stream.Dispose()
}

if ($bytesRead -eq $headerBytes.Length -and
        [System.Text.Encoding]::ASCII.GetString($prefixBytes) -eq $lfsPointerHeader) {
    throw "Stemgen model is a Git LFS pointer, not materialized model content: $modelPath"
}

if ($model.Length -ne $expectedSize) {
    throw "Stemgen model has size $($model.Length) bytes; expected $expectedSize bytes: $modelPath"
}

$actualSha256 = (Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualSha256 -ne $expectedSha256) {
    throw "Stemgen model SHA-256 is $actualSha256; expected $expectedSha256: $modelPath"
}

Write-Host "Verified Stemgen model: $modelPath ($($model.Length) bytes, SHA-256 $actualSha256)"
