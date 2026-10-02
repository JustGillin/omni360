/*
FILE : CoverArt.cpp
PROJECT : Omni360
DESCRIPTION : Box art from xboxunity.net, else Xbox Live, looked up and cached on a worker
              thread. See CoverArt.h.
*/

#include "CoverArt.h"
#include "downloadFile.h"
#include "HttpPlain.h"
#include "OutputConsole.h"
#include "cJSON.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define COVER_DIR          "game:\\Covers"
#define COVER_INFO_URL     "https://xboxunity.net/Resources/Lib/CoverInfo.php?titleid=%08lX"
#define COVER_IMAGE_URL    "https://xboxunity.net/Resources/Lib/Cover.php?size=large&cid=%s"
#define BOXART_URL         "http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d802%08lx/1033/boxartlg.jpg"

// The marker for a game neither source has a cover for. Renamed from "none",
// which meant only that xboxunity had none - those are asked about again, as
// Xbox Live may have one.
#define NONE_EXTENSION     "none2"

// The cache file: a small header, then the front of the case as pixels.
// Older files read as unusable and are fetched again, once: "OMC1" held the
// whole square with the cover's own header strip, and "OMC2" fronts were cut
// close enough to the spine to keep a sliver of it on some covers.
#define COVER_FILE_MAGIC   0x4F4D4333 // "OMC3"
#define COVER_PIXEL_BYTES  ((unsigned long)COVER_FRONT_W * COVER_SIZE * 4)

// How long "no cover anywhere for this" is believed before asking again.
#define NONE_RETRY_DAYS    7

// Generous: the covers seen are about 530KB, and the listing a few KB.
#define INFO_BUFFER_BYTES  (64 * 1024)
#define IMAGE_BUFFER_BYTES (2 * 1024 * 1024)

#define MAX_REQUESTED      256
#define MAX_READY          4  // decoded covers are 1MB each - don't run far ahead of the UI
#define MAX_SAVES          16

struct CoverFileHeader
{
    unsigned long magic;
    unsigned long width;
    unsigned long height;
    unsigned long reserved;
};

static CRITICAL_SECTION g_lock;
static HANDLE g_wake = NULL;
static HANDLE g_thread = NULL;
static bool g_running = false;
static bool g_shutdown = false;

// What's asked for, and how far through it the worker is.
static unsigned long g_requested[MAX_REQUESTED];
static int g_requestedCount = 0;
static int g_nextRequest = 0;

// Handed over already, this session - not asked for again if the library is
// rescanned.
static unsigned long g_delivered[MAX_REQUESTED];
static int g_deliveredCount = 0;

// The Store's tiles on screen, served first.
#define MAX_STORE_REQUESTED 64
static unsigned long g_storeRequested[MAX_STORE_REQUESTED];
static int g_storeRequestedCount = 0;
static int g_nextStoreRequest = 0;

static CoverData g_ready[MAX_READY];
static int g_readyCount = 0;

struct SaveJob
{
    unsigned long titleId;
    unsigned long *pixels; // NULL writes a "none" marker instead
};
static SaveJob g_saves[MAX_SAVES];
static int g_saveCount = 0;

// ---------------------------------------------------------------------------
// The cache on disk
// ---------------------------------------------------------------------------

static void CoverPath(unsigned long titleId, const char *extension, char *out, size_t outSize)
{
    _snprintf(out, outSize, COVER_DIR "\\%08lX.%s", titleId, extension);
    out[outSize - 1] = '\0';
}

// The cached square, or NULL.
static unsigned char *ReadCachedCover(unsigned long titleId)
{
    char path[64];
    CoverPath(titleId, "bin", path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    CoverFileHeader header;
    unsigned char *pixels = NULL;

    if (fread(&header, sizeof(header), 1, f) == 1 && header.magic == COVER_FILE_MAGIC &&
        header.width == COVER_FRONT_W && header.height == COVER_SIZE)
    {
        pixels = (unsigned char *)malloc(COVER_PIXEL_BYTES);
        if (pixels != NULL && fread(pixels, 1, COVER_PIXEL_BYTES, f) != COVER_PIXEL_BYTES)
        {
            free(pixels);
            pixels = NULL;
        }
    }
    fclose(f);

    if (pixels == NULL)
    {
        // Truncated or from some other version - fetched afresh instead.
        dprintf("[covers] %s is unreadable, fetching the cover again\n", path);
        DeleteFileA(path);
    }
    return pixels;
}

// True while a "none" marker is younger than NONE_RETRY_DAYS.
static bool RecentlyMissing(unsigned long titleId)
{
    char path[64];
    CoverPath(titleId, NONE_EXTENSION, path, sizeof(path));

    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attrs))
        return false;

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);

    ULARGE_INTEGER now, written;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    written.LowPart = attrs.ftLastWriteTime.dwLowDateTime;
    written.HighPart = attrs.ftLastWriteTime.dwHighDateTime;

    const unsigned long long retryAfter = (unsigned long long)NONE_RETRY_DAYS * 24 * 60 * 60 * 10000000ULL;
    return now.QuadPart < written.QuadPart + retryAfter;
}

static void WriteCoverFile(unsigned long titleId, const unsigned long *pixels)
{
    CreateDirectoryA(COVER_DIR, NULL); // fails harmlessly once it exists

    char path[64], noneMarker[64], oldMarker[64];
    CoverPath(titleId, pixels != NULL ? "bin" : NONE_EXTENSION, path, sizeof(path));
    CoverPath(titleId, NONE_EXTENSION, noneMarker, sizeof(noneMarker));
    CoverPath(titleId, "none", oldMarker, sizeof(oldMarker));
    DeleteFileA(oldMarker); // superseded either way

    FILE *f = fopen(path, "wb");
    if (f == NULL)
    {
        dprintf("[covers] couldn't write %s\n", path);
        return;
    }

    bool ok = true;
    if (pixels != NULL)
    {
        CoverFileHeader header = {COVER_FILE_MAGIC, COVER_FRONT_W, COVER_SIZE, 0};
        ok = fwrite(&header, sizeof(header), 1, f) == 1 &&
             fwrite(pixels, 1, COVER_PIXEL_BYTES, f) == COVER_PIXEL_BYTES;
    }
    fclose(f);

    if (!ok)
    {
        dprintf("[covers] writing %s failed - removed\n", path);
        DeleteFileA(path);
    }
    else if (pixels != NULL)
    {
        DeleteFileA(noneMarker); // it has a cover now
    }
}

// ---------------------------------------------------------------------------
// xboxunity
// ---------------------------------------------------------------------------

// The HTTP client's chatter, failures only: browsing the Store fetches covers
// by the dozen, and a few lines for each slowed it and buried the rest.
static void QuietPrint(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';

    if (strstr(line, "ERROR") != NULL || strstr(line, "Failed") != NULL || strstr(line, "failed") != NULL)
        dprintf("%s", line);
}

enum FetchResult
{
    FETCH_OK,
    FETCH_NO_COVER,   // xboxunity and Xbox Live answered, and have nothing for this game
    FETCH_FAILED      // couldn't ask - the network, or an error; tried again next launch
};

// The CoverID to download: the first official cover, else the first listed.
static bool ChooseCoverId(const char *json, unsigned long long jsonLen, char *outId, size_t outSize)
{
    cJSON *root = cJSON_ParseWithLength(json, (size_t)jsonLen);
    if (root == NULL)
        return false;

    const char *chosen = NULL;
    const cJSON *covers = cJSON_GetObjectItemCaseSensitive(root, "Covers");

    for (int pass = 0; pass < 2 && chosen == NULL; ++pass)
    {
        const cJSON *cover = NULL;
        cJSON_ArrayForEach(cover, covers)
        {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(cover, "CoverID");
            const cJSON *official = cJSON_GetObjectItemCaseSensitive(cover, "Official");
            if (!cJSON_IsString(id) || id->valuestring == NULL || id->valuestring[0] == '\0')
                continue;

            bool isOfficial = cJSON_IsString(official) && official->valuestring != NULL &&
                              strcmp(official->valuestring, "1") == 0;
            if (pass == 0 && !isOfficial)
                continue;

            chosen = id->valuestring;
            break;
        }
    }

    bool found = false;
    if (chosen != NULL && strlen(chosen) < outSize)
    {
        // Only digits go into the next URL.
        found = true;
        for (const char *c = chosen; *c != '\0'; ++c)
        {
            if (*c < '0' || *c > '9')
                found = false;
        }
        if (found)
            strcpy(outId, chosen);
    }

    cJSON_Delete(root);
    return found;
}

// Xbox Live's box art, for a game xboxunity has no cover for: the front
// alone, which GameListUI cuts under its header strip like an insert's.
static FetchResult FetchBoxArt(unsigned long titleId, CoverData *out)
{
    char url[160];
    _snprintf(url, sizeof(url), BOXART_URL, titleId);
    url[sizeof(url) - 1] = '\0';

    // About 60KB; the buffer is the insert's size all the same.
    char *image = (char *)malloc(IMAGE_BUFFER_BYTES + 1);
    if (image == NULL)
        return FETCH_FAILED;

    unsigned long long imageLen = 0;
    const int status = HttpGetPlain(url, NULL, image, IMAGE_BUFFER_BYTES, &imageLen);
    if (status == 404)
    {
        dprintf("[covers] %08lX: no cover on xboxunity or Xbox Live\n", titleId);
        free(image);
        return FETCH_NO_COVER;
    }
    if (status != 200 || imageLen < 64)
    {
        dprintf("[covers] %08lX: Xbox Live box art -> HTTP %d, %I64u bytes\n", titleId, status, imageLen);
        free(image);
        return FETCH_FAILED;
    }

    out->titleId = titleId;
    out->kind = COVER_DATA_JPEG;
    out->bytes = (unsigned char *)image;
    out->size = (unsigned long)imageLen;
    return FETCH_OK;
}

static FetchResult FetchCover(unsigned long titleId, CoverData *out)
{
    char url[256];
    _snprintf(url, sizeof(url), COVER_INFO_URL, titleId);
    url[sizeof(url) - 1] = '\0';

    char *info = (char *)malloc(INFO_BUFFER_BYTES + 1);
    if (info == NULL)
        return FETCH_FAILED;

    unsigned long long infoLen = INFO_BUFFER_BYTES;
    int status = httpRequestHTTPS(url, HTTP_GET, NULL, NULL, "", info, &infoLen, false, NULL, 0, QuietPrint);
    if (status != 200)
    {
        dprintf("[covers] %08lX: cover list -> HTTP %d\n", titleId, status);
        free(info);
        return FETCH_FAILED;
    }
    info[infoLen] = '\0';

    char coverId[32];
    bool haveCover = ChooseCoverId(info, infoLen, coverId, sizeof(coverId));
    free(info);

    if (!haveCover)
        return FetchBoxArt(titleId, out);

    _snprintf(url, sizeof(url), COVER_IMAGE_URL, coverId);
    url[sizeof(url) - 1] = '\0';

    char *image = (char *)malloc(IMAGE_BUFFER_BYTES);
    if (image == NULL)
        return FETCH_FAILED;

    unsigned long long imageLen = IMAGE_BUFFER_BYTES;
    status = httpRequestHTTPS(url, HTTP_GET, NULL, NULL, "", image, &imageLen, false, NULL, 0, QuietPrint);
    if (status != 200 || imageLen < 64)
    {
        dprintf("[covers] %08lX: cover %s -> HTTP %d, %I64u bytes\n", titleId, coverId, status, imageLen);
        free(image);
        return FETCH_FAILED;
    }

    out->titleId = titleId;
    out->kind = COVER_DATA_JPEG;
    out->bytes = (unsigned char *)image;
    out->size = (unsigned long)imageLen;
    return FETCH_OK;
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static bool AlreadyDelivered(unsigned long titleId)
{
    for (int i = 0; i < g_deliveredCount; ++i)
    {
        if (g_delivered[i] == titleId)
            return true;
    }
    return false;
}

// Under the lock.
static void Deliver(const CoverData &data)
{
    g_ready[g_readyCount++] = data;
    if (g_deliveredCount < MAX_REQUESTED)
        g_delivered[g_deliveredCount++] = data.titleId;
}

static DWORD WINAPI CoverEntry(LPVOID)
{
    for (;;)
    {
        EnterCriticalSection(&g_lock);

        if (g_shutdown)
        {
            LeaveCriticalSection(&g_lock);
            break;
        }

        // Writing the cache first: it frees a megabyte each.
        if (g_saveCount > 0)
        {
            SaveJob job = g_saves[--g_saveCount];
            LeaveCriticalSection(&g_lock);

            WriteCoverFile(job.titleId, job.pixels);
            if (job.pixels != NULL)
                free(job.pixels);
            continue;
        }

        // Nothing to do, or the UI hasn't caught up: wait to be woken.
        const bool storeWaiting = (g_nextStoreRequest < g_storeRequestedCount);
        if ((!storeWaiting && g_nextRequest >= g_requestedCount) || g_readyCount >= MAX_READY)
        {
            LeaveCriticalSection(&g_lock);
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }

        // The Store's first: they're on screen now.
        const bool forStore = storeWaiting;
        unsigned long titleId = forStore ? g_storeRequested[g_nextStoreRequest++] : g_requested[g_nextRequest++];
        bool skip = !forStore && AlreadyDelivered(titleId);
        LeaveCriticalSection(&g_lock);

        if (skip)
            continue;

        CoverData data;
        memset(&data, 0, sizeof(data));

        unsigned char *cached = ReadCachedCover(titleId);
        if (cached != NULL)
        {
            data.titleId = titleId;
            data.kind = COVER_DATA_PIXELS;
            data.bytes = cached;
            data.size = COVER_PIXEL_BYTES;
        }
        else if (RecentlyMissing(titleId))
        {
            continue;
        }
        else
        {
            FetchResult result = FetchCover(titleId, &data);
            if (result == FETCH_NO_COVER)
                WriteCoverFile(titleId, NULL);
            if (result != FETCH_OK)
                continue;
        }

        data.forStore = forStore;
        EnterCriticalSection(&g_lock);
        if (forStore)
            g_ready[g_readyCount++] = data; // not marked delivered: the Store may ask again
        else
            Deliver(data);
        LeaveCriticalSection(&g_lock);
    }

    return 0;
}

bool StartCoverArt()
{
    if (g_running)
        return true;

    InitializeCriticalSection(&g_lock);

    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
    if (g_wake == NULL)
    {
        dprintf("[covers] CreateEvent failed (%lu)\n", GetLastError());
        return false;
    }

    // The same stack as the other workers, for the same HTTP and TLS buffers.
    g_thread = CreateThread(NULL, 256 * 1024, CoverEntry, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread == NULL)
    {
        dprintf("[covers] CreateThread failed (%lu)\n", GetLastError());
        CloseHandle(g_wake);
        g_wake = NULL;
        return false;
    }

#ifdef _XBOX
    // Hardware thread 5, core 2's second - the download worker's core, but
    // a cover is a moment of TLS between long waits on the network, so the
    // two hardly meet. The search worker has core 1 to itself for matching.
    XSetThreadProcessor(g_thread, 5);
#endif

    g_running = true;
    ResumeThread(g_thread);

    dprintf("[covers] cover worker started\n");
    return true;
}

bool StopCoverArt(DWORD timeoutMs)
{
    if (!g_running)
        return true;

    EnterCriticalSection(&g_lock);
    g_shutdown = true;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);

    bool stopped = (WaitForSingleObject(g_thread, timeoutMs) == WAIT_OBJECT_0);
    if (!stopped)
    {
        dprintf("[covers] still running after %lums - leaving anyway\n", timeoutMs);
        return false;
    }

    CloseHandle(g_thread);
    g_thread = NULL;
    g_running = false;

    // Anything never collected, and squares never written.
    for (int i = 0; i < g_readyCount; ++i)
        free(g_ready[i].bytes);
    g_readyCount = 0;
    for (int i = 0; i < g_saveCount; ++i)
    {
        if (g_saves[i].pixels != NULL)
            free(g_saves[i].pixels);
    }
    g_saveCount = 0;

    return true;
}

void RequestCoverArt(const unsigned long *titleIds, int count)
{
    if (!g_running)
        return;

    if (count > MAX_REQUESTED)
        count = MAX_REQUESTED;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < count; ++i)
        g_requested[i] = titleIds[i];
    g_requestedCount = count;
    g_nextRequest = 0;
    LeaveCriticalSection(&g_lock);

    SetEvent(g_wake);
}

void RequestStoreCoverArt(const unsigned long *titleIds, int count)
{
    if (!g_running)
        return;

    if (count > MAX_STORE_REQUESTED)
        count = MAX_STORE_REQUESTED;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < count; ++i)
        g_storeRequested[i] = titleIds[i];
    g_storeRequestedCount = count;
    g_nextStoreRequest = 0;
    LeaveCriticalSection(&g_lock);

    SetEvent(g_wake);
}

bool TakeCoverData(CoverData *out)
{
    if (!g_running || out == NULL)
        return false;

    bool took = false;

    EnterCriticalSection(&g_lock);
    if (g_readyCount > 0)
    {
        *out = g_ready[0];
        for (int i = 1; i < g_readyCount; ++i)
            g_ready[i - 1] = g_ready[i];
        g_readyCount--;
        took = true;
    }
    LeaveCriticalSection(&g_lock);

    if (took)
        SetEvent(g_wake); // room for the next one
    return took;
}

static void QueueSave(unsigned long titleId, unsigned long *pixels)
{
    if (!g_running)
    {
        if (pixels != NULL)
            free(pixels);
        return;
    }

    bool queued = false;

    EnterCriticalSection(&g_lock);
    if (g_saveCount < MAX_SAVES)
    {
        g_saves[g_saveCount].titleId = titleId;
        g_saves[g_saveCount].pixels = pixels;
        g_saveCount++;
        queued = true;
    }
    LeaveCriticalSection(&g_lock);

    if (queued)
    {
        SetEvent(g_wake);
    }
    else if (pixels != NULL)
    {
        // Only if the UI runs far ahead of the disk; it's fetched again next
        // launch.
        free(pixels);
    }
}

void SaveCoverPixels(unsigned long titleId, unsigned long *pixels)
{
    QueueSave(titleId, pixels);
}

void MarkCoverUnusable(unsigned long titleId)
{
    QueueSave(titleId, NULL);
}
