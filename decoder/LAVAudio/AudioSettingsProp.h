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

#include "LAVAudioSettings.h"
#include "BaseDSPropPage.h"
#include "Media.h"

// {DF67AEED-E48D-49F4-BDA5-C51D2280DD2D}
DEFINE_GUID(CLSID_LAVAudioSettingsProp, 0xdf67aeed, 0xe48d, 0x49f4, 0xbd, 0xa5, 0xc5, 0x1d, 0x22, 0x80, 0xdd, 0x2d);

// {0243FA03-8873-4F9B-81D6-4139E0D85660}
DEFINE_GUID(CLSID_LAVAudioMixingProp, 0x243fa03, 0x8873, 0x4f9b, 0x81, 0xd6, 0x41, 0x39, 0xe0, 0xd8, 0x56, 0x60);

// {C5489907-9B61-4589-8000-2D8E07F8D76C}
DEFINE_GUID(CLSID_LAVAudioStatusProp, 0xc5489907, 0x9b61, 0x4589, 0x80, 0x0, 0x2d, 0x8e, 0x7, 0xf8, 0xd7, 0x6c);

// {DEFF5FD5-0B93-4F75-9F46-D5AF2AA2F755}
DEFINE_GUID(CLSID_LAVAudioFormatsProp, 0xdeff5fd5, 0xb93, 0x4f75, 0x9f, 0x46, 0xd5, 0xaf, 0x2a, 0xa2, 0xf7, 0x55);

class CLAVAudioSettingsProp : public CBaseDSPropPage
{
  public:
    CLAVAudioSettingsProp(LPUNKNOWN pUnk, HRESULT *phr);
    ~CLAVAudioSettingsProp();

    HRESULT OnActivate();
    HRESULT OnConnect(IUnknown *pUnk);
    HRESULT OnDisconnect();
    HRESULT OnApplyChanges();
    INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  private:
    HRESULT LoadData();

    void SetDirty()
    {
        m_bDirty = TRUE;
        if (m_pPageSite)
        {
            m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
        }
    }

  private:
    ILAVAudioSettings *m_pAudioSettings = nullptr;
    ILAVAudioDualMono *m_pDualMono = nullptr;

    BOOL m_bDRCEnabled;
    int m_iDRCLevel;
    DWORD m_dwDualMonoMode = DualMono_Main;

    bool m_bBitstreaming[Bitstream_NB];
    BOOL m_bDTSHDFraming;
    BOOL m_bBitstreamingFallback;
    BOOL m_bAutoAVSync;
    BOOL m_bOutputStdLayout;
    BOOL m_bOutput51Legacy;
    BOOL m_bExpandMono;
    BOOL m_bExpand61;
    bool m_bSampleFormats[SampleFormat_NB];
    BOOL m_bDither;
    BOOL m_bAudioDelay;
    int m_iAudioDelay;
    BOOL m_TrayIcon;
};

class CLAVAudioMixingProp : public CBaseDSPropPage
{
  public:
    CLAVAudioMixingProp(LPUNKNOWN pUnk, HRESULT *phr);
    ~CLAVAudioMixingProp();

    HRESULT OnActivate();
    HRESULT OnConnect(IUnknown *pUnk);
    HRESULT OnDisconnect();
    HRESULT OnApplyChanges();
    INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  private:
    HRESULT LoadData();

    void SetDirty()
    {
        m_bDirty = TRUE;
        if (m_pPageSite)
        {
            m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
        }
    }

  private:
    ILAVAudioSettings *m_pAudioSettings = nullptr;

    BOOL m_bMixing;
    DWORD m_dwSpeakerLayout;
    DWORD m_dwFlags;
    DWORD m_dwMixingMode;
    DWORD m_dwMixCenter;
    DWORD m_dwMixSurround;
    DWORD m_dwMixLFE;
};

class CLAVAudioFormatsProp : public CBaseDSPropPage
{
  public:
    CLAVAudioFormatsProp(LPUNKNOWN pUnk, HRESULT *phr);
    ~CLAVAudioFormatsProp();

    HRESULT OnActivate();
    HRESULT OnConnect(IUnknown *pUnk);
    HRESULT OnDisconnect();
    HRESULT OnApplyChanges();
    INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  private:
    HRESULT LoadData();

    void SetDirty()
    {
        m_bDirty = TRUE;
        if (m_pPageSite)
        {
            m_pPageSite->OnStatusChange(PROPPAGESTATUS_DIRTY);
        }
    }

  private:
    ILAVAudioSettings *m_pAudioSettings = nullptr;

    bool m_bFormats[Codec_AudioNB];
};

class CLAVAudioStatusProp : public CBaseDSPropPage
{
  public:
    CLAVAudioStatusProp(LPUNKNOWN pUnk, HRESULT *phr);
    ~CLAVAudioStatusProp();

    HRESULT OnActivate();
    HRESULT OnDeactivate();
    HRESULT OnConnect(IUnknown *pUnk);
    HRESULT OnDisconnect();
    INT_PTR OnReceiveMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

  private:
    void UpdateVolumeDisplay();

  private:
    ILAVAudioStatus *m_pAudioStatus = nullptr;
    int m_nChannels = 0;
};
