#include "stdafx.h"
#include "LAVAudio.h"
#include "moreuuids.h"
#include <atomic>
#include <thread>
#include <cstdio>
#include <stdexcept>
static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
struct AudioTest : CLAVAudio
{
    AudioTest(HRESULT *hr)
        : CLAVAudio(nullptr, hr)
    {
    }
    void lockReceive() { m_csReceive.Lock(); }
    void unlockReceive() { m_csReceive.Unlock(); }
    void checkDecoderSelection(bool requireApplied = false)
    {
        CAutoLock receive(&m_csReceive);
        CAutoLock selection(&m_csDualMono);
        if (!IsDualMonoCapable())
            return;
        int64_t applied = -1;
        check(m_pAVCtx && m_pAVCtx->priv_data &&
                  av_opt_get_int(m_pAVCtx->priv_data, "dual_mono_mode", 0, &applied) >= 0,
              "read actual AAC decoder selection");
        check(applied == m_settings.DualMonoMode || (!requireApplied && m_bDualMonoPending),
              "latest selection is applied or still pending, never lost");
        if (requireApplied)
            check(!m_bDualMonoPending, "applied selection is acknowledged");
    }
    void applyPendingSelection()
    {
        CAutoLock receive(&m_csReceive);
        DWORD mode;
        if (TakePendingDualMonoMode(&mode))
            ApplyDualMonoMode(mode);
    }
};
static CMediaType media(bool aac)
{
    CMediaType mt;
    mt.SetType(&MEDIATYPE_Audio);
    mt.SetSubtype(aac ? &MEDIASUBTYPE_RAW_AAC1 : &MEDIASUBTYPE_PCM_SOWT);
    mt.SetFormatType(&FORMAT_WaveFormatEx);
    auto *wf = reinterpret_cast<WAVEFORMATEX *>(mt.AllocFormatBuffer(sizeof(WAVEFORMATEX)));
    *wf = {};
    wf->wFormatTag = aac ? 0x00ff : WAVE_FORMAT_PCM;
    wf->nChannels = 2;
    wf->nSamplesPerSec = 48000;
    wf->wBitsPerSample = aac ? 0 : 16;
    wf->nBlockAlign = aac ? 1 : 4;
    wf->nAvgBytesPerSec = aac ? 16000 : 192000;
    return mt;
}
int wmain(int argc, wchar_t **argv)
{
    try
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        check(argc == 2, "provide audio DLL path");
        HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        check(dll != nullptr, "load built audio DLL");
        auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, void **)>(
            GetProcAddress(dll, "DllGetClassObject"));
        check(getClass != nullptr, "audio class factory export");
        CComPtr<IClassFactory> factory;
        check(SUCCEEDED(getClass(__uuidof(CLAVAudio), IID_IClassFactory, reinterpret_cast<void **>(&factory))),
              "audio class factory");
        CComPtr<ILAVAudioSettings> settings;
        check(SUCCEEDED(
                  factory->CreateInstance(nullptr, __uuidof(ILAVAudioSettings), reinterpret_cast<void **>(&settings))),
              "audio settings interface");
        settings->SetRuntimeConfig(TRUE);
        CComPtr<ILAVAudioDualMono> modes;
        check(SUCCEEDED(settings->QueryInterface(&modes)), "built audio dual mono interface");
        check(SUCCEEDED(modes->SetDualMonoMode(DualMono_Sub)) && modes->GetDualMonoMode() == DualMono_Sub,
              "built audio mode control");
        HRESULT hr = S_OK;
        AudioTest audio(&hr);
        check(SUCCEEDED(hr), "construct audio filter");
        audio.SetRuntimeConfig(TRUE);
        auto aac = media(true), pcm = media(false);
        check(SUCCEEDED(audio.SetDualMonoMode(DualMono_Main)), "select main before initialization");
        check(SUCCEEDED(audio.SetMediaType(PINDIR_INPUT, &aac)), "initialize AAC");
        audio.checkDecoderSelection(true);
        HANDLE started = CreateEvent(nullptr, TRUE, FALSE, nullptr), done = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        audio.lockReceive();
        std::thread blocked([&] {
            SetEvent(started);
            audio.SetDualMonoMode(DualMono_Sub);
            SetEvent(done);
        });
        WaitForSingleObject(started, INFINITE);
        // The setter must NOT wait for the receive lock. It is called on the
        // player's message thread (property page, IAMStreamSelect::Enable),
        // and a renderer holds Receive() until the sample's presentation time,
        // so waiting here deadlocks the whole player. The decoder change is
        // deferred to the top of the next Receive() instead.
        bool prompt = WaitForSingleObject(done, 5000) == WAIT_OBJECT_0;
        audio.unlockReceive();
        blocked.join();
        CloseHandle(started);
        CloseHandle(done);
        check(prompt, "dual mono setter must not wait for the receive lock");
        check(audio.GetDualMonoMode() == DualMono_Sub, "mode published while receive is held");
        audio.checkDecoderSelection();
        audio.applyPendingSelection();
        audio.checkDecoderSelection(true);
        std::atomic<bool> ok{true};
        std::thread changes([&] {
            for (int i = 0; i < 5000; ++i)
            {
                if (FAILED(audio.SetDualMonoMode(i % 3)))
                {
                    fprintf(stderr, "mode failed\n");
                    ok = false;
                }
                DWORD count = 0;
                if (FAILED(audio.Count(&count)) || (count != 0 && count != 3))
                {
                    fprintf(stderr, "count failed: %lu\n", count);
                    ok = false;
                }
                WCHAR *name = nullptr;
                HRESULT info = audio.Info(i % 3, nullptr, nullptr, nullptr, nullptr, &name, nullptr, nullptr);
                if (FAILED(info) && info != E_NOTIMPL)
                {
                    fprintf(stderr, "info failed: %08lx\n", info);
                    ok = false;
                }
                CoTaskMemFree(name);
            }
        });
        for (int i = 0; i < 1000; ++i)
        {
            HRESULT h = audio.SetMediaType(PINDIR_INPUT, i % 2 ? &aac : &pcm);
            if (FAILED(h))
            {
                fprintf(stderr, "init %d failed: %08lx\n", i, h);
                ok = false;
                break;
            }
            // Check the decoder, not just the public setting: reinitialization
            // must not clear a newer request while applying an older snapshot.
            audio.checkDecoderSelection();
        }
        changes.join();
        check(ok, "concurrent mode changes and decoder reinitialization");
        check(SUCCEEDED(audio.SetMediaType(PINDIR_INPUT, &aac)), "initialize final AAC decoder");
        audio.checkDecoderSelection(true);
        check(SUCCEEDED(audio.SetDualMonoMode(DualMono_Main)), "select main after reinitialization");
        audio.applyPendingSelection();
        audio.checkDecoderSelection(true);
        check(audio.SetDualMonoMode(3) == E_INVALIDARG, "invalid mode rejected");
        puts("PASS: setter never waits for receive; decoder selection is applied or pending across "
             "5000 mode changes and 1000 decoder reinitializations");
        return 0;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
