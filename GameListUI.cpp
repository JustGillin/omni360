/*
FILE : GameListUI.cpp
PROJECT : Omni360
DESCRIPTION : icon-list game picker + progress bar, replacing X-Store's
              original typed-search text UI for this fork's flow (browse your
              own installed games instead of typing a query).

X-Store's existing ATG::Console class is output-only (see AtgConsole.h's own
comment: "Use this INSTEAD of AtgApplication" for apps with no input) and
keeps its D3D device/font as private members, so it can't be extended for
interactive icon rendering - this is a small, separate render loop that gets
the device via a small accessor added to Console (ATG::Console::GetDevice(),
see AtgConsole.h) rather than a restructure onto the fuller ATG::Application
framework. That would be the cleaner long-term foundation if this UI grows
further.

Shader/vertex-declaration/DrawPrimitiveUP pattern mirrors
Common/AtgFont.cpp's Font::CreateFontShaders() and Common/AtgUtil.cpp's
RenderBackground() - Xbox 360's D3D9 has no fixed-function pipeline, so even
a plain textured quad needs its own compiled vertex/pixel shader pair.

Confirmed rendering correctly on real hardware (icon grid, selection
highlight, D-pad navigation, per-row text) - the staged diagnostic render
scaffolding that proved it out has since been removed. The render loop in
ShowGameListUI is a genuine per-frame Resume/Clear/draw/Present/Suspend loop
hand-paced to ~60fps, so per-frame animation is viable here.
*/

#include "stdafx.h"
#include <xgraphics.h>
#include "GameListUI.h"
#include "AtgConsole.h"
#include "AtgFont.h"
#include "OutputConsole.h"

#include <stdio.h>
#include <string.h>

using namespace ATG;

// Set from ATG::Console::GetDevice() in InitGameListUI(). Originally this
// referenced the shared ATG::g_pd3dDevice global directly (same one
// AtgConsole.cpp sets up), but that hit a linker issue pulling in its
// defining object file (AtgApp.obj) from another compiland - going through
// Console's own accessor instead avoids depending on that cross-module
// global resolving at all, since Console is already known-reliably-linked
// (any text output at all already depends on it).
//
// Explicitly ATG:: qualified: Common/AtgDevice.h defines ATG::D3DDevice as
// its own subclass of the global ::D3DDevice, and with `using namespace
// ATG;` above, the bare name "D3DDevice" is ambiguous between the two.
static ATG::D3DDevice *g_pd3dDevice = NULL;

// ---------------------------------------------------------------------------
// Quad shader (icons + solid-color rects, the latter via a 1x1 white texture)
// ---------------------------------------------------------------------------

static const char g_strQuadShader[] =
    "struct VS_IN\n"
    "{\n"
    "    float2 Pos : POSITION;\n"
    "    float2 Tex : TEXCOORD0;\n"
    "};\n"
    "struct VS_OUT\n"
    "{\n"
    "    float4 Position : POSITION;\n"
    "    float2 TexCoord0 : TEXCOORD0;\n"
    "};\n"
    "VS_OUT QuadVertexShader( VS_IN In )\n"
    "{\n"
    "    VS_OUT Out;\n"
    // D3DRS_VIEWPORTENABLE is FALSE for our draws (matching Font::DrawText's
    // own state block), which on the Xbox 360 GPU means the vertex shader's
    // output position is taken as RAW SCREEN PIXELS directly, not normalized
    // -1..1 clip coordinates - there is no viewport transform to undo it.
    // The previous (pos/ScreenSize)*2-1 NDC math was the PC D3D9 pattern,
    // which is wrong here: it produced values in roughly [-1,1], which the
    // GPU then read as literal pixel coordinates - collapsing every quad
    // into a ~2x2 pixel patch at the screen origin (the "tiny red pixel,
    // top-left, unaffected by any render state" symptom). Matching
    // Common/AtgFont.cpp's FontVertexShader exactly: pass pixel coordinates
    // straight through (the -0.5 is texel-center alignment, same as Font's).
    "    Out.Position.x = In.Pos.x - 0.5;\n"
    "    Out.Position.y = In.Pos.y - 0.5;\n"
    "    Out.Position.z = 0.0;\n"
    "    Out.Position.w = 1.0;\n"
    "    Out.TexCoord0 = In.Tex;\n"
    "    return Out;\n"
    "}\n"
    "uniform float4 Tint : register(c1);\n"
    // Two-stop gradient, evaluated per-pixel from the quad's own interpolated
    // texture coordinate. Doing it here rather than with per-vertex colors is
    // deliberate: it needs no change to the vertex declaration or the
    // BeginVertices write pattern (both of which took real hardware debugging
    // to get right), and it avoids hand-filling a ramp TEXTURE, where the
    // Xbox 360 GPU's texture tiling makes a naive linear LockRect write
    // incorrect for anything bigger than the existing 1x1 white texture.
    // Pixel shader constants are the one mechanism here already proven to
    // work - Tint at c1 is the same thing.
    //
    // GradAxis selects the direction: (0,1) vertical, (1,0) horizontal. For a
    // plain solid draw, DrawQuad sets GradA = GradB = white, which makes the
    // lerp a no-op and leaves the existing Tint-only behaviour exactly as it
    // was.
    "uniform float4 GradA : register(c2);\n"
    "uniform float4 GradB : register(c3);\n"
    "uniform float4 GradAxis : register(c4);\n"
    "sampler QuadTexture : register(s0);\n"
    "float4 QuadPixelShader( VS_OUT In ) : COLOR0\n"
    "{\n"
    "    float t = dot( In.TexCoord0, GradAxis.xy );\n"
    "    float4 grad = lerp( GradA, GradB, t );\n"
    "    return tex2D( QuadTexture, In.TexCoord0 ) * Tint * grad;\n"
    "}\n";

struct QuadVertex
{
    float x, y;
    float u, v;
};

static D3DVertexDeclaration *g_pQuadVertexDecl = NULL;
static D3DVertexShader *g_pQuadVertexShader = NULL;
static D3DPixelShader *g_pQuadPixelShader = NULL;
static D3DTexture *g_pWhiteTexture = NULL; // 1x1 white, for DrawRect (solid-color quads reuse the textured-quad path)

static ATG::Font g_UiFont;
static bool g_Initialized = false;

static void ReleaseIcons(); // defined with the cover-art cache below; called from ShutdownGameListUI

static bool CreateQuadResources()
{
    static const D3DVERTEXELEMENT9 decl[] =
        {
            {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END()};

    if (FAILED(g_pd3dDevice->CreateVertexDeclaration(decl, &g_pQuadVertexDecl)))
        return false;

    ID3DXBuffer *pShaderCode = NULL;

    if (FAILED(D3DXCompileShader(g_strQuadShader, sizeof(g_strQuadShader) - 1,
                                 NULL, NULL, "QuadVertexShader", "vs.2.0", 0, &pShaderCode, NULL, NULL)))
        return false;

    HRESULT hr = g_pd3dDevice->CreateVertexShader((DWORD *)pShaderCode->GetBufferPointer(), &g_pQuadVertexShader);
    pShaderCode->Release();
    if (FAILED(hr))
        return false;

    if (FAILED(D3DXCompileShader(g_strQuadShader, sizeof(g_strQuadShader) - 1,
                                 NULL, NULL, "QuadPixelShader", "ps.2.0", 0, &pShaderCode, NULL, NULL)))
        return false;

    hr = g_pd3dDevice->CreatePixelShader((DWORD *)pShaderCode->GetBufferPointer(), &g_pQuadPixelShader);
    pShaderCode->Release();
    if (FAILED(hr))
        return false;

    // 1x1 white texture so DrawRect() can go through the exact same textured
    // draw path as icons, just with a solid-color texture and a tint.
    if (FAILED(g_pd3dDevice->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_pWhiteTexture, NULL)))
        return false;

    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_pWhiteTexture->LockRect(0, &lr, NULL, 0)))
    {
        *(DWORD *)lr.pBits = 0xFFFFFFFF;
        g_pWhiteTexture->UnlockRect(0);
    }

    return true;
}

// D3DCOLOR (ARGB) -> the float4 the pixel shader constants expect.
static void ColorToFloat4(D3DCOLOR c, float *out)
{
    out[0] = ((c >> 16) & 0xFF) / 255.0f; // R
    out[1] = ((c >> 8) & 0xFF) / 255.0f;  // G
    out[2] = (c & 0xFF) / 255.0f;         // B
    out[3] = ((c >> 24) & 0xFF) / 255.0f; // A
}

// Core quad draw. gradA/gradB/vertical describe the two-stop gradient the
// pixel shader applies on top of tint; pass the same color for both to get a
// flat fill (which is what DrawQuad/DrawRect below do).
static void DrawQuadEx(D3DTexture *texture, float x, float y, float w, float h,
                       D3DCOLOR tint, D3DCOLOR gradA, D3DCOLOR gradB, bool vertical)
{
    float tintF[4];
    float gradAF[4];
    float gradBF[4];
    ColorToFloat4(tint, tintF);
    ColorToFloat4(gradA, gradAF);
    ColorToFloat4(gradB, gradBF);

    float axis[4] = {vertical ? 0.0f : 1.0f, vertical ? 1.0f : 0.0f, 0.0f, 0.0f};

    // Tint is declared "register(c1)" inside QuadPixelShader, not the vertex
    // shader - vertex and pixel shader constants are separate register
    // banks in D3D9. Setting it as a vertex constant left the pixel shader's
    // real c1 at its default (0,0,0,0), multiplying every quad's sampled
    // color to fully transparent black - every DrawQuad/DrawRect call
    // (icons, placeholders, the selection highlight) was invisible. The
    // gradient constants below live in the same pixel-shader bank for the
    // same reason.
    g_pd3dDevice->SetPixelShaderConstantF(1, tintF, 1);
    g_pd3dDevice->SetPixelShaderConstantF(2, gradAF, 1);
    g_pd3dDevice->SetPixelShaderConstantF(3, gradBF, 1);
    g_pd3dDevice->SetPixelShaderConstantF(4, axis, 1);

    g_pd3dDevice->SetTexture(0, texture != NULL ? texture : g_pWhiteTexture);
    g_pd3dDevice->SetVertexDeclaration(g_pQuadVertexDecl);
    g_pd3dDevice->SetVertexShader(g_pQuadVertexShader);
    g_pd3dDevice->SetPixelShader(g_pQuadPixelShader);

    // Explicit, complete state setup - matching Font::DrawText's own list in
    // Common/AtgFont.cpp (the only other proven-working dynamic 2D draw in
    // this project) rather than the handful of blend states this function
    // set before. Leaving the rest at whatever was last inherited (e.g.
    // Z-test enabled with no properly-configured depth buffer for this draw,
    // which Console's own RenderBackground() explicitly turns on) is a very
    // plausible way to fault the GPU exactly like we were seeing.
    g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pd3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pd3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pd3dDevice->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_pd3dDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    g_pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); // don't cull our quad regardless of winding
    g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE); // matches our manual pixel->clip-space math in the vertex shader
    // TEMPORARY diagnostic fix: a full-screen test quad rendered completely
    // invisible (not mispositioned - literally nothing), which is the classic
    // symptom of D3DRS_COLORWRITEENABLE being masked to 0 by something else
    // and never explicitly restored here. Also disabling scissor test in
    // case a leftover zero-area scissor rect is in play.
    g_pd3dDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0000000F); // all 4 channels (R,G,B,A)
    g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_pd3dDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_pd3dDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_pd3dDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE); // all our textures (icons + the 1x1 white) are single-mip now
    g_pd3dDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_pd3dDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    // Xbox 360 needs vertex data submitted through BeginVertices/EndVertices
    // (writing directly into write-combined GPU ring-buffer memory), not
    // DrawPrimitiveUP from a plain CPU array the way PC D3D9 code typically
    // does - confirmed against Common/AtgFont.cpp's Font::DrawText, the one
    // piece of proven-working dynamic-geometry code in this project. Per its
    // own comments: writes must go in ascending address order, and never
    // with anything smaller than a 32-bit store - both satisfied here since
    // we're writing consecutive floats.
    //
    // Also matching Font::DrawText's exact primitive choice: D3DPT_QUADLIST
    // (a native Xbox 360 GPU primitive with no PC D3D9 equivalent) with
    // vertices in TL, TR, BR, BL order - not D3DPT_TRIANGLESTRIP, which is
    // what an earlier version of this function used and still crashed the
    // GPU even after switching to BeginVertices/EndVertices.
    volatile FLOAT *pVertex;
    HRESULT hr = g_pd3dDevice->BeginVertices(D3DPT_QUADLIST, 4, sizeof(QuadVertex), (VOID **)&pVertex);
    if (SUCCEEDED(hr))
    {
        pVertex[0] = x;         pVertex[1] = y;         pVertex[2] = 0.0f; pVertex[3] = 0.0f;  // TL
        pVertex[4] = x + w;     pVertex[5] = y;         pVertex[6] = 1.0f; pVertex[7] = 0.0f;  // TR
        pVertex[8] = x + w;     pVertex[9] = y + h;     pVertex[10] = 1.0f; pVertex[11] = 1.0f; // BR
        pVertex[12] = x;        pVertex[13] = y + h;    pVertex[14] = 0.0f; pVertex[15] = 1.0f; // BL

        g_pd3dDevice->EndVertices();
    }
    else
    {
        // TEMPORARY: this branch was previously silent - if BeginVertices
        // itself fails at runtime, we'd skip drawing entirely with zero
        // indication why, which would perfectly explain "nothing renders,
        // nothing crashes".
        static bool loggedFailure = false;
        if (!loggedFailure)
        {
            loggedFailure = true;
            dprintf("[DIAG] BeginVertices FAILED, hr=0x%08X\n", hr);
        }
    }

    g_pd3dDevice->SetTexture(0, NULL);
    g_pd3dDevice->SetVertexDeclaration(NULL);
    g_pd3dDevice->SetVertexShader(NULL);
    g_pd3dDevice->SetPixelShader(NULL);
}

// Flat textured quad - the gradient stops are both white, making the shader's
// lerp a no-op so this behaves exactly as it did before gradients existed.
static void DrawQuad(D3DTexture *texture, float x, float y, float w, float h, D3DCOLOR tint)
{
    DrawQuadEx(texture, x, y, w, h, tint, 0xFFFFFFFF, 0xFFFFFFFF, true);
}

static void DrawRect(float x, float y, float w, float h, D3DCOLOR color)
{
    DrawQuad(NULL, x, y, w, h, color);
}

// Two-stop gradient fill. Tint is left white so the gradient stops carry the
// color (and their alpha) on their own.
static void DrawGradientRect(float x, float y, float w, float h,
                             D3DCOLOR colorA, D3DCOLOR colorB, bool vertical)
{
    DrawQuadEx(NULL, x, y, w, h, 0xFFFFFFFF, colorA, colorB, vertical);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// StfsParser's UTF-16BE decode path does clamp to ASCII/Latin-1 (see its own
// comment on non-Latin titles), but its UTF-8 passthrough path does NOT - it
// copies the package's raw UTF-8 bytes verbatim, multi-byte sequences and
// all. A byte-widen (one WCHAR per input byte, as this used to do) is only
// correct for single-byte UTF-8, so any title with a genuine multi-byte
// character (confirmed: "Modern Warfare(R) 3", U+00AE encoded as UTF-8's
// 0xC2 0xAE) rendered as two garbled characters ("Â®") instead of one. Real
// UTF-8 decoding here (BMP range is enough - these are STFS display names,
// never supplementary-plane characters) fixes both paths at once.
static void Utf8ToWide(const char *in, WCHAR *out, int outSize)
{
    const unsigned char *p = (const unsigned char *)in;
    int o = 0;

    while (*p != '\0' && o < outSize - 1)
    {
        unsigned char b0 = p[0];
        unsigned long cp;
        int extraBytes;

        if (b0 < 0x80)          { cp = b0;          extraBytes = 0; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; extraBytes = 1; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; extraBytes = 2; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; extraBytes = 3; }
        else { p++; out[o++] = L'?'; continue; } // stray continuation/invalid lead byte

        const unsigned char *seq = p + 1;
        int i;
        for (i = 0; i < extraBytes; ++i)
        {
            if (seq[i] == '\0' || (seq[i] & 0xC0) != 0x80)
                break; // truncated/malformed sequence
            cp = (cp << 6) | (seq[i] & 0x3F);
        }

        if (i < extraBytes)
        {
            // malformed - emit '?' for the lead byte only, resync there
            out[o++] = L'?';
            p++;
            continue;
        }

        p += 1 + extraBytes;
        out[o++] = (cp <= 0xFFFF) ? (WCHAR)cp : L'?'; // outside BMP: not expected for a title name, and this font has no such glyphs anyway
    }

    out[o] = L'\0';
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

// An original dark palette: a near-black background with a slight green cast,
// one green accent, and a small set of neutral text weights. Chosen for a TV
// at couch distance - low overall brightness, but high contrast where it
// matters (selected row, primary text).
#define COL_BG_TOP         0xFF16231C // subtle wash, lightest at the top
#define COL_BG_BOTTOM      0xFF050907
#define COL_HEADER_RULE_A  0xFF5FBF46 // accent rule under the header, fading out to the right
#define COL_HEADER_RULE_B  0x005FBF46
#define COL_PANEL          0x12FFFFFF // unselected row plate
#define COL_PANEL_SEL_A    0x33FFFFFF // selected row plate, brightest at its left edge
#define COL_PANEL_SEL_B    0x0CFFFFFF
#define COL_ACCENT         0xFF7FD44F // selection marker, progress fill
#define COL_ACCENT_DIM     0xFF3E8A2E
#define COL_ICON_PLACEHLD  0xFF2A332C
#define COL_TEXT_PRIMARY   0xFFFFFFFF
#define COL_TEXT_SECONDARY 0xFFA8B4AC
// Lightened from a first pass at 0xFF6E7A72, which measured only ~3.5:1
// against the background - under the 4.5:1 readability threshold, and this
// carries the footer hints and per-row title IDs on a TV at couch distance.
#define COL_TEXT_DIM       0xFF8A968E
#define COL_SCROLL_TRACK   0x1AFFFFFF
#define COL_SCROLL_THUMB   0x99FFFFFF
#define COL_BAR_TROUGH     0x26FFFFFF

// ---------------------------------------------------------------------------
// Layout metrics
// ---------------------------------------------------------------------------

// Everything below is derived from the real back buffer size rather than
// hardcoded, because it genuinely varies: AtgConsole creates a 1280x720 back
// buffer on an HD display but 640x480 otherwise (AtgConsole.cpp's
// bEnable720p check). The previous layout's fixed 900px-wide rows and 80px
// margins silently ran off the right edge of a 640-wide buffer.
struct UiMetrics
{
    float screenW, screenH;
    float scale;        // 1.0 at 720p, ~0.67 at 480p - multiplies font size and spacing
    float contentX;     // title-safe left edge
    float contentW;     // title-safe width
    float headerTextY;
    float headerRuleY;
    float listY;
    float rowH;
    float iconSize;
    float footerY;
    int visibleRows;
};

static UiMetrics g_M;

static bool ComputeUiMetrics()
{
    // Mirrors AtgConsole::Create()'s own video-mode check exactly, rather
    // than querying the device, so this cannot disagree with the back buffer
    // that was actually created.
    XVIDEO_MODE VideoMode;
    ZeroMemory(&VideoMode, sizeof(VideoMode));
    XGetVideoMode(&VideoMode);

    bool is720p = (VideoMode.dwDisplayWidth >= 1280);

    g_M.screenW = is720p ? 1280.0f : 640.0f;
    g_M.screenH = is720p ? 720.0f : 480.0f;
    g_M.scale = g_M.screenH / 720.0f;

    // Title-safe inset, matching AtgConsole's percentages (90% of the screen
    // on HD, 85% on 4:3) - a real TV can and does crop the rest.
    float safePct = is720p ? 0.90f : 0.85f;
    float safeX = g_M.screenW * (1.0f - safePct) * 0.5f;
    float safeY = g_M.screenH * (1.0f - safePct) * 0.5f;

    g_M.contentX = safeX;
    g_M.contentW = g_M.screenW - safeX * 2.0f;

    g_M.headerTextY = safeY;
    g_M.headerRuleY = safeY + 40.0f * g_M.scale;
    g_M.listY = safeY + 64.0f * g_M.scale;

    g_M.rowH = 76.0f * g_M.scale;
    g_M.iconSize = 60.0f * g_M.scale;

    g_M.footerY = g_M.screenH - safeY - 28.0f * g_M.scale;

    // Fit as many rows as the space between the list top and the footer
    // allows, rather than assuming a fixed six - at 480p six rows of the old
    // fixed height did not fit, and on HD there's room for more.
    float listSpace = g_M.footerY - g_M.listY - 16.0f * g_M.scale;
    g_M.visibleRows = (int)(listSpace / g_M.rowH);
    if (g_M.visibleRows < 1) g_M.visibleRows = 1;
    if (g_M.visibleRows > 16) g_M.visibleRows = 16; // sanity clamp; nothing realistic hits this

    return true;
}

// Full-screen background wash, drawn first every frame. Replaces the flat
// Clear() color - the Clear itself still happens, this just paints over it.
static void DrawBackground()
{
    DrawGradientRect(0.0f, 0.0f, g_M.screenW, g_M.screenH, COL_BG_TOP, COL_BG_BOTTOM, true);
}

bool InitGameListUI()
{
    if (g_Initialized)
        return true;

    g_pd3dDevice = Console::GetDevice();
    if (g_pd3dDevice == NULL)
        return false;

    if (!CreateQuadResources())
        return false;

    if (FAILED(g_UiFont.Create("embed:\\font"))) // same embedded font resource MakeConsole() already loads
        return false;

    if (!ComputeUiMetrics())
        return false;

    g_Initialized = true;
    return true;
}

void ShutdownGameListUI()
{
    if (!g_Initialized)
        return;

    ReleaseIcons(); // cover art is held for the whole session now - see EnsureIconsLoaded

    g_UiFont.Destroy();

    if (g_pWhiteTexture != NULL) { g_pWhiteTexture->Release(); g_pWhiteTexture = NULL; }
    if (g_pQuadPixelShader != NULL) { g_pQuadPixelShader->Release(); g_pQuadPixelShader = NULL; }
    if (g_pQuadVertexShader != NULL) { g_pQuadVertexShader->Release(); g_pQuadVertexShader = NULL; }
    if (g_pQuadVertexDecl != NULL) { g_pQuadVertexDecl->Release(); g_pQuadVertexDecl = NULL; }

    g_Initialized = false;
}

// ---------------------------------------------------------------------------
// Game list
// ---------------------------------------------------------------------------

// Row geometry, icon size and visible-row count all live in g_M now (see
// ComputeUiMetrics) rather than in fixed #defines, so the layout adapts to a
// 640x480 back buffer instead of overflowing it.

static WORD CurrentButtons(); // defined with the other shared screen helpers below

// One loaded cover, plus the SOURCE image's aspect ratio. STFS thumbnails are
// usually square but the header doesn't promise it, and stretching a
// non-square one into a fixed square tile looks wrong - so DrawIconFitted
// letterboxes within a consistent tile box, which keeps rows aligned whatever
// shape the art turns out to be.
struct Icon
{
    D3DTexture *texture;
    float aspect; // source width / source height
};

// Creates a texture from raw PNG/JPEG bytes, recording the source aspect.
// Leaves out->texture NULL on any failure, so callers can just fall through
// to the next source.
static bool CreateIconTexture(const unsigned char *data, unsigned long size, Icon *out)
{
    if (data == NULL || size == 0)
        return false;

    // Aspect comes from the SOURCE image, not the created texture: D3DX may
    // round the texture up to power-of-two dimensions, and it resamples the
    // whole image into whatever it creates, so the texture's own size says
    // nothing about the original proportions while UV 0..1 still covers it.
    float aspect = 1.0f;
    D3DXIMAGE_INFO imgInfo;
    if (SUCCEEDED(D3DXGetImageInfoFromFileInMemory(data, size, &imgInfo)) && imgInfo.Height > 0)
        aspect = (float)imgInfo.Width / (float)imgInfo.Height;

    // MipLevels=1 and an explicit D3DFMT_A8R8G8B8 (not D3DX_DEFAULT /
    // D3DFMT_UNKNOWN) - matching g_pWhiteTexture's own known-working creation
    // parameters exactly. Letting D3DX auto-generate a full mip chain and
    // auto-detect the format from arbitrary image data is a plausible way to
    // end up with a texture the GPU's texture-fetch hardware stalls on -
    // confirmed necessary previously: the call stack for a GPU deadlock went
    // through Present() waiting on a resource fence that never signaled, only
    // once real (not placeholder) icon textures started getting drawn.
    // Explicit dimensions rather than D3DX_DEFAULT (which would keep the
    // source size), so one oversized thumbnail can't quietly cost far more
    // texture memory than a 60px tile can show. 128 leaves better than 2x
    // headroom over the largest size these are ever drawn at; raise it if the
    // tiles ever grow.
    //
    // Resampling to a square does NOT distort non-square art: the aspect
    // recorded above comes from the source image, and DrawIconFitted uses it
    // to draw into a correctly-proportioned rect, which undoes the squash.
    const UINT ICON_TEXTURE_SIZE = 128;

    D3DTexture *texture = NULL;
    HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(
        g_pd3dDevice, data, size,
        ICON_TEXTURE_SIZE, ICON_TEXTURE_SIZE, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &texture);

    if (FAILED(hr) || texture == NULL)
        return false;

    out->texture = texture;
    out->aspect = aspect;
    return true;
}

// Draws a cover centred inside a boxSize x boxSize tile, preserving its
// aspect ratio rather than stretching it to fill.
static void DrawIconFitted(const Icon *icon, float boxX, float boxY, float boxSize)
{
    float w = boxSize;
    float h = boxSize;

    if (icon->aspect > 1.0f)
        h = boxSize / icon->aspect; // wider than tall
    else if (icon->aspect < 1.0f)
        w = boxSize * icon->aspect; // taller than wide

    DrawQuad(icon->texture,
             boxX + (boxSize - w) * 0.5f,
             boxY + (boxSize - h) * 0.5f,
             w, h, 0xFFFFFFFF);
}

// Cover art, loaded once and kept for the life of the UI.
//
// This used to be a local array built on entry and released on exit, which was
// fine when the list was shown exactly once. Now that it's the app's root
// screen and the user returns to it after every download, that meant
// re-reading and re-decoding the whole library - 27 STFS header reads and 27
// PNG decodes - every single time they backed out of a pack. Freed in
// ShutdownGameListUI.
static Icon *g_icons = NULL;
static int g_iconCount = 0;

static void ReleaseIcons()
{
    if (g_icons == NULL)
        return;

    for (int i = 0; i < g_iconCount; ++i)
    {
        if (g_icons[i].texture != NULL)
            g_icons[i].texture->Release();
    }

    free(g_icons);
    g_icons = NULL;
    g_iconCount = 0;
}

static void EnsureIconsLoaded(const InstalledGame *games, int gameCount)
{
    if (g_icons != NULL && g_iconCount == gameCount)
        return; // already loaded for this library

    ReleaseIcons();

    // Installed-game libraries on a modded 360 are realistically dozens, not
    // thousands, of titles, and each image is at most 16KB of source PNG, so
    // loading the lot up front is a bounded one-time cost rather than
    // something that needs lazy/scroll-triggered loading.
    g_icons = (Icon *)malloc(sizeof(Icon) * gameCount);
    if (g_icons == NULL)
        return;

    g_iconCount = gameCount;
    memset(g_icons, 0, sizeof(Icon) * gameCount);

    int fromStfs = 0, placeholders = 0;

    for (int i = 0; i < gameCount; ++i)
    {
        g_icons[i].texture = NULL;
        g_icons[i].aspect = 1.0f;

        StfsTitleInfo info;
        if (StfsReadTitleInfo(games[i].packagePath, &info))
        {
            if (info.titleThumbnail != NULL)
                CreateIconTexture(info.titleThumbnail, info.titleThumbnailSize, &g_icons[i]);

            // Second embedded image - GOD converters frequently leave the
            // title thumbnail zeroed while this one survives.
            if (g_icons[i].texture == NULL && info.contentThumbnail != NULL)
                CreateIconTexture(info.contentThumbnail, info.contentThumbnailSize, &g_icons[i]);

            if (g_icons[i].texture != NULL)
                fromStfs++;
        }
        StfsFreeTitleInfo(&info);

        if (g_icons[i].texture == NULL)
        {
            // Name them. A bare count says three titles came up empty but not
            // WHICH three, and that's the part worth acting on - it's the
            // difference between "some GOD conversions dropped their
            // thumbnails" and "a whole content type isn't being handled".
            // Capped so a library that finds no art at all doesn't flood the
            // log.
            if (placeholders < 10)
                dprintf("  No cover art for \"%s\" (Title ID %08lX)\n",
                        games[i].displayName, games[i].titleId);
            else if (placeholders == 10)
                dprintf("  (further titles without cover art not listed)\n");

            placeholders++;
        }
    }

    dprintf("Cover art: %d from package metadata, %d placeholder\n", fromStfs, placeholders);
}

GameListUIResult ShowGameListUI(const InstalledGame *games, int gameCount, int initialSelection,
                                const bool *hasDlcInstalled)
{
    GameListUIResult result = {false, -1, false};

    if (!g_Initialized || gameCount <= 0)
        return result;

    EnsureIconsLoaded(games, gameCount);
    if (g_icons == NULL)
        return result;

    Icon *icons = g_icons;

    int selected = initialSelection;
    if (selected < 0) selected = 0;
    if (selected > gameCount - 1) selected = gameCount - 1;

    // Start the view with the selection already on screen, rather than
    // scrolled to the top and then snapping once the loop's own clamp runs.
    int scrollOffset = selected - g_M.visibleRows / 2;
    if (scrollOffset > gameCount - g_M.visibleRows) scrollOffset = gameCount - g_M.visibleRows;
    if (scrollOffset < 0) scrollOffset = 0;
    WORD prevButtons = CurrentButtons(); // see CurrentButtons: a still-held A must not read as a fresh press

    // D-pad auto-repeat state. Pure edge-triggered input (which this used to
    // be) means one tap per row - fine for a handful of titles, genuinely
    // painful for a real 27-game library, where reaching the bottom was 26
    // discrete presses. Frame counts rather than milliseconds because the
    // loop below is hand-paced at a fixed ~16ms via Sleep(), so frames ARE
    // the clock here.
    WORD heldDirection = 0;    // which of DPAD_UP/DPAD_DOWN is currently held, 0 if neither
    int repeatCountdown = 0;   // frames until the next auto-repeat move
    const int REPEAT_DELAY_FRAMES = 24; // ~400ms before auto-repeat kicks in, so single taps stay precise
    const int REPEAT_RATE_FRAMES = 5;   // ~85ms between repeats once it does

    for (;;)
    {
        // Console::Render() (Common/AtgConsole.cpp) always pairs its
        // Present() with Resume() before and Suspend() after - "Take away
        // GPU control so that the Guide can be rendered". Matching that
        // exactly, per-frame, is what actually fixed the GPU deadlock this
        // session (confirmed: reached every diagnostic stage with no crash).
        // A later attempt to bracket the whole session instead of each
        // frame was based on an untested guess, not evidence, and made
        // things worse (stuck with no progress) - reverted. Called below,
        // after the A/B early-exit checks rather than here, so a `break`
        // never leaves a Resume() unmatched by its Suspend().
        // Through CurrentButtons(), not a raw XInputGetState: that helper is
        // what folds the left thumbstick into the d-pad bits, and reading the
        // pad directly here would leave the stick working on every screen
        // except this one.
        WORD buttons = CurrentButtons();
        WORD pressed = buttons & ~prevButtons; // edge-triggered: only the frame a button first goes down
        prevButtons = buttons;

        // Auto-repeat: a fresh press moves once immediately and arms the long
        // initial delay; continuing to hold fires at the faster repeat rate.
        // Releasing (or switching direction) disarms it, so a tap is still
        // exactly one row.
        WORD direction = buttons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN);
        if (direction != XINPUT_GAMEPAD_DPAD_UP && direction != XINPUT_GAMEPAD_DPAD_DOWN)
            direction = 0; // neither held, or both at once (a real possibility on a worn d-pad) - treat as no input

        bool moveNow = false;

        if (direction == 0)
        {
            heldDirection = 0;
        }
        else if (direction != heldDirection)
        {
            heldDirection = direction;
            repeatCountdown = REPEAT_DELAY_FRAMES;
            moveNow = true;
        }
        else if (--repeatCountdown <= 0)
        {
            repeatCountdown = REPEAT_RATE_FRAMES;
            moveNow = true;
        }

        if (moveNow)
        {
            if (heldDirection == XINPUT_GAMEPAD_DPAD_UP && selected > 0)
                selected--;
            else if (heldDirection == XINPUT_GAMEPAD_DPAD_DOWN && selected < gameCount - 1)
                selected++;
        }

        // Shoulder buttons jump a full page - the fast way through a large
        // library even with auto-repeat available.
        if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
        {
            selected -= g_M.visibleRows;
            if (selected < 0) selected = 0;
        }
        if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
        {
            selected += g_M.visibleRows;
            if (selected > gameCount - 1) selected = gameCount - 1;
        }

        if (pressed & XINPUT_GAMEPAD_A)
        {
            result.selected = true;
            result.selectedIndex = selected;
            result.titleUpdates = false;
            break;
        }
        if (pressed & XINPUT_GAMEPAD_Y)
        {
            result.selected = true;
            result.selectedIndex = selected;
            result.titleUpdates = true;
            break;
        }
        if (pressed & XINPUT_GAMEPAD_B)
        {
            result.selected = false;
            break;
        }

        if (selected < scrollOffset) scrollOffset = selected;
        if (selected >= scrollOffset + g_M.visibleRows) scrollOffset = selected - g_M.visibleRows + 1;

        g_pd3dDevice->Resume();
        g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG_BOTTOM, 1.0f, 0);

        // Two passes, quads first and then ALL text in a single Font
        // Begin/End. This isn't just a micro-optimization: Font::Begin()
        // installs the font's own shaders and render state, which every
        // DrawQuad() here then overwrites with its own, so interleaving the
        // two (as this used to, with a Begin/End around every single row's
        // name) meant re-establishing that state once per row. Text rendering
        // on this hardware is already the expensive part - each character
        // costs a MultiByteToWideChar plus a glyph-width measurement, enough
        // to measurably throttle download throughput elsewhere in this
        // project - so batching it is worth the slightly less obvious
        // structure.

        // --- Pass 1: quads (background, plates, icons, scroll indicator) ---
        DrawBackground();

        // Accent rule under the header, fading out to the right so it reads as
        // a highlight rather than a hard divider.
        DrawGradientRect(g_M.contentX, g_M.headerRuleY, g_M.contentW, 2.0f * g_M.scale,
                         COL_HEADER_RULE_A, COL_HEADER_RULE_B, false);

        const float rowGap = 6.0f * g_M.scale;
        const float plateH = g_M.rowH - rowGap;
        const float accentW = 4.0f * g_M.scale;
        const float iconX = g_M.contentX + 16.0f * g_M.scale;
        const float scrollW = 5.0f * g_M.scale;
        const float scrollGutter = 18.0f * g_M.scale;

        // Rows stop short of the scrollbar only when one is actually shown,
        // so a small library uses the full width.
        bool showScroll = (gameCount > g_M.visibleRows);
        float plateW = g_M.contentW - (showScroll ? scrollGutter : 0.0f);

        for (int row = 0; row < g_M.visibleRows; ++row)
        {
            int index = scrollOffset + row;
            if (index >= gameCount)
                break;

            float rowY = g_M.listY + row * g_M.rowH;
            float iconY = rowY + (plateH - g_M.iconSize) * 0.5f; // vertically centred in its plate

            if (index == selected)
            {
                // Selected: a brighter plate that falls off to the right, plus
                // a solid accent marker down the left edge. The horizontal
                // falloff is what keeps a full-width highlight from looking
                // like a flat block.
                DrawGradientRect(g_M.contentX, rowY, plateW, plateH,
                                 COL_PANEL_SEL_A, COL_PANEL_SEL_B, false);
                DrawRect(g_M.contentX, rowY, accentW, plateH, COL_ACCENT);
            }
            else
            {
                DrawRect(g_M.contentX, rowY, plateW, plateH, COL_PANEL);
            }

            if (icons[index].texture != NULL)
                DrawIconFitted(&icons[index], iconX, iconY, g_M.iconSize);
            else
                DrawRect(iconX, iconY, g_M.iconSize, g_M.iconSize, COL_ICON_PLACEHLD); // titles with no embedded icon
        }

        // Scrollbar - only when there's actually something off-screen. Without
        // it there's no indication the library extends past the visible rows,
        // let alone how far.
        if (showScroll)
        {
            const float trackX = g_M.contentX + g_M.contentW - scrollW;
            const float trackY = g_M.listY;
            const float trackH = g_M.visibleRows * g_M.rowH - rowGap;

            float thumbH = trackH * ((float)g_M.visibleRows / (float)gameCount);
            float minThumb = 24.0f * g_M.scale;
            if (thumbH < minThumb) thumbH = minThumb; // stays visible on a very large library

            // Positioned by scroll range, not item count, so the thumb lands
            // flush at the bottom on the last page.
            float scrollRange = (float)(gameCount - g_M.visibleRows);
            float thumbY = trackY + (trackH - thumbH) * ((float)scrollOffset / scrollRange);

            DrawRect(trackX, trackY, scrollW, trackH, COL_SCROLL_TRACK);
            DrawGradientRect(trackX, thumbY, scrollW, thumbH, COL_SCROLL_THUMB, COL_ACCENT_DIM, true);
        }

        // --- Pass 2: all text, one Begin/End ---
        g_UiFont.Begin();

        // Header. SetScaleFactors is applied per-glyph inside DrawText (see
        // AtgFont.cpp's m_fXScaleFactor use), not captured at Begin(), so it's
        // safe to change between calls inside a single batch - which is what
        // gives this screen an actual type hierarchy rather than one uniform
        // size everywhere.
        g_UiFont.SetScaleFactors(1.25f * g_M.scale, 1.25f * g_M.scale);
        g_UiFont.DrawText(g_M.contentX, g_M.headerTextY, COL_TEXT_PRIMARY, L"YOUR LIBRARY");

        // Position readout, right-aligned against the content edge.
        // Fixed "%d" specifiers into a generously sized buffer, explicitly
        // null-terminated: _snprintf on this toolchain does not null-terminate
        // on truncation, and its dynamic-precision specifiers have caused a
        // real crash in this project before.
        char counter[64];
        _snprintf(counter, sizeof(counter), "%d / %d", selected + 1, gameCount);
        counter[sizeof(counter) - 1] = '\0';

        WCHAR wideCounter[64];
        Utf8ToWide(counter, wideCounter, 64);

        g_UiFont.SetScaleFactors(0.9f * g_M.scale, 0.9f * g_M.scale);
        g_UiFont.DrawText(g_M.contentX + g_M.contentW, g_M.headerTextY + 6.0f * g_M.scale,
                          COL_TEXT_DIM, wideCounter, ATGFONT_RIGHT);

        // Footer hints. GLYPH_A_BUTTON/GLYPH_B_BUTTON (AtgFont.h) render as
        // empty boxes - those private-use codepoints only have glyph bitmaps
        // in the ATG samples' font assets that were actually built with them
        // baked in; our embedded "embed:\font" resource evidently wasn't.
        // Plain text needs nothing from the font but the basic glyphs already
        // proven working everywhere else in this UI.
        //
        // "Exit", not "Back" - this screen is the root, so it's the one place
        // B leaves the app rather than stepping back a screen. Every other
        // screen says Back, which is what makes that distinction readable.
        g_UiFont.DrawText(g_M.contentX, g_M.footerY, COL_TEXT_DIM,
                          showScroll ? L"A  DLC      Y  Title update      B  Exit      LB / RB  Page"
                                     : L"A  DLC      Y  Title update      B  Exit");

        // Row text. The name is truncated with an ellipsis rather than
        // overrunning into the scrollbar - ATGFONT_TRUNCATED plus a max pixel
        // width is built into Font::DrawText, so this costs nothing to do
        // properly.
        float textX = iconX + g_M.iconSize + 18.0f * g_M.scale;
        float textMaxW = (g_M.contentX + plateW) - textX - 16.0f * g_M.scale;

        for (int row = 0; row < g_M.visibleRows; ++row)
        {
            int index = scrollOffset + row;
            if (index >= gameCount)
                break;

            float rowY = g_M.listY + row * g_M.rowH;
            bool isSelected = (index == selected);

            WCHAR wideName[256];
            Utf8ToWide(games[index].displayName, wideName, 256);

            g_UiFont.SetScaleFactors(1.0f * g_M.scale, 1.0f * g_M.scale);
            g_UiFont.DrawText(textX, rowY + 14.0f * g_M.scale,
                              isSelected ? COL_TEXT_PRIMARY : COL_TEXT_SECONDARY,
                              wideName, ATGFONT_TRUNCATED, textMaxW);

            // Secondary line: the title ID, which is the thing that actually
            // identifies a title when two share a display name.
            char idText[32];
            _snprintf(idText, sizeof(idText), "%08lX", games[index].titleId);
            idText[sizeof(idText) - 1] = '\0';

            WCHAR wideId[32];
            Utf8ToWide(idText, wideId, 32);

            g_UiFont.SetScaleFactors(0.72f * g_M.scale, 0.72f * g_M.scale);
            g_UiFont.DrawText(textX, rowY + 40.0f * g_M.scale,
                              isSelected ? COL_TEXT_SECONDARY : COL_TEXT_DIM, wideId);

            // "Already has DLC" marker, sharing the secondary line with the
            // title ID. Plain text in the accent colour rather than a tick
            // glyph: GLYPH_* codepoints render as empty boxes in the embedded
            // font (see the footer hint comment above), so a checkmark would
            // come out as a blank square on hardware.
            if (hasDlcInstalled != NULL && hasDlcInstalled[index])
            {
                g_UiFont.DrawText(textX + 90.0f * g_M.scale, rowY + 40.0f * g_M.scale,
                                  COL_ACCENT, L"INSTALLED");
            }
        }

        // Leave the font at its default scale - Console keeps its own Font
        // instance, but anything else reusing g_UiFont shouldn't inherit
        // whatever scale the last row happened to set.
        g_UiFont.SetScaleFactors(1.0f, 1.0f);

        g_UiFont.End();

        g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
        g_pd3dDevice->Suspend();

        // Device is created with D3DPRESENT_INTERVAL_IMMEDIATE (no vsync),
        // so pace the loop by hand instead of hammering Present() as fast as
        // the CPU can spin.
        Sleep(16); // ~60fps pacing, not a real vsync wait, just enough to stop hammering the GPU queue
    }

    // Icons are NOT released here - they live until ShutdownGameListUI, so
    // coming back to this screen after a download is instant.
    return result;
}

// ---------------------------------------------------------------------------
// Progress bar
// ---------------------------------------------------------------------------

void RenderProgressFrame(const char *title, const char *statusLine,
                         const char *detailLine, float fraction0to1)
{
    if (!g_Initialized)
        return;

    // A negative fraction means "total size unknown" - draw the empty trough
    // and let the detail line carry the bytes-so-far, rather than showing a
    // 0% bar that looks stalled.
    bool indeterminate = (fraction0to1 < 0.0f);
    if (fraction0to1 < 0.0f) fraction0to1 = 0.0f;
    if (fraction0to1 > 1.0f) fraction0to1 = 1.0f;

    // Same Resume()/Present()/Suspend() bracketing as ShowGameListUI's loop
    // and Console::Render() itself - see the comment there for why.
    g_pd3dDevice->Resume();

    g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG_BOTTOM, 1.0f, 0);

    // Vertically centred block, sized off the same metrics as the list screen
    // so the two read as one app rather than two unrelated screens.
    const float barH = 22.0f * g_M.scale;
    const float blockY = g_M.screenH * 0.5f - 60.0f * g_M.scale;
    const float barY = blockY + 52.0f * g_M.scale;

    // Quads before text, for the same batching reason as ShowGameListUI - see
    // the two-pass comment there.
    DrawBackground();

    DrawGradientRect(g_M.contentX, g_M.headerRuleY, g_M.contentW, 2.0f * g_M.scale,
                     COL_HEADER_RULE_A, COL_HEADER_RULE_B, false);

    DrawRect(g_M.contentX, barY, g_M.contentW, barH, COL_BAR_TROUGH);

    if (!indeterminate && fraction0to1 > 0.0f)
    {
        // Gradient along the fill's length, so a nearly-full bar still reads
        // as having direction rather than as a flat green block.
        DrawGradientRect(g_M.contentX, barY, g_M.contentW * fraction0to1, barH,
                         COL_ACCENT_DIM, COL_ACCENT, false);
    }

    WCHAR wideTitle[256];
    WCHAR wideStatus[256];
    WCHAR wideDetail[256];
    Utf8ToWide(title != NULL ? title : "", wideTitle, 256);
    Utf8ToWide(statusLine != NULL ? statusLine : "", wideStatus, 256);
    Utf8ToWide(detailLine != NULL ? detailLine : "", wideDetail, 256);

    g_UiFont.Begin();

    g_UiFont.SetScaleFactors(1.25f * g_M.scale, 1.25f * g_M.scale);
    g_UiFont.DrawText(g_M.contentX, g_M.headerTextY, COL_TEXT_PRIMARY, L"DOWNLOADING");

    // Pack name truncated rather than overrunning - these are real archive
    // filenames and they get long.
    g_UiFont.SetScaleFactors(1.0f * g_M.scale, 1.0f * g_M.scale);
    g_UiFont.DrawText(g_M.contentX, blockY, COL_TEXT_PRIMARY, wideTitle,
                      ATGFONT_TRUNCATED, g_M.contentW);

    g_UiFont.SetScaleFactors(0.85f * g_M.scale, 0.85f * g_M.scale);
    g_UiFont.DrawText(g_M.contentX, blockY + 28.0f * g_M.scale, COL_TEXT_SECONDARY, wideStatus);

    // Percentage, right-aligned above the bar opposite the status line.
    if (!indeterminate)
    {
        char pct[16];
        _snprintf(pct, sizeof(pct), "%d%%", (int)(fraction0to1 * 100.0f + 0.5f));
        pct[sizeof(pct) - 1] = '\0';

        WCHAR widePct[16];
        Utf8ToWide(pct, widePct, 16);
        g_UiFont.DrawText(g_M.contentX + g_M.contentW, blockY + 28.0f * g_M.scale,
                          COL_ACCENT, widePct, ATGFONT_RIGHT);
    }

    g_UiFont.SetScaleFactors(0.8f * g_M.scale, 0.8f * g_M.scale);
    g_UiFont.DrawText(g_M.contentX, barY + barH + 12.0f * g_M.scale, COL_TEXT_DIM, wideDetail);

    g_UiFont.SetScaleFactors(1.0f, 1.0f);
    g_UiFont.End();

    g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
    g_pd3dDevice->Suspend();
}

// ---------------------------------------------------------------------------
// Shared screen chrome
// ---------------------------------------------------------------------------

// Background + accent rule. Quads only - text goes in the caller's own Font
// batch, per the two-pass structure ShowGameListUI explains.
static void DrawChromeQuads()
{
    DrawBackground();
    DrawGradientRect(g_M.contentX, g_M.headerRuleY, g_M.contentW, 2.0f * g_M.scale,
                     COL_HEADER_RULE_A, COL_HEADER_RULE_B, false);
}

// The heading, drawn at the same size and position on every screen so they
// read as one app rather than a set of unrelated views. Must be called inside
// an open Font Begin/End.
static void DrawChromeHeading(const char *heading)
{
    WCHAR wide[128];
    Utf8ToWide(heading != NULL ? heading : "", wide, 128);

    g_UiFont.SetScaleFactors(1.25f * g_M.scale, 1.25f * g_M.scale);
    g_UiFont.DrawText(g_M.contentX, g_M.headerTextY, COL_TEXT_PRIMARY, wide);
}

// Seeds edge-triggered input from the CURRENT pad state rather than from zero.
//
// Every one of these screens is entered immediately after the A press that
// chose to come here, and A is often still physically held at that moment.
// Starting from zero would read that hold as a fresh press on frame one and
// instantly confirm whatever the new screen defaults to - picking the first
// DLC pack in the list with no chance to even look at it.
// It also folds the LEFT THUMBSTICK into the D-pad up/down bits, so both
// drive navigation and neither needs its own handling. Doing it here rather
// than at each use is what keeps the auto-repeat, edge detection and
// held-direction tracking identical for both inputs - they all key off these
// same two bits and never learn where the input came from.
//
// The threshold is deliberately well above XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE
// (7849): that value exists to reject noise at rest, but a worn 360 stick can
// sit meaningfully off-centre, and a list that creeps on its own because a
// ten-year-old controller has drift is worse than one that needs a firm push.
// Roughly half deflection asks for a deliberate movement.
//
// Pushing the stick one way while holding the d-pad the other sets both bits,
// which the direction check downstream reads as "no clear input" and ignores -
// the right outcome for contradictory input.
static WORD CurrentButtons()
{
    XINPUT_STATE state;
    ZeroMemory(&state, sizeof(state));
    XInputGetState(0, &state);

    WORD buttons = state.Gamepad.wButtons;

    const SHORT NAV_THRESHOLD = 16000;

    if (state.Gamepad.sThumbLY > NAV_THRESHOLD)
        buttons |= XINPUT_GAMEPAD_DPAD_UP;
    else if (state.Gamepad.sThumbLY < -NAV_THRESHOLD)
        buttons |= XINPUT_GAMEPAD_DPAD_DOWN;

    return buttons;
}

// ---------------------------------------------------------------------------
// Status / message screens
// ---------------------------------------------------------------------------

static void RenderStatusFrameInternal(const char *heading, const char *message,
                                      const char *detailLine, const char *footerHint)
{
    if (!g_Initialized)
        return;

    g_pd3dDevice->Resume();
    g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG_BOTTOM, 1.0f, 0);

    DrawChromeQuads();

    const float blockY = g_M.screenH * 0.5f - 40.0f * g_M.scale;

    WCHAR wideMessage[256];
    WCHAR wideDetail[256];
    Utf8ToWide(message != NULL ? message : "", wideMessage, 256);
    Utf8ToWide(detailLine != NULL ? detailLine : "", wideDetail, 256);

    g_UiFont.Begin();

    DrawChromeHeading(heading);

    g_UiFont.SetScaleFactors(1.0f * g_M.scale, 1.0f * g_M.scale);
    g_UiFont.DrawText(g_M.contentX, blockY, COL_TEXT_PRIMARY, wideMessage,
                      ATGFONT_TRUNCATED, g_M.contentW);

    if (detailLine != NULL && detailLine[0] != '\0')
    {
        g_UiFont.SetScaleFactors(0.85f * g_M.scale, 0.85f * g_M.scale);
        g_UiFont.DrawText(g_M.contentX, blockY + 32.0f * g_M.scale, COL_TEXT_SECONDARY,
                          wideDetail, ATGFONT_TRUNCATED, g_M.contentW);
    }

    if (footerHint != NULL)
    {
        WCHAR wideHint[128];
        Utf8ToWide(footerHint, wideHint, 128);
        g_UiFont.SetScaleFactors(0.9f * g_M.scale, 0.9f * g_M.scale);
        g_UiFont.DrawText(g_M.contentX, g_M.footerY, COL_TEXT_DIM, wideHint);
    }

    g_UiFont.SetScaleFactors(1.0f, 1.0f);
    g_UiFont.End();

    g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
    g_pd3dDevice->Suspend();
}

void RenderStatusFrame(const char *heading, const char *message, const char *detailLine)
{
    RenderStatusFrameInternal(heading, message, detailLine, NULL);
}

void ShowMessageUI(const char *heading, const char *message, const char *detailLine)
{
    if (!g_Initialized)
        return;

    WORD prevButtons = CurrentButtons();

    for (;;)
    {
        WORD buttons = CurrentButtons();
        WORD pressed = buttons & ~prevButtons;
        prevButtons = buttons;

        if (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_A))
            break;

        RenderStatusFrameInternal(heading, message, detailLine, "B  Continue");
        Sleep(16);
    }
}

// ---------------------------------------------------------------------------
// Generic list picker
// ---------------------------------------------------------------------------

int ShowChoiceUI(const char *heading, const char **labels, const char **sublabels,
                 int count, int initialSelection)
{
    if (!g_Initialized || labels == NULL || count <= 0)
        return -1;

    // Shorter rows than the game list - there's no artwork to make room for,
    // so row height is set by the two text lines alone.
    const float rowH = 56.0f * g_M.scale;
    const float rowGap = 6.0f * g_M.scale;
    const float plateH = rowH - rowGap;
    const float accentW = 4.0f * g_M.scale;
    const float scrollW = 5.0f * g_M.scale;
    const float scrollGutter = 18.0f * g_M.scale;

    float listSpace = g_M.footerY - g_M.listY - 16.0f * g_M.scale;
    int visibleRows = (int)(listSpace / rowH);
    if (visibleRows < 1) visibleRows = 1;

    int selected = initialSelection;
    if (selected < 0) selected = 0;
    if (selected > count - 1) selected = count - 1;

    int scrollOffset = selected - visibleRows / 2;
    if (scrollOffset > count - visibleRows) scrollOffset = count - visibleRows;
    if (scrollOffset < 0) scrollOffset = 0;

    WORD prevButtons = CurrentButtons();
    WORD heldDirection = 0;
    int repeatCountdown = 0;
    const int REPEAT_DELAY_FRAMES = 24;
    const int REPEAT_RATE_FRAMES = 5;

    for (;;)
    {
        WORD buttons = CurrentButtons();
        WORD pressed = buttons & ~prevButtons;
        prevButtons = buttons;

        // Same auto-repeat rules as the game list - see ShowGameListUI.
        WORD direction = buttons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN);
        if (direction != XINPUT_GAMEPAD_DPAD_UP && direction != XINPUT_GAMEPAD_DPAD_DOWN)
            direction = 0;

        bool moveNow = false;

        if (direction == 0)
        {
            heldDirection = 0;
        }
        else if (direction != heldDirection)
        {
            heldDirection = direction;
            repeatCountdown = REPEAT_DELAY_FRAMES;
            moveNow = true;
        }
        else if (--repeatCountdown <= 0)
        {
            repeatCountdown = REPEAT_RATE_FRAMES;
            moveNow = true;
        }

        if (moveNow)
        {
            if (heldDirection == XINPUT_GAMEPAD_DPAD_UP && selected > 0)
                selected--;
            else if (heldDirection == XINPUT_GAMEPAD_DPAD_DOWN && selected < count - 1)
                selected++;
        }

        if (pressed & XINPUT_GAMEPAD_A)
            return selected;

        if (pressed & XINPUT_GAMEPAD_B)
            return -1;

        if (selected < scrollOffset) scrollOffset = selected;
        if (selected >= scrollOffset + visibleRows) scrollOffset = selected - visibleRows + 1;

        g_pd3dDevice->Resume();
        g_pd3dDevice->Clear(0, NULL, D3DCLEAR_TARGET, COL_BG_BOTTOM, 1.0f, 0);

        // --- Pass 1: quads ---
        DrawChromeQuads();

        bool showScroll = (count > visibleRows);
        float plateW = g_M.contentW - (showScroll ? scrollGutter : 0.0f);

        for (int row = 0; row < visibleRows; ++row)
        {
            int index = scrollOffset + row;
            if (index >= count)
                break;

            float rowY = g_M.listY + row * rowH;

            if (index == selected)
            {
                DrawGradientRect(g_M.contentX, rowY, plateW, plateH,
                                 COL_PANEL_SEL_A, COL_PANEL_SEL_B, false);
                DrawRect(g_M.contentX, rowY, accentW, plateH, COL_ACCENT);
            }
            else
            {
                DrawRect(g_M.contentX, rowY, plateW, plateH, COL_PANEL);
            }
        }

        if (showScroll)
        {
            const float trackX = g_M.contentX + g_M.contentW - scrollW;
            const float trackY = g_M.listY;
            const float trackH = visibleRows * rowH - rowGap;

            float thumbH = trackH * ((float)visibleRows / (float)count);
            float minThumb = 24.0f * g_M.scale;
            if (thumbH < minThumb) thumbH = minThumb;

            float thumbY = trackY + (trackH - thumbH) * ((float)scrollOffset / (float)(count - visibleRows));

            DrawRect(trackX, trackY, scrollW, trackH, COL_SCROLL_TRACK);
            DrawGradientRect(trackX, thumbY, scrollW, thumbH, COL_SCROLL_THUMB, COL_ACCENT_DIM, true);
        }

        // --- Pass 2: all text, one Begin/End ---
        g_UiFont.Begin();

        DrawChromeHeading(heading);

        char counter[64];
        _snprintf(counter, sizeof(counter), "%d / %d", selected + 1, count);
        counter[sizeof(counter) - 1] = '\0';

        WCHAR wideCounter[64];
        Utf8ToWide(counter, wideCounter, 64);

        g_UiFont.SetScaleFactors(0.9f * g_M.scale, 0.9f * g_M.scale);
        g_UiFont.DrawText(g_M.contentX + g_M.contentW, g_M.headerTextY + 6.0f * g_M.scale,
                          COL_TEXT_DIM, wideCounter, ATGFONT_RIGHT);

        g_UiFont.DrawText(g_M.contentX, g_M.footerY, COL_TEXT_DIM,
                          L"A  Download      B  Back");

        float textX = g_M.contentX + 18.0f * g_M.scale;
        float textMaxW = (g_M.contentX + plateW) - textX - 16.0f * g_M.scale;

        for (int row = 0; row < visibleRows; ++row)
        {
            int index = scrollOffset + row;
            if (index >= count)
                break;

            float rowY = g_M.listY + row * rowH;
            bool isSelected = (index == selected);

            // Scene release filenames are long and the interesting part is at
            // the front, so these rely on DrawText's own ellipsis truncation
            // rather than wrapping onto a second line.
            WCHAR wideLabel[256];
            Utf8ToWide(labels[index] != NULL ? labels[index] : "", wideLabel, 256);

            g_UiFont.SetScaleFactors(0.95f * g_M.scale, 0.95f * g_M.scale);
            g_UiFont.DrawText(textX, rowY + 10.0f * g_M.scale,
                              isSelected ? COL_TEXT_PRIMARY : COL_TEXT_SECONDARY,
                              wideLabel, ATGFONT_TRUNCATED, textMaxW);

            if (sublabels != NULL && sublabels[index] != NULL)
            {
                WCHAR wideSub[128];
                Utf8ToWide(sublabels[index], wideSub, 128);

                g_UiFont.SetScaleFactors(0.72f * g_M.scale, 0.72f * g_M.scale);
                g_UiFont.DrawText(textX, rowY + 32.0f * g_M.scale,
                                  isSelected ? COL_TEXT_SECONDARY : COL_TEXT_DIM,
                                  wideSub, ATGFONT_TRUNCATED, textMaxW);
            }
        }

        g_UiFont.SetScaleFactors(1.0f, 1.0f);
        g_UiFont.End();

        g_pd3dDevice->Present(NULL, NULL, NULL, NULL);
        g_pd3dDevice->Suspend();

        Sleep(16);
    }
}
