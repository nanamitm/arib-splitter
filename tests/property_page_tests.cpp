#include <windows.h>
#include <commctrl.h>
#include <dshow.h>
#include <ocidl.h>
#include <atlbase.h>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "strmiids.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Site : IPropertyPageSite {
    ULONG refs = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (riid != IID_IUnknown && riid != IID_IPropertyPageSite) return E_NOINTERFACE;
        *out = this; AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetLocaleID(LCID* value) override { *value = GetUserDefaultLCID(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPageContainer(IUnknown** value) override { *value = nullptr; return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE TranslateAccelerator(MSG*) override { return S_FALSE; }
};
struct RegistrySandbox {
    HKEY root = nullptr, theme = nullptr;
    std::wstring path = L"Software\\ARIBSplitterThemeTest-" + std::to_wstring(GetCurrentProcessId());
    RegistrySandbox() {
        check(RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS, "sandbox key");
        check(RegCreateKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &theme, nullptr) == ERROR_SUCCESS, "theme key");
        check(RegOverridePredefKey(HKEY_CURRENT_USER, root) == ERROR_SUCCESS, "redirect test process HKCU");
    }
    void set(bool dark) { DWORD light = !dark; check(RegSetValueExW(theme, L"AppsUseLightTheme", 0, REG_DWORD, reinterpret_cast<BYTE*>(&light), sizeof(light)) == ERROR_SUCCESS, "set test theme"); }
    ~RegistrySandbox() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (theme) RegCloseKey(theme);
        if (root) RegCloseKey(root);
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    }
};
static void pump() { MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); } }
static BOOL CALLBACK settings(HWND hwnd, LPARAM) {
    SendMessageW(hwnd, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet")); return TRUE;
}
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc >= 2, "usage: property_page_tests <bin directory> [splitter|audio page-index dark|light]");
        check(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "COM init");
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES}; InitCommonControlsEx(&icc);
        RegistrySandbox registry;
        bool preview = argc == 5;
        for (int filterIndex = 0; filterIndex < 2; ++filterIndex) {
            if (preview && ((wcscmp(argv[2], L"audio") == 0) != (filterIndex == 1))) continue;
            std::wstring dllPath = std::wstring(argv[1]) + (filterIndex ? L"\\ARIBAudio.ax" : L"\\ARIBSplitter.ax");
            HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            check(dll != nullptr, "load filter DLL");
            auto getClass = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject"));
            check(getClass != nullptr, "class factory export");
            CLSID clsid;
            CLSIDFromString(filterIndex ? L"{40920401-7808-4AB3-B7EF-6EEAF8C262F0}" : L"{1AA767C2-BF31-4791-B65A-474678685956}", &clsid);
            CComPtr<IClassFactory> factory;
            check(SUCCEEDED(getClass(clsid, IID_PPV_ARGS(&factory))), "filter factory");
            CComPtr<IUnknown> filter;
            check(SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&filter))), "create filter");
            CComQIPtr<ISpecifyPropertyPages> pages(filter);
            check(pages != nullptr, "property pages interface");
            CAUUID ids{}; check(SUCCEEDED(pages->GetPages(&ids)), "page IDs");
            std::vector<CLSID> pageIds(ids.pElems, ids.pElems + ids.cElems);
            if (filterIndex) {
                CLSID status;
                CLSIDFromString(L"{C5489907-9B61-4589-8000-2D8E07F8D76C}", &status);
                pageIds.push_back(status); // Also test Status without a connected audio stream.
            }
            for (ULONG i = 0; i < pageIds.size(); ++i) {
                if (preview && i != static_cast<ULONG>(_wtoi(argv[3]))) continue;
                CComPtr<IClassFactory> pageFactory;
                check(SUCCEEDED(getClass(pageIds[i], IID_PPV_ARGS(&pageFactory))), "page factory");
                CComPtr<IPropertyPage> page;
                check(SUCCEEDED(pageFactory->CreateInstance(nullptr, IID_PPV_ARGS(&page))), "create page");
                Site site;
                check(SUCCEEDED(page->SetPageSite(&site)), "page site");
                IUnknown* object = filter;
                check(SUCCEEDED(page->SetObjects(1, &object)), "page object");
                PROPPAGEINFO info{sizeof(info)}; check(SUCCEEDED(page->GetPageInfo(&info)), "page info");
                std::wstring title = L"ARIB theme preview - " + std::wstring(info.pszTitle);
                RECT client{0, 0, info.size.cx, info.size.cy};
                RECT frame = client; AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
                HWND host = CreateWindowExW(0, L"STATIC", title.c_str(), WS_OVERLAPPEDWINDOW,
                    100, 100, frame.right-frame.left, frame.bottom-frame.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                check(host != nullptr, "host window");
                for (int cycle = 0; cycle < (preview ? 1 : 2); ++cycle) {
                    registry.set(preview ? wcscmp(argv[4], L"dark") == 0 : cycle == 0);
                    check(SUCCEEDED(page->Activate(host, &client, FALSE)), "activate");
                    const HRESULT initialDirty = page->IsPageDirty();
                    HWND dialog = FindWindowExW(host, nullptr, L"#32770", nullptr);
                    check(dialog != nullptr, "page dialog");
                    if (preview) { ShowWindow(host, SW_SHOW); ShowWindow(host, SW_SHOW); UpdateWindow(host); }
                    for (int mode = 0; mode < (preview ? 1 : 3); ++mode) {
                        bool dark = preview ? wcscmp(argv[4], L"dark") == 0 : mode != 1;
                        registry.set(dark);
                        EnumThreadWindows(GetCurrentThreadId(), settings, 0);
                        pump();
                        HDC dc = GetDC(dialog);
                        HBRUSH brush = reinterpret_cast<HBRUSH>(SendMessageW(dialog, WM_CTLCOLORDLG, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(dialog)));
                        LOGBRUSH lb{}; check(GetObjectW(brush, sizeof(lb), &lb) != 0, "background brush");
                        ReleaseDC(dialog, dc);
                        bool isDark = GetRValue(lb.lbColor) < 128;
                        check(isDark == dark, "page background follows theme");
                        check(page->IsPageDirty() == initialDirty, "theme must not change dirty state");
                        wprintf(L"PASS %s page %lu cycle %d %s background=%06lx\n", filterIndex ? L"audio" : L"splitter", i, cycle, dark ? L"dark" : L"light", lb.lbColor);
                    }
                    if (preview) { MSG msg; while (IsWindow(host) && GetMessageW(&msg, nullptr, 0, 0) > 0) { if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) break; TranslateMessage(&msg); DispatchMessageW(&msg); } }
                    check(SUCCEEDED(page->Deactivate()), "deactivate");
                }
                DestroyWindow(host);
                page->SetObjects(0, nullptr); page->SetPageSite(nullptr);
                CoTaskMemFree(info.pszTitle); CoTaskMemFree(info.pszDocString); CoTaskMemFree(info.pszHelpFile);
            }
            CoTaskMemFree(ids.pElems);
            pages.Release(); filter.Release(); factory.Release();
            auto canUnload = reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(dll, "DllCanUnloadNow"));
            check(canUnload && canUnload() == S_OK, "filter can unload after closing pages");
            FreeLibrary(dll);
        }
        CoUninitialize(); puts("Property page tests passed."); return 0;
    } catch (const std::exception& e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
