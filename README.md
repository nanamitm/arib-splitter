# ARIBSplitter

ARIBSplitter is a DirectShow source/splitter filter for MPEG-2 TS files with
ARIB captions.  It is based on LAV Filters and is intended for use as an
external filter in players such as MPC-BE.

The main goal of this fork is to make Japanese broadcast TS files playable
through DirectShow while exposing ARIB captions as subtitle samples.

## Current Scope

- MPEG-2 TS source filter registration for `.ts`, `.m2ts`, `.mts`, and `.m2t`
- ARIB caption decoding through libaribcaption
- ASS subtitle output for normal captions, multi-region captions, and ruby
- Caption duration handling for explicit and indefinite captions
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

Register the built filter:

```bat
install_aribsplitter.bat
```

The script requests administrator privileges and registers
`ARIBSplitter.ax` in the same directory when used from a release package.
In a development checkout, it falls back to `bin_x64\ARIBSplitter.ax`.
To register the debug build from a development checkout instead:

```bat
install_aribsplitter.bat debug
```

To unregister:

```bat
uninstall_aribsplitter.bat
```

For MPC-BE, add ARIBSplitter as an external filter and prefer it for TS files.

## Release Package

Create a release zip from the x64 Release build:

```powershell
.\make_release_package.ps1 -Version 20260529
```

The package is written under `dist\` and includes `ARIBSplitter.ax`, required
runtime DLLs, install/uninstall scripts, `README.md`, `COPYING`, and a small
`PACKAGE.txt` manifest.

## Notes

ARIBSplitter keeps LAV Filters' original license and upstream structure.  See
`COPYING` for license details.
