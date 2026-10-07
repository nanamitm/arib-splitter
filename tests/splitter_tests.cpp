// Run the production input callbacks with deterministic IAsyncReader data.
#include "stdafx.h"
#include <initguid.h>
#include <qnetwork.h>
#include "LAVSplitter.h"
#include "InputPin.h"
#include "OutputPin.h"
#include "moreuuids.h"
#include "IGraphRebuildDelegate.h"
#include "IMediaSideDataFFmpeg.h"
#include "ILAVDynamicAllocator.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>

CFactoryTemplate g_Templates[1] = {};
int g_cTemplates = 0;

static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}

struct Reader : IAsyncReader
{
    const std::vector<BYTE> data{0x47, 1, 2, 3, 4, 5, 6};
    bool reliableLength = true;
    std::vector<LONGLONG> singleByteReads;

    STDMETHODIMP QueryInterface(REFIID iid, void **object) override
    {
        if (!object)
            return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != IID_IAsyncReader)
            return E_NOINTERFACE;
        *object = static_cast<IAsyncReader *>(this);
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP RequestAllocator(IMemAllocator *, ALLOCATOR_PROPERTIES *, IMemAllocator **) override
    {
        return E_NOTIMPL;
    }
    STDMETHODIMP Request(IMediaSample *, DWORD_PTR) override { return E_NOTIMPL; }
    STDMETHODIMP WaitForNext(DWORD, IMediaSample **, DWORD_PTR *) override { return E_NOTIMPL; }
    STDMETHODIMP SyncReadAligned(IMediaSample *) override { return E_NOTIMPL; }
    STDMETHODIMP SyncRead(LONGLONG pos, LONG size, BYTE *buffer) override
    {
        if (size == 1)
            singleByteReads.push_back(pos);
        if (pos < 0 || size < 0)
            return E_INVALIDARG;
        if (pos >= static_cast<LONGLONG>(data.size()))
            return S_FALSE;
        const size_t count = (std::min)(static_cast<size_t>(size), data.size() - static_cast<size_t>(pos));
        memcpy(buffer, data.data() + pos, count);
        return count == static_cast<size_t>(size) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Length(LONGLONG *total, LONGLONG *available) override
    {
        if (!reliableLength)
            return E_NOTIMPL;
        *total = *available = data.size();
        return S_OK;
    }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
};

struct InputPinTest : CLAVInputPin
{
    InputPinTest(CLAVSplitter *filter, Reader *reader, HRESULT *hr)
        : CLAVInputPin(NAME("test input"), filter, filter, hr)
    {
        m_pAsyncReader = reader;
    }
    ~InputPinTest() { m_pAsyncReader = nullptr; }
    int read(BYTE *buffer, int size) { return Read(this, buffer, size); }
    int64_t seek(int64_t offset, int whence) { return Seek(this, offset, whence); }
    LONGLONG position() const { return m_llPos; }
};

static void inputTests(CLAVSplitter &filter)
{
    HRESULT hr = S_OK;
    Reader reader;
    InputPinTest pin(&filter, &reader, &hr);
    check(SUCCEEDED(hr), "construct input pin");
    BYTE buffer[16] = {};
    check(pin.seek(0, SEEK_SET) == 0, "seek to start");
    check(pin.read(buffer, 3) == 3 && memcmp(buffer, reader.data.data(), 3) == 0, "complete read");
    check(pin.read(buffer, sizeof(buffer)) == 4 && memcmp(buffer, reader.data.data() + 3, 4) == 0,
          "partial read with reliable length");
    check(pin.read(buffer, sizeof(buffer)) == AVERROR_EOF, "EOF after reliable partial read");

    check(pin.seek(2, SEEK_SET) == 2, "seek for fallback");
    reader.reliableLength = false;
    check(pin.read(buffer, sizeof(buffer)) == 5 && memcmp(buffer, reader.data.data() + 2, 5) == 0,
          "fallback reads consecutive bytes rather than repeating one");
    check(reader.singleByteReads == std::vector<LONGLONG>({2, 3, 4, 5, 6, 7}), "fallback checks EOF once");
    check(pin.position() == 7, "fallback advances by actual bytes");
    check(pin.read(buffer, sizeof(buffer)) == AVERROR_EOF, "fallback EOF");

    reader.reliableLength = true;
    check(pin.seek(-3, SEEK_END) == 4, "negative end offset seeks before EOF");
    check(pin.read(buffer, 3) == 3 && memcmp(buffer, reader.data.data() + 4, 3) == 0, "read actual tail bytes");
    check(pin.seek(0, SEEK_END) == 7, "zero end offset seeks to EOF");
    check(pin.seek(3, SEEK_END) == 7, "positive end offset clamps at EOF");
    check(pin.seek(-100, SEEK_END) == 0, "end offset clamps at start");
    check(pin.seek(2, SEEK_CUR) == 2, "relative seek still works");
    check(pin.seek(0, AVSEEK_SIZE) == 7 && pin.position() == 2, "size query preserves position");
    puts("PASS: input complete/partial reads, unreliable length, EOF, signed tail seeks");
}

struct SeekDemuxer : CBaseDemuxer
{
    std::atomic<HRESULT> seekResult{S_OK};
    std::atomic<int> seeks{0}, reads{0};
    CAMEvent readDone{TRUE};
    explicit SeekDemuxer(CCritSec *lock) : CBaseDemuxer(NAME("seek test"), lock) {}
    STDMETHODIMP Open(LPCOLESTR, LPCOLESTR, LPCOLESTR) override { return S_OK; }
    REFERENCE_TIME GetDuration() const override { return 100000000; }
    STDMETHODIMP GetNextPacket(Packet **packet) override
    {
        ++reads;
        *packet = nullptr;
        readDone.Set();
        return E_FAIL;
    }
    STDMETHODIMP Seek(REFERENCE_TIME) override
    {
        ++seeks;
        return seekResult.load();
    }
    STDMETHODIMP Reset() override { return S_OK; }
    const char *GetContainerFormat() const override { return "mpegts"; }
    const stream *SelectVideoStream() override { return nullptr; }
    const stream *SelectAudioStream(std::list<std::string>) override { return nullptr; }
    const stream *SelectSubtitleStream(std::list<CSubtitleSelector>, std::string) override { return nullptr; }
};

struct SplitterTest : CLAVSplitter
{
    using CLAVSplitter::SetPositionsInternal;
    explicit SplitterTest(HRESULT *hr) : CLAVSplitter(nullptr, hr) { SetRuntimeConfig(TRUE); }
    void start(SeekDemuxer *demuxer)
    {
        demuxer->AddRef();
        m_pDemuxer = demuxer;
        m_ePlaybackInit.Reset();
        check(Create(), "create demux worker");
        wait();
    }
    void wait() { check(m_ePlaybackInit.Wait(5000), "demux worker finishes seek or startup"); }
    bool retained(REFERENCE_TIME start, REFERENCE_TIME stop, BOOL stopValid) const
    {
        return m_rtStart == start && m_rtCurrent == start && m_rtNewStart == start && m_rtStop == stop &&
               m_rtNewStop == stop && m_bStopValid == stopValid;
    }
};

static void seekTests()
{
    HRESULT hr = S_OK;
    SplitterTest filter(&hr);
    check(SUCCEEDED(hr), "construct seek test splitter");
    auto *demuxer = new SeekDemuxer(&filter);
    filter.start(demuxer);
    check(demuxer->readDone.Wait(5000), "initial read finishes");
    demuxer->readDone.Reset();
    LONGLONG current = 10000000, stop = 90000000;
    check(filter.SetPositions(&current, AM_SEEKING_AbsolutePositioning, &stop, AM_SEEKING_AbsolutePositioning) == S_OK,
          "successful worker seek reaches caller");
    filter.wait();
    check(demuxer->readDone.Wait(5000), "successful seek resumes packet reading");
    check(filter.retained(current, stop, TRUE), "successful seek publishes new segment state");
    demuxer->seekResult = E_ACCESSDENIED;
    const int reads = demuxer->reads;
    current = 20000000;
    stop = 80000000;
    check(filter.SetPositions(&current, AM_SEEKING_AbsolutePositioning, &stop, AM_SEEKING_AbsolutePositioning) ==
              E_ACCESSDENIED,
          "worker seek failure reaches IMediaSeeking caller");
    filter.wait();
    check(filter.retained(10000000, 90000000, TRUE), "failed seek restores position and stop state");
    check(demuxer->reads == reads, "failed seek does not read packets for a false new segment");
    const int seeks = demuxer->seeks;
    int otherCaller = 0;
    check(filter.SetPositionsInternal(&otherCaller, &current, AM_SEEKING_AbsolutePositioning, &stop,
                                     AM_SEEKING_AbsolutePositioning) == E_ACCESSDENIED,
          "another caller does not see cached success for a failed seek");
    filter.wait();
    check(demuxer->seeks == seeks + 1, "failed seek is retried");
    demuxer->seekResult = S_OK;
    demuxer->readDone.Reset();
    check(filter.SetPositions(&current, AM_SEEKING_AbsolutePositioning, &stop, AM_SEEKING_AbsolutePositioning) == S_OK,
          "worker accepts recovery seek");
    filter.wait();
    check(demuxer->readDone.Wait(5000), "recovery seek resumes packet reading");
    check(filter.retained(current, stop, TRUE), "recovery seek updates segment state");
    puts("PASS: worker seek result, state rollback, halted delivery, failure retry, recovery");
}

struct ParserPinTest : CLAVOutputPin
{
    ParserPinTest(CLAVSplitter *filter, std::deque<CMediaType> &types, HRESULT *hr)
        : CLAVOutputPin(types, L"test PCM", filter, filter, hr, CBaseDemuxer::audio, "mpegts")
    {
        check(SUCCEEDED(SetMediaType(&types.front())), "set planar PCM media type");
    }
    Packet *pop() { return m_queue.Get(); }
};

static void freePCM(void *opaque, uint8_t *data)
{
    --*static_cast<int *>(opaque);
    av_free(data);
}

static Packet *planarPacket(int &live, const std::vector<int16_t> &samples)
{
    AVPacket source = {};
    source.size = static_cast<int>(samples.size() * sizeof(int16_t));
    source.data = static_cast<uint8_t *>(av_mallocz(source.size + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(source.data, samples.data(), source.size);
    source.buf = av_buffer_create(source.data, source.size, freePCM, &live, 0);
    ++live;
    auto *packet = new Packet();
    check(packet->SetPacket(&source) == 0, "reference planar input buffer");
    av_packet_unref(&source);
    packet->dwFlags = LAV_PACKET_PLANAR_PCM;
    packet->StreamId = 42;
    packet->rtStart = 1000000;
    packet->rtStop = 2000000;
    return packet;
}

static void pcmTests(CLAVSplitter &filter)
{
    for (WORD channels : {WORD(1), WORD(2), WORD(3)})
    {
        CMediaType mt;
        mt.SetType(&MEDIATYPE_Audio);
        mt.SetSubtype(&MEDIASUBTYPE_PCM);
        mt.SetFormatType(&FORMAT_WaveFormatEx);
        auto *wf = reinterpret_cast<WAVEFORMATEX *>(mt.AllocFormatBuffer(sizeof(WAVEFORMATEX)));
        *wf = {};
        wf->wFormatTag = WAVE_FORMAT_PCM;
        wf->nChannels = channels;
        wf->nSamplesPerSec = 48000;
        wf->wBitsPerSample = 16;
        wf->nBlockAlign = channels * 2;
        std::deque<CMediaType> types{mt};
        HRESULT hr = S_OK;
        ParserPinTest pin(&filter, types, &hr);
        check(SUCCEEDED(hr), "construct planar PCM output pin");
        CStreamParser parser(&pin, "mpegts");
        std::vector<int16_t> planar, interleaved;
        for (int channel = 0; channel < channels; ++channel)
            for (int sample = 1; sample <= 3; ++sample)
                planar.push_back(static_cast<int16_t>(channel * 10 + sample));
        for (int sample = 1; sample <= 3; ++sample)
            for (int channel = 0; channel < channels; ++channel)
                interleaved.push_back(static_cast<int16_t>(channel * 10 + sample));
        int live = 0;
        for (int iteration = 0; iteration < 1000; ++iteration)
        {
            check(parser.Parse(MEDIASUBTYPE_PCM, planarPacket(live, planar)) == S_OK, "convert planar PCM");
            check(live == (channels == 1 ? 1 : 0), "converted input is released immediately");
            std::unique_ptr<Packet> out(pin.pop());
            check(out && out->GetDataSize() == static_cast<int>(interleaved.size() * sizeof(int16_t)),
                  "PCM output size");
            check(memcmp(out->GetData(), interleaved.data(), out->GetDataSize()) == 0, "PCM channel interleaving");
            check(out->StreamId == 42 && out->rtStart == 1000000 && out->rtStop == 2000000,
                  "PCM timestamps and stream preserved");
            out.reset();
            check(live == 0, "all PCM input buffers released");
        }
    }
    puts("PASS: mono passthrough, stereo/three-channel interleaving, 3000 PCM input buffer releases");
}

int main()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try
    {
        HRESULT hr = S_OK;
        CLAVSplitter filter(nullptr, &hr);
        check(SUCCEEDED(hr), "construct splitter");
        filter.SetRuntimeConfig(TRUE);
        inputTests(filter);
        seekTests();
        pcmTests(filter);
        puts("ALL SPLITTER TESTS PASSED");
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return 0;
}
