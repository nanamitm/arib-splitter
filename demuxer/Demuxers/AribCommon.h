/*
 *      Copyright (C) 2024 ARIBSplitter
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#pragma once

#include <string>

// Helpers shared by the ARIB caption code in LAVFDemuxer, LAVFStreamInfo and
// BaseDemuxer. Defined in LAVFDemuxer.cpp.

// Path of this DLL with the extension replaced by ".ini".
void AribGetIniPath(WCHAR *iniPath, DWORD size);

// ASS script header (Script Info / V4+ Styles / Events) used as the subtitle
// media type extradata. The Default style font comes from [ARIB] FontName.
std::string AribBuildASSScriptHeader();
