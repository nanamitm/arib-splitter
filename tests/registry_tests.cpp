// Exercise the production registration helpers under redirected registry roots.
#include "../demuxer/LAVSplitter/dllmain.cpp"
#include <cstdio>
#include <stdexcept>
static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
struct RegistrySandbox
{
    std::wstring path;
    HKEY root = nullptr, classes = nullptr, machine = nullptr;
    RegistrySandbox()
    {
        path = L"Software\\ARIBSplitterTests\\" + std::to_wstring(GetCurrentProcessId());
        check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root,
                              nullptr) == 0,
              "sandbox root");
        check(RegCreateKeyExW(root, L"Classes", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &classes, nullptr) == 0,
              "classes root");
        check(RegCreateKeyExW(root, L"Machine", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &machine, nullptr) == 0,
              "machine root");
        check(RegOverridePredefKey(HKEY_CLASSES_ROOT, classes) == 0, "redirect HKCR");
        check(RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine) == 0, "redirect HKLM");
    }
    ~RegistrySandbox()
    {
        RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
        RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        if (classes)
            RegCloseKey(classes);
        if (machine)
            RegCloseKey(machine);
        if (root)
            RegCloseKey(root);
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};
static void set(const std::wstring &path, const wchar_t *name, const std::wstring &value)
{
    HKEY key;
    check(RegCreateKeyExW(HKEY_CLASSES_ROOT, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == 0,
          "create test mapping");
    LONG r = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
                            DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    check(r == 0, "set test mapping");
}
static std::wstring get(const std::wstring &path, const wchar_t *name)
{
    wchar_t value[1024] = {};
    DWORD bytes = sizeof(value);
    check(RegGetValueW(HKEY_CLASSES_ROOT, path.c_str(), name, RRF_RT_REG_SZ, nullptr, value, &bytes) == 0,
          "read test mapping");
    return value;
}
int main()
{
    try
    {
        RegistrySandbox sandbox;
        const std::wstring oldClsid = L"{00000000-0000-0000-0000-000000000001}";
        const std::wstring newOwner = L"{00000000-0000-0000-0000-000000000002}";
        for (auto ext : {L".ts", L".m2ts", L".mts"})
        {
            set(GetExtensionKeyPath(ext), L"Source Filter", oldClsid);
            set(GetExtensionKeyPath(ext), L"Subtype", L"original subtype");
        }
        auto streamPath = GetStreamSubtypeKeyPath(MEDIASUBTYPE_MPEG2_TRANSPORT);
        set(streamPath, L"Source Filter", oldClsid);
        set(streamPath, L"0", L"original signature");
        check(SUCCEEDED(RegisterTsSourceFilters()), "register");
        check(SUCCEEDED(RegisterTsSourceFilters()), "repeat registration");
        for (auto ext : kTsExtensions)
            check(get(GetExtensionKeyPath(ext), L"Source Filter") == GetClsidString(__uuidof(CLAVSplitterSource)),
                  "new mapping");
        set(GetExtensionKeyPath(L".mts"), L"Source Filter", newOwner);
        UnregisterTsSourceFilters();
        for (auto ext : {L".ts", L".m2ts"})
        {
            check(get(GetExtensionKeyPath(ext), L"Source Filter") == oldClsid, "restore original owner");
            check(get(GetExtensionKeyPath(ext), L"Subtype") == L"original subtype", "restore original metadata");
        }
        check(get(GetExtensionKeyPath(L".mts"), L"Source Filter") == newOwner, "preserve subsequent owner");
        HKEY missing = nullptr;
        check(RegOpenKeyExW(HKEY_CLASSES_ROOT, GetExtensionKeyPath(L".m2t").c_str(), 0, KEY_READ, &missing) ==
                  ERROR_FILE_NOT_FOUND,
              "remove originally absent mapping");
        check(get(streamPath, L"Source Filter") == oldClsid, "restore stream owner");
        check(get(streamPath, L"0") == L"original signature", "restore stream signature");
        puts("PASS: original mappings, repeat install, absent key, subsequent owner, stream mapping");
        return 0;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
