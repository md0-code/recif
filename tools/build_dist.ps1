[CmdletBinding()]
param([switch]$NoTest)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -LiteralPath (Join-Path $root 'src/VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid source version.' }
$dist = Join-Path $root 'dist'
$releases = Join-Path $dist 'releases'
$null = New-Item -ItemType Directory -Force -Path $releases

$pack = Join-Path $root 'work/disks/unix211-rp06.dsk'

function Add-PackageFiles([string]$build, [string]$destination, [switch]$KeepPack) {
    $docs = Join-Path $destination 'docs'
    $null = New-Item -ItemType Directory -Force -Path $docs
    # The 2.11BSD pack; an unpacked copy in use keeps its contents.
    $disks = Join-Path $destination 'disks'
    $null = New-Item -ItemType Directory -Force -Path $disks
    $target = Join-Path $disks 'unix211-rp06.dsk'
    if (-not ($KeepPack -and (Test-Path -LiteralPath $target))) {
        Copy-Item -LiteralPath $pack -Destination $target -Force
    }
    foreach ($name in @('recif.exe', 'daf2020.exe')) {
        Copy-Item -LiteralPath (Join-Path $build $name) -Destination $destination -Force
    }
    $legacyCli = Join-Path $destination 'recif-cli.exe'
    if (Test-Path -LiteralPath $legacyCli -PathType Leaf) {
        Remove-Item -LiteralPath $legacyCli
    }
    # The READMEs without the lines that show the pictures of assets/, which the archives do not hold.
    foreach ($name in @('README.md', 'README.ro.md')) {
        $text = [System.IO.File]::ReadAllText((Join-Path $root $name))
        $text = [regex]::Replace($text, '(?m)^(?:!\[[^\]]*\]\(assets/[^)]*\)|<p align="center">.*\bassets/.*</p>)\r?\n\r?\n', '')
        [System.IO.File]::WriteAllText((Join-Path $destination $name), $text, [System.Text.UTF8Encoding]::new($false))
    }
    Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $destination -Force
    foreach ($name in @('THIRD_PARTY.md', 'CHANGELOG.md', 'NOTES.md', 'NOTES.ro.md')) {
        Copy-Item -LiteralPath (Join-Path $root "docs/$name") -Destination $docs -Force
    }
}

$archives = foreach ($arch in @('x64', 'arm64')) {
    $build = Join-Path $root "build/$arch"
    $stage = Join-Path $root "build/package-$arch"
    $options = @('-Arch', $arch, '-Config', 'Release', '-StaticRuntime')
    if ($NoTest) { $options += '-NoTest' }
    & pwsh -NoProfile -File (Join-Path $PSScriptRoot 'build.ps1') @options | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Windows $arch build failed." }
    if (-not (Test-Path -LiteralPath $pack)) {
        & python (Join-Path $PSScriptRoot 'unix211.py') --out $pack | Out-Host
        if ($LASTEXITCODE -ne 0) { throw '2.11BSD pack build failed.' }
    }
    Add-PackageFiles $build $stage
    $archive = Join-Path $releases "Recif-$version-windows-$arch.zip"
    $temporary = Join-Path $releases "Recif-$version-windows-$arch.$PID.tmp"
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $temporary)
    Move-Item -LiteralPath $temporary -Destination $archive -Force
    $archive
}
Add-PackageFiles (Join-Path $root 'build/x64') $dist -KeepPack

# The Linux archives, built in WSL Debian 12: the arm64 sysroot and both targets in one session.
$linuxRoot = (& wsl -e wslpath -a $root).Trim()
$linuxArgs = @('sysroot', 'all')
if ($NoTest) { $linuxArgs += '--no-test' }
& wsl -u root -e bash "$linuxRoot/tools/build_linux.sh" @linuxArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'Linux build failed.' }
$archives += @('x64', 'arm64') | ForEach-Object { Join-Path $releases "Recif-$version-linux-$_.tar.gz" }

Get-ChildItem -LiteralPath $releases -File | Where-Object { $_.Name -like 'Recif-*-windows-*.zip' -or $_.Name -like 'Recif-*-linux-*.tar.gz' } |
    Where-Object { $_.FullName -notin $archives } |
    Remove-Item
$archives
