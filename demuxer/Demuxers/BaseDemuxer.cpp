/*
 *      Copyright (C) 2010-2021 Hendrik Leppkes
 *      http://www.1f0.de
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

#include "stdafx.h"
#include "BaseDemuxer.h"

#include "moreuuids.h"

CBaseDemuxer::CBaseDemuxer(LPCTSTR pName, CCritSec *pLock)
    : CUnknown(pName, nullptr)
    , m_pLock(pLock)
{
    for (int i = 0; i < unknown; ++i)
    {
        m_dActiveStreams[i] = -1;
    }
}

void CBaseDemuxer::CreateNoSubtitleStream()
{
    stream s;
    s.pid = NO_SUBTITLE_PID;
    s.streamInfo = new CStreamInfo();
    s.language = "und";
    // Create the media type
    CMediaType mtype;
    mtype.majortype = MEDIATYPE_Subtitle;
    mtype.subtype = MEDIASUBTYPE_UTF8;
    mtype.formattype = FORMAT_SubtitleInfo;
    SUBTITLEINFO *subInfo = (SUBTITLEINFO *)mtype.AllocFormatBuffer(sizeof(SUBTITLEINFO));
    memset(subInfo, 0, mtype.FormatLength());
    wcscpy_s(subInfo->TrackName, NO_SUB_STRING);
    strcpy_s(subInfo->IsoLang, "und");
    subInfo->dwOffset = sizeof(SUBTITLEINFO);
    s.streamInfo->mtypes.push_back(mtype);
    // Append it to the list
    m_streams[subpic].push_back(s);
}

void CBaseDemuxer::CreatePGSForcedSubtitleStream()
{
    stream s;
    s.pid = FORCED_SUBTITLE_PID;
    s.streamInfo = new CStreamInfo();
    s.language = "und";
    // Create the media type
    CMediaType mtype;
    mtype.majortype = MEDIATYPE_Subtitle;
    mtype.subtype = MEDIASUBTYPE_HDMVSUB;
    mtype.formattype = FORMAT_SubtitleInfo;
    SUBTITLEINFO *subInfo = (SUBTITLEINFO *)mtype.AllocFormatBuffer(sizeof(SUBTITLEINFO));
    memset(subInfo, 0, mtype.FormatLength());
    wcscpy_s(subInfo->TrackName, FORCED_SUB_STRING);
    subInfo->dwOffset = sizeof(SUBTITLEINFO);
    s.streamInfo->mtypes.push_back(mtype);
    // Append it to the list
    m_streams[subpic].push_back(s);
}

void CBaseDemuxer::CreateLateAribSubtitleStream()
{
    stream s;
    s.pid = LATE_ARIB_SUBTITLE_PID;
    s.streamInfo = new CStreamInfo();
    s.language = "jpn";
    s.lcid = 0;
    s.trackName = "ARIB Captions";
    s.streamInfo->codecInfo = "ARIB Captions";

    CMediaType mtype;
    mtype.majortype = MEDIATYPE_Subtitle;
    mtype.subtype = MEDIASUBTYPE_ASS;
    mtype.formattype = FORMAT_SubtitleInfo;

    std::string assHeader =
        "[Script Info]\r\n"
        "ScriptType: v4.00+\r\n"
        "PlayResX: 1920\r\n"
        "PlayResY: 1080\r\n"
        "WrapStyle: 2\r\n"
        "\r\n"
        "[V4+ Styles]\r\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
        "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, "
        "Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\r\n"
        "Style: Default,MS Gothic,64,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,0,0,2,20,20,20,1\r\n"
        "\r\n"
        "[Events]\r\n"
        "Format: ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n";

    SUBTITLEINFO *subInfo = (SUBTITLEINFO *)mtype.AllocFormatBuffer(sizeof(SUBTITLEINFO) + assHeader.size());
    memset(subInfo, 0, mtype.FormatLength());
    wcscpy_s(subInfo->TrackName, LATE_ARIB_SUB_STRING);
    strcpy_s(subInfo->IsoLang, "jpn");
    subInfo->dwOffset = sizeof(SUBTITLEINFO);
    memcpy(mtype.pbFormat + sizeof(SUBTITLEINFO), assHeader.c_str(), assHeader.size());
    s.streamInfo->mtypes.push_back(mtype);

    m_streams[subpic].push_back(s);
}

// CStreamList
const WCHAR *CBaseDemuxer::CStreamList::ToStringW(int type)
{
    return type == video ? L"Video" : type == audio ? L"Audio" : type == subpic ? L"Subtitle" : L"Unknown";
}

const CHAR *CBaseDemuxer::CStreamList::ToString(int type)
{
    return type == video ? "Video" : type == audio ? "Audio" : type == subpic ? "Subtitle" : "Unknown";
}

CBaseDemuxer::stream *CBaseDemuxer::CStreamList::FindStream(DWORD pid)
{
    std::deque<stream>::iterator it;
    for (it = begin(); it != end(); ++it)
    {
        if ((*it).pid == pid)
        {
            return &(*it);
        }
    }

    return nullptr;
}

void CBaseDemuxer::CStreamList::Clear()
{
    std::deque<stream>::iterator it;
    for (it = begin(); it != end(); ++it)
    {
        delete (*it).streamInfo;
    }
    __super::clear();
}

CBaseDemuxer::stream *CBaseDemuxer::FindStream(DWORD pid)
{
    for (int i = 0; i < StreamType::unknown; i++)
    {
        stream *pStream = m_streams[i].FindStream(pid);
        if (pStream)
            return pStream;
    }
    return nullptr;
}
