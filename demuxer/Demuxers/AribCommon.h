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

#include <cstdint>
#include <string>
#include <vector>

// Helpers shared by the ARIB caption code in LAVFDemuxer, LAVFStreamInfo and
// BaseDemuxer. Defined in LAVFDemuxer.cpp, except for the EIT ones below.

// Path of this DLL with the extension replaced by ".ini".
void AribGetIniPath(WCHAR *iniPath, DWORD size);

// ASS script header (Script Info / V4+ Styles / Events) used as the subtitle
// media type extradata. The Default style font comes from [ARIB] FontName.
std::string AribBuildASSScriptHeader();

// Whether |u8str|, the text of one caption cell, is a single glyph that
// advances exactly one em in |fontName| (UTF-8; empty means the default
// MS Gothic), so a run of such cells can be laid out with \fsp alone.
bool AribGlyphAdvancesOneEm(const std::string &fontName, const char *u8str);

// EIT helpers, defined in AribEIT.cpp.

// One ARIB audio component descriptor (0xC4) of an EIT event. componentTag
// matches the PMT stream identifier descriptor (0x52) of the audio stream.
struct AribAudioComponent
{
    int componentType = 0;
    int componentTag = -1;
    bool mainComponent = false;
    std::string language;  // ISO 639-2 code, e.g. "jpn"
    std::string language2; // second language of a dual mono stream, or empty
    std::string text;      // UTF-8 component text, used as the track name
};

// Times below are JST seconds counted from MJD 0, or -1 when undefined.
struct AribEITEvent
{
    bool present = false; // section seen
    int eventId = -1;     // -1 for a section without an event
    int64_t startTime = -1;
    int64_t duration = -1;
    std::vector<AribAudioComponent> audio;
};

// Audio components of the present (0) and following (1) events of a service,
// and the time of the first TOT/TDT, which dates the start of a recording.
struct AribEITAudio
{
    AribEITEvent events[2];
    int64_t firstTime = -1;
    const AribAudioComponent *Find(int sectionNumber, int componentTag) const;
};

// The event (0 present, 1 following) a recording of |duration| seconds shows
// the most of, from its first TOT and the event times. A recording usually
// starts shortly before its event, while the present one is still the previous
// programme. Without times, the event that describes every one of
// |componentTags| (the present one first); -1 if neither does.
int AribRecordedEvent(const AribEITAudio &eit, int64_t duration, const std::vector<int> &componentTags);

// Store the time of a TOT (0x73) or TDT (0x70) section in |eit| if it has none.
bool AribParseTOT(const uint8_t *section, size_t size, AribEITAudio &eit);

// Track name of an audio component: its text, or else one built from the
// audio mode, the two languages of a dual mono stream and whether it is the
// main audio. Empty when none of these say anything.
std::string AribAudioComponentName(const AribAudioComponent &c);

// Decode an ARIB STD-B24 8-bit coded SI text into UTF-8.
std::string AribDecodeString(const uint8_t *data, size_t size);

// Store the audio components of one EIT present/following actual section
// (table 0x4E, section 0 or 1) of |serviceId| in |eit|. Returns false for any
// other section, or one that fails its CRC.
bool AribParseEITAudioComponents(const uint8_t *section, size_t size, int serviceId, AribEITAudio &eit);

struct AVIOContext;
struct AVStream;

// Component tag of the PMT stream identifier descriptor (0x52) of an mpegts
// stream, or -1 without one.
int AribStreamComponentTag(AVStream *st);

// Read the EIT present/following of |serviceId| and the first TOT from the
// start of a seekable TS until all are found or 16 MiB in, then seek |pb| back
// to where it was. Returns whether either event was found.
bool AribScanEIT(AVIOContext *pb, int serviceId, AribEITAudio &eit);
