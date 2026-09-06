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
#include "LAVFDemuxer.h"
#include "LAVFUtils.h"
#include "LAVFStreamInfo.h"
#include "ILAVPinInfo.h"
#include "LAVFVideoHelper.h"
#include "ExtradataParser.h"
#include "IMediaSideDataFFmpeg.h"

#include "LAVSplitterSettingsInternal.h"

#include "moreuuids.h"
#include "AribCommon.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

extern "C"
{
    typedef struct CodecMime
    {
        char str[32];
        enum AVCodecID id;
    } CodecMime;
#include "libavformat/mpegts.h"
#include "libavformat/matroska.h"
#include "libavutil/avstring.h"

    enum AVCodecID ff_get_pcm_codec_id(int bps, int flt, int be, int sflags);
#include "libavformat/isom.h"
#include "libavformat/demux.h"
}

#ifdef DEBUG
#include "lavf_log.h"
#endif

// Fill iniPath with the path of the DLL with extension replaced by ".ini".
void AribGetIniPath(WCHAR *iniPath, DWORD size)
{
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&AribGetIniPath), &hMod);
    if (!GetModuleFileNameW(hMod, iniPath, size)) { iniPath[0] = L'\0'; return; }
    WCHAR *dot = wcsrchr(iniPath, L'.');
    if (dot) wcscpy_s(dot, size - (DWORD)(dot - iniPath), L".ini");
}

static void GetAribIniPath(WCHAR *iniPath, DWORD size)
{
    AribGetIniPath(iniPath, size);
}

// ASS script header used both for the subtitle media type extradata and for the
// late-binding placeholder pin, so the two never drift apart.
std::string AribBuildASSScriptHeader()
{
    WCHAR iniPath[MAX_PATH] = {};
    AribGetIniPath(iniPath, MAX_PATH);

    WCHAR fontW[256] = {};
    GetPrivateProfileStringW(L"ARIB", L"FontName", L"MS Gothic", fontW, _countof(fontW), iniPath);
    char fontA[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, fontW, -1, fontA, _countof(fontA), nullptr, nullptr);

    char styleLine[512];
    snprintf(styleLine, sizeof(styleLine),
             "Style: Default,%s,64,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
             "0,0,0,0,100,100,0,0,1,0,0,2,20,20,20,1\r\n", fontA);

    // Timing comes from IMediaSample::GetTime(), so the event format starts with
    // ReadOrder (matches mmts-dsfilter's convention and MPC-BE's expectation).
    return std::string(
               "[Script Info]\r\n"
               "ScriptType: v4.00+\r\n"
               "PlayResX: 1920\r\n"
               "PlayResY: 1080\r\n"
               "WrapStyle: 2\r\n"
               "\r\n"
               "[V4+ Styles]\r\n"
               "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, "
               "Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, "
               "Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\r\n") +
           styleLine +
           "\r\n"
           "[Events]\r\n"
           "Format: ReadOrder, Layer, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n";
}

struct AribDebugLogConfig
{
    std::wstring filePath;
    bool verbose = false;
};

static AribDebugLogConfig g_aribDebugLog;
static std::mutex g_aribDebugLogMutex;
// Log file kept open between writes; reopened when the configured path changes.
static HANDLE g_aribDebugLogFile = INVALID_HANDLE_VALUE;
static std::wstring g_aribDebugLogOpenPath;

// Caller must hold g_aribDebugLogMutex.
static void CloseAribDebugLogFile()
{
    if (g_aribDebugLogFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_aribDebugLogFile);
        g_aribDebugLogFile = INVALID_HANDLE_VALUE;
    }
    g_aribDebugLogOpenPath.clear();
}

void AribConfigureDebugLogging(const std::wstring &filePath, bool verbose)
{
    std::lock_guard<std::mutex> lock(g_aribDebugLogMutex);
    if (filePath != g_aribDebugLog.filePath)
        CloseAribDebugLogFile();
    g_aribDebugLog.filePath = filePath;
    g_aribDebugLog.verbose = verbose;
}

// True when anything would actually be written. Lets ARIB_LOG skip building its
// arguments while logging is off.
bool AribDebugLogEnabled()
{
    std::lock_guard<std::mutex> lock(g_aribDebugLogMutex);
#ifdef _DEBUG
    return true;
#else
    return g_aribDebugLog.verbose || !g_aribDebugLog.filePath.empty();
#endif
}

void AribConfigureDebugLoggingFromIni()
{
    WCHAR iniPath[MAX_PATH] = {};
    WCHAR path[MAX_PATH] = {};
    WCHAR verbose[32] = {};
    GetAribIniPath(iniPath, MAX_PATH);

    std::wstring debugLogPath;
    bool verboseLog = false;
    if (GetPrivateProfileStringW(L"ARIB", L"DebugLogPath", L"", path, _countof(path), iniPath) > 0)
        debugLogPath = path;
    if (GetPrivateProfileStringW(L"ARIB", L"VerboseLog", L"", verbose, _countof(verbose), iniPath) > 0)
        verboseLog = _wtoi(verbose) != 0;

    AribConfigureDebugLogging(debugLogPath, verboseLog);
}

static bool GetAribDebugLogConfig(std::wstring &filePath, bool &verbose)
{
    std::lock_guard<std::mutex> lock(g_aribDebugLogMutex);
#ifdef _DEBUG
    verbose = true;
#else
    verbose = g_aribDebugLog.verbose;
#endif
    // Copy the path only when something is actually going to be written.
    if (!verbose && g_aribDebugLog.filePath.empty())
        return false;
    filePath = g_aribDebugLog.filePath;
    return true;
}

void AribDbgLog(const char *fmt, ...)
{
    std::wstring filePath;
    bool verbose = false;
    if (!GetAribDebugLogConfig(filePath, verbose))
        return;

    char stackBuf[2048];
    va_list args;
    va_start(args, fmt);
    va_list argsCopy;
    va_copy(argsCopy, args);
    int needed = vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
    va_end(args);

    const char *text = stackBuf;
    std::vector<char> dynamicBuf;
    if (needed < 0)
    {
        va_end(argsCopy);
        return;
    }
    if (needed >= static_cast<int>(sizeof(stackBuf)))
    {
        dynamicBuf.resize(static_cast<size_t>(needed) + 1);
        vsnprintf(dynamicBuf.data(), dynamicBuf.size(), fmt, argsCopy);
        text = dynamicBuf.data();
    }
    va_end(argsCopy);

    if (verbose)
        OutputDebugStringA(text);

    if (filePath.empty())
        return;

    std::lock_guard<std::mutex> lock(g_aribDebugLogMutex);
    if (g_aribDebugLogFile == INVALID_HANDLE_VALUE || g_aribDebugLogOpenPath != filePath)
    {
        CloseAribDebugLogFile();
        g_aribDebugLogFile = CreateFileW(filePath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_aribDebugLogFile == INVALID_HANDLE_VALUE)
            return;
        g_aribDebugLogOpenPath = filePath;
    }

    DWORD ignored = 0;
    WriteFile(g_aribDebugLogFile, text, static_cast<DWORD>(strlen(text)), &ignored, nullptr);
}
// Skip evaluating the arguments (some of them build strings) unless logging is on.
#define ARIB_LOG(...) ((void)(AribDebugLogEnabled() && (AribDbgLog(__VA_ARGS__), 0)))

#include "BDDemuxer.h"
#include "CueSheet.h"

#define AVFORMAT_OPEN_TIMEOUT 20

extern void lavf_get_iformat_infos(const AVInputFormat *pFormat, const char **pszName, const char **pszDescription);

static const AVRational AV_RATIONAL_TIMEBASE = {1, AV_TIME_BASE};

std::set<FormatInfo> CLAVFDemuxer::GetFormatList()
{
    std::set<FormatInfo> formats;
    const AVInputFormat *f = nullptr;
    void *state = NULL;

    while (f = av_demuxer_iterate(&state))
    {
        FormatInfo format;
        lavf_get_iformat_infos(f, &format.strName, &format.strDescription);
        if (format.strName)
            formats.insert(format);
    }
    return formats;
}

CLAVFDemuxer::CLAVFDemuxer(CCritSec *pLock, ILAVFSettingsInternal *settings)
    : CBaseDemuxer(L"lavf demuxer", pLock)
{
#ifdef DEBUG
    DbgSetModuleLevel(LOG_CUSTOM1, DWORD_MAX); // FFMPEG messages use custom1
    av_log_set_callback(lavf_log_callback);
#else
    av_log_set_callback(nullptr);
#endif

    m_bSubStreams = settings->GetSubstreamsEnabled();

    m_pSettings = settings;

    WCHAR fileName[1024];
    GetModuleFileName(nullptr, fileName, 1024);
    const WCHAR *file = PathFindFileName(fileName);

    if (_wcsicmp(file, L"zplayer.exe") == 0)
    {
        m_bEnableTrackInfo = FALSE;

        // TrackInfo is only properly handled in ZoomPlayer 8.0.0.74 and above
        DWORD dwVersionSize = GetFileVersionInfoSize(fileName, nullptr);
        if (dwVersionSize > 0)
        {
            void *versionInfo = CoTaskMemAlloc(dwVersionSize);
            if (!versionInfo)
                return;
            GetFileVersionInfo(fileName, 0, dwVersionSize, versionInfo);
            VS_FIXEDFILEINFO *info;
            unsigned cbInfo;
            BOOL bInfoPresent = VerQueryValue(versionInfo, TEXT("\\"), (LPVOID *)&info, &cbInfo);
            if (bInfoPresent)
            {
                bInfoPresent = bInfoPresent;
                uint64_t version = info->dwFileVersionMS;
                version <<= 32;
                version += info->dwFileVersionLS;
                if (version >= 0x000800000000004A)
                    m_bEnableTrackInfo = TRUE;
            }
            CoTaskMemFree(versionInfo);
        }
    }
}

CLAVFDemuxer::~CLAVFDemuxer()
{
    CleanupAVFormat();
    SAFE_DELETE(m_pFontInstaller);
    CleanupAribDecoders();
}

// Drop caption packets that are waiting for their stop time, plus the per-region
// packets queued behind them. Used on seek and whenever the subtitle selection
// changes, so packets addressed to the previous pin are not delivered late.
void CLAVFDemuxer::FlushAribPendingPackets()
{
    for (auto &kv : m_aribPendingPackets)
        delete kv.second;
    m_aribPendingPackets.clear();
    m_aribPendingExplicitStop.clear();
    for (auto &kv : m_aribPendingExtras)
        for (auto *p : kv.second)
            delete p;
    m_aribPendingExtras.clear();
    for (auto *p : m_aribRegionQueue)
        delete p;
    m_aribRegionQueue.clear();
}

void CLAVFDemuxer::CleanupAribDecoders()
{
    for (auto &kv : m_aribDecoders)
        aribcc_decoder_free(kv.second);
    m_aribDecoders.clear();
    for (auto &kv : m_aribContexts)
        aribcc_context_free(kv.second);
    m_aribContexts.clear();

    for (auto &kv : m_aribSuperDecoders)
        aribcc_decoder_free(kv.second);
    m_aribSuperDecoders.clear();
    for (auto &kv : m_aribSuperContexts)
        aribcc_context_free(kv.second);
    m_aribSuperContexts.clear();

    FlushAribPendingPackets();
}

aribcc_decoder_t *CLAVFDemuxer::GetOrCreateAribDecoder(int streamIndex, bool superimpose)
{
    auto &decoderMap = superimpose ? m_aribSuperDecoders : m_aribDecoders;
    auto &contextMap = superimpose ? m_aribSuperContexts : m_aribContexts;
    const aribcc_profile_t profile =
        m_avFormat->streams[streamIndex]->codecpar->profile == AV_PROFILE_ARIB_PROFILE_C
            ? ARIBCC_PROFILE_C : ARIBCC_PROFILE_A;

    auto it = decoderMap.find(streamIndex);
    if (it != decoderMap.end())
        return it->second;

    aribcc_context_t *ctx = aribcc_context_alloc();
    if (!ctx)
        return nullptr;

    aribcc_decoder_t *dec = aribcc_decoder_alloc(ctx);
    if (!dec)
    {
        aribcc_context_free(ctx);
        return nullptr;
    }

    aribcc_captiontype_t type = superimpose ? ARIBCC_CAPTIONTYPE_SUPERIMPOSE : ARIBCC_CAPTIONTYPE_CAPTION;
    if (!aribcc_decoder_initialize(dec, ARIBCC_ENCODING_SCHEME_AUTO, type,
                                   profile, ARIBCC_LANGUAGEID_FIRST))
    {
        aribcc_decoder_free(dec);
        aribcc_context_free(ctx);
        return nullptr;
    }

    contextMap[streamIndex] = ctx;
    decoderMap[streamIndex]  = dec;
    return dec;
}

// True while the placeholder subtitle pin is the selected subtitle stream,
// regardless of which ARIB stream it ended up bound to.
bool CLAVFDemuxer::IsLateAribPlaceholderSelected() const
{
    return m_dActiveStreams[subpic] == (int)LATE_ARIB_SUBTITLE_PID;
}

bool CLAVFDemuxer::IsLateAribSubtitleActive(int streamIndex) const
{
    return IsLateAribPlaceholderSelected() &&
           (m_LateAribSubtitleStream == -1 || m_LateAribSubtitleStream == streamIndex);
}

// ARIB caption streams must keep flowing while the placeholder pin is selected
// but not yet bound, and after binding so a caption stream can take over from
// superimpose.
bool CLAVFDemuxer::IsLateAribCandidateStream(int streamIndex) const
{
    if (!IsLateAribPlaceholderSelected() || streamIndex < 0 ||
        (unsigned)streamIndex >= m_avFormat->nb_streams)
        return false;
    return m_avFormat->streams[streamIndex]->codecpar->codec_id == AV_CODEC_ID_ARIB_CAPTION;
}

static int AribSectionWidth(const aribcc_caption_char_t &ch)
{
    return (int)std::floor((ch.char_width + ch.char_horizontal_spacing) * ch.char_horizontal_scale);
}

static int AribSectionHeight(const aribcc_caption_char_t &ch)
{
    return (int)std::floor((ch.char_height + ch.char_vertical_spacing) * ch.char_vertical_scale);
}

struct AribCaptionSettings
{
    int          captionAlpha       = 0;    // ASS \1a: 0=opaque, 255=fully transparent
    int          backgroundAlpha    = -1;   // -1=use ARIB data, 0-255=fixed override
    bool         showBackground     = true;
    bool         showRubyBackground = true; // whether to draw background behind ruby text
    bool         backgroundPadding  = true; // extend background to full ARIB cell (equal top/bottom padding)
    int          outlineWidth       = 0;    // ASS \bord value (0=no outline)
    std::string  fontName;                  // empty = use style default (MS Gothic)
    int          delayMs            = 0;    // timing offset in ms (can be negative)
    std::vector<uint32_t> stretchChars;      // Unicode codepoints to stretch horizontally
    int          stretchScale        = 100;  // extra ASS \fscx scale for stretchChars
};

static uint32_t DecodeFirstUTF8Codepoint(const char *s);

// Read settings from one INI section.  Keys absent from the section keep the
// value already in |s| (i.e. the caller may pre-fill |s| as a fallback).
static void ReadIniSection(AribCaptionSettings &s, const WCHAR *iniPath, const WCHAR *section)
{
    WCHAR buf[MAX_PATH] = {};

    auto present = [&](const WCHAR *key) -> bool {
        return GetPrivateProfileStringW(section, key, L"", buf, _countof(buf), iniPath) > 0;
    };

    if (present(L"CaptionTransparency"))
    {
        int t = max(0, min(100, _wtoi(buf)));
        s.captionAlpha = t * 255 / 100;
    }
    if (present(L"BackgroundTransparency"))
    {
        int t = max(0, min(100, _wtoi(buf)));
        s.backgroundAlpha = t * 255 / 100;
    }
    if (present(L"ShowBackground"))
        s.showBackground = _wtoi(buf) != 0;
    if (present(L"ShowRubyBackground"))
        s.showRubyBackground = _wtoi(buf) != 0;
    if (present(L"BackgroundPadding"))
        s.backgroundPadding = _wtoi(buf) != 0;
    if (present(L"OutlineWidth"))
        s.outlineWidth = max(0, min(10, _wtoi(buf)));
    if (present(L"FontName"))
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1) { s.fontName.resize(len - 1); WideCharToMultiByte(CP_UTF8, 0, buf, -1, &s.fontName[0], len, nullptr, nullptr); }
    }
    if (present(L"DelayMs"))
        s.delayMs = max(-30000, min(30000, _wtoi(buf)));
    if (present(L"StretchChars"))
    {
        s.stretchChars.clear();

        int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        if (len > 1)
        {
            std::string utf8(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, &utf8[0], len, nullptr, nullptr);

            for (size_t i = 0; i < utf8.size();)
            {
                uint32_t cp = DecodeFirstUTF8Codepoint(utf8.c_str() + i);
                if (cp == 0)
                {
                    i++;
                    continue;
                }
                if (std::find(s.stretchChars.begin(), s.stretchChars.end(), cp) == s.stretchChars.end())
                    s.stretchChars.push_back(cp);

                if (cp < 0x80) i += 1;
                else if (cp < 0x800) i += 2;
                else if (cp < 0x10000) i += 3;
                else i += 4;
            }
        }
    }
    if (present(L"StretchScale"))
        s.stretchScale = max(10, min(400, _wtoi(buf)));
}

// Load settings for caption (isSuperimpose=false) or superimpose (true).
// [Superimpose] inherits from [ARIB] for any key not explicitly set.
//
// This runs for every caption event, so the result is cached and only re-read
// when the INI's last write time changes (checked at most once a second).
struct AribSettingsCache
{
    bool valid = false;
    AribCaptionSettings settings;
    FILETIME iniWriteTime{};
    ULONGLONG lastCheckTick = 0;
};

static AribSettingsCache g_aribSettingsCache[2];
static std::mutex g_aribSettingsMutex;

static AribCaptionSettings GetAribCaptionSettings(bool isSuperimpose = false)
{
    WCHAR iniPath[MAX_PATH] = {};
    GetAribIniPath(iniPath, MAX_PATH);

    std::lock_guard<std::mutex> lock(g_aribSettingsMutex);
    AribSettingsCache &cache = g_aribSettingsCache[isSuperimpose ? 1 : 0];

    ULONGLONG now = GetTickCount64();
    if (cache.valid && now - cache.lastCheckTick < 1000)
        return cache.settings;
    cache.lastCheckTick = now;

    FILETIME writeTime = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExW(iniPath, GetFileExInfoStandard, &attributes))
        writeTime = attributes.ftLastWriteTime;
    if (cache.valid && CompareFileTime(&writeTime, &cache.iniWriteTime) == 0)
        return cache.settings;

    AribCaptionSettings s;
    ReadIniSection(s, iniPath, L"ARIB");
    if (isSuperimpose)
        ReadIniSection(s, iniPath, L"Superimpose");
    AribConfigureDebugLoggingFromIni();

    cache.settings = s;
    cache.iniWriteTime = writeTime;
    cache.valid = true;
    return s;
}

static uint32_t DecodeFirstUTF8Codepoint(const char *s)
{
    const auto *u = reinterpret_cast<const unsigned char *>(s);
    if (!u || !u[0])
        return 0;
    if (u[0] < 0x80)
        return u[0];
    if ((u[0] & 0xE0) == 0xC0 && (u[1] & 0xC0) == 0x80)
        return ((uint32_t)(u[0] & 0x1F) << 6) | (uint32_t)(u[1] & 0x3F);
    if ((u[0] & 0xF0) == 0xE0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80)
        return ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) |
               (uint32_t)(u[2] & 0x3F);
    if ((u[0] & 0xF8) == 0xF0 && (u[1] & 0xC0) == 0x80 && (u[2] & 0xC0) == 0x80 &&
        (u[3] & 0xC0) == 0x80)
        return ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) |
               ((uint32_t)(u[2] & 0x3F) << 6) | (uint32_t)(u[3] & 0x3F);
    return 0;
}

static bool IsUnicodeHalfwidthGlyph(const aribcc_caption_char_t &ch)
{
    uint32_t cp = DecodeFirstUTF8Codepoint(ch.u8str);
    return cp < 0x80 || (cp >= 0xFF61 && cp <= 0xFF9F);
}

static bool ShouldStretchASSGlyph(const AribCaptionSettings &settings, const aribcc_caption_char_t &ch)
{
    if (settings.stretchScale == 100 || settings.stretchChars.empty() || ch.type == ARIBCC_CHARTYPE_DRCS)
        return false;

    uint32_t cp = DecodeFirstUTF8Codepoint(ch.u8str);
    return cp != 0 && std::find(settings.stretchChars.begin(), settings.stretchChars.end(), cp) != settings.stretchChars.end();
}

// ---------------------------------------------------------------------------
// DRCS utilities
// ---------------------------------------------------------------------------

// Number of bytes libaribcaption stores for a DRCS bitmap. Pixels are packed
// as a continuous bit stream, so a pixel may straddle a byte boundary.
static size_t DRCSBitmapSize(int w, int h, int depth_bits)
{
    if (w <= 0 || h <= 0 || depth_bits <= 0 || depth_bits > 8)
        return 0;
    return ((size_t)w * (size_t)h * (size_t)depth_bits + 7) / 8;
}

// Unpack ARIB DRCS bit-packed pixels to 8-bit grayscale (0=black, 255=white).
// rawSz is the size of the buffer behind |raw|; short buffers are rejected so a
// malformed stream cannot make us read past the end of the bitmap.
static std::vector<uint8_t> UnpackDRCSPixels(const uint8_t *raw, size_t rawSz, int w, int h, int depth_bits)
{
    size_t needed = DRCSBitmapSize(w, h, depth_bits);
    std::vector<uint8_t> gray((size_t)max(w, 0) * (size_t)max(h, 0), 255);
    if (!raw || needed == 0 || rawSz < needed)
        return gray;

    int maxVal = (1 << depth_bits) - 1;
    for (size_t i = 0; i < gray.size(); ++i)
    {
        // Read depth_bits bits starting at bit offset i * depth_bits (MSB first).
        size_t bitPos = i * (size_t)depth_bits;
        size_t byteIdx = bitPos >> 3;
        int bitInByte = (int)(bitPos & 7);
        int val = 0;
        for (int b = 0; b < depth_bits; ++b)
        {
            int bit = (raw[byteIdx] >> (7 - bitInByte)) & 1;
            val = (val << 1) | bit;
            if (++bitInByte == 8)
            {
                bitInByte = 0;
                byteIdx++;
            }
        }
        gray[i] = (uint8_t)(255 - val * 255 / maxVal);
    }
    return gray;
}

static double CaptionPlaneToASSScale(const aribcc_caption_t &caption);

// ASS ReadOrder counter shared by every caption event of this process.
static LONG g_aribReadOrder = 0;

static LONG NextAribReadOrder()
{
    return InterlockedIncrement(&g_aribReadOrder);
}

// Copy a packet including its payload. Returns nullptr if it cannot be copied.
static Packet *ClonePacket(Packet *src)
{
    if (!src)
        return nullptr;
    Packet *copy = new Packet();
    copy->CopyProperties(src);
    if (src->GetDataSize() > 0 && copy->SetData(src->GetData(), src->GetDataSize()) < 0)
    {
        delete copy;
        return nullptr;
    }
    return copy;
}

// Replace the leading "ReadOrder," field of an ASS dialogue payload.
static void RenumberASSReadOrder(Packet *packet, LONG readOrder)
{
    if (!packet || packet->GetDataSize() <= 0)
        return;

    const char *data = reinterpret_cast<const char *>(packet->GetData());
    int size = packet->GetDataSize();
    const char *comma = static_cast<const char *>(memchr(data, ',', size));
    if (!comma)
        return;

    char prefix[32];
    int prefixLen = snprintf(prefix, sizeof(prefix), "%ld", readOrder);
    if (prefixLen <= 0)
        return;

    std::string payload;
    payload.reserve((size_t)prefixLen + (size - (comma - data)));
    payload.assign(prefix, prefixLen);
    payload.append(comma, size - (comma - data));
    packet->SetData(payload.c_str(), (int)payload.size());
}


static void AppendASSDrawingPoint(std::string &out, int v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", v);
    out += buf;
}

static std::string BuildDRCSDrawingText(const aribcc_caption_t &caption,
                                        const aribcc_caption_char_t &ch)
{
    if (ch.type != ARIBCC_CHARTYPE_DRCS || !caption.drcs_map)
        return {};

    aribcc_drcs_t *drcs = aribcc_drcsmap_get(caption.drcs_map, ch.drcs_code);
    if (!drcs)
        return {};

    int w = 0, h = 0, depth = 0, depth_bits = 0;
    aribcc_drcs_get_size(drcs, &w, &h);
    aribcc_drcs_get_depth(drcs, &depth, &depth_bits);

    uint8_t *raw = nullptr;
    size_t rawSz = 0;
    aribcc_drcs_get_pixels(drcs, &raw, &rawSz);
    if (!raw || w <= 0 || h <= 0 || depth_bits <= 0 || depth_bits > 8)
        return {};

    size_t needed = DRCSBitmapSize(w, h, depth_bits);
    if (needed == 0 || rawSz < needed)
    {
        ARIB_LOG("[ARIB] DRCS bitmap too small: %dx%d depth_bits=%d need=%zu have=%zu\n",
                 w, h, depth_bits, needed, rawSz);
        return {};
    }

    auto gray = UnpackDRCSPixels(raw, rawSz, w, h, depth_bits);
    double scale = CaptionPlaneToASSScale(caption);
    double drawW = ch.char_width * ch.char_horizontal_scale * scale;
    double drawH = ch.char_height * ch.char_vertical_scale * scale;
    if (drawW <= 0 || drawH <= 0)
        return {};

    // One rectangle per horizontal run of set pixels rather than per pixel:
    // a 36x36 glyph would otherwise produce over a thousand rectangles.
    std::string path;
    path.reserve((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
    {
        int runStart = -1;
        for (int x = 0; x <= w; ++x)
        {
            const bool set = (x < w) && gray[(size_t)y * w + x] < 250;
            if (set)
            {
                if (runStart < 0)
                    runStart = x;
                continue;
            }
            if (runStart < 0)
                continue;

            int x1 = (int)std::floor(runStart * drawW / w);
            int y1 = (int)std::floor(y * drawH / h);
            int x2 = (int)std::ceil(x * drawW / w);
            int y2 = (int)std::ceil((y + 1) * drawH / h);
            runStart = -1;
            if (x1 >= x2 || y1 >= y2)
                continue;

            path += "m ";
            AppendASSDrawingPoint(path, x1);
            path += " ";
            AppendASSDrawingPoint(path, y1);
            path += " l ";
            AppendASSDrawingPoint(path, x2);
            path += " ";
            AppendASSDrawingPoint(path, y1);
            path += " ";
            AppendASSDrawingPoint(path, x2);
            path += " ";
            AppendASSDrawingPoint(path, y2);
            path += " ";
            AppendASSDrawingPoint(path, x1);
            path += " ";
            AppendASSDrawingPoint(path, y2);
            path += " ";
        }
    }
    if (path.empty())
        return {};

    return "{\\bord0\\p1}" + path + "{\\p0}";
}

// ---------------------------------------------------------------------------

static double CaptionPlaneToASSScale(const aribcc_caption_t &caption)
{
    if (caption.plane_width <= 0 || caption.plane_height <= 0)
        return 1.0;

    double scaleX = 1920.0 / caption.plane_width;
    double scaleY = 1080.0 / caption.plane_height;
    return (std::min)(scaleX, scaleY);
}

static int ScaleCaptionXToASSArea(const aribcc_caption_t &caption, double x)
{
    if (caption.plane_width <= 0 || caption.plane_height <= 0)
        return (int)std::floor(x);

    double scale = CaptionPlaneToASSScale(caption);
    double offsetX = (1920.0 - caption.plane_width * scale) / 2.0;
    return (int)std::floor(offsetX + x * scale);
}

static int ScaleCaptionYToASSArea(const aribcc_caption_t &caption, double y)
{
    if (caption.plane_width <= 0 || caption.plane_height <= 0)
        return (int)std::floor(y);

    double scale = CaptionPlaneToASSScale(caption);
    double offsetY = (1080.0 - caption.plane_height * scale) / 2.0;
    return (int)std::floor(offsetY + y * scale);
}

static std::string BuildASSPositionTag(const aribcc_caption_t &caption, double x, double y)
{
    char posBuf[64];
    if (caption.plane_width > 0 && caption.plane_height > 0)
    {
        int posX = ScaleCaptionXToASSArea(caption, x);
        int posY = ScaleCaptionYToASSArea(caption, y);
        snprintf(posBuf, sizeof(posBuf), "{\\an7\\pos(%d,%d)}", posX, posY);
    }
    else
    {
        snprintf(posBuf, sizeof(posBuf), "{\\an7}");
    }
    return posBuf;
}

struct AribASSRegion
{
    int x = 0;
    int y = 0;
    double textX = 0.0;
    double textY = 0.0;
    int right = 0;
    int charHeight = 0;   // glyph height only (excludes vertical spacing)
    int cellHeight = 0;   // full ARIB cell height (char_height + vertical_spacing)
    bool isRuby     = false;
    bool isVertical = false;
    uint32_t backColor = 0;
    std::string text;
};

// Append caption text as ASS dialogue text. ARIB includes the ASCII set, so the
// text can contain '{', '}' and '\\', which would otherwise be taken as an
// override block or a tag like \N. Same approach as ffmpeg's
// ff_ass_bprint_text_event(): escape '{' for libass and add an empty block for
// standard ASS, break up backslash sequences with a word joiner (U+2060), and
// turn line breaks into \N.
static void AppendASSEscapedText(std::string &out, const char *utf8)
{
    if (!utf8)
        return;

    for (const char *p = utf8; *p; p++)
    {
        switch (*p)
        {
        case '{':
            out += "\\{{}";
            break;
        case '\\':
            out += "\\\xe2\x81\xa0";
            break;
        case '\r':
            break;
        case '\n':
            out += "\\N";
            break;
        default:
            out += *p;
            break;
        }
    }
}

// True when consecutive characters in the region advance in Y (vertical writing).
static bool IsVerticalRegion(const aribcc_caption_region_t &region)
{
    if (region.char_count < 2) return false;
    int dy = region.chars[1].y - region.chars[0].y;
    int dx = region.chars[1].x - region.chars[0].x;
    return std::abs(dy) > std::abs(dx);
}

static double AribTextOffsetX(const aribcc_caption_char_t &ch)
{
    return ch.char_horizontal_spacing * ch.char_horizontal_scale / 2.0;
}

static double AribTextOffsetY(const aribcc_caption_char_t &ch)
{
    return ch.char_vertical_spacing * ch.char_vertical_scale / 2.0;
}

// Build style + text payload for a single ARIB character (no \fsp).
static std::string BuildASSSingleCharText(const aribcc_caption_t &caption,
                                          const aribcc_caption_char_t &ch,
                                          const AribCaptionSettings &settings)
{
    std::string result;
    int scaledH = (int)std::floor(ch.char_height * ch.char_vertical_scale);
    int assFs   = (caption.plane_height > 0 && scaledH > 0)
                  ? (int)(scaledH * 1080.0 / caption.plane_height) : 0;
    if (assFs > 0) { char b[32]; snprintf(b, sizeof(b), "{\\fs%d}", assFs); result += b; }
    float effectiveHScale = ch.char_horizontal_scale;
    bool needsGlyphSquish = ch.type != ARIBCC_CHARTYPE_DRCS &&
                            effectiveHScale < 0.99f &&
                            !IsUnicodeHalfwidthGlyph(ch);
    int fscx = needsGlyphSquish ? (int)std::lround(effectiveHScale * 100.0f) : 100;
    if (ShouldStretchASSGlyph(settings, ch))
        fscx = (int)std::lround((double)fscx * settings.stretchScale / 100.0);
    if (fscx != 100)
    {
        char b[32];
        snprintf(b, sizeof(b), "{\\fscx%d}", fscx);
        result += b;
    }
    {
        char b[64];
        snprintf(b, sizeof(b), "{\\c&H%02X%02X%02X&}",
                 ARIBCC_COLOR_B(ch.text_color),
                 ARIBCC_COLOR_G(ch.text_color),
                 ARIBCC_COLOR_R(ch.text_color));
        result += b;
    }
    if (ch.style & ARIBCC_CHARSTYLE_BOLD)      result += "{\\b1}";
    if (ch.style & ARIBCC_CHARSTYLE_ITALIC)    result += "{\\i1}";
    if (ch.style & ARIBCC_CHARSTYLE_UNDERLINE) result += "{\\u1}";
    if (ch.type == ARIBCC_CHARTYPE_DRCS)
        result += BuildDRCSDrawingText(caption, ch);
    else if (ch.u8str[0] != '\0')
        AppendASSEscapedText(result, ch.u8str);
    return result;
}

struct ASSEvent
{
    int layer = 1;
    std::string payload;
};

// Returns ASS Dialogue payloads. Horizontal, ruby, and vertical captions are
// emitted one ARIB cell at a time so layout follows the broadcast grid rather
// than renderer-specific font metrics.
static std::vector<ASSEvent> BuildASSFromCaption(const aribcc_caption_t &caption,
                                                  const AribCaptionSettings &settings)
{
    // Build an ASS Dialogue line body from libaribcaption output.
    std::vector<AribASSRegion> regions;
    for (uint32_t ri = 0; ri < caption.region_count; ri++)
    {
        const aribcc_caption_region_t &region = caption.regions[ri];
        if (region.char_count == 0)
            continue;

        // ARIB/libaribcaption region coordinates are top-left based. Keep the
        // same anchor in ASS so smaller ruby text does not drift vertically.

        // Log all region char positions for diagnosis
        if (region.char_count > 0)
        {
            const aribcc_caption_char_t &fc = region.chars[0];
            const aribcc_caption_char_t &lc = region.chars[region.char_count - 1];
            ARIB_LOG("[ARIB] region[%u] is_ruby=%d rx=%d ry=%d rw=%d rh=%d "
                     "chars=%u firstCh x=%d w=%d h=%d lastCh x=%d w=%d\n",
                     ri, (int)region.is_ruby,
                     region.x, region.y, region.width, region.height,
                     region.char_count,
                     fc.x, fc.char_width, fc.char_height,
                     lc.x, lc.char_width);
        }

        if (caption.plane_width > 0 && caption.plane_height > 0)
        {
            // Vertical writing: split into one AribASSRegion per character.
            if (!region.is_ruby && IsVerticalRegion(region))
            {
                for (uint32_t ci = 0; ci < region.char_count; ci++)
                {
                    const aribcc_caption_char_t &ch = region.chars[ci];
                    AribASSRegion vr;
                    vr.x          = ch.x;
                    vr.y          = ch.y;
                    vr.textX      = ch.x + AribTextOffsetX(ch);
                    vr.textY      = ch.y + AribTextOffsetY(ch);
                    vr.right      = ch.x + AribSectionWidth(ch);
                    vr.charHeight = (int)std::floor(ch.char_height * ch.char_vertical_scale);
                    vr.cellHeight = AribSectionHeight(ch);
                    vr.isVertical = true;
                    vr.backColor  = ch.back_color;
                    vr.text       = BuildASSSingleCharText(caption, ch, settings);
                    if (!vr.text.empty())
                        regions.push_back(vr);
                }
                continue;
            }

            // ARIB-cell layout for horizontal captions and ruby. Each character
            // is positioned at its transmitted cell coordinate, and backgrounds
            // use ARIB cell width instead of renderer-specific font metrics.
            for (uint32_t ci = 0; ci < region.char_count; ci++)
            {
                const aribcc_caption_char_t &ch = region.chars[ci];
                AribASSRegion cr;
                cr.x          = ch.x;
                cr.y          = ch.y;
                cr.textX      = ch.x + AribTextOffsetX(ch);
                cr.textY      = ch.y + AribTextOffsetY(ch);
                cr.right      = ch.x + AribSectionWidth(ch);
                cr.charHeight = (int)std::floor(ch.char_height * ch.char_vertical_scale);
                cr.cellHeight = AribSectionHeight(ch);
                cr.isRuby     = region.is_ruby;
                cr.backColor  = ch.back_color;
                cr.text       = BuildASSSingleCharText(caption, ch, settings);
                if (!cr.text.empty())
                    regions.push_back(cr);
            }
        }
        else
        {
            for (uint32_t ci = 0; ci < region.char_count; ci++)
            {
                const aribcc_caption_char_t &ch = region.chars[ci];
                AribASSRegion cr;
                cr.x          = ch.x;
                cr.y          = ch.y;
                cr.textX      = ch.x + AribTextOffsetX(ch);
                cr.textY      = ch.y + AribTextOffsetY(ch);
                cr.right      = ch.x + AribSectionWidth(ch);
                cr.charHeight = (int)std::floor(ch.char_height * ch.char_vertical_scale);
                cr.cellHeight = AribSectionHeight(ch);
                cr.isRuby     = region.is_ruby;
                cr.backColor  = ch.back_color;
                cr.text       = BuildASSSingleCharText(caption, ch, settings);
                if (!cr.text.empty())
                    regions.push_back(cr);
            }
        }
    }

    std::stable_sort(regions.begin(), regions.end(), [](const AribASSRegion &a, const AribASSRegion &b) {
        if (a.isRuby != b.isRuby)
            return !a.isRuby;
        if (a.y != b.y)
            return a.y < b.y;
        return a.x < b.x;
    });

    // Pre-build per-event override tags (empty when at default).
    char captionAlphaTag[16] = {};
    if (settings.captionAlpha > 0)
        snprintf(captionAlphaTag, sizeof(captionAlphaTag), "{\\1a&H%02X&}", (uint8_t)settings.captionAlpha);

    char outlineTag[16] = {};
    if (settings.outlineWidth > 0)
        snprintf(outlineTag, sizeof(outlineTag), "{\\bord%d}", settings.outlineWidth);

    // \fn tag - applied once per event; empty if no custom font.
    std::string fontTag;
    if (!settings.fontName.empty())
        fontTag = "{\\fn" + settings.fontName + "}";

    std::vector<ASSEvent> results;
    // Emit a Layer 0 background rect using the ARIB cell width.
    // When backgroundPadding is true the rect spans the full ARIB cell (y .. y+cellHeight),
    // giving equal top/bottom padding from the row spacing; otherwise it covers the glyph only.
    auto emitBackgroundRect = [&](const AribASSRegion &r) {
        if (!settings.showBackground) return;
        double top    = settings.backgroundPadding ? (double)r.y : r.textY;
        int    height = settings.backgroundPadding ? r.cellHeight : r.charHeight;
        int assFs = (caption.plane_height > 0 && height > 0)
                    ? (int)(height * 1080.0 / caption.plane_height) : 0;
        if (assFs <= 0) return;

        int x1 = ScaleCaptionXToASSArea(caption, r.x);
        int y1 = ScaleCaptionYToASSArea(caption, top);
        int x2 = ScaleCaptionXToASSArea(caption, r.right);
        int y2 = ScaleCaptionYToASSArea(caption, top + height);
        uint32_t backColor = r.backColor;
        if (x1 >= x2 || y1 >= y2) return;

        uint8_t b = ARIBCC_COLOR_B(backColor);
        uint8_t g = ARIBCC_COLOR_G(backColor);
        uint8_t rv = ARIBCC_COLOR_R(backColor);
        uint8_t assAlpha;
        if (settings.backgroundAlpha >= 0)
        {
            if (settings.backgroundAlpha >= 255) return;
            assAlpha = (uint8_t)settings.backgroundAlpha;
        }
        else
        {
            uint8_t a = ARIBCC_COLOR_A(backColor);
            if (a == 0) return;
            assAlpha = 255 - a;
        }
        char buf[256];
        snprintf(buf, sizeof(buf),
            "{\\an7\\pos(0,0)\\p1\\1c&H%02X%02X%02X&\\1a&H%02X&}"
            "m %d %d l %d %d %d %d %d %d{\\p0}",
            b, g, rv, assAlpha, x1, y1, x2, y1, x2, y2, x1, y2);
        results.push_back({0, std::string(buf)});
    };

    bool hasBackgroundRun = false;
    AribASSRegion backgroundRun;
    auto flushBackgroundRun = [&]() {
        if (!hasBackgroundRun) return;
        emitBackgroundRect(backgroundRun);
        hasBackgroundRun = false;
    };

    for (const auto &region : regions)
    {
        if (region.isRuby)
            continue;

        if (region.isVertical)
        {
            flushBackgroundRun();
            emitBackgroundRect(region);
            continue;
        }

        const bool sameRun =
            hasBackgroundRun &&
            backgroundRun.y == region.y &&
            std::abs(backgroundRun.textY - region.textY) < 0.5 &&
            backgroundRun.charHeight == region.charHeight &&
            backgroundRun.backColor == region.backColor &&
            region.x <= backgroundRun.right;

        if (!sameRun)
        {
            flushBackgroundRun();
            backgroundRun = region;
            hasBackgroundRun = true;
            continue;
        }

        if (region.right > backgroundRun.right)
            backgroundRun.right = region.right;
    }
    flushBackgroundRun();

    // Ruby background pass - only when ShowRubyBackground=1.
    if (settings.showRubyBackground)
    {
        bool hasRubyRun = false;
        AribASSRegion rubyRun;
        auto flushRubyRun = [&]() {
            if (!hasRubyRun) return;
            emitBackgroundRect(rubyRun);
            hasRubyRun = false;
        };

        for (const auto &region : regions)
        {
            if (!region.isRuby)
                continue;

            const bool sameRun =
                hasRubyRun &&
                rubyRun.y == region.y &&
                std::abs(rubyRun.textY - region.textY) < 0.5 &&
                rubyRun.charHeight == region.charHeight &&
                rubyRun.backColor == region.backColor &&
                region.x <= rubyRun.right;

            if (!sameRun)
            {
                flushRubyRun();
                rubyRun = region;
                hasRubyRun = true;
                continue;
            }

            if (region.right > rubyRun.right)
                rubyRun.right = region.right;
        }
        flushRubyRun();
    }

    for (const auto &region : regions)
    {
        std::string payload = BuildASSPositionTag(caption, region.textX, region.textY);
        payload += fontTag;
        payload += captionAlphaTag;
        payload += outlineTag;
        payload += region.text;
        results.push_back({1, std::move(payload)});
    }

    // Fallback: if no regions produced output, use plain text representation.
    if (results.empty() && caption.text && caption.text[0] != '\0')
    {
        std::string plain;
        AppendASSEscapedText(plain, caption.text);
        results.push_back({1, std::move(plain)});
    }

    return results;
}

STDMETHODIMP CLAVFDemuxer::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
    CheckPointer(ppv, E_POINTER);

    *ppv = nullptr;

    return QI(IKeyFrameInfo) m_bEnableTrackInfo &&
           QI(ITrackInfo) QI2(IAMExtendedSeeking) QI2(IAMMediaContent) QI(IPropertyBag)
               QI(IDSMResourceBag) __super::NonDelegatingQueryInterface(riid, ppv);
}

/////////////////////////////////////////////////////////////////////////////
// Demuxer Functions
STDMETHODIMP CLAVFDemuxer::Open(LPCOLESTR pszFileName, LPCOLESTR pszUserAgent, LPCOLESTR pszReferrer)
{
    return OpenInputStream(nullptr, pszFileName, nullptr, TRUE, false, pszUserAgent, pszReferrer);
}

STDMETHODIMP CLAVFDemuxer::Start()
{
    if (m_bH264MVCCombine)
    {
        CMediaType *pmt = m_pSettings->GetOutputMediatype(m_nH264MVCBaseStream);
        if (pmt)
        {
            if (pmt->subtype != MEDIASUBTYPE_AMVC && pmt->subtype != MEDIASUBTYPE_MVC1)
            {
                DbgLog(
                    (LOG_TRACE, 10,
                     L"CLAVFDemuxer::Start(): Disabling MVC demuxing, downstream did not select an appropriate type"));
                m_bH264MVCCombine = FALSE;
                m_nH264MVCBaseStream = -1;
                m_nH264MVCExtensionStream = -1;
            }
        }
    }

    if (m_avFormat)
        av_read_play(m_avFormat);

    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::AbortOpening(int mode, int timeout)
{
    m_Abort = mode;
    m_timeAbort = timeout ? time(nullptr) + timeout : 0;
    return S_OK;
}

int CLAVFDemuxer::avio_interrupt_cb(void *opaque)
{
    CLAVFDemuxer *demux = (CLAVFDemuxer *)opaque;

    // Check for file opening timeout
    time_t now = time(nullptr);
    if (demux->m_timeOpening && now > (demux->m_timeOpening + AVFORMAT_OPEN_TIMEOUT))
        return 1;

    if (demux->m_Abort && now > demux->m_timeAbort)
        return 1;

    return 0;
}

static LPCWSTR wszImageExtensions[] = {
    L".png",  L".mng",  L".pns", // PNG
    L".tif",  L".tiff",          // TIFF
    L".jpeg", L".jpg",  L".jps", // JPEG
    L".tga",                     // TGA
    L".bmp",                     // BMP
    L".j2c",                     // JPEG2000
    L".webp",                    // WebP
};

static LPCWSTR wszBlockedExtensions[] = {L".ifo", L".bup"};

static std::pair<const char *, const char *> rtmpParametersTranslate[] = {
    std::make_pair("app", "rtmp_app"),
    std::make_pair("buffer", "rtmp_buffer"),
    std::make_pair("conn", "rtmp_conn"),
    std::make_pair("flashVer", "rtmp_flashver"),
    std::make_pair("rtmp_flush_interval", "rtmp_flush_interval"),
    std::make_pair("live", "rtmp_live"),
    std::make_pair("pageUrl", "rtmp_pageurl"),
    std::make_pair("playpath", "rtmp_playpath"),
    std::make_pair("subscribe", "rtmp_subscribe"),
    std::make_pair("swfHash", "rtmp_swfhash"),
    std::make_pair("swfSize", "rtmp_swfsize"),
    std::make_pair("swfUrl", "rtmp_swfurl"),
    std::make_pair("swfVfy", "rtmp_swfverify"),
    std::make_pair("tcUrl", "rtmp_tcurl")};

STDMETHODIMP CLAVFDemuxer::OpenInputStream(AVIOContext *byteContext, LPCOLESTR pszFileName, const char *format,
                                           BOOL bForce, BOOL bFileSource, LPCOLESTR pszUserAgent,
                                           LPCOLESTR pszReferrer)
{
    CAutoLock lock(m_pLock);
    HRESULT hr = S_OK;

    int ret; // return code from avformat functions

    // Convert the filename from wchar to char for avformat
    char *fileName = NULL;
    if (pszFileName)
        fileName = CoTaskGetMultiByteFromWideChar(CP_UTF8, 0, pszFileName, -1);

    if (fileName == NULL)
    {
        fileName = (char *)CoTaskMemAlloc(1);
        *fileName = 0;
    }

    // handle pipe, we only support stdin pipes
    if (_strnicmp("pipe://", fileName, 7) == 0)
    {
        // convert pipe://stdin to pipe:0
        fileName[5] = '0';
        fileName[6] = 0;
    }

    if (_strnicmp("mms:", fileName, 4) == 0)
    {
        memmove(fileName + 1, fileName, strlen(fileName));
        memcpy(fileName, "mmsh", 4);
    }

    // replace "icyx" protocol by http
    if (_strnicmp("icyx:", fileName, 5) == 0)
    {
        memcpy(fileName, "http", 4);
    }

    ARIB_LOG("[ARIB] LAVF OpenInputStream file=\"%s\" forcedFormat=%s bForce=%d bFileSource=%d customIO=%d\n",
             fileName ? fileName : "(null)",
             format ? format : "(auto)",
             bForce ? 1 : 0,
             bFileSource ? 1 : 0,
             byteContext ? 1 : 0);

    char *rtmp_prameters = nullptr;
    const char *rtsp_transport = nullptr;
    // check for rtsp transport protocol options
    if (_strnicmp("rtsp", fileName, 4) == 0)
    {
        if (_strnicmp("rtspu:", fileName, 6) == 0)
        {
            rtsp_transport = "udp";
        }
        else if (_strnicmp("rtspm:", fileName, 6) == 0)
        {
            rtsp_transport = "udp_multicast";
        }
        else if (_strnicmp("rtspt:", fileName, 6) == 0)
        {
            rtsp_transport = "tcp";
        }
        else if (_strnicmp("rtsph:", fileName, 6) == 0)
        {
            rtsp_transport = "http";
        }

        // replace "rtsp[u|m|t|h]" protocol by rtsp
        if (rtsp_transport != nullptr)
        {
            memmove(fileName + 4, fileName + 5, strlen(fileName) - 4);
        }
    }
    else if (_strnicmp("rtmp", fileName, 4) == 0)
    {
        rtmp_prameters = strchr(fileName, ' ');
        if (rtmp_prameters)
        {
            *rtmp_prameters = '\0'; // Trim not supported part form fileName
        }
    }

    AVIOInterruptCB cb = {avio_interrupt_cb, this};

trynoformat:
    // Create the avformat_context
    m_avFormat = avformat_alloc_context();
    m_avFormat->pb = byteContext;
    m_avFormat->interrupt_callback = cb;

    if (m_avFormat->pb)
        m_avFormat->flags |= AVFMT_FLAG_CUSTOM_IO;

    LPWSTR extension = pszFileName ? PathFindExtensionW(pszFileName) : nullptr;

    const AVInputFormat *inputFormat = nullptr;
    if (format)
    {
        inputFormat = av_find_input_format(format);
    }
    else if (pszFileName)
    {
        LPWSTR extension = PathFindExtensionW(pszFileName);
        for (int i = 0; i < countof(wszImageExtensions); i++)
        {
            if (_wcsicmp(extension, wszImageExtensions[i]) == 0)
            {
                if (byteContext)
                {
                    inputFormat = av_find_input_format("image2pipe");
                }
                else
                {
                    inputFormat = av_find_input_format("image2");
                }
                break;
            }
        }

        if (byteContext == nullptr || bFileSource)
        {
            for (int i = 0; i < countof(wszBlockedExtensions); i++)
            {
                if (_wcsicmp(extension, wszBlockedExtensions[i]) == 0)
                {
                    goto done;
                }
            }
        }
    }

    // Disable loading of external mkv segments, if required
    if (!m_pSettings->GetLoadMatroskaExternalSegments())
        m_avFormat->flags |= AVFMT_FLAG_NOEXTERNAL;

    // demuxer/protocol options
    AVDictionary *options = nullptr;
    av_dict_set(&options, "icy", "1", 0);               // request ICY metadata
    av_dict_set(&options, "advanced_editlist", "0", 0); // disable broken mov editlist handling
    av_dict_set(&options, "reconnect", "1", 0);         // for http, reconnect if we get disconnected
    av_dict_set(&options, "skip_clear", "1", 0);        // mpegts program handling
    av_dict_set(&options, "max_reload", "7", 0);        // playlist reloading for HLS
    av_dict_set(&options, "extension_picky", "0", 0);   // less strict HLS parsing

    if (pszUserAgent)
    {
        char *strUserAgent = CoTaskGetMultiByteFromWideChar(CP_UTF8, 0, pszUserAgent, -1);
        if (strUserAgent && *strUserAgent) // if valid, and non-empty
            av_dict_set(&options, "user_agent", strUserAgent, 0);

        SAFE_CO_FREE(strUserAgent);
    }

    if (pszReferrer != NULL)
    {
        char *strReferrer = CoTaskGetMultiByteFromWideChar(CP_UTF8, 0, pszReferrer, -1);
        if (strReferrer && *strReferrer) // if valid, and non-empty
            av_dict_set(&options, "referer", strReferrer, 0);

        SAFE_CO_FREE(strReferrer);
    }
    else
    {
        av_dict_set(&options, "referer", fileName, 0); // for http, send self as referer if none was specified explicitly
    }

    if (rtsp_transport != nullptr)
    {
        av_dict_set(&options, "rtsp_transport", rtsp_transport, 0);
    }

    if (rtmp_prameters != nullptr)
    {
        char buff[4100];
        char *next_token = nullptr;
        bool bSwfVerify = false;

        strcpy_s(buff, rtmp_prameters + 1);
        const char *token = strtok_s(buff, " ", &next_token);
        while (token)
        {
            for (size_t i = 0; i < _countof(rtmpParametersTranslate); i++)
            {
                const size_t len = strlen(rtmpParametersTranslate[i].first);
                if (_strnicmp(token, rtmpParametersTranslate[i].first, len) == 0)
                {
                    if (strlen(token) > len + 1 && token[len] == '=')
                    {
                        if (_strnicmp("swfVfy", rtmpParametersTranslate[i].first, len) == 0)
                        {
                            bSwfVerify = token[len + 1] == '1';
                            continue;
                        }
                        else if (_strnicmp("live", rtmpParametersTranslate[i].first, len) == 0)
                        {
                            if (token[len + 1] == '1')
                            {
                                av_dict_set(&options, rtmpParametersTranslate[i].second, "live", 0);
                            }
                            else if (token[len + 1] == '0')
                            {
                                av_dict_set(&options, rtmpParametersTranslate[i].second, "recorded", 0);
                            }
                            continue;
                        }
                        av_dict_set(&options, rtmpParametersTranslate[i].second, token + len + 1, 0);
                    }
                }
            }
            token = strtok_s(nullptr, " ", &next_token);
        }

        if (bSwfVerify)
        {
            const AVDictionaryEntry *swfUrlEntry = av_dict_get(options, "rtmp_swfurl", nullptr, 0);
            if (swfUrlEntry)
            {
                av_dict_set(&options, "rtmp_swfverify", swfUrlEntry->value, 0);
            }
        }
    }

    m_timeOpening = time(nullptr);
    ret = avformat_open_input(&m_avFormat, fileName, inputFormat, &options);
    av_dict_free(&options);
    if (ret < 0)
    {
        DbgLog((LOG_ERROR, 0, TEXT("::OpenInputStream(): avformat_open_input failed (%d)"), ret));
        ARIB_LOG("[ARIB] avformat_open_input failed ret=%d file=\"%s\" forcedFormat=%s\n",
                 ret, fileName ? fileName : "(null)", format ? format : "(auto)");
        if (format)
        {
            DbgLog((LOG_ERROR, 0, TEXT(" -> trying again without specific format")));
            format = nullptr;
            avformat_close_input(&m_avFormat);
            goto trynoformat;
        }
        goto done;
    }
    DbgLog((LOG_TRACE, 10,
            TEXT("::OpenInputStream(): avformat_open_input opened file of type '%S' (took %I64d seconds)"),
            m_avFormat->iformat->name, time(nullptr) - m_timeOpening));
    ARIB_LOG("[ARIB] avformat_open_input ok format=%s streams=%u programs=%u took=%llds\n",
             m_avFormat->iformat ? m_avFormat->iformat->name : "(null)",
             m_avFormat->nb_streams,
             m_avFormat->nb_programs,
             (long long)(time(nullptr) - m_timeOpening));
    m_timeOpening = 0;

    CHECK_HR(hr = InitAVFormat(pszFileName, bForce));

    SAFE_CO_FREE(fileName);
    return S_OK;
done:
    CleanupAVFormat();
    SAFE_CO_FREE(fileName);
    return E_FAIL;
}

void CLAVFDemuxer::AddMPEGTSStream(int pid, uint32_t stream_type)
{
    if (m_avFormat)
    {
        int program = -1;
        if (m_avFormat->nb_programs > 0)
        {
            unsigned nb_streams = 0;
            for (unsigned i = 0; i < m_avFormat->nb_programs; i++)
            {
                if (m_avFormat->programs[i]->nb_stream_indexes > nb_streams)
                    program = i;
            }
        }
        avpriv_mpegts_add_stream(m_avFormat, pid, stream_type, program >= 0 ? m_avFormat->programs[program]->id : -1);
    }
}

HRESULT CLAVFDemuxer::CheckBDM2TSCPLI(LPCOLESTR pszFileName)
{
    size_t len = wcslen(pszFileName);

    if (len <= 23 || (_wcsnicmp(pszFileName + len - 23, L"\\BDMV\\STREAM\\", 13) != 0 &&
                      (len <= 28 || _wcsnicmp(pszFileName + len - 28, L"\\BDMV\\STREAM\\SSIF\\", 18) != 0)))
        return E_FAIL;

    // Get the base file name (should be a number, like 00000)
    const WCHAR *file = pszFileName + (len - 10);
    WCHAR basename[6];
    wcsncpy_s(basename, file, 5);
    basename[5] = 0;

    // Convert to UTF-8 path
    size_t a_len = WideCharToMultiByte(CP_UTF8, 0, pszFileName, -1, nullptr, 0, nullptr, nullptr);
    a_len += 2; // one extra char because CLIPINF is 7 chars and STREAM is 6, and one for the terminating-zero

    char *path = (char *)CoTaskMemAlloc(a_len * sizeof(char));
    if (!path)
        return E_OUTOFMEMORY;
    WideCharToMultiByte(CP_UTF8, 0, pszFileName, -1, path, (int)a_len, nullptr, nullptr);

    // Remove file name itself
    PathRemoveFileSpecA(path);

    // Remove SSIF if appropriate
    BOOL bSSIF = FALSE;
    if (_strnicmp(path + strlen(path) - 5, "\\SSIF", 5) == 0)
    {
        bSSIF = TRUE;
        PathRemoveFileSpecA(path);
    }

    // Remove STREAM folder
    PathRemoveFileSpecA(path);

    // Write new path
    sprintf_s(path + strlen(path), a_len - strlen(path), "\\CLIPINF\\%S.clpi", basename);

    CLPI_CL *cl = bd_read_clpi(path);
    if (!cl)
        return E_FAIL;

    // Clip Info was found, add the language metadata to the AVStreams
    for (unsigned i = 0; i < cl->program.num_prog; ++i)
    {
        CLPI_PROG *p = &cl->program.progs[i];
        for (unsigned k = 0; k < p->num_streams; ++k)
        {
            CLPI_PROG_STREAM *s = &p->streams[k];
            AVStream *avstream = GetAVStreamByPID(s->pid);
            if (avstream)
            {
                if (s->lang[0] != 0)
                    av_dict_set(&avstream->metadata, "language", (const char *)s->lang, 0);
            }
        }
    }
    // Free the clip
    bd_free_clpi(cl);
    cl = nullptr;

    if (bSSIF)
    {
        uint32_t clip_id = _wtoi(basename);

        // Remove filename
        PathRemoveFileSpecA(path);

        // Remove CLIPINF
        PathRemoveFileSpecA(path);

        // Remove BDMV
        PathRemoveFileSpecA(path);

        BLURAY *bd = bd_open(path, nullptr);
        if (!bd)
            return S_FALSE;

        uint32_t nTitles = bd_get_titles(bd, TITLES_RELEVANT, 0);
        BOOL found = FALSE;
        for (uint32_t n = 0; n < nTitles && !found; n++)
        {
            BLURAY_TITLE_INFO *TitleInfo = bd_get_title_info(bd, n, 0);
            if (TitleInfo)
            {
                for (uint32_t i = 0; i < TitleInfo->clip_count; i++)
                {
                    BLURAY_CLIP_INFO *Clip = &TitleInfo->clips[i];
                    if (Clip->idx == clip_id)
                    {
                        AVStream *avstream = nullptr;
                        for (uint8_t c = 0; c < Clip->video_stream_count && !avstream; c++)
                        {
                            if (Clip->video_streams[c].coding_type == BLURAY_STREAM_TYPE_VIDEO_H264)
                                avstream = GetAVStreamByPID(Clip->video_streams[c].pid);
                        }

                        if (avstream)
                            av_dict_set(&avstream->metadata, "stereo_mode",
                                        TitleInfo->mvc_base_view_r_flag ? "mvc_rl" : "mvc_lr", 0);

                        found = TRUE;
                        break;
                    }
                }

                bd_free_title_info(TitleInfo);
            }
        }

        bd_close(bd);
    }

    return S_OK;
}

inline static int init_parser(AVFormatContext *s, AVStream *st)
{
    if (av_lav_stream_parser_get_needed(st) && !(s->flags & AVFMT_FLAG_NOPARSE))
    {
        av_lav_stream_parser_init(st);
    }
    return 0;
}

void CLAVFDemuxer::UpdateParserFlags(AVStream *st)
{
    int flags = av_lav_stream_parser_get_flags(st);
    if ((st->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO || st->codecpar->codec_id == AV_CODEC_ID_MPEG1VIDEO) &&
        _stricmp(m_pszInputFormat, "mpegvideo") != 0)
    {
        flags |= PARSER_FLAG_NO_TIMESTAMP_MANGLING;
    }
    else if (st->codecpar->codec_id == AV_CODEC_ID_H264)
    {
        flags |= PARSER_FLAG_NO_TIMESTAMP_MANGLING;
    }
    else if (st->codecpar->codec_id == AV_CODEC_ID_VC1)
    {
        if (m_bVC1Correction)
        {
            flags &= ~PARSER_FLAG_NO_TIMESTAMP_MANGLING;
        }
        else
        {
            flags |= PARSER_FLAG_NO_TIMESTAMP_MANGLING;
        }
    }
    av_lav_stream_parser_update_flags(st, flags);
}

static struct sCoverMimeTypes
{
    AVCodecID codec;
    LPCWSTR mime;
    LPCWSTR ext;
} CoverMimeTypes[] = {
    {AV_CODEC_ID_MJPEG, L"image/jpeg", L".jpg"}, {AV_CODEC_ID_PNG, L"image/png", L".png"},
    {AV_CODEC_ID_GIF, L"image/gif", L".gif"},    {AV_CODEC_ID_BMP, L"image/bmp", L".bmp"},
    {AV_CODEC_ID_TIFF, L"image/tiff", L".tiff"},
};

STDMETHODIMP CLAVFDemuxer::InitAVFormat(LPCOLESTR pszFileName, BOOL bForce)
{
    HRESULT hr = S_OK;

    const char *format = nullptr;
    lavf_get_iformat_infos(m_avFormat->iformat, &format, nullptr);
    if (!bForce && (!format || !m_pSettings->IsFormatEnabled(format)))
    {
        DbgLog((LOG_TRACE, 20, L"::InitAVFormat() - format of type '%S' disabled, failing",
                format ? format : m_avFormat->iformat->name));
        return E_FAIL;
    }

    m_pszInputFormat = format ? format : m_avFormat->iformat->name;

    m_bVC1SeenTimestamp = FALSE;

    LPWSTR extension = pszFileName ? PathFindExtensionW(pszFileName) : nullptr;

    m_bMatroska = (_strnicmp(m_pszInputFormat, "matroska", 8) == 0);
    m_bOgg = (_strnicmp(m_pszInputFormat, "ogg", 3) == 0);
    m_bAVI = (_strnicmp(m_pszInputFormat, "avi", 3) == 0);
    m_bMPEGTS = (_strnicmp(m_pszInputFormat, "mpegts", 6) == 0);
    m_bMPEGPS = (_stricmp(m_pszInputFormat, "mpeg") == 0);
    m_bRM = (_stricmp(m_pszInputFormat, "rm") == 0);
    m_bPMP = (_stricmp(m_pszInputFormat, "pmp") == 0);
    m_bMP4 = (_stricmp(m_pszInputFormat, "mp4") == 0);

    m_bTSDiscont = (m_avFormat->iformat->flags & AVFMT_TS_DISCONT) || m_bRM || (_stricmp(m_pszInputFormat, "dash") == 0);

    WCHAR szProt[24] = L"file";
    if (pszFileName)
    {
        DWORD dwNumChars = 24;
        hr = UrlGetPart(pszFileName, szProt, &dwNumChars, URL_PART_SCHEME, 0);
        if (SUCCEEDED(hr) && dwNumChars && (_wcsicmp(szProt, L"file") != 0))
        {
            m_avFormat->flags |= AVFMT_FLAG_NETWORK;
            DbgLog((LOG_TRACE, 10, TEXT("::InitAVFormat(): detected network protocol: %s"), szProt));
        }
    }

    // TODO: make both durations below configurable
    // decrease analyze duration for network streams
    if (m_avFormat->flags & AVFMT_FLAG_NETWORK ||
        (m_avFormat->flags & AVFMT_FLAG_CUSTOM_IO && !m_avFormat->pb->seekable))
    {
        // require at least 0.2 seconds
        av_opt_set_int(m_avFormat, "analyzeduration",
                       max(m_pSettings->GetNetworkStreamAnalysisDuration() * 1000, 200000), 0);
    }
    else
    {
        av_opt_set_int(m_avFormat, "analyzeduration", 7500000, 0);
        // And increase it for mpeg-ts/ps files
        if (m_bMPEGTS || m_bMPEGPS)
        {
            av_opt_set_int(m_avFormat, "analyzeduration", 30000000, 0);
            av_opt_set_int(m_avFormat, "probesize", 75000000, 0);
        }
    }

    av_opt_set_int(m_avFormat, "correct_ts_overflow", !m_pBluRay, 0);

    m_timeOpening = time(nullptr);
    int ret = avformat_find_stream_info(m_avFormat, nullptr);
    if (ret < 0)
    {
        DbgLog((LOG_ERROR, 0, TEXT("::InitAVFormat(): av_find_stream_info failed (%d)"), ret));
        goto done;
    }
    DbgLog((LOG_TRACE, 10, TEXT("::InitAVFormat(): avformat_find_stream_info finished, took %I64d seconds"),
            time(nullptr) - m_timeOpening));
    m_timeOpening = 0;

    // Check if this is a m2ts in a BD structure, and if it is, read some extra stream properties out of the CLPI files
    if (m_pBluRay)
    {
        m_pBluRay->ProcessBluRayMetadata();
    }
    else if (pszFileName && m_bMPEGTS)
    {
        CheckBDM2TSCPLI(pszFileName);
    }

    char *icy_headers = nullptr;
    if (av_opt_get(m_avFormat, "icy_metadata_headers", AV_OPT_SEARCH_CHILDREN, (uint8_t **)&icy_headers) >= 0 &&
        icy_headers && strlen(icy_headers) > 0)
    {
        std::string icyHeaders(icy_headers);
        std::stringstream icyHeaderStream(icyHeaders);

        std::string line;
        while (std::getline(icyHeaderStream, line))
        {
            size_t seperatorIdx = line.find_first_of(":");
            std::string token = line.substr(0, seperatorIdx);
            std::string value = line.substr(seperatorIdx + 1);
            if (_stricmp(token.c_str(), "icy-name") == 0)
            {
                // not entirely correct, but this way it gets exported through IAMMediaContent
                av_dict_set(&m_avFormat->metadata, "artist", value.c_str(), 0);
            }
            else if (_stricmp(token.c_str(), "icy-description") == 0)
            {
                av_dict_set(&m_avFormat->metadata, "comment", value.c_str(), 0);
            }
            else if (_stricmp(token.c_str(), "icy-genre") == 0)
            {
                av_dict_set(&m_avFormat->metadata, "genre", value.c_str(), 0);
            }
        }

        ParseICYMetadataPacket();
    }
    av_freep(&icy_headers);

    SAFE_CO_FREE(m_stOrigParser);
    m_stOrigParser = (enum AVStreamParseType *)CoTaskMemAlloc(m_avFormat->nb_streams * sizeof(enum AVStreamParseType));
    if (!m_stOrigParser)
        return E_OUTOFMEMORY;

    for (unsigned int idx = 0; idx < m_avFormat->nb_streams; ++idx)
    {
        AVStream *st = m_avFormat->streams[idx];
        AVProgram *streamProgForLog = av_find_program_from_stream(m_avFormat, nullptr, idx);
        ARIB_LOG("[ARIB] stream info idx=%u id=%d type=%d codec_id=%d codec=%s profile=%d disp=0x%x program=%d time_base=%d/%d\n",
                 idx,
                 st->id,
                 st->codecpar->codec_type,
                 (int)st->codecpar->codec_id,
                 avcodec_get_name(st->codecpar->codec_id),
                 st->codecpar->profile,
                 st->disposition,
                 streamProgForLog ? streamProgForLog->pmt_pid : -1,
                 st->time_base.num,
                 st->time_base.den);

        // Disable full stream parsing for these formats
        if (av_lav_stream_parser_get_needed(st) == AVSTREAM_PARSE_FULL)
        {
            if (st->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE)
            {
                av_lav_stream_parser_set_needed(st, AVSTREAM_PARSE_NONE);
            }
        }

        if (m_bOgg && st->codecpar->codec_id == AV_CODEC_ID_H264)
        {
            av_lav_stream_parser_set_needed(st, AVSTREAM_PARSE_FULL);
        }

        // Create the parsers with the appropriate flags
        init_parser(m_avFormat, st);
        UpdateParserFlags(st);

#ifdef DEBUG
        DbgLog((LOG_TRACE, 30, L"Stream %d (pid %d) - program: %d, codec: %S; parsing: %S;", idx, st->id,
                streamProgForLog ? streamProgForLog->pmt_pid : -1, avcodec_get_name(st->codecpar->codec_id),
                lavf_get_parsing_string(av_lav_stream_parser_get_needed(st))));
#endif
        m_stOrigParser[idx] = av_lav_stream_parser_get_needed(st);

        if ((st->codecpar->codec_id == AV_CODEC_ID_DTS && st->codecpar->codec_tag == 0xA2) ||
            (st->codecpar->codec_id == AV_CODEC_ID_EAC3 && st->codecpar->codec_tag == 0xA1))
            st->disposition |= LAVF_DISPOSITION_SECONDARY_AUDIO;

        UpdateSubStreams();

        if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT)
        {
            const AVDictionaryEntry *attachFilename = av_dict_get(st->metadata, "filename", nullptr, 0);
            const AVDictionaryEntry *attachMimeType = av_dict_get(st->metadata, "mimetype", nullptr, 0);
            const AVDictionaryEntry *attachDescription = av_dict_get(st->metadata, "comment", nullptr, 0);

            if (attachFilename && attachMimeType)
            {
                LPWSTR chFilename =
                    CoTaskGetWideCharFromMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, attachFilename->value, -1);
                LPWSTR chMimetype =
                    CoTaskGetWideCharFromMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, attachMimeType->value, -1);
                LPWSTR chDescription = nullptr;

                if (attachDescription)
                    chDescription =
                        CoTaskGetWideCharFromMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, attachDescription->value, -1);

                if (chFilename && chMimetype)
                    ResAppend(chFilename, chDescription, chMimetype, st->codecpar->extradata,
                              (DWORD)st->codecpar->extradata_size);

                SAFE_CO_FREE(chFilename);
                SAFE_CO_FREE(chMimetype);
                SAFE_CO_FREE(chDescription);
            }
            else
            {
                DbgLog((LOG_TRACE, 10, L" -> Unknown attachment, missing filename or mimetype"));
            }

            // Try to guess the codec id for fonts only listed by name
            if (st->codecpar->codec_id == AV_CODEC_ID_NONE && attachFilename)
            {
                char *dot = strrchr(attachFilename->value, '.');
                if (dot && !_stricmp(dot, ".ttf"))
                    st->codecpar->codec_id = AV_CODEC_ID_TTF;
                else if (dot && !_stricmp(dot, ".otf"))
                    st->codecpar->codec_id = AV_CODEC_ID_OTF;
            }

            if (st->codecpar->codec_id == AV_CODEC_ID_TTF || st->codecpar->codec_id == AV_CODEC_ID_OTF)
            {
                if (!m_pFontInstaller)
                {
                    m_pFontInstaller = new CFontInstaller();
                }
                m_pFontInstaller->InstallFont(st->codecpar->extradata, st->codecpar->extradata_size);
            }
        }
        else if (st->disposition & AV_DISPOSITION_ATTACHED_PIC && st->attached_pic.data && st->attached_pic.size > 0)
        {
            LPWSTR chFilename = nullptr;
            LPWSTR chMimeType = nullptr;
            LPWSTR chDescription = nullptr;

            // gather a filename
            const AVDictionaryEntry *attachFilename = av_dict_get(st->metadata, "filename", nullptr, 0);
            if (attachFilename)
                chFilename = CoTaskGetWideCharFromMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, attachFilename->value, -1);

            // gather a mimetype
            const AVDictionaryEntry *attachMimeType = av_dict_get(st->metadata, "mimetype", nullptr, 0);
            if (attachMimeType)
                chMimeType = CoTaskGetWideCharFromMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, attachMimeType->value, -1);

            // gather description
            const AVDictionaryEntry *attachDescription = av_dict_get(st->metadata, "comment", nullptr, 0);
            if (attachDescription)
                chDescription =
                    CoTaskGetWideCharFromMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, attachDescription->value, -1);

            for (int c = 0; c < countof(CoverMimeTypes); c++)
            {
                if (CoverMimeTypes[c].codec == st->codecpar->codec_id)
                {
                    if (chFilename == nullptr)
                    {
                        size_t size = wcslen(CoverMimeTypes[c].ext) + 15;
                        chFilename = (LPWSTR)CoTaskMemAlloc(size * sizeof(wchar_t));
                        wcscpy_s(chFilename, size, L"EmbeddedCover");
                        wcscat_s(chFilename, size, CoverMimeTypes[c].ext);
                    }
                    if (chMimeType == nullptr)
                    {
                        size_t size = wcslen(CoverMimeTypes[c].mime) + 1;
                        chMimeType = (LPWSTR)CoTaskMemAlloc(size * sizeof(wchar_t));
                        wcscpy_s(chMimeType, size, CoverMimeTypes[c].mime);
                    }
                    break;
                }
            }

            // Export embedded cover-art through IDSMResourceBag interface
            if (chFilename && chMimeType)
            {
                ResAppend(chFilename, chDescription, chMimeType, st->attached_pic.data, (DWORD)st->attached_pic.size);
            }
            else
            {
                DbgLog((LOG_TRACE, 10, L" -> Unknown attachment, missing filename or mimetype"));
            }

            SAFE_CO_FREE(chFilename);
            SAFE_CO_FREE(chMimeType);
            SAFE_CO_FREE(chDescription);
        }
    }

    if (AVDictionaryEntry *cue = av_dict_get(m_avFormat->metadata, "CUESHEET", nullptr, 0))
    {
        CCueSheet cueSheet;
        if (SUCCEEDED(cueSheet.Parse(cue->value)))
        {
            // Metadata
            if (!cueSheet.m_Title.empty() && !av_dict_get(m_avFormat->metadata, "title", nullptr, 0))
                av_dict_set(&m_avFormat->metadata, "title", cueSheet.m_Title.c_str(), 0);
            if (!cueSheet.m_Performer.empty() && !av_dict_get(m_avFormat->metadata, "artist", nullptr, 0))
                av_dict_set(&m_avFormat->metadata, "artist", cueSheet.m_Performer.c_str(), 0);
            // Free old chapters
            while (m_avFormat->nb_chapters--)
            {
                av_dict_free(&m_avFormat->chapters[m_avFormat->nb_chapters]->metadata);
                av_freep(&m_avFormat->chapters[m_avFormat->nb_chapters]);
            }
            av_freep(&m_avFormat->chapters);
            m_avFormat->nb_chapters = 0;
            for (CCueSheet::Track track : cueSheet.m_Tracks)
            {
                avpriv_new_chapter(m_avFormat, track.index, AVRational{1, DSHOW_TIME_BASE}, track.Time, track.Time,
                                   cueSheet.FormatTrack(track).c_str());
            }
        }
    }

    CHECK_HR(hr = CreateStreams());

    return S_OK;
done:
    CleanupAVFormat();
    return E_FAIL;
}

void CLAVFDemuxer::CleanupAVFormat()
{
    FlushMVCExtensionQueue();
    if (m_avFormat)
    {
        // Override abort timer to ensure the close function in network protocols can actually close the stream
        AbortOpening(1, 5);
        avformat_close_input(&m_avFormat);
    }
    SAFE_CO_FREE(m_stOrigParser);
}

AVStream *CLAVFDemuxer::GetAVStreamByPID(int pid)
{
    if (!m_avFormat)
        return nullptr;

    for (unsigned int idx = 0; idx < m_avFormat->nb_streams; ++idx)
    {
        if (m_avFormat->streams[idx]->id == pid &&
            !(m_avFormat->streams[idx]->disposition & LAVF_DISPOSITION_SUB_STREAM))
            return m_avFormat->streams[idx];
    }

    return nullptr;
}

HRESULT CLAVFDemuxer::SetActiveStream(StreamType type, int pid)
{
    HRESULT hr = S_OK;

    if (type == audio)
        UpdateForcedSubtitleStream(pid);
    else if (type == subpic)
    {
        // Anything still queued belongs to the previous selection.
        if (pid != m_dActiveStreams[subpic])
            FlushAribPendingPackets();
        if (pid != (int)LATE_ARIB_SUBTITLE_PID)
            m_LateAribSubtitleStream = -1;
            m_LateAribSubtitleIsSuperimpose = false;
    }

    hr = __super::SetActiveStream(type, pid);

    // Usually selecting an audio stream would set the forced substream (since it uses the audio stream language)
    // but in case there is no audio stream, do a fallback selection of any PGS stream here.
    if (type == subpic && pid == FORCED_SUBTITLE_PID && m_ForcedSubStream == -1)
    {
        std::list<CSubtitleSelector> selectors;
        CSubtitleSelector selector;
        selector.audioLanguage = "*";
        selector.subtitleLanguage = "*";
        selector.dwFlagsSet = SUBTITLE_FLAG_PGS;
        selector.dwFlagsNot = 0;
        selectors.push_back(selector);

        const stream *subst = SelectSubtitleStream(selectors, "");
        if (subst)
            m_ForcedSubStream = subst->pid;
    }

    for (unsigned int idx = 0; idx < m_avFormat->nb_streams; ++idx)
    {
        AVStream *st = m_avFormat->streams[idx];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            st->discard = (m_dActiveStreams[video] == idx) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;

            // don't discard h264 mvc streams
            if (m_bH264MVCCombine && st->codecpar->codec_id == AV_CODEC_ID_H264_MVC)
                st->discard = AVDISCARD_DEFAULT;
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            st->discard = (m_dActiveStreams[audio] == idx) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
            // If the stream is a sub stream, make sure to activate the main stream as well
            if (m_bMPEGTS && (st->disposition & LAVF_DISPOSITION_SUB_STREAM) && st->discard == AVDISCARD_DEFAULT)
            {
                for (unsigned int idx2 = 0; idx2 < m_avFormat->nb_streams; ++idx2)
                {
                    AVStream *mst = m_avFormat->streams[idx2];
                    if (mst->id == st->id)
                    {
                        mst->discard = AVDISCARD_DEFAULT;
                        break;
                    }
                }
            }
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
            st->discard = (m_dActiveStreams[subpic] == idx ||
                           IsLateAribCandidateStream(idx) ||
                           (m_dActiveStreams[subpic] == FORCED_SUBTITLE_PID && m_ForcedSubStream == idx))
                              ? AVDISCARD_DEFAULT
                              : AVDISCARD_ALL;
        }
        else
        {
            st->discard = AVDISCARD_ALL;
        }
    }

    return hr;
}

void CLAVFDemuxer::UpdateSubStreams()
{
    for (unsigned int idx = 0; idx < m_avFormat->nb_streams; ++idx)
    {
        AVStream *st = m_avFormat->streams[idx];
        // Find and flag the AC-3 substream
        if (m_bMPEGTS && st->codecpar->codec_id == AV_CODEC_ID_TRUEHD)
        {
            int id = st->id;
            AVStream *sub_st = nullptr;

            for (unsigned int i = 0; i < m_avFormat->nb_streams; ++i)
            {
                AVStream *sst = m_avFormat->streams[i];
                if (idx != i && sst->id == id)
                {
                    sub_st = sst;
                    break;
                }
            }
            if (sub_st)
            {
                sub_st->disposition = st->disposition | LAVF_DISPOSITION_SUB_STREAM;
                av_dict_copy(&sub_st->metadata, st->metadata, 0);
            }
        }
    }
}

STDMETHODIMP CLAVFDemuxer::SetTitle(int idx)
{
    if (!m_bMatroska)
        return E_NOTIMPL;
    av_mkv_set_next_edition(m_avFormat, idx);

    // Update duration
    AVEdition *editions = nullptr;
    av_mkv_get_editions(m_avFormat, &editions);
    m_avFormat->duration = editions[idx].duration;

    return S_OK;
}

STDMETHODIMP_(int) CLAVFDemuxer::GetTitle()
{
    if (!m_bMatroska)
        return 0;
    return av_mkv_get_edition(m_avFormat);
}

STDMETHODIMP CLAVFDemuxer::GetTitleInfo(int idx, REFERENCE_TIME *rtDuration, WCHAR **ppszName)
{
    if (!m_bMatroska)
        return E_NOTIMPL;

    AVEdition *editions = nullptr;
    av_mkv_get_editions(m_avFormat, &editions);

    AVEdition *current_edition = &editions[idx];

    if (rtDuration)
        *rtDuration = av_rescale(current_edition->duration, DSHOW_TIME_BASE, AV_TIME_BASE);
    if (ppszName)
    {
        char *title = nullptr;
        int total_seconds = (int)(current_edition->duration / AV_TIME_BASE);
        int seconds = total_seconds % 60;
        int minutes = total_seconds / 60 % 60;
        int hours = total_seconds / 3600;
        if (current_edition->title)
        {
            title = av_asprintf("E: %s [%02d:%02d:%02d]", current_edition->title, hours, minutes, seconds);
        }
        else
        {
            title = av_asprintf("E: Edition %d [%02d:%02d:%02d]", idx + 1, hours, minutes, seconds);
        }
        *ppszName = CoTaskGetWideCharFromMultiByte(CP_UTF8, MB_ERR_INVALID_CHARS, title, -1);
        av_freep(&title);
    }
    return S_OK;
}

STDMETHODIMP_(int) CLAVFDemuxer::GetNumTitles()
{
    if (!m_bMatroska || !m_avFormat || !m_avFormat->priv_data || !m_avFormat->iformat || strcmp(m_avFormat->iformat->name, "matroska") != 0)
        return 0;
    return av_mkv_get_num_editions(m_avFormat);
}

void CLAVFDemuxer::SettingsChanged(ILAVFSettingsInternal *pSettings)
{
    int vc1Mode = pSettings->GetVC1TimestampMode();
    if (vc1Mode == 1 || strcmp(m_pszInputFormat, "rawvideo") == 0)
    {
        m_bVC1Correction = true;
    }
    else if (vc1Mode == 2)
    {
        BOOL bReq = pSettings->IsVC1CorrectionRequired();
        m_bVC1Correction = m_bMatroska ? !bReq : bReq;
    }
    else
    {
        m_bVC1Correction = false;
    }

    for (unsigned int idx = 0; idx < m_avFormat->nb_streams; ++idx)
    {
        AVStream *st = m_avFormat->streams[idx];
        if (st->codecpar->codec_id == AV_CODEC_ID_VC1)
        {
            UpdateParserFlags(st);
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            av_lav_stream_parser_set_needed(st, m_stOrigParser[idx]);
        }
    }

    m_bPGSNoParsing = !pSettings->GetPGSOnlyForced();
}

REFERENCE_TIME CLAVFDemuxer::GetDuration() const
{
    int64_t iLength = 0;
    if (m_avFormat->duration == (int64_t)AV_NOPTS_VALUE || m_avFormat->duration < 0LL)
    {
        // no duration is available for us
        // try to calculate it
        // TODO
        /*if (m_rtCurrent != Packet::INVALID_TIME && m_avFormat->file_size > 0 && m_avFormat->pb && m_avFormat->pb->pos
        > 0) { iLength = (((m_rtCurrent * m_avFormat->file_size) / m_avFormat->pb->pos) / 1000) & 0xFFFFFFFF;
        }*/
        // DbgLog((LOG_ERROR, 1, TEXT("duration is not available")));
        return -1;
    }
    else
    {
        iLength = m_avFormat->duration;
    }
    return ConvertTimestampToRT(iLength, 1, AV_TIME_BASE, 0);
}

#define VC1_CODE_RES0 0x00000100
#define IS_VC1_MARKER(x) (((x) & ~0xFF) == VC1_CODE_RES0)

STDMETHODIMP CLAVFDemuxer::CreatePacketMediaType(Packet *pPacket, enum AVCodecID codec_id, BYTE *extradata,
                                                 int extradata_size, BYTE *paramchange, int paramchange_size)
{
    CMediaType *pmt = m_pSettings->GetOutputMediatype(pPacket->StreamId);
    if (pmt)
    {
        if (extradata && extradata_size)
        {
            if (codec_id == AV_CODEC_ID_H264)
            {
                MPEG2VIDEOINFO *mp2vi =
                    (MPEG2VIDEOINFO *)pmt->ReallocFormatBuffer(sizeof(MPEG2VIDEOINFO) + extradata_size);
                int ret = g_VideoHelper.ProcessH264Extradata(extradata, extradata_size, mp2vi, FALSE);
                if (ret < 0)
                {
                    mp2vi->cbSequenceHeader = extradata_size;
                    memcpy(&mp2vi->dwSequenceHeader[0], extradata, extradata_size);
                }
                else
                {
                    int mp2visize = SIZE_MPEG2VIDEOINFO(mp2vi);
                    memset((BYTE *)mp2vi + mp2visize, 0, pmt->cbFormat - mp2visize);
                }
            }
            else if (codec_id == AV_CODEC_ID_MPEG2VIDEO)
            {
                MPEG2VIDEOINFO *mp2vi =
                    (MPEG2VIDEOINFO *)pmt->ReallocFormatBuffer(sizeof(MPEG2VIDEOINFO) + extradata_size);
                CExtradataParser parser = CExtradataParser(extradata, extradata_size);
                mp2vi->cbSequenceHeader = (DWORD)parser.ParseMPEGSequenceHeader((BYTE *)&mp2vi->dwSequenceHeader[0]);
            }
            else if (codec_id == AV_CODEC_ID_VC1)
            {
                VIDEOINFOHEADER2 *vih2 =
                    (VIDEOINFOHEADER2 *)pmt->ReallocFormatBuffer(sizeof(VIDEOINFOHEADER2) + extradata_size + 1);
                int i = 0;
                for (i = 0; i < (extradata_size - 4); i++)
                {
                    uint32_t code = AV_RB32(extradata + i);
                    if (IS_VC1_MARKER(code))
                        break;
                }
                if (i == 0)
                {
                    *((BYTE *)vih2 + sizeof(VIDEOINFOHEADER2)) = 0;
                    memcpy((BYTE *)vih2 + sizeof(VIDEOINFOHEADER2) + 1, extradata, extradata_size);
                }
                else
                {
                    memcpy((BYTE *)vih2 + sizeof(VIDEOINFOHEADER2), extradata, extradata_size);
                }
            }
            else if (codec_id == AV_CODEC_ID_ASS)
            {
                SUBTITLEINFO *sif = (SUBTITLEINFO *)pmt->ReallocFormatBuffer(sizeof(SUBTITLEINFO) + extradata_size);
                memcpy((BYTE *)sif + sizeof(SUBTITLEINFO), extradata, extradata_size);
            }
            else
            {
                if (pmt->formattype == FORMAT_VideoInfo)
                {
                    VIDEOINFOHEADER *vih =
                        (VIDEOINFOHEADER *)pmt->ReallocFormatBuffer(sizeof(VIDEOINFOHEADER) + extradata_size);
                    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER) + extradata_size;
                    memcpy((BYTE *)vih + sizeof(VIDEOINFOHEADER), extradata, extradata_size);
                }
                else if (pmt->formattype == FORMAT_VideoInfo2)
                {
                    VIDEOINFOHEADER2 *vih2 =
                        (VIDEOINFOHEADER2 *)pmt->ReallocFormatBuffer(sizeof(VIDEOINFOHEADER2) + extradata_size);
                    vih2->bmiHeader.biSize = sizeof(BITMAPINFOHEADER) + extradata_size;
                    memcpy((BYTE *)vih2 + sizeof(VIDEOINFOHEADER2), extradata, extradata_size);
                }
                else if (pmt->formattype == FORMAT_WaveFormatEx)
                {
                    WAVEFORMATEX *wfex =
                        (WAVEFORMATEX *)pmt->ReallocFormatBuffer(sizeof(WAVEFORMATEX) + extradata_size);
                    wfex->cbSize = extradata_size;
                    memcpy((BYTE *)wfex + sizeof(WAVEFORMATEX), extradata, extradata_size);
                }
                else if (pmt->formattype == FORMAT_WaveFormatExFFMPEG)
                {
                    WAVEFORMATEXFFMPEG *wfex =
                        (WAVEFORMATEXFFMPEG *)pmt->ReallocFormatBuffer(sizeof(WAVEFORMATEXFFMPEG) + extradata_size);
                    wfex->wfex.cbSize = extradata_size;
                    memcpy((BYTE *)wfex + sizeof(WAVEFORMATEXFFMPEG), extradata, extradata_size);
                }
                else if (pmt->formattype == FORMAT_VorbisFormat2)
                {
                    BYTE *p = extradata;
                    std::vector<int> sizes;
                    for (BYTE n = *p++; n > 0; n--)
                    {
                        int size = 0;
                        // Xiph Lacing
                        do
                        {
                            size += *p;
                        } while (*p++ == 0xFF);
                        sizes.push_back(size);
                    }

                    int totalsize = 0;
                    for (size_t i = 0; i < sizes.size(); i++)
                        totalsize += sizes[i];

                    sizes.push_back(extradata_size - (int)(p - extradata) - totalsize);
                    totalsize += sizes[sizes.size() - 1];

                    // 3 blocks is the currently valid Vorbis format
                    if (sizes.size() == 3)
                    {
                        VORBISFORMAT2 *pvf2 =
                            (VORBISFORMAT2 *)pmt->ReallocFormatBuffer(sizeof(VORBISFORMAT2) + totalsize);
                        BYTE *p2 = (BYTE *)pvf2 + sizeof(VORBISFORMAT2);
                        for (unsigned int i = 0; i < sizes.size(); p += sizes[i], p2 += sizes[i], i++)
                        {
                            memcpy(p2, p, pvf2->HeaderSize[i] = sizes[i]);
                        }
                    }
                }
                else
                {
                    DbgLog((LOG_TRACE, 10, L"::GetNextPacket() - Unsupported PMT change on codec %S",
                            avcodec_get_name(codec_id)));
                }
            }
        }
        if (paramchange)
        {
            uint32_t flags = AV_RL32(paramchange);
            int channels = 0, sample_rate = 0, width = 0, height = 0, aspect_num = 0, aspect_den = 0;
            paramchange += 4;
            if (flags & AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_COUNT)
            {
                channels = AV_RL32(paramchange);
                paramchange += 4;
            }
            if (flags & AV_SIDE_DATA_PARAM_CHANGE_CHANNEL_LAYOUT)
            {
                paramchange += 8;
            }
            if (flags & AV_SIDE_DATA_PARAM_CHANGE_SAMPLE_RATE)
            {
                sample_rate = AV_RL32(paramchange);
                paramchange += 4;
            }
            if (flags & AV_SIDE_DATA_PARAM_CHANGE_DIMENSIONS)
            {
                width = AV_RL32(paramchange);
                height = AV_RL32(paramchange + 4);
                paramchange += 8;
            }
            if (flags & AV_SIDE_DATA_PARAM_CHANGE_ASPECTRATIO)
            {
                aspect_num = AV_RL32(paramchange);
                aspect_den = AV_RL32(paramchange + 4);
                paramchange += 8;
            }
            if (pmt->majortype == MEDIATYPE_Video)
            {
                if ((pmt->formattype == FORMAT_VideoInfo || pmt->formattype == FORMAT_MPEGVideo) && width && height)
                {
                    VIDEOINFOHEADER *vih = (VIDEOINFOHEADER *)pmt->pbFormat;
                    vih->bmiHeader.biWidth = width;
                    vih->bmiHeader.biHeight = height;
                    vih->rcTarget.right = vih->rcSource.right = width;
                    vih->rcTarget.bottom = vih->rcSource.bottom = height;
                }
                else if ((pmt->formattype == FORMAT_VideoInfo2 || pmt->formattype == FORMAT_MPEG2Video) &&
                         ((width && height) || (aspect_num && aspect_den)))
                {
                    VIDEOINFOHEADER2 *vih2 = (VIDEOINFOHEADER2 *)pmt->pbFormat;
                    if (width && height)
                    {
                        vih2->bmiHeader.biWidth = width;
                        vih2->bmiHeader.biHeight = height;
                        vih2->rcTarget.right = vih2->rcSource.right = width;
                        vih2->rcTarget.bottom = vih2->rcSource.bottom = height;
                    }
                    if (aspect_num && aspect_den)
                    {
                        int num = vih2->bmiHeader.biWidth, den = vih2->bmiHeader.biHeight;
                        av_reduce(&num, &den, (int64_t)aspect_num * num, (int64_t)aspect_den * den, INT_MAX);
                        vih2->dwPictAspectRatioX = num;
                        vih2->dwPictAspectRatioY = den;
                    }
                }
            }
            else if (pmt->majortype == MEDIATYPE_Audio)
            {
                if ((pmt->formattype == FORMAT_WaveFormatEx || pmt->formattype == FORMAT_WaveFormatExFFMPEG) &&
                    (channels || sample_rate))
                {
                    WAVEFORMATEX *wfex = nullptr;
                    if (pmt->formattype == FORMAT_WaveFormatExFFMPEG)
                    {
                        WAVEFORMATEXFFMPEG *wfexff = (WAVEFORMATEXFFMPEG *)pmt->pbFormat;
                        wfex = &wfexff->wfex;
                    }
                    else
                    {
                        wfex = (WAVEFORMATEX *)pmt->pbFormat;
                    }
                    if (channels)
                        wfex->nChannels = channels;
                    if (sample_rate)
                        wfex->nSamplesPerSec = sample_rate;
                }
            }
        }

        if (pmt)
        {
            pPacket->pmt = CreateMediaType(pmt);
            SAFE_DELETE(pmt);
        }
    }
    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::ParseICYMetadataPacket()
{
    char *icy_data = nullptr;
    if (av_opt_get(m_avFormat, "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, (uint8_t **)&icy_data) >= 0 && icy_data &&
        strlen(icy_data) > 0)
    {
        std::string icyData(icy_data);
        size_t idx = icyData.find("StreamTitle");
        if (idx != std::string::npos)
        {
            // strip StreamTitle token and =
            std::string value = icyData.substr(idx + 12);
            idx = value.find_first_of(";");
            if (idx != std::string::npos)
                value = value.substr(0, idx);
            if (value[0] == '\'' || value[0] == '"')
                value = value.substr(1);
            if (value[value.length() - 1] == '\'' || value[value.length() - 1] == '"')
                value = value.substr(0, value.length() - 1);
            if (value.length() > 0)
            {
                av_dict_set(&m_avFormat->metadata, "title", value.c_str(), 0);
            }
        }
        // clear value, and only read again when its send again
        av_opt_set(m_avFormat, "icy_metadata_packet", "", AV_OPT_SEARCH_CHILDREN);
    }
    av_freep(&icy_data);

    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::GetNextPacket(Packet **ppPacket)
{
    CheckPointer(ppPacket, E_POINTER);

    // Return queued per-region ARIB packets before reading new data
    if (!m_aribRegionQueue.empty())
    {
        *ppPacket = m_aribRegionQueue.front();
        m_aribRegionQueue.pop_front();
        return S_OK;
    }

    // If true, S_FALSE is returned, indicating a soft-failure
    bool bReturnEmpty = false;

    // Read packet
    AVPacket pkt;
    Packet *pPacket = nullptr;

    // assume we are not eof
    if (m_avFormat->pb)
    {
        m_avFormat->pb->eof_reached = 0;
    }

    int result = 0;
    try
    {
        DBG_TIMING("av_read_frame", 30, result = av_read_frame(m_avFormat, &pkt))
    }
    catch (...)
    {
        // ignore..
    }

    if (result == AVERROR(EINTR) || result == AVERROR(EAGAIN))
    {
        // timeout, probably no real error, return empty packet
        bReturnEmpty = true;
    }
    else if (result == AVERROR_EOF)
    {
        DbgLog((LOG_TRACE, 10, L"::GetNextPacket(): End of File reached"));
    }
    else if (result < 0)
    {
        // meh, fail
    }
    else if (pkt.size <= 0 || pkt.stream_index < 0 || (unsigned)pkt.stream_index >= m_avFormat->nb_streams)
    {
        // XXX, in some cases ffmpeg returns a zero or negative packet size
        if (m_avFormat->pb && !m_avFormat->pb->eof_reached)
        {
            bReturnEmpty = true;
        }
        av_packet_unref(&pkt);
    }
    else
    {
        // Check right here if the stream is active, we can drop the package otherwise.
        AVStream *stream = m_avFormat->streams[pkt.stream_index];
        BOOL streamActive = FALSE;
        BOOL forcedSubStream = FALSE;
        for (int i = 0; i < unknown; ++i)
        {
            if (m_dActiveStreams[i] == pkt.stream_index)
            {
                streamActive = TRUE;
                break;
            }
        }

        // Accept it if its the forced subpic stream
        if (m_dActiveStreams[subpic] == FORCED_SUBTITLE_PID && pkt.stream_index == m_ForcedSubStream)
        {
            forcedSubStream = streamActive = TRUE;
        }

        // Accept H264 MVC streams, as they get combined with the base stream later
        if (m_bH264MVCCombine && stream->codecpar->codec_id == AV_CODEC_ID_H264_MVC)
            streamActive = TRUE;

        if (!streamActive && stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            stream->codecpar->codec_id == AV_CODEC_ID_ARIB_CAPTION && IsLateAribPlaceholderSelected())
        {
            // Caption type is the PES data_identifier, independent of profile
            // (A = full segment, C = one segment). Prefer captions on this pin.
            if (pkt.size < 3 || (pkt.data[0] != 0x80 && pkt.data[0] != 0x81))
            {
                av_packet_unref(&pkt);
                return S_FALSE;
            }
            const bool isSuperimpose = pkt.data[0] == 0x81;
            const bool boundToSuperimpose = m_LateAribSubtitleIsSuperimpose;

            if (m_LateAribSubtitleStream == -1 || (boundToSuperimpose && !isSuperimpose))
            {
                ARIB_LOG("[ARIB] bind late ARIB subtitle placeholder to stream=%d id=%d profile=%d (was %d)\n",
                         pkt.stream_index, stream->id, stream->codecpar->profile, m_LateAribSubtitleStream);
                m_LateAribSubtitleStream = pkt.stream_index;
                m_LateAribSubtitleIsSuperimpose = isSuperimpose;
                FlushAribPendingPackets();
            }

            if (m_LateAribSubtitleStream != pkt.stream_index ||
                m_LateAribSubtitleIsSuperimpose != isSuperimpose)
            {
                // Bound to another stream (typically captions while this is
                // superimpose): drop this packet instead of mixing both.
                av_packet_unref(&pkt);
                return S_FALSE;
            }

            stream->discard = AVDISCARD_DEFAULT;
            streamActive = TRUE;
        }

        if (!streamActive)
        {
            if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                ARIB_LOG("[ARIB] subtitle pkt DROPPED stream=%d active[subpic]=%d active[video]=%d active[audio]=%d\n",
                         pkt.stream_index,
                         m_dActiveStreams[subpic], m_dActiveStreams[video], m_dActiveStreams[audio]);
            av_packet_unref(&pkt);
            return S_FALSE;
        }

        pPacket = new Packet();
        if (!pPacket)
            return E_OUTOFMEMORY;

        // Convert timestamps to reference time and set them on the packet
        REFERENCE_TIME pts = ConvertTimestampToRT(pkt.pts, stream->time_base.num, stream->time_base.den);
        REFERENCE_TIME dts = ConvertTimestampToRT(pkt.dts, stream->time_base.num, stream->time_base.den);
        REFERENCE_TIME duration = ConvertTimestampToRT(pkt.duration, stream->time_base.num, stream->time_base.den, 0);

        pPacket->rtPTS = pts;
        pPacket->rtDTS = dts;
        pPacket->StreamId = (DWORD)pkt.stream_index;
        pPacket->bPosition = pkt.pos;

        if (stream->codecpar->codec_id == AV_CODEC_ID_H264)
        {
            if (m_bMatroska || m_bOgg)
            {
                if (!stream->codecpar->extradata_size || stream->codecpar->extradata[0] != 1 ||
                    AV_RB32(pkt.data) == 0x00000001)
                {
                    pPacket->dwFlags |= LAV_PACKET_H264_ANNEXB;
                }
                else
                { // No DTS for H264 in native format
                    dts = Packet::INVALID_TIME;
                }
            }
            else if (!m_bPMP && !m_bAVI)
            { // For most formats, DTS timestamps for h.264 are no fun
                dts = Packet::INVALID_TIME;
            }
        }

        if (m_bAVI && stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            // AVI's always have borked pts, specially if m_pFormatContext->flags includes
            // AVFMT_FLAG_GENPTS so always use dts
            pts = Packet::INVALID_TIME;
        }

        if (stream->codecpar->codec_id == AV_CODEC_ID_RV10 || stream->codecpar->codec_id == AV_CODEC_ID_RV20 ||
            stream->codecpar->codec_id == AV_CODEC_ID_RV30 || stream->codecpar->codec_id == AV_CODEC_ID_RV40)
        {
            pts = Packet::INVALID_TIME;
        }

        // Never use DTS for these formats
        if (!m_bAVI && (stream->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
                        stream->codecpar->codec_id == AV_CODEC_ID_MPEG1VIDEO))
            dts = Packet::INVALID_TIME;

        if (pkt.data)
        {
            result = pPacket->SetPacket(&pkt);
            if (result < 0)
            {
                SAFE_DELETE(pPacket);
                return E_OUTOFMEMORY;
            }
        }

        // Select the appropriate timestamps
        REFERENCE_TIME rt = Packet::INVALID_TIME;
        // Try the different times set, pts first, dts when pts is not valid
        if (pts != Packet::INVALID_TIME)
        {
            rt = pts;
        }
        else if (dts != Packet::INVALID_TIME)
        {
            rt = dts;
        }

        if (stream->codecpar->codec_id == AV_CODEC_ID_VC1)
        {
            if (m_bMatroska && m_bVC1Correction)
            {
                rt = pts;
                if (!m_bVC1SeenTimestamp)
                {
                    if (rt == Packet::INVALID_TIME && dts != Packet::INVALID_TIME)
                        rt = dts;
                    m_bVC1SeenTimestamp = (pts != Packet::INVALID_TIME);
                }
            }
            else if (m_bVC1Correction)
            {
                rt = dts;
                pPacket->dwFlags |= LAV_PACKET_PARSED;
            }
        }
        else if (stream->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT)
        {
            pPacket->dwFlags |= LAV_PACKET_MOV_TEXT;
        }

        // Mark the packet as parsed, so the forced subtitle parser doesn't hit it
        if (stream->codecpar->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE && m_bPGSNoParsing)
        {
            pPacket->dwFlags |= LAV_PACKET_PARSED;
        }

        pPacket->rtStart = pPacket->rtStop = rt;
        if (rt != Packet::INVALID_TIME)
        {
            pPacket->rtStop += (duration > 0 || stream->codecpar->codec_id == AV_CODEC_ID_TRUEHD) ? duration : 1;
        }

        if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
        {
            ARIB_LOG("[ARIB] subtitle pkt: stream=%d codec_id=%d size=%d\n",
                     (int)pPacket->StreamId,
                     (int)stream->codecpar->codec_id,
                     pPacket->GetDataSize());
            pPacket->bDiscontinuity = TRUE;

            if (forcedSubStream)
            {
                pPacket->dwFlags |= LAV_PACKET_FORCED_SUBTITLE;
                pPacket->dwFlags &= ~LAV_PACKET_PARSED;
            }

            if (stream->codecpar->codec_id == AV_CODEC_ID_SRT)
            {
                pPacket->dwFlags |= LAV_PACKET_SRT;
            }

            // ARIB B-24 caption / superimpose decoding
            if (stream->codecpar->codec_id == AV_CODEC_ID_ARIB_CAPTION)
            {
                // 0x80 = captions, 0x81 = superimpose; profile is separate.
                bool isSuperimpose = pPacket->GetDataSize() > 0 && pPacket->GetData()[0] == 0x81;
                int streamIdx = (int)pPacket->StreamId;
                DWORD outputStreamId = IsLateAribSubtitleActive(streamIdx) ? LATE_ARIB_SUBTITLE_PID : (DWORD)streamIdx;
                int dataSize  = pPacket->GetDataSize();
                const uint8_t *rawData = pPacket->GetData();

                ARIB_LOG("[ARIB] pkt: stream=%d super=%d size=%d pts_rt=%lld\n",
                         streamIdx, (int)isSuperimpose, dataSize, (long long)rt);
                if (rawData && dataSize >= 3)
                    ARIB_LOG("[ARIB] first3: %02X %02X %02X\n",
                             rawData[0], rawData[1], rawData[2]);

                aribcc_decoder_t *dec = GetOrCreateAribDecoder(streamIdx, isSuperimpose);
                if (dec && dataSize > 0)
                {
                    int64_t pts_ms = (rt != Packet::INVALID_TIME) ? (rt / 10000LL) : ARIBCC_PTS_NOPTS;

                    aribcc_caption_t caption;
                    aribcc_decode_status_t status = aribcc_decoder_decode(
                        dec, rawData, (size_t)dataSize, pts_ms, &caption);

                    ARIB_LOG("[ARIB] decode status=%d regions=%u text=%s\n",
                             (int)status,
                             (status == ARIBCC_DECODE_STATUS_GOT_CAPTION) ? caption.region_count : 0u,
                             (status == ARIBCC_DECODE_STATUS_GOT_CAPTION && caption.text) ? caption.text : "(none)");

                    if (status == ARIBCC_DECODE_STATUS_GOT_CAPTION)
                    {
                        // Read INI settings (caption or superimpose).
                        AribCaptionSettings captionSettings = GetAribCaptionSettings(isSuperimpose);

                        // Compute stop time, then apply delay offset.
                        REFERENCE_TIME rtNewStop;
                        constexpr REFERENCE_TIME kIndefiniteChunk = 10LL * 10000000LL; // 10s
                        bool hasExplicitStop =
                            caption.wait_duration != ARIBCC_DURATION_INDEFINITE && caption.wait_duration > 0;
                        if (hasExplicitStop)
                            rtNewStop = pPacket->rtStart + caption.wait_duration * 10000LL;
                        else
                            rtNewStop = pPacket->rtStart + kIndefiniteChunk;

                        REFERENCE_TIME delayHns = (REFERENCE_TIME)captionSettings.delayMs * 10000LL;
                        pPacket->rtStart += delayHns;
                        rtNewStop        += delayHns;

                        ARIB_LOG("[ARIB] caption timing flags=0x%x wait=%lld start=%lld stop=%lld delay=%dms\n",
                                 (unsigned)caption.flags, (long long)caption.wait_duration,
                                 (long long)pPacket->rtStart, (long long)rtNewStop,
                                 captionSettings.delayMs);

                        std::vector<ASSEvent> regionTexts = BuildASSFromCaption(caption, captionSettings);
                        aribcc_caption_cleanup(&caption);

                        // Flatten for debug log
                        ARIB_LOG("[ARIB] assText(%zu events): %s\n", regionTexts.size(),
                                 regionTexts.empty() ? "(empty)" :
                                 regionTexts[0].payload.substr(0, 100).c_str());

                        // Helper: release pending packet with corrected rtStop and
                        // queue its extra regions (ruby etc.).
                        auto flushPending = [&](int sid, REFERENCE_TIME stopBound) -> Packet *
                        {
                            auto it = m_aribPendingPackets.find(sid);
                            if (it == m_aribPendingPackets.end() || !it->second)
                                return nullptr;
                            Packet *p = it->second;
                            it->second = nullptr;
                            m_aribPendingPackets.erase(it);
                            bool explicitStop = false;
                            auto xit = m_aribPendingExplicitStop.find(sid);
                            if (xit != m_aribPendingExplicitStop.end())
                            {
                                explicitStop = xit->second;
                                m_aribPendingExplicitStop.erase(xit);
                            }
                            if (stopBound <= p->rtStart)
                            {
                                delete p;
                                auto eit = m_aribPendingExtras.find(sid);
                                if (eit != m_aribPendingExtras.end())
                                {
                                    for (auto *ep : eit->second)
                                        delete ep;
                                    eit->second.clear();
                                }
                                return nullptr;
                            }
                            p->rtStop = explicitStop ? (std::min)(p->rtStop, stopBound) : stopBound;

                            // Also release any extra regions stored with this pending
                            auto eit = m_aribPendingExtras.find(sid);
                            if (eit != m_aribPendingExtras.end())
                            {
                                for (auto *ep : eit->second)
                                {
                                    ep->rtStop = p->rtStop;
                                    m_aribRegionQueue.push_back(ep);
                                }
                                eit->second.clear();
                            }
                            return p;
                        };

                        Packet *toDeliver = nullptr;

                        if (regionTexts.empty())
                        {
                            // Clear-screen: release pending with corrected stop time
                            toDeliver = flushPending(streamIdx, pPacket->rtStart);
                            SAFE_DELETE(pPacket);
                        }
                        else
                        {
                            // Release previous pending (corrected rtStop = this event's start)
                            toDeliver = flushPending(streamIdx, pPacket->rtStart);

                            // Build event 0 (main packet).
                            {
                                const ASSEvent &ev = regionTexts[0];
                                LONG ro = NextAribReadOrder();
                                char roPrefix[32];
                                snprintf(roPrefix, sizeof(roPrefix), "%ld,%d,Default,,0,0,0,,", ro, ev.layer);
                                std::string payload = roPrefix + ev.payload;
                                pPacket->StreamId = outputStreamId;
                                pPacket->rtStop = rtNewStop;
                                pPacket->SetData(payload.c_str(), (int)payload.size());
                                pPacket->dwFlags |= LAV_PACKET_PARSED;
                            }

                            std::vector<Packet *> currentExtras;
                            auto &extras = m_aribPendingExtras[streamIdx];
                            for (auto *ep : extras) delete ep;
                            extras.clear();
                            for (size_t ri = 1; ri < regionTexts.size(); ri++)
                            {
                                const ASSEvent &ev = regionTexts[ri];
                                Packet *rPkt = new Packet();
                                if (!rPkt) break;
                                rPkt->StreamId = outputStreamId;
                                rPkt->rtStart = pPacket->rtStart;
                                rPkt->rtStop  = pPacket->rtStop;
                                rPkt->bDiscontinuity = TRUE;
                                rPkt->dwFlags = LAV_PACKET_PARSED;
                                LONG ro = NextAribReadOrder();
                                char roPrefix[32];
                                snprintf(roPrefix, sizeof(roPrefix), "%ld,%d,Default,,0,0,0,,", ro, ev.layer);
                                std::string payload = roPrefix + ev.payload;
                                rPkt->SetData(payload.c_str(), (int)payload.size());
                                currentExtras.push_back(rPkt);
                            }

                            if (hasExplicitStop)
                            {
                                Packet *current = pPacket;
                                pPacket = nullptr;
                                if (toDeliver)
                                {
                                    m_aribRegionQueue.push_back(current);
                                    for (auto *ep : currentExtras)
                                        m_aribRegionQueue.push_back(ep);
                                }
                                else
                                {
                                    pPacket = current;
                                    for (auto *ep : currentExtras)
                                        m_aribRegionQueue.push_back(ep);
                                }
                            }
                            else
                            {
                                m_aribPendingPackets[streamIdx] = pPacket;
                                m_aribPendingExplicitStop[streamIdx] = false;
                                pPacket = nullptr;
                                extras.swap(currentExtras);
                            }
                        }

                        if (toDeliver)
                        {
                            pPacket = toDeliver;
                        }
                        else if (!pPacket)
                        {
                            return S_FALSE;
                        }
                        if (pPacket)
                            pPacket->StreamId = outputStreamId;
                    }
                    else
                    {
                        // NO_CAPTION packets can appear repeatedly while an indefinite
                        // ARIB caption should remain on screen. If the current PTS has
                        // crossed the pending chunk boundary, resend the same payload as
                        // the next timed chunk so MPC-BE receives samples before they are
                        // already stale.
                        aribcc_caption_cleanup(&caption);
                        auto &pendingRef = m_aribPendingPackets[streamIdx];
                        auto xit = m_aribPendingExplicitStop.find(streamIdx);
                        bool explicitStop = (xit != m_aribPendingExplicitStop.end() && xit->second);
                        if (pendingRef && !explicitStop && rt != Packet::INVALID_TIME &&
                            pendingRef->rtStop != Packet::INVALID_TIME && rt >= pendingRef->rtStop)
                        {
                            constexpr REFERENCE_TIME kIndefiniteChunk = 10LL * 10000000LL; // 10s
                            Packet *deliver = pendingRef;
                            pendingRef = ClonePacket(deliver);
                            if (pendingRef)
                            {
                                pendingRef->rtStart = deliver->rtStop;
                                pendingRef->rtStop = pendingRef->rtStart + kIndefiniteChunk;
                                pendingRef->bDiscontinuity = TRUE;
                                // Renderers may key events by ReadOrder, so the
                                // next chunk needs one of its own.
                                RenumberASSReadOrder(pendingRef, NextAribReadOrder());
                            }

                            auto eit = m_aribPendingExtras.find(streamIdx);
                            if (eit != m_aribPendingExtras.end())
                            {
                                std::vector<Packet *> nextExtras;
                                for (auto *ep : eit->second)
                                {
                                    Packet *nextExtra = ClonePacket(ep);
                                    if (nextExtra && pendingRef)
                                    {
                                        nextExtra->rtStart = pendingRef->rtStart;
                                        nextExtra->rtStop = pendingRef->rtStop;
                                        RenumberASSReadOrder(nextExtra, NextAribReadOrder());
                                        nextExtras.push_back(nextExtra);
                                    }
                                    else
                                    {
                                        delete nextExtra;
                                    }
                                    m_aribRegionQueue.push_back(ep);
                                }
                                eit->second.swap(nextExtras);
                            }

                            SAFE_DELETE(pPacket);
                            pPacket = deliver;
                        }
                        else
                        {
                            SAFE_DELETE(pPacket);
                            return S_FALSE;
                        }
                        if (pPacket)
                            pPacket->StreamId = outputStreamId;
                    }
                }
                else
                {
                    ARIB_LOG("[ARIB] skipped: dec=%p size=%d\n", (void*)dec, dataSize);
                    SAFE_DELETE(pPacket);
                    return S_FALSE;
                }
            }
        }

        if (stream->codecpar->codec_id == AV_CODEC_ID_PCM_S16BE_PLANAR ||
            stream->codecpar->codec_id == AV_CODEC_ID_PCM_S16LE_PLANAR ||
            stream->codecpar->codec_id == AV_CODEC_ID_PCM_S24LE_PLANAR ||
            stream->codecpar->codec_id == AV_CODEC_ID_PCM_S32LE_PLANAR)
            pPacket->dwFlags |= LAV_PACKET_PLANAR_PCM;

        if (stream->codecpar->codec_id == AV_CODEC_ID_WEBVTT)
        {
            size_t id_size = 0, settings_size = 0;
            uint8_t *id = NULL, *settings = NULL;
            id = av_packet_get_side_data(&pkt, AV_PKT_DATA_WEBVTT_IDENTIFIER, &id_size);
            settings = av_packet_get_side_data(&pkt, AV_PKT_DATA_WEBVTT_SETTINGS, &settings_size);

            int text_size = pPacket->GetDataSize();

            // allocate size for id/settings
            int pkt_size = text_size + (int)id_size + 2 + (int)settings_size + 2;
            pPacket->SetDataSize(pkt_size);

            uint8_t *data = pPacket->GetData();

            // offset data
            memmove(data + id_size + 2 + settings_size + 2, data, text_size);

            // write id
            if (id && id_size > 0)
                memcpy(data, id, id_size);

            data[id_size + 0] = '\r';
            data[id_size + 1] = '\n';

            // write settings
            if (settings && settings_size > 0)
                memcpy(data + id_size + 2, settings, settings_size);

            data[id_size + 2 + settings_size + 0] = '\r';
            data[id_size + 2 + settings_size + 1] = '\n';
        }

        // Update extradata and send new mediatype, when required
        size_t sidedata_size = 0;
        uint8_t *sidedata = av_packet_get_side_data(&pkt, AV_PKT_DATA_NEW_EXTRADATA, &sidedata_size);
        size_t paramchange_size = 0;
        uint8_t *paramchange = av_packet_get_side_data(&pkt, AV_PKT_DATA_PARAM_CHANGE, &paramchange_size);
        if ((sidedata && sidedata_size) || (paramchange && paramchange_size))
        {
            CreatePacketMediaType(pPacket, stream->codecpar->codec_id, sidedata, (int)sidedata_size, paramchange,
                                  (int)paramchange_size);
        }

        pPacket->bSyncPoint = pkt.flags & AV_PKT_FLAG_KEY;
        pPacket->bDiscontinuity = !m_pBluRay && (pkt.flags & AV_PKT_FLAG_CORRUPT);
#ifdef DEBUG
        if (pkt.flags & AV_PKT_FLAG_CORRUPT)
            DbgLog((LOG_TRACE, 10, L"::GetNextPacket() - Signaling Discontinuinty because of corrupt package"));
#endif

        if (pPacket->rtStart != AV_NOPTS_VALUE)
            m_rtCurrent = pPacket->rtStart;

        av_packet_unref(&pkt);
    }

    if (m_pBluRay && pPacket)
    {
        HRESULT hr = m_pBluRay->ProcessPacket(pPacket);
        if (hr != S_OK)
        {
            SAFE_DELETE(pPacket);
            bReturnEmpty = bReturnEmpty || hr == S_FALSE;
        }
    }

    if (m_bH264MVCCombine && pPacket && pPacket->StreamId == m_nH264MVCExtensionStream)
    {
        if (FAILED(QueueMVCExtension(pPacket)))
        {
            SAFE_DELETE(pPacket);
            return E_FAIL;
        }

        return S_FALSE;
    }

    if (m_bH264MVCCombine && pPacket && pPacket->StreamId == m_nH264MVCBaseStream)
    {
        HRESULT hr = CombineMVCBaseExtension(pPacket);
        if (hr != S_OK)
        {
            SAFE_DELETE(pPacket);

            // S_FALSE indicates a skipped packet, not a hard failure
            if (hr == S_FALSE)
                bReturnEmpty = true;
        }
    }

    if (bReturnEmpty && !pPacket)
    {
        return S_FALSE;
    }
    if (!pPacket)
    {
        return E_FAIL;
    }

    ParseICYMetadataPacket();

    *ppPacket = pPacket;
    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::QueueMVCExtension(Packet *pPacket)
{
    m_MVCExtensionQueue.push_back(pPacket);
    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::FlushMVCExtensionQueue()
{
    for (auto it = m_MVCExtensionQueue.begin(); it != m_MVCExtensionQueue.end(); it++)
    {
        delete (*it);
    }

    m_MVCExtensionQueue.clear();
    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::CombineMVCBaseExtension(Packet *pBasePacket)
{
    while (!m_MVCExtensionQueue.empty())
    {
        Packet *pExtensionPacket = m_MVCExtensionQueue.front();
        if (pExtensionPacket->rtDTS == pBasePacket->rtDTS || pBasePacket->rtDTS == Packet::INVALID_TIME ||
            pExtensionPacket->rtDTS == Packet::INVALID_TIME)
        {
            if (pBasePacket->Append(pExtensionPacket) < 0)
                return E_FAIL;

            m_MVCExtensionQueue.pop_front();
            delete pExtensionPacket;
            return S_OK;
        }
        else if (pExtensionPacket->rtDTS < pBasePacket->rtDTS)
        {
            DbgLog((LOG_TRACE, 10, L"CLAVFDemuxer::CombineMVCBaseExtension(): Dropping extension %I64d, base is %I64d",
                    pExtensionPacket->rtDTS, pBasePacket->rtDTS));
            m_MVCExtensionQueue.pop_front();
            delete pExtensionPacket;
        }
        else if (pExtensionPacket->rtDTS > pBasePacket->rtDTS)
        {
            DbgLog((LOG_TRACE, 10,
                    L"CLAVFDemuxer::CombineMVCBaseExtension(): Dropping base %I64d, next extension is %I64d",
                    pBasePacket->rtDTS, pExtensionPacket->rtDTS));
            return S_FALSE;
        }
    }

    if (m_pBluRay && m_MVCExtensionQueue.empty())
    {
        HRESULT hr = m_pBluRay->FillMVCExtensionQueue(pBasePacket->rtDTS);
        if (hr == S_OK)
            return CombineMVCBaseExtension(pBasePacket);
        else if (FAILED(hr))
        {
            DbgLog((LOG_TRACE, 10, L"CLAVFDemuxer::CombineMVCBaseExtension(): Filling MVC extension queue failed"));
            return hr;
        }
    }

    DbgLog((LOG_TRACE, 10, L"CLAVFDemuxer::CombineMVCBaseExtension(): Ran out of extension packets for base %I64d",
            pBasePacket->rtDTS));
    return S_FALSE;
}

STDMETHODIMP CLAVFDemuxer::Seek(REFERENCE_TIME rTime)
{
    int seekStreamId = m_dActiveStreams[video];
    int64_t seek_pts = 0;
retry:
    // If we have a video stream, seek on that one. If we don't, well, then don't!
    if (rTime > 0)
    {
        if (seekStreamId != -1)
        {
            AVStream *stream = m_avFormat->streams[seekStreamId];
            seek_pts = ConvertRTToTimestamp(rTime, stream->time_base.num, stream->time_base.den);
        }
        else
        {
            seek_pts = ConvertRTToTimestamp(rTime, 1, AV_TIME_BASE);
        }
    }

    if (seek_pts < 0)
        seek_pts = 0;

    if (strcmp(m_pszInputFormat, "rawvideo") == 0 && seek_pts == 0)
        return SeekByte(0, AVSEEK_FLAG_BACKWARD);

    int flags = AVSEEK_FLAG_BACKWARD;

    int ret = av_seek_frame(m_avFormat, seekStreamId, seek_pts, flags);
    if (ret < 0)
    {
        DbgLog((LOG_CUSTOM1, 1, L"::Seek() -- Key-Frame Seek failed"));
        ret = av_seek_frame(m_avFormat, seekStreamId, seek_pts, flags | AVSEEK_FLAG_ANY);
        if (ret < 0)
        {
            DbgLog((LOG_ERROR, 1, L"::Seek() -- Inaccurate Seek failed as well"));
            if (seekStreamId == m_dActiveStreams[video] && seekStreamId != -1 && m_dActiveStreams[audio] != -1)
            {
                DbgLog((LOG_ERROR, 1, L"::Seek() -- retrying seek on audio stream"));
                seekStreamId = m_dActiveStreams[audio];
                goto retry;
            }
            if (seek_pts == 0)
            {
                DbgLog((LOG_ERROR, 1, L" -> attempting byte seek to position 0"));
                return SeekByte(0, AVSEEK_FLAG_BACKWARD);
            }
        }
    }

    for (unsigned i = 0; i < m_avFormat->nb_streams; i++)
    {
        init_parser(m_avFormat, m_avFormat->streams[i]);
        UpdateParserFlags(m_avFormat->streams[i]);
    }

    m_bVC1SeenTimestamp = FALSE;

    // Flush MVC extensions on seek (no-op if empty)
    FlushMVCExtensionQueue();

    // Flush ARIB decoders and pending packets on seek
    for (auto &kv : m_aribDecoders)
        aribcc_decoder_flush(kv.second);
    for (auto &kv : m_aribSuperDecoders)
        aribcc_decoder_flush(kv.second);
    FlushAribPendingPackets();

    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::SeekByte(int64_t pos, int flags)
{
    int ret = av_seek_frame(m_avFormat, -1, pos, flags | AVSEEK_FLAG_BYTE);
    if (ret < 0)
    {
        DbgLog((LOG_ERROR, 1, L"::SeekByte() -- Seek failed"));
    }

    for (unsigned i = 0; i < m_avFormat->nb_streams; i++)
    {
        init_parser(m_avFormat, m_avFormat->streams[i]);
        UpdateParserFlags(m_avFormat->streams[i]);
    }

    m_bVC1SeenTimestamp = FALSE;

    // Flush MVC extensions on seek (no-op if empty)
    FlushMVCExtensionQueue();

    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::Reset()
{
    return SeekByte(0, AVSEEK_FLAG_ANY);
}

const char *CLAVFDemuxer::GetContainerFormat() const
{
    return m_pszInputFormat;
}

/////////////////////////////////////////////////////////////////////////////
// IAMExtendedSeeking
STDMETHODIMP CLAVFDemuxer::get_ExSeekCapabilities(long *pExCapabilities)
{
    CheckPointer(pExCapabilities, E_POINTER);
    *pExCapabilities = AM_EXSEEK_CANSEEK;
    if (m_avFormat && m_avFormat->nb_chapters > 0)
        *pExCapabilities |= AM_EXSEEK_MARKERSEEK;
    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::get_MarkerCount(long *pMarkerCount)
{
    CheckPointer(pMarkerCount, E_POINTER);
    CheckPointer(m_avFormat, E_UNEXPECTED);
    *pMarkerCount = (long)m_avFormat->nb_chapters;
    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::get_CurrentMarker(long *pCurrentMarker)
{
    CheckPointer(pCurrentMarker, E_POINTER);
    CheckPointer(m_avFormat, E_UNEXPECTED);

    *pCurrentMarker = 0;

    REFERENCE_TIME rtCurrent = m_rtCurrent;
    IFilterGraph *pGraph = m_pSettings->GetFilterGraph();
    if (pGraph)
    {
        IMediaSeeking *pSeeking = nullptr;
        if (SUCCEEDED(pGraph->QueryInterface(&pSeeking)))
        {
            if (FAILED(pSeeking->GetCurrentPosition(&rtCurrent)))
            {
                DbgLog((LOG_TRACE, 10, L"get_CurrentMarker: Obtaining current playback position failed"));
                rtCurrent = m_rtCurrent;
            }
            SafeRelease(&pSeeking);
        }
        SafeRelease(&pGraph);
    }

    // Can the time_base change in between chapters?
    // Anyhow, we do the calculation in the loop, just to be safe
    for (unsigned int i = 0; i < m_avFormat->nb_chapters; ++i)
    {
        int64_t pts = ConvertRTToTimestamp(rtCurrent, m_avFormat->chapters[i]->time_base.num,
                                           m_avFormat->chapters[i]->time_base.den);
        // Check if the pts is in between the bounds of the chapter
        if (pts >= m_avFormat->chapters[i]->start)
        {
            *pCurrentMarker = (i + 1);
            // Many files only have chapter start points and no end times
            if (pts <= m_avFormat->chapters[i]->end)
                return S_OK;
        }
    }
    return *pCurrentMarker > 0 ? S_OK : E_FAIL;
}

STDMETHODIMP CLAVFDemuxer::GetMarkerTime(long MarkerNum, double *pMarkerTime)
{
    CheckPointer(pMarkerTime, E_POINTER);
    // Chapters go by a 1-based index, doh
    unsigned int index = MarkerNum - 1;
    if (index >= m_avFormat->nb_chapters)
    {
        return E_FAIL;
    }

    REFERENCE_TIME rt =
        ConvertTimestampToRT(m_avFormat->chapters[index]->start, m_avFormat->chapters[index]->time_base.num,
                             m_avFormat->chapters[index]->time_base.den);
    *pMarkerTime = (double)rt / DSHOW_TIME_BASE;

    return S_OK;
}

STDMETHODIMP CLAVFDemuxer::GetMarkerName(long MarkerNum, BSTR *pbstrMarkerName)
{
    CheckPointer(pbstrMarkerName, E_POINTER);
    // Chapters go by a 1-based index, doh
    unsigned int index = MarkerNum - 1;
    if (index >= m_avFormat->nb_chapters)
    {
        return E_FAIL;
    }
    // Get the title, or generate one
    if (AVDictionaryEntry *dictEntry = av_dict_get(m_avFormat->chapters[index]->metadata, "title", nullptr, 0))
    {
        *pbstrMarkerName = ConvertCharToBSTR(dictEntry->value);
    }
    else
    {
        OLECHAR wTitle[128];
        swprintf_s(wTitle, L"Chapter %d", MarkerNum);
        *pbstrMarkerName = SysAllocString(wTitle);
    }

    return S_OK;
}

/////////////////////////////////////////////////////////////////////////////
// IKeyFrameInfo
STDMETHODIMP CLAVFDemuxer::GetKeyFrameCount(UINT &nKFs)
{
    if (m_dActiveStreams[video] < 0)
    {
        return E_NOTIMPL;
    }

    if (!m_bMatroska && !m_bAVI && !m_bMP4)
    {
        return E_FAIL;
    }

    // No reliable info for fragmented mp4 files
    if (m_bMP4)
    {
        MOVContext *mov = (MOVContext *)m_avFormat->priv_data;
        if (mov->frag_index.nb_items)
            return S_FALSE;
    }

    nKFs = 0;

    AVStream *stream = m_avFormat->streams[m_dActiveStreams[video]];
    int nb_indexes = avformat_index_get_entries_count(stream);
    for (int i = 0; i < nb_indexes; i++)
    {
        const AVIndexEntry *entry = avformat_index_get_entry(stream, i);
        if (entry && (entry->flags & AVINDEX_KEYFRAME))
            nKFs++;
    }
    return (nKFs == stream->nb_frames) ? S_FALSE : S_OK;
}

STDMETHODIMP CLAVFDemuxer::GetKeyFrames(const GUID *pFormat, REFERENCE_TIME *pKFs, UINT &nKFs)
{
    CheckPointer(pFormat, E_POINTER);
    CheckPointer(pKFs, E_POINTER);

    if (m_dActiveStreams[video] < 0)
    {
        return E_NOTIMPL;
    }

    if (!m_bMatroska && !m_bAVI && !m_bMP4)
    {
        return E_FAIL;
    }

    // No reliable info for fragmented mp4 files
    if (m_bMP4)
    {
        MOVContext *mov = (MOVContext *)m_avFormat->priv_data;
        if (mov->frag_index.nb_items)
            return S_FALSE;
    }

    if (*pFormat != TIME_FORMAT_MEDIA_TIME)
        return E_INVALIDARG;

    UINT nKFsMax = nKFs;
    nKFs = 0;

    // CTTS counter for MP4
    int ctts_sample_counter = 0;
    uint32_t ctts_index = 0;

    AVStream *stream = m_avFormat->streams[m_dActiveStreams[video]];
    int nb_indexes = avformat_index_get_entries_count(stream);
    for (int i = 0; i < nb_indexes && nKFs < nKFsMax; i++)
    {
        const AVIndexEntry *entry = avformat_index_get_entry(stream, i);
        if (entry && (entry->flags & AVINDEX_KEYFRAME))
        {
            int64_t timestamp = entry->timestamp;

            // MP4 index timestamps are DTS, seeking expects PTS however, so offset them accordingly to ensure seeking
            // works as expected
            if (m_bMP4)
            {
                MOVStreamContext *sc = (MOVStreamContext *)stream->priv_data;
                if (i < sc->sample_offsets_count)
                    timestamp += (sc->sample_offsets[i] + sc->dts_shift);
                else if (sc->tts_count)
                {
                    // find the next CTTS entry, if needed
                    while (ctts_sample_counter <= i && ctts_index < sc->tts_count)
                    {
                        ctts_sample_counter += sc->tts_data[ctts_index++].count;
                    }

                    // apply the CTTS offset to the timestamp
                    if (ctts_sample_counter > i)
                        timestamp += (sc->tts_data[ctts_index - 1].offset + sc->dts_shift);
                    else
                        timestamp += (sc->min_corrected_pts + sc->dts_shift);
                }
                else
                    timestamp += (sc->min_corrected_pts + sc->dts_shift);
            }

            pKFs[nKFs] = ConvertTimestampToRT(timestamp, stream->time_base.num, stream->time_base.den);
            nKFs++;
        }
    }
    return S_OK;
}

int CLAVFDemuxer::GetStreamIdxFromTotalIdx(size_t index) const
{
    const stream *st = GetStreamFromTotalIdx(index);
    if (st)
        return st->pid;
    return -1;
}

const CBaseDemuxer::stream *CLAVFDemuxer::GetStreamFromTotalIdx(size_t index) const
{
    int type = video;
    size_t count_v = m_streams[video].size();
    size_t count_a = m_streams[audio].size();
    size_t count_s = m_streams[subpic].size();
    if (index >= count_v)
    {
        index -= count_v;
        type = audio;
        if (index >= count_a)
        {
            index -= count_a;
            type = subpic;
            if (index >= count_s)
                return nullptr;
        }
    }

    return &m_streams[type][index];
}

/////////////////////////////////////////////////////////////////////////////
// ITrackInfo
STDMETHODIMP_(UINT) CLAVFDemuxer::GetTrackCount()
{
    if (!m_avFormat)
        return 0;

    size_t count = m_streams[video].size() + m_streams[audio].size() + m_streams[subpic].size();

    return (UINT)count;
}

// \param aTrackIdx the track index (from 0 to GetTrackCount()-1)
STDMETHODIMP_(BOOL) CLAVFDemuxer::GetTrackInfo(UINT aTrackIdx, struct TrackElement *pStructureToFill)
{
    DbgLog((LOG_TRACE, 20, L"ITrackInfo::GetTrackInfo(): index %d, struct: %p", aTrackIdx, pStructureToFill));

    if (!m_avFormat || !pStructureToFill)
        return FALSE;

    ZeroMemory(pStructureToFill, sizeof(*pStructureToFill));
    pStructureToFill->Size = sizeof(*pStructureToFill);

    const stream *st = GetStreamFromTotalIdx(aTrackIdx);
    if (!st || st->pid < 0 || st->pid == NO_SUBTITLE_PID)
        return FALSE;

    if (st->pid == FORCED_SUBTITLE_PID)
    {
        pStructureToFill->FlagDefault = 0;
        pStructureToFill->FlagForced = 1;
        pStructureToFill->Type = TypeSubtitle;
        strcpy_s(pStructureToFill->Language, "und");
    }
    else if (st->pid == LATE_ARIB_SUBTITLE_PID)
    {
        pStructureToFill->FlagDefault = 1;
        pStructureToFill->FlagForced = 0;
        pStructureToFill->Type = TypeSubtitle;
        strcpy_s(pStructureToFill->Language, "jpn");
    }
    else
    {
        const AVStream *avst = m_avFormat->streams[st->pid];

        // Fill structure
        pStructureToFill->FlagDefault = (avst->disposition & AV_DISPOSITION_DEFAULT);
        pStructureToFill->FlagForced = (avst->disposition & AV_DISPOSITION_FORCED);
        strncpy_s(pStructureToFill->Language, st->language.c_str(), _TRUNCATE);
        pStructureToFill->Language[3] = '\0';

        pStructureToFill->Type = (avst->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                                     ? TypeVideo
                                     : (avst->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
                                           ? TypeAudio
                                           : (avst->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) ? TypeSubtitle : 0;
    }

    // The following flags are not exported via avformat
    pStructureToFill->FlagLacing = 0;
    pStructureToFill->MaxCache = 0;
    pStructureToFill->MinCache = 0;

    return TRUE;
}

// Get an extended information struct relative to the track type
STDMETHODIMP_(BOOL) CLAVFDemuxer::GetTrackExtendedInfo(UINT aTrackIdx, void *pStructureToFill)
{
    if (!m_avFormat || !pStructureToFill)
        return FALSE;

    int id = GetStreamIdxFromTotalIdx(aTrackIdx);
    if (id == (int)LATE_ARIB_SUBTITLE_PID)
        return FALSE;
    if (id < 0 || (unsigned)id >= m_avFormat->nb_streams)
        return FALSE;

    const AVStream *st = m_avFormat->streams[id];

    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
    {
        TrackExtendedInfoVideo *pTEIV = (TrackExtendedInfoVideo *)pStructureToFill;

        ZeroMemory(pTEIV, sizeof(*pTEIV));

        pTEIV->Size = sizeof(*pTEIV);
        pTEIV->DisplayUnit = 0; // always pixels
        pTEIV->DisplayWidth = st->codecpar->width;
        pTEIV->DisplayHeight = st->codecpar->height;

        pTEIV->PixelWidth = st->codecpar->width;
        pTEIV->PixelHeight = st->codecpar->height;

        pTEIV->AspectRatioType = 0;
        pTEIV->Interlaced = 0;
    }
    else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
    {
        TrackExtendedInfoAudio *pTEIA = (TrackExtendedInfoAudio *)pStructureToFill;

        ZeroMemory(pTEIA, sizeof(*pTEIA));

        pTEIA->Size = sizeof(*pTEIA);
        pTEIA->BitDepth = st->codecpar->bits_per_coded_sample;
        pTEIA->Channels = st->codecpar->ch_layout.nb_channels;
        pTEIA->OutputSamplingFrequency = (FLOAT)st->codecpar->sample_rate;
        pTEIA->SamplingFreq = (FLOAT)st->codecpar->sample_rate;
    }

    return TRUE;
}

STDMETHODIMP_(BSTR) CLAVFDemuxer::GetTrackName(UINT aTrackIdx)
{

    const stream *st = GetStreamFromTotalIdx(aTrackIdx);
    if (!st)
        return nullptr;

    BSTR trackName = nullptr;
    if (!st->trackName.empty())
    {
        trackName = ConvertCharToBSTR(st->trackName.c_str());
    }

    return trackName;
}

STDMETHODIMP_(BSTR) CLAVFDemuxer::GetTrackCodecName(UINT aTrackIdx)
{
    if (!m_avFormat)
        return nullptr;

    int id = GetStreamIdxFromTotalIdx(aTrackIdx);
    if (id == (int)LATE_ARIB_SUBTITLE_PID)
        return ConvertCharToBSTR("ARIB Captions");
    if (id < 0 || (unsigned)id >= m_avFormat->nb_streams)
        return nullptr;

    const AVStream *st = m_avFormat->streams[id];

    BSTR codecName = nullptr;

    std::string codec = get_codec_name(st->codecpar);
    if (!codec.empty())
    {
        codecName = ConvertCharToBSTR(codec.c_str());
    }

    return codecName;
}

/////////////////////////////////////////////////////////////////////////////
// IPropertyBag

static struct
{
    const char *original;
    const char *map;
    int stream; // 0 = none, 1 = video, 2 = audio, 3 = sub
} mappedPropertys[] = {
    {"rotation", "rotate", 1},
    {"rotate", nullptr, 1},
    {"stereoscopic3dmode", "stereo_mode", 1},
    {"stereo_mode", nullptr, 1},
    {"stereo_subtitle_offset_id", "3d-plane", 3},
    {"stereo_subtitle_offset_ids", "pg_offset_sequences", 0},
    {"stereo_interactive_offset_ids", "ig_offset_sequences", 0},
};

STDMETHODIMP CLAVFDemuxer::Read(LPCOLESTR pszPropName, VARIANT *pVar, IErrorLog *pErrorLog)
{
    CheckPointer(pszPropName, E_INVALIDARG);
    CheckPointer(pVar, E_INVALIDARG);
    int stream = -1;

    // Verify type
    if (pVar->vt != VT_EMPTY && pVar->vt != VT_BSTR)
        return E_FAIL;

    ATL::CW2A propNameConv(pszPropName);
    const char *propName = propNameConv;

    // Map property names
    for (int i = 0; i < countof(mappedPropertys); i++)
    {
        if (_stricmp(propName, mappedPropertys[i].original) == 0)
        {
            if (mappedPropertys[i].map)
                propName = mappedPropertys[i].map;
            if (mappedPropertys[i].stream)
            {
                int nStreamType = mappedPropertys[i].stream - 1;
                stream = m_dActiveStreams[nStreamType];
                if (nStreamType == subpic && stream == FORCED_SUBTITLE_PID)
                    stream = m_ForcedSubStream;
                else if (nStreamType == subpic && stream == (int)LATE_ARIB_SUBTITLE_PID)
                    stream = m_LateAribSubtitleStream;
            }
            break;
        }
    }

    BSTR bstrValue = nullptr;
    HRESULT hr = GetBSTRMetadata(propName, &bstrValue, stream);
    if (SUCCEEDED(hr))
    {
        VariantClear(pVar);
        pVar->vt = VT_BSTR;
        pVar->bstrVal = bstrValue;
    }
    return hr;
}

STDMETHODIMP CLAVFDemuxer::Write(LPCOLESTR pszPropName, VARIANT *pVar)
{
    return E_NOTIMPL;
}

/////////////////////////////////////////////////////////////////////////////
// Internal Functions
STDMETHODIMP CLAVFDemuxer::AddStream(int streamId)
{
    HRESULT hr = S_OK;
    AVStream *pStream = m_avFormat->streams[streamId];

    if (pStream->codecpar->codec_type == AVMEDIA_TYPE_UNKNOWN || pStream->discard == AVDISCARD_ALL ||
        (pStream->codecpar->codec_id == AV_CODEC_ID_NONE && pStream->codecpar->codec_tag == 0) ||
        (!m_bSubStreams && (pStream->disposition & LAVF_DISPOSITION_SUB_STREAM)) ||
        (pStream->disposition & AV_DISPOSITION_ATTACHED_PIC))
    {
        ARIB_LOG("[ARIB] AddStream skip idx=%d id=%d type=%d codec_id=%d tag=0x%08x discard=%d disp=0x%x substreams=%d\n",
                 pStream->index,
                 pStream->id,
                 pStream->codecpar->codec_type,
                 (int)pStream->codecpar->codec_id,
                 (unsigned)pStream->codecpar->codec_tag,
                 pStream->discard,
                 pStream->disposition,
                 m_bSubStreams ? 1 : 0);
        pStream->discard = AVDISCARD_ALL;
        return S_FALSE;
    }

    stream s;
    s.pid = streamId;

    // Extract language
    const char *lang = nullptr;
    if (AVDictionaryEntry *dictEntry = av_dict_get(pStream->metadata, "language", nullptr, 0))
    {
        lang = dictEntry->value;
    }
    if (lang)
    {
        s.language = ProbeForISO6392(lang);
        s.lcid = ProbeLangForLCID(s.language.c_str());
    }
    else
    {
        s.language = "und";
        s.lcid = 0;
    }
    const char *title = lavf_get_stream_title(pStream);
    if (title)
        s.trackName = title;
    s.streamInfo = new CLAVFStreamInfo(m_avFormat, pStream, m_pszInputFormat, hr);

    if (hr != S_OK)
    {
        delete s.streamInfo;
        pStream->discard = AVDISCARD_ALL;
        return hr;
    }

    switch (pStream->codecpar->codec_type)
    {
    case AVMEDIA_TYPE_VIDEO:
        ARIB_LOG("[ARIB] AddStream video: idx=%d id=%d codec_id=%d\n",
                 pStream->index, pStream->id, (int)pStream->codecpar->codec_id);
        m_streams[video].push_back(s);
        break;
    case AVMEDIA_TYPE_AUDIO:
        ARIB_LOG("[ARIB] AddStream audio: idx=%d id=%d codec_id=%d lang=%s\n",
                 pStream->index, pStream->id, (int)pStream->codecpar->codec_id, s.language.c_str());
        m_streams[audio].push_back(s);
        break;
    case AVMEDIA_TYPE_SUBTITLE:
        ARIB_LOG("[ARIB] AddStream subtitle: idx=%d id=%d codec_id=%d profile=%d disp=0x%x lang=%s extradata=%d\n",
                 pStream->index,
                 pStream->id,
                 (int)pStream->codecpar->codec_id,
                 pStream->codecpar->profile,
                 pStream->disposition,
                 s.language.c_str(),
                 pStream->codecpar->extradata_size);
        // Mark ARIB captions as default so SelectSubtitleStream auto-selects them.
        // MPEG TS typically doesn't set AV_DISPOSITION_DEFAULT on caption streams,
        // but without it the subtitle selector (*:*|d) never matches.
        if (pStream->codecpar->codec_id == AV_CODEC_ID_ARIB_CAPTION)
        {
            if (!(pStream->disposition & AV_DISPOSITION_DEFAULT) && m_streams[subpic].empty())
                pStream->disposition |= AV_DISPOSITION_DEFAULT;
            s.language = "jpn";
            s.lcid = ProbeLangForLCID("jpn");
        }
        m_streams[subpic].push_back(s);
        break;
    default:
        // unsupported stream
        // Normally this should be caught while creating the stream info already.
        delete s.streamInfo;
        return E_FAIL;
    }
    return S_OK;
}

// Pin creation
STDMETHODIMP CLAVFDemuxer::CreateStreams()
{
    DbgLog((LOG_TRACE, 10, L"CLAVFDemuxer::CreateStreams()"));
    CAutoLock lock(m_pLock);

    for (int i = 0; i < countof(m_streams); ++i)
    {
        m_streams[i].Clear();
    }

    m_program = UINT_MAX;

    if (m_avFormat->nb_programs && !m_pBluRay)
    {
        DbgLog(
            (LOG_TRACE, 10, L" -> File has %d programs, trying to detect the correct one..", m_avFormat->nb_programs));
        // Use a scoring system to select the best available program
        // A "good" program at least has a valid video and audio stream
        // We'll try here to detect these streams and decide on the best program
        // Every present stream gets one point, if it appears to be valid, it gets 4
        // Every present video stream has also video resolution score: width x height.
        // Valid video streams have a width and height, valid audio streams have a channel count.
        // Total program bitrate is used as a tiebreaker.
        // We search for "good" program with highest score.
        DWORD dwScore = 0;                       // Stream found: 1, stream valid: 4
        DWORD dwVideoResolutionProgramScore = 0; // Score = width x height
        DWORD dwProgramBitrate = 0;              // Total bitrate of the program
        for (unsigned int i = 0; i < m_avFormat->nb_programs; ++i)
        {
            AVProgram *program = m_avFormat->programs[i];
            if (program->nb_stream_indexes > 0)
            {
                DWORD dwVideoScore = 0;
                DWORD dwVideoResolutionScore = 0;
                DWORD dwAudioScore = 0;
                DWORD dwBitrate = 0;
                for (unsigned k = 0; k < program->nb_stream_indexes; ++k)
                {
                    unsigned streamIdx = program->stream_index[k];
                    AVStream *st = m_avFormat->streams[streamIdx];
                    if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                    {
                        if (st->codecpar->width != 0 && st->codecpar->height != 0)
                        {
                            dwVideoScore = 4;
                            DWORD dwResolutionScore = st->codecpar->width * st->codecpar->height;
                            if (dwResolutionScore > dwVideoResolutionScore)
                                dwVideoResolutionScore = dwResolutionScore;
                        }
                        else if (dwVideoScore == 0)
                            dwVideoScore = 1;
                    }
                    else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && dwAudioScore < 4)
                    {
                        if (st->codecpar->ch_layout.nb_channels != 0)
                            dwAudioScore = 4;
                        else
                            dwAudioScore = 1;
                    }
                }

                AVDictionaryEntry *dict = av_dict_get(program->metadata, "variant_bitrate", nullptr, 0);
                if (dict && dict->value)
                    dwBitrate = atol(dict->value);

                // Check the score of the previously found stream
                // In addition, we always require a valid video stream (or none), a invalid one is not allowed.
                DbgLog((LOG_TRACE, 10,
                        L"  -> Program %d with score: %ld (video), %ld (video resolution), %ld (audio), %ld (bitrate)",
                        i, dwVideoScore, dwVideoResolutionScore, dwAudioScore, dwBitrate));
                DWORD dwVideoAndAudioScore = dwVideoScore + dwAudioScore;
                if (dwVideoScore != 1 &&
                    (dwVideoAndAudioScore > dwScore ||
                     (dwVideoAndAudioScore == dwScore && dwVideoResolutionScore > dwVideoResolutionProgramScore) ||
                     (dwVideoAndAudioScore == dwScore && dwVideoResolutionScore == dwVideoResolutionProgramScore &&
                      dwBitrate > dwProgramBitrate)))
                {
                    dwScore = dwVideoAndAudioScore;
                    dwVideoResolutionProgramScore = dwVideoResolutionScore;
                    dwProgramBitrate = dwBitrate;
                    m_program = i;
                }
            }
        }
        DbgLog((LOG_TRACE, 10, L" -> Using Program %d", m_program));
    }

    // File has programs
    bool bProgram = (m_program < m_avFormat->nb_programs);

    // Discard unwanted programs
    if (bProgram)
    {
        for (unsigned int i = 0; i < m_avFormat->nb_programs; ++i)
        {
            if (i != m_program)
                m_avFormat->programs[i]->discard = AVDISCARD_ALL;
        }
    }

    // Re-compute the overall file duration based on video and audio durations
    int64_t duration = INT64_MIN;
    int64_t st_duration = 0;
    int64_t start_time = INT64_MAX;
    int64_t st_start_time = 0;

    // Number of streams (either in file or in program)
    unsigned int nbIndex = bProgram ? m_avFormat->programs[m_program]->nb_stream_indexes : m_avFormat->nb_streams;

    // File has PGS streams
    bool bHasPGS = false;

    // add streams from selected program, or all streams if no program was selected
    for (unsigned int i = 0; i < nbIndex; ++i)
    {
        int streamIdx = bProgram ? m_avFormat->programs[m_program]->stream_index[i] : i;
        if (S_OK != AddStream(streamIdx))
            continue;

        AVStream *st = m_avFormat->streams[streamIdx];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO || st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            if (st->duration != AV_NOPTS_VALUE)
            {
                st_duration = av_rescale_q(st->duration, st->time_base, AV_RATIONAL_TIMEBASE);
                if (st_duration > duration)
                    duration = st_duration;
            }

            if (st->start_time != AV_NOPTS_VALUE)
            {
                st_start_time = av_rescale_q(st->start_time, st->time_base, AV_RATIONAL_TIMEBASE);
                if (st_start_time < start_time)
                    start_time = st_start_time;
            }
        }
        if (st->codecpar->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE)
            bHasPGS = true;
    }

    if ((m_bTSDiscont || m_avFormat->duration == AV_NOPTS_VALUE) && duration != INT64_MIN)
    {
        DbgLog(
            (LOG_TRACE, 10, L" -> Changing duration to %I64d (from %I64d, diff %.3fs)", duration, m_avFormat->duration,
             m_avFormat->duration == AV_NOPTS_VALUE ? 0.0f
                                                    : (float)(duration - m_avFormat->duration) / (float)AV_TIME_BASE));
        m_avFormat->duration = duration;
    }
    if ((m_bTSDiscont || m_avFormat->start_time == AV_NOPTS_VALUE) && start_time != INT64_MAX)
    {
        DbgLog((LOG_TRACE, 10, L" -> Changing start_time to %I64d (from %I64d, diff %.3fs)", start_time,
                m_avFormat->start_time,
                m_avFormat->start_time == AV_NOPTS_VALUE
                    ? 0.0f
                    : (float)(start_time - m_avFormat->start_time) / (float)AV_TIME_BASE));
        m_avFormat->start_time = start_time;
    }

    if (bHasPGS && m_pSettings->GetPGSForcedStream())
    {
        CreatePGSForcedSubtitleStream();
    }

    if (m_bMPEGTS && m_streams[subpic].empty())
    {
        ARIB_LOG("[ARIB] Create late ARIB subtitle placeholder pin\n");
        CreateLateAribSubtitleStream();
    }

    // Create fake subtitle pin
    if (!m_streams[subpic].empty())
    {
        CreateNoSubtitleStream();
    }

    if (m_bMPEGTS)
    {
        m_bH264MVCCombine = GetH264MVCStreamIndices(m_avFormat, &m_nH264MVCBaseStream, &m_nH264MVCExtensionStream);
    }

    if (m_bMatroska)
    {
        std::list<std::string> pg_sequences;
        for (unsigned i = 0; i < m_avFormat->nb_streams; i++)
        {
            if (m_avFormat->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
            {
                AVDictionaryEntry *e =
                    av_dict_get(m_avFormat->streams[i]->metadata, "3d-plane", nullptr, AV_DICT_IGNORE_SUFFIX);
                if (e && e->value)
                {
                    pg_sequences.push_back(std::string(e->value));
                }
            }
        }

        // export the list of pg sequences
        if (pg_sequences.size() > 0)
        {
            // strip duplicate entries
            pg_sequences.sort();
            pg_sequences.unique();

            size_t size = pg_sequences.size() * 4;
            char *offsets = new char[size];
            offsets[0] = 0;

            // Append all offsets to the string
            for (auto it = pg_sequences.begin(); it != pg_sequences.end(); it++)
            {
                size_t len = strlen(offsets);
                if (len > 0)
                {
                    offsets[len] = ',';
                    len++;
                }
                strcpy_s(offsets + len, size - len, it->c_str());
            }

            av_dict_set(&m_avFormat->metadata, "pg_offset_sequences", offsets, 0);
            delete[] offsets;
        }
    }

    return S_OK;
}

REFERENCE_TIME CLAVFDemuxer::GetStartTime() const
{
    return av_rescale(m_avFormat->start_time, DSHOW_TIME_BASE, AV_TIME_BASE);
}

// Converts the lavf pts timestamp to a DShow REFERENCE_TIME
// Based on DVDDemuxFFMPEG
REFERENCE_TIME CLAVFDemuxer::ConvertTimestampToRT(int64_t pts, int num, int den, int64_t starttime) const
{
    if (pts == (int64_t)AV_NOPTS_VALUE)
    {
        return Packet::INVALID_TIME;
    }

    if (starttime == AV_NOPTS_VALUE)
    {
        if (m_avFormat->start_time != AV_NOPTS_VALUE)
        {
            starttime = av_rescale(m_avFormat->start_time, den, (int64_t)AV_TIME_BASE * num);
        }
        else
        {
            starttime = 0;
        }
    }

    if (starttime != 0)
    {
        pts -= starttime;
    }

    // Let av_rescale do the work, its smart enough to not overflow
    REFERENCE_TIME timestamp = av_rescale(pts, (int64_t)num * DSHOW_TIME_BASE, den);

    return timestamp;
}

// Converts the lavf pts timestamp to a DShow REFERENCE_TIME
// Based on DVDDemuxFFMPEG
int64_t CLAVFDemuxer::ConvertRTToTimestamp(REFERENCE_TIME timestamp, int num, int den, int64_t starttime) const
{
    if (timestamp == Packet::INVALID_TIME)
    {
        return (int64_t)AV_NOPTS_VALUE;
    }

    if (starttime == AV_NOPTS_VALUE)
    {
        if (m_avFormat->start_time != AV_NOPTS_VALUE)
        {
            starttime = av_rescale(m_avFormat->start_time, den, (int64_t)AV_TIME_BASE * num);
        }
        else
        {
            starttime = 0;
        }
    }

    int64_t pts = av_rescale(timestamp, den, (int64_t)num * DSHOW_TIME_BASE);
    if (starttime != 0)
    {
        pts += starttime;
    }

    return pts;
}

HRESULT CLAVFDemuxer::UpdateForcedSubtitleStream(unsigned audio_pid)
{
    if (!m_avFormat || audio_pid >= m_avFormat->nb_streams)
        return E_UNEXPECTED;

    stream *audiost = GetStreams(audio)->FindStream(audio_pid);
    if (!audiost)
        return E_FAIL;

    // Build CSubtitleSelector for this special case
    std::list<CSubtitleSelector> selectors;
    CSubtitleSelector selector;
    selector.audioLanguage = "*";
    selector.subtitleLanguage = audiost->language;
    selector.dwFlagsSet = SUBTITLE_FLAG_PGS;
    selector.dwFlagsNot = 0;
    selectors.push_back(selector);

    selector.subtitleLanguage = "*";
    selectors.push_back(selector);

    const stream *subst = SelectSubtitleStream(selectors, audiost->language);
    if (subst)
    {
        m_ForcedSubStream = subst->pid;

        CStreamList *streams = GetStreams(subpic);
        stream *forced = streams->FindStream(FORCED_SUBTITLE_PID);
        if (forced)
        {
            CMediaType mtype = forced->streamInfo->mtypes.back();
            forced->streamInfo->mtypes.pop_back();
            forced->language = audiost->language;
            forced->lcid = audiost->lcid;

            SUBTITLEINFO *subInfo = (SUBTITLEINFO *)mtype.Format();
            strncpy_s(subInfo->IsoLang, audiost->language.c_str(), 3);
            subInfo->IsoLang[3] = 0;

            forced->streamInfo->mtypes.push_back(mtype);
        }
    }

    return subst ? S_OK : S_FALSE;
}

// Select the best video stream
const CBaseDemuxer::stream *CLAVFDemuxer::SelectVideoStream()
{
    const stream *best = nullptr;
    CStreamList *streams = GetStreams(video);

    std::deque<stream>::iterator it;
    for (it = streams->begin(); it != streams->end(); ++it)
    {
        stream *check = &*it;

        if (!best)
        {
            best = check;
            continue;
        }

        // if the best stream is an unknown codec, prefer any other
        if (m_avFormat->streams[best->pid]->codecpar->codec_id == AV_CODEC_ID_NONE &&
            m_avFormat->streams[check->pid]->codecpar->codec_id != AV_CODEC_ID_NONE)
        {
            best = check;
            continue;
        }

        // prefer default streams
        bool checkDefault = m_avFormat->streams[check->pid]->disposition & AV_DISPOSITION_DEFAULT;
        bool bestDefault = m_avFormat->streams[best->pid]->disposition & AV_DISPOSITION_DEFAULT;

        if (checkDefault != bestDefault)
        {
            if (checkDefault)
                best = check;
            continue;
        }

        uint64_t bestPixels = (uint64_t)m_avFormat->streams[best->pid]->codecpar->width *
                              m_avFormat->streams[best->pid]->codecpar->height;
        uint64_t checkPixels = (uint64_t)m_avFormat->streams[check->pid]->codecpar->width *
                               m_avFormat->streams[check->pid]->codecpar->height;

        int check_nb_f = av_lav_stream_codec_info_nb_frames(m_avFormat->streams[check->pid]);
        int best_nb_f = av_lav_stream_codec_info_nb_frames(m_avFormat->streams[best->pid]);
        if (m_bRM && (check_nb_f > 0 && best_nb_f <= 0))
        {
            best = check;
        }
        else if (m_bMP4 && m_avFormat->streams[check->pid]->nb_frames == 1 && m_avFormat->streams[best->pid]->nb_frames > 1)
        {
            // avoid selecting a video stream with only one frame
        }
        else if (m_bMP4 && m_avFormat->streams[best->pid]->nb_frames == 1 && m_avFormat->streams[check->pid]->nb_frames > 1)
        {
            // prefer a stream with more then one frame, if available
            best = check;
        }
        else if (!m_bRM || check_nb_f > 0)
        {
            if (checkPixels > bestPixels)
            {
                best = check;
            }
            else if (checkPixels == bestPixels)
            {
                int64_t best_rate = m_avFormat->streams[best->pid]->codecpar->bit_rate;
                int64_t check_rate = m_avFormat->streams[check->pid]->codecpar->bit_rate;
                if (best_rate && check_rate && check_rate > best_rate)
                    best = check;
            }
        }
    }

    return best;
}

static int audio_codec_priority(const AVCodecParameters *par)
{
    int priority = 0;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);

    // lossless codecs have highest priority
    if (desc && ((desc->props & (AV_CODEC_PROP_LOSSLESS | AV_CODEC_PROP_LOSSY)) == AV_CODEC_PROP_LOSSLESS))
    {
        priority = 10;
    }
    else if (desc && (desc->props & AV_CODEC_PROP_LOSSLESS))
    {
        priority = 8;

        if (par->codec_id == AV_CODEC_ID_DTS)
        {
            priority = 7;

            if (par->profile == AV_PROFILE_DTS_EXPRESS)
            {
                priority -= 1;
            }
            else if (par->profile == AV_PROFILE_DTS_HD_MA)
            {
                priority += 3;
            }
            else if (par->profile == AV_PROFILE_DTS_HD_HRA)
            {
                priority += 2;
            }
            else if (par->profile >= AV_PROFILE_DTS_ES)
            {
                priority += 1;
            }
        }
    }
    else
    {
        switch (par->codec_id)
        {
        case AV_CODEC_ID_EAC3: priority = 7; break;
        case AV_CODEC_ID_AC3:
        case AV_CODEC_ID_AAC:
        case AV_CODEC_ID_AAC_LATM: priority = 5; break;
        case AV_CODEC_ID_MP3: priority = 3; break;
        }

        // WAVE_FORMAT_EXTENSIBLE is multi-channel PCM, which doesn't have a proper tag otherwise
        if (par->codec_tag == WAVE_FORMAT_EXTENSIBLE)
        {
            priority = 10;
        }
    }

    // low priority for S302M with non-pcm content
    if (par->codec_id == AV_CODEC_ID_S302M && par->codec_tag != -1)
        priority = -1;

    return priority;
}

// Select the best audio stream
const CBaseDemuxer::stream *CLAVFDemuxer::SelectAudioStream(std::list<std::string> prefLanguages)
{
    const stream *best = nullptr;
    CStreamList *streams = GetStreams(audio);

    std::deque<stream *> checkedStreams;

    // Filter for language
    if (!prefLanguages.empty())
    {
        std::list<std::string>::iterator it;
        for (it = prefLanguages.begin(); it != prefLanguages.end(); ++it)
        {
            std::string checkLanguage = ProbeForISO6392(it->c_str());
            std::deque<stream>::iterator sit;
            for (sit = streams->begin(); sit != streams->end(); ++sit)
            {
                std::string language = sit->language;
                // check if the language matches
                if (language == checkLanguage)
                {
                    checkedStreams.push_back(&*sit);
                }
            }
            // First language that has any streams is a match
            if (!checkedStreams.empty())
            {
                break;
            }
        }
    }

    // If no language was set, or no matching streams were found
    // Put all streams in there
    if (checkedStreams.empty())
    {
        std::deque<stream>::iterator sit;
        for (sit = streams->begin(); sit != streams->end(); ++sit)
        {
            checkedStreams.push_back(&*sit);
        }
    }

    // Check for a stream with a default flag
    // If in our current set is one, that one prevails
    std::deque<stream *>::iterator sit;
    for (sit = checkedStreams.begin(); sit != checkedStreams.end(); ++sit)
    {
        if (m_avFormat->streams[(*sit)->pid]->disposition & AV_DISPOSITION_DEFAULT)
        {
            best = *sit;
            break;
        }
    }

    BOOL bCheckQuality = m_pSettings->GetPreferHighQualityAudioStreams();
    BOOL bImpaired = m_pSettings->GetUseAudioForHearingVisuallyImpaired();
#define DISPO_IMPAIRED (AV_DISPOSITION_HEARING_IMPAIRED | AV_DISPOSITION_VISUAL_IMPAIRED)

    if (!best && !checkedStreams.empty())
    {
        // If only one stream is left, just use that one
        if (checkedStreams.size() == 1)
        {
            best = checkedStreams.at(0);
        }
        else
        {
            // Check for quality
            std::deque<stream *>::iterator sit;
            for (sit = checkedStreams.begin(); sit != checkedStreams.end(); ++sit)
            {
                if (!best)
                {
                    best = *sit;
                    continue;
                }
                AVStream *old_stream = m_avFormat->streams[best->pid];
                AVStream *new_stream = m_avFormat->streams[(*sit)->pid];

                // ignore streams with an unknown codec or no decoder
                if (new_stream->codecpar->codec_id == AV_CODEC_ID_NONE || avcodec_find_decoder(new_stream->codecpar->codec_id) == NULL)
                    continue;
                else if (old_stream->codecpar->codec_id == AV_CODEC_ID_NONE || avcodec_find_decoder(old_stream->codecpar->codec_id) == NULL)
                {
                    best = *sit;
                    continue;
                }

                int check_nb_f = av_lav_stream_codec_info_nb_frames(new_stream);
                int best_nb_f = av_lav_stream_codec_info_nb_frames(old_stream);
                if (m_bRM && (check_nb_f > 0 && best_nb_f <= 0))
                {
                    best = *sit;
                }
                else if (!m_bRM || check_nb_f > 0)
                {
                    if (!(old_stream->disposition & DISPO_IMPAIRED) != !(new_stream->disposition & DISPO_IMPAIRED))
                    {
                        if ((bImpaired && !(old_stream->disposition & DISPO_IMPAIRED)) ||
                            (!bImpaired && !(new_stream->disposition & DISPO_IMPAIRED)))
                        {
                            best = *sit;
                        }
                        continue;
                    }
                    if (!bCheckQuality)
                        continue;

                    // First, check number of channels
                    int old_num_chans = old_stream->codecpar->ch_layout.nb_channels;
                    int new_num_chans = new_stream->codecpar->ch_layout.nb_channels;
                    if (new_num_chans > old_num_chans)
                    {
                        best = *sit;
                    }
                    else if (new_num_chans == old_num_chans)
                    {
                        // Same number of channels, check codec
                        int old_priority = audio_codec_priority(old_stream->codecpar);
                        int new_priority = audio_codec_priority(new_stream->codecpar);
                        if (new_priority > old_priority)
                        {
                            best = *sit;
                        }
                    }
                }
            }
        }
    }

    return best;
}

static inline bool does_language_match(std::string selector, std::string selectee)
{
    return (selector == "*" || selector == selectee);
}

// ugly hack to only convert ascii to lower case, as there is no proper unicode function for utf-8 in std::string
static inline char asciitolower(char in)
{
    if (in <= 'Z' && in >= 'A')
        return in - ('Z' - 'z');
    return in;
}

// Select the best subtitle stream
const CBaseDemuxer::stream *CLAVFDemuxer::SelectSubtitleStream(std::list<CSubtitleSelector> subtitleSelectors,
                                                               std::string audioLanguage)
{
    const stream *best = nullptr;
    CStreamList *streams = GetStreams(subpic);

    std::deque<stream *> checkedStreams;

    std::list<CSubtitleSelector>::iterator it = subtitleSelectors.begin();
    for (it = subtitleSelectors.begin(); it != subtitleSelectors.end() && checkedStreams.empty(); it++)
    {

        if (!does_language_match(it->audioLanguage, audioLanguage))
            continue;

        if (it->subtitleLanguage == "off")
            break;

        // lower-case version of the trackname query
        std::string subtitleTrackNameQueryLower = it->subtitleTrackName;
        if (subtitleTrackNameQueryLower.empty() == false)
            std::transform(subtitleTrackNameQueryLower.begin(), subtitleTrackNameQueryLower.end(),
                           subtitleTrackNameQueryLower.begin(), asciitolower);

        std::deque<stream>::iterator sit;
        for (sit = streams->begin(); sit != streams->end(); sit++)
        {
            if (sit->pid == NO_SUBTITLE_PID)
                continue;



            if (!subtitleTrackNameQueryLower.empty())
            {
                // create lowercase version of the track name
                std::string trackNameLower = sit->trackName;
                std::transform(trackNameLower.begin(), trackNameLower.end(), trackNameLower.begin(), asciitolower);

                if (trackNameLower.find(subtitleTrackNameQueryLower) == std::string::npos)
                    continue;
            }

            if (sit->pid == FORCED_SUBTITLE_PID)
            {
                if ((it->dwFlagsSet == 0 || it->dwFlagsSet & SUBTITLE_FLAG_VIRTUAL) &&
                    does_language_match(it->subtitleLanguage, audioLanguage))
                    checkedStreams.push_back(&*sit);
                continue;
            }

            if (sit->pid == LATE_ARIB_SUBTITLE_PID)
            {
                const bool flagsSetMatch =
                    it->dwFlagsSet == 0 || (it->dwFlagsSet & SUBTITLE_FLAG_DEFAULT);
                const bool flagsNotMatch =
                    !((it->dwFlagsNot & SUBTITLE_FLAG_DEFAULT) ||
                      (it->dwFlagsNot & SUBTITLE_FLAG_FORCED) ||
                      (it->dwFlagsNot & SUBTITLE_FLAG_IMPAIRED) ||
                      (it->dwFlagsNot & SUBTITLE_FLAG_NORMAL));
                if (flagsSetMatch && flagsNotMatch && does_language_match(it->subtitleLanguage, sit->language))
                    checkedStreams.push_back(&*sit);
                continue;
            }

            bool streamIsDefault = m_avFormat->streams[sit->pid]->disposition & AV_DISPOSITION_DEFAULT;
            bool streamIsForced = m_avFormat->streams[sit->pid]->disposition & AV_DISPOSITION_FORCED;
            bool streamIsImpaired = m_avFormat->streams[sit->pid]->disposition &
                                    (AV_DISPOSITION_HEARING_IMPAIRED | AV_DISPOSITION_VISUAL_IMPAIRED);
            bool streamIsNormal = !streamIsDefault && !streamIsForced && !streamIsImpaired;
            bool streamIsPgsFormat =
                (m_avFormat->streams[sit->pid]->codecpar->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE);

            bool flagsSetMatch =
                ((it->dwFlagsSet == 0) ||
                 ((it->dwFlagsSet & SUBTITLE_FLAG_DEFAULT) && streamIsDefault) ||
                 ((it->dwFlagsSet & SUBTITLE_FLAG_FORCED) && streamIsForced) ||
                 ((it->dwFlagsSet & SUBTITLE_FLAG_IMPAIRED) && streamIsImpaired) ||
                 ((it->dwFlagsSet & SUBTITLE_FLAG_PGS) && streamIsPgsFormat) ||
                 ((it->dwFlagsSet & SUBTITLE_FLAG_NORMAL) && streamIsNormal)
                );

            bool flagsNotMatch =
                (!((it->dwFlagsNot & SUBTITLE_FLAG_DEFAULT) && streamIsDefault) &&
                 !((it->dwFlagsNot & SUBTITLE_FLAG_FORCED) && streamIsForced) &&
                 !((it->dwFlagsNot & SUBTITLE_FLAG_IMPAIRED) && streamIsImpaired) &&
                 !((it->dwFlagsNot & SUBTITLE_FLAG_NORMAL) && streamIsNormal)
                );

            if (flagsSetMatch && flagsNotMatch)
            {
                std::string streamLanguage = sit->language;
                if (does_language_match(it->subtitleLanguage, streamLanguage))
                    checkedStreams.push_back(&*sit);
            }
        }
    }

    if (!checkedStreams.empty())
        best = streams->FindStream(checkedStreams.front()->pid);
    else
        best = streams->FindStream(NO_SUBTITLE_PID);

    return best;
}

#include "libavformat/isom.h"

STDMETHODIMP_(DWORD) CLAVFDemuxer::GetStreamFlags(DWORD dwStream)
{
    if (!m_avFormat || dwStream >= m_avFormat->nb_streams)
        return 0;

    DWORD dwFlags = 0;
    AVStream *st = m_avFormat->streams[dwStream];

    if (strcmp(m_pszInputFormat, "rawvideo") == 0)
        dwFlags |= LAV_STREAM_FLAG_ONLY_DTS;

    if (st->codecpar->codec_id == AV_CODEC_ID_H264 &&
        (m_bAVI || m_bPMP || (m_bMatroska && (!st->codecpar->extradata_size || st->codecpar->extradata[0] != 1)) ||
         (m_bMP4 && st->priv_data && ((MOVStreamContext *)st->priv_data)->ctts_count == 0)))
        dwFlags |= LAV_STREAM_FLAG_ONLY_DTS;

    if (st->codecpar->codec_id == AV_CODEC_ID_HEVC &&
        (m_bAVI || (m_bMP4 && st->priv_data && ((MOVStreamContext *)st->priv_data)->ctts_count == 0)))
        dwFlags |= LAV_STREAM_FLAG_ONLY_DTS;

    if (st->codecpar->codec_id == AV_CODEC_ID_VVC &&
        (m_bAVI || (m_bMP4 && st->priv_data && ((MOVStreamContext *)st->priv_data)->ctts_count == 0)))
        dwFlags |= LAV_STREAM_FLAG_ONLY_DTS;

    if (m_bMatroska && (st->codecpar->codec_id == AV_CODEC_ID_RV30 || st->codecpar->codec_id == AV_CODEC_ID_RV40))
        dwFlags |= LAV_STREAM_FLAG_RV34_MKV;

    if (m_avFormat->flags & AVFMT_FLAG_NETWORK)
        dwFlags |= LAV_STREAM_FLAG_LIVE;

    return dwFlags;
}

STDMETHODIMP_(int) CLAVFDemuxer::GetPixelFormat(DWORD dwStream)
{
    if (!m_avFormat || dwStream >= m_avFormat->nb_streams)
        return AV_PIX_FMT_NONE;

    return m_avFormat->streams[dwStream]->codecpar->format;
}

STDMETHODIMP_(int) CLAVFDemuxer::GetHasBFrames(DWORD dwStream)
{
    if (!m_avFormat || dwStream >= m_avFormat->nb_streams)
        return -1;

    return m_avFormat->streams[dwStream]->codecpar->video_delay;
}

STDMETHODIMP CLAVFDemuxer::GetSideData(DWORD dwStream, GUID guidType, const BYTE **pData, size_t *pSize)
{
    if (!m_avFormat || dwStream >= m_avFormat->nb_streams)
        return E_INVALIDARG;

    if (guidType == IID_MediaSideDataFFMpeg)
    {
        CBaseDemuxer::stream *pStream = FindStream(dwStream);
        if (!pStream)
            return E_FAIL;

        pStream->SideData.side_data = m_avFormat->streams[dwStream]->codecpar->coded_side_data;
        pStream->SideData.side_data_elems = m_avFormat->streams[dwStream]->codecpar->nb_coded_side_data;
        *pData = (BYTE *)&pStream->SideData;
        *pSize = sizeof(pStream->SideData);

        return S_OK;
    }

    return E_INVALIDARG;
}

STDMETHODIMP CLAVFDemuxer::GetBSTRMetadata(const char *key, BSTR *pbstrValue, int stream)
{
    if (!m_avFormat)
        return VFW_E_NOT_FOUND;

    if (stream >= (int)m_avFormat->nb_streams)
        return E_INVALIDARG;

    if (_stricmp(key, "rotate") == 0 && stream >= 0)
    {
        const AVPacketSideData *sd = av_packet_side_data_get(
            m_avFormat->streams[stream]->codecpar->coded_side_data,
            m_avFormat->streams[stream]->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);

        if (sd && sd->size == (9*sizeof(int32_t)))
        {
            double dRotation = av_display_rotation_get((const int32_t *)sd->data);
            int nRotation = -lrint(dRotation);

            // normalize rotation to 0-360
            while (nRotation < 0)
                nRotation += 360;

            while (nRotation >= 360)
                nRotation -= 360;

            char buf[34] = {0};
            sprintf_s(buf, "%d", nRotation);
            *pbstrValue = ConvertCharToBSTR(buf);
            if (*pbstrValue == nullptr)
                return E_OUTOFMEMORY;

            return S_OK;
        }
    }

    AVDictionaryEntry *entry = av_dict_get(stream >= 0 ? m_avFormat->streams[stream]->metadata : m_avFormat->metadata,
                                           key, nullptr, AV_DICT_IGNORE_SUFFIX);
    if (!entry || !entry->value || entry->value[0] == '\0')
        return VFW_E_NOT_FOUND;

    *pbstrValue = ConvertCharToBSTR(entry->value);
    if (*pbstrValue == nullptr)
        return E_OUTOFMEMORY;

    return S_OK;
}
