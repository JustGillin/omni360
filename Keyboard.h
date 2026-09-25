#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <string>
#include <xtl.h>

// Shows the system on-screen keyboard (XShowKeyboardUI) and blocks until the
// user presses Done or Cancel. Returns ERROR_SUCCESS with *outputString set
// on Done, or the XShowKeyboardUI/XGetOverlappedResult error code (including
// ERROR_CANCELLED) otherwise. Lifted as-is from X-Store's original search-box
// input code (user interface/ui.cpp) - the only working, XDK-appropriate way
// to get typed text on a console with no attached keyboard, so it's reused
// verbatim rather than reinvented.
//
// Note: the on-screen keyboard shows typed characters as you type - there's
// no password-masking flag wired up here, so archive.org password entry is
// visible on screen while typing (same caveat as this project's earlier
// Aurora Lua prototype had for the same reason).
DWORD OpenKeyboardToString(
    DWORD userIndex,
    std::string *outputString,
    LPCWSTR title,
    LPCWSTR description,
    LPCWSTR defaultText);

#endif
