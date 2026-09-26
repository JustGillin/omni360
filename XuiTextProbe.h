/*
FILE : XuiTextProbe.h
PROJECT : Omni360
DESCRIPTION : Spike - can XUI's text rendering share a frame with this app's
              own D3D quad path?

This exists to answer ONE question before anything is built on top of it.

The UI currently draws text through ATG::Font, which renders from a bitmap
atlas baked at a fixed strike size. That forces two compromises: every string
is resampled away from 1:1 (so small text is soft - see g_M.textScale), and the
atlas has a fixed character range, so a title with an accented character draws
the unknown glyph.

XUI rasterises from a TrueType file at whatever size is asked for, which would
remove both problems and the whole FontMaker step with them. But XUI wants to
initialise against the D3D device and brackets its drawing with
XuiRenderBegin/XuiRenderEnd - and XuiRenderBegin takes a MANDATORY clear
colour with no documented way to skip it. If it clears, it erases every quad
drawn before it that frame.

That is the question. Everything else about adopting XUI depends on the
answer, and nothing in the headers settles it - only hardware does.

Nothing here is load-bearing: if initialisation fails, or is never called, the
app renders exactly as it did before. The probe draws one string over the game
list and logs the result of every call it makes.
*/

#ifndef __XuiTextProbe_h
#define __XuiTextProbe_h

// Brings XUI up against the device Console already created. Safe to call more
// than once; the second call is a no-op.
//
// Returns false and logs the failing step if any part of the chain fails.
// Callers should carry on regardless - a false here means "no probe text this
// run", not "the UI is broken".
bool InitXuiText();

// True once InitXuiText has succeeded, so callers can skip the probe rather
// than calling into an uninitialised XUI.
bool IsXuiTextReady();

// Draws one string at pixel coordinates, bracketed by its own
// XuiRenderBegin/End. Does nothing if XUI is not ready.
//
// Call this AFTER the frame's own quads and ATG text, immediately before
// Present. If the screen comes back with only this string on it, XuiRenderBegin
// cleared the target and XUI cannot share a frame this way.
void DrawXuiTextProbe(float x, float y, const wchar_t *text, unsigned long color);

// Tears XUI down in reverse order. Safe to call when init never succeeded.
void ShutdownXuiText();

#endif // __XuiTextProbe_h
