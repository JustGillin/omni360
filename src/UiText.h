/*
FILE : UiText.h
PROJECT : Omni360
DESCRIPTION : The UI's text renderer, backed by XUI rasterising a TrueType
              file directly.

This replaced ATG::Font, which drew from a bitmap atlas baked at one fixed
size by the XDK's FontMaker. That cost two things:

  - Sharpness. The atlas had a 28px strike while the layout wants 22px, so
    every string was resampled away from 1:1 and small text was visibly soft.
    XUI rasterises at whatever size is asked for, so text is drawn at its
    real size rather than scaled to it.

  - Characters. The atlas stopped at codepoint 0x7F, so a trademark or
    registered sign - both of which appear in real title names - had nothing
    to draw and came out as a gap. A TrueType file has no such ceiling.

It also lets the repo ship selawk.ttf unmodified, which is what the SIL Open
Font License contemplates, instead of a derived atlas that cannot keep the
font's name.

The class deliberately mirrors ATG::Font's shape - same method names, same
signatures, same flag values - so switching over changed the type of one
variable rather than forty-six call sites. Sizing still goes through
SetScaleFactors for the same reason: every caller already spoke that
language, and a migration that rewrites every call site is a migration that
introduces bugs in every call site.

ATG::Font is still in the build and still used by ATG::Console for the
pre-UI log output, which has to work before any of this is initialised.
*/

#ifndef __UiText_h
#define __UiText_h

#include <xtl.h>

// Same values ATG::Font uses, so callers can keep passing ATGFONT_* without
// caring which renderer is underneath.
#define UITEXT_LEFT       0x00000000
#define UITEXT_RIGHT      0x00000001
#define UITEXT_CENTER_X   0x00000002
#define UITEXT_CENTER_Y   0x00000004
#define UITEXT_TRUNCATED  0x00000008

class UiFont
{
public:
    UiFont();

    // Brings XUI up against the device ATG::Console already created, loads
    // the typeface, and works out how point sizes map to pixels on this
    // device. The path argument is accepted and ignored - it exists so the
    // call site reads the same as it did with ATG::Font.
    HRESULT Create(const char *ignoredLegacyPath);
    void    Destroy();

    // Brackets a run of DrawText calls. Maps to XuiRenderBegin/XuiRenderEnd.
    //
    // Begin clears with a fully transparent colour, which on hardware leaves
    // everything already drawn this frame intact - confirmed before any of
    // this was built on top of it. That is what lets XUI text sit over the
    // quad path rather than replacing it.
    void Begin();
    void End();

    // Sets the size for subsequent draws, as a multiple of the UI's design
    // text height. Both axes are taken, for signature compatibility, but only
    // the X value is used - XUI has no independent vertical scale, and no
    // caller ever passed different values.
    void SetScaleFactors(float scaleX, float scaleY);

    // Bold or regular for subsequent draws and measurements. Selawik Bold is
    // a second typeface, loaded beside the regular one; if it couldn't be,
    // bold quietly draws regular instead.
    void SetBold(bool bold);

    // dwFlags takes UITEXT_* (identical to ATGFONT_*). fMaxPixelWidth bounds
    // the text and, with UITEXT_TRUNCATED, gives it an ellipsis.
    void DrawText(float sx, float sy, DWORD dwColor, const WCHAR *strText,
                  DWORD dwFlags = 0L, float fMaxPixelWidth = 0.0f);

    // Rendered width of a string at the current scale.
    float GetTextWidth(const WCHAR *strText);

    // The design text height, NOT the underlying font's strike height.
    //
    // ComputeUiMetrics divides the design height by this to correct for a
    // font whose strike differs from what the layout assumes. With XUI that
    // correction is unnecessary, because the size requested is the size
    // drawn - so returning the design height makes that calculation a no-op
    // and leaves the layout code untouched.
    float GetFontHeight() const;

    bool IsReady() const;

private:
    float SizeForScale(float scale) const;
    void *FontForPixelSize(float pixels, bool bold); // HXUIFONT, opaque here to keep XUI headers out of this file

    float m_scale;
    bool  m_bold;
    bool  m_ready;
};

#endif // __UiText_h
