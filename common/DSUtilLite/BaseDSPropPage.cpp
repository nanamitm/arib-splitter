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

#include "stdafx.h"
#include "BaseDSPropPage.h"
#include "../../thirdparty/darkmodelib/include/Darkmodelib.h"

CBaseDSPropPage::CBaseDSPropPage(LPCTSTR pName, __inout_opt LPUNKNOWN pUnk, int DialogId, int TitleId)
    : CBasePropertyPage(pName, pUnk, DialogId, TitleId)
{
}

CBaseDSPropPage::~CBaseDSPropPage()
{
    if (m_hThemeObserver)
        DestroyWindow(m_hThemeObserver);
}

STDMETHODIMP CBaseDSPropPage::Activate(HWND hwndParent, LPCRECT pRect, BOOL fModal)
{
    dmlib::initDarkMode();
    // Refresh even when the system changed theme while all pages were closed.
    dmlib::handleSettingChange(reinterpret_cast<LPARAM>(L"ImmersiveColorSet"));
    HRESULT hr = __super::Activate(hwndParent, pRect, fModal);
    if (SUCCEEDED(hr))
    {
        ApplyTheme();
        // Broadcasts do not reach child pages. Use our own invisible top-level
        // window so no subclass remains attached to the host when the DLL unloads.
        m_hThemeObserver = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
                                          WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_hInst, nullptr);
        if (m_hThemeObserver)
            SetWindowSubclass(m_hThemeObserver, ThemeObserverProc, 1, reinterpret_cast<DWORD_PTR>(this));
    }
    return hr;
}

STDMETHODIMP CBaseDSPropPage::Deactivate()
{
    if (m_hThemeObserver)
    {
        DestroyWindow(m_hThemeObserver);
        m_hThemeObserver = nullptr;
    }
    const HRESULT hr = __super::Deactivate();
    if (SUCCEEDED(hr))
        m_hHint = nullptr; // The tooltip is owned by the destroyed page.
    return hr;
}

void CBaseDSPropPage::ApplyTheme()
{
    // Match MPCVideoDec's child-page integration; the host owns the outer frame.
    dmlib::setWindowEraseBgSubclass(m_Dlg);
    dmlib::setWindowCtlColorSubclass(m_Dlg);
    dmlib::setChildCtrlsSubclassAndTheme(m_Dlg);
    dmlib::setWindowNotifyCustomDrawSubclass(m_Dlg);
    if (m_hHint)
        dmlib::setDarkTooltips(m_hHint, 0);
    RedrawWindow(m_Dlg, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

LRESULT CALLBACK CBaseDSPropPage::ThemeObserverProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                  UINT_PTR id, DWORD_PTR data)
{
    auto *page = reinterpret_cast<CBaseDSPropPage *>(data);
    if (msg == WM_SETTINGCHANGE || msg == WM_SYSCOLORCHANGE)
    {
        // High contrast changes may use a null section or Accessibility instead
        // of ImmersiveColorSet. Always refresh the library's system preference.
        if (dmlib::handleSettingChange(reinterpret_cast<LPARAM>(L"ImmersiveColorSet")))
            page->ApplyTheme();
    }
    else if (msg == WM_NCDESTROY)
        RemoveWindowSubclass(hwnd, ThemeObserverProc, id);
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

HWND CBaseDSPropPage::createHintWindow(HWND parent, int timePop, int timeInit, int timeReshow)
{
    HWND hhint =
        CreateWindowEx(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT,
                       CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr, nullptr, nullptr);
    SetWindowPos(hhint, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessage(hhint, TTM_SETDELAYTIME, TTDT_AUTOPOP, MAKELONG(timePop, 0));
    SendMessage(hhint, TTM_SETDELAYTIME, TTDT_INITIAL, MAKELONG(timeInit, 0));
    SendMessage(hhint, TTM_SETDELAYTIME, TTDT_RESHOW, MAKELONG(timeReshow, 0));
    SendMessage(hhint, TTM_SETMAXTIPWIDTH, 0, 470);
    dmlib::setDarkTooltips(hhint, 0);
    return hhint;
}

TOOLINFO CBaseDSPropPage::addHint(int id, const LPWSTR text)
{
    if (!m_hHint)
        m_hHint = createHintWindow(m_Dlg, 15000);
    TOOLINFO ti;
    ti.cbSize = sizeof(TOOLINFO);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.hwnd = m_Dlg;
    ti.uId = (LPARAM)GetDlgItem(m_Dlg, id);
    ti.lpszText = text;
    SendMessage(m_hHint, TTM_ADDTOOL, 0, (LPARAM)&ti);
    return ti;
}

void CBaseDSPropPage::ListView_AddCol(HWND hlv, int &ncol, int w, const wchar_t *txt, bool right)
{
    LVCOLUMN lvc;
    lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
    lvc.iSubItem = ncol;
    lvc.pszText = (LPWSTR)txt;
    lvc.cx = w;
    lvc.fmt = right ? LVCFMT_RIGHT : LVCFMT_LEFT;
    ListView_InsertColumn(hlv, ncol, &lvc);
    ncol++;
}

HRESULT CBaseDSPropPage::ShowPropPageDialog(IBaseFilter *pFilter, HWND hwndOwner)
{
    CheckPointer(pFilter, E_INVALIDARG);
    CoInitialize(nullptr);

    // Get PropertyPages interface
    ISpecifyPropertyPages *pProp = nullptr;
    HRESULT hr = pFilter->QueryInterface<ISpecifyPropertyPages>(&pProp);
    if (SUCCEEDED(hr) && pProp)
    {
        // Get the filter's name and IUnknown pointer.
        FILTER_INFO FilterInfo;
        hr = pFilter->QueryFilterInfo(&FilterInfo);
        // We don't need the graph, so don't sit on a ref to it
        if (FilterInfo.pGraph)
            FilterInfo.pGraph->Release();

        IUnknown *pFilterUnk = nullptr;
        pFilter->QueryInterface<IUnknown>(&pFilterUnk);

        // Show the page.
        CAUUID caGUID;
        pProp->GetPages(&caGUID);
        pProp->Release();
        hr = OleCreatePropertyFrame(hwndOwner,          // Parent window
                                    0, 0,               // Reserved
                                    FilterInfo.achName, // Caption for the dialog box
                                    1,                  // Number of objects (just the filter)
                                    &pFilterUnk,        // Array of object pointers.
                                    caGUID.cElems,      // Number of property pages
                                    caGUID.pElems,      // Array of property page CLSIDs
                                    0,                  // Locale identifier
                                    0, nullptr          // Reserved
        );

        // Clean up.
        pFilterUnk->Release();
        CoTaskMemFree(caGUID.pElems);

        hr = S_OK;
    }
    CoUninitialize();
    return hr;
}
