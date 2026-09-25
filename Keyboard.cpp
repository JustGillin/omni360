/*
FILE : Keyboard.cpp
PROJECT : xstore (DLC fork)
DESCRIPTION : on-screen keyboard text input, lifted from X-Store's original
              "user interface/ui.cpp" (OpenKeyboardToString/WideToCharSimple)
              since that whole file is otherwise Vimm-search-specific and
              being dropped from this fork - only this reusable piece is kept.
*/

#include "Keyboard.h"
#include "OutputConsole.h"

static void WideToCharSimple(const WCHAR *src, char *dst, DWORD dstSize)
{
    if (!dst || dstSize == 0)
        return;

    dst[0] = '\0';

    if (!src)
        return;

    DWORD i = 0;

    for (; i < dstSize - 1 && src[i] != L'\0'; ++i)
    {
        // Simple narrow conversion. Characters outside 8-bit range become '?'.
        dst[i] = (src[i] <= 0xFF) ? (char)src[i] : '?';
    }

    dst[i] = '\0';
}

DWORD OpenKeyboardToString(
    DWORD userIndex,
    std::string *outputString,
    LPCWSTR title,
    LPCWSTR description,
    LPCWSTR defaultText)
{
    const DWORD MAX_KEYBOARD_CHARS = 256;
    WCHAR wideResult[MAX_KEYBOARD_CHARS];
    char finalResult[MAX_KEYBOARD_CHARS];
    ZeroMemory(wideResult, sizeof(wideResult));
    finalResult[0] = '\0';

    XOVERLAPPED overlapped;
    ZeroMemory(&overlapped, sizeof(overlapped));

    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent)
    {
        dprintf("CreateEvent failed\n");
        return GetLastError();
    }

    // XShowKeyboardUI launches a separate system title (vk.xex) to actually
    // render the on-screen keyboard. Calling it again immediately after a
    // previous keyboard closes is a known trap: vk.xex is still tearing
    // down (confirmed from a real log: the debugger's own "Unloaded
    // 'vk.xex'" line landed AFTER our very next XShowKeyboardUI call had
    // already failed) and the call returns an immediate error instead of
    // ERROR_IO_PENDING - not because the keyboard itself is broken, just
    // busy. Retry with a short backoff rather than treating the first
    // failure as final; this is exactly the back-to-back access-key->secret-key
    // prompt sequence PromptKeys() does.
    DWORD result;
    const int MAX_ATTEMPTS = 5;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; ++attempt)
    {
        result = XShowKeyboardUI(
            userIndex,
            VKBD_LATIN_FULL | VKBD_HIGHLIGHT_TEXT,
            defaultText,
            title,
            description,
            wideResult,
            MAX_KEYBOARD_CHARS,
            &overlapped);

        if (result == ERROR_IO_PENDING)
            break;

        dprintf("Keyboard Error: XShowKeyboardUI failed, result=%lu/0x%08X (attempt %d/%d)\n",
                result, result, attempt, MAX_ATTEMPTS);

        if (attempt < MAX_ATTEMPTS)
            Sleep(500);
    }

    if (result != ERROR_IO_PENDING)
    {
        CloseHandle(overlapped.hEvent);
        return result;
    }

    // Blocking wait until the user presses Done or Cancel.
    result = XGetOverlappedResult(&overlapped, NULL, TRUE);

    DWORD extended = XGetOverlappedExtendedError(&overlapped);

    CloseHandle(overlapped.hEvent);

    if (result != ERROR_SUCCESS)
    {
        dprintf("Failed to get string from keyboard\n");
        dprintf("XGetOverlappedResult result: %lu / 0x%08X\n", result, result);
        dprintf("Keyboard extended: %lu / 0x%08X\n", extended, extended);
        return result;
    }

    // ERROR_CANCELLED means the user backed out.
    if (extended != ERROR_SUCCESS)
    {
        dprintf("Keyboard input canceled by user\n");
        return extended;
    }

    WideToCharSimple(wideResult, finalResult, MAX_KEYBOARD_CHARS);

    outputString->assign(finalResult);

    return ERROR_SUCCESS;
}
