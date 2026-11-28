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
#include "AribCommon.h"
#include "LAVFVideoHelper.h"
extern "C"
{
#include "libavformat/demux.h"
#include "libavutil/crc.h"
}
CFactoryTemplate g_Templates[1] = {};
int g_cTemplates = 0;
static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
// A sample with pts == kNoPts is fed without a timestamp, like a PES that
// arrives right after a seek or at a stream discontinuity.
static const int64_t kNoPts = INT64_MIN;
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
    pkt->pts = pkt->dts = (s.pts == kNoPts) ? AV_NOPTS_VALUE : s.pts;
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
    // Takes over a context opened with FFmpeg's own mpegts demuxer.
    AribDemuxerTest(CCritSec *lock, ILAVFSettingsInternal *settings, AVFormatContext *ts)
        : CLAVFDemuxer(lock, settings)
    {
        m_avFormat = ts;
        m_pszInputFormat = "mpegts";
        m_bMPEGTS = TRUE;
    }
    HRESULT createStreams() { return CreateStreams(); }
    const stream &audioStream(size_t i) const { return m_streams[audio].at(i); }
    size_t audioCount() const { return m_streams[audio].size(); }
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
        check(!events.empty(), "caption handed over as A/V advances");
        check(events.front().readAt == 5, "caption shorter than the commit interval waits for the clear");
        check(events.front().start == 0, "caption starts at its own timestamp");
        std::map<std::string, REFERENCE_TIME> ends;
        for (const auto &e : events)
        {
            const auto payload = e.data.substr(e.data.find(','));
            check(e.start == 0 && e.stop == 6250000, "short caption is sent as a single batch");
            ends[payload] = e.stop;
        }
        for (const auto &end : ends)
            check(end.second == 6250000, "clear truncates the caption exactly");
        check(d.pendingEmpty(), "clear removes pending caption");
    }
    check(f.live == 0, "all caption and A/V buffers released");
    // A/V is muxed 820ms ahead of the captions: the clear at 9800ms is read only
    // once A/V has reached 10620ms. Committing against the A/V time alone would
    // send a 5-10s interval across the clear; with the lead taken off, only the
    // first 5s interval is committed before it.
    Feed leading;
    for (int ms = 0; ms <= 800; ms += 20)
        leading.samples.push_back({1, ms, {0, 0, 0, 0}});
    leading.samples.push_back({0, 0, textPES()});
    for (int ms = 820; ms <= 10600; ms += 20)
        leading.samples.push_back({1, ms, {0, 0, 0, 0}});
    leading.samples.push_back({0, 9800, pes({0x0c})});
    for (int ms = 10620; ms <= 11200; ms += 20)
        leading.samples.push_back({1, ms, {0, 0, 0, 0}});
    {
        Demuxer d(&lock, settings, leading);
        auto events = drain(d, leading);
        check(events.size() > 1 && events.front().readAt < 41 + 1 + 490,
              "caption intervals delivered before the clear while A/V leads it");
        std::map<std::string, REFERENCE_TIME> ends;
        for (const auto &e : events)
        {
            const auto payload = e.data.substr(e.data.find(','));
            check(e.start == ends[payload], "leading A/V keeps caption intervals contiguous");
            check(e.stop <= 98000000, "leading A/V does not commit past the clear");
            ends[payload] = e.stop;
        }
        for (const auto &end : ends)
            check(end.second == 98000000, "clear truncates the caption exactly despite leading A/V");
        check(d.pendingEmpty(), "clear removes pending caption with leading A/V");
    }
    check(leading.live == 0, "leading A/V buffers released");
    // A/V jumps back from 23s to 0s, as at a program boundary in a recording.
    // The caption from the old timeline ends where A/V got on it, and the next
    // caption is placed on the new timeline instead of behind the old maximum.
    Feed discont;
    for (int ms = 20000; ms <= 21000; ms += 20)
        discont.samples.push_back({1, ms, {0, 0, 0, 0}});
    discont.samples.push_back({0, 21000, textPES()});
    for (int ms = 21020; ms <= 23000; ms += 20)
        discont.samples.push_back({1, ms, {0, 0, 0, 0}});
    for (int ms = 0; ms <= 1000; ms += 20)
        discont.samples.push_back({1, ms, {0, 0, 0, 0}});
    discont.samples.push_back({0, 1000, pes({0x0c, 0x0e, 0x42})});
    for (int ms = 1020; ms <= 9000; ms += 20)
        discont.samples.push_back({1, ms, {0, 0, 0, 0}});
    {
        Demuxer d(&lock, settings, discont);
        auto events = drain(d, discont);
        std::map<std::string, std::pair<REFERENCE_TIME, REFERENCE_TIME>> spans;
        for (const auto &e : events)
        {
            const auto payload = e.data.substr(e.data.find(','));
            auto it = spans.find(payload);
            if (it == spans.end())
                spans[payload] = {e.start, e.stop};
            else
            {
                check(e.start == it->second.second, "caption intervals stay contiguous across a discontinuity");
                it->second.second = e.stop;
            }
        }
        check(spans.size() == 2, "captions on both sides of the discontinuity are delivered");
        bool oldSeen = false, newSeen = false;
        for (const auto &span : spans)
        {
            if (span.second.first == 210000000)
            {
                check(span.second.second == 230200000, "old caption ends where A/V got on the old timeline");
                oldSeen = true;
            }
            if (span.second.first == 10000000)
            {
                check(span.second.second == 90200000, "new caption runs to the end of the new timeline");
                newSeen = true;
            }
        }
        check(oldSeen && newSeen, "captions keep their own timelines");
        check(d.pendingEmpty(), "discontinuity test drains pending captions");
    }
    check(discont.live == 0, "discontinuity buffers released");
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
    const auto timedPES = pes({0x0c, 0x0e, 0x41, 0x9d, 0x20, 0x43});
    Feed explicitWait{{{0, 100, timedPES}, {1, 450, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, explicitWait);
        auto events = drain(d, explicitWait);
        check(!events.empty() && events.front().readAt == 2, "explicit duration emits at its deadline");
        check(events.front().start == 1000000 && events.front().stop == 4000000, "explicit wait duration preserved");
    }
    check(explicitWait.live == 0, "explicit wait ownership");
    Feed earlyClear{{{0, 100, timedPES}, {0, 200, pes({0x0c})}, {1, 450, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, earlyClear);
        auto events = drain(d, earlyClear);
        check(!events.empty(), "explicit caption handed over by early clear");
        for (const auto &e : events)
            check(e.start == 1000000 && e.stop == 2000000, "early clear truncates explicit duration");
    }
    check(earlyClear.live == 0, "early clear ownership");
    Feed earlyReplacement{{{0, 100, timedPES}, {0, 200, textPES()}, {1, 450, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, earlyReplacement);
        auto events = drain(d, earlyReplacement);
        bool replaced = false;
        for (const auto &e : events)
            if (e.start == 1000000)
            {
                check(e.stop == 2000000, "early replacement truncates explicit duration");
                replaced = true;
            }
        check(replaced, "explicit caption handed over by early replacement");
    }
    check(earlyReplacement.live == 0, "early replacement ownership");
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
    Feed noPts{{{1, 100, {0, 0, 0, 0}}, {0, kNoPts, textPES()}, {1, 400, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, noPts);
        auto events = drain(d, noPts);
        check(!events.empty(), "caption without a timestamp still reaches the timeline");
        check(events.front().start == 1200000, "caption without a timestamp starts at the A/V position");
        for (const auto &e : events)
            check(e.start >= 0 && e.stop > e.start, "no INVALID_TIME arithmetic leaks into the events");
    }
    check(noPts.live == 0, "timestamp-less caption ownership");
    Feed noClock{{{0, kNoPts, textPES()}, {0, 200, textPES()}, {1, 600, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, noClock);
        auto events = drain(d, noClock);
        check(!events.empty(), "a later caption still works after a skipped one");
        check(events.front().start == 2000000, "the skipped caption leaves the timeline untouched");
    }
    check(noClock.live == 0, "skipped caption ownership");
    Feed longCaption;
    longCaption.samples.push_back({0, 0, textPES()});
    // A stream keeps sending packets that carry no caption while one is on screen.
    for (int ms = 1000; ms <= 60000; ms += 1000)
        longCaption.samples.push_back({0, ms, pes({})});
    longCaption.samples.push_back({1, 60500, {0, 0, 0, 0}});
    {
        Demuxer d(&lock, settings, longCaption);
        auto events = drain(d, longCaption);
        std::set<std::string> readOrders;
        std::map<std::string, REFERENCE_TIME> ends;
        std::map<std::string, int> parts;
        for (const auto &e : events)
        {
            size_t comma = e.data.find(',');
            check(readOrders.insert(e.data.substr(0, comma)).second, "unique ASS ReadOrder for every interval");
            std::string payload = e.data.substr(comma);
            check(e.start == ends[payload], "continuous intervals without overlaps or gaps");
            ends[payload] = e.stop;
            parts[payload]++;
        }
        check(!ends.empty(), "long caption emitted");
        for (const auto &e : ends)
            check(e.second >= 600000000, "caption persists for all 60 seconds through EOF");
        // One batch for each media timestamp here, rather than repeated events
        // covering the same interval.
        for (const auto &e : parts)
            check(e.second <= 62, "caption batches are bounded by media progress");
    }
    check(longCaption.live == 0, "long caption buffer ownership");
    for (REFERENCE_TIME delay : {-2000000LL, 0LL, 5000000LL})
    {
        Feed sparse;
        sparse.samples.push_back({0, 0, textPES()});
        for (int ms = 20; ms <= 60000; ms += 20)
            sparse.samples.push_back({1, ms, {0, 0, 0, 0}});
        {
            Demuxer d(&lock, settings, sparse);
            Packet *p = nullptr;
            check(d.GetNextPacket(&p) == S_FALSE && !p, "sparse caption starts pending");
            d.delayPending(delay);
            const auto events = drain(d, sparse);
            check(!events.empty() && events.front().readAt <= 266,
                  "sparse caption delivered within the commit guard plus one interval of A/V");
            std::map<std::string, REFERENCE_TIME> ends;
            std::set<std::string> readOrders;
            for (const auto &e : events)
            {
                const auto comma = e.data.find(',');
                check(readOrders.insert(e.data.substr(0, comma)).second, "sparse intervals have unique read orders");
                const auto payload = e.data.substr(comma);
                const auto it = ends.find(payload);
                check(e.start == (it == ends.end() ? delay : it->second), "sparse intervals have no gap or overlap");
                check(e.stop > e.start, "sparse interval has positive duration");
                ends[payload] = e.stop;
            }
            for (const auto &end : ends)
                check(end.second == 600200000 + delay, "sparse caption extends through final A/V packet");
            check(d.pendingEmpty(), "EOF drains sparse caption");
        }
        check(sparse.live == 0, "sparse caption releases all input buffers");
    }
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
            check(events.back().stop == 10000000 + delay, "offset applied at the last interval");
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
// Text of the caption events, without the ReadOrder/Layer/Style prefix.
static std::vector<std::string> eventTexts(const std::vector<Event> &events)
{
    std::vector<std::string> texts;
    for (const auto &e : events)
    {
        size_t at = 0;
        for (int field = 0; field < 8 && at != std::string::npos; ++field)
            at = e.data.find(',', at) + 1;
        texts.push_back(e.data.substr(at));
    }
    return texts;
}
static bool anyContains(const std::vector<std::string> &texts, const char *needle)
{
    return std::any_of(texts.begin(), texts.end(),
                       [needle](const std::string &t) { return t.find(needle) != std::string::npos; });
}
static void glyphRunTests(ILAVFSettingsInternal *settings)
{
    // U+3042, U+301C and U+2192 are one em in MS Gothic; U+266C is not in it.
    check(AribGlyphAdvancesOneEm("", "\xE3\x81\x82"), "kana advances one em");
    check(AribGlyphAdvancesOneEm("", "\xE3\x80\x9C"), "wave dash advances one em");
    check(AribGlyphAdvancesOneEm("", "\xE2\x86\x92"), "arrow the font draws one em wide merges");
    check(!AribGlyphAdvancesOneEm("", "\xE2\x99\xAC"), "symbol missing from the font does not merge");
    check(!AribGlyphAdvancesOneEm("", "A"), "half width glyph does not merge");
    check(!AribGlyphAdvancesOneEm("", "\xE3\x82\x99"), "combining sound mark does not merge");
    check(!AribGlyphAdvancesOneEm("", "\xE3\x81\x82\xEF\xB8\x80"), "cell of several code points does not merge");
    check(!AribGlyphAdvancesOneEm("", ""), "empty cell does not merge");
    check(AribGlyphAdvancesOneEm("MS Gothic", "\xE3\x81\xA3"), "small kana is one em in a monospaced font");
    check(!AribGlyphAdvancesOneEm("MS PGothic", "\xE3\x81\xA3"), "small kana is narrower in a proportional font");
    check(!AribGlyphAdvancesOneEm("No Such Caption Font", "\xE3\x81\x82"), "unknown font does not merge");

    // The "U+266C U+301C" line that was drawn with the wave dash pulled onto the
    // note: the note is placed on its own cell, the rest of the line is one run.
    CCritSec lock;
    Feed f{{{0, 0, pes({0x0c, 0x22, 0x7c, 0x21, 0x41, 0x24, 0x22, 0x24, 0x24})},
            {1, 250, {0, 0, 0, 0}},
            {0, 500, pes({0x0c})},
            {1, 1000, {0, 0, 0, 0}}}};
    {
        Demuxer d(&lock, settings, f);
        auto texts = eventTexts(drain(d, f));
        check(!texts.empty(), "glyph run caption decodes");
        check(!anyContains(texts, "\xE2\x99\xAC\xE3\x80\x9C"), "note is not merged with the next glyph");
        check(std::any_of(texts.begin(), texts.end(),
                          [](const std::string &t) {
                              return t.size() >= 3 && t.compare(t.size() - 3, 3, "\xE2\x99\xAC") == 0;
                          }),
              "note is an event of its own");
        check(anyContains(texts, "}\xE3\x80\x9C\xE3\x81\x82\xE3\x81\x84"), "one em glyphs after the note stay one run");
    }
    check(f.live == 0, "glyph run packet ownership");
    puts("PASS: caption glyph runs merge only glyphs one em wide in the caption font");
}
static int seekStub(AVFormatContext *, int, int64_t, int)
{
    return 0;
}
// The test format has no I/O context, like RTSP. Whether a failed seek leaves
// playback running depends on the demuxer's own seek and a known duration.
static void seekableTests(ILAVFSettingsInternal *settings)
{
    CCritSec lock;
    Feed f;
    {
        Demuxer d(&lock, settings, f);
        check(!d.IsSeekable(), "format without I/O or its own seek is unseekable");
    }
    format.read_seek = seekStub;
    {
        Demuxer d(&lock, settings, f);
        check(d.IsSeekable(), "format with its own seek and a duration is seekable");
        d.unknownDuration();
        check(!d.IsSeekable(), "live stream with its own seek but no duration is unseekable");
    }
    format.read_seek = nullptr;
    puts("PASS: seekability from I/O, demuxer seek and duration");
}
// NAL length size read from HEVC and VVC configuration records. Annex B and
// records too short to hold the field leave it unset.
static DWORD nalLengthSize(bool vvc, std::vector<BYTE> extradata)
{
    MPEG2VIDEOINFO mp2vi = {};
    // FFmpeg pads extradata, so a short record still has readable zeros after it.
    extradata.resize(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE);
    const int size = static_cast<int>(extradata.size()) - AV_INPUT_BUFFER_PADDING_SIZE;
    if (vvc)
        g_VideoHelper.ProcessVVCExtradata(extradata.data(), size, &mp2vi);
    else
        g_VideoHelper.ProcessHEVCExtradata(extradata.data(), size, &mp2vi);
    return mp2vi.dwFlags;
}
static void videoHelperTests()
{
    std::vector<BYTE> hevc(23);
    hevc[0] = 1;
    hevc[21] = 0xfc | 1;
    check(nalLengthSize(false, hevc) == 2, "23-byte HEVC record gives its NAL length size");
    hevc.resize(22);
    check(nalLengthSize(false, hevc) == 0, "HEVC record too short for the field is ignored");
    check(nalLengthSize(false, {0, 0, 1, 0x40}) == 0, "HEVC Annex B has no NAL length size");
    check(nalLengthSize(true, {0xff, 0}) == 4, "VVC record with LengthSizeMinusOne 3");
    check(nalLengthSize(true, {0xfb, 0}) == 2, "VVC record with LengthSizeMinusOne 1");
    check(nalLengthSize(true, {0, 0, 0, 1}) == 0, "VVC Annex B has no NAL length size");
    puts("PASS: HEVC and VVC NAL length size from configuration records");
}
// A long form PSI section with its MPEG-2 CRC.
static std::vector<uint8_t> section(uint8_t tableId, uint16_t id, uint8_t number, std::vector<uint8_t> body)
{
    const size_t length = 5 + body.size() + 4;
    std::vector<uint8_t> s = {tableId, uint8_t(0xB0 | (length >> 8)), uint8_t(length), uint8_t(id >> 8), uint8_t(id),
                              0xC1, number, 1};
    s.insert(s.end(), body.begin(), body.end());
    // FFmpeg's table gives the CRC byte swapped, so its low byte goes first.
    const uint32_t crc = av_crc(av_crc_get_table(AV_CRC_32_IEEE), UINT32_MAX, s.data(), s.size());
    for (int shift = 0; shift < 32; shift += 8)
        s.push_back(uint8_t(crc >> shift));
    return s;
}
// ARIB audio component descriptor (0xC4) of an AAC component, stereo unless
// |type| says otherwise. |language2| makes it a multi-lingual (dual mono) one.
static std::vector<uint8_t> audioComponent(uint8_t tag, bool main, std::vector<uint8_t> text, uint8_t type = 0x03,
                                           const char *language2 = nullptr)
{
    const uint8_t flags = uint8_t((language2 ? 0x80 : 0) | (main ? 0x6F : 0x2F));
    std::vector<uint8_t> d = {0xC4, 0, 0xF2, type, tag, 0x0F, 0xFF, flags, 'j', 'p', 'n'};
    if (language2)
        d.insert(d.end(), language2, language2 + 3);
    d.insert(d.end(), text.begin(), text.end());
    d[1] = uint8_t(d.size() - 2);
    return d;
}
// EIT present/following actual section with one 30 minute event holding
// |descriptors|, starting at |hour|:|minute| (BCD) of MJD 0xE8B4.
static std::vector<uint8_t> eitSection(uint16_t serviceId, uint8_t number, std::vector<uint8_t> descriptors,
                                       uint8_t hour = 0x06, uint8_t minute = 0x30)
{
    std::vector<uint8_t> body = {0x7F, 0xE0, 0x7F, 0xE0, 1,    0x4E, 0x10, uint8_t(number),
                                 0xE8, 0xB4, hour, minute, 0x00, 0x00, 0x30, 0x00};
    body.push_back(uint8_t(0x80 | (descriptors.size() >> 8)));
    body.push_back(uint8_t(descriptors.size()));
    body.insert(body.end(), descriptors.begin(), descriptors.end());
    return section(0x4E, serviceId, number, body);
}
// TOT at |hour|:|minute|:|second| (BCD) of MJD 0xE8B4, without descriptors.
static std::vector<uint8_t> tot(uint8_t hour, uint8_t minute, uint8_t second)
{
    std::vector<uint8_t> s = {0x73, 0x70, 11, 0xE8, 0xB4, hour, minute, second, 0xF0, 0x00};
    const uint32_t crc = av_crc(av_crc_get_table(AV_CRC_32_IEEE), UINT32_MAX, s.data(), s.size());
    for (int shift = 0; shift < 32; shift += 8)
        s.push_back(uint8_t(crc >> shift));
    return s;
}
struct TsBuffer
{
    std::vector<uint8_t> data;
    std::map<int, int> cc;
    size_t pos = 0;
    void put(int pid, const std::vector<uint8_t> &sec)
    {
        std::vector<uint8_t> payload = {0}; // pointer_field
        payload.insert(payload.end(), sec.begin(), sec.end());
        for (size_t off = 0; off < payload.size(); off += 184)
        {
            uint8_t pkt[188];
            memset(pkt, 0xFF, sizeof(pkt));
            pkt[0] = 0x47;
            pkt[1] = uint8_t((off == 0 ? 0x40 : 0) | (pid >> 8));
            pkt[2] = uint8_t(pid);
            pkt[3] = uint8_t(0x10 | (cc[pid]++ & 0x0F));
            memcpy(pkt + 4, payload.data() + off, std::min<size_t>(184, payload.size() - off));
            data.insert(data.end(), pkt, pkt + sizeof(pkt));
        }
    }
};
static int readTs(void *opaque, uint8_t *buf, int size)
{
    auto *ts = static_cast<TsBuffer *>(opaque);
    const size_t n = std::min<size_t>(size, ts->data.size() - ts->pos);
    if (n == 0)
        return AVERROR_EOF;
    memcpy(buf, ts->data.data() + ts->pos, n);
    ts->pos += n;
    return static_cast<int>(n);
}
static int64_t seekTs(void *opaque, int64_t offset, int whence)
{
    auto *ts = static_cast<TsBuffer *>(opaque);
    if (whence == AVSEEK_SIZE)
        return ts->data.size();
    if ((whence & ~AVSEEK_FORCE) != SEEK_SET || offset < 0 || offset > (int64_t)ts->data.size())
        return -1;
    ts->pos = static_cast<size_t>(offset);
    return offset;
}
// Audio track names and languages come from the EIT audio component
// descriptors, matched to the streams by the PMT stream identifier descriptor.
static void eitTests(ILAVFSettingsInternal *settings)
{
    const std::vector<uint8_t> japanese = {0x46, 0x7C, 0x4B, 0x5C, 0x38, 0x6C};
    const std::vector<uint8_t> commentary = {0x46, 0x7C, 0x4B, 0x5C, 0x38, 0x6C, 0x21, 0x4A,
                                             0x32, 0x72, 0x40, 0x62, 0x21, 0x4B};
    check(AribDecodeString(commentary.data(), commentary.size()) == u8"日本語（解説）", "kanji SI text");
    // GR hiragana, LS1 alphanumeric full and middle size, LS3R katakana.
    const std::vector<uint8_t> kana = {0xA2, 0x0E, 0x41, 0x89, 0x41, 0x20, 0x1B, 0x7C, 0xA2};
    check(AribDecodeString(kana.data(), kana.size()) == u8"あＡA ア", "hiragana, alphanumeric and MSZ SI text");

    // Without a text the name comes from the main flag and component_type,
    // whose b4-b0 give the audio mode and b6-b5 audio for a person with
    // disabilities (ARIB STD-B10 Table 6-43); b7, dialog control, is not named.
    AribAudioComponent component;
    component.componentType = 0x03;
    component.mainComponent = true;
    component.language = "jpn";
    check(AribAudioComponentName(component) == u8"ステレオ", "main stereo without text");
    component.componentType = 0x83;
    check(AribAudioComponentName(component) == u8"ステレオ", "dialog control flag is not named");
    component.componentType = 0x51;
    check(AribAudioComponentName(component) == u8"22.2ch 聴覚障害者用", "22.2ch for the hearing impaired");
    component.componentType = 0x09;
    component.mainComponent = false;
    check(AribAudioComponentName(component) == u8"副音声 5.1ch", "secondary 5.1ch without text");
    component.componentType = 0x23;
    check(AribAudioComponentName(component) == u8"副音声 ステレオ 解説", "secondary stereo commentary");
    component.componentType = 0x00;
    check(AribAudioComponentName(component) == u8"副音声", "secondary reserved mode without text");
    component.mainComponent = true;
    check(AribAudioComponentName(component).empty(), "main reserved mode without text has no name");
    component.text = u8"日本語";
    check(AribAudioComponentName(component) == u8"日本語", "text wins over the audio mode");

    // The recorded event is the one the recording overlaps most; without a
    // TOT, the one describing every audio stream, the present one first.
    AribEITAudio events;
    AribAudioComponent mainAudio, subAudio;
    mainAudio.componentTag = 0x10;
    subAudio.componentTag = 0x11;
    events.events[0].audio = {mainAudio};
    events.events[1].audio = {mainAudio, subAudio};
    check(AribRecordedEvent(events, 1830, {0x10, 0x11}) == 1, "no TOT: the event describing every stream");
    check(AribRecordedEvent(events, 1830, {0x10}) == 0, "no TOT: the present event first");
    check(AribRecordedEvent(events, 1830, {0x12}) == -1, "no TOT: neither event describes the stream");
    events.events[0].audio = events.events[1].audio;
    events.events[0].startTime = 1000;
    events.events[0].duration = 1800;
    events.events[1].startTime = 2800;
    events.events[1].duration = 1800;
    events.firstTime = 2775;
    check(AribRecordedEvent(events, 1830, {0x10, 0x11}) == 1, "TOT: the event the recording overlaps most");
    events.firstTime = 1000;
    check(AribRecordedEvent(events, 1830, {0x10, 0x11}) == 0, "TOT: the present event when it is recorded");

    const uint16_t service = 0x0400, other = 0x0401;
    TsBuffer ts;
    ts.put(0x0000, section(0x00, 0x7FE0, 0, {0x04, 0x00, 0xE1, 0xF0}));
    ts.put(0x01F0, section(0x02, service, 0,
                           {0xE1, 0x10, 0xF0, 0x00,                               // PCR PID, no program info
                            0x0F, 0xE1, 0x10, 0xF0, 0x03, 0x52, 0x01, 0x10,       // AAC, component tag 0x10
                            0x0F, 0xE1, 0x11, 0xF0, 0x03, 0x52, 0x01, 0x11,      // AAC, component tag 0x11
                            0x0F, 0xE1, 0x12, 0xF0, 0x03, 0x52, 0x01, 0x12}));    // AAC, component tag 0x12
    // The recording starts at 06:29:35 in the previous event, which describes
    // every audio stream as well; the recorded 06:30 event, still the
    // following one, has commentary and a dual mono component without a text.
    ts.put(0x0014, tot(0x06, 0x29, 0x35));
    auto present = audioComponent(0x10, true, commentary);
    auto second = audioComponent(0x11, false, japanese);
    present.insert(present.end(), second.begin(), second.end());
    second = audioComponent(0x12, false, japanese);
    present.insert(present.end(), second.begin(), second.end());
    ts.put(0x0012, eitSection(service, 0, present, 0x06, 0x00));
    auto following = audioComponent(0x10, true, japanese);
    second = audioComponent(0x11, false, commentary);
    following.insert(following.end(), second.begin(), second.end());
    second = audioComponent(0x12, false, {}, 0x02, "eng");
    following.insert(following.end(), second.begin(), second.end());
    const auto followingSection = eitSection(service, 1, following);
    AribEITAudio parsed;
    check(AribParseEITAudioComponents(followingSection.data(), followingSection.size(), service, parsed) &&
              parsed.Find(1, 0x11) && parsed.Find(1, 0x11)->text == u8"日本語（解説）",
          "EIT section parsed into audio components");
    ts.put(0x0012, followingSection);
    // A corrupt copy and another service's EIT must not rename the tracks.
    auto corrupt = eitSection(service, 1, audioComponent(0x11, false, japanese));
    corrupt.back() ^= 0xFF;
    ts.put(0x0012, corrupt);
    ts.put(0x0012, eitSection(other, 1, present));
    while (ts.data.size() < 64 * 188)
        ts.put(0x1FFF, {});

    const int bufferSize = 4096;
    AVIOContext *pb = avio_alloc_context(static_cast<uint8_t *>(av_malloc(bufferSize)), bufferSize, 0, &ts, readTs,
                                         nullptr, seekTs);
    AVFormatContext *ctx = avformat_alloc_context();
    ctx->pb = pb;
    ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
    check(avformat_open_input(&ctx, nullptr, av_find_input_format("mpegts"), nullptr) == 0, "open synthetic TS");
    check(avformat_find_stream_info(ctx, nullptr) >= 0, "probe synthetic TS");
    ctx->duration = 1830 * int64_t(AV_TIME_BASE); // 06:29:35 to 07:00:05
    {
        CCritSec lock;
        Demuxer d(&lock, settings, ctx);
        check(d.createStreams() == S_OK, "create streams of synthetic TS");
        check(d.audioCount() == 3, "all audio streams");
        const auto &main = d.audioStream(0);
        const auto &sub = d.audioStream(1);
        const auto &dual = d.audioStream(2);
        check(main.trackName == u8"日本語", "main audio named from the following event");
        check(sub.trackName == u8"日本語（解説）", "commentary named from the following event");
        check(dual.trackName == u8"副音声 二重音声（日本語/英語）", "dual mono without text named from its mode");
        check(main.language == "jpn" && sub.language == "jpn" && dual.language == "jpn",
              "audio language from the EIT");
        check(sub.lcid == ProbeLangForLCID("jpn"), "audio LCID from the EIT language");
        check(sub.streamInfo->codecInfo.find(u8"日本語（解説） [jpn]") != std::string::npos,
              "stream selection name carries the EIT name");
    }
    av_freep(&pb->buffer);
    avio_context_free(&pb);
    puts("PASS: audio track names and languages from the EIT audio component descriptor");
}
int wmain(int argc, wchar_t **argv)
{
    try
    {
        check(argc == 2, "provide splitter DLL path");
        // AribStreamComponentTag reads FFStream, a private FFmpeg structure.
        // Even a minor version mismatch can move its fields while the public
        // DLL ABI remains compatible. Diagnose stale dependencies before tests.
        const unsigned runtimeVersion = avformat_version();
        if (runtimeVersion != LIBAVFORMAT_VERSION_INT)
        {
            fprintf(stderr,
                    "FFmpeg version mismatch: libavformat headers=%u.%u.%u, DLL=%u.%u.%u. "
                    "Rebuild FFmpeg with build_ffmpeg.sh x64, then rebuild ARIBSplitter.\n",
                    LIBAVFORMAT_VERSION_MAJOR, LIBAVFORMAT_VERSION_MINOR, LIBAVFORMAT_VERSION_MICRO,
                    runtimeVersion >> 16, (runtimeVersion >> 8) & 0xFF, runtimeVersion & 0xFF);
            throw std::runtime_error("FFmpeg headers and runtime must match for private FFStream access");
        }
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
        glyphRunTests(settings);
        seekableTests(settings);
        eitTests(settings);
        videoHelperTests();
        puts("ALL DEMUXER TESTS PASSED");
        return 0;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
