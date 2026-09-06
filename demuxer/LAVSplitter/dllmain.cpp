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

// The TS media type and extension keys are shared with every other DirectShow
// filter on the machine, so registration saves whatever was there before and
// uninstallation puts it back instead of deleting the key outright.
static const LPCWSTR kRegistryBackupRoot = L"Software\\ARIBSplitter\\RegistryBackup";
static const LPCWSTR kBackupExistedValue = L"__ARIBSplitterKeyExisted";

static std::wstring GetClsidString(REFCLSID clsid)
{
    WCHAR clsidStr[64] = {};
    if (StringFromGUID2(clsid, clsidStr, ARRAYSIZE(clsidStr)) == 0)
        return {};
    return clsidStr;
}

static std::wstring GetStreamSubtypeKeyPath(REFGUID subtype)
{
    WCHAR majortypeStr[64] = {};
    WCHAR subtypeStr[64] = {};
    if (StringFromGUID2(MEDIATYPE_Stream, majortypeStr, ARRAYSIZE(majortypeStr)) == 0 ||
        StringFromGUID2(subtype, subtypeStr, ARRAYSIZE(subtypeStr)) == 0)
        return {};

    WCHAR keyPath[MAX_PATH] = {};
    swprintf_s(keyPath, ARRAYSIZE(keyPath), L"Media Type\\%s\\%s", majortypeStr, subtypeStr);
    return keyPath;
}

static std::wstring GetExtensionKeyPath(LPCWSTR ext)
{
    WCHAR keyPath[MAX_PATH] = {};
    swprintf_s(keyPath, ARRAYSIZE(keyPath), L"Media Type\\Extensions\\%s", ext);
    return keyPath;
}

// Read the "Source Filter" value of a key, if it is a string.
static bool ReadSourceFilterValue(HKEY hKey, std::wstring &value)
{
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(hKey, L"Source Filter", nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes < sizeof(WCHAR))
        return false;

    std::vector<WCHAR> buf(bytes / sizeof(WCHAR) + 1, L'\0');
    if (RegQueryValueExW(hKey, L"Source Filter", nullptr, &type, reinterpret_cast<BYTE *>(buf.data()), &bytes) !=
        ERROR_SUCCESS)
        return false;

    value = buf.data();
    return true;
}

// Copy every string value of |targetPath| into the backup area. Does nothing if
// a backup already exists, so registering twice cannot overwrite the original.
static void BackupRegistryKey(LPCWSTR targetPath, LPCWSTR backupName)
{
    std::wstring backupPath = std::wstring(kRegistryBackupRoot) + L"\\" + backupName;

    HKEY hExisting = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupPath.c_str(), 0, KEY_QUERY_VALUE, &hExisting) == ERROR_SUCCESS)
    {
        RegCloseKey(hExisting);
        return;
    }

    HKEY hBackup = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, backupPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                        nullptr, &hBackup, nullptr) != ERROR_SUCCESS)
        return;

    HKEY hTarget = nullptr;
    DWORD existed = (RegOpenKeyExW(HKEY_CLASSES_ROOT, targetPath, 0, KEY_QUERY_VALUE, &hTarget) == ERROR_SUCCESS) ? 1 : 0;
    RegSetValueExW(hBackup, kBackupExistedValue, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&existed),
                   sizeof(existed));

    if (existed)
    {
        for (DWORD i = 0;; i++)
        {
            WCHAR name[16384] = {};
            DWORD nameLen = ARRAYSIZE(name);
            DWORD type = 0;
            DWORD bytes = 0;
            LONG r = RegEnumValueW(hTarget, i, name, &nameLen, nullptr, &type, nullptr, &bytes);
            if (r == ERROR_NO_MORE_ITEMS)
                break;
            if (r != ERROR_SUCCESS || type != REG_SZ)
                continue;

            std::vector<BYTE> data(bytes ? bytes : sizeof(WCHAR), 0);
            DWORD dataLen = (DWORD)data.size();
            nameLen = ARRAYSIZE(name);
            if (RegEnumValueW(hTarget, i, name, &nameLen, nullptr, &type, data.data(), &dataLen) != ERROR_SUCCESS)
                continue;

            RegSetValueExW(hBackup, name, 0, REG_SZ, data.data(), dataLen);
        }
        RegCloseKey(hTarget);
    }

    RegCloseKey(hBackup);
}

// Put |targetPath| back the way BackupRegistryKey() found it, but only while the
// key still points at us - if another filter has taken over since, leave it be.
static void RestoreRegistryKey(LPCWSTR targetPath, LPCWSTR backupName, const std::wstring &ourClsid)
{
    HKEY hTarget = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, targetPath, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hTarget) != ERROR_SUCCESS)
        return;

    std::wstring current;
    bool ours = ReadSourceFilterValue(hTarget, current) && _wcsicmp(current.c_str(), ourClsid.c_str()) == 0;
    RegCloseKey(hTarget);
    if (!ours)
        return;

    std::wstring backupPath = std::wstring(kRegistryBackupRoot) + L"\\" + backupName;
    HKEY hBackup = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, backupPath.c_str(), 0, KEY_QUERY_VALUE, &hBackup) != ERROR_SUCCESS)
    {
        // No backup (installed by an older build): drop only our own value.
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, targetPath, 0, KEY_SET_VALUE, &hTarget) == ERROR_SUCCESS)
        {
            RegDeleteValueW(hTarget, L"Source Filter");
            RegCloseKey(hTarget);
        }
        return;
    }

    DWORD existed = 0;
    DWORD type = 0;
    DWORD bytes = sizeof(existed);
    if (RegQueryValueExW(hBackup, kBackupExistedValue, nullptr, &type, reinterpret_cast<BYTE *>(&existed), &bytes) !=
            ERROR_SUCCESS ||
        type != REG_DWORD)
        existed = 1;

    if (!existed)
    {
        // The key did not exist before we installed, so it is ours to remove.
        RegCloseKey(hBackup);
        RegDeleteTreeW(HKEY_CLASSES_ROOT, targetPath);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, backupPath.c_str());
        return;
    }

    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, targetPath, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &hTarget) == ERROR_SUCCESS)
    {
        // Remove the string values we added (anything not in the backup), then
        // write the saved ones back.
        std::vector<std::wstring> toDelete;
        for (DWORD i = 0;; i++)
        {
            WCHAR name[16384] = {};
            DWORD nameLen = ARRAYSIZE(name);
            DWORD valType = 0;
            LONG r = RegEnumValueW(hTarget, i, name, &nameLen, nullptr, &valType, nullptr, nullptr);
            if (r == ERROR_NO_MORE_ITEMS)
                break;
            if (r != ERROR_SUCCESS || valType != REG_SZ)
                continue;

            DWORD savedBytes = 0;
            DWORD savedType = 0;
            if (RegQueryValueExW(hBackup, name, nullptr, &savedType, nullptr, &savedBytes) != ERROR_SUCCESS)
                toDelete.push_back(name);
        }
        for (const auto &name : toDelete)
            RegDeleteValueW(hTarget, name.c_str());

        for (DWORD i = 0;; i++)
        {
            WCHAR name[16384] = {};
            DWORD nameLen = ARRAYSIZE(name);
            DWORD valType = 0;
            DWORD valBytes = 0;
            LONG r = RegEnumValueW(hBackup, i, name, &nameLen, nullptr, &valType, nullptr, &valBytes);
            if (r == ERROR_NO_MORE_ITEMS)
                break;
            if (r != ERROR_SUCCESS || valType != REG_SZ)
                continue;

            std::vector<BYTE> data(valBytes ? valBytes : sizeof(WCHAR), 0);
            DWORD dataLen = (DWORD)data.size();
            nameLen = ARRAYSIZE(name);
            if (RegEnumValueW(hBackup, i, name, &nameLen, nullptr, &valType, data.data(), &dataLen) != ERROR_SUCCESS)
                continue;

            RegSetValueExW(hTarget, name, 0, REG_SZ, data.data(), dataLen);
        }
        RegCloseKey(hTarget);
    }

    RegCloseKey(hBackup);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, backupPath.c_str());
}

static HRESULT RegisterExtensionSourceFilter(LPCWSTR ext, REFCLSID clsid)
{
    std::wstring clsidStr = GetClsidString(clsid);
    if (clsidStr.empty())
        return E_FAIL;

    std::wstring keyPath = GetExtensionKeyPath(ext);
    BackupRegistryKey(keyPath.c_str(), ext + 1 /* skip the leading dot */);

    HKEY hKey = nullptr;
    LONG r = RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath.c_str(), 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr);
    if (r != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(r);

    r = RegSetValueExW(hKey, L"Source Filter", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(clsidStr.c_str()),
                       static_cast<DWORD>((clsidStr.size() + 1) * sizeof(WCHAR)));
    RegCloseKey(hKey);
    return HRESULT_FROM_WIN32(r);
}

static void UnregisterExtensionSourceFilter(LPCWSTR ext, REFCLSID clsid)
{
    std::wstring clsidStr = GetClsidString(clsid);
    if (clsidStr.empty())
        return;

    RestoreRegistryKey(GetExtensionKeyPath(ext).c_str(), ext + 1, clsidStr);
}

static void UnregisterStreamSourceFilterIfOwned(REFGUID subtype, REFCLSID clsid)
{
    std::wstring clsidStr = GetClsidString(clsid);
    std::wstring keyPath = GetStreamSubtypeKeyPath(subtype);
    if (clsidStr.empty() || keyPath.empty())
        return;

    RestoreRegistryKey(keyPath.c_str(), L"MPEG2_TRANSPORT", clsidStr);
}

static HRESULT RegisterTsSourceFilters()
{
    std::wstring streamKeyPath = GetStreamSubtypeKeyPath(MEDIASUBTYPE_MPEG2_TRANSPORT);
    if (!streamKeyPath.empty())
        BackupRegistryKey(streamKeyPath.c_str(), L"MPEG2_TRANSPORT");

    // RegisterSourceFilter deletes these extension keys. Save them before
    // calling it, otherwise RegisterExtensionSourceFilter sees empty keys.
    for (LPCWSTR ext : kTsExtensions)
        BackupRegistryKey(GetExtensionKeyPath(ext).c_str(), ext + 1);

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
