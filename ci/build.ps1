# ReticleCenter CI build（仓库根目录布局）
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root      = Split-Path -Parent $PSScriptRoot          # 仓库根
$commonlib = Join-Path $root 'CommonLibF4'

Write-Host '============================================================'
Write-Host 'RETICLE_CENTER_BUILD_MARKER_03'
Write-Host '============================================================'
Write-Host "root      = $root"
Write-Host "commonlib = $commonlib"
Write-Host "vcpkg     = $env:VCPKG_INSTALLATION_ROOT"

if (-not (Test-Path $commonlib)) {
    throw "CommonLibF4 not found at $commonlib"
}

$toolchain = "$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
Write-Host "toolchain = $toolchain"

$buildDir = Join-Path $root 'build'

Write-Host '=== CMake configure ==='
cmake -S $root -B $buildDir -A x64 `
    -DVCPKG_TARGET_TRIPLET=x64-windows-static-md `
    -DCMAKE_TOOLCHAIN_FILE="$toolchain"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

Write-Host '=== CMake build ==='
cmake --build $buildDir --config Release -- /m
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }

$dll = Get-ChildItem $buildDir -Recurse -Filter 'ReticleCenter.dll' | Select-Object -First 1
if (-not $dll) { throw 'ReticleCenter.dll not produced' }
Write-Host "=== OK: $($dll.FullName)  ($($dll.Length) bytes) ==="
