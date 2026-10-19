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

// ARIB EIT parsing for audio track names. FFmpeg's mpegts demuxer does not
// look at the ARIB descriptors of the EIT. It hands its sections through as
// packets of an AV_CODEC_ID_EPG data stream, but only once reading starts, and
// avformat_find_stream_info can stop before the first one, so the start of the
// file is read here instead.

// Built without stdafx.h: FFmpeg's internal.h, which holds the component tag,
// clashes with the AVCodecTag stdafx.h declares.
#include <windows.h>
#include <algorithm>
#include "AribCommon.h"

extern "C"
{
#include "libavformat/internal.h"
#include "libavutil/crc.h"
}

// Code tables only; the libaribcaption decoder itself works on caption PES.
#include "../../libaribcaption/src/decoder/b24_conv_tables.hpp"
#include "../../libaribcaption/src/decoder/b24_gaiji_table.hpp"

namespace
{

enum class AribCharSet
{
    Kanji,
    Alphanumeric,
    Hiragana,
    Katakana,
    JISX0201Katakana,
    // Mosaic, DRCS and macro sets have no text form.
    Other,
};

struct AribCodeSet
{
    AribCharSet set;
    int bytes;
};

const AribCodeSet kKanjiSet = {AribCharSet::Kanji, 2};
const AribCodeSet kAlphanumericSet = {AribCharSet::Alphanumeric, 1};
const AribCodeSet kHiraganaSet = {AribCharSet::Hiragana, 1};
const AribCodeSet kKatakanaSet = {AribCharSet::Katakana, 1};

// Final byte of a graphic set designation (ARIB STD-B24 Part 2 Table 7-3).
AribCodeSet AribCodeSetByF(uint8_t f, bool twoByte)
{
    if (twoByte)
    {
        switch (f)
        {
        case 0x42: // Kanji
        case 0x39: // JIS compatible Kanji plane 1
        case 0x3A: // JIS compatible Kanji plane 2
        case 0x3B: // Additional symbols, rows 85 and up of the same table
            return kKanjiSet;
        default:
            return {AribCharSet::Other, 2};
        }
    }
    switch (f)
    {
    case 0x4A: // Alphanumeric
    case 0x36: // Proportional alphanumeric
        return kAlphanumericSet;
    case 0x30: // Hiragana
    case 0x37: // Proportional hiragana
        return kHiraganaSet;
    case 0x31: // Katakana
    case 0x38: // Proportional katakana
        return kKatakanaSet;
    case 0x49:
        return {AribCharSet::JISX0201Katakana, 1};
    default:
        return {AribCharSet::Other, 1};
    }
}

void AppendUTF8(std::string &out, uint32_t cp)
{
    if (cp < 0x80)
        out += (char)cp;
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

template <size_t N> uint32_t TableAt(const uint32_t (&table)[N], size_t index)
{
    return index < N ? table[index] : 0;
}

// One graphic character; c1 and c2 are already reduced to 0x21-0x7E.
uint32_t AribCharToUnicode(const AribCodeSet &cs, uint8_t c1, uint8_t c2, bool middleSize)
{
    using namespace aribcaption;
    const size_t index = c1 - 0x21;
    switch (cs.set)
    {
    case AribCharSet::Kanji: {
        // Rows 85 and up hold the ARIB additional kanji and symbols.
        const size_t ku = c1 - 0x21, ten = c2 - 0x21;
        if (ku < 84)
            return TableAt(kKanjiTable, ku * 94 + ten);
        return TableAt(kAdditionalSymbolsTable_Unicode, (ku - 84) * 94 + ten);
    }
    case AribCharSet::Alphanumeric:
        return middleSize ? TableAt(kAlphanumericTable_Halfwidth, index)
                          : TableAt(kAlphanumericTable_Fullwidth, index);
    case AribCharSet::Hiragana:
        return TableAt(kHiraganaTable, index);
    case AribCharSet::Katakana:
        return TableAt(kKatakanaTable, index);
    case AribCharSet::JISX0201Katakana:
        return TableAt(kJISX0201KatakanaTable, index);
    default:
        return 0;
    }
}

int ReadBE16(const uint8_t *p)
{
    return (p[0] << 8) | p[1];
}

// hh mm ss in BCD, or -1 if any digit is not one (all ones means undefined).
int64_t ReadBCDTime(const uint8_t *p)
{
    int64_t seconds = 0;
    const int scale[3] = {3600, 60, 1};
    for (int i = 0; i < 3; ++i)
    {
        const int hi = p[i] >> 4, lo = p[i] & 0x0F;
        if (hi > 9 || lo > 9)
            return -1;
        seconds += (hi * 10 + lo) * scale[i];
    }
    return seconds;
}

// MJD date and BCD time of day, as used by the EIT and the TOT.
int64_t ReadMJDTime(const uint8_t *p)
{
    const int64_t time = ReadBCDTime(p + 2);
    if (time < 0)
        return -1;
    return ReadBE16(p) * 86400LL + time;
}

void ParseAudioComponent(const uint8_t *d, size_t len, std::vector<AribAudioComponent> &out)
{
    // stream_content, component_type, component_tag, stream_type,
    // simulcast_group_tag, flags, ISO_639_language_code
    if (len < 9)
        return;
    AribAudioComponent c;
    c.componentType = d[1];
    c.componentTag = d[2];
    const bool multiLingual = (d[5] & 0x80) != 0;
    c.mainComponent = (d[5] & 0x40) != 0;
    c.language.assign(reinterpret_cast<const char *>(d + 6), 3);
    size_t textPos = 9;
    if (multiLingual)
    {
        if (len < 12)
            return;
        c.language2.assign(reinterpret_cast<const char *>(d + 9), 3);
        textPos = 12;
    }
    c.text = AribDecodeString(d + textPos, len - textPos);
    out.push_back(c);
}

} // namespace

std::string AribDecodeString(const uint8_t *data, size_t size)
{
    // Initial state of an SI text string (ARIB STD-B24 Part 2 7.1.3).
    AribCodeSet g[4] = {kKanjiSet, kAlphanumericSet, kHiraganaSet, kKatakanaSet};
    int gl = 0, gr = 2;
    bool middleSize = false;
    std::string out;

    size_t i = 0;
    auto putChar = [&](const AribCodeSet &cs, uint8_t c1) {
        uint8_t c2 = 0;
        if (cs.bytes == 2)
        {
            if (i >= size)
                return;
            c2 = data[i++] & 0x7F;
            if (c2 < 0x21 || c2 > 0x7E)
                return;
        }
        if (uint32_t cp = AribCharToUnicode(cs, c1, c2, middleSize))
            AppendUTF8(out, cp);
    };

    while (i < size)
    {
        const uint8_t c = data[i++];
        if ((c >= 0x21 && c <= 0x7E) || (c >= 0xA1 && c <= 0xFE))
        {
            putChar(g[c < 0x80 ? gl : gr], c & 0x7F);
            continue;
        }
        switch (c)
        {
        case 0x20: // SP
        case 0xA0:
            AppendUTF8(out, middleSize ? 0x20 : 0x3000);
            break;
        case 0x0D: // APR, a line break in an SI text
            out += ' ';
            break;
        case 0x0E: // LS1
            gl = 1;
            break;
        case 0x0F: // LS0
            gl = 0;
            break;
        case 0x19: // SS2
        case 0x1D: // SS3
            if (i < size)
            {
                const uint8_t c1 = data[i++] & 0x7F;
                if (c1 >= 0x21 && c1 <= 0x7E)
                    putChar(g[c == 0x19 ? 2 : 3], c1);
            }
            break;
        case 0x1B: { // ESC
            if (i >= size)
                break;
            const uint8_t e = data[i++];
            switch (e)
            {
            case 0x6E: gl = 2; break; // LS2
            case 0x6F: gl = 3; break; // LS3
            case 0x7E: gr = 1; break; // LS1R
            case 0x7D: gr = 2; break; // LS2R
            case 0x7C: gr = 3; break; // LS3R
            case 0x24: // 2-byte G set, into G1-G3 with an intermediate byte
                if (i < size && data[i] >= 0x28 && data[i] <= 0x2B)
                {
                    const int index = data[i++] - 0x28;
                    if (i < size && data[i] == 0x20) // DRCS
                    {
                        g[index] = {AribCharSet::Other, 2};
                        i += 2;
                    }
                    else if (i < size)
                        g[index] = AribCodeSetByF(data[i++], true);
                }
                else if (i < size)
                    g[0] = AribCodeSetByF(data[i++], true);
                break;
            case 0x28:
            case 0x29:
            case 0x2A:
            case 0x2B:
                if (i < size && data[i] == 0x20) // 1-byte DRCS
                {
                    g[e - 0x28] = {AribCharSet::Other, 1};
                    i += 2;
                }
                else if (i < size)
                    g[e - 0x28] = AribCodeSetByF(data[i++], false);
                break;
            }
            break;
        }
        case 0x89: // MSZ
            middleSize = true;
            break;
        case 0x8A: // NSZ
        case 0x88: // SSZ
            middleSize = false;
            break;
        case 0x8B: // SZX
        case 0x91: // FLC
        case 0x93: // POL
        case 0x94: // WMM
        case 0x97: // HLC
        case 0x98: // RPC
            ++i;
            break;
        case 0x90: // COL, with an extra byte after the 0x20 intermediate
            if (i < size && data[i] == 0x20)
                ++i;
            ++i;
            break;
        default:
            // The other C0/C1 controls carry no text.
            break;
        }
    }
    return out;
}

bool AribParseEITAudioComponents(const uint8_t *section, size_t size, int serviceId, AribEITAudio &eit)
{
    // table_id 0x4E is EIT present/following of the actual TS.
    if (size < 18 || section[0] != 0x4E || !(section[1] & 0x80))
        return false;
    const size_t sectionLength = ((section[1] & 0x0F) << 8) | section[2];
    if (sectionLength < 15 || sectionLength + 3 > size)
        return false;
    size = sectionLength + 3;
    // FFmpeg opens the EIT filter without a CRC check.
    if (av_crc(av_crc_get_table(AV_CRC_32_IEEE), UINT32_MAX, section, size) != 0)
        return false;
    if (ReadBE16(section + 3) != serviceId || !(section[5] & 0x01))
        return false;
    const int sectionNumber = section[6];
    if (sectionNumber > 1)
        return false;

    AribEITEvent event;
    event.present = true;
    const uint8_t *p = section + 14;
    const uint8_t *end = section + size - 4;
    // An empty section 1 means there is no following event.
    if (end - p >= 12)
    {
        event.eventId = ReadBE16(p);
        event.startTime = ReadMJDTime(p + 2);
        event.duration = ReadBCDTime(p + 7);
        size_t loopLength = ReadBE16(p + 10) & 0x0FFF;
        p += 12;
        if (loopLength > (size_t)(end - p))
            loopLength = end - p;
        const uint8_t *loopEnd = p + loopLength;
        while (loopEnd - p >= 2)
        {
            const uint8_t tag = p[0];
            const size_t len = p[1];
            p += 2;
            if (len > (size_t)(loopEnd - p))
                break;
            if (tag == 0xC4)
                ParseAudioComponent(p, len, event.audio);
            p += len;
        }
    }
    eit.events[sectionNumber] = event;
    return true;
}

const AribAudioComponent *AribEITAudio::Find(int sectionNumber, int componentTag) const
{
    for (const auto &c : events[sectionNumber].audio)
        if (c.componentTag == componentTag)
            return &c;
    return nullptr;
}

bool AribParseTOT(const uint8_t *section, size_t size, AribEITAudio &eit)
{
    // table_id, section_length, JST_time; the TOT adds descriptors and a CRC.
    if (size < 8 || (section[0] != 0x73 && section[0] != 0x70))
        return false;
    const size_t sectionLength = ((section[1] & 0x0F) << 8) | section[2];
    if (sectionLength < 5 || sectionLength + 3 > size)
        return false;
    if (section[0] == 0x73 && av_crc(av_crc_get_table(AV_CRC_32_IEEE), UINT32_MAX, section, sectionLength + 3) != 0)
        return false;
    const int64_t time = ReadMJDTime(section + 3);
    if (time < 0)
        return false;
    if (eit.firstTime < 0)
        eit.firstTime = time;
    return true;
}

int AribRecordedEvent(const AribEITAudio &eit, int64_t duration, const std::vector<int> &componentTags)
{
    if (eit.firstTime >= 0 && duration >= 0)
    {
        const int64_t begin = eit.firstTime, end = begin + duration;
        int best = -1;
        int64_t bestOverlap = 0;
        for (int i = 0; i < 2; ++i)
        {
            const AribEITEvent &e = eit.events[i];
            if (e.startTime < 0 || e.duration < 0)
                continue;
            const int64_t overlap = (std::min)(end, e.startTime + e.duration) - (std::max)(begin, e.startTime);
            if (overlap > bestOverlap)
            {
                best = i;
                bestOverlap = overlap;
            }
        }
        if (best >= 0)
            return best;
    }

    for (int i = 0; i < 2; ++i)
    {
        if (eit.events[i].audio.empty())
            continue;
        bool all = true;
        for (int tag : componentTags)
            all = all && eit.Find(i, tag) != nullptr;
        if (all)
            return i;
    }
    return -1;
}

namespace
{

// Audio mode, b4-b0 of component_type (ARIB STD-B10 Table 6-43), or nullptr.
// The multi-channel modes are named by their channel count.
const char *AribAudioModeName(int mode)
{
    switch (mode)
    {
    case 0x01: return u8"\u30E2\u30CE\u30E9\u30EB"; // mono
    case 0x02: return u8"\u4E8C\u91CD\u97F3\u58F0"; // dual mono
    case 0x03: return u8"\u30B9\u30C6\u30EC\u30AA"; // stereo
    case 0x04: return "2/1";
    case 0x05: return "3/0";
    case 0x06: return "2/2";
    case 0x07: return "3/1";
    case 0x08: return "3/2";
    case 0x09: return "5.1ch";  // 3/2.1
    case 0x0A: return "6.1ch";  // 3/3.1
    case 0x0B: return "6.1ch";  // 2/0/0-2/0/2-0.1
    case 0x0C: return "7.1ch";  // 5/2.1
    case 0x0D: return "7.1ch";  // 3/2/2.1
    case 0x0E: return "7.1ch";  // 2/0/0-3/0/2-0.1
    case 0x0F: return "7.2ch";  // 0/2/0-3/0/2-0.2
    case 0x10: return "10.2ch"; // 2/0/0-3/2/3-0.2
    case 0x11: return "22.2ch"; // 3/3/3-5/2/3-3/0/0.2
    default: return nullptr;
    }
}

// Audio for a person with disabilities, b6-b5 of component_type, or nullptr.
const char *AribAccessibilityName(int value)
{
    switch (value)
    {
    case 0x01: return u8"\u89E3\u8AAC"; // commentary for the visually impaired
    case 0x02: return u8"\u8074\u899A\u969C\u5BB3\u8005\u7528"; // for the hearing impaired
    default: return nullptr;
    }
}

std::string AribLanguageName(const std::string &code)
{
    if (code == "jpn")
        return u8"\u65E5\u672C\u8A9E";
    if (code == "eng")
        return u8"\u82F1\u8A9E";
    return code;
}

void AppendWord(std::string &name, const std::string &word)
{
    if (!name.empty())
        name += ' ';
    name += word;
}

} // namespace

std::string AribAudioComponentName(const AribAudioComponent &c)
{
    if (!c.text.empty())
        return c.text;

    // e.g. "secondary audio, stereo, commentary" from 0x23 of a sub component.
    std::string name;
    if (!c.mainComponent)
        name = u8"\u526F\u97F3\u58F0"; // secondary audio
    std::string mode;
    if (const char *m = AribAudioModeName(c.componentType & 0x1F))
        mode = m;
    // Dual mono carries the language of each channel, e.g. (Japanese/English).
    if (!c.language2.empty() && c.language2 != c.language)
        mode += u8"\uFF08" + AribLanguageName(c.language) + "/" + AribLanguageName(c.language2) + u8"\uFF09";
    if (!mode.empty())
        AppendWord(name, mode);
    if (const char *a = AribAccessibilityName((c.componentType >> 5) & 0x03))
        AppendWord(name, a);
    return name;
}

int AribStreamComponentTag(AVStream *st)
{
    // mpegts stores the stream identifier descriptor plus one, 0 when absent.
    return ffstream(st)->stream_identifier - 1;
}

namespace
{

// Reassembles the PSI sections of one PID from its TS packets.
struct AribSectionReader
{
    std::vector<uint8_t> section;
    int lastCC = -1;

    template <typename F> void Packet(const uint8_t *p, F &&onSection)
    {
        const bool unitStart = (p[1] & 0x40) != 0;
        const int afc = (p[3] >> 4) & 0x03;
        const int cc = p[3] & 0x0F;
        if (!(afc & 0x01))
            return;
        if (lastCC >= 0 && cc == lastCC)
            return; // duplicate packet
        if (lastCC >= 0 && cc != ((lastCC + 1) & 0x0F))
            section.clear();
        lastCC = cc;

        const uint8_t *q = p + 4, *end = p + 188;
        if (afc & 0x02)
            q += 1 + q[0];
        if (q >= end)
            return;
        if (unitStart)
        {
            const uint8_t *next = q + 1 + q[0];
            if (next > end)
            {
                section.clear();
                return;
            }
            // The bytes before the pointer end the section already started.
            if (!section.empty())
            {
                section.insert(section.end(), q + 1, next);
                Flush(onSection);
            }
            section.assign(next, end);
        }
        else if (!section.empty())
            section.insert(section.end(), q, end);
        Flush(onSection);
    }

    template <typename F> void Flush(F &&onSection)
    {
        while (section.size() >= 3)
        {
            if (section[0] == 0xFF) // stuffing up to the end of the packet
            {
                section.clear();
                return;
            }
            const size_t length = 3 + (((section[1] & 0x0F) << 8) | section[2]);
            if (section.size() < length)
                return;
            onSection(section.data(), length);
            section.erase(section.begin(), section.begin() + length);
        }
    }
};

// Offset of the first of three TS sync bytes |size| apart, or -1.
ptrdiff_t FindTSSync(const std::vector<uint8_t> &data, size_t from, int size)
{
    for (size_t i = from; i + 2 * size < data.size(); ++i)
    {
        if (data[i] == 0x47 && data[i + size] == 0x47 && data[i + 2 * size] == 0x47)
            return (ptrdiff_t)i;
    }
    return -1;
}

} // namespace

bool AribScanEIT(AVIOContext *pb, int serviceId, AribEITAudio &eit)
{
    // EIT p/f actual repeats at least every 2 seconds and the TOT every 5;
    // 16 MiB is about 8 seconds of a 16 Mbps broadcast.
    const int64_t kScanBytes = 16 << 20;
    if (!pb || !(pb->seekable & AVIO_SEEKABLE_NORMAL))
        return false;
    const int64_t resume = avio_tell(pb);
    if (resume < 0 || avio_seek(pb, 0, SEEK_SET) < 0)
        return false;

    AribSectionReader eitReader, totReader;
    auto onEIT = [&](const uint8_t *s, size_t size) { AribParseEITAudioComponents(s, size, serviceId, eit); };
    auto onTOT = [&](const uint8_t *s, size_t size) { AribParseTOT(s, size, eit); };
    std::vector<uint8_t> data;
    std::vector<uint8_t> chunk(64 * 1024);
    size_t pos = 0;
    int packetSize = 0;
    for (int64_t total = 0;
         total < kScanBytes && !(eit.events[0].present && eit.events[1].present && eit.firstTime >= 0);)
    {
        const int n = avio_read(pb, chunk.data(), (int)chunk.size());
        if (n <= 0)
            break;
        total += n;
        data.erase(data.begin(), data.begin() + pos);
        pos = 0;
        data.insert(data.end(), chunk.begin(), chunk.begin() + n);

        if (!packetSize)
        {
            // 188-byte TS, 192-byte M2TS with its timestamp in front, 204 with RS parity.
            for (int size : {188, 192, 204})
            {
                const ptrdiff_t sync = FindTSSync(data, 0, size);
                if (sync >= 0)
                {
                    packetSize = size;
                    pos = (size_t)sync;
                    break;
                }
            }
            if (!packetSize)
                continue;
        }
        while (pos + packetSize <= data.size())
        {
            const uint8_t *p = data.data() + pos;
            if (p[0] != 0x47)
            {
                ++pos; // lost sync, look for the next sync byte
                continue;
            }
            const int pid = ((p[1] & 0x1F) << 8) | p[2];
            if (pid == 0x12)
                eitReader.Packet(p, onEIT);
            else if (pid == 0x14 && eit.firstTime < 0)
                totReader.Packet(p, onTOT);
            pos += packetSize;
        }
    }
    avio_seek(pb, resume, SEEK_SET);
    return eit.events[0].present || eit.events[1].present;
}
