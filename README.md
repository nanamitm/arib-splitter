# ARIBSplitter

ARIBSplitter is a DirectShow source/splitter filter for MPEG-2 TS files with
ARIB captions.  It is based on LAV Filters and is intended for use as an
external filter in players such as MPC-BE.

The main goal of this fork is to make Japanese broadcast TS files playable
through DirectShow while exposing ARIB captions as subtitle samples.

## Current Scope

- MPEG-2 TS source filter registration for `.ts`, `.m2ts`, `.mts`, and `.m2t`
- ARIB caption decoding through libaribcaption (Profile A / Profile C)
- ASS subtitle output covering:
  - Horizontal captions positioned one ARIB cell at a time
  - Vertical writing (SWF mode 8/10) — characters positioned individually
  - Ruby (furigana) positioned one ARIB cell at a time
  - Superimpose streams handled separately from normal captions
  - Layer 0 drawing-command background rectangle + Layer 1 text, preventing
    background overlap when multiple caption rows are on screen simultaneously
  - DRCS glyphs rendered directly as ASS drawing commands when needed
- Caption timing: explicit `wait_duration` and indefinite-duration handling
- INI-based configuration for font, transparency, background, outline width,
  and timing offset (see [Configuration](#configuration))
- ARIB dual mono (bilingual) audio: main / sub selection in the bundled
  `ARIBAudio.ax` decoder (see [ARIB dual mono audio](#arib-dual-mono-audio))
- MPC-BE external filter usage

This repository is still close to the original LAV Filters tree.  Some demuxer
and shared utility files remain from upstream because ARIBSplitter builds on
that DirectShow splitter infrastructure.

The release targets of this fork are the x64 `ARIBSplitter.ax` and
`ARIBAudio.ax`, built from `ARIBSplitter.sln`.

## Repository Layout

```text
common/                 Shared DirectShow/base utility code from LAV Filters
demuxer/Demuxers/       Demuxing code and ARIB caption handling
demuxer/LAVSplitter/    DirectShow splitter/source filter implementation
ffmpeg/                 FFmpeg submodule
libaribcaption/         libaribcaption submodule
libbluray/              libbluray submodule
resources/              Filter resources
thirdparty/             Prebuilt x64 third-party headers/libraries for FFmpeg
```

Generated build outputs live under `bin_*` and are intentionally ignored.
Local Visual Studio state such as `.vs/` is also ignored.

## Submodules

After cloning, initialize submodules:

```bat
git submodule update --init --recursive
```

The project uses these submodules:

- `ffmpeg`
- `libaribcaption`
- `libbluray`

## Build

The current development and release build targets Windows x64 only and has
been tested with Visual Studio/MSBuild. The filters require Windows 7 or later,
matching FFmpeg.

The FFmpeg DLLs are built with `build_ffmpeg.sh` from an MSYS2 MINGW64 shell:

```sh
./build_ffmpeg.sh x64
```

The script links zlib statically even when MSYS2 also provides `libz.dll.a`,
and fails if an FFmpeg DLL would import a MinGW runtime DLL that the release
package does not ship (only `libwinpthread-1.dll` is shipped). Copy
`/mingw64/bin/libwinpthread-1.dll` into `bin_x64\` before packaging.

Build libaribcaption first if needed:

```bat
build_libaribcaption.bat
```

Then build `ARIBSplitter.sln` for Release|x64. A release needs both
`ARIBSplitter.ax` and `ARIBAudio.ax`, so build the whole solution rather than
`demuxer\LAVSplitter\LAVSplitter.vcxproj` alone:

```bat
msbuild ARIBSplitter.sln /m /p:Configuration=Release /p:Platform=x64
```

`tests\run_tests.ps1` builds the solution the same way and then runs the tests.

The x64 Release output is written under:

```text
bin_x64\
```

## Register

> **Administrator privileges are required.**
> Right-click `Install_ARIBSplitter_64.cmd` and choose **Run as administrator**.

```cmd
Install_ARIBSplitter_64.cmd
```

The script registers both `ARIBSplitter.ax` and `ARIBAudio.ax` with
`regsvr32` silently and then shows a success/failure message. Do not delete
either `.ax`, the bundled runtime DLLs, or `ARIBSplitter.ini` after
installation. The installer does not copy files anywhere; the filters run from
the release-package folder.

To unregister:

```cmd
Uninstall_ARIBSplitter_64.cmd
```

## MPC-BE Setup

1. Open MPC-BE → **Options** → **External Filters**
2. Click **Add Filter…** and select **ARIB Splitter Source**
3. Set the merit to **Prefer**
4. Click OK and restart MPC-BE

> **ARIB Splitter Source** handles local TS files directly.
> **ARIB Splitter** (the pure splitter without source) is only needed for
> network streams where a separate source filter provides the data.

For dual mono audio, add **ARIB Audio Decoder** the same way and set it to
**Prefer**. It has its own CLSIDs, so it can be installed next to official LAV
Filters; whichever one is preferred in MPC-BE is the one that decodes.

## Release Package

Create a release zip from the x64 Release build:

```powershell
.\make_release_package.ps1 -Version 20260529
```

The package is written under `dist\` and includes `ARIBSplitter.ax`,
`ARIBAudio.ax`, required runtime DLLs, install/uninstall scripts, `README.md`,
`COPYING`, and a small `PACKAGE.txt` manifest.

Before writing the zip, the script loads each `.ax` from the package folder
with only that folder and System32 on the DLL search path, so a runtime DLL
missing from the package fails the build instead of the installation.

## ARIB dual mono audio

Japanese bilingual programs are transmitted as a single AAC stream carrying two
single channel elements: main audio on the left, sub audio on the right. A
program can switch between ordinary stereo and dual mono partway through.

`ARIBAudio.ax` selects which one to play. The chosen channel is copied to both
outputs, so the output stays stereo and nothing renegotiates at the switch.

| Mode | Meaning |
| --- | --- |
| 0 | Both — main left, sub right, as transmitted |
| 1 | Main audio only (default) |
| 2 | Sub audio only |

Three ways to change it:

- **During playback**: MPC-BE → **View** → **Filters** → **ARIB Audio Decoder**
  → 主音声 / 副音声 / 主音声 + 副音声
- **Property page**: the *ARIB Dual Mono* box on the decoder's settings page
- **Registry**: `HKCU\Software\ARIBSplitter\Audio\DualMonoMode`

The setting persists and applies from the next frame, so switching mid-program
is immediate.

## Configuration

Place `ARIBSplitter.ini` in the same folder as `ARIBSplitter.ax`.
A sample with all available keys is provided in `settings/ARIBSplitter.ini`.

### [ARIB] — caption settings

| Key | Default | Description |
|-----|---------|-------------|
| `FontName` | MS Gothic | Caption font |
| `CaptionTransparency` | 0 | Text transparency 0 (opaque) – 100 (invisible) |
| `BackgroundTransparency` | *(stream value)* | Background transparency 0–100; omit the key entirely to use the alpha value embedded in the broadcast stream |
| `ShowBackground` | 1 | `0` to hide the caption background |
| `ShowRubyBackground` | 1 | `0` to hide the background behind ruby (furigana) text |
| `BackgroundPadding` | 1 | `1` extends the background to the full ARIB cell height (equal top/bottom padding from row spacing); `0` covers the glyph only |
| `OutlineWidth` | 0 | Text outline thickness (ASS `\bord` value, 0 = none) |
| `DelayMs` | 0 | Caption timing offset in milliseconds; negative values advance display |
| `DebugLogPath` | *(empty)* | File path for ARIBSplitter debug logs. Empty disables file logging |
| `VerboseLog` | 0 | `1` enables `OutputDebugString` and verbose ARIBSplitter logs in Release builds |
| `StretchChars` | *(empty)* | Characters that receive extra horizontal ASS `\fscx` scaling, for example `♬♪♫` |
| `StretchScale` | 100 | Extra horizontal scale for `StretchChars`; `200` doubles the broadcast-specified width |

### [Superimpose] — superimpose-specific overrides

Same keys as `[ARIB]`.  Any key omitted here falls back to the `[ARIB]` value.
Useful for giving news-ticker superimpose a different transparency or font.

> **INI encoding:** To use characters outside Shift-JIS (e.g. rare kanji or
> symbols) save `ARIBSplitter.ini` as **UTF-16 LE** (called "Unicode" in Windows
> Notepad).  The Windows INI API reads UTF-16 LE files natively when a BOM is
> present.

> **Font recommendation:** MS Gothic (the default) covers standard ARIB caption
> characters.  For CJK Extension glyphs or rare kanji variants, consider
> setting `FontName=Noto Sans JP` (requires the font to be installed separately).

### Debug logging

Release builds are quiet by default. Set `DebugLogPath` to write ARIBSplitter
diagnostic logs to a file. Set `VerboseLog=1` to also emit those logs through
`OutputDebugString` for DebugView or an attached debugger. Debug builds always
emit `OutputDebugString` logs.

To check caption timing without a player, `tools\arib_dump.ps1` runs the
demuxer of the x64 Release build on a recording and prints every caption event
sent to the subtitle pin, with its start and stop times and how far A/V had
been read when it was sent:

```powershell
.\tools\arib_dump.ps1 -InputFile recording.ts -Seconds 135 -Output dump.txt -Ini ARIBSplitter.ini -Log arib.log
```

`-Ini` selects the caption settings (the repository default when omitted) and
`-Log` writes the debug log described above.

### Vertical text

Vertical writing mode (ARIB SWF modes 8 and 10) is detected automatically by
inspecting the direction in which characters advance within each caption region.
No INI setting is required.  Each character is positioned individually in the
ASS output so that vertical-layout captions appear at their correct coordinates.

## Notes

### Property page theme

Splitter and Audio Decoder property pages follow the Windows app light/dark
setting, including changes while a page is open. High contrast uses the system
colors. The host application controls the outer title bar, tab strip, dialog
buttons and Pin Info page.

Theme support uses the bundled darkmodelib (MPL-2.0/MIT); see
`thirdparty/darkmodelib/ARIBSplitter.md` and the license files in that directory.
Release packages include these notices under `licenses/darkmodelib`.

ARIBSplitter keeps LAV Filters' original license and upstream structure.  See
`COPYING` for license details.
