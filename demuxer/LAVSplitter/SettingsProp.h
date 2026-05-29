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

#pragma once

#include "BaseDSPropPage.h"
#include "LAVSplitterSettingsInternal.h"

// {5F7C07BB-C7C1-46F1-8D47-36AC2214E349}
DEFINE_GUID(CLSID_LAVSplitterSettingsProp, 0x5f7c07bb, 0xc7c1, 0x46f1, 0x8d, 0x47, 0x36, 0xac, 0x22, 0x14, 0xe3, 0x49);

// {11671F17-64B5-492D-B00C-7B1E9ACF2131}
DEFINE_GUID(CLSID_LAVSplitterFormatsProp, 0x11671f17, 0x64b5, 0x492d, 0xb0, 0x0c, 0x7b, 0x1e, 0x9a, 0xcf, 0x21, 0x31);

#define LANG_BUFFER_SIZE 16384

class CLAVSplitterSettingsProp : public CBaseDSPropPage
{
  public:
    CLAVSplitterSettingsProp(LPUNKNOWN pUnk, HRESULT *phr);
    virtual ~CLAVSplitterSettingsProp(void);

    HRESULT OnActivate();
    HRESULT OnConnect(IUnknown *pUnk);
    HRESULT OnDisconnect();
    HRESULT OnApplyChanges();
    INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  private:
    HRESULT LoadData();
    void UpdateSubtitleMode(LAVSubtitleMode mode);

    void SetDirty()
    {
        m_bDirty = TRUE;
        if (m_pPageSite)
        {
            m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
        }
    }

  private:
    ILAVFSettingsInternal *m_pLAVF = nullptr;

    // Settings
    WCHAR *m_pszPrefLang = nullptr;
    WCHAR *m_pszPrefSubLang = nullptr;
    WCHAR *m_pszAdvSubConfig = nullptr;

    LAVSubtitleMode m_subtitleMode;
    BOOL m_PGSForcedStream;
    BOOL m_PGSOnlyForced;
    int m_VC1Mode;
    BOOL m_substreams;
    BOOL m_MKVExternal;

    BOOL m_StreamSwitchReselectSubs;

    BOOL m_StreamSwitchRemoveAudio;
    BOOL m_PreferHighQualityAudio;
    BOOL m_ImpairedAudio;
    DWORD m_QueueMaxMem;
    DWORD m_QueueMaxPackets;
    DWORD m_NetworkAnalysisDuration;

    BOOL m_TrayIcon;

    LAVSubtitleMode m_selectedSubMode;
    WCHAR m_subLangBuffer[LANG_BUFFER_SIZE];
    WCHAR m_advSubBuffer[LANG_BUFFER_SIZE];

    WCHAR stringBuffer[256];
};

class CLAVSplitterFormatsProp : public CBaseDSPropPage
{
  public:
    CLAVSplitterFormatsProp(LPUNKNOWN pUnk, HRESULT *phr);
    virtual ~CLAVSplitterFormatsProp(void);

    HRESULT OnActivate();
    HRESULT OnConnect(IUnknown *pUnk);
    HRESULT OnDisconnect();
    HRESULT OnApplyChanges();
    INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  private:
    void SetDirty()
    {
        m_bDirty = TRUE;
        if (m_pPageSite)
        {
            m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
        }
    }

  private:
    ILAVFSettingsInternal *m_pLAVF = nullptr;

    std::set<FormatInfo> m_Formats;
    BOOL *m_bFormats = nullptr;

    WCHAR stringBuffer[256];
};
