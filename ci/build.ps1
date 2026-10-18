# ReticleCenter CI build（仓库根目录布局）
#
# 除了构建自己，还负责把 powerof3/CommonLibF4 master 上当前编译不过/ABI 不适配
# 的地方打上补丁（这套补丁来自 TrueThroughScope 的 CI，已在其构建里验证过）：
#   1) hkVector4.h    GetNormalized() 返回引用 -> 返回值（MSVC 编不过）
#   2) API.cpp        比 fmt v12 要求 .string()
#   3) Interfaces.h   F4SE 0.6.23（游戏 1.10.163）没有 GetSaveFolderName，
#                     留着会在 F4SE::Init 里跳到空指针，插件一加载就崩
#   4) REX/LOG.cpp    宽字符 LOG 重载必须先转 UTF-8 才能进 spdlog
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root        = Split-Path -Parent $PSScriptRoot
$commonlibf4 = Join-Path $root 'CommonLibF4'

Write-Host '============================================================'
Write-Host 'RETICLE_CENTER_BUILD_MARKER_04'
Write-Host '============================================================'
Write-Host "root       = $root"
Write-Host "commonlibf4= $commonlibf4"
Write-Host "vcpkg      = $env:VCPKG_INSTALLATION_ROOT"

if (-not (Test-Path $commonlibf4)) { throw "CommonLibF4 not found at $commonlibf4" }

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
function Write-NoBom([string]$path, [string]$text) { [System.IO.File]::WriteAllText($path, $text, $utf8NoBom) }

# ================================================================ 0) 把 ImageSpaceEffect.h 换成 dev-1.10.163 的版本
#
# master 的这个头文件里，bDisable 等是
#     inline static REL::Relocation<bool*> bDisable{ REL::ID(2713225) };
# —— class-scope static，DLL 一加载（早于 DllMain）就初始化，而 2713225 是
# 新版本游戏的 ID，1.10.163 的地址库里没有 -> 直接弹
#     REL/IDDB.cpp(127): id 2713225 not found!
# 并用 MessageBox 终止进程。dev-1.10.163 分支同一位置是 1500590，对得上。
$clf4 = Join-Path $root 'CommonLibF4'
$target = Join-Path $clf4 'CommonLibF4/include/RE/Bethesda/ImageSpaceEffect.h'
if ((Test-Path (Join-Path $clf4 '.git')) -and (Test-Path $target)) {
    Push-Location $clf4
    try {
        git fetch --depth=1 origin dev-1.10.163 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'git fetch dev-1.10.163 failed' }
        $devText = git show FETCH_HEAD:CommonLibF4/include/RE/Bethesda/ImageSpaceEffect.h 2>&1
        if ($LASTEXITCODE -ne 0) { throw 'git show ImageSpaceEffect.h failed' }
        $devText = ($devText -join "`r`n")
        if ($devText -notmatch 'REL::ID') { throw 'ImageSpaceEffect.h from dev has no REL::ID -- unexpected' }
        [System.IO.File]::WriteAllText($target, $devText, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host '  [0/4] ImageSpaceEffect.h -> dev-1.10.163 IDs' -ForegroundColor Green
    } finally {
        Pop-Location
    }
} else {
    Write-Host '  [0/4] ImageSpaceEffect.h overlay skipped' -ForegroundColor Yellow
}

# ================================================================ 1) hkVector4.h
$hv = Join-Path $commonlibf4 'CommonLibF4/include/RE/Havok/hkVector4.h'
if (Test-Path $hv) {
    $t = [System.IO.File]::ReadAllText($hv)
    $t2 = $t.Replace('hkVector4f& GetNormalized()', 'hkVector4f GetNormalized()')
    if ($t2 -eq $t) { Write-Host '  [1/4] hkVector4.h: pattern not found' -ForegroundColor Yellow }
    else { Write-NoBom $hv $t2; Write-Host '  [1/4] hkVector4.h patched' -ForegroundColor Green }
}

# ================================================================ 2) API.cpp
$api = Join-Path $commonlibf4 'CommonLibF4/src/F4SE/API.cpp'
if (Test-Path $api) {
    $a = [System.IO.File]::ReadAllText($api)
    $a2 = $a.Replace(
        'log::info("{} v{}", GetPluginName(), GetPluginVersion());',
        'log::info("{} v{}", GetPluginName(), GetPluginVersion().string());')
    if ($a2 -eq $a) { Write-Host '  [2/4] API.cpp: pattern not found' -ForegroundColor Yellow }
    else { Write-NoBom $api $a2; Write-Host '  [2/4] API.cpp patched' -ForegroundColor Green }
}

# ================================================================ 3) F4SE/Interfaces.h -- 0.6.23 ABI
# dev-1.10.163 分支本身对得上 1.10.163，这段多半是 no-op；只在真的匹配上时才改。
$intf = Join-Path $commonlibf4 'CommonLibF4/include/F4SE/Interfaces.h'
if (Test-Path $intf) {
    $intfText = [System.IO.File]::ReadAllText($intf)
    $intfOrig = $intfText
    $intfText = $intfText.Replace(
        'const char*(F4SEAPI* GetSaveFolderName)(void);',
        '/* [RC CI] GetSaveFolderName is F4SE 0.7.1+; the 1.10.163 loader (0.6.23) has no such entry */')
    $intfText = $intfText.Replace('return GetProxy().GetSaveFolderName();', 'return {};')
    if ($intfText -eq $intfOrig) {
        Write-Host '  [3/4] Interfaces.h: nothing to patch (already 1.10.163-compatible)' -ForegroundColor Yellow
    } else {
        Write-NoBom $intf $intfText
        Write-Host '  [3/4] Interfaces.h patched (0.6.23 ABI)' -ForegroundColor Green
    }
}

# ================================================================ 4) REX/LOG.cpp
$lg = Join-Path $commonlibf4 'CommonLibF4/src/REX/REX/LOG.cpp'
if (Test-Path $lg) {
    $g = [System.IO.File]::ReadAllText($lg)
    if ($g -notmatch 'REX/REX/CONVERT\.h') {
        $g = $g.Replace('#include "REX/W32/USER32.h"', "#include `"REX/W32/USER32.h`"`r`n#include `"REX/REX/CONVERT.h`"")
    }
    $rx = [regex]"(?s)void LOG\(const std::source_location a_loc, const LOG_LEVEL a_level, const std::wstring_view a_fmt\)\s*\{.*?\r?\n\t\}"
    $m = $rx.Match($g)
    if (-not $m.Success) {
        Write-Host '  [4/4] LOG.cpp: wide overload not found' -ForegroundColor Yellow
        Write-NoBom $lg $g
    } else {
        $repl = "void LOG(const std::source_location a_loc, const LOG_LEVEL a_level, const std::wstring_view a_fmt)`r`n`t{`r`n`t`tstd::string narrow;`r`n`t`tif (!REX::UTF16_TO_UTF8(a_fmt, narrow)) {`r`n`t`t`tnarrow = `"<encoding error>`";`r`n`t`t}`r`n`t`tLOG(a_loc, a_level, std::string_view{ narrow });`r`n`t}"
        $g2 = $g.Remove($m.Index, $m.Length).Insert($m.Index, $repl)
        Write-NoBom $lg $g2
        Write-Host '  [4/4] LOG.cpp patched' -ForegroundColor Green
    }
}
Write-Host ''

# ================================================================ configure + build
$toolchain = "$env:VCPKG_INSTALLATION_ROOT/scripts/buildsystems/vcpkg.cmake"
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
