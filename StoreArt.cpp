/*
FILE : StoreArt.cpp
PROJECT : Omni360
DESCRIPTION : The Store's artwork and details from Xbox Live, fetched and
              cached on a worker thread. See StoreArt.h.
*/

#include "StoreArt.h"
#include "HttpPlain.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STORE_DIR       "game:\\Store"
#define IMAGE_BASE_URL  "http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d802%08lx/1033/%s"
#define CATALOG_URL     "http://catalog.xboxlive.com/Catalog/Catalog.asmx/Query?methodName=FindGames" \
                        "&Names=Locale&Values=en-US&Names=LegalLocale&Values=en-US&Names=Store&Values=1" \
                        "&Names=PageSize&Values=10&Names=PageNum&Values=1&Names=DetailView&Values=5" \
                        "&Names=OfferFilterLevel&Values=1&Names=MediaIds&Values=66acd000-77fe-1000-9115-d802%08lx" \
                        "&Names=UserTypes&Values=2&Names=MediaTypes&Values=1&Names=MediaTypes&Values=21" \
                        "&Names=MediaTypes&Values=23&Names=MediaTypes&Values=37&Names=MediaTypes&Values=46"

#define NONE_RETRY_DAYS   7
#define IMAGE_MAX_BYTES   (1024 * 1024)  // screenshots are about 250KB, backgrounds under 100KB
#define CATALOG_MAX_BYTES (256 * 1024)   // a game's entry is about 13KB
#define MAX_REQUESTS      64
#define MAX_READY         4
#define MAX_DETAILS       8

struct ArtRequest
{
    unsigned long titleId;
    StoreArtKind kind;
    bool done; // fetched, read or given up on
};

struct DetailsSlot
{
    StoreDetailsState state;
    unsigned long asked; // when, by GetTickCount - the oldest slot is reused
    StoreDetails details;
};

static CRITICAL_SECTION g_lock;
static HANDLE g_wake = NULL;
static HANDLE g_thread = NULL;
static bool g_running = false;
static bool g_shutdown = false;

static ArtRequest g_requests[MAX_REQUESTS];
static int g_requestCount = 0;

static StoreArtData g_ready[MAX_READY];
static int g_readyCount = 0;

static DetailsSlot g_details[MAX_DETAILS];

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

static void FileNameOf(StoreArtKind kind, char *out, size_t outSize)
{
    if (kind >= STORE_ART_SCREEN)
        _snprintf(out, outSize, "screenlg%d.jpg", kind - STORE_ART_SCREEN);
    else
        _snprintf(out, outSize, "background.jpg");
    out[outSize - 1] = '\0';
}

static void CachePath(unsigned long titleId, StoreArtKind kind, const char *suffix, char *out, size_t outSize)
{
    if (kind >= STORE_ART_SCREEN)
        _snprintf(out, outSize, STORE_DIR "\\%08lX.s%d.%s", titleId, kind - STORE_ART_SCREEN, suffix);
    else
        _snprintf(out, outSize, STORE_DIR "\\%08lX.bg.%s", titleId, suffix);
    out[outSize - 1] = '\0';
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// The whole file, malloc'd with a NUL after it, or NULL.
static unsigned char *ReadWholeFile(const char *path, unsigned long maxBytes, unsigned long *outSize)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    unsigned char *bytes = NULL;
    if (fseek(f, 0, SEEK_END) == 0)
    {
        long size = ftell(f);
        if (size > 0 && (unsigned long)size <= maxBytes && fseek(f, 0, SEEK_SET) == 0)
        {
            bytes = (unsigned char *)malloc(size + 1);
            if (bytes != NULL && fread(bytes, 1, size, f) != (size_t)size)
            {
                free(bytes);
                bytes = NULL;
            }
            if (bytes != NULL)
            {
                bytes[size] = '\0';
                *outSize = (unsigned long)size;
            }
        }
    }
    fclose(f);
    return bytes;
}

static bool WriteWholeFile(const char *path, const unsigned char *bytes, unsigned long size)
{
    CreateDirectoryA(STORE_DIR, NULL); // fails harmlessly once it exists
    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return false;
    bool ok = (size == 0 || fwrite(bytes, 1, size, f) == size);
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
        DeleteFileA(path);
    return ok;
}

// True while a file - a "none" marker, or a cached catalog entry - is
// younger than NONE_RETRY_DAYS.
static bool WrittenRecently(const char *markerPath)
{
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExA(markerPath, GetFileExInfoStandard, &attrs))
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

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

// From the cache, else the marketplace. NULL if there's nothing to show.
static unsigned char *LoadArt(unsigned long titleId, StoreArtKind kind, unsigned long *outSize)
{
    char path[64], marker[64], file[32];
    CachePath(titleId, kind, "jpg", path, sizeof(path));
    CachePath(titleId, kind, "none", marker, sizeof(marker));
    FileNameOf(kind, file, sizeof(file));

    unsigned char *bytes = ReadWholeFile(path, IMAGE_MAX_BYTES, outSize);
    if (bytes != NULL)
        return bytes;
    if (WrittenRecently(marker))
        return NULL;

    char url[160];
    _snprintf(url, sizeof(url), IMAGE_BASE_URL, titleId, file);
    url[sizeof(url) - 1] = '\0';

    char *image = (char *)malloc(IMAGE_MAX_BYTES + 1);
    if (image == NULL)
        return NULL;

    unsigned long long len = 0;
    int status = HttpGetPlain(url, NULL, image, IMAGE_MAX_BYTES, &len);
    if (status != 200 || len < 64)
    {
        dprintf("[store] %08lX %s: HTTP %d, %I64u bytes\n", titleId, file, status, len);
        free(image);
        // The marketplace answering is the only sign it has nothing; a
        // network failure is tried again next time.
        if (status == 404 || status == 403)
            WriteWholeFile(marker, NULL, 0);
        return NULL;
    }

    if (!WriteWholeFile(path, (unsigned char *)image, (unsigned long)len))
        dprintf("[store] couldn't write %s\n", path);
    DeleteFileA(marker);

    *outSize = (unsigned long)len;
    return (unsigned char *)image;
}

// ---------------------------------------------------------------------------
// Details
// ---------------------------------------------------------------------------

// Appends a code point as UTF-8.
static int PutUtf8(unsigned long cp, char *out, int room)
{
    if (cp < 0x80 && room >= 1) { out[0] = (char)cp; return 1; }
    if (cp < 0x800 && room >= 2)
    {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000 && room >= 3)
    {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    return 0;
}

// The text between <live:name> and </live:name> from `from`, with XML's
// escapes undone and runs of spaces folded. False if there's no such tag.
// *next, if given, is left after the closing tag.
static bool TagText(const char *from, const char *name, char *out, size_t outSize, const char **next)
{
    char open[64], close[64];
    _snprintf(open, sizeof(open), "<live:%s>", name);
    _snprintf(close, sizeof(close), "</live:%s>", name);
    open[sizeof(open) - 1] = close[sizeof(close) - 1] = '\0';

    const char *start = strstr(from, open);
    if (start == NULL)
        return false;
    start += strlen(open);
    const char *end = strstr(start, close);
    if (end == NULL)
        return false;
    if (next != NULL)
        *next = end + strlen(close);

    int o = 0;
    const int room = (int)outSize - 1;
    bool lastSpace = true; // trims leading space
    for (const char *p = start; p < end && o < room;)
    {
        unsigned long cp = 0;
        int take = 1;
        if (*p == '&')
        {
            const char *semi = (const char *)memchr(p, ';', end - p < 10 ? end - p : 10);
            if (semi != NULL)
            {
                take = (int)(semi - p) + 1;
                if (strncmp(p, "&amp;", 5) == 0) cp = '&';
                else if (strncmp(p, "&lt;", 4) == 0) cp = '<';
                else if (strncmp(p, "&gt;", 4) == 0) cp = '>';
                else if (strncmp(p, "&quot;", 6) == 0) cp = '"';
                else if (strncmp(p, "&apos;", 6) == 0) cp = '\'';
                else if (p[1] == '#' && (p[2] == 'x' || p[2] == 'X')) cp = strtoul(p + 3, NULL, 16);
                else if (p[1] == '#') cp = strtoul(p + 2, NULL, 10);
                else { cp = '&'; take = 1; }
            }
            else
            {
                cp = '&';
            }
            if (cp == 0)
                cp = '?';
        }
        else if (*p == '\r' || *p == '\t')
        {
            cp = ' ';
        }

        if (cp == ' ' || (cp == 0 && *p == ' '))
        {
            if (!lastSpace)
                out[o++] = ' ';
            lastSpace = true;
        }
        else if (cp == 0)
        {
            // A raw byte - UTF-8 passes straight through; so does '\n'.
            if (*p == '\n')
            {
                while (o > 0 && out[o - 1] == ' ')
                    o--;
                out[o++] = '\n';
                lastSpace = true;
            }
            else
            {
                out[o++] = *p;
                lastSpace = false;
            }
        }
        else
        {
            int n = PutUtf8(cp, out + o, room - o);
            if (n == 0)
                break;
            o += n;
            lastSpace = false;
        }
        p += take;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n'))
        o--;
    out[o] = '\0';
    return true;
}

static long TagNumber(const char *xml, const char *name)
{
    char text[32];
    return TagText(xml, name, text, sizeof(text), NULL) ? atol(text) : -1;
}

static void AppendPart(char *out, size_t outSize, const char *part)
{
    if (part[0] == '\0')
        return;
    size_t used = strlen(out);
    _snprintf(out + used, outSize - used, "%s%s", used > 0 ? "  \xC2\xB7  " : "", part);
    out[outSize - 1] = '\0';
}

// "2-16", or "16" when the two are the same; empty for none.
static void Range(long lo, long hi, char *out, size_t outSize)
{
    out[0] = '\0';
    if (hi <= 0)
        return;
    if (lo > 0 && lo != hi)
        _snprintf(out, outSize, "%ld-%ld", lo, hi);
    else
        _snprintf(out, outSize, "%ld", hi);
    out[outSize - 1] = '\0';
}

static bool ParseCatalog(const char *xml, unsigned long titleId, StoreDetails *d)
{
    memset(d, 0, sizeof(*d));
    d->titleId = titleId;

    if (TagNumber(xml, "totalItems") <= 0 || strstr(xml, "<entry") == NULL)
        return false;

    TagText(xml, "developer", d->developer, sizeof(d->developer), NULL);
    TagText(xml, "publisher", d->publisher, sizeof(d->publisher), NULL);
    TagText(xml, "reducedDescription", d->description, sizeof(d->description), NULL);

    char number[32];
    if (TagText(xml, "ratingAggregate", number, sizeof(number), NULL))
        d->rating = (float)atof(number);
    long ratings = TagNumber(xml, "numberOfRatings");
    d->ratings = ratings > 0 ? (unsigned long)ratings : 0;

    // The genres are the <live:name>s of the categories, less the two
    // that every game has.
    const char *p = xml;
    char name[64];
    while (TagText(p, "name", name, sizeof(name), &p))
    {
        if (strcmp(name, "Game Genres") != 0 && strcmp(name, "Xbox LIVE Games") != 0 &&
            strstr(d->genre, name) == NULL)
        {
            size_t used = strlen(d->genre);
            _snprintf(d->genre + used, sizeof(d->genre) - used, "%s%s", used > 0 ? ", " : "", name);
            d->genre[sizeof(d->genre) - 1] = '\0';
        }
    }

    char part[48], range[24];
    Range(TagNumber(xml, "offlinePlayersMin"), TagNumber(xml, "offlinePlayersMax"), range, sizeof(range));
    if (range[0] != '\0')
    {
        _snprintf(part, sizeof(part), "%s player%s", range, strcmp(range, "1") == 0 ? "" : "s");
        AppendPart(d->players, sizeof(d->players), part);
    }
    Range(TagNumber(xml, "onlineMultiplayerMin"), TagNumber(xml, "onlineMultiplayerMax"), range, sizeof(range));
    if (range[0] != '\0' && strcmp(range, "1") != 0)
    {
        _snprintf(part, sizeof(part), "online %s", range);
        AppendPart(d->players, sizeof(d->players), part);
    }
    Range(TagNumber(xml, "offlineSystemLinkMin"), TagNumber(xml, "offlineSystemLinkMax"), range, sizeof(range));
    if (range[0] != '\0' && strcmp(range, "1") != 0)
    {
        _snprintf(part, sizeof(part), "system link %s", range);
        AppendPart(d->players, sizeof(d->players), part);
    }

    // The screenshots, by the numbers in their file names, in order.
    for (const char *s = strstr(xml, "screenlg"); s != NULL && d->screenshotCount < STORE_MAX_SCREENSHOTS;
         s = strstr(s + 8, "screenlg"))
    {
        int n = atoi(s + 8);
        bool seen = (n <= 0);
        for (int i = 0; i < d->screenshotCount && !seen; ++i)
            seen = (d->screenshots[i] == n);
        if (!seen)
            d->screenshots[d->screenshotCount++] = n;
    }
    return true;
}

static StoreDetailsState LoadDetails(unsigned long titleId, StoreDetails *out)
{
    char path[64];
    _snprintf(path, sizeof(path), STORE_DIR "\\%08lX.cat.xml", titleId);
    path[sizeof(path) - 1] = '\0';

    // A cached entry is used for a week, then asked for again - ratings
    // drift, and an entry once missing may turn up.
    unsigned long size = 0;
    char *xml = NULL;
    char marker[64];
    _snprintf(marker, sizeof(marker), STORE_DIR "\\%08lX.cat.none", titleId);
    marker[sizeof(marker) - 1] = '\0';

    if (WrittenRecently(path))
        xml = (char *)ReadWholeFile(path, CATALOG_MAX_BYTES, &size);
    if (xml == NULL && WrittenRecently(marker))
        return STORE_DETAILS_NONE;

    if (xml == NULL)
    {
        char url[600];
        _snprintf(url, sizeof(url), CATALOG_URL, titleId);
        url[sizeof(url) - 1] = '\0';

        xml = (char *)malloc(CATALOG_MAX_BYTES + 1);
        if (xml == NULL)
            return STORE_DETAILS_NONE;

        unsigned long long len = 0;
        int status = HttpGetPlain(url, NULL, xml, CATALOG_MAX_BYTES, &len);
        if (status != 200 || len == 0)
        {
            dprintf("[store] %08lX catalog: HTTP %d\n", titleId, status);
            free(xml);

            // An old entry beats none; otherwise it's tried again the next
            // time the page opens.
            xml = (char *)ReadWholeFile(path, CATALOG_MAX_BYTES, &size);
            if (xml == NULL)
                return STORE_DETAILS_NONE;
        }
        else
        {
            xml[len] = '\0';
            size = (unsigned long)len;
            dprintf("[store] %08lX catalog: %lu bytes\n", titleId, size);
        }
    }

    bool found = ParseCatalog(xml, titleId, out);
    if (found)
    {
        WriteWholeFile(path, (unsigned char *)xml, size);
        DeleteFileA(marker);
    }
    else
    {
        WriteWholeFile(marker, NULL, 0);
    }
    free(xml);
    return found ? STORE_DETAILS_READY : STORE_DETAILS_NONE;
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static DWORD WINAPI StoreArtEntry(LPVOID)
{
    for (;;)
    {
        EnterCriticalSection(&g_lock);
        if (g_shutdown)
        {
            LeaveCriticalSection(&g_lock);
            break;
        }

        // Details first: the page waits on them for its screenshots.
        int detail = -1;
        for (int i = 0; i < MAX_DETAILS && detail < 0; ++i)
        {
            if (g_details[i].state == STORE_DETAILS_LOADING)
                detail = i;
        }
        if (detail >= 0)
        {
            const unsigned long titleId = g_details[detail].details.titleId;
            LeaveCriticalSection(&g_lock);

            StoreDetails *parsed = (StoreDetails *)malloc(sizeof(StoreDetails));
            StoreDetailsState state = STORE_DETAILS_NONE;
            if (parsed != NULL)
                state = LoadDetails(titleId, parsed);

            EnterCriticalSection(&g_lock);
            // Still the same game's slot - it may have been reused meanwhile.
            if (g_details[detail].details.titleId == titleId && g_details[detail].state == STORE_DETAILS_LOADING)
            {
                g_details[detail].state = state;
                if (state == STORE_DETAILS_READY)
                    g_details[detail].details = *parsed;
            }
            LeaveCriticalSection(&g_lock);
            free(parsed);
            continue;
        }

        int next = -1;
        if (g_readyCount < MAX_READY)
        {
            for (int i = 0; i < g_requestCount && next < 0; ++i)
            {
                if (!g_requests[i].done)
                    next = i;
            }
        }
        if (next < 0)
        {
            LeaveCriticalSection(&g_lock);
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }
        ArtRequest request = g_requests[next];
        g_requests[next].done = true;
        LeaveCriticalSection(&g_lock);

        unsigned long size = 0;
        unsigned char *bytes = LoadArt(request.titleId, request.kind, &size);
        if (bytes == NULL)
            continue;

        EnterCriticalSection(&g_lock);
        StoreArtData &data = g_ready[g_readyCount++];
        data.titleId = request.titleId;
        data.kind = request.kind;
        data.bytes = bytes;
        data.size = size;
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}

bool StartStoreArt()
{
    if (g_running)
        return true;

    InitializeCriticalSection(&g_lock);
    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (g_wake == NULL)
    {
        dprintf("[store] CreateEvent failed (%lu)\n", GetLastError());
        return false;
    }

    // The same stack as the other workers: a redirect to HTTPS goes through
    // the TLS client.
    g_thread = CreateThread(NULL, 256 * 1024, StoreArtEntry, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread == NULL)
    {
        dprintf("[store] CreateThread failed (%lu)\n", GetLastError());
        CloseHandle(g_wake);
        g_wake = NULL;
        return false;
    }

#ifdef _XBOX
    // Beside the cover worker: both are a moment's work between waits on
    // the network, and plain HTTP needs no TLS.
    XSetThreadProcessor(g_thread, 5);
#endif

    g_running = true;
    ResumeThread(g_thread);
    dprintf("[store] art worker started\n");
    return true;
}

bool StopStoreArt(DWORD timeoutMs)
{
    if (!g_running)
        return true;

    EnterCriticalSection(&g_lock);
    g_shutdown = true;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);

    if (WaitForSingleObject(g_thread, timeoutMs) != WAIT_OBJECT_0)
    {
        dprintf("[store] still running after %lums - leaving anyway\n", timeoutMs);
        return false;
    }

    CloseHandle(g_thread);
    g_thread = NULL;
    g_running = false;

    for (int i = 0; i < g_readyCount; ++i)
        free(g_ready[i].bytes);
    g_readyCount = 0;
    return true;
}

void RequestStoreArt(unsigned long titleId, StoreArtKind kind)
{
    if (!g_running || titleId == 0)
        return;

    EnterCriticalSection(&g_lock);
    int slot = -1;
    for (int i = 0; i < g_requestCount && slot < 0; ++i)
    {
        if (g_requests[i].titleId == titleId && g_requests[i].kind == kind)
            slot = i;
    }
    if (slot < 0 && g_requestCount < MAX_REQUESTS)
    {
        slot = g_requestCount++;
    }
    else if (slot < 0)
    {
        // Full: the first finished one makes room.
        for (int i = 0; i < g_requestCount && slot < 0; ++i)
        {
            if (g_requests[i].done)
                slot = i;
        }
    }
    if (slot >= 0)
    {
        g_requests[slot].titleId = titleId;
        g_requests[slot].kind = kind;
        g_requests[slot].done = false;
    }
    LeaveCriticalSection(&g_lock);

    if (slot >= 0)
        SetEvent(g_wake);
}

bool TakeStoreArt(StoreArtData *out)
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

void DiscardStoreArt(unsigned long titleId, StoreArtKind kind)
{
    // On the UI thread, but a delete is quick.
    char path[64];
    CachePath(titleId, kind, "jpg", path, sizeof(path));
    DeleteFileA(path);
}

void RequestStoreDetails(unsigned long titleId)
{
    if (!g_running || titleId == 0)
        return;

    EnterCriticalSection(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_DETAILS && slot < 0; ++i)
    {
        if (g_details[i].state != STORE_DETAILS_UNKNOWN && g_details[i].details.titleId == titleId)
            slot = i;
    }
    bool wake = false;
    if (slot >= 0)
    {
        // Known already. One that couldn't be had is tried again.
        if (g_details[slot].state == STORE_DETAILS_NONE)
        {
            g_details[slot].state = STORE_DETAILS_LOADING;
            wake = true;
        }
        g_details[slot].asked = GetTickCount();
    }
    else
    {
        slot = 0;
        for (int i = 0; i < MAX_DETAILS; ++i)
        {
            if (g_details[i].state == STORE_DETAILS_UNKNOWN)
            {
                slot = i;
                break;
            }
            if (g_details[i].asked < g_details[slot].asked)
                slot = i;
        }
        memset(&g_details[slot], 0, sizeof(g_details[slot]));
        g_details[slot].state = STORE_DETAILS_LOADING;
        g_details[slot].details.titleId = titleId;
        g_details[slot].asked = GetTickCount();
        wake = true;
    }
    LeaveCriticalSection(&g_lock);

    if (wake)
        SetEvent(g_wake);
}

StoreDetailsState GetStoreDetails(unsigned long titleId, StoreDetails *out)
{
    if (!g_running || titleId == 0)
        return STORE_DETAILS_UNKNOWN;

    StoreDetailsState state = STORE_DETAILS_UNKNOWN;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_DETAILS; ++i)
    {
        if (g_details[i].state != STORE_DETAILS_UNKNOWN && g_details[i].details.titleId == titleId)
        {
            state = g_details[i].state;
            if (state == STORE_DETAILS_READY && out != NULL)
                *out = g_details[i].details;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return state;
}

// ---------------------------------------------------------------------------
// A game's icon, for the packages GodConvert writes
// ---------------------------------------------------------------------------

#define TITLE_ICON_URL "http://image.xboxlive.com/global/t.%08lx/icon/0/8000"

bool FetchTitleIcon(unsigned long titleId, unsigned char *out, unsigned long capacity, unsigned long *outSize)
{
    if (titleId == 0 || out == NULL || capacity < 8 || outSize == NULL)
        return false;
    *outSize = 0;

    char url[96];
    _snprintf(url, sizeof(url), TITLE_ICON_URL, titleId);
    url[sizeof(url) - 1] = '\0';

    // HttpGetPlain wants room for a terminator past the capacity.
    char *buffer = (char *)malloc(capacity + 1);
    if (buffer == NULL)
        return false;

    unsigned long long len = 0;
    const int status = HttpGetPlain(url, NULL, buffer, capacity, &len);
    static const unsigned char kPng[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    const bool ok = (status == 200 && len >= 8 && len <= capacity && memcmp(buffer, kPng, 8) == 0);
    if (ok)
    {
        memcpy(out, buffer, (size_t)len);
        *outSize = (unsigned long)len;
    }
    else
    {
        dprintf("[store] %08lX: no icon from Xbox Live (HTTP %d, %I64u bytes)\n", titleId, status, len);
    }
    free(buffer);
    return ok;
}
