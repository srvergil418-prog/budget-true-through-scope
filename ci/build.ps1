# ReticleCenter CI build (仓库根目录布局)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root      = Split-Path -Parent $PSScriptRoot          # 仓库根
$commonlib = Join-Path $root 'CommonLibF4'

Write-Host '============================================================'
Write-Host 'RETICLE_CENTER_BUILD_MARKER_02'
Write-Host '============================================================'
Write-Host "root      = $root"
Write-Host "commonlib = $commonlib"

if (-not (Test-Path $commonlib)) {
    throw "CommonLibF4 not found at $commonlib"
}

$buildDir = Join-Path $root 'build'
Write-Host '=== CMake configure ==='
cmake -S $root -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

Write-Host '=== CMake build ==='
cmake --build $buildDir --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

$dll = Get-ChildItem $buildDir -Recurse -Filter 'ReticleCenter.dll' | Select-Object -First 1
if (-not $dll) { throw 'ReticleCenter.dll not produced' }
Write-Host "=== OK: $($dll.FullName)  ($($dll.Length) bytes) ==="
