/*
FILE : Featured.cpp
PROJECT : Omni360
DESCRIPTION : The Store's featured games, from featured.json in the
              repository. See Featured.h.
*/

#include "Featured.h"
#include "StoreCatalog.h" // StoreGameByTitleId: only games the Store has
#include "downloadFile.h" // HttpsSession
#include "OutputConsole.h"
#include "cJSON.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// The file as it is on the default branch, raw. api.github.com rather than
// raw.githubusercontent.com: it's the host the update check already trusts
// (USERTrust ECC, in TrustAnchors.h).
#define FEATURED_URL    "https://api.github.com/repos/JustGillin/omni360/contents/featured.json"
#define FEATURED_ACCEPT "Accept: application/vnd.github.raw+json\r\n"
#define FEATURED_CACHE  "game:\\Store\\featured.json"
#define FEATURED_MAX    (64 * 1024)

// Until a file says otherwise.
static const unsigned long kBuiltIn[] = { 0x4D5307E6, 0x545407D8, 0x4D530AA4 }; // Halo 3, BioShock, Forza Horizon 2

static CRITICAL_SECTION g_lock;
static bool g_lockReady = false;
static FeaturedSet g_set;
static unsigned long g_changes = 0;

// ---------------------------------------------------------------------------
// Dates
// ---------------------------------------------------------------------------

// Days since 1970-01-01 (Howard Hinnant's days_from_civil).
static long DaysFromCivil(int y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

struct Today
{
    bool known;
    int y, m, d;
};

// An HTTP date: "Mon, 05 Oct 2026 15:20:00 GMT".
static bool ParseHttpDate(const char *text, Today *out)
{
    static const char *const kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    const char *comma = strchr(text, ',');
    if (comma == NULL)
        return false;
    int d = 0, y = 0;
    char month[4] = "";
    if (sscanf(comma + 1, " %d %3s %d", &d, month, &y) != 3)
        return false;
    for (int m = 0; m < 12; ++m)
    {
        if (_stricmp(month, kMonths[m]) == 0 && d >= 1 && d <= 31 && y >= 2020)
        {
            out->known = true;
            out->y = y;
            out->m = m + 1;
            out->d = d;
            return true;
        }
    }
    return false;
}

// The console's own clock - unknown if it's plainly never been set.
static Today ConsoleToday()
{
    SYSTEMTIME now;
    GetSystemTime(&now);
    Today t;
    t.known = (now.wYear >= 2025);
    t.y = now.wYear;
    t.m = now.wMonth;
    t.d = now.wDay;
    return t;
}

// "2026-10-05" as 20261005; 0 if it isn't one.
static long DateNumber(const char *text)
{
    int y = 0, m = 0, d = 0;
    if (text == NULL || sscanf(text, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31)
        return 0;
    return y * 10000L + m * 100L + d;
}

// ---------------------------------------------------------------------------
// Choosing
// ---------------------------------------------------------------------------

// One to five title IDs from a JSON array of hex strings - false unless
// every one is a game the Store has. Past five are left off.
static bool ReadGames(const cJSON *games, FeaturedSet *out)
{
    const int count = cJSON_IsArray(games) ? cJSON_GetArraySize(games) : 0;
    if (count < 1)
        return false;
    out->count = count < FEATURED_MAX_GAMES ? count : FEATURED_MAX_GAMES;
    unsigned long *ids = out->titleIds;
    for (int i = 0; i < out->count; ++i)
    {
        const cJSON *id = cJSON_GetArrayItem(games, i);
        if (!cJSON_IsString(id) || id->valuestring == NULL)
            return false;
        char *end = NULL;
        ids[i] = strtoul(id->valuestring, &end, 16);
        StoreGame game;
        if (end == NULL || *end != '\0' || ids[i] == 0 || !StoreGameByTitleId(ids[i], &game))
        {
            dprintf("[featured] %s isn't a game the Store has - set passed over\n", id->valuestring);
            return false;
        }
    }
    return true;
}

// Today's set from the file's text: an event running today, else this week's
// in the rotation. False if the file gives none that can be used.
static bool Choose(const char *text, size_t len, const Today &today, FeaturedSet *out)
{
    cJSON *root = cJSON_ParseWithLength(text, len);
    if (root == NULL)
    {
        dprintf("[featured] the file isn't JSON\n");
        return false;
    }

    bool chosen = false;
    if (today.known)
    {
        const long now = today.y * 10000L + today.m * 100L + today.d;
        const cJSON *event = NULL;
        cJSON_ArrayForEach(event, cJSON_GetObjectItemCaseSensitive(root, "events"))
        {
            const cJSON *from = cJSON_GetObjectItemCaseSensitive(event, "from");
            const cJSON *to = cJSON_GetObjectItemCaseSensitive(event, "to");
            const long first = DateNumber(cJSON_IsString(from) ? from->valuestring : NULL);
            const long last = DateNumber(cJSON_IsString(to) ? to->valuestring : NULL);
            if (first == 0 || last == 0 || now < first || now > last)
                continue;
            if (!ReadGames(cJSON_GetObjectItemCaseSensitive(event, "games"), out))
                continue;
            const cJSON *label = cJSON_GetObjectItemCaseSensitive(event, "label");
            _snprintf(out->label, sizeof(out->label), "%s",
                      cJSON_IsString(label) && label->valuestring[0] != '\0' ? label->valuestring : "Featured");
            out->label[sizeof(out->label) - 1] = '\0';
            dprintf("[featured] event \"%s\" (%s to %s)\n", out->label, from->valuestring, to->valuestring);
            chosen = true;
            break;
        }
    }

    const cJSON *rotation = cJSON_GetObjectItemCaseSensitive(root, "rotation");
    const int sets = cJSON_IsArray(rotation) ? cJSON_GetArraySize(rotation) : 0;
    if (!chosen && sets > 0)
    {
        // Weeks from Monday 1970-01-05, so it turns over each Monday; the
        // first set while the date isn't known. A set that won't do gives
        // way to the next.
        const long week = today.known ? (DaysFromCivil(today.y, today.m, today.d) - 4) / 7 : 0;
        for (int tried = 0; tried < sets && !chosen; ++tried)
        {
            const int pick = (int)((week + tried) % sets);
            if (ReadGames(cJSON_GetArrayItem(rotation, pick), out))
            {
                _snprintf(out->label, sizeof(out->label), "Featured");
                dprintf("[featured] week %ld: set %d of %d\n", week, pick + 1, sets);
                chosen = true;
            }
        }
    }

    cJSON_Delete(root);
    return chosen;
}

static void Publish(const FeaturedSet &set)
{
    EnterCriticalSection(&g_lock);
    const bool changed = memcmp(&g_set, &set, sizeof(set)) != 0;
    g_set = set;
    if (changed)
        g_changes++;
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------

// The HTTP client's chatter, failures only.
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

static DWORD WINAPI FetchEntry(LPVOID)
{
    char *reply = (char *)malloc(FEATURED_MAX + 1);
    HttpsSession *session = HttpsSessionOpen(QuietPrint);
    if (reply == NULL || session == NULL)
    {
        free(reply);
        HttpsSessionClose(session);
        return 0;
    }

    unsigned long long len = FEATURED_MAX;
    const int status = HttpsSessionGet(session, FEATURED_URL, FEATURED_ACCEPT, reply, &len);
    char date[64] = "";
    HttpsSessionServerDate(session, date, sizeof(date));
    HttpsSessionClose(session);

    Today today;
    memset(&today, 0, sizeof(today));
    if (!ParseHttpDate(date, &today))
        today = ConsoleToday();

    FeaturedSet set;
    memset(&set, 0, sizeof(set));
    if (status == 200 && len <= FEATURED_MAX)
    {
        reply[len] = '\0';
        if (Choose(reply, (size_t)len, today, &set))
        {
            Publish(set);
            // Kept for next time, and for offline.
            FILE *out = fopen(FEATURED_CACHE, "wb");
            if (out != NULL)
            {
                fwrite(reply, 1, (size_t)len, out);
                fclose(out);
            }
        }
    }
    else
    {
        dprintf("[featured] couldn't fetch featured.json: HTTP %d - keeping what's there\n", status);
    }
    free(reply);
    return 0;
}

void StartFeatured()
{
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }

    // The built-in three, then the kept file's choice by the console's
    // clock, then - shortly - the fetched file's by GitHub's.
    FeaturedSet set;
    memset(&set, 0, sizeof(set));
    memcpy(set.titleIds, kBuiltIn, sizeof(kBuiltIn));
    set.count = sizeof(kBuiltIn) / sizeof(kBuiltIn[0]);
    _snprintf(set.label, sizeof(set.label), "Featured");

    FILE *in = fopen(FEATURED_CACHE, "rb");
    if (in != NULL)
    {
        static char kept[FEATURED_MAX + 1];
        const size_t len = fread(kept, 1, FEATURED_MAX, in);
        fclose(in);
        kept[len] = '\0';
        FeaturedSet fromFile;
        memset(&fromFile, 0, sizeof(fromFile));
        if (len > 0 && Choose(kept, len, ConsoleToday(), &fromFile))
            set = fromFile;
    }
    Publish(set);

    HANDLE thread = CreateThread(NULL, 256 * 1024, FetchEntry, NULL, CREATE_SUSPENDED, NULL);
    if (thread == NULL)
        return;
#ifdef _XBOX
    XSetThreadProcessor(thread, 5); // with the update check and the art workers: one request
#endif
    ResumeThread(thread);
    CloseHandle(thread);
}

void GetFeatured(FeaturedSet *out, unsigned long *changeCount)
{
    if (!g_lockReady)
    {
        memset(out, 0, sizeof(*out));
        memcpy(out->titleIds, kBuiltIn, sizeof(kBuiltIn));
        out->count = sizeof(kBuiltIn) / sizeof(kBuiltIn[0]);
        _snprintf(out->label, sizeof(out->label), "Featured");
        if (changeCount != NULL)
            *changeCount = 0;
        return;
    }
    EnterCriticalSection(&g_lock);
    *out = g_set;
    if (changeCount != NULL)
        *changeCount = g_changes;
    LeaveCriticalSection(&g_lock);
}
