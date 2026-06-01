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
- MPC-BE external filter usage

This repository is still close to the original LAV Filters tree.  Many decoder
and demuxer files remain from upstream even when ARIBSplitter currently focuses
on TS splitting and caption delivery.

## Repository Layout

```text
common/                 Shared DirectShow/base utility code from LAV Filters
decoder/                Decoder projects inherited from LAV Filters
demuxer/Demuxers/       Demuxing code and ARIB caption handling
demuxer/LAVSplitter/    DirectShow splitter/source filter implementation
ffmpeg/                 FFmpeg submodule
libaribcaption/         libaribcaption submodule
libbluray/              libbluray submodule
qsdecoder/              Intel Quick Sync decoder submodule
resources/              Filter resources
thirdparty/             Prebuilt third-party headers/libraries from upstream
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
- `qsdecoder`

## Build

The current development build has been tested with Visual Studio/MSBuild on
Windows x64.

Build libaribcaption first if needed:

```bat
build_libaribcaption.bat
```

Then build the splitter project from `LAVFilters.sln`, or build
`demuxer\LAVSplitter\LAVSplitter.vcxproj` directly with MSBuild.

Example:

```bat
msbuild demuxer\LAVSplitter\LAVSplitter.vcxproj /m /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=%CD%\
```

The x64 Release output is written under:

```text
bin_x64\
```

## Register

> **Administrator privileges are required.**
> Right-click `install_aribsplitter.bat` and choose **Run as administrator**.

```bat
install_aribsplitter.bat
```

`regsvr32` will show a dialog confirming success or failure.

To unregister:

```bat
uninstall_aribsplitter.bat
```

## MPC-BE Setup

1. Open MPC-BE → **Options** → **External Filters**
2. Click **Add Filter…** and select **ARIB Splitter Source**
3. Set the merit to **Prefer**
4. Click OK and restart MPC-BE

> **ARIB Splitter Source** handles local TS files directly.
> **ARIB Splitter** (the pure splitter without source) is only needed for
> network streams where a separate source filter provides the data.

## Release Package

Create a release zip from the x64 Release build:

```powershell
.\make_release_package.ps1 -Version 20260529
```

The package is written under `dist\` and includes `ARIBSplitter.ax`, required
runtime DLLs, install/uninstall scripts, `README.md`, `COPYING`, and a small
`PACKAGE.txt` manifest.

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
| `OutlineWidth` | 0 | Text outline thickness (ASS `\bord` value, 0 = none) |
| `DelayMs` | 0 | Caption timing offset in milliseconds; negative values advance display |

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

### Vertical text

Vertical writing mode (ARIB SWF modes 8 and 10) is detected automatically by
inspecting the direction in which characters advance within each caption region.
No INI setting is required.  Each character is positioned individually in the
ASS output so that vertical-layout captions appear at their correct coordinates.

## Notes

ARIBSplitter keeps LAV Filters' original license and upstream structure.  See
`COPYING` for license details.
