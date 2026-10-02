/*
FILE : SearchWorker.cpp
PROJECT : Omni360
DESCRIPTION : DLC and title update searches on a worker thread, so the picker
              opens straight away instead of holding the screen. See
              SearchWorker.h.
*/

#include "SearchWorker.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <string.h>

static CRITICAL_SECTION g_lock;
static HANDLE g_wake = NULL; // set when a search is asked for, or to shut down
static HANDLE g_thread = NULL;
static bool g_running = false;
static bool g_shutdown = false;
static int g_nextRequestId = 1;

// The newest search asked for and not yet started. A newer request simply
// replaces it - nobody is waiting on the old one any more.
static bool g_pending = false;
static int g_pendingId = 0;
static SearchKind g_pendingKind = SEARCH_DLC;
static char g_pendingName[256] = "";

// The last search to finish, until it's collected. Only one is kept: the UI
// only ever waits on its newest request.
static bool g_haveResult = false;
static SearchResult g_result;

// Worker-only: the search being run, filled without holding the lock. Static
// rather than on the stack - it's 8KB of match arrays.
static SearchResult g_working;

static DWORD WINAPI SearchEntry(LPVOID)
{
    char gameName[256];

    for (;;)
    {
        bool haveWork = false;

        EnterCriticalSection(&g_lock);

        if (g_shutdown)
        {
            LeaveCriticalSection(&g_lock);
            break;
        }

        if (g_pending)
        {
            g_pending = false;
            g_working.requestId = g_pendingId;
            g_working.kind = g_pendingKind;
            memcpy(gameName, g_pendingName, sizeof(gameName));
            haveWork = true;
        }

        LeaveCriticalSection(&g_lock);

        if (!haveWork)
        {
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }

        dprintf("[search] %s for \"%s\" (request %d)\n",
                g_working.kind == SEARCH_DLC ? "DLC" : "title updates", gameName, g_working.requestId);

        if (g_working.kind == SEARCH_DLC)
            g_working.count = FindDlcRarFilenames(gameName, g_working.packs, MAX_DLC_RAR_MATCHES, dprintf);
        else
            g_working.count = FindTitleUpdates(gameName, g_working.updates, MAX_TITLE_UPDATE_MATCHES, dprintf);

        EnterCriticalSection(&g_lock);
        g_result = g_working;
        g_haveResult = true;
        LeaveCriticalSection(&g_lock);
    }

    return 0;
}

bool StartSearchWorker()
{
    if (g_running)
        return true;

    InitializeCriticalSection(&g_lock);

    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
    if (g_wake == NULL)
    {
        dprintf("[search] CreateEvent failed (%lu)\n", GetLastError());
        return false;
    }

    // The same stack as the download worker, for the same HTTP and TLS
    // buffers - see StartDownloadQueue.
    g_thread = CreateThread(NULL, 256 * 1024, SearchEntry, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread == NULL)
    {
        dprintf("[search] CreateThread failed (%lu)\n", GetLastError());
        CloseHandle(g_wake);
        g_wake = NULL;
        return false;
    }

#ifdef _XBOX
    // Hardware thread 3, core 1's second. The download worker is on core 2,
    // where its TLS decryption is the CPU-heavy part of a download, and
    // matching a listing is real work too, so the two don't share a core.
    // Core 1's other thread is the disc install's read-ahead, which spends
    // its time waiting on the drive.
    XSetThreadProcessor(g_thread, 3);
#endif

    g_running = true;
    ResumeThread(g_thread);

    dprintf("[search] search worker started\n");
    return true;
}

bool StopSearchWorker(DWORD timeoutMs)
{
    if (!g_running)
        return true;

    EnterCriticalSection(&g_lock);
    g_shutdown = true;
    g_pending = false;
    LeaveCriticalSection(&g_lock);

    SetEvent(g_wake);

    bool stopped = (WaitForSingleObject(g_thread, timeoutMs) == WAIT_OBJECT_0);
    if (stopped)
    {
        CloseHandle(g_thread);
        g_thread = NULL;
        g_running = false;
    }
    else
    {
        dprintf("[search] search still running after %lums - leaving anyway\n", timeoutMs);
    }

    return stopped;
}

int BeginSearch(SearchKind kind, const char *gameName)
{
    if (!g_running)
        return 0;

    EnterCriticalSection(&g_lock);

    int id = g_nextRequestId++;
    g_pending = true;
    g_pendingId = id;
    g_pendingKind = kind;
    strncpy(g_pendingName, gameName != NULL ? gameName : "", sizeof(g_pendingName) - 1);
    g_pendingName[sizeof(g_pendingName) - 1] = '\0';

    LeaveCriticalSection(&g_lock);

    SetEvent(g_wake);
    return id;
}

bool TakeSearchResult(int requestId, SearchResult *out)
{
    if (!g_running || out == NULL || requestId == 0)
        return false;

    bool found = false;

    EnterCriticalSection(&g_lock);
    if (g_haveResult && g_result.requestId == requestId)
    {
        *out = g_result;
        g_haveResult = false;
        found = true;
    }
    LeaveCriticalSection(&g_lock);

    return found;
}
