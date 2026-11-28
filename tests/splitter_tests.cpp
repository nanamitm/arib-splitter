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
    bool seekable = true;
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
    bool IsSeekable() const override { return seekable; }
    const char *GetContainerFormat() const override { return "mpegts"; }
    const stream *SelectVideoStream() override { return nullptr; }
    const stream *SelectVideoELStream(DWORD) override { return nullptr; }
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
    void useTimestampFilter(SeekDemuxer *demuxer, const std::vector<CLAVOutputPin *> &pins)
    {
        m_pDemuxer = demuxer;
        m_pActivePins = pins;
    }
    void releaseTimestampFilter()
    {
        m_pActivePins.clear();
        m_pDemuxer = nullptr;
    }
    // Runs one packet through the discontinuity filter and reports whether it
    // asked for a discontinuity downstream.
    REFERENCE_TIME filterTimestamp(CLAVOutputPin *pin, DWORD streamId, REFERENCE_TIME rt, bool &resetDownstream)
    {
        Packet packet;
        packet.StreamId = streamId;
        packet.rtStart = packet.rtStop = rt;
        m_bDiscontinuitySent.insert(streamId);
        FilterTimestampDiscontinuity(pin, &packet);
        resetDownstream = m_bDiscontinuitySent.empty();
        return packet.rtStart;
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
    LONGLONG previous = 10000000, previousStop = 90000000;
    int previousCaller = 0;
    check(filter.SetPositionsInternal(&previousCaller, &previous, AM_SEEKING_AbsolutePositioning, &previousStop,
                                     AM_SEEKING_AbsolutePositioning) == E_ACCESSDENIED,
          "last successful target also needs a real seek after failure");
    filter.wait();
    check(demuxer->seeks == seeks + 2, "failed seek invalidates previous successful target cache");
    demuxer->seekResult = S_OK;
    demuxer->readDone.Reset();
    check(filter.SetPositions(&current, AM_SEEKING_AbsolutePositioning, &stop, AM_SEEKING_AbsolutePositioning) == S_OK,
          "worker accepts recovery seek");
    filter.wait();
    check(demuxer->readDone.Wait(5000), "recovery seek resumes packet reading");
    check(filter.retained(current, stop, TRUE), "recovery seek updates segment state");
    puts("PASS: worker seek result, state rollback, halted delivery, failure retry, recovery");
}

static void unseekableSeekTests()
{
    HRESULT hr = S_OK;
    SplitterTest filter(&hr);
    check(SUCCEEDED(hr), "construct unseekable test splitter");
    auto *demuxer = new SeekDemuxer(&filter);
    demuxer->seekable = false;
    filter.start(demuxer);
    check(demuxer->readDone.Wait(5000), "unseekable initial read finishes");
    demuxer->readDone.Reset();
    LONGLONG current = 10000000, stop = 90000000;
    check(filter.SetPositions(&current, AM_SEEKING_AbsolutePositioning, &stop, AM_SEEKING_AbsolutePositioning) == S_OK,
          "unseekable source accepts a seek that succeeds");
    filter.wait();
    check(demuxer->readDone.Wait(5000), "unseekable source reads after successful seek");
    demuxer->readDone.Reset();
    demuxer->seekResult = E_FAIL;
    const int reads = demuxer->reads;
    current = 20000000;
    stop = 80000000;
    check(filter.SetPositions(&current, AM_SEEKING_AbsolutePositioning, &stop, AM_SEEKING_AbsolutePositioning) ==
              E_FAIL,
          "unseekable seek failure reaches IMediaSeeking caller");
    filter.wait();
    check(filter.retained(10000000, 90000000, TRUE), "unseekable failure keeps the previous segment");
    check(demuxer->readDone.Wait(5000) && demuxer->reads > reads, "unseekable failure keeps playback running");
    const int seeks = demuxer->seeks;
    check(filter.SetPositions(&current, AM_SEEKING_AbsolutePositioning, &stop, AM_SEEKING_AbsolutePositioning) ==
              E_FAIL,
          "failed unseekable target is not cached as a success");
    filter.wait();
    check(demuxer->seeks == seeks + 1, "failed unseekable seek is retried");
    puts("PASS: unseekable seek failure reported, previous segment kept, playback continues");
}

struct ParserPinTest : CLAVOutputPin
{
    ParserPinTest(CLAVSplitter *filter, std::deque<CMediaType> &types, HRESULT *hr,
                  CBaseDemuxer::StreamType type = CBaseDemuxer::audio)
        : CLAVOutputPin(types, L"test PCM", filter, filter, hr, type, "mpegts")
    {
        check(SUCCEEDED(SetMediaType(&types.front())), "set planar PCM media type");
    }
    Packet *pop() { return m_queue.Get(); }
    size_t queued() { return m_queue.Size(); }
};

static void h264EOFTests(CLAVSplitter &filter)
{
    CMediaType mt;
    mt.SetType(&MEDIATYPE_Video);
    mt.SetSubtype(&MEDIASUBTYPE_AVC1);
    std::deque<CMediaType> types{mt};
    HRESULT hr = S_OK;
    ParserPinTest pin(&filter, types, &hr, CBaseDemuxer::video);
    CStreamParser parser(&pin, "mpegts");
    auto feed = [&](const std::vector<BYTE> &bytes, REFERENCE_TIME time) {
        auto *packet = new Packet();
        check(packet->SetData(bytes.data(), static_cast<int>(bytes.size())) == 0, "allocate H264 input");
        packet->StreamId = 42;
        packet->rtStart = time;
        packet->rtStop = time + 333667;
        packet->bSyncPoint = TRUE;
        packet->bDiscontinuity = TRUE;
        check(parser.Parse(MEDIASUBTYPE_AVC1, packet) == S_OK, "parse H264 input");
    };
    auto expect = [&](const std::vector<BYTE> &bytes, REFERENCE_TIME time) {
        check(pin.queued() > 0, "H264 output precedes EOS");
        std::unique_ptr<Packet> packet(pin.pop());
        check(packet && packet->GetDataSize() == static_cast<int>(bytes.size()) &&
                  memcmp(packet->GetData(), bytes.data(), bytes.size()) == 0,
              "H264 output contains every length-prefixed NAL in order");
        check(packet->StreamId == 42 && packet->rtStart == time && packet->rtStop == time + 333667 &&
                  packet->bSyncPoint && packet->bDiscontinuity,
              "H264 output preserves timestamps and flags");
    };
    auto expectEOS = [&] {
        check(pin.queued() == 1 && pin.pop() == nullptr, "exactly one EOS follows the last H264 access unit");
    };

    // A single NAL never sees a following start code before EOF.
    for (int prefix : {3, 4})
    {
        std::vector<BYTE> bytes(prefix - 1, 0);
        bytes.insert(bytes.end(), {1, 0x65, 0x88, 0x84, 0x21});
        feed(bytes, 1000000);
        check(pin.queued() == 0, "final NAL is held until EOF");
        parser.Parse(MEDIASUBTYPE_AVC1, nullptr);
        expect({0, 0, 0, 4, 0x65, 0x88, 0x84, 0x21}, 1000000);
        expectEOS();
    }

    // Drain both an already queued AUD/access unit and the final buffered NAL.
    feed({0, 0, 1, 9, 0xF0, 0, 0, 1, 0x65, 0x88, 0x84, 0x21}, 2000000);
    feed({0, 0, 1, 9, 0xF0, 0, 0, 1, 0x41, 0x88, 0x84, 0x22}, 2333667);
    parser.Parse(MEDIASUBTYPE_AVC1, nullptr);
    expect({0, 0, 0, 2, 9, 0xF0, 0, 0, 0, 4, 0x65, 0x88, 0x84, 0x21}, 2000000);
    expect({0, 0, 0, 2, 9, 0xF0, 0, 0, 0, 4, 0x41, 0x88, 0x84, 0x22}, 2333667);
    expectEOS();

    // A seek flush discards old data rather than delivering it at EOF.
    feed({0, 0, 1, 0x65, 0x88, 0x84, 0x21}, 3000000);
    parser.Flush();
    parser.Parse(MEDIASUBTYPE_AVC1, nullptr);
    expectEOS();
    parser.Parse(MEDIASUBTYPE_AVC1, nullptr);
    expectEOS();
    puts("PASS: H264 EOF drains final NAL/access units with timestamps; flush discards buffered data");
}

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

struct TimedPacket
{
    REFERENCE_TIME rt;
    int stream;
};

// Video every frame and audio every AAC frame for 80 s from the segment start, in
// timestamp order, with the audio left out between gapStart and gapEnd. Times
// at or after jumpAt are moved by jump on both streams.
static std::vector<TimedPacket> timedPackets(REFERENCE_TIME gapStart, REFERENCE_TIME gapEnd, REFERENCE_TIME jumpAt,
                                             REFERENCE_TIME jump)
{
    const REFERENCE_TIME end = 800000000;
    const REFERENCE_TIME frame = 333667, aacFrame = 213333;
    std::vector<TimedPacket> packets;
    for (REFERENCE_TIME rt = 0; rt < end; rt += frame)
        packets.push_back({rt, 0});
    for (REFERENCE_TIME rt = 0; rt < end; rt += aacFrame)
        if (rt < gapStart || rt >= gapEnd)
            packets.push_back({rt, 1});
    std::stable_sort(packets.begin(), packets.end(),
                     [](const TimedPacket &a, const TimedPacket &b) { return a.rt < b.rt; });
    for (auto &packet : packets)
        if (packet.rt >= jumpAt)
            packet.rt += jump;
    return packets;
}

static void timestampDiscontinuityTests()
{
    HRESULT hr = S_OK;
    SplitterTest filter(&hr);
    check(SUCCEEDED(hr), "construct timestamp filter splitter");
    CMediaType mt;
    mt.SetType(&MEDIATYPE_Audio);
    mt.SetSubtype(&MEDIASUBTYPE_PCM);
    mt.SetFormatType(&FORMAT_WaveFormatEx);
    auto *wf = reinterpret_cast<WAVEFORMATEX *>(mt.AllocFormatBuffer(sizeof(WAVEFORMATEX)));
    *wf = {};
    wf->wFormatTag = WAVE_FORMAT_PCM;
    wf->nChannels = 2;
    wf->nSamplesPerSec = 48000;
    wf->wBitsPerSample = 16;
    wf->nBlockAlign = 4;
    std::deque<CMediaType> types{mt};
    auto *demuxer = new SeekDemuxer(&filter);
    demuxer->AddRef();

    auto run = [&](const std::vector<TimedPacket> &packets, std::vector<REFERENCE_TIME> &delivered, int &resets) {
        HRESULT pinHr = S_OK;
        ParserPinTest video(&filter, types, &pinHr), audio(&filter, types, &pinHr);
        check(SUCCEEDED(pinHr), "construct timestamp filter pins");
        CLAVOutputPin *pins[] = {&video, &audio};
        filter.useTimestampFilter(demuxer, {&video, &audio});
        delivered.clear();
        resets = 0;
        for (const auto &packet : packets)
        {
            bool reset = false;
            delivered.push_back(filter.filterTimestamp(pins[packet.stream], packet.stream, packet.rt, reset));
            resets += reset;
        }
        filter.releaseTimestampFilter();
    };

    // The commentary track of the recording that showed this: PID 0x111 left
    // the PMT at 1801.728 s and came back at 1831.765 s while video went on;
    // the packets here start 1780 s into that recording.
    std::vector<REFERENCE_TIME> delivered;
    int resets = 0;
    auto packets = timedPackets(217280000, 517650000, _I64_MAX, 0);
    run(packets, delivered, resets);
    check(resets == 0, "a gap in one stream does not reset downstream");
    for (size_t i = 0; i < packets.size(); ++i)
        check(delivered[i] == packets[i].rt, "a gap in one stream leaves all timestamps unchanged");

    // A program boundary moves every stream back by 1000 s. The streams must
    // keep running on from where they were.
    const REFERENCE_TIME jumpAt = 400000000, jump = -10000000000;
    packets = timedPackets(_I64_MAX, _I64_MAX, jumpAt, jump);
    run(packets, delivered, resets);
    check(resets == 1, "a jump on all streams resets downstream once");
    REFERENCE_TIME last[2] = {AV_NOPTS_VALUE, AV_NOPTS_VALUE};
    for (size_t i = 0; i < packets.size(); ++i)
    {
        REFERENCE_TIME &prev = last[packets[i].stream];
        // The stream that jumps first is lined up on its previous packet, so the
        // others can land up to one of its frames early.
        check(prev == AV_NOPTS_VALUE || _abs64(delivered[i] - prev) <= 333667,
              "timestamps stay continuous across a jump on all streams");
        prev = delivered[i];
    }
    check(last[0] > jumpAt + 300000000 && last[1] > jumpAt + 300000000,
          "timestamps after a jump on all streams advance in real time");

    // After a seek the shared offset is unset until an A/V packet comes in. A
    // pending caption can be handed over before that, when the next caption PES
    // closes it; its times must come through as they are.
    {
        SplitterTest seeked(&hr);
        check(SUCCEEDED(hr), "construct seeked timestamp filter splitter");
        HRESULT pinHr = S_OK;
        ParserPinTest video(&seeked, types, &pinHr), caption(&seeked, types, &pinHr, CBaseDemuxer::subpic);
        check(SUCCEEDED(pinHr) && caption.IsSubtitlePin(), "construct caption timestamp filter pin");
        seeked.useTimestampFilter(demuxer, {&video, &caption});
        bool reset = false;
        // Times are relative to the segment start, as DeliverPacket passes them.
        check(seeked.filterTimestamp(&caption, 2, 1000000, reset) == 1000000,
              "a caption before any A/V packet keeps its timestamp");
        check(seeked.filterTimestamp(&video, 0, 1200000, reset) == 1200000,
              "the first A/V packet after a caption keeps its timestamp");
        check(seeked.filterTimestamp(&caption, 2, 3000000, reset) == 3000000,
              "a caption after the first A/V packet keeps its timestamp");
        seeked.releaseTimestampFilter();
    }

    demuxer->Release();
    puts("PASS: a gap in one stream keeps timestamps, a jump on all streams is still smoothed over, "
         "a caption ahead of A/V keeps its timestamp");
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
        unseekableSeekTests();
        pcmTests(filter);
        h264EOFTests(filter);
        timestampDiscontinuityTests();
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
