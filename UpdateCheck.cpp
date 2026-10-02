/*
FILE : UpdateCheck.cpp
PROJECT : Omni360
DESCRIPTION : Asks GitHub whether there's a newer release. See UpdateCheck.h.
*/

#include "UpdateCheck.h"
#include "UpdateKey.h"    // kUpdatePublicKey
#include "settings.h"     // CURRENT_VERSION
#include "downloadFile.h" // httpRequestHTTPS
#include "OutputConsole.h"
#include "cJSON.h"
#include "inc\bearssl.h"  // SHA-256 and ECDSA, for the release's signature

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>

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

static bool EndsWithNoCase(const char *s, const char *suffix)
{
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && _stricmp(s + n - m, suffix) == 0;
}

// The release's XEX and its signature, from its assets.
static void FindReleaseFiles(const cJSON *release, UpdateInfo *info)
{
    const cJSON *asset = NULL;
    cJSON_ArrayForEach(asset, cJSON_GetObjectItemCaseSensitive(release, "assets"))
    {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
        if (!cJSON_IsString(name) || !cJSON_IsString(url) || strncmp(url->valuestring, "https://", 8) != 0)
            continue;
        if (EndsWithNoCase(name->valuestring, ".xex.sig"))
        {
            CopyString(info->sigUrl, sizeof(info->sigUrl), url->valuestring);
        }
        else if (EndsWithNoCase(name->valuestring, ".xex"))
        {
            CopyString(info->xexUrl, sizeof(info->xexUrl), url->valuestring);
            const cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
            info->xexSize = cJSON_IsNumber(size) && size->valuedouble > 0 ? (unsigned long long)size->valuedouble : 0;
        }
    }
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
            FindReleaseFiles(best, &found);
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

// ---------------------------------------------------------------------------
// Installing one
// ---------------------------------------------------------------------------

static UpdateProgressFn g_progress = NULL;

static bool DownloadProgress(unsigned long long done, unsigned long long total, unsigned long long, unsigned long long)
{
    if (g_progress != NULL)
        g_progress(done, total);
    return true;
}

// A GET into buffer, following GitHub's redirects to where a release's files
// are kept. The client hands back a redirect's Location in the buffer.
static int GetFollowing(const char *url, char *buffer, unsigned long long capacity, unsigned long long *outLen,
                        DownloadProgressFn progress)
{
    std::string at = url;
    for (int hop = 0; hop < 5; ++hop)
    {
        unsigned long long len = capacity;
        const int status = httpRequestHTTPS(at, HTTP_GET, NULL, "", "", buffer, &len, false, NULL, 0, QuietPrint, 0,
                                            progress);
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            char next[2048];
            strncpy(next, buffer, sizeof(next) - 1);
            next[sizeof(next) - 1] = '\0';
            if (strncmp(next, "https://", 8) != 0)
                return -1; // only ever on to another HTTPS address
            at = next;
            continue;
        }
        *outLen = len;
        return status;
    }
    return -1;
}

// Which XEX in game:\ - the folder Omni360 runs from - is this one: the only
// one there, or Omni360.xex if there are several.
static bool FindRunningXex(char *name, size_t nameSize)
{
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA("game:\\*.xex", &found);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    int count = 0;
    bool haveOmni = false;
    char only[MAX_PATH] = "";
    do
    {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        count++;
        CopyString(only, sizeof(only), found.cFileName);
        if (_stricmp(found.cFileName, "Omni360.xex") == 0)
            haveOmni = true;
    } while (FindNextFileA(h, &found));
    FindClose(h);

    if (count == 1)
        CopyString(name, nameSize, only);
    else if (haveOmni)
        CopyString(name, nameSize, "Omni360.xex");
    else
        return false;
    return true;
}

// The signature, as tools/sign_release.py makes it: ECDSA on P-256 over the
// XEX's SHA-256, DER-encoded, against the key compiled in from UpdateKey.h.
static bool SignatureChecks(const unsigned char *data, size_t len, const unsigned char *sig, size_t sigLen)
{
    unsigned char hash[32];
    br_sha256_context sha;
    br_sha256_init(&sha);
    br_sha256_update(&sha, data, len);
    br_sha256_out(&sha, hash);

    br_ec_public_key key;
    key.curve = BR_EC_secp256r1;
    key.q = (unsigned char *)kUpdatePublicKey;
    key.qlen = sizeof(kUpdatePublicKey);
    return br_ecdsa_i31_vrfy_asn1(&br_ec_p256_m31, hash, sizeof(hash), &key, sig, sigLen) == 1;
}

UpdateInstallResult InstallUpdate(const UpdateInfo &info, UpdateProgressFn progress,
                                  char *outXexPath, unsigned long outXexPathSize)
{
    if (!UpdateInstallable(info))
        return UPDATE_INSTALL_NOT_SIGNED;

    char name[MAX_PATH];
    if (!FindRunningXex(name, sizeof(name)))
        return UPDATE_INSTALL_NO_RUNNING_XEX;

    // The signature first: it's small, and without it there's no point.
    char sig[512];
    unsigned long long sigLen = 0;
    int status = GetFollowing(info.sigUrl, sig, sizeof(sig) - 1, &sigLen, NULL);
    if (status != 200 || sigLen < 8 || sigLen > 200)
    {
        dprintf("[update] signature download: HTTP %d, %I64u bytes\n", status, sigLen);
        return UPDATE_INSTALL_DOWNLOAD_FAILED;
    }

    // The XEX, into memory - a few MB. Room for what the release says, and
    // then some, in case it doesn't say.
    unsigned long long capacity = info.xexSize > 0 ? info.xexSize + 1024 * 1024 : 32ULL * 1024 * 1024;
    if (capacity > 64ULL * 1024 * 1024)
        capacity = 64ULL * 1024 * 1024;
    char *xex = (char *)malloc((size_t)capacity + 1);
    if (xex == NULL)
        return UPDATE_INSTALL_DOWNLOAD_FAILED;

    g_progress = progress;
    unsigned long long xexLen = 0;
    status = GetFollowing(info.xexUrl, xex, capacity, &xexLen, DownloadProgress);
    g_progress = NULL;
    if (status != 200 || xexLen < 4096 || (info.xexSize > 0 && xexLen != info.xexSize))
    {
        dprintf("[update] XEX download: HTTP %d, %I64u bytes (expected %I64u)\n", status, xexLen, info.xexSize);
        free(xex);
        return UPDATE_INSTALL_DOWNLOAD_FAILED;
    }
    if (memcmp(xex, "XEX2", 4) != 0)
    {
        free(xex);
        return UPDATE_INSTALL_NOT_AN_XEX;
    }
    if (!SignatureChecks((const unsigned char *)xex, (size_t)xexLen, (const unsigned char *)sig, (size_t)sigLen))
    {
        dprintf("[update] %s's signature doesn't match this app's key - refused\n", info.version);
        free(xex);
        return UPDATE_INSTALL_BAD_SIGNATURE;
    }

    // Beside the running one, then swapped in: the running XEX becomes .old,
    // and goes back if the new one can't be put in its place.
    char current[MAX_PATH + 8], fresh[MAX_PATH + 16], old[MAX_PATH + 16];
    _snprintf(current, sizeof(current), "game:\\%s", name);
    _snprintf(fresh, sizeof(fresh), "game:\\%s.new", name);
    _snprintf(old, sizeof(old), "game:\\%s.old", name);
    current[sizeof(current) - 1] = '\0';
    fresh[sizeof(fresh) - 1] = '\0';
    old[sizeof(old) - 1] = '\0';

    FILE *f = fopen(fresh, "wb");
    bool written = (f != NULL) && fwrite(xex, 1, (size_t)xexLen, f) == (size_t)xexLen;
    if (f != NULL && fclose(f) != 0)
        written = false;
    free(xex);
    if (!written)
    {
        remove(fresh);
        return UPDATE_INSTALL_WRITE_FAILED;
    }

    remove(old);
    if (rename(current, old) != 0)
    {
        dprintf("[update] couldn't move %s aside (%lu)\n", current, GetLastError());
        remove(fresh);
        return UPDATE_INSTALL_SWAP_FAILED;
    }
    if (rename(fresh, current) != 0)
    {
        dprintf("[update] couldn't put %s in place (%lu) - restoring\n", fresh, GetLastError());
        rename(old, current);
        remove(fresh);
        return UPDATE_INSTALL_SWAP_FAILED;
    }

    dprintf("[update] installed %s as %s; the previous one is %s\n", info.version, current, old);
    CopyString(outXexPath, outXexPathSize, current);
    return UPDATE_INSTALL_OK;
}

const char *UpdateInstallResultText(UpdateInstallResult result)
{
    switch (result)
    {
    case UPDATE_INSTALL_OK:              return "Installed.";
    case UPDATE_INSTALL_NOT_SIGNED:      return "This release isn't signed, so it can't be installed from here.";
    case UPDATE_INSTALL_DOWNLOAD_FAILED: return "It couldn't be downloaded from GitHub. Try again later.";
    case UPDATE_INSTALL_BAD_SIGNATURE:   return "Its signature doesn't match - it may not be a genuine release. Not installed.";
    case UPDATE_INSTALL_NOT_AN_XEX:      return "What GitHub sent isn't an Xbox 360 program. Not installed.";
    case UPDATE_INSTALL_NO_RUNNING_XEX:  return "Couldn't tell which file in Omni360's folder is the app.";
    case UPDATE_INSTALL_WRITE_FAILED:    return "There wasn't room to save it beside the current version.";
    case UPDATE_INSTALL_SWAP_FAILED:     return "It couldn't replace the current version, which is unchanged.";
    }
    return "Something went wrong.";
}
