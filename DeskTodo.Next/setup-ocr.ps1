[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$dependencyRoot = Join-Path $PSScriptRoot 'deps\ocr'
$downloadRoot = Join-Path $dependencyRoot 'downloads'
$runtimeRoot = Join-Path $dependencyRoot 'runtime'
New-Item -ItemType Directory -Path $downloadRoot, $runtimeRoot -Force | Out-Null

function Get-VerifiedFile([string]$Url, [string]$Path, [string]$Hash) {
    if ((Test-Path -LiteralPath $Path) -and $Hash -and
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -eq $Hash) { return }
    Write-Output "Downloading $([IO.Path]::GetFileName($Path))"
    & curl.exe --fail --location --silent --show-error --retry 3 --connect-timeout 30 --output ($Path + '.part') $Url
    if ($LASTEXITCODE -ne 0) { throw "Download failed: $Url" }
    if ($Hash -and (Get-FileHash -LiteralPath ($Path + '.part') -Algorithm SHA256).Hash -ne $Hash) {
        throw "SHA256 mismatch: $Path"
    }
    Move-Item -LiteralPath ($Path + '.part') -Destination $Path -Force
}

$ortVersion = '1.31.0'
$ortPackage = Join-Path $downloadRoot "onnxruntime-win-x64-$ortVersion.zip"
Get-VerifiedFile "https://github.com/microsoft/onnxruntime/releases/download/v$ortVersion/onnxruntime-win-x64-$ortVersion.zip" $ortPackage '3958d8a44984160692c1b4ffd0de3cb48f8c39f9d34bb31e433db06e30b82195'
$ortRoot = Join-Path $dependencyRoot "onnxruntime-win-x64-$ortVersion"
if (-not (Test-Path -LiteralPath (Join-Path $ortRoot 'include\onnxruntime_cxx_api.h'))) {
    Expand-Archive -LiteralPath $ortPackage -DestinationPath $dependencyRoot -Force
}
Copy-Item -LiteralPath (Join-Path $ortRoot 'lib\onnxruntime.dll') -Destination $runtimeRoot -Force
Get-ChildItem -LiteralPath (Join-Path $ortRoot 'lib') -Filter 'onnxruntime_providers_*.dll' |
    Copy-Item -Destination $runtimeRoot -Force

$opencvPackage = Join-Path $downloadRoot 'opencv-4.12.0-windows.exe'
Get-VerifiedFile 'https://github.com/opencv/opencv/releases/download/4.12.0/opencv-4.12.0-windows.exe' $opencvPackage 'b753b14d880b9bc8d89d6acd3b665c040baec0211078435432fcae117db707af'
$opencvRoot = Join-Path $dependencyRoot 'opencv'
if (-not (Test-Path -LiteralPath (Join-Path $opencvRoot 'build\include\opencv2\core.hpp'))) {
    $extractor = Start-Process -FilePath $opencvPackage -ArgumentList @('-y', ('-o' + $dependencyRoot)) -WindowStyle Hidden -Wait -PassThru
    if ($extractor.ExitCode -ne 0) { throw 'OpenCV extraction failed' }
}
Copy-Item -LiteralPath (Join-Path $opencvRoot 'build\x64\vc16\bin\opencv_world4120.dll') -Destination $runtimeRoot -Force
Copy-Item -LiteralPath (Join-Path $opencvRoot 'build\LICENSE') -Destination (Join-Path $runtimeRoot 'OpenCV-LICENSE') -Force
$opencvLicenses = Join-Path $runtimeRoot 'OpenCV-licenses'
New-Item -ItemType Directory -Path $opencvLicenses -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $opencvRoot 'build\etc\licenses') |
    Copy-Item -Destination $opencvLicenses -Recurse -Force

$modelBase = 'https://www.modelscope.cn/models/RapidAI/RapidOCR/resolve/v3.10.0/onnx/PP-OCRv6'
Get-VerifiedFile "$modelBase/det/PP-OCRv6_det_medium.onnx" (Join-Path $runtimeRoot 'PP-OCRv6_det_medium.onnx') '92078b7355007ccfffcd4c8cd441a3afd4538904d06881b29a155e1e679907c2'
Get-VerifiedFile "$modelBase/rec/PP-OCRv6_rec_medium.onnx" (Join-Path $runtimeRoot 'PP-OCRv6_rec_medium.onnx') 'eef444829dbbe18d7fea59a3f6eb75647518d2b3a9568d27c92e42940204894b'
Get-VerifiedFile 'https://raw.githubusercontent.com/PaddlePaddle/PaddleOCR/main/LICENSE' (Join-Path $runtimeRoot 'PaddleOCR-LICENSE') '3840c5c0c61c294264d2dd77b8777be6ddd90121ef4e0e64abcd22edea581d6e'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\ocr\NOTICE.md') -Destination (Join-Path $runtimeRoot 'NOTICE.md') -Force

foreach ($license in @('LICENSE', 'ThirdPartyNotices.txt')) {
    $licensePath = Join-Path $ortRoot $license
    if (Test-Path -LiteralPath $licensePath) {
        Copy-Item -LiteralPath $licensePath -Destination (Join-Path $runtimeRoot ('ONNXRuntime-' + $license)) -Force
    }
}
Write-Output "Offline OCR dependencies ready: $runtimeRoot"
