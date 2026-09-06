#include "stdafx.h"
#include <initguid.h>
#include "moreuuids.h"
#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <vector>
#include <memory>
#include <cstdio>
#include <stdexcept>
#include "BaseDemuxer.h"
#include "LAVSplitterSettingsInternal.h"
#include "IKeyFrameInfo.h"
#include "ITrackInfo.h"
#include "FontInstaller.h"
#include "DSMResourceBag.h"
#include "LAVFDemuxer.h"
extern "C"
{
#include "libavformat/demux.h"
}
CFactoryTemplate g_Templates[1] = {};
int g_cTemplates = 0;
static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
struct Sample
{
    int stream;
    int64_t pts;
    std::vector<uint8_t> data;
};
struct Feed
{
    std::vector<Sample> samples;
    size_t next = 0;
    int live = 0;
};
static void freePacket(void *opaque, uint8_t *data)
{
    --static_cast<Feed *>(opaque)->live;
    av_free(data);
}
static int readHeader(AVFormatContext *ctx)
{
    auto *caption = avformat_new_stream(ctx, nullptr);
    caption->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    caption->codecpar->codec_id = AV_CODEC_ID_ARIB_CAPTION;
    caption->codecpar->profile = AV_PROFILE_ARIB_PROFILE_A;
    caption->time_base = {1, 1000};
    auto *audio = avformat_new_stream(ctx, nullptr);
    audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    audio->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    audio->codecpar->sample_rate = 48000;
    av_channel_layout_default(&audio->codecpar->ch_layout, 2);
    audio->time_base = {1, 1000};
    ctx->start_time = 0;
    ctx->duration = 1000000;
    return 0;
}
static int readPacket(AVFormatContext *ctx, AVPacket *pkt)
{
    auto *feed = static_cast<Feed *>(ctx->opaque);
    if (feed->next == feed->samples.size())
        return AVERROR_EOF;
    const auto &s = feed->samples[feed->next++];
    auto *data = static_cast<uint8_t *>(av_mallocz(s.data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    memcpy(data, s.data.data(), s.data.size());
    pkt->buf = av_buffer_create(data, s.data.size(), freePacket, feed, 0);
    ++feed->live;
    pkt->data = data;
    pkt->size = static_cast<int>(s.data.size());
    pkt->stream_index = s.stream;
    pkt->pts = pkt->dts = s.pts;
    pkt->duration = s.stream == 1 ? 20 : 0;
    return 0;
}
static FFInputFormat inputFormat()
{
    FFInputFormat f = {};
    f.p.name = "arib-regression";
    f.p.flags = AVFMT_NOFILE;
    f.read_header = readHeader;
    f.read_packet = readPacket;
    return f;
}
static FFInputFormat format = inputFormat();
struct AribDemuxerTest : CLAVFDemuxer
{
    AribDemuxerTest(CCritSec *lock, ILAVFSettingsInternal *settings, Feed &feed,
                    int profile = AV_PROFILE_ARIB_PROFILE_A)
        : CLAVFDemuxer(lock, settings)
    {
        m_avFormat = avformat_alloc_context();
        m_avFormat->opaque = &feed;
        m_avFormat->flags |= AVFMT_FLAG_NOPARSE | AVFMT_FLAG_NOFILLIN;
        check(avformat_open_input(&m_avFormat, nullptr, &format.p, nullptr) == 0, "open test input");
        m_avFormat->streams[0]->codecpar->profile = profile;
        m_dActiveStreams[subpic] = 0;
        m_dActiveStreams[audio] = 1;
    }
    void unknownDuration() { m_avFormat->duration = AV_NOPTS_VALUE; }
    void clearPending() { FlushAribPendingPackets(); }
    void delayPending(REFERENCE_TIME delay)
    {
        auto shift = [delay](Packet *p) {
            p->rtStart += delay;
            p->rtStop += delay;
        };
        for (auto &entry : m_aribPendingPackets)
        {
            shift(entry.second);
            m_aribPendingDelay[entry.first] = delay;
        }
        for (auto &entry : m_aribPendingExtras)
            for (Packet *p : entry.second)
                shift(p);
    }
    bool pendingEmpty() const { return m_aribPendingPackets.empty(); }
    bool isSuperimpose() const { return m_LateAribSubtitleIsSuperimpose; }
    void placeholder() { m_dActiveStreams[subpic] = LATE_ARIB_SUBTITLE_PID; }
};
using Demuxer = AribDemuxerTest;
// A minimal statement PES with one text data unit. CS alone is a clear event.
static std::vector<uint8_t> pes(std::vector<uint8_t> text, bool super = false)
{
    size_t unit = 5 + text.size();
    size_t group = 4 + unit;
    std::vector<uint8_t> p = {uint8_t(super ? 0x81 : 0x80),
                              0xff,
                              0xf0,
                              0x04,
                              0,
                              0,
                              uint8_t(group >> 8),
                              uint8_t(group),
                              0,
                              0,
                              uint8_t(unit >> 8),
                              uint8_t(unit),
                              0x1f,
                              0x20,
                              0,
                              uint8_t(text.size() >> 8),
                              uint8_t(text.size())};
    p.insert(p.end(), text.begin(), text.end());
    p.push_back(0);
    p.push_back(0);
    return p;
}
static std::vector<uint8_t> textPES()
{
    return pes({0x0c, 0x0e, 0x41});
}
struct Event
{
    REFERENCE_TIME start, stop;
    std::string data;
    size_t readAt;
};
static std::vector<Event> drain(Demuxer &d, Feed &feed)
{
    std::vector<Event> events;
    for (int i = 0; i < 10000; ++i)
    {
        Packet *p = nullptr;
        HRESULT hr = d.GetNextPacket(&p);
        if (FAILED(hr))
            break;
        if (hr != S_OK)
            continue;
        check(p != nullptr, "S_OK packet");
        if (p->StreamId != 1)
            events.push_back({p->rtStart, p->rtStop,
                              std::string(reinterpret_cast<char *>(p->GetData()), p->GetDataSize()), feed.next});
        delete p;
    }
    return events;
}
static void timelineTests(ILAVFSettingsInternal *settings)
{
    CCritSec lock;
    Feed f{{{0, 0, textPES()},
            {1, 100, {0, 0, 0, 0}},
            {1, 250, {0, 0, 0, 0}},
            {1, 500, {0, 0, 0, 0}},
            {0, 625, pes({0x0c})},
            {1, 1000, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, f);
        auto events = drain(d, f);
        check(events.size() >= 3, "indefinite caption emitted without next caption");
        check(events.front().readAt == 3, "first caption emitted by 250ms A/V packet");
        check(events.front().start == 0 && events.front().stop == 2500000, "first interval");
        check(events.back().stop == 6250000, "clear truncates pending tail exactly");
        for (const auto &e : events)
            check(e.stop <= 6250000, "nothing extends beyond clear");
        check(d.pendingEmpty(), "clear removes pending caption");
    }
    check(f.live == 0, "all caption and A/V buffers released");
    Feed tail{{{0, 900, textPES()}}};
    {
        Demuxer d(&lock, settings, tail);
        auto events = drain(d, tail);
        check(!events.empty(), "EOF emits final pending caption");
        check(events.back().stop == 10000000, "EOF ends at duration");
    }
    check(tail.live == 0, "EOF releases buffers");
    Feed invalid;
    for (int i = 0; i < 1000; ++i)
        invalid.samples.push_back({0, i, {0x80, 0xff, 0xf0}});
    {
        Demuxer d(&lock, settings, invalid);
        drain(d, invalid);
    }
    check(invalid.live == 0, "decode failure early returns release 1000 packets");
    Feed explicitWait{{{0, 100, pes({0x0c, 0x0e, 0x41, 0x9d, 0x20, 0x43})}}};
    {
        Demuxer d(&lock, settings, explicitWait);
        auto events = drain(d, explicitWait);
        check(!events.empty() && events.front().readAt == 1, "explicit duration emits immediately");
        check(events.front().start == 1000000 && events.front().stop == 4000000, "explicit wait duration preserved");
    }
    check(explicitWait.live == 0, "explicit wait ownership");
    Feed unknown{{{0, 900, textPES()}}};
    {
        Demuxer d(&lock, settings, unknown);
        d.unknownDuration();
        auto events = drain(d, unknown);
        check(!events.empty(), "unknown-duration EOF emits final caption");
    }
    check(unknown.live == 0, "unknown duration ownership");
    Feed reset{{{0, 0, textPES()}, {1, 1000, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, reset);
        Packet *p = nullptr;
        check(d.GetNextPacket(&p) == S_FALSE && !p, "pending caption before flush");
        d.clearPending();
        check(drain(d, reset).empty(), "seek/selection flush drops old captions");
    }
    check(reset.live == 0, "flush ownership");
    Feed longCaption;
    longCaption.samples.push_back({0, 0, textPES()});
    for (int ms = 250; ms <= 60000; ms += 250)
        longCaption.samples.push_back({1, ms, {0, 0, 0, 0}});
    {
        Demuxer d(&lock, settings, longCaption);
        auto events = drain(d, longCaption);
        std::set<std::string> readOrders;
        std::map<std::string, REFERENCE_TIME> ends;
        for (const auto &e : events)
        {
            size_t comma = e.data.find(',');
            check(readOrders.insert(e.data.substr(0, comma)).second, "unique ASS ReadOrder for every interval");
            std::string payload = e.data.substr(comma);
            check(e.start == ends[payload], "continuous intervals without overlaps or gaps");
            ends[payload] = e.stop;
        }
        check(!ends.empty(), "long caption emitted");
        for (const auto &e : ends)
            check(e.second == 600200000, "caption persists for all 60 seconds through EOF");
    }
    check(longCaption.live == 0, "long caption buffer ownership");
    for (REFERENCE_TIME delay : {-2000000LL, 5000000LL})
    {
        Feed shifted{{{0, 0, textPES()}, {1, 250, {0, 0, 0, 0}}}};
        {
            Demuxer d(&lock, settings, shifted);
            Packet *p = nullptr;
            check(d.GetNextPacket(&p) == S_FALSE && !p, "pending caption before offset");
            d.delayPending(delay);
            auto events = drain(d, shifted);
            check(!events.empty() && events.front().start == delay, "caption offset start");
            check(events.front().stop == 2500000 + delay, "offset does not delay chunk scheduling");
            check(events.back().stop == 10000000 + delay, "offset applied at EOF");
        }
        check(shifted.live == 0, "offset packet ownership");
    }
    puts("PASS: A/V-driven intervals, clear, EOF (known/unknown duration), explicit wait, flush, packet ownership");
}
static void profileTests(ILAVFSettingsInternal *settings)
{
    CCritSec lock;
    for (int profile : {AV_PROFILE_ARIB_PROFILE_A, AV_PROFILE_ARIB_PROFILE_C})
    {
        for (bool super : {false, true})
        {
            Feed f{{{0, 0, pes({0x0c, 0x0e, 0x41}, super)}, {1, 250, {0, 0, 0, 0}}}};
            {
                Demuxer d(&lock, settings, f, profile);
                auto events = drain(d, f);
                check(!events.empty(), "caption type/profile combination decodes");
            }
            check(f.live == 0, "profile test packet ownership");
        }
    }
    Feed f{{{0, 0, pes({0x0c, 0x0e, 0x41}, true)}, {0, 100, textPES()}, {1, 350, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, f, AV_PROFILE_ARIB_PROFILE_C);
        d.placeholder();
        auto events = drain(d, f);
        check(!events.empty() && events.front().start == 1000000, "placeholder prefers caption even on same PID");
        check(!d.isSuperimpose(), "placeholder tracks PES type");
    }
    check(f.live == 0, "placeholder packet ownership");
    puts("PASS: Profile A/C captions and superimpose, placeholder takeover");
}
int wmain(int argc, wchar_t **argv)
{
    try
    {
        check(argc == 2, "provide splitter DLL path");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HMODULE dll = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        check(dll != nullptr, "load built splitter");
        auto getClass = reinterpret_cast<HRESULT(STDAPICALLTYPE *)(REFCLSID, REFIID, void **)>(
            GetProcAddress(dll, "DllGetClassObject"));
        CLSID clsid;
        CLSIDFromString(L"{DB05F97C-F39C-417C-8011-33D093234F1A}", &clsid);
        CComPtr<IClassFactory> factory;
        check(SUCCEEDED(getClass(clsid, IID_IClassFactory, reinterpret_cast<void **>(&factory))), "class factory");
        CComPtr<ILAVFSettingsInternal> settings;
        check(SUCCEEDED(factory->CreateInstance(nullptr, __uuidof(ILAVFSettingsInternal),
                                                reinterpret_cast<void **>(&settings))),
              "settings interface");
        settings->SetRuntimeConfig(TRUE);
        timelineTests(settings);
        profileTests(settings);
        puts("ALL DEMUXER TESTS PASSED");
        return 0;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
