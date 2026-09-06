# ARIB regression tests

Run from PowerShell after building the FFmpeg and libaribcaption dependencies:

```powershell
.\tests\run_tests.ps1
```

The script finds Visual Studio C++ tools, builds Release x64, compiles the tests
against the production objects, and fails on the first failing test. Use
`-SkipBuild` only when the Release objects already match the checkout.

- `arib_demuxer_tests.cpp` sends synthetic caption PES and A/V packets through
  the real `GetNextPacket()` using a deterministic FFmpeg input format. It checks
  bounded caption delivery, exact clear timing, EOF, explicit waits, flushing,
  Profile A/C, caption/superimpose selection, and buffer reference release.
- `registry_tests.cpp` runs the production registration helpers with HKCR and
  HKLM redirected to temporary HKCU keys. It checks restoration, repeat install,
  absent mappings, and another filter taking ownership. System filter mappings
  are not modified; administrator rights are not required.
- `audio_tests.cpp` verifies that a mode change blocks while the receive lock is
  held, then races 5,000 mode changes against 1,000 AAC/PCM reinitializations.
  Runtime configuration prevents changes to the user's saved audio settings.

These are native code and packet-level tests. They do not test MPC-BE's rendered
subtitle appearance or audible output with recorded broadcast material.
