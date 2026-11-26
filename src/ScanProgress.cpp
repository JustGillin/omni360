/*
FILE : ScanProgress.cpp
PROJECT : Omni360
DESCRIPTION : How far a library read has got. See ScanProgress.h.
*/

#include "ScanProgress.h"

#include <xtl.h>
#include <stdio.h>
#include <string.h>

static CRITICAL_SECTION g_lock;
static volatile LONG g_lockReady = 0;
static ScanProgressSnapshot g_progress;
static DWORD g_thread = 0; // the reading thread; 0 when none

static void Lock()
{
    // Made on first use - from the UI thread, or the first read's.
    if (InterlockedCompareExchange(&g_lockReady, 1, 0) == 0)
    {
        InitializeCriticalSection(&g_lock);
        InterlockedExchange(&g_lockReady, 2);
    }
    while (g_lockReady != 2)
        Sleep(0);
    EnterCriticalSection(&g_lock);
}

static void Unlock()
{
    LeaveCriticalSection(&g_lock);
}

void ScanProgressBegin(int folderCount, bool apps)
{
    Lock();
    // What couldn't be read stays the library read's, for its toast.
    const int skipped = apps ? g_progress.skipped : 0;
    memset(&g_progress, 0, sizeof(g_progress));
    g_progress.running = true;
    g_progress.apps = apps;
    g_progress.skipped = skipped;
    g_progress.folderCount = folderCount;
    g_thread = GetCurrentThreadId();
    Unlock();
}

void ScanProgressFolder(int index, const char *path, int gamesSoFar)
{
    Lock();
    if (g_thread == GetCurrentThreadId())
    {
        g_progress.folder = index;
        g_progress.games = gamesSoFar;
        g_progress.folderGames = 0;
        _snprintf(g_progress.path, sizeof(g_progress.path), "%s", path);
        g_progress.path[sizeof(g_progress.path) - 1] = '\0';
    }
    Unlock();
}

void ScanProgressGame()
{
    if (g_thread != GetCurrentThreadId())
        return;
    Lock();
    g_progress.games++;
    g_progress.folderGames++;
    Unlock();
}

void ScanProgressSkipped()
{
    if (g_thread != GetCurrentThreadId())
        return;
    Lock();
    g_progress.skipped++;
    Unlock();
}

void ScanProgressEnd()
{
    Lock();
    if (g_thread == GetCurrentThreadId())
    {
        g_progress.running = false;
        g_progress.path[0] = '\0';
        g_thread = 0;
    }
    Unlock();
}

void ScanProgressGet(ScanProgressSnapshot *out)
{
    Lock();
    *out = g_progress;
    Unlock();
}
