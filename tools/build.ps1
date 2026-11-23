[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",
    [string]$Config = "RelWithDebInfo",
    [string]$BuildDir = "",
    [switch]$NoTest,
    [switch]$CoreOnly,
    [switch]$StaticRuntime
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if (!$BuildDir) { $BuildDir = Join-Path $root "build/$Arch" }
$vswhere = Join-Path "${env:ProgramFiles(x86)}" 'Microsoft Visual Studio/Installer/vswhere.exe'
$installs = @(& $vswhere -all -products '*' -property installationPath)
$vs = $installs | Where-Object {
    $tools = Join-Path $_ 'VC/Tools/MSVC'
    Get-ChildItem -LiteralPath $tools -Directory -ErrorAction SilentlyContinue | Where-Object {
        Test-Path -LiteralPath (Join-Path $_.FullName "bin/Hostx64/$Arch/cl.exe")
    } | Select-Object -First 1
} | Select-Object -First 1
if (!$vs) { throw "Visual Studio with the $Arch C++ tools is required." }

# VsDevCmd expands PATH inside cmd.exe conditions. Set aside entries with
# cmd metacharacters until the PowerShell developer environment has loaded.
$previousPath = $env:Path
$entries = $previousPath -split ';' | Where-Object { $_ }
$env:Path = ($entries | Where-Object { $_ -notmatch '[()&!^"]' }) -join ';'
try {
    $target = if ($Arch -eq 'x64') { 'amd64' } else { 'arm64' }
    & (Join-Path $vs 'Common7/Tools/Launch-VsDevShell.ps1') -Arch $target -HostArch amd64 -SkipAutomaticLocation -NoLogo
} finally {
    $env:Path += ';' + (($entries | Where-Object { $_ -match '[()&!^"]' }) -join ';')
}
if (!(Get-Command cl -ErrorAction SilentlyContinue)) { throw "MSVC environment setup failed." }

$desktop = if ($CoreOnly) { 'OFF' } else { 'ON' }
$options = @("-DRECIF_DESKTOP=$desktop", "-DCMAKE_BUILD_TYPE=$Config")
if ($StaticRuntime) { $options += '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded' }
if ($Arch -eq 'arm64') { $options += '-DCMAKE_SYSTEM_NAME=Windows', '-DCMAKE_SYSTEM_PROCESSOR=ARM64' }
cmake -S (Join-Path $root 'src') -B $BuildDir -G Ninja @options
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
cmake --build $BuildDir
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if (!$NoTest -and $Arch -eq 'x64') {
    ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Verification failed.' }
}
