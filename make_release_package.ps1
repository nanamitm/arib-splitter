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
    "ARIBSplitter.Dependencies.manifest",
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

function Get-RuntimeFileHint {
    param([string]$File)

    switch ($File) {
        "libwinpthread-1.dll" {
            return @(
                "libwinpthread-1.dll is copied from the MSYS2 MinGW64 runtime after building FFmpeg.",
                "Run build_ffmpeg.sh x64 in an MSYS2 MINGW64 shell, or copy /mingw64/bin/libwinpthread-1.dll into bin_x64 before packaging."
            )
        }
        { $_ -like "av*-lav-*.dll" -or $_ -like "sw*-lav-*.dll" } {
            return @(
                "This FFmpeg runtime DLL is produced by build_ffmpeg.sh.",
                "Run build_ffmpeg.sh x64 before creating a release package."
            )
        }
        "libbluray.dll" {
            return @(
                "libbluray.dll is produced by the Release|x64 build.",
                "Build ARIBSplitter.sln or demuxer\LAVSplitter\LAVSplitter.vcxproj for Release|x64 before packaging."
            )
        }
        default {
            return @(
                "Build ARIBSplitter for Release|x64 before creating a release package."
            )
        }
    }
}

# settings/ARIBSplitter.ini → ARIBSplitter.ini in the package root
$iniSrc = Join-Path $repoRoot "settings\ARIBSplitter.ini"

if (-not (Test-Path -LiteralPath $binDir)) {
    throw "Build output directory was not found: $binDir"
}

foreach ($file in $payload) {
    $path = Join-Path $binDir $file
    if (-not (Test-Path -LiteralPath $path)) {
        $hint = Get-RuntimeFileHint $file
        $message = @(
            "Required runtime file was not found: $path",
            "",
            "Hint:"
        )
        $message += $hint | ForEach-Object { "  $_" }
        throw ($message -join [Environment]::NewLine)
    }
}

foreach ($file in $rootFiles) {
    $path = Join-Path $repoRoot $file
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required package file was not found: $path"
    }
}

if (-not (Test-Path -LiteralPath $iniSrc)) {
    throw "Required package file was not found: $iniSrc"
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

Copy-Item -LiteralPath $iniSrc -Destination (Join-Path $packageDir "ARIBSplitter.ini")

$manifest = @(
    "ARIBSplitter release package",
    "Version: $Version",
    "Platform: $Platform",
    "",
    "Files:",
    ($payload + $rootFiles + @("ARIBSplitter.ini") | Sort-Object | ForEach-Object { "  $_" })
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
