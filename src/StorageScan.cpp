/*
FILE : StorageScan.cpp
PROJECT : Omni360
DESCRIPTION : What's taking the space on each drive - see StorageScan.h.
*/

#include "StorageScan.h"
#include "StfsParser.h"    // StfsReadTitleInfo, for Cache\TU_ files
#include "OutputConsole.h" // dprintf
#include "parsing.h"       // LogEscapePercent

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef INVALID_FILE_ATTRIBUTES // not in the XDK's headers
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

#define CONTENT_SUFFIX "\\Content\\0000000000000000"
#define XNA_RUNTIME_TITLE 0x584E07D2UL // indie games install as its "DLC"

static const char *const kDrives[STORAGE_MAX_DRIVES] = { "Hdd1", "Usb0", "Usb1", "Usb2" };
static const char *const kPartialFolders[] = { "Omni360Staging", "Omni360-partial" };

// Two snapshots: the one handed out, and the one the next scan fills.
static StorageSnapshot g_buffers[2];
static StorageSnapshot *g_published = &g_buffers[0];
static StorageSnapshot *g_work = &g_buffers[1];
static CRITICAL_SECTION g_lock;
static bool g_lockReady = false;
static volatile LONG g_running = 0;
static int g_generation = 0;
static StorageScanRequest g_request;

static void EnsureLock()
{
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
}

// ---------------------------------------------------------------------------
// Walking
// ---------------------------------------------------------------------------

static bool IsDots(const char *name)
{
    return name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

// Eight hex digits, as title ID and content type folders are named.
static bool ParseHex8(const char *name, unsigned long *out)
{
    if (strlen(name) != 8)
        return false;
    char *end = NULL;
    unsigned long v = strtoul(name, &end, 16);
    if (end == NULL || *end != '\0')
        return false;
    *out = v;
    return true;
}

// Everything under dir, added up. *topItems (if given) counts what's directly
// in it, less the ".data" folders that belong to a package file beside them.
static unsigned long long FolderSize(const char *dir, int *topItems)
{
    char pattern[600];
    _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    pattern[sizeof(pattern) - 1] = '\0';

    unsigned long long total = 0;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do
    {
        if (IsDots(found.cFileName))
            continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            char sub[600];
            _snprintf(sub, sizeof(sub), "%s\\%s", dir, found.cFileName);
            sub[sizeof(sub) - 1] = '\0';
            total += FolderSize(sub, NULL);
            const size_t len = strlen(found.cFileName);
            const bool dataFolder = len > 5 && _stricmp(found.cFileName + len - 5, ".data") == 0;
            if (topItems != NULL && !dataFolder)
                (*topItems)++;
        }
        else
        {
            total += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
            if (topItems != NULL)
                (*topItems)++;
        }
    } while (FindNextFileA(h, &found));
    FindClose(h);
    return total;
}

static StorageKind KindOf(unsigned long titleId, unsigned long contentType)
{
    switch (contentType)
    {
    case 0x00001000: // Xbox 360 Title
    case 0x00005000: // Original Xbox
    case 0x00007000: // Games on Demand
    case 0x00080000: // Game Demo
    case 0x000A0000: // Game Title
    case 0x000D0000: // Arcade
    case 0x000E0000: // XNA indie
        return STORAGE_GAME;
    case 0x00004000:
        return STORAGE_DISC_INSTALL;
    case 0x00000002:
        return titleId == XNA_RUNTIME_TITLE ? STORAGE_GAME : STORAGE_DLC;
    case 0x000B0000:
        return STORAGE_UPDATE;
    case 0x00000001:
        return STORAGE_SAVES;
    default:
        return STORAGE_OTHER;
    }
}

static StorageTitle *TitleRecord(StorageSnapshot &snap, unsigned long titleId, int drive)
{
    for (int i = 0; i < snap.titleCount; ++i)
    {
        if (snap.titles[i].titleId == titleId && snap.titles[i].drive == drive)
            return &snap.titles[i];
    }
    if (snap.titleCount >= STORAGE_MAX_TITLES)
    {
        snap.titlesFull = true;
        return NULL;
    }
    StorageTitle *t = &snap.titles[snap.titleCount++];
    memset(t, 0, sizeof(*t));
    t->titleId = titleId;
    t->drive = drive;
    t->system = (titleId >> 16) == 0xFFFE || titleId == XNA_RUNTIME_TITLE;
    return t;
}

static int DriveOf(const char *path)
{
    const char *colon = strchr(path, ':');
    if (colon == NULL)
        return -1;
    const size_t n = (size_t)(colon - path);
    for (int d = 0; d < STORAGE_MAX_DRIVES; ++d)
    {
        if (strlen(kDrives[d]) == n && _strnicmp(path, kDrives[d], n) == 0)
            return d;
    }
    return -1;
}

// One content-layout root: TitleID\ContentType\... for every title in it.
static void WalkContentRoot(StorageSnapshot &snap, const char *root, int drive)
{
    char pattern[600];
    _snprintf(pattern, sizeof(pattern), "%s\\*", root);
    pattern[sizeof(pattern) - 1] = '\0';

    WIN32_FIND_DATAA titleFound;
    HANDLE th = FindFirstFileA(pattern, &titleFound);
    if (th == INVALID_HANDLE_VALUE)
        return;
    do
    {
        unsigned long titleId = 0;
        if (!(titleFound.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ParseHex8(titleFound.cFileName, &titleId))
            continue; // a folder game's folder, or anything else that isn't a title

        char titleDir[600];
        _snprintf(titleDir, sizeof(titleDir), "%s\\%s", root, titleFound.cFileName);
        titleDir[sizeof(titleDir) - 1] = '\0';
        _snprintf(pattern, sizeof(pattern), "%s\\*", titleDir);
        pattern[sizeof(pattern) - 1] = '\0';

        WIN32_FIND_DATAA typeFound;
        HANDLE yh = FindFirstFileA(pattern, &typeFound);
        if (yh == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            unsigned long contentType = 0;
            if (!(typeFound.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ParseHex8(typeFound.cFileName, &contentType))
                continue;
            char typeDir[600];
            _snprintf(typeDir, sizeof(typeDir), "%s\\%s", titleDir, typeFound.cFileName);
            typeDir[sizeof(typeDir) - 1] = '\0';

            int items = 0;
            const unsigned long long bytes = FolderSize(typeDir, &items);
            if (items == 0 && bytes == 0)
                continue; // an empty folder left behind
            StorageTitle *t = TitleRecord(snap, titleId, drive);
            if (t == NULL)
                continue;
            const StorageKind kind = KindOf(titleId, contentType);
            t->bytes[kind] += bytes;
            t->items[kind] += items;
        } while (FindNextFileA(yh, &typeFound));
        FindClose(yh);
    } while (FindNextFileA(th, &titleFound));
    FindClose(th);
}

// The shared Cache's uppercase title updates: whose they are is only in the
// package header.
static void WalkCacheUpdates(StorageSnapshot &snap, int drive)
{
    char pattern[64];
    _snprintf(pattern, sizeof(pattern), "%s:\\Cache\\TU_*", kDrives[drive]);
    pattern[sizeof(pattern) - 1] = '\0';

    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char path[600];
        _snprintf(path, sizeof(path), "%s:\\Cache\\%s", kDrives[drive], found.cFileName);
        path[sizeof(path) - 1] = '\0';

        StfsTitleInfo info;
        memset(&info, 0, sizeof(info));
        if (!StfsReadTitleInfo(path, &info))
        {
            dprintf("[storage] couldn't read %s's header\n", found.cFileName);
            continue;
        }
        const unsigned long titleId = info.titleId;
        StfsFreeTitleInfo(&info);

        StorageTitle *t = TitleRecord(snap, titleId, drive);
        if (t == NULL)
            continue;
        t->bytes[STORAGE_UPDATE] += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        t->items[STORAGE_UPDATE]++;
    } while (FindNextFileA(h, &found));
    FindClose(h);
}

static void SameRootKey(const char *in, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s", in);
    out[outSize - 1] = '\0';
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == '\\')
        out[--n] = '\0';
}

static void Scan(StorageSnapshot &snap, const StorageScanRequest &req)
{
    const DWORD start = GetTickCount();
    memset(&snap, 0, sizeof(snap));
    snap.driveCount = STORAGE_MAX_DRIVES;

    // Every drive's capacity, and its own content folder - then the extra
    // roots, each only once.
    char roots[STORAGE_MAX_DRIVES + STORAGE_MAX_ROOTS][512];
    int rootCount = 0;
    for (int d = 0; d < STORAGE_MAX_DRIVES; ++d)
    {
        StorageDrive &drive = snap.drives[d];
        _snprintf(drive.name, sizeof(drive.name), "%s", kDrives[d]);
        char root[16];
        _snprintf(root, sizeof(root), "%s:\\", kDrives[d]);
        ULARGE_INTEGER freeToCaller, total;
        if (GetDiskFreeSpaceExA(root, &freeToCaller, &total, NULL) && total.QuadPart > 0)
        {
            drive.present = true;
            drive.capacity = total.QuadPart;
            drive.freeBytes = freeToCaller.QuadPart;
            _snprintf(roots[rootCount], sizeof(roots[0]), "%s:" CONTENT_SUFFIX, kDrives[d]);
            rootCount++;
        }
    }
    for (int r = 0; r < req.rootCount && r < STORAGE_MAX_ROOTS; ++r)
    {
        char key[512];
        SameRootKey(req.roots[r], key, sizeof(key));
        bool seen = false;
        for (int i = 0; i < rootCount && !seen; ++i)
            seen = _stricmp(roots[i], key) == 0;
        if (!seen && key[0] != '\0')
        {
            memcpy(roots[rootCount], key, sizeof(key));
            rootCount++;
        }
    }

    for (int r = 0; r < rootCount; ++r)
    {
        const int d = DriveOf(roots[r]);
        if (d < 0 || !snap.drives[d].present)
            continue;
        const DWORD t0 = GetTickCount();
        WalkContentRoot(snap, roots[r], d);
        dprintf("[storage] walked %s in %lu ms\n", roots[r], GetTickCount() - t0);
    }

    for (int d = 0; d < STORAGE_MAX_DRIVES; ++d)
    {
        if (!snap.drives[d].present)
            continue;
        WalkCacheUpdates(snap, d);
        for (int p = 0; p < sizeof(kPartialFolders) / sizeof(kPartialFolders[0]); ++p)
        {
            char dir[64];
            _snprintf(dir, sizeof(dir), "%s:\\%s", kDrives[d], kPartialFolders[p]);
            dir[sizeof(dir) - 1] = '\0';
            snap.drives[d].partialBytes += FolderSize(dir, &snap.drives[d].partialItems);
        }
    }

    for (int f = 0; f < req.folderCount && f < STORAGE_MAX_FOLDERS; ++f)
    {
        const int d = DriveOf(req.folders[f].path);
        if (d < 0 || !snap.drives[d].present)
            continue;
        StorageTitle *t = TitleRecord(snap, req.folders[f].titleId, d);
        if (t == NULL)
            continue;
        t->bytes[STORAGE_GAME] += FolderSize(req.folders[f].path, NULL);
        t->items[STORAGE_GAME]++;
    }

    // Whether each title has a game on any drive, and each drive's totals.
    for (int i = 0; i < snap.titleCount; ++i)
    {
        StorageTitle &t = snap.titles[i];
        for (int j = 0; j < snap.titleCount && !t.hasGameAnywhere; ++j)
        {
            const StorageTitle &o = snap.titles[j];
            t.hasGameAnywhere = o.titleId == t.titleId && (o.items[STORAGE_GAME] > 0 || o.items[STORAGE_DISC_INSTALL] > 0);
        }
        for (int k = 0; k < STORAGE_KIND_COUNT; ++k)
            snap.drives[t.drive].bytes[k] += t.bytes[k];
    }

    snap.scanMs = (double)(GetTickCount() - start);
}

static DWORD WINAPI StorageScanEntry(LPVOID)
{
    Scan(*g_work, g_request);

    EnterCriticalSection(&g_lock);
    g_work->generation = ++g_generation;
    StorageSnapshot *done = g_work;
    g_work = g_published;
    g_published = done;
    LeaveCriticalSection(&g_lock);

    dprintf("[storage] scan %d done: %d titles in %.0f ms\n", done->generation, done->titleCount, done->scanMs);
    InterlockedExchange(&g_running, 0);
    return 0;
}

bool StartStorageScan(const StorageScanRequest &request)
{
    EnsureLock();
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0)
        return false;

    g_request = request;
    HANDLE thread = CreateThread(NULL, 256 * 1024, StorageScanEntry, NULL, 0, NULL);
    if (thread == NULL)
    {
        dprintf("[storage] CreateThread failed (%lu)\n", GetLastError());
        InterlockedExchange(&g_running, 0);
        return false;
    }
    CloseHandle(thread);
    return true;
}

bool StorageScanRunning()
{
    return g_running != 0;
}

const StorageSnapshot *StorageSnapshotAcquire()
{
    EnsureLock();
    EnterCriticalSection(&g_lock);
    if (g_published->generation == 0)
    {
        LeaveCriticalSection(&g_lock);
        return NULL;
    }
    return g_published;
}

void StorageSnapshotRelease()
{
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------------------
// Removing
// ---------------------------------------------------------------------------

// path and everything under it. False if anything is left.
static bool DeleteTree(const char *path, unsigned long long *freed, int depth)
{
    if (depth > 16)
        return false;
    char pattern[600];
    _snprintf(pattern, sizeof(pattern), "%s\\*", path);
    pattern[sizeof(pattern) - 1] = '\0';

    bool ok = true;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (IsDots(found.cFileName))
                continue;
            char child[600];
            _snprintf(child, sizeof(child), "%s\\%s", path, found.cFileName);
            child[sizeof(child) - 1] = '\0';
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                ok = DeleteTree(child, freed, depth + 1) && ok;
            else if (DeleteFileA(child))
            {
                if (freed != NULL)
                    *freed += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
            }
            else
            {
                dprintf("[storage] couldn't delete %s (%lu)\n", child, GetLastError());
                ok = false;
            }
        } while (FindNextFileA(h, &found));
        FindClose(h);
    }
    if (!RemoveDirectoryA(path) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        ok = false;
    return ok;
}

bool ClearPartialDownloads(unsigned long long *freed)
{
    bool ok = true;
    for (int d = 0; d < STORAGE_MAX_DRIVES; ++d)
    {
        for (int p = 0; p < sizeof(kPartialFolders) / sizeof(kPartialFolders[0]); ++p)
        {
            char dir[64];
            _snprintf(dir, sizeof(dir), "%s:\\%s", kDrives[d], kPartialFolders[p]);
            dir[sizeof(dir) - 1] = '\0';
            if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES)
                continue;
            const bool removed = DeleteTree(dir, freed, 0);
            dprintf("[storage] cleared %s%s\n", dir, removed ? "" : " - not all of it");
            ok = removed && ok;
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// The log summary
// ---------------------------------------------------------------------------

static void Size(unsigned long long bytes, char *out, size_t outSize)
{
    if (bytes >= 1024ULL * 1024 * 1024)
        _snprintf(out, outSize, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else
        _snprintf(out, outSize, "%.1f MB", (double)bytes / (1024.0 * 1024));
    out[outSize - 1] = '\0';
}

static unsigned long long TitleTotal(const StorageTitle &t)
{
    unsigned long long total = 0;
    for (int k = 0; k < STORAGE_KIND_COUNT; ++k)
        total += t.bytes[k];
    return total;
}

static const StorageSnapshot *g_sortSnap;
static int CompareBySize(const void *a, const void *b)
{
    const unsigned long long x = TitleTotal(g_sortSnap->titles[*(const int *)a]);
    const unsigned long long y = TitleTotal(g_sortSnap->titles[*(const int *)b]);
    return x < y ? 1 : x > y ? -1 : 0;
}

static void LogTitle(const StorageSnapshot &snap, const StorageTitle &t, const char *(*nameOf)(unsigned long))
{
    const char *name = nameOf != NULL ? nameOf(t.titleId) : NULL;
    char escaped[300];
    if (name == NULL || !LogEscapePercent(name, escaped, sizeof(escaped)))
        _snprintf(escaped, sizeof(escaped), "?");
    escaped[sizeof(escaped) - 1] = '\0';

    char total[24], game[24], disc[24], dlc[24], upd[24];
    Size(TitleTotal(t), total, sizeof(total));
    Size(t.bytes[STORAGE_GAME], game, sizeof(game));
    Size(t.bytes[STORAGE_DISC_INSTALL], disc, sizeof(disc));
    Size(t.bytes[STORAGE_DLC], dlc, sizeof(dlc));
    Size(t.bytes[STORAGE_UPDATE], upd, sizeof(upd));
    dprintf("[storage]   %s %08lX %s: %s (game %s x%d, disc install %s, DLC %s x%d, updates %s x%d, saves x%d, other x%d)\n",
            snap.drives[t.drive].name, t.titleId, escaped, total, game, t.items[STORAGE_GAME], disc, dlc,
            t.items[STORAGE_DLC], upd, t.items[STORAGE_UPDATE], t.items[STORAGE_SAVES], t.items[STORAGE_OTHER]);
}

void LogStorageSnapshot(const char *(*nameOf)(unsigned long titleId))
{
    const StorageSnapshot *snap = StorageSnapshotAcquire();
    if (snap == NULL)
    {
        dprintf("[storage] no scan yet\n");
        return;
    }

    static const char *const kKindNames[STORAGE_KIND_COUNT] = { "games", "disc installs", "DLC", "updates", "saves",
                                                                "other" };
    for (int d = 0; d < snap->driveCount; ++d)
    {
        const StorageDrive &drive = snap->drives[d];
        if (!drive.present)
            continue;
        char cap[24], fr[24], part[24];
        Size(drive.capacity, cap, sizeof(cap));
        Size(drive.freeBytes, fr, sizeof(fr));
        Size(drive.partialBytes, part, sizeof(part));
        dprintf("[storage] %s: %s free of %s, partial downloads %s (%d)\n", drive.name, fr, cap, part,
                drive.partialItems);
        for (int k = 0; k < STORAGE_KIND_COUNT; ++k)
        {
            char b[24];
            Size(drive.bytes[k], b, sizeof(b));
            dprintf("[storage]   %s: %s\n", kKindNames[k], b);
        }
    }

    static int order[STORAGE_MAX_TITLES];
    for (int i = 0; i < snap->titleCount; ++i)
        order[i] = i;
    g_sortSnap = snap;
    qsort(order, snap->titleCount, sizeof(int), CompareBySize);

    dprintf("[storage] biggest:\n");
    for (int i = 0; i < snap->titleCount && i < 25; ++i)
        LogTitle(*snap, snap->titles[order[i]], nameOf);

    dprintf("[storage] DLC/updates with no game on any drive:\n");
    int leftovers = 0;
    for (int i = 0; i < snap->titleCount; ++i)
    {
        const StorageTitle &t = snap->titles[order[i]];
        if (StorageTitleIsLeftover(t))
        {
            LogTitle(*snap, t, nameOf);
            leftovers++;
        }
    }
    dprintf("[storage] %d leftover candidates, %d titles%s\n", leftovers, snap->titleCount,
            snap->titlesFull ? " (list full - some dropped)" : "");
    StorageSnapshotRelease();
}
