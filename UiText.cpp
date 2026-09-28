/*
FILE : UiText.cpp
PROJECT : Omni360
DESCRIPTION : XUI-backed implementation of the UI's text renderer. See
              UiText.h for why this replaced ATG::Font.
*/

#include "stdafx.h"
#include <xtl.h>
#include <xui.h>
#include <xuirender.h>
#include <xuielement.h>
#include <xuierror.h>
#include <d3dx9math.h>
#include <stdio.h> // swprintf_s, for the section:// locator

#include "UiText.h"
#include "AtgConsole.h"
#include "OutputConsole.h"

// Deliberately NOT "using namespace ATG". The XDK declares D3DDevice at
// global scope and ATG forward-declares it inside its namespace; pulling ATG
// in wholesale makes every unqualified use ambiguous (C2872).

// The TrueType file, embedded in the XEX itself as the "selawk" section (see
// AdditionalSections in XboxTLS2.vcxproj), so Omni360 stays a single file to
// deploy. It is embedded byte for byte, not converted, so the font is still
// the font - the SIL OFL allows exactly this kind of bundling, provided the
// licence goes with it, which it does as the "ofl" section.
//
// XUI reads XEX sections through its section:// transport, addressed as
// section://<module handle in hex>,<section name> - the form the XDK's own
// AtgXime uses. The locator is built at runtime because the handle is only
// known then.
//
// A selawk.ttf beside the XEX is the fallback if the section is missing, e.g.
// from a build made without it. Locators are URIs, not Win32 paths: XUI
// requires a scheme and forward slashes, and rejects anything else with
// XUI_ERR_RESOURCE_LOCATOR_MUST_BE_ABSOLUTE.
#define UITEXT_TYPEFACE_SECTION   "selawk"
#define UITEXT_TYPEFACE_FILE_URI  L"file://game:/selawk.ttf"
#define UITEXT_TYPEFACE_NAME  L"Selawik"

// The text height the layout is written against. Every SetScaleFactors value
// in the UI is a multiple of this, and GetFontHeight returns it so the
// resolution-scaling maths in ComputeUiMetrics stays a no-op.
#define UITEXT_DESIGN_PX      22.0f

// Applied on top of the calibrated point size. XUI's reported line height
// includes leading, so sizing purely by it renders glyphs slightly smaller
// than a strike height of the same number would. This is the one constant to
// turn if the type comes out uniformly too small or too large.
#define UITEXT_SIZE_TRIM      1.18f

// Distinct sizes in play are few - six scale factors, and at most two
// resolutions in a session - so a small fixed cache covers it without ever
// evicting.
#define UITEXT_MAX_FONTS      16

struct CachedFont
{
    float    pixels;
    HXUIFONT font;
};

static CachedFont g_fonts[UITEXT_MAX_FONTS];
static int        g_fontCount = 0;

static HXUIDC g_hDC = NULL;

static float g_backBufferW = 0.0f;
static float g_backBufferH = 0.0f;

// Pixels of line height per point, measured on this device at init rather
// than assumed. Points are not pixels and the ratio is a property of the
// renderer, so it is cheaper to ask once than to guess and rebuild.
static float g_pixelsPerPoint = 1.0f;

// Which init steps completed, so teardown undoes only those. A blind reverse
// teardown after a half-finished init is its own way to crash.
static bool g_renderInited   = false;
static bool g_dcCreated      = false;
static bool g_xuiInited      = false;
static bool g_typefaceLoaded = false;

UiFont::UiFont()
    : m_scale(1.0f)
    , m_ready(false)
{
}

bool UiFont::IsReady() const
{
    return m_ready;
}

float UiFont::GetFontHeight() const
{
    return UITEXT_DESIGN_PX;
}

float UiFont::SizeForScale(float scale) const
{
    float px = UITEXT_DESIGN_PX * scale;
    return (px < 1.0f) ? 1.0f : px;
}

void UiFont::SetScaleFactors(float scaleX, float /*scaleY*/)
{
    m_scale = scaleX;
}

void *UiFont::FontForPixelSize(float pixels)
{
    // Rounded before comparing so scale factors that differ only by floating
    // point noise share one font rather than each creating their own.
    float key = (float)((int)(pixels + 0.5f));

    for (int i = 0; i < g_fontCount; ++i)
    {
        if (g_fonts[i].pixels == key)
            return g_fonts[i].font;
    }

    if (g_fontCount >= UITEXT_MAX_FONTS)
    {
        // Falling back to an existing font keeps text on screen at slightly
        // the wrong size, which is far better than drawing nothing.
        dprintf("[UiText] font cache full at %d entries, reusing %.0fpx for %.0fpx\n",
                UITEXT_MAX_FONTS, g_fonts[0].pixels, key);
        return g_fontCount > 0 ? g_fonts[0].font : NULL;
    }

    float points = (key / g_pixelsPerPoint) * UITEXT_SIZE_TRIM;

    HXUIFONT font = NULL;
    HRESULT hr = XuiCreateFont(UITEXT_TYPEFACE_NAME, points, XUI_FONT_STYLE_NORMAL, 0, &font);
    if (FAILED(hr))
    {
        dprintf("[UiText] XuiCreateFont(%.1fpt for %.0fpx) -> 0x%08lX\n",
                points, key, (unsigned long)hr);
        return g_fontCount > 0 ? g_fonts[0].font : NULL;
    }

    g_fonts[g_fontCount].pixels = key;
    g_fonts[g_fontCount].font = font;
    g_fontCount++;

    return font;
}

HRESULT UiFont::Create(const char * /*ignoredLegacyPath*/)
{
    if (m_ready)
        return S_OK;

    ::D3DDevice *pDevice = ATG::Console::GetDevice();
    const D3DPRESENT_PARAMETERS *pParams = ATG::Console::GetPresentParams();

    if (pDevice == NULL || pParams == NULL)
    {
        dprintf("[UiText] no device or present params from Console\n");
        return E_FAIL;
    }

    HRESULT hr = XuiRenderInitShared(pDevice, pParams, XuiD3DXTextureLoader);
    if (FAILED(hr))
    {
        dprintf("[UiText] XuiRenderInitShared -> 0x%08lX\n", (unsigned long)hr);
        return hr;
    }
    g_renderInited = true;

    g_backBufferW = (float)pParams->BackBufferWidth;
    g_backBufferH = (float)pParams->BackBufferHeight;

    hr = XuiRenderCreateDC(&g_hDC);
    if (FAILED(hr))
    {
        dprintf("[UiText] XuiRenderCreateDC -> 0x%08lX\n", (unsigned long)hr);
        Destroy();
        return hr;
    }
    g_dcCreated = true;

    XUIInitParams initParams;
    ZeroMemory(&initParams, sizeof(initParams));
    initParams.cbSize = sizeof(initParams);
    initParams.dwFlags = 0;
    initParams.pHooks = NULL; // no scene graph here, so no runtime hooks

    hr = XuiInit(&initParams);
    if (FAILED(hr))
    {
        dprintf("[UiText] XuiInit -> 0x%08lX\n", (unsigned long)hr);
        Destroy();
        return hr;
    }
    g_xuiInited = true;

    // Prefer the copy inside the XEX. Checked with XGetModuleSection first
    // rather than inferred from XuiRegisterTypeface failing, because whether
    // registration opens the resource straight away or only when a font is
    // first created isn't documented - a failure there could surface later
    // and far from its cause.
    HANDLE hModule = GetModuleHandle(NULL);
    PVOID sectionData = NULL;
    ULONG sectionSize = 0;

    WCHAR sectionUri[64];
    LPCWSTR locator = UITEXT_TYPEFACE_FILE_URI;

    if (XGetModuleSection(hModule, UITEXT_TYPEFACE_SECTION, &sectionData, &sectionSize) && sectionSize > 0)
    {
        swprintf_s(sectionUri, ARRAYSIZE(sectionUri), L"section://%x,%S",
                   (unsigned int)(UINT_PTR)hModule, UITEXT_TYPEFACE_SECTION);
        locator = sectionUri;
        dprintf("[UiText] font: XEX section \"%s\" (%lu bytes)\n", UITEXT_TYPEFACE_SECTION, sectionSize);
    }
    else
    {
        dprintf("[UiText] font: no \"%s\" section in the XEX, trying selawk.ttf beside it\n",
                UITEXT_TYPEFACE_SECTION);
    }

    TypefaceDescriptor desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.szTypeface = UITEXT_TYPEFACE_NAME;
    desc.szLocator = locator;
    desc.szReserved1 = NULL;
    desc.fBaselineAdjust = 0.0f;
    desc.szFallbackTypeface = NULL;

    hr = XuiRegisterTypeface(&desc, TRUE);
    if (FAILED(hr))
    {
        const char *why = "unrecognised - look it up in xuierror.h";

        if (hr == XUI_ERR_RESOURCE_LOCATOR_MUST_BE_ABSOLUTE)
            why = "locator is not an absolute URI - needs a scheme, e.g. file://game:/name.ttf";
        else if (hr == XUI_ERR_RESOURCE_COULD_NOT_BE_OPENED)
            why = "locator parsed, but the resource could not be opened";
        else if (hr == XUI_ERR_INVALID_RESOURCE_PATH)
            why = "resource path rejected - check the drive alias exists";
        else if (hr == XUI_ERR_FILE_INVALID)
            why = "file opened but was not a usable font";

        dprintf("[UiText] XuiRegisterTypeface(%S) -> 0x%08lX: %s\n", locator, (unsigned long)hr, why);
        Destroy();
        return hr;
    }
    g_typefaceLoaded = true;

    // Calibrate points to pixels on this device instead of assuming a DPI.
    // One reference font, one metrics query; every size after this is derived
    // from the ratio.
    const float REFERENCE_POINTS = 20.0f;

    HXUIFONT reference = NULL;
    hr = XuiCreateFont(UITEXT_TYPEFACE_NAME, REFERENCE_POINTS, XUI_FONT_STYLE_NORMAL, 0, &reference);
    if (FAILED(hr))
    {
        dprintf("[UiText] reference XuiCreateFont -> 0x%08lX\n", (unsigned long)hr);
        Destroy();
        return hr;
    }

    XUIFontMetrics metrics;
    if (SUCCEEDED(XuiGetFontMetrics(reference, &metrics)) && metrics.fLineHeight > 0.0f)
    {
        g_pixelsPerPoint = metrics.fLineHeight / REFERENCE_POINTS;
    }
    else
    {
        // 1:1 is wrong but survivable - text renders, just at the wrong size,
        // which is visible and fixable rather than a blank screen.
        g_pixelsPerPoint = 1.0f;
        dprintf("[UiText] font metrics unavailable, assuming 1px per point\n");
    }

    XuiReleaseFont(reference);

    dprintf("[UiText] ready: backbuffer %.0fx%.0f, %.3f px per point\n",
            g_backBufferW, g_backBufferH, g_pixelsPerPoint);

    m_ready = true;
    return S_OK;
}

void UiFont::Begin()
{
    if (!m_ready)
        return;

    // Transparent clear: confirmed on hardware to leave the frame's existing
    // quads and any other drawing intact.
    XuiRenderBegin(g_hDC, D3DCOLOR_ARGB(0, 0, 0, 0));

    D3DXMATRIX matView;
    D3DXMatrixIdentity(&matView);
    XuiRenderSetViewTransform(g_hDC, &matView);
}

void UiFont::End()
{
    if (!m_ready)
        return;

    XuiRenderEnd(g_hDC);

    // Deliberately not XuiRenderPresent - this app presents its own frame.
}

float UiFont::GetTextWidth(const WCHAR *strText)
{
    if (!m_ready || strText == NULL)
        return 0.0f;

    HXUIFONT font = (HXUIFONT)FontForPixelSize(SizeForScale(m_scale));
    if (font == NULL)
        return 0.0f;

    XUIRect rect;
    rect.left = 0.0f;
    rect.top = 0.0f;
    rect.right = g_backBufferW;
    rect.bottom = g_backBufferH;

    XuiMeasureText(font, strText, -1, XUI_FONT_STYLE_NORMAL, 0, &rect);

    return rect.right - rect.left;
}

void UiFont::DrawText(float sx, float sy, DWORD dwColor, const WCHAR *strText,
                      DWORD dwFlags, float fMaxPixelWidth)
{
    if (!m_ready || strText == NULL || strText[0] == L'\0')
        return;

    HXUIFONT font = (HXUIFONT)FontForPixelSize(SizeForScale(m_scale));
    if (font == NULL)
        return;

    // Single line, no wrapping: every caller here draws one line into a fixed
    // row, and letting XUI wrap would silently push text outside its row.
    DWORD style = XUI_FONT_STYLE_SINGLE_LINE | XUI_FONT_STYLE_NO_WORDWRAP;

    if (dwFlags & UITEXT_TRUNCATED)
        style |= XUI_FONT_STYLE_ELLIPSIS;

    float width = (fMaxPixelWidth > 0.0f) ? fMaxPixelWidth : (g_backBufferW - sx);
    if (width < 1.0f)
        width = 1.0f;

    // Alignment is resolved by moving the draw origin rather than by handing
    // XUI an alignment flag. ATG::Font treats sx as the RIGHT edge for
    // right-aligned text, whereas XUI would align within its rect - so
    // measuring and shifting is what actually reproduces the old behaviour.
    float originX = sx;

    if (dwFlags & (UITEXT_RIGHT | UITEXT_CENTER_X))
    {
        float textW = GetTextWidth(strText);
        if (fMaxPixelWidth > 0.0f && textW > fMaxPixelWidth)
            textW = fMaxPixelWidth;

        if (dwFlags & UITEXT_RIGHT)
            originX = sx - textW;
        else
            originX = sx - textW * 0.5f;

        width = (textW > 1.0f) ? textW : 1.0f;
    }

    XUIRect rect;
    rect.left = 0.0f;
    rect.top = 0.0f;
    rect.right = width;
    rect.bottom = g_backBufferH - sy;
    if (rect.bottom < 1.0f)
        rect.bottom = 1.0f;

    D3DXMATRIX matPos;
    D3DXMatrixIdentity(&matPos);
    matPos._41 = originX;
    matPos._42 = sy;
    XuiRenderSetTransform(g_hDC, &matPos);

    XuiSelectFont(g_hDC, font);
    XuiSetColorFactor(g_hDC, dwColor);

    XuiDrawText(g_hDC, strText, style, 0, &rect);
}

void UiFont::Destroy()
{
    for (int i = 0; i < g_fontCount; ++i)
    {
        if (g_fonts[i].font != NULL)
            XuiReleaseFont(g_fonts[i].font);

        g_fonts[i].font = NULL;
        g_fonts[i].pixels = 0.0f;
    }
    g_fontCount = 0;

    if (g_typefaceLoaded)
    {
        XuiUnregisterTypeface(UITEXT_TYPEFACE_NAME);
        g_typefaceLoaded = false;
    }

    if (g_xuiInited)
    {
        XuiUninit();
        g_xuiInited = false;
    }

    if (g_dcCreated)
    {
        XuiRenderDestroyDC(g_hDC);
        g_hDC = NULL;
        g_dcCreated = false;
    }

    if (g_renderInited)
    {
        XuiRenderUninit();
        g_renderInited = false;
    }

    m_ready = false;
}
