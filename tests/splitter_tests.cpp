// Run the production input callbacks with deterministic IAsyncReader data.
#include "stdafx.h"
#include <initguid.h>
#include <qnetwork.h>
#include "LAVSplitter.h"
#include "InputPin.h"
#include "moreuuids.h"
#include "IGraphRebuildDelegate.h"
#include "IMediaSideDataFFmpeg.h"
#include "ILAVDynamicAllocator.h"
#include <algorithm>
#include <cstdio>
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
