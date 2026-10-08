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
  caption hand-over on replacement, hold-interval re-sends, clear timing, EOF,
  explicit waits, flushing, Profile A/C, caption/superimpose selection, and
  buffer reference release. It also checks which caption cells are merged into
  one ASS event: only glyphs one em wide in the caption font. These checks
  measure MS Gothic and MS PGothic, so they need the Japanese fonts installed.
- `registry_tests.cpp` runs the production registration helpers with HKCR and
  HKLM redirected to temporary HKCU keys. It checks restoration, repeat install,
  absent mappings, and another filter taking ownership. System filter mappings
  are not modified; administrator rights are not required.
- `audio_tests.cpp` verifies that a mode change blocks while the receive lock is
  held, then races 5,000 mode changes against 1,000 AAC/PCM reinitializations.
  Runtime configuration prevents changes to the user's saved audio settings.

- `splitter_tests.cpp` exercises production input callbacks with a deterministic
  `IAsyncReader`: complete/partial reads, unavailable length, EOF, and signed
  end-relative seeks. It runs the demux worker to verify seek failure propagation,
  position rollback, blocked packet delivery, retry, and recovery, and that an
  unseekable source reports the failure but keeps playing its previous segment. PCM tests check
  mono passthrough and multi-channel interleaving while counting input-buffer
  releases over 3,000 packets.

- `property_page_tests.cpp` loads both filter DLLs without registration and tests
  all six filter-owned pages, including Audio Status. It switches dark/light/dark,
  reactivates the same COM page, checks background colors and unchanged dirty state,
  then verifies that the DLL can unload. HKCU is redirected inside the test process
  to a temporary key; the user's Windows theme and filter settings are untouched.
  Run `bin_x64\property_page_tests.exe <absolute-bin_x64-path> splitter 0 dark`
  for a visual preview (Escape closes it). Use `audio`, page indices 0-3, and
  `light` to inspect the other pages and theme.

These are native code, property-page and packet-level tests. They do not test MPC-BE's rendered
subtitle appearance or audible output with recorded broadcast material.
