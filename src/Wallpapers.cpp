/*
FILE : Wallpapers.cpp
PROJECT : Omni360
DESCRIPTION : Home wallpapers from xbox.com - the list, the previews and the
              one picked - fetched, cached and decoded on a worker thread.
              See Wallpapers.h.
*/

#include "Wallpapers.h"
#include "downloadFile.h" // httpRequestHTTPS
#include "OutputConsole.h"
#include "ImageDecode.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define WALLPAPER_DIR      "game:\\Wallpapers"
#define LIST_URL           "https://www.xbox.com/en-US/wallpapers/JS/wallpapers-Sheet.js"
#define ASSET_URL          "https://assets.xboxservices.com/assets%s"
#define LIST_FILE          WALLPAPER_DIR "\\list.js"
#define HOME_JPG           WALLPAPER_DIR "\\home.jpg"
#define HOME_PNG           WALLPAPER_DIR "\\home.png"
#define HOME_TITLE_FILE    WALLPAPER_DIR "\\home.txt"

#define LIST_MAX_BYTES     (2 * 1024 * 1024) // about 500KB today
#define PREVIEW_MAX_BYTES  (1024 * 1024)     // a JPEG preview is under 200KB, a PNG one 90KB
#define FULL_MAX_BYTES     (8 * 1024 * 1024) // a 1920x1080 PNG is about 3.6MB
#define LIST_FRESH_DAYS    7
#define MAX_ENTRIES        1024
#define MAX_REQUESTS       48
#define MAX_READY          4                 // a preview is 0.5MB of pixels, Home's 3.5MB
#define HOME_W             1280.0f           // Home's screen; the saved one is scaled to cover it
#define HOME_H             720.0f

static CRITICAL_SECTION g_lock;
static HANDLE g_wake = NULL;
static HANDLE g_thread = NULL;
static bool g_running = false;
static bool g_shutdown = false;

static WallpaperEntry *g_entries = NULL; // MAX_ENTRIES, once the list is in
static int g_entryCount = 0;
static WallpaperListState g_listState = WALLPAPER_LIST_NONE;

// Previews asked for, the newest last; the worker takes from the end.
static int g_requests[MAX_REQUESTS];
static int g_requestCount = 0;
static int g_working = -2; // the preview being fetched, or -2 for none

static int g_pickIndex = -1; // asked to be picked, not yet started
static WallpaperPickState g_pickState = WALLPAPER_PICK_IDLE;
static bool g_homeWanted = false;

static WallpaperArt g_ready[MAX_READY];
static int g_readyCount = 0;

// httpRequestHTTPS's chatter, down to its errors - as StoreArt.cpp does.
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
    CreateDirectoryA(WALLPAPER_DIR, NULL); // fails harmlessly once it exists
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

// True while a file is younger than days.
static bool WrittenWithin(const char *path, int days)
{
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
    return now.QuadPart < written.QuadPart + (unsigned long long)days * 24 * 60 * 60 * 10000000ULL;
}

static bool FileExists(const char *path)
{
    return GetFileAttributesA(path) != (DWORD)-1;
}

// One image from assets.xboxservices.com, malloc'd, or NULL.
static unsigned char *FetchAsset(const char *path, unsigned long maxBytes, unsigned long *outSize)
{
    char url[256];
    _snprintf(url, sizeof(url), ASSET_URL, path);
    url[sizeof(url) - 1] = '\0';

    char *bytes = (char *)malloc(maxBytes + 1);
    if (bytes == NULL)
        return NULL;
    unsigned long long len = maxBytes;
    const int status = httpRequestHTTPS(url, HTTP_GET, NULL, NULL, "", bytes, &len, false, NULL, 0, QuietPrint);
    if (status != 200 || len < 64)
    {
        dprintf("[wallpapers] %s -> HTTP %d, %I64u bytes\n", path, status, len);
        free(bytes);
        return NULL;
    }
    *outSize = (unsigned long)len;
    return (unsigned char *)bytes;
}

// The cache's name for an image: its file name in the path - a GUID and its
// extension, so unique - under WALLPAPER_DIR.
static void CachePathFor(const char *path, char *out, size_t outSize)
{
    const char *slash = strrchr(path, '/');
    _snprintf(out, outSize, WALLPAPER_DIR "\\%s", slash != NULL ? slash + 1 : path);
    out[outSize - 1] = '\0';
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

// "games", "Devices", "devices, games", "25th Anniversary" - the page's
// categories are loose. The first word it knows decides.
static int CategoryOf(const char *text)
{
    char lower[64];
    size_t i = 0;
    for (; text[i] != '\0' && i < sizeof(lower) - 1; ++i)
        lower[i] = (char)tolower((unsigned char)text[i]);
    lower[i] = '\0';

    const char *best = NULL;
    int category = WALLPAPER_COMMUNITY;
    const char *words[] = { "game", "communit", "device", "event" };
    const int ids[] = { WALLPAPER_GAMES, WALLPAPER_COMMUNITY, WALLPAPER_DEVICES, WALLPAPER_EVENTS };
    for (int w = 0; w < 4; ++w)
    {
        const char *at = strstr(lower, words[w]);
        if (at != NULL && (best == NULL || at < best))
        {
            best = at;
            category = ids[w];
        }
    }
    return category;
}

// A path as the list gives it, without its "?n=name.jpg".
static void CopyPath(const char *from, char *out, size_t outSize)
{
    size_t i = 0;
    for (; from[i] != '\0' && from[i] != '?' && i < outSize - 1; ++i)
        out[i] = from[i];
    out[i] = '\0';
}

// The script's array into entries. False if it isn't one.
static bool ParseList(char *script)
{
    char *open = strchr(script, '[');
    char *close = strrchr(script, ']');
    if (open == NULL || close == NULL || close < open)
        return false;
    close[1] = '\0';

    cJSON *root = cJSON_Parse(open);
    if (root == NULL || !cJSON_IsArray(root))
    {
        if (root != NULL)
            cJSON_Delete(root);
        return false;
    }

    WallpaperEntry *entries = (WallpaperEntry *)malloc(sizeof(WallpaperEntry) * MAX_ENTRIES);
    if (entries == NULL)
    {
        cJSON_Delete(root);
        return false;
    }

    int count = 0;
    const int size = cJSON_GetArraySize(root);
    for (int i = 0; i < size && count < MAX_ENTRIES; ++i)
    {
        cJSON *item = cJSON_GetArrayItem(root, i);
        cJSON *title = cJSON_GetObjectItem(item, "title");
        cJSON *category = cJSON_GetObjectItem(item, "category");
        cJSON *preview = cJSON_GetObjectItem(item, "preview");
        cJSON *full = cJSON_GetObjectItem(item, "console");
        if (!cJSON_IsString(title) || !cJSON_IsString(preview) || !cJSON_IsString(full) ||
            preview->valuestring[0] != '/' || full->valuestring[0] != '/')
            continue;

        WallpaperEntry &e = entries[count];
        _snprintf(e.title, sizeof(e.title), "%s", title->valuestring);
        e.title[sizeof(e.title) - 1] = '\0';
        // Some titles end in a stray space.
        for (size_t n = strlen(e.title); n > 0 && e.title[n - 1] == ' '; --n)
            e.title[n - 1] = '\0';
        e.category = cJSON_IsString(category) ? CategoryOf(category->valuestring) : WALLPAPER_COMMUNITY;
        CopyPath(preview->valuestring, e.preview, sizeof(e.preview));
        CopyPath(full->valuestring, e.full, sizeof(e.full));
        count++;
    }
    cJSON_Delete(root);

    if (count == 0)
    {
        free(entries);
        return false;
    }

    EnterCriticalSection(&g_lock);
    g_entries = entries;
    g_entryCount = count;
    LeaveCriticalSection(&g_lock);
    dprintf("[wallpapers] %d in the list\n", count);
    return true;
}

// From the cache while it's under a week old, else xbox.com - else the cache
// however old, rather than nothing.
static bool LoadList()
{
    unsigned long size = 0;
    unsigned char *bytes = NULL;
    if (WrittenWithin(LIST_FILE, LIST_FRESH_DAYS))
        bytes = ReadWholeFile(LIST_FILE, LIST_MAX_BYTES, &size);

    if (bytes == NULL)
    {
        char *fetched = (char *)malloc(LIST_MAX_BYTES + 1);
        if (fetched != NULL)
        {
            unsigned long long len = LIST_MAX_BYTES;
            const int status = httpRequestHTTPS(LIST_URL, HTTP_GET, NULL, NULL, "", fetched, &len, false, NULL, 0,
                                                QuietPrint);
            dprintf("[wallpapers] list -> HTTP %d, %I64u bytes\n", status, len);
            if (status == 200 && len > 1024)
            {
                fetched[len] = '\0';
                if (!WriteWholeFile(LIST_FILE, (unsigned char *)fetched, (unsigned long)len))
                    dprintf("[wallpapers] couldn't write %s\n", LIST_FILE);
                bytes = (unsigned char *)fetched;
            }
            else
                free(fetched);
        }
    }

    if (bytes == NULL)
        bytes = ReadWholeFile(LIST_FILE, LIST_MAX_BYTES, &size);
    if (bytes == NULL)
        return false;

    const bool ok = ParseList((char *)bytes);
    free(bytes);
    return ok;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

static void Ready(int index, unsigned long *pixels, int w, int h)
{
    EnterCriticalSection(&g_lock);
    WallpaperArt &art = g_ready[g_readyCount++];
    art.index = index;
    art.pixels = pixels;
    art.width = w;
    art.height = h;
    LeaveCriticalSection(&g_lock);
}

// A preview, from the cache or xbox.com, halved.
static void LoadPreview(int index)
{
    const WallpaperEntry &e = g_entries[index];
    char path[96];
    CachePathFor(e.preview, path, sizeof(path));

    unsigned long size = 0;
    unsigned char *bytes = ReadWholeFile(path, PREVIEW_MAX_BYTES, &size);
    if (bytes == NULL)
    {
        bytes = FetchAsset(e.preview, PREVIEW_MAX_BYTES, &size);
        if (bytes == NULL)
            return;
        if (!WriteWholeFile(path, bytes, size))
            dprintf("[wallpapers] couldn't write %s\n", path);
    }

    int w = 0, h = 0;
    unsigned long *pixels = DecodeImageToArgb(bytes, size, &w, &h);
    free(bytes);
    if (pixels == NULL || w < 16 || h < 16)
    {
        dprintf("[wallpapers] preview %d wouldn't decode (%s)\n", index, ImageDecodeError());
        if (pixels != NULL)
            free(pixels);
        DeleteFileA(path);
        return;
    }
    unsigned long *half = HalveArgb(pixels, w, h);
    free(pixels);
    if (half != NULL)
        Ready(index, half, w / 2, h / 2);
}

// The saved one, scaled to just cover Home's 1280x720.
static void LoadHome()
{
    unsigned long size = 0;
    unsigned char *bytes = ReadWholeFile(HOME_JPG, FULL_MAX_BYTES, &size);
    if (bytes == NULL)
        bytes = ReadWholeFile(HOME_PNG, FULL_MAX_BYTES, &size);
    if (bytes == NULL)
        return;

    int w = 0, h = 0;
    unsigned long *pixels = DecodeImageToArgb(bytes, size, &w, &h);
    free(bytes);
    if (pixels == NULL || w < 16 || h < 16)
    {
        dprintf("[wallpapers] Home's wallpaper wouldn't decode (%s)\n", ImageDecodeError());
        if (pixels != NULL)
            free(pixels);
        return;
    }

    const float scale = max(HOME_W / (float)w, HOME_H / (float)h);
    if (scale < 1.0f)
    {
        const int sw = max(1, (int)(w * scale + 0.5f)), sh = max(1, (int)(h * scale + 0.5f));
        unsigned long *scaled = (unsigned long *)malloc((size_t)sw * sh * sizeof(unsigned long));
        if (scaled != NULL && ScaleArgb(pixels, w, h, 0.0f, 0.0f, (float)w, (float)h, sw, sh, scaled))
        {
            free(pixels);
            pixels = scaled;
            w = sw;
            h = sh;
        }
        else if (scaled != NULL)
            free(scaled);
    }
    Ready(WALLPAPER_HOME, pixels, w, h);
}

// Downloads one at full size and makes it Home's. False if it couldn't be
// had or wouldn't decode - the old one stays then.
static bool Pick(int index)
{
    const WallpaperEntry &e = g_entries[index];
    unsigned long size = 0;
    unsigned char *bytes = FetchAsset(e.full, FULL_MAX_BYTES, &size);
    if (bytes == NULL)
        return false;

    // Checked before it replaces anything.
    int w = 0, h = 0;
    unsigned long *pixels = DecodeImageToArgb(bytes, size, &w, &h);
    if (pixels == NULL)
    {
        dprintf("[wallpapers] %s wouldn't decode (%s)\n", e.full, ImageDecodeError());
        free(bytes);
        return false;
    }
    free(pixels);

    const size_t len = strlen(e.full);
    const bool png = len > 4 && _stricmp(e.full + len - 4, ".png") == 0;
    const char *path = png ? HOME_PNG : HOME_JPG;
    const bool ok = WriteWholeFile(path, bytes, size);
    free(bytes);
    if (!ok)
    {
        dprintf("[wallpapers] couldn't write %s\n", path);
        return false;
    }
    DeleteFileA(png ? HOME_JPG : HOME_PNG);
    WriteWholeFile(HOME_TITLE_FILE, (const unsigned char *)e.title, (unsigned long)strlen(e.title));
    dprintf("[wallpapers] Home's wallpaper is now \"%s\" (%lu KB)\n", e.title, size / 1024);
    return true;
}

static DWORD WINAPI WallpapersEntry(LPVOID)
{
    for (;;)
    {
        EnterCriticalSection(&g_lock);
        if (g_shutdown)
        {
            LeaveCriticalSection(&g_lock);
            break;
        }

        // A pick first: someone's waiting on it with a progress dialog up.
        if (g_pickIndex >= 0)
        {
            const int index = g_pickIndex;
            g_pickIndex = -1;
            LeaveCriticalSection(&g_lock);
            const bool ok = Pick(index);
            EnterCriticalSection(&g_lock);
            g_pickState = ok ? WALLPAPER_PICK_DONE : WALLPAPER_PICK_FAILED;
            if (ok)
                g_homeWanted = true;
            LeaveCriticalSection(&g_lock);
            continue;
        }

        if (g_homeWanted && g_readyCount < MAX_READY)
        {
            g_homeWanted = false;
            LeaveCriticalSection(&g_lock);
            LoadHome();
            continue;
        }

        if (g_listState == WALLPAPER_LIST_LOADING)
        {
            LeaveCriticalSection(&g_lock);
            const bool ok = LoadList();
            EnterCriticalSection(&g_lock);
            g_listState = ok ? WALLPAPER_LIST_READY : WALLPAPER_LIST_FAILED;
            LeaveCriticalSection(&g_lock);
            continue;
        }

        if (g_listState == WALLPAPER_LIST_READY && g_requestCount > 0 && g_readyCount < MAX_READY)
        {
            const int index = g_requests[--g_requestCount];
            g_working = index;
            LeaveCriticalSection(&g_lock);
            LoadPreview(index);
            EnterCriticalSection(&g_lock);
            g_working = -2;
            LeaveCriticalSection(&g_lock);
            continue;
        }

        LeaveCriticalSection(&g_lock);
        WaitForSingleObject(g_wake, INFINITE);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// The UI's side
// ---------------------------------------------------------------------------

bool StartWallpapers()
{
    if (g_running)
        return true;

    InitializeCriticalSection(&g_lock);
    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (g_wake == NULL)
    {
        dprintf("[wallpapers] CreateEvent failed (%lu)\n", GetLastError());
        return false;
    }

    // The TLS client needs the stack the other HTTPS workers have.
    g_thread = CreateThread(NULL, 256 * 1024, WallpapersEntry, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread == NULL)
    {
        dprintf("[wallpapers] CreateThread failed (%lu)\n", GetLastError());
        CloseHandle(g_wake);
        g_wake = NULL;
        return false;
    }
#ifdef _XBOX
    XSetThreadProcessor(g_thread, 5);
#endif
    g_running = true;
    ResumeThread(g_thread);
    return true;
}

bool StopWallpapers(DWORD timeoutMs)
{
    if (!g_running)
        return true;

    EnterCriticalSection(&g_lock);
    g_shutdown = true;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);

    if (WaitForSingleObject(g_thread, timeoutMs) != WAIT_OBJECT_0)
    {
        dprintf("[wallpapers] still running after %lums - leaving anyway\n", timeoutMs);
        return false;
    }
    CloseHandle(g_thread);
    g_thread = NULL;
    g_running = false;

    for (int i = 0; i < g_readyCount; ++i)
        free(g_ready[i].pixels);
    g_readyCount = 0;
    return true;
}

void RequestWallpaperList()
{
    if (!g_running)
        return;
    EnterCriticalSection(&g_lock);
    const bool start = (g_listState == WALLPAPER_LIST_NONE || g_listState == WALLPAPER_LIST_FAILED);
    if (start)
        g_listState = WALLPAPER_LIST_LOADING;
    LeaveCriticalSection(&g_lock);
    if (start)
        SetEvent(g_wake);
}

WallpaperListState WallpaperListStatus()
{
    if (!g_running)
        return WALLPAPER_LIST_FAILED;
    EnterCriticalSection(&g_lock);
    const WallpaperListState state = g_listState;
    LeaveCriticalSection(&g_lock);
    return state;
}

int WallpaperCount()
{
    return WallpaperListStatus() == WALLPAPER_LIST_READY ? g_entryCount : 0;
}

const WallpaperEntry *WallpaperAt(int index)
{
    if (index < 0 || index >= WallpaperCount())
        return NULL;
    return &g_entries[index];
}

void RequestWallpaperPreview(int index)
{
    if (!g_running || index < 0 || index >= WallpaperCount())
        return;
    EnterCriticalSection(&g_lock);
    // Moved to the end if it's there already: the newest goes first.
    int at = -1;
    for (int i = 0; i < g_requestCount && at < 0; ++i)
        if (g_requests[i] == index)
            at = i;
    if (at >= 0)
    {
        for (int i = at; i + 1 < g_requestCount; ++i)
            g_requests[i] = g_requests[i + 1];
        g_requestCount--;
    }
    else if (g_requestCount == MAX_REQUESTS)
    {
        // Full: the oldest - long scrolled past - goes.
        for (int i = 0; i + 1 < g_requestCount; ++i)
            g_requests[i] = g_requests[i + 1];
        g_requestCount--;
    }
    g_requests[g_requestCount++] = index;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

bool WallpaperPreviewPending(int index)
{
    if (!g_running)
        return false;
    EnterCriticalSection(&g_lock);
    bool pending = (g_working == index);
    for (int i = 0; i < g_requestCount && !pending; ++i)
        pending = (g_requests[i] == index);
    for (int i = 0; i < g_readyCount && !pending; ++i)
        pending = (g_ready[i].index == index);
    LeaveCriticalSection(&g_lock);
    return pending;
}

void RequestWallpaperPick(int index)
{
    if (!g_running || index < 0 || index >= WallpaperCount())
        return;
    EnterCriticalSection(&g_lock);
    g_pickIndex = index;
    g_pickState = WALLPAPER_PICK_WORKING;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

WallpaperPickState WallpaperPickStatus()
{
    if (!g_running)
        return WALLPAPER_PICK_FAILED;
    EnterCriticalSection(&g_lock);
    const WallpaperPickState state = g_pickState;
    LeaveCriticalSection(&g_lock);
    return state;
}

void ResetWallpaperPick()
{
    if (!g_running)
        return;
    EnterCriticalSection(&g_lock);
    if (g_pickState != WALLPAPER_PICK_WORKING)
        g_pickState = WALLPAPER_PICK_IDLE;
    LeaveCriticalSection(&g_lock);
}

void RequestHomeWallpaper()
{
    if (!g_running || !HasHomeWallpaper())
        return;
    EnterCriticalSection(&g_lock);
    g_homeWanted = true;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

bool HasHomeWallpaper()
{
    return FileExists(HOME_JPG) || FileExists(HOME_PNG);
}

void HomeWallpaperTitle(char *out, size_t outSize)
{
    out[0] = '\0';
    unsigned long size = 0;
    unsigned char *bytes = ReadWholeFile(HOME_TITLE_FILE, 1024, &size);
    if (bytes == NULL)
        return;
    _snprintf(out, outSize, "%s", (const char *)bytes);
    out[outSize - 1] = '\0';
    free(bytes);
}

bool TakeWallpaperArt(WallpaperArt *out)
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
