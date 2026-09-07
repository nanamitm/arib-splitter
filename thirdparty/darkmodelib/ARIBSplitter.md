# darkmodelib integration

Vendored from MPCVideoDec-fork commit fa99647299c4edb3cf662bc14f19b5451090723e,
src/ExtLib/darkmodelib (upstream: https://github.com/ozone10/darkmodelib).
The original source headers, MPL-2.0 and MIT license texts are retained.

Built into DSUtilLite with C++20 and _DARKMODELIB_NO_INI_CONFIG. No additional
runtime DLL or INI file is required. Property pages follow the Windows app theme.
Only filter-owned child pages and controls are themed; the host owns its frame,
tab strip, buttons and Pin Info page.

Local change: DmlibWinApi.cpp does not call AllowDarkModeForApp or FlushMenuThemes
from SetDarkMode, to avoid changing the host application's process-wide mode.
Scrollbar hooking is not enabled.
