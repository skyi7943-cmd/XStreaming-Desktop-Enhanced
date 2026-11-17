$ErrorActionPreference = 'Stop'
# XStreaming Desktop Enhanced - 原生 C++20 神经渲染与低延迟 X2 补帧引擎一键编译脚本

$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$zigCandidates = @(
    'E:\DLSS5-Swapper-Enhanced\tools\zig-x86_64-windows-0.14.1\zig.exe',
    (Join-Path $projectRoot 'tools\zig\zig.exe'),
    'zig.exe'
)
$zig = $zigCandidates | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
if (!$zig) {
    throw '未找到 zig.exe 编译器，请确认 E:\DLSS5-Swapper-Enhanced\tools\zig-x86_64-windows-0.14.1\zig.exe 存在。'
}

if (Test-Path 'E:\DLSS5-Swapper-Enhanced\tools\zig-cache') {
    $env:ZIG_GLOBAL_CACHE_DIR = 'E:\DLSS5-Swapper-Enhanced\tools\zig-cache'
    $env:ZIG_LOCAL_CACHE_DIR  = 'E:\DLSS5-Swapper-Enhanced\tools\zig-local-cache'
}

$binDir = Join-Path $PSScriptRoot 'bin'
New-Item -ItemType Directory -Force -Path $binDir | Out-Null

$srcFile = Join-Path $PSScriptRoot 'src\xstreaming_enhance_engine.cpp'
$outExe  = Join-Path $binDir 'xstreaming-enhance-engine.exe'

Write-Host "[1/2] 正在编译超低延迟原生引擎: $outExe ..."
& $zig c++ `
    -target x86_64-windows-gnu `
    -std=c++20 `
    -O3 `
    -static `
    -DUNICODE `
    -D_UNICODE `
    $srcFile `
    -ldxgi -ld3d11 -ld3d12 -luser32 -lgdi32 -ladvapi32 -ldwmapi -lwinmm -lavrt `
    -o $outExe

if ($LASTEXITCODE -ne 0) {
    throw "xstreaming-enhance-engine.exe 编译失败 (ExitCode=$LASTEXITCODE)"
}

Write-Host "[2/3] 正在执行 NVIDIA RTX 硬件全管线自动化自检 (--self-test) ..."
$testOut = & $outExe --self-test
Write-Host $testOut
if ($LASTEXITCODE -ne 0) {
    throw "NVIDIA 硬件管线自检未通过 (ExitCode=$LASTEXITCODE)"
}

Write-Host "[3/3] 正在执行 AMD Radeon (RadeonNR + 三级金字塔双向光流) 全管线实机自检 (--self-test-amd) ..."
$testAmdOut = & $outExe --self-test-amd
Write-Host $testAmdOut
if ($LASTEXITCODE -ne 0) {
    throw "AMD Radeon 硬件管线自检未通过 (ExitCode=$LASTEXITCODE)"
}
Write-Host "✅ 原生双架构 (NVIDIA DLSS 5/NVOFA + AMD RadeonNR/PyrFlow) 低延迟 X2 插帧引擎编译与实机验证全部通过！"
