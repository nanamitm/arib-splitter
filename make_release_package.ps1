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
    "ARIBAudio.ax",
    "ARIBSplitter.Dependencies.manifest",
    "avformat-lav-63.dll",
    "avcodec-lav-63.dll",
    "avutil-lav-61.dll",
    "swresample-lav-7.dll",
    "libbluray.dll",
    "libwinpthread-1.dll"
)

$rootFiles = @(
    "Install_ARIBSplitter_64.cmd",
    "Uninstall_ARIBSplitter_64.cmd",
    "README.md",
    "COPYING"
)

function Get-RuntimeFileHint {
    param([string]$File)

    switch ($File) {
        "libwinpthread-1.dll" {
            return @(
                "libwinpthread-1.dll is copied from the MSYS2 UCRT64 runtime after building FFmpeg.",
                "Run build_ffmpeg.sh x64 in an MSYS2 UCRT64 shell, or copy /ucrt64/bin/libwinpthread-1.dll into bin_x64 before packaging."
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
                "Build ARIBSplitter.sln for Release|x64 before packaging."
            )
        }
        "ARIBAudio.ax" {
            return @(
                "ARIBAudio.ax is produced by the Release|x64 build of decoder\LAVAudio.",
                "Build ARIBSplitter.sln for Release|x64 before packaging."
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

# Load each filter with only the package directory and System32 on the DLL
# search path, so a runtime DLL that the build output depends on but the
# package does not ship (such as zlib1.dll) fails here instead of on install.
Add-Type -Namespace ARIBPackage -Name NativeMethods -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
[DllImport("kernel32.dll")]
public static extern bool FreeLibrary(IntPtr module);
'@
$LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR = 0x100
$LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x800
foreach ($file in $payload | Where-Object { $_ -like "*.ax" }) {
    $path = Join-Path $packageDir $file
    $module = [ARIBPackage.NativeMethods]::LoadLibraryExW($path, [IntPtr]::Zero,
        $LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR -bor $LOAD_LIBRARY_SEARCH_SYSTEM32)
    if ($module -eq [IntPtr]::Zero) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        throw "$file does not load from the package (Win32 error $err). A DLL it depends on is missing from the payload; check FFmpeg's imports with objdump -p."
    }
    [void][ARIBPackage.NativeMethods]::FreeLibrary($module)
}

$licenseDir = Join-Path $packageDir "licenses\darkmodelib"
New-Item -ItemType Directory -Force -Path $licenseDir | Out-Null
Copy-Item -Path (Join-Path $repoRoot "thirdparty\darkmodelib\LICENSE*.md") -Destination $licenseDir
Copy-Item -LiteralPath (Join-Path $repoRoot "thirdparty\darkmodelib\ARIBSplitter.md") -Destination $licenseDir
Copy-Item -Path (Join-Path $repoRoot "thirdparty\darkmodelib\docs\LICENSE*.md") -Destination $licenseDir

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
