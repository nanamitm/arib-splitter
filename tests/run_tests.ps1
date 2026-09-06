param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Push-Location $repoRoot
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'Visual Studio C++ tools were not found.' }
    Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    New-Item -ItemType Directory -Force work | Out-Null
    function Invoke-Checked {
        param([string]$Program, [string[]]$Arguments)
        & $Program @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
    }
    if (-not $SkipBuild) {
        Invoke-Checked 'MSBuild.exe' @('ARIBSplitter.sln', '/nologo', '/m', '/p:Configuration=Release', '/p:Platform=x64', '/nodeReuse:false', '/v:minimal')
    }
    $common = @('/nologo', '/std:c++17', '/EHsc', '/MT', '/O2', '/Gy', '/utf-8', '/DUNICODE', '/D_UNICODE', '/DNDEBUG')
    $includes = @('include', 'common\includes', 'common\baseclasses', 'common\DSUtilLite', 'ffmpeg', 'libbluray\src', 'libaribcaption\include', 'libaribcaption\build\x64\Release\include') | ForEach-Object { "/I$_" }
    $link = @('/link', '/LTCG', '/OPT:REF', '/LIBPATH:bin_x64\lib', '/LIBPATH:libaribcaption\build\x64\Release\Release', 'dsutil.lib', 'strmbase.lib', 'libbluray.lib', 'avformat-lav.lib', 'avcodec-lav.lib', 'avutil-lav.lib', 'aribcaption.lib', 'strmiids.lib', 'advapi32.lib', 'ole32.lib', 'oleaut32.lib', 'user32.lib', 'gdi32.lib', 'winmm.lib', 'shlwapi.lib', 'shell32.lib', 'version.lib', 'uuid.lib', 'dwrite.lib', 'comctl32.lib')
    Invoke-Checked 'cl.exe' ($common + @('/Idemuxer\Demuxers') + $includes + @('tests\arib_demuxer_tests.cpp', '/Fo:work\arib_demuxer_tests.obj', '/Fe:bin_x64\arib_demuxer_tests.exe') + $link + @('demuxers.lib'))
    Invoke-Checked '.\bin_x64\arib_demuxer_tests.exe' @((Join-Path $repoRoot 'bin_x64\ARIBSplitter.ax'))
    $splitterObjects = Get-ChildItem bin_x64\LAVSplitter\*.obj | Where-Object Name -ne 'dllmain.obj' | ForEach-Object FullName
    Invoke-Checked 'cl.exe' ($common + @('/Idemuxer\LAVSplitter', '/Idemuxer\Demuxers') + $includes + @('tests\registry_tests.cpp', '/Fo:work\registry_tests.obj', '/Fe:bin_x64\registry_tests.exe') + $link + @('demuxers.lib') + $splitterObjects)
    Invoke-Checked '.\bin_x64\registry_tests.exe' @()
    $audioObjects = Get-ChildItem bin_x64\LAVAudio\*.obj | ForEach-Object FullName
    Invoke-Checked 'cl.exe' ($common + @('/Idecoder\LAVAudio') + $includes + @('tests\audio_tests.cpp', '/Fo:work\audio_tests.obj', '/Fe:bin_x64\audio_tests.exe') + $link + @('swresample-lav.lib') + $audioObjects)
    Invoke-Checked '.\bin_x64\audio_tests.exe' @((Join-Path $repoRoot 'bin_x64\ARIBAudio.ax'))
} finally {
    Pop-Location
}
