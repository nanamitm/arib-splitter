// Dump the ARIB caption events the demuxer hands to the subtitle pin, one line
// per ASS event, for comparing caption timing between builds without a player.
//
// Each line gives the event's start and stop, the latest A/V start time read
// when it was delivered (how far the demuxer had read ahead of the caption),
// the payload size and the ASS event itself. Caption settings and the ARIB debug
// log come from arib_dump.ini next to the executable; tools\arib_dump.ps1
// writes it.
#include "stdafx.h"
#include <initguid.h>
#include "moreuuids.h"
#include <cstdio>
#include <string>
#include <stdexcept>
#include "BaseDemuxer.h"
#include "LAVSplitterSettingsInternal.h"
#include "IKeyFrameInfo.h"
#include "ITrackInfo.h"
#include "FontInstaller.h"
#include "DSMResourceBag.h"
#include "LAVFDemuxer.h"

CFactoryTemplate g_Templates[1] = {};
int g_cTemplates = 0;

static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

int wmain(int argc, wchar_t **argv)
{
    try
    {
        check(argc >= 3, "usage: arib_dump <ARIBSplitter.ax> <file.ts> [maxSeconds]");
        const double maxSeconds = (argc >= 4) ? _wtof(argv[3]) : 120.0;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        // The splitter DLL only provides the settings object; the demuxer itself
        // is the one linked into this executable.
        HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        check(dll != nullptr, "load splitter DLL");
        auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, void **)>(
            GetProcAddress(dll, "DllGetClassObject"));
        check(getClass != nullptr, "DllGetClassObject");
        CLSID clsid;
        CLSIDFromString(L"{DB05F97C-F39C-417C-8011-33D093234F1A}", &clsid);
        CComPtr<IClassFactory> factory;
        check(SUCCEEDED(getClass(clsid, IID_IClassFactory, reinterpret_cast<void **>(&factory))), "class factory");
        CComPtr<ILAVFSettingsInternal> settings;
        check(SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(ILAVFSettingsInternal),
                                                reinterpret_cast<void **>(&settings))),
              "settings interface");
        settings->SetRuntimeConfig(TRUE);

        CCritSec lock;
        CLAVFDemuxer *demuxer = new CLAVFDemuxer(&lock, settings);
        demuxer->AddRef();
        check(SUCCEEDED(demuxer->Open(argv[2])), "open input");

        // Select the first stream of each type, as a player would by default.
        for (int type = 0; type < CBaseDemuxer::unknown; ++type)
        {
            auto *streams = demuxer->GetStreams(static_cast<CBaseDemuxer::StreamType>(type));
            fprintf(stderr, "type %d: %zu streams\n", type, streams->size());
            for (const auto &s : *streams)
                fprintf(stderr, "   pid=%u name=%s lang=%s\n", s.pid, s.trackName.c_str(), s.language.c_str());
            if (!streams->empty())
                demuxer->SetActiveStream(static_cast<CBaseDemuxer::StreamType>(type), (int)streams->front().pid);
        }

        const REFERENCE_TIME limit = static_cast<REFERENCE_TIME>(maxSeconds * 10000000.0);
        REFERENCE_TIME latestAV = 0;
        for (;;)
        {
            Packet *p = nullptr;
            const HRESULT hr = demuxer->GetNextPacket(&p);
            if (FAILED(hr))
                break;
            if (hr != S_OK || !p)
                continue;

            bool isSubtitle = false;
            for (const auto &s : *demuxer->GetStreams(CBaseDemuxer::subpic))
                if (s.pid == p->StreamId)
                    isSubtitle = true;
            if (isSubtitle)
            {
                std::string payload(reinterpret_cast<const char *>(p->GetData()), p->GetDataSize());
                for (auto &c : payload)
                    if (c == '\n' || c == '\r')
                        c = ' ';
                printf("SUB start=%lld stop=%lld av=%lld len=%d |%s\n", (long long)p->rtStart, (long long)p->rtStop,
                       (long long)latestAV, p->GetDataSize(), payload.c_str());
            }
            else if (p->rtStart != Packet::INVALID_TIME && p->rtStart > latestAV)
            {
                latestAV = p->rtStart;
            }

            const REFERENCE_TIME rt = p->rtStart;
            delete p;
            if (rt != Packet::INVALID_TIME && rt > limit)
                break;
        }
        demuxer->Release();
        return 0;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
