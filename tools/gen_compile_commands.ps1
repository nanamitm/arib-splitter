# Write compile_commands.json at the repository root for clangd and other
# tooling. The compile commands come from the CL.command.1.tlog files MSBuild
# leaves under bin_x64, so build ARIBSplitter.sln for Release|x64 (or run
# tests\run_tests.ps1) first, and run this again after project settings change.
#
#   .\tools\gen_compile_commands.ps1
#
# The tests and tools are compiled by scripts rather than MSBuild; they get the
# Demuxers flags with the include paths their scripts add.
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# The tlog spells every path in upper case. Map them back to the checkout's
# spelling so the file names match what editors open.
$sources = @{}
foreach ($f in (git -C $repoRoot ls-files -- '*.c' '*.cpp' '*.cc')) {
    $full = [IO.Path]::GetFullPath((Join-Path $repoRoot $f))
    $sources[$full.ToUpperInvariant()] = $full
}

# Splits a cl.exe command line. Quotes may sit inside an argument (/I"dir").
function Split-CommandLine([string]$line) {
    [regex]::Matches($line, '(?:[^\s"]+|"[^"]*")+') | ForEach-Object { $_.Value.Replace('"', '') }
}

# Flags that name build outputs or precompiled headers mean nothing to clangd.
$dropped = '^/(Fo|Fd|Fp|Fa|FR|Fr|Yu|Yc|Zi|ZI|GL|Gm-?|MP|diagnostics:)'

$entries = [Collections.Generic.List[object]]::new()
$demuxerFlags = $null
$tlogs = Get-ChildItem -Path (Join-Path $repoRoot 'bin_x64') -Recurse -Filter 'CL.command.1.tlog'
if (-not $tlogs) { throw 'No CL.command.1.tlog under bin_x64. Build ARIBSplitter.sln for Release|x64 first.' }
foreach ($tlog in $tlogs) {
    $lines = Get-Content -LiteralPath $tlog.FullName -Encoding Unicode
    for ($i = 0; $i + 1 -lt $lines.Count; $i++) {
        if (-not $lines[$i].StartsWith('^')) { continue }
        # The command line ends with the source files it compiled, which each
        # entry adds back on its own.
        $compiled = $lines[$i].Substring(1).Split('|')
        $flags = @(Split-CommandLine $lines[$i + 1] |
                   Where-Object { $_ -notmatch $dropped -and $compiled -notcontains $_ })
        foreach ($src in $compiled) {
            $file = $sources[$src.ToUpperInvariant()]
            if (-not $file) { continue }
            if ($tlog.FullName -match '\\Demuxers\\' -and -not $demuxerFlags) { $demuxerFlags = $flags }
            $entries.Add([ordered]@{
                directory = Split-Path -Parent $file
                file      = $file
                arguments = @('cl.exe') + $flags + @($file)
            })
        }
    }
}

if ($demuxerFlags) {
    $extraIncludes = @('demuxer\Demuxers', 'demuxer\LAVSplitter', 'decoder\LAVAudio') |
        ForEach-Object { '/I' + (Join-Path $repoRoot $_) }
    foreach ($f in (git -C $repoRoot ls-files -- 'tests/*.cpp' 'tools/*.cpp')) {
        $file = [IO.Path]::GetFullPath((Join-Path $repoRoot $f))
        $entries.Add([ordered]@{
            directory = $repoRoot
            file      = $file
            arguments = @('cl.exe') + $demuxerFlags + $extraIncludes + @('/utf-8', $file)
        })
    }
}

$out = Join-Path $repoRoot 'compile_commands.json'
[IO.File]::WriteAllText($out, (ConvertTo-Json -InputObject $entries.ToArray() -Depth 4),
                        [Text.UTF8Encoding]::new($false))
Write-Host "Wrote $($entries.Count) entries to $out"
