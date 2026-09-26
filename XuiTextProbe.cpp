/*
FILE : XuiTextProbe.cpp
PROJECT : Omni360
DESCRIPTION : Spike implementation - see XuiTextProbe.h for what this is
              answering and why.

Every XUI call logs its HRESULT. That is the point of the exercise: this runs
on hardware with no debugger attached and a build cycle measured in minutes,
so a run that fails should say exactly which step failed rather than leaving
the next guess to cost another cycle.
*/

#include "stdafx.h"
#include <xtl.h>
#include <xui.h>
#include <xuirender.h>
#include <xuielement.h>
#include <d3dx9math.h>

#include "XuiTextProbe.h"
#include "AtgConsole.h"
#include "OutputConsole.h"

// Deliberately NOT "using namespace ATG". The XDK declares D3DDevice at
// global scope (d3d9.h) and ATG forward-declares it inside its own namespace;
// pulling ATG in wholesale makes both visible and every unqualified use
// ambiguous:
//
//     error C2872: 'D3DDevice' : ambiguous symbol
//
// Qualifying each name explicitly sidesteps it and says which one is meant.

// The TrueType file, deployed beside the XEX rather than embedded as a XEX
// section the way the bitmap atlas is.
//
// A section would mean bundler and a fixed blob again; the whole point of this
// route is that the font stays a font. game: is where the app already keeps
// its own runtime files (settings.txt, ArchiveOrgKeys.txt), so this is
// consistent with the rest rather than a new convention.
#define XUI_TYPEFACE_PATH  L"game:\\selawk.ttf"
#define XUI_TYPEFACE_NAME  L"Selawik"
#define XUI_PROBE_FONT_PT  22.0f

static bool      g_XuiReady = false;
static HXUIDC    g_hDC      = NULL;
static HXUIFONT  g_hFont    = NULL;

// Which steps actually completed, so teardown only undoes what was done. A
// blind reverse teardown after a half-finished init is its own way to crash.
static bool g_RenderInited   = false;
static bool g_DCCreated      = false;
static bool g_XuiInited      = false;
static bool g_TypefaceLoaded = false;

bool IsXuiTextReady()
{
    return g_XuiReady;
}

bool InitXuiText()
{
    if (g_XuiReady)
        return true;

    ::D3DDevice *pDevice = ATG::Console::GetDevice();
    if (pDevice == NULL)
    {
        dprintf("[XUI] no D3D device from Console - init skipped\n");
        return false;
    }

    const D3DPRESENT_PARAMETERS *pParams = ATG::Console::GetPresentParams();
    if (pParams == NULL)
    {
        dprintf("[XUI] no present params from Console - init skipped\n");
        return false;
    }

    // Shared, not exclusive: XUI is told to render onto the device this app
    // already owns rather than creating its own. If XUI can coexist with the
    // quad path at all, this is the call that makes it possible.
    HRESULT hr = XuiRenderInitShared(pDevice, pParams, XuiD3DXTextureLoader);
    dprintf("[XUI] XuiRenderInitShared -> 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr))
        return false;
    g_RenderInited = true;

    hr = XuiRenderCreateDC(&g_hDC);
    dprintf("[XUI] XuiRenderCreateDC -> 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr))
    {
        ShutdownXuiText();
        return false;
    }
    g_DCCreated = true;

    XUIInitParams initParams;
    ZeroMemory(&initParams, sizeof(initParams));
    initParams.cbSize = sizeof(initParams);
    initParams.dwFlags = 0;
    initParams.pHooks = NULL; // no scene graph here, so no runtime hooks needed

    hr = XuiInit(&initParams);
    dprintf("[XUI] XuiInit -> 0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr))
    {
        ShutdownXuiText();
        return false;
    }
    g_XuiInited = true;

    // szLocator is a path to the font FILE - this is the part that makes the
    // whole approach worth testing. No pre-baked atlas, no fixed strike size,
    // no character-range ceiling.
    TypefaceDescriptor desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.szTypeface = XUI_TYPEFACE_NAME;
    desc.szLocator = XUI_TYPEFACE_PATH;
    desc.szReserved1 = NULL;
    desc.fBaselineAdjust = 0.0f;
    desc.szFallbackTypeface = NULL;

    hr = XuiRegisterTypeface(&desc, TRUE);
    dprintf("[XUI] XuiRegisterTypeface(%ls) -> 0x%08lX\n", XUI_TYPEFACE_PATH, (unsigned long)hr);
    if (FAILED(hr))
    {
        // Overwhelmingly the likely cause is the file simply not being on the
        // console yet - it has to sit beside the XEX. Worth saying plainly,
        // because it looks identical to a real XUI failure from the log.
        dprintf("[XUI] typeface failed - is selawk.ttf deployed next to the XEX?\n");
        ShutdownXuiText();
        return false;
    }
    g_TypefaceLoaded = true;

    hr = XuiCreateFont(XUI_TYPEFACE_NAME, XUI_PROBE_FONT_PT, XUI_FONT_STYLE_NORMAL, 0, &g_hFont);
    dprintf("[XUI] XuiCreateFont(%.1f) -> 0x%08lX\n", XUI_PROBE_FONT_PT, (unsigned long)hr);
    if (FAILED(hr))
    {
        ShutdownXuiText();
        return false;
    }

    g_XuiReady = true;
    dprintf("[XUI] ready\n");
    return true;
}

void DrawXuiTextProbe(float x, float y, const wchar_t *text, unsigned long color)
{
    if (!g_XuiReady || text == NULL)
        return;

    // A fully transparent clear colour, which is the only lever this API
    // offers against XuiRenderBegin's mandatory clear. If XUI honours the
    // alpha, the frame survives and the probe draws over it. If it clears
    // regardless, the screen comes back black with only this string on it -
    // and that is the answer, just the unwelcome one.
    HRESULT hr = XuiRenderBegin(g_hDC, D3DCOLOR_ARGB(0, 0, 0, 0));
    if (FAILED(hr))
    {
        static bool logged = false;
        if (!logged) { logged = true; dprintf("[XUI] XuiRenderBegin -> 0x%08lX\n", (unsigned long)hr); }
        return;
    }

    // Identity view, so the transform below is in whatever space the DC was
    // created for - the back buffer. Getting this wrong puts the text
    // off-screen rather than failing, which is itself worth seeing.
    D3DXMATRIX matView;
    D3DXMatrixIdentity(&matView);
    XuiRenderSetViewTransform(g_hDC, &matView);

    D3DXMATRIX matPos;
    D3DXMatrixTranslation(&matPos, x, y, 0.0f);
    XuiRenderSetTransform(g_hDC, &matPos);

    XuiSelectFont(g_hDC, g_hFont);
    XuiSetColorFactor(g_hDC, (DWORD)color);

    // A generous clip rect - this is a probe, and a too-small rect would
    // silently clip the string and look like a failure that it is not.
    // Float fields, and the struct has a constructor - so it is left to
    // default-construct rather than being ZeroMemory'd.
    XUIRect clipRect;
    clipRect.left = 0.0f;
    clipRect.top = 0.0f;
    clipRect.right = 1280.0f;
    clipRect.bottom = 720.0f;

    XuiDrawText(g_hDC, text, XUI_FONT_STYLE_NORMAL, 0, &clipRect);

    XuiRenderEnd(g_hDC);

    // Deliberately NOT calling XuiRenderPresent. This app presents its own
    // frame, and letting XUI present too would be a second Present on the same
    // frame - which is its own failure mode and would muddy what this probe is
    // trying to measure.
}

void ShutdownXuiText()
{
    if (g_hFont != NULL)
    {
        XuiReleaseFont(g_hFont);
        g_hFont = NULL;
    }

    if (g_TypefaceLoaded)
    {
        XuiUnregisterTypeface(XUI_TYPEFACE_NAME);
        g_TypefaceLoaded = false;
    }

    if (g_XuiInited)
    {
        XuiUninit();
        g_XuiInited = false;
    }

    if (g_DCCreated)
    {
        XuiRenderDestroyDC(g_hDC);
        g_hDC = NULL;
        g_DCCreated = false;
    }

    if (g_RenderInited)
    {
        XuiRenderUninit();
        g_RenderInited = false;
    }

    g_XuiReady = false;
}
