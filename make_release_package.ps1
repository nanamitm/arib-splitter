param(
    [string]$Version = (Get-Date -Format "yyyyMMdd"),
    [ValidateSet("x64")]
    [string]$Platform = "x64",
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$binDir = Join-Path $repoRoot "bin_$Platform"
$distRoot = Join-Path $repoRoot "dist"
$packageName = "ARIBSplitter-$Version-$Platform"
$packageDir = Join-Path $distRoot $packageName
$zipPath = Join-Path $distRoot "$packageName.zip"

$payload = @(
    "ARIBSplitter.ax",
    "LAVFilters.Dependencies.manifest",
    "avformat-lav-62.dll",
    "avcodec-lav-62.dll",
    "avutil-lav-60.dll",
    "swresample-lav-6.dll",
    "libbluray.dll",
    "libwinpthread-1.dll"
)

$rootFiles = @(
    "install_aribsplitter.bat",
    "uninstall_aribsplitter.bat",
    "README.md",
    "COPYING"
)

if (-not (Test-Path -LiteralPath $binDir)) {
    throw "Build output directory was not found: $binDir"
}

foreach ($file in $payload) {
    $path = Join-Path $binDir $file
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required runtime file was not found: $path"
    }
}

foreach ($file in $rootFiles) {
    $path = Join-Path $repoRoot $file
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required package file was not found: $path"
    }
}

New-Item -ItemType Directory -Force -Path $distRoot | Out-Null
if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $packageDir | Out-Null

foreach ($file in $payload) {
    Copy-Item -LiteralPath (Join-Path $binDir $file) -Destination (Join-Path $packageDir $file)
}

foreach ($file in $rootFiles) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $file) -Destination (Join-Path $packageDir $file)
}

$manifest = @(
    "ARIBSplitter release package",
    "Version: $Version",
    "Platform: $Platform",
    "",
    "Files:",
    ($payload + $rootFiles | Sort-Object | ForEach-Object { "  $_" })
)
$manifest | Set-Content -LiteralPath (Join-Path $packageDir "PACKAGE.txt") -Encoding ASCII

if (-not $NoZip) {
    if (Test-Path -LiteralPath $zipPath) {
        Remove-Item -LiteralPath $zipPath -Force
    }
    $items = Get-ChildItem -LiteralPath $packageDir
    Compress-Archive -LiteralPath $items.FullName -DestinationPath $zipPath -Force
}

Write-Host "Package directory: $packageDir"
if (-not $NoZip) {
    Write-Host "Package zip:       $zipPath"
}
