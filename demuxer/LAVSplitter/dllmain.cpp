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

// Based on the SampleParser Template by GDCL
// --------------------------------------------------------------------------------
// Copyright (c) GDCL 2004. All Rights Reserved.
// You are free to re-use this as the basis for your own filter development,
// provided you retain this copyright notice in the source.
// http://www.gdcl.co.uk
// --------------------------------------------------------------------------------

#include "stdafx.h"

// Initialize the GUIDs
#include <InitGuid.h>

#include <qnetwork.h>
#include "LAVSplitter.h"
#include "moreuuids.h"

#include "registry.h"
#include "IGraphRebuildDelegate.h"
#include "IMediaSideDataFFmpeg.h"
#include "ILAVDynamicAllocator.h"

// The GUID we use to register the splitter media types
DEFINE_GUID(MEDIATYPE_LAVSplitter, 0x9c53931c, 0x7d5a, 0x4a75, 0xb2, 0x6f, 0x4e, 0x51, 0x65, 0x4d, 0xb2, 0xc0);

// --- COM factory table and registration code --------------

const AMOVIESETUP_MEDIATYPE sudMediaTypes[] = {
    {&MEDIATYPE_Stream, &MEDIASUBTYPE_NULL},
};

const AMOVIESETUP_PIN sudOutputPins[] = {{
                                             L"Output",   // pin name
                                             FALSE,       // is rendered?
                                             TRUE,        // is output?
                                             FALSE,       // zero instances allowed?
                                             TRUE,        // many instances allowed?
                                             &CLSID_NULL, // connects to filter (for bridge pins)
                                             nullptr,     // connects to pin (for bridge pins)
                                             0,           // count of registered media types
                                             nullptr      // list of registered media types
                                         },
                                         {
                                             L"Input",         // pin name
                                             FALSE,            // is rendered?
                                             FALSE,            // is output?
                                             FALSE,            // zero instances allowed?
                                             FALSE,            // many instances allowed?
                                             &CLSID_NULL,      // connects to filter (for bridge pins)
                                             nullptr,          // connects to pin (for bridge pins)
                                             1,                // count of registered media types
                                             &sudMediaTypes[0] // list of registered media types
                                         }};

const AMOVIESETUP_FILTER sudFilterReg = {&__uuidof(CLAVSplitter), // filter clsid
                                         L"ARIB Splitter",        // filter name
                                         MERIT_PREFERRED + 4,     // merit
                                         2,                       // count of registered pins
                                         sudOutputPins,           // list of pins to register
                                         CLSID_LegacyAmFilterCategory};

const AMOVIESETUP_FILTER sudFilterRegSource = {&__uuidof(CLAVSplitterSource), // filter clsid
                                               L"ARIB Splitter Source",       // filter name
                                               MERIT_PREFERRED + 4,           // merit
                                               1,                             // count of registered pins
                                               sudOutputPins,                 // list of pins to register
                                               CLSID_LegacyAmFilterCategory};

static const LPCWSTR kTsExtensions[] = {
    L".ts",
    L".m2ts",
    L".mts",
    L".m2t",
};

static HRESULT RegisterExtensionSourceFilter(LPCWSTR ext, REFCLSID clsid)
{
    WCHAR clsidStr[64] = {};
    if (StringFromGUID2(clsid, clsidStr, ARRAYSIZE(clsidStr)) == 0)
        return E_FAIL;

    WCHAR keyPath[MAX_PATH] = {};
    swprintf_s(keyPath, ARRAYSIZE(keyPath), L"Media Type\\Extensions\\%s", ext);

    HKEY hKey = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(r);

    r = RegSetValueExW(hKey, L"Source Filter", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(clsidStr),
                       static_cast<DWORD>((wcslen(clsidStr) + 1) * sizeof(WCHAR)));
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(r);
}

static void UnregisterExtensionSourceFilter(LPCWSTR ext, REFCLSID clsid)
{
    WCHAR clsidStr[64] = {};
    if (StringFromGUID2(clsid, clsidStr, ARRAYSIZE(clsidStr)) == 0)
        return;

    WCHAR keyPath[MAX_PATH] = {};
    swprintf_s(keyPath, ARRAYSIZE(keyPath), L"Media Type\\Extensions\\%s", ext);

    HKEY hKey = nullptr;
    LONG r = RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey);
    if (r != ERROR_SUCCESS)
        return;

    WCHAR current[64] = {};
    DWORD type = 0;
    DWORD bytes = sizeof(current);
    r = RegQueryValueExW(hKey, L"Source Filter", nullptr, &type, reinterpret_cast<BYTE *>(current), &bytes);
    if (r == ERROR_SUCCESS && type == REG_SZ && _wcsicmp(current, clsidStr) == 0)
        RegDeleteValueW(hKey, L"Source Filter");

    RegCloseKey(hKey);
}

static void UnregisterStreamSourceFilterIfOwned(REFGUID subtype, REFCLSID clsid)
{
    WCHAR clsidStr[64] = {};
    if (StringFromGUID2(clsid, clsidStr, ARRAYSIZE(clsidStr)) == 0)
        return;

    WCHAR majortypeStr[64] = {};
    WCHAR subtypeStr[64] = {};
    if (StringFromGUID2(MEDIATYPE_Stream, majortypeStr, ARRAYSIZE(majortypeStr)) == 0 ||
        StringFromGUID2(subtype, subtypeStr, ARRAYSIZE(subtypeStr)) == 0)
        return;

    WCHAR keyPath[MAX_PATH] = {};
    swprintf_s(keyPath, ARRAYSIZE(keyPath), L"Media Type\\%s\\%s", majortypeStr, subtypeStr);

    HKEY hKey = nullptr;
    LONG r = RegOpenKeyExW(HKEY_CLASSES_ROOT, keyPath, 0, KEY_QUERY_VALUE, &hKey);
    if (r != ERROR_SUCCESS)
        return;

    WCHAR current[64] = {};
    DWORD type = 0;
    DWORD bytes = sizeof(current);
    r = RegQueryValueExW(hKey, L"Source Filter", nullptr, &type, reinterpret_cast<BYTE *>(current), &bytes);
    RegCloseKey(hKey);

    if (r == ERROR_SUCCESS && type == REG_SZ && _wcsicmp(current, clsidStr) == 0)
        RegDeleteTreeW(HKEY_CLASSES_ROOT, keyPath);
}

static HRESULT RegisterTsSourceFilters()
{
    std::list<LPCWSTR> chkbytes;
    chkbytes.push_back(L"0,1,,47");
    chkbytes.push_back(L"188,1,,47");
    chkbytes.push_back(L"376,1,,47");
    RegisterSourceFilter(__uuidof(CLAVSplitterSource), MEDIASUBTYPE_MPEG2_TRANSPORT,
                         chkbytes, L".ts", L".m2ts", L".mts", L".m2t", nullptr);

    HRESULT hr = S_OK;
    for (LPCWSTR ext : kTsExtensions)
    {
        HRESULT hrExt = RegisterExtensionSourceFilter(ext, __uuidof(CLAVSplitterSource));
        if (FAILED(hrExt) && SUCCEEDED(hr))
            hr = hrExt;
    }
    return hr;
}

static void UnregisterTsSourceFilters()
{
    for (LPCWSTR ext : kTsExtensions)
        UnregisterExtensionSourceFilter(ext, __uuidof(CLAVSplitterSource));
    UnregisterStreamSourceFilterIfOwned(MEDIASUBTYPE_MPEG2_TRANSPORT, __uuidof(CLAVSplitterSource));
}
// --- COM factory table and registration code --------------

// DirectShow base class COM factory requires this table,
// declaring all the COM objects in this DLL
CFactoryTemplate g_Templates[] = {
    // one entry for each CoCreate-able object
    {sudFilterReg.strName, sudFilterReg.clsID, CreateInstance<CLAVSplitter>, nullptr, &sudFilterReg},
    {sudFilterRegSource.strName, sudFilterRegSource.clsID, CreateInstance<CLAVSplitterSource>, nullptr,
     &sudFilterRegSource},
    // This entry is for the property page.
    {L"ARIB Splitter Properties", &CLSID_LAVSplitterSettingsProp, CreateInstance<CLAVSplitterSettingsProp>, nullptr,
     nullptr},
    {L"ARIB Splitter Input Formats", &CLSID_LAVSplitterFormatsProp, CreateInstance<CLAVSplitterFormatsProp>, nullptr,
     nullptr}};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

// self-registration entrypoint
STDAPI DllRegisterServer()
{
    // base classes will handle registration using the factory template table
    HRESULT hr = AMovieDllRegisterServer2(true);
    if (FAILED(hr))
        return hr;

    // Write TS source mappings last, because generic filter registration may
    // recreate extension keys while registering the factory template table.
    return RegisterTsSourceFilters();
}

STDAPI DllUnregisterServer()
{
    UnregisterTsSourceFilters();

    // base classes will handle de-registration using the factory template table
    return AMovieDllRegisterServer2(false);
}

// if we declare the correct C runtime entrypoint and then forward it to the DShow base
// classes we will be sure that both the C/C++ runtimes and the base classes are initialized
// correctly
extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);
BOOL WINAPI DllMain(HANDLE hDllHandle, DWORD dwReason, LPVOID lpReserved)
{
    return DllEntryPoint(reinterpret_cast<HINSTANCE>(hDllHandle), dwReason, lpReserved);
}

void CALLBACK OpenConfiguration(HWND hwnd, HINSTANCE hinst, LPSTR lpszCmdLine, int nCmdShow)
{
    HRESULT hr = S_OK;
    CUnknown *pInstance = CreateInstance<CLAVSplitter>(nullptr, &hr);
    IBaseFilter *pFilter = nullptr;
    pInstance->NonDelegatingQueryInterface(IID_IBaseFilter, (void **)&pFilter);
    if (pFilter)
    {
        pFilter->AddRef();
        CBaseDSPropPage::ShowPropPageDialog(pFilter);
    }
    delete pInstance;
}
