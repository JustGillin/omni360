/*
FILE : UpdateCheck.cpp
PROJECT : Omni360
DESCRIPTION : Asks GitHub whether there's a newer release. See UpdateCheck.h.
*/

#include "UpdateCheck.h"
#include "settings.h"     // CURRENT_VERSION
#include "downloadFile.h" // httpRequestHTTPS
#include "OutputConsole.h"
#include "cJSON.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// The newest twenty, newest first - plenty to find the latest that applies.
// api.github.com chains to USERTrust ECC, in TrustAnchors.h for this.
#define RELEASES_URL "https://api.github.com/repos/JustGillin/omni360/releases?per_page=20"
#define REPLY_MAX    (256 * 1024)

static CRITICAL_SECTION g_lock;
static bool g_lockReady = false;
static HANDLE g_thread = NULL;
static UpdateState g_state = UPDATE_NOT_CHECKED;
static UpdateInfo g_info;
static unsigned long g_changes = 0;

// Under the lock.
static void SetState(UpdateState state)
{
    g_state = state;
    g_changes++;
}

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

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

// "v0.2.0-beta" into its three numbers and what follows the '-'.
static void SplitVersion(const char *v, unsigned long nums[3], const char **suffix)
{
    nums[0] = nums[1] = nums[2] = 0;
    *suffix = "";
    if (v == NULL)
        return;
    if (*v == 'v' || *v == 'V')
        v++;

    for (int i = 0; i < 3; ++i)
    {
        char *end = NULL;
        nums[i] = strtoul(v, &end, 10);
        v = end;
        if (*v != '.')
            break;
        v++;
    }
    while (*v != '\0' && *v != '-' && *v != '+')
        v++; // anything else odd is ignored
    if (*v == '-')
        *suffix = v + 1;
}

static bool IsDigits(const char *s, size_t n)
{
    if (n == 0)
        return false;
    for (size_t i = 0; i < n; ++i)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

// "beta.2" against "beta.10": part by part, numbers as numbers and below
// words, and fewer parts below more when the rest are equal.
static int CompareSuffixes(const char *a, const char *b)
{
    for (;;)
    {
        if (*a == '\0' || *a == '+')
            return (*b == '\0' || *b == '+') ? 0 : -1;
        if (*b == '\0' || *b == '+')
            return 1;

        size_t na = strcspn(a, ".+"), nb = strcspn(b, ".+");
        const bool da = IsDigits(a, na), db = IsDigits(b, nb);
        int c;
        if (da && db)
        {
            unsigned long x = strtoul(a, NULL, 10), y = strtoul(b, NULL, 10);
            c = (x < y) ? -1 : (x > y ? 1 : 0);
        }
        else if (da != db)
        {
            c = da ? -1 : 1;
        }
        else
        {
            c = strncmp(a, b, na < nb ? na : nb);
            if (c == 0)
                c = (na < nb) ? -1 : (na > nb ? 1 : 0);
        }
        if (c != 0)
            return c;

        a += na;
        b += nb;
        if (*a == '.')
            a++;
        if (*b == '.')
            b++;
    }
}

int CompareVersions(const char *a, const char *b)
{
    unsigned long na[3], nb[3];
    const char *sa, *sb;
    SplitVersion(a, na, &sa);
    SplitVersion(b, nb, &sb);

    for (int i = 0; i < 3; ++i)
    {
        if (na[i] != nb[i])
            return na[i] < nb[i] ? -1 : 1;
    }
    // A release is newer than its own pre-releases.
    const bool pa = (*sa != '\0'), pb = (*sb != '\0');
    if (pa != pb)
        return pa ? -1 : 1;
    return CompareSuffixes(sa, sb);
}

// ---------------------------------------------------------------------------
// Release notes
// ---------------------------------------------------------------------------

// GitHub's Markdown as plain lines: no headings' #s, emphasis or code marks,
// links as their text, list items as bullets, and at most one blank line in
// a row. A paragraph's lines are joined back up, as Markdown reads a lone
// line break as a space - the console wraps them to its own width.
static void PlainNotes(const char *in, char *out, size_t outSize)
{
    size_t o = 0;
    int blanks = 0;
    bool afterText = false; // the last line written was paragraph or list text
    out[0] = '\0';

    while (in != NULL && *in != '\0' && o + 8 < outSize)
    {
        // One line, without its '\r'.
        const char *end = strchr(in, '\n');
        const size_t len = end != NULL ? (size_t)(end - in) : strlen(in);
        char line[1024];
        size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;
        memcpy(line, in, n);
        line[n] = '\0';
        in = end != NULL ? end + 1 : in + len;
        while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = '\0';

        const char *p = line;
        while (*p == ' ')
            p++;
        if (*p == '\0')
        {
            if (o > 0 && blanks++ == 0)
                out[o++] = '\n';
            afterText = false;
            continue;
        }
        blanks = 0;

        bool heading = false;
        if (*p == '#')
        {
            while (*p == '#')
                p++;
            while (*p == ' ')
                p++;
            heading = true;
        }
        else if ((p[0] == '-' || p[0] == '*') && p[1] == ' ')
        {
            static const char kBullet[] = "\xE2\x80\xA2 "; // U+2022
            for (const char *b = kBullet; *b != '\0' && o + 1 < outSize; ++b)
                out[o++] = *b;
            p += 2;
        }
        else if (afterText && o > 0 && out[o - 1] == '\n')
        {
            out[o - 1] = ' '; // the same paragraph, or list item, carrying on
        }
        afterText = !heading;

        for (; *p != '\0' && o + 2 < outSize; ++p)
        {
            if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_'))
            {
                p++;
                continue;
            }
            if (*p == '`')
                continue;
            if (*p == '[')
            {
                // [text](url) is its text.
                const char *close = strchr(p, ']');
                if (close != NULL && close[1] == '(' && strchr(close, ')') != NULL)
                {
                    for (const char *t = p + 1; t < close && o + 2 < outSize; ++t)
                        out[o++] = *t;
                    p = strchr(close, ')');
                    continue;
                }
            }
            out[o++] = *p;
        }
        out[o++] = '\n';
    }

    while (o > 0 && out[o - 1] == '\n')
        o--;
    out[o] = '\0';
}

// ---------------------------------------------------------------------------
// The check
// ---------------------------------------------------------------------------

static void CopyString(char *out, size_t outSize, const char *in)
{
    _snprintf(out, outSize, "%s", in != NULL ? in : "");
    out[outSize - 1] = '\0';
}

static DWORD WINAPI CheckEntry(LPVOID)
{
    char *reply = (char *)malloc(REPLY_MAX + 1);
    if (reply == NULL)
    {
        EnterCriticalSection(&g_lock);
        SetState(UPDATE_FAILED);
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    unsigned long long len = REPLY_MAX;
    const int status = httpRequestHTTPS(RELEASES_URL, HTTP_GET, NULL, "Accept: application/vnd.github+json\r\n", "",
                                        reply, &len, false, NULL, 0, QuietPrint);
    reply[len <= REPLY_MAX ? (size_t)len : REPLY_MAX] = '\0';

    // Pre-releases are offered only to a pre-release.
    const bool wantPre = (strchr(CURRENT_VERSION, '-') != NULL);

    UpdateState result = UPDATE_FAILED;
    UpdateInfo found;
    memset(&found, 0, sizeof(found));

    cJSON *root = (status == 200) ? cJSON_ParseWithLength(reply, (size_t)len) : NULL;
    if (root != NULL && cJSON_IsArray(root))
    {
        const cJSON *best = NULL;
        const char *bestVersion = NULL;
        const cJSON *release = NULL;
        cJSON_ArrayForEach(release, root)
        {
            const cJSON *tag = cJSON_GetObjectItemCaseSensitive(release, "tag_name");
            if (!cJSON_IsString(tag) || tag->valuestring == NULL ||
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "draft")))
                continue;
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(release, "prerelease")) && !wantPre)
                continue;
            if (best == NULL || CompareVersions(tag->valuestring, bestVersion) > 0)
            {
                best = release;
                bestVersion = tag->valuestring;
            }
        }

        if (best != NULL && CompareVersions(bestVersion, CURRENT_VERSION) > 0)
        {
            CopyString(found.version, sizeof(found.version), bestVersion[0] == 'v' ? bestVersion + 1 : bestVersion);
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(best, "name");
            CopyString(found.title, sizeof(found.title),
                       cJSON_IsString(name) && name->valuestring[0] != '\0' ? name->valuestring : found.version);
            const cJSON *body = cJSON_GetObjectItemCaseSensitive(best, "body");
            PlainNotes(cJSON_IsString(body) ? body->valuestring : "", found.notes, sizeof(found.notes));
            result = UPDATE_AVAILABLE;
        }
        else
        {
            result = UPDATE_CURRENT;
        }
    }
    else
    {
        dprintf("[update] couldn't check: HTTP %d, %I64u bytes\n", status, len);
    }
    if (root != NULL)
        cJSON_Delete(root);
    free(reply);

    if (result == UPDATE_AVAILABLE)
        dprintf("[update] %s is available (running %s)\n", found.version, CURRENT_VERSION);
    else if (result == UPDATE_CURRENT)
        dprintf("[update] up to date (%s)\n", CURRENT_VERSION);

    EnterCriticalSection(&g_lock);
    if (result == UPDATE_AVAILABLE)
        g_info = found;
    SetState(result);
    LeaveCriticalSection(&g_lock);
    return 0;
}

void StartUpdateCheck()
{
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }

    EnterCriticalSection(&g_lock);
    const bool running = (g_state == UPDATE_CHECKING);
    if (!running)
        SetState(UPDATE_CHECKING);
    LeaveCriticalSection(&g_lock);
    if (running)
        return;

    if (g_thread != NULL)
    {
        CloseHandle(g_thread); // the last check's, finished
        g_thread = NULL;
    }

    // The same stack as the other workers, for the HTTP and TLS buffers.
    g_thread = CreateThread(NULL, 256 * 1024, CheckEntry, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread == NULL)
    {
        EnterCriticalSection(&g_lock);
        SetState(UPDATE_FAILED);
        LeaveCriticalSection(&g_lock);
        return;
    }
#ifdef _XBOX
    // Hardware thread 5, with the cover and Store art workers: one request.
    XSetThreadProcessor(g_thread, 5);
#endif
    ResumeThread(g_thread);
}

UpdateState GetUpdateState(UpdateInfo *info, unsigned long *changeCount)
{
    if (!g_lockReady)
    {
        if (changeCount != NULL)
            *changeCount = 0;
        return UPDATE_NOT_CHECKED;
    }

    EnterCriticalSection(&g_lock);
    const UpdateState state = g_state;
    if (info != NULL && state == UPDATE_AVAILABLE)
        *info = g_info;
    if (changeCount != NULL)
        *changeCount = g_changes;
    LeaveCriticalSection(&g_lock);
    return state;
}
