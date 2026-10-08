# Build tools\arib_dump.cpp against the Release|x64 build output and dump the
# ARIB caption events of a recording. Build ARIBSplitter.sln (or run
# tests\run_tests.ps1) first; the dump links the same demuxers.lib.
#
#   .\tools\arib_dump.ps1 -InputFile recording.ts -Seconds 135 -Output dump.txt
#   .\tools\arib_dump.ps1 -InputFile recording.ts -Log arib.log
#
# Caption settings come from -Ini (the repository default unless given), so a
# dump can match an installed ARIBSplitter.ini. -Log adds DebugLogPath, which
# records every caption PES and how it was decoded.
param(
    [Parameter(Mandatory = $true)][string]$InputFile,
    [double]$Seconds = 120,
    [string]$Output,
    [string]$Ini,
    [string]$Log,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$InputFile = (Resolve-Path -LiteralPath $InputFile).Path
if (-not $Ini) { $Ini = Join-Path $repoRoot 'settings\ARIBSplitter.ini' }
$Ini = (Resolve-Path -LiteralPath $Ini).Path
if ($Output) { $Output = [IO.Path]::GetFullPath($Output) }
if ($Log) { $Log = [IO.Path]::GetFullPath($Log) }

Push-Location $repoRoot
try {
    $exe = Join-Path $repoRoot 'bin_x64\arib_dump.exe'
    if (-not $SkipBuild -or -not (Test-Path -LiteralPath $exe)) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $vsRoot) { throw 'Visual Studio C++ tools were not found.' }
        # VsDevCmd.bat runs vswhere.exe by name from its directory, which fails when
        # NoDefaultCurrentDirectoryInExePath is set, so put that directory on PATH.
        $env:PATH = (Split-Path -Parent $vswhere) + ';' + $env:PATH
        Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
        Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
        if (-not (Test-Path -LiteralPath 'bin_x64\lib\demuxers.lib')) {
            throw 'bin_x64\lib\demuxers.lib was not found. Build ARIBSplitter.sln for Release|x64 first.'
        }
        New-Item -ItemType Directory -Force work | Out-Null
        $common = @('/nologo', '/std:c++17', '/EHsc', '/MT', '/O2', '/Gy', '/utf-8', '/DUNICODE', '/D_UNICODE', '/DNDEBUG')
        $includes = @('include', 'common\includes', 'common\baseclasses', 'common\DSUtilLite', 'ffmpeg', 'libbluray\src', 'libaribcaption\include', 'libaribcaption\build\x64\Release\include') | ForEach-Object { "/I$_" }
        $link = @('/link', '/LTCG', '/OPT:REF', '/LIBPATH:bin_x64\lib', '/LIBPATH:libaribcaption\build\x64\Release\Release', 'dsutil.lib', 'strmbase.lib', 'libbluray.lib', 'avformat-lav.lib', 'avcodec-lav.lib', 'avutil-lav.lib', 'aribcaption.lib', 'strmiids.lib', 'advapi32.lib', 'ole32.lib', 'oleaut32.lib', 'user32.lib', 'gdi32.lib', 'winmm.lib', 'shlwapi.lib', 'shell32.lib', 'version.lib', 'uuid.lib', 'dwrite.lib', 'comctl32.lib', 'demuxers.lib')
        & cl.exe @($common + @('/Idemuxer\Demuxers', '/Idemuxer\LAVSplitter') + $includes + @('tools\arib_dump.cpp', '/Fo:work\arib_dump.obj', "/Fe:$exe") + $link)
        if ($LASTEXITCODE -ne 0) { throw "cl.exe failed with exit code $LASTEXITCODE" }
    }

    # The demuxer reads its settings from the ini named after the executable.
    $settings = Get-Content -LiteralPath $Ini | Where-Object { $_ -notmatch '^\s*(DebugLogPath|VerboseLog)\s*=' }
    $settings = foreach ($line in $settings) {
        $line
        if ($Log -and $line -match '^\s*\[ARIB\]\s*$') { "DebugLogPath=$Log" }
    }
    Set-Content -LiteralPath (Join-Path $repoRoot 'bin_x64\arib_dump.ini') -Value $settings -Encoding Unicode

    $splitter = Join-Path $repoRoot 'bin_x64\ARIBSplitter.ax'
    $arguments = @("`"$splitter`"", "`"$InputFile`"", "$Seconds")
    if ($Output) {
        # Redirect at the process level so the UTF-8 event text is written as is,
        # not re-encoded through the console code page.
        $process = Start-Process -FilePath $exe -ArgumentList $arguments -NoNewWindow -Wait -PassThru -RedirectStandardOutput $Output
        $exitCode = $process.ExitCode
    } else {
        & $exe $splitter $InputFile $Seconds
        $exitCode = $LASTEXITCODE
    }
    if ($exitCode -ne 0) { throw "arib_dump.exe failed with exit code $exitCode" }
} finally {
    Pop-Location
}
