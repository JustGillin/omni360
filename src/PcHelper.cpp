#include "PcHelper.h"
#include "Strings.h" // Tr() - every string the UI shows
#include "HttpPlain.h"
#include "OutputConsole.h"
#include "parsing.h" // LogEscapePercent, UrlEncodeFormValue

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELPER_PORT        36060
#define HELPER_FRESH_MS    15000 // it announces every 3 seconds
#define XBOX_SO_BYPASS_SECURITY 0x5801

static CRITICAL_SECTION g_helperLock;
static bool g_lockReady = false;

// The helper heard most recently.
static char g_heardUrl[64];
static char g_heardId[32];
static char g_heardName[64];
static DWORD g_heardAt = 0;
static bool g_heardEver = false;
static bool g_reportedGone = false; // said in the log that it went quiet

// The one paired with.
static char g_pairedId[32];
static char g_pairedToken[64];
static bool g_enabled = true;

static void EnsureLock()
{
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_helperLock);
        g_lockReady = true;
    }
}

static void CopyOut(char *out, unsigned int outSize, const char *from)
{
    if (out == NULL || outSize == 0)
        return;
    strncpy(out, from, outSize - 1);
    out[outSize - 1] = '\0';
}

static bool FreshLocked() { return g_heardEver && GetTickCount() - g_heardAt <= HELPER_FRESH_MS; }

static DWORD WINAPI ListenEntry(LPVOID)
{
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&xnsp) != 0)
        return 0;
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
        return 0;

    SOCKET s = INVALID_SOCKET;
    for (;;)
    {
        if (s == INVALID_SOCKET)
        {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s != INVALID_SOCKET)
            {
                BOOL yes = TRUE;
                setsockopt(s, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));
                setsockopt(s, SOL_SOCKET, SO_BROADCAST, (PCSTR)&yes, sizeof(yes));
                sockaddr_in sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port = htons(HELPER_PORT);
                sa.sin_addr.s_addr = INADDR_ANY;
                if (bind(s, (sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR)
                {
                    dprintf("[helper] couldn't listen on UDP %d: WSA error %d\n", HELPER_PORT, WSAGetLastError());
                    closesocket(s);
                    s = INVALID_SOCKET;
                }
            }
            if (s == INVALID_SOCKET)
            {
                Sleep(10000); // the network may not be up yet
                continue;
            }
        }

        // Wakes every few seconds even when nothing comes, to notice the
        // helper going quiet - closed, or the PC switched off.
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval wait = {5, 0};
        if (select(0, &readable, NULL, NULL, &wait) == 0)
        {
            EnterCriticalSection(&g_helperLock);
            const bool gone = g_heardEver && !g_reportedGone && !FreshLocked();
            if (gone)
                g_reportedGone = true;
            const bool paired = strcmp(g_heardId, g_pairedId) == 0;
            char name[64];
            CopyOut(name, sizeof(name), g_heardName);
            LeaveCriticalSection(&g_helperLock);
            if (gone)
                dprintf("[helper] PC helper \"%s\" stopped answering (%s) - downloads go to archive.org\n", name,
                        paired ? "paired" : "not paired");
            continue;
        }

        char buf[256];
        sockaddr_in from;
        int fromLen = sizeof(from);
        const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fromLen);
        if (n <= 0)
        {
            closesocket(s);
            s = INVALID_SOCKET;
            Sleep(2000);
            continue;
        }
        buf[n] = '\0';

        // "OMNI360-HELPER 2 <port> <id> <name>" - the name may have spaces.
        int version = 0, port = 0;
        char id[32] = "";
        if (sscanf(buf, "OMNI360-HELPER %d %d %31s", &version, &port, id) != 3 || version != 2 ||
            port <= 0 || port > 65535)
            continue;
        const char *pcName = buf;
        for (int spaces = 0; spaces < 4 && pcName != NULL; ++spaces)
        {
            pcName = strchr(pcName, ' ');
            if (pcName != NULL)
                ++pcName;
        }

        const unsigned char *ip = (const unsigned char *)&from.sin_addr.s_addr;
        char url[64];
        _snprintf(url, sizeof(url), "http://%u.%u.%u.%u:%d", ip[0], ip[1], ip[2], ip[3], port);
        url[sizeof(url) - 1] = '\0';

        EnterCriticalSection(&g_helperLock);
        const bool changed = !FreshLocked() || strcmp(url, g_heardUrl) != 0 || strcmp(id, g_heardId) != 0;
        CopyOut(g_heardUrl, sizeof(g_heardUrl), url);
        CopyOut(g_heardId, sizeof(g_heardId), id);
        CopyOut(g_heardName, sizeof(g_heardName), pcName != NULL ? pcName : "");
        g_heardAt = GetTickCount();
        g_heardEver = true;
        g_reportedGone = false;
        const bool paired = strcmp(id, g_pairedId) == 0;
        LeaveCriticalSection(&g_helperLock);
        if (changed)
            dprintf("[helper] PC helper \"%s\" at %s (%s)\n", pcName != NULL ? pcName : "", url,
                    paired ? "paired" : "not paired");
    }
}

// Checks in with the paired helper every 10 seconds, so its tray can say an
// Xbox is connected - the helper otherwise only hears from the console when
// it downloads. A thread of its own: a request can take a while to fail, and
// the listener mustn't miss announcements meanwhile.
//
// It is also what keeps a helper that was found by a scan, or remembered from
// settings.txt, counted as running: some PCs' broadcasts never reach the
// console - a firewall, a VPN or a second network adapter taking them - and
// without this, such a helper would drop out 15 seconds after it was found.
// /hello answers "omni360-helper 2 <id>", so a reply is as good as an
// announcement. Without a token it needs no pairing, so it works for a
// helper found but not yet paired with too.
static bool ParseHello(const char *reply, char *id, unsigned int idSize)
{
    int version = 0;
    char got[32] = "";
    if (sscanf(reply, "omni360-helper %d %31s", &version, got) != 2 || version != 2)
        return false;
    CopyOut(id, idSize, got);
    return true;
}

static DWORD WINAPI CheckInEntry(LPVOID)
{
    for (;;)
    {
        Sleep(8000);
        EnterCriticalSection(&g_helperLock);
        char base[64], heardId[32], pairedId[32], token[64];
        CopyOut(base, sizeof(base), g_heardEver ? g_heardUrl : "");
        CopyOut(heardId, sizeof(heardId), g_heardId);
        CopyOut(pairedId, sizeof(pairedId), g_pairedId);
        CopyOut(token, sizeof(token), g_pairedToken);
        LeaveCriticalSection(&g_helperLock);
        if (base[0] == '\0')
            continue;

        char url[160], reply[64];
        if (token[0] != '\0' && strcmp(heardId, pairedId) == 0)
            _snprintf(url, sizeof(url), "%s/hello?t=%s", base, token);
        else
            _snprintf(url, sizeof(url), "%s/hello", base);
        url[sizeof(url) - 1] = '\0';
        unsigned long long len = 0;
        char id[32];
        if (HttpGetPlain(url, NULL, reply, sizeof(reply) - 1, &len) != 200 || !ParseHello(reply, id, sizeof(id)))
            continue;

        EnterCriticalSection(&g_helperLock);
        // Unless an announcement moved it somewhere else meanwhile.
        if (strcmp(g_heardUrl, base) == 0)
        {
            const bool back = !FreshLocked();
            CopyOut(g_heardId, sizeof(g_heardId), id);
            g_heardAt = GetTickCount();
            g_reportedGone = false;
            if (back)
                dprintf("[helper] PC helper at %s answered\n", base);
        }
        LeaveCriticalSection(&g_helperLock);
    }
}

// ---------------------------------------------------------------------------
// Scanning for the helper
// ---------------------------------------------------------------------------
//
// For when its announcements don't arrive. Every address on the console's
// /24 is tried on the helper's HTTP port, a batch at a time with non-blocking
// connects, and whatever accepts is asked /hello. A PC that pairing could work
// with at all has that port open, so this finds every helper pairing could use.
//
// The wait per batch is long for a LAN on purpose. A PC on Wi-Fi that has
// dozed off to save power can take well over half a second to answer - the
// same helper answered in 7 ms on one scan and not within 700 ms on three
// others - so a short wait missed it. A batch is no bigger because XNet allows
// 32 TCP sockets in all, and downloads and the FTP server hold some of them.
// The scan stops at the first helper, so a PC low on the subnet is quick.

#define HELPER_HTTP_PORT   36061 // the helper's default, as its announcements say
#define SCAN_BATCH         24
#define SCAN_WAIT_MS       2500

// The addresses in one batch that accepted a connection, into open[].
static int ScanBatch(const unsigned long *addrs, int count, unsigned long *open)
{
    SOCKET socks[SCAN_BATCH];
    bool pending[SCAN_BATCH];
    for (int i = 0; i < count; ++i)
    {
        pending[i] = false;
        socks[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socks[i] == INVALID_SOCKET)
            continue;
        BOOL yes = TRUE;
        setsockopt(socks[i], SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));
        u_long nonBlocking = 1;
        ioctlsocket(socks[i], FIONBIO, &nonBlocking);
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(HELPER_HTTP_PORT);
        sa.sin_addr.s_addr = addrs[i];
        if (connect(socks[i], (sockaddr *)&sa, sizeof(sa)) == 0 || WSAGetLastError() == WSAEWOULDBLOCK)
            pending[i] = true;
    }

    int found = 0;
    const DWORD start = GetTickCount();
    for (;;)
    {
        const DWORD gone = GetTickCount() - start;
        if (gone >= SCAN_WAIT_MS)
            break;
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        int waiting = 0;
        for (int i = 0; i < count; ++i)
        {
            if (pending[i])
            {
                FD_SET(socks[i], &writable);
                FD_SET(socks[i], &failed);
                ++waiting;
            }
        }
        if (waiting == 0)
            break;
        const DWORD left = SCAN_WAIT_MS - gone;
        timeval wait;
        wait.tv_sec = left / 1000;
        wait.tv_usec = (left % 1000) * 1000;
        if (select(0, NULL, &writable, &failed, &wait) <= 0)
            break;
        for (int i = 0; i < count; ++i)
        {
            if (!pending[i])
                continue;
            if (FD_ISSET(socks[i], &failed))
            {
                pending[i] = false;
            }
            else if (FD_ISSET(socks[i], &writable))
            {
                // Writable means connected - a refused or failed connect is
                // reported in the failed set instead. /hello weeds out
                // anything else listening on the port.
                pending[i] = false;
                open[found++] = addrs[i];
            }
        }
    }

    for (int i = 0; i < count; ++i)
    {
        if (socks[i] != INVALID_SOCKET)
            closesocket(socks[i]);
    }
    return found;
}

PcHelperScanResult PcHelperScan(char *id, unsigned int idSize, char *name, unsigned int nameSize,
                                PcHelperScanProgress progress)
{
    EnsureLock();
    XNADDR xna;
    memset(&xna, 0, sizeof(xna));
    const DWORD flags = XNetGetTitleXnAddr(&xna);
    if (flags == XNET_GET_XNADDR_PENDING || (flags & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        return SCAN_NO_NETWORK;

    const unsigned char *own = (const unsigned char *)&xna.ina.s_addr;
    dprintf("[helper] scanning %u.%u.%u.1-254 for a helper on port %d\n", own[0], own[1], own[2], HELPER_HTTP_PORT);

    unsigned long addrs[SCAN_BATCH];
    unsigned long open[SCAN_BATCH];
    int done = 0;
    for (int host = 1; host <= 254;)
    {
        int count = 0;
        for (; host <= 254 && count < SCAN_BATCH; ++host)
        {
            if (host == own[3])
                continue;
            unsigned char *b = (unsigned char *)&addrs[count];
            b[0] = own[0];
            b[1] = own[1];
            b[2] = own[2];
            b[3] = (unsigned char)host;
            ++count;
        }
        if (progress != NULL)
            progress(done, 253);
        const int found = ScanBatch(addrs, count, open);
        done += count;

        for (int i = 0; i < found; ++i)
        {
            const unsigned char *ip = (const unsigned char *)&open[i];
            char base[64], url[96], reply[64], gotId[32];
            _snprintf(base, sizeof(base), "http://%u.%u.%u.%u:%d", ip[0], ip[1], ip[2], ip[3], HELPER_HTTP_PORT);
            base[sizeof(base) - 1] = '\0';
            _snprintf(url, sizeof(url), "%s/hello", base);
            url[sizeof(url) - 1] = '\0';
            unsigned long long len = 0;
            if (HttpGetPlain(url, NULL, reply, sizeof(reply) - 1, &len) != 200 || !ParseHello(reply, gotId, sizeof(gotId)))
                continue;

            char pcName[64];
            EnterCriticalSection(&g_helperLock);
            // /hello doesn't say the PC's name; keep the one an announcement
            // gave, or go by the address.
            if (strcmp(g_heardId, gotId) != 0 || g_heardName[0] == '\0')
                _snprintf(g_heardName, sizeof(g_heardName), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            g_heardName[sizeof(g_heardName) - 1] = '\0';
            CopyOut(g_heardUrl, sizeof(g_heardUrl), base);
            CopyOut(g_heardId, sizeof(g_heardId), gotId);
            g_heardAt = GetTickCount();
            g_heardEver = true;
            g_reportedGone = false;
            CopyOut(pcName, sizeof(pcName), g_heardName);
            LeaveCriticalSection(&g_helperLock);

            dprintf("[helper] scan found the helper at %s\n", base);
            CopyOut(id, idSize, gotId);
            CopyOut(name, nameSize, pcName);
            return SCAN_FOUND;
        }
    }
    dprintf("[helper] scan found no helper\n");
    return SCAN_NOT_FOUND;
}

bool PcHelperKnownUrl(char *url, unsigned int urlSize)
{
    EnsureLock();
    EnterCriticalSection(&g_helperLock);
    const bool known = g_heardEver && g_heardUrl[0] != '\0';
    CopyOut(url, urlSize, known ? g_heardUrl : "");
    LeaveCriticalSection(&g_helperLock);
    return known;
}

void StartPcHelperListener()
{
    EnsureLock();
    HANDLE h = CreateThread(NULL, 0, ListenEntry, NULL, 0, NULL);
    if (h != NULL)
        CloseHandle(h);
    h = CreateThread(NULL, 0, CheckInEntry, NULL, 0, NULL);
    if (h != NULL)
        CloseHandle(h);
}

void SetPcHelperPairing(const char *id, const char *token, bool enabled, const char *url)
{
    EnsureLock();
    EnterCriticalSection(&g_helperLock);
    CopyOut(g_pairedId, sizeof(g_pairedId), id != NULL ? id : "");
    CopyOut(g_pairedToken, sizeof(g_pairedToken), token != NULL ? token : "");
    g_enabled = enabled;
    // Where the paired helper was last: the check-in asks it straight away,
    // in case its announcements never get here. Not counted as running until
    // it answers. Only when nothing has been heard yet - an announcement is
    // newer than settings.txt.
    if (url != NULL && url[0] != '\0' && g_pairedId[0] != '\0' && !g_heardEver)
    {
        CopyOut(g_heardUrl, sizeof(g_heardUrl), url);
        CopyOut(g_heardId, sizeof(g_heardId), g_pairedId);
        g_heardAt = GetTickCount() - HELPER_FRESH_MS - 1;
        g_heardEver = true;
        g_reportedGone = true; // nothing to report going quiet yet
    }
    LeaveCriticalSection(&g_helperLock);
}

PcHelperStatus GetPcHelperStatus(char *name, unsigned int nameSize)
{
    EnsureLock();
    EnterCriticalSection(&g_helperLock);
    const bool fresh = FreshLocked();
    const bool paired = g_pairedId[0] != '\0' && g_pairedToken[0] != '\0';
    PcHelperStatus status;
    if (paired)
        status = (fresh && strcmp(g_heardId, g_pairedId) == 0) ? PC_HELPER_PAIRED_ONLINE : PC_HELPER_PAIRED_OFFLINE;
    else
        status = fresh ? PC_HELPER_FOUND : PC_HELPER_NONE;
    CopyOut(name, nameSize, g_heardEver ? g_heardName : "");
    LeaveCriticalSection(&g_helperLock);
    return status;
}

bool PcHelperUrl(char *url, unsigned int urlSize, char *token, unsigned int tokenSize,
                 char *name, unsigned int nameSize)
{
    EnsureLock();
    EnterCriticalSection(&g_helperLock);
    const bool ok = g_enabled && g_pairedToken[0] != '\0' && FreshLocked() && strcmp(g_heardId, g_pairedId) == 0;
    if (ok)
    {
        CopyOut(url, urlSize, g_heardUrl);
        CopyOut(token, tokenSize, g_pairedToken);
        CopyOut(name, nameSize, g_heardName);
    }
    LeaveCriticalSection(&g_helperLock);
    return ok;
}

// The helper being paired with: where pairing started, so the wait goes to
// the same one.
static char g_pairingUrl[64];

bool PcHelperRequestPair(char *requestId, unsigned int requestIdSize, char *code, unsigned int codeSize,
                         char *message, unsigned int messageSize)
{
    EnsureLock();
    EnterCriticalSection(&g_helperLock);
    const bool fresh = FreshLocked();
    CopyOut(g_pairingUrl, sizeof(g_pairingUrl), g_heardUrl);
    LeaveCriticalSection(&g_helperLock);
    if (!fresh)
    {
        CopyOut(message, messageSize, Tr(STR_THE_HELPER_ISNT_RUNNING_ON));
        return false;
    }

    char url[128];
    _snprintf(url, sizeof(url), "%s/pair/request", g_pairingUrl);
    url[sizeof(url) - 1] = '\0';
    char reply[128];
    unsigned long long len = 0;
    const int status = HttpGetPlain(url, NULL, reply, sizeof(reply) - 1, &len);
    char gotId[40] = "", gotCode[8] = "";
    if (status == 200 && sscanf(reply, "%39s %7s", gotId, gotCode) == 2)
    {
        CopyOut(requestId, requestIdSize, gotId);
        CopyOut(code, codeSize, gotCode);
        return true;
    }
    dprintf("[helper] pairing request failed: HTTP %d\n", status);
    CopyOut(message, messageSize, status == 404 ? Tr(STR_YOUR_PCS_HELPER_IS_OUT)
                                                : Tr(STR_COULDNT_REACH_THE_HELPER_CHECK));
    return false;
}

PcHelperPairResult PcHelperWaitPair(const char *requestId, char *token, unsigned int tokenSize, char *id,
                                    unsigned int idSize, char *message, unsigned int messageSize)
{
    char url[160];
    _snprintf(url, sizeof(url), "%s/pair/wait?r=%s", g_pairingUrl, requestId);
    url[sizeof(url) - 1] = '\0';
    char reply[128];
    unsigned long long len = 0;
    const int status = HttpGetPlain(url, NULL, reply, sizeof(reply) - 1, &len);
    char gotToken[64] = "", gotId[32] = "";
    if (status == 200 && sscanf(reply, "%63s %31s", gotToken, gotId) == 2)
    {
        CopyOut(token, tokenSize, gotToken);
        CopyOut(id, idSize, gotId);
        dprintf("[helper] paired with helper %s\n", gotId);
        return PAIR_ALLOWED;
    }
    if (status == 408)
        return PAIR_WAITING;
    dprintf("[helper] pairing ended: HTTP %d\n", status);
    // 410: the PC's prompt ran out with nobody answering - not a No.
    CopyOut(message, messageSize, status == 403   ? Tr(STR_PAIRING_WASNT_ALLOWED_ON_YOUR)
                                  : status == 410 ? Tr(STR_NOBODY_ANSWERED_ON_YOUR_PC)
                                                  : Tr(STR_LOST_TOUCH_WITH_THE_HELPER));
    return PAIR_FAILED;
}

// Routing archive.org requests through the helper.

static DWORD g_routeFailedAt = 0;
static bool g_routeFailed = false;
static char g_lastRouted[160]; // the last file routed, to log each file once
static __declspec(thread) bool t_bypass = false;

void PcHelperSetThreadBypass(bool bypass) { t_bypass = bypass; }

// "<item>/<file>" out of an archive.org file URL; false for anything else.
static bool ArchiveFilePath(const char *url, const char **start)
{
    if (_strnicmp(url, "https://", 8) != 0)
        return false;
    const char *host = url + 8;
    const char *slash = strchr(host, '/');
    if (slash == NULL)
        return false;
    const size_t hostLen = slash - host;
    const char *suffix = ".archive.org";
    const size_t suffixLen = strlen(suffix);
    const bool mainHost = hostLen == 11 && _strnicmp(host, "archive.org", 11) == 0;
    const bool storage = hostLen > suffixLen && _strnicmp(slash - suffixLen, suffix, suffixLen) == 0;

    const char *rest = NULL;
    if (mainHost && strncmp(slash, "/download/", 10) == 0)
        rest = slash + 10;
    else if (storage)
    {
        // "/<number>/items/<item>/<file>"
        const char *items = strstr(slash, "/items/");
        if (items == NULL)
            return false;
        for (const char *c = slash + 1; c < items; ++c)
        {
            if (*c < '0' || *c > '9')
                return false;
        }
        rest = items + 7;
    }
    if (rest == NULL)
        return false;

    // Exactly "<item>/<file>": one slash, and no query.
    const char *mid = strchr(rest, '/');
    if (mid == NULL || mid == rest || mid[1] == '\0' || strchr(mid + 1, '/') != NULL || strchr(rest, '?') != NULL)
        return false;
    *start = rest;
    return true;
}

bool PcHelperRoute(const char *archiveUrl, char *helperUrl, unsigned int helperUrlSize)
{
    const char *path = NULL;
    if (t_bypass || archiveUrl == NULL || !ArchiveFilePath(archiveUrl, &path))
        return false;

    char base[64], token[64], name[64];
    if (!PcHelperUrl(base, sizeof(base), token, sizeof(token), name, sizeof(name)))
        return false;

    EnterCriticalSection(&g_helperLock);
    if (g_routeFailed && GetTickCount() - g_routeFailedAt < 60000)
    {
        LeaveCriticalSection(&g_helperLock);
        return false;
    }
    g_routeFailed = false;
    const bool newFile = strncmp(g_lastRouted, path, sizeof(g_lastRouted) - 1) != 0;
    if (newFile)
        CopyOut(g_lastRouted, sizeof(g_lastRouted), path);
    LeaveCriticalSection(&g_helperLock);

    const int n = _snprintf(helperUrl, helperUrlSize, "%s/file/%s?t=%s", base, path, token);
    if (n < 0 || (unsigned int)n >= helperUrlSize)
        return false;
    if (newFile)
    {
        // Escaped: the path is URL-encoded, and the log formats a line twice,
        // so "Halo%20Spartan" read "%20S" as a wide string and crashed.
        char safePath[320];
        LogEscapePercent(path, safePath, sizeof(safePath));
        dprintf("[helper] fetching %s through the PC helper \"%s\"\n", safePath, name);
    }
    return true;
}

bool PcHelperWouldRoute(const char *archiveUrl)
{
    const char *path = NULL;
    if (t_bypass || archiveUrl == NULL || !ArchiveFilePath(archiveUrl, &path))
        return false;
    if (!PcHelperUrl(NULL, 0, NULL, 0, NULL, 0))
        return false;
    EnterCriticalSection(&g_helperLock);
    const bool backedOff = g_routeFailed && GetTickCount() - g_routeFailedAt < 60000;
    LeaveCriticalSection(&g_helperLock);
    return !backedOff;
}

// A cancelled game, for the helper to drop - sent on a thread of its own so
// cancelling never waits on the network.
struct ForgetRequest
{
    char url[1400];
    char name[200];
};

static DWORD WINAPI ForgetEntry(LPVOID param)
{
    ForgetRequest *job = (ForgetRequest *)param;
    char reply[64];
    unsigned long long len = 0;
    const int status = HttpGetPlain(job->url, NULL, reply, sizeof(reply) - 1, &len);
    char safe[400];
    LogEscapePercent(job->name, safe, sizeof(safe));
    dprintf("[helper] told the PC to drop %s (HTTP %d)\n", safe, status);
    free(job);
    return 0;
}

void PcHelperForget(const char *item, const char *file)
{
    if (item == NULL || file == NULL || item[0] == '\0' || file[0] == '\0')
        return;
    char base[64], token[64];
    if (!PcHelperUrl(base, sizeof(base), token, sizeof(token), NULL, 0))
        return;

    char encItem[200], encFile[800];
    if (!UrlEncodeFormValue(item, encItem, sizeof(encItem)) || !UrlEncodeFormValue(file, encFile, sizeof(encFile)))
        return;

    ForgetRequest *job = (ForgetRequest *)malloc(sizeof(ForgetRequest));
    if (job == NULL)
        return;
    const int n = _snprintf(job->url, sizeof(job->url), "%s/forget?item=%s&file=%s&t=%s", base, encItem, encFile, token);
    if (n < 0 || n >= (int)sizeof(job->url))
    {
        free(job);
        return;
    }
    _snprintf(job->name, sizeof(job->name), "%s/%s", item, file);
    job->name[sizeof(job->name) - 1] = '\0';

    HANDLE h = CreateThread(NULL, 0, ForgetEntry, job, 0, NULL);
    if (h == NULL)
        free(job);
    else
        CloseHandle(h);
}

// --- What the PC has fetched ahead, for the Queue's waiting rows ---------

#define AHEAD_SLOTS 16

struct AheadEntry
{
    char item[96];
    char file[256];
    int percent;      // -1 until known
    DWORD askedAt;    // the Queue last wanted it
};

static AheadEntry g_ahead[AHEAD_SLOTS];
static bool g_aheadThread = false;

static DWORD WINAPI AheadEntryThread(LPVOID)
{
    for (;;)
    {
        Sleep(3000);
        for (int i = 0; i < AHEAD_SLOTS; ++i)
        {
            char item[96], file[256];
            EnterCriticalSection(&g_helperLock);
            const bool wanted = g_ahead[i].item[0] != '\0' && GetTickCount() - g_ahead[i].askedAt < 10000;
            CopyOut(item, sizeof(item), g_ahead[i].item);
            CopyOut(file, sizeof(file), g_ahead[i].file);
            LeaveCriticalSection(&g_helperLock);
            if (!wanted)
                continue;

            char base[64], token[64];
            if (!PcHelperUrl(base, sizeof(base), token, sizeof(token), NULL, 0))
                continue;
            char encItem[200], encFile[800], url[1400];
            if (!UrlEncodeFormValue(item, encItem, sizeof(encItem)) || !UrlEncodeFormValue(file, encFile, sizeof(encFile)))
                continue;
            _snprintf(url, sizeof(url), "%s/status?item=%s&file=%s&t=%s", base, encItem, encFile, token);
            url[sizeof(url) - 1] = '\0';

            char reply[64];
            unsigned long long len = 0, size = 0, have = 0;
            int percent = -1;
            if (HttpGetPlain(url, NULL, reply, sizeof(reply) - 1, &len) == 200 &&
                sscanf(reply, "%I64u %I64u", &size, &have) == 2 && size > 0)
                percent = (int)(have * 100 / size);

            EnterCriticalSection(&g_helperLock);
            if (strcmp(g_ahead[i].item, item) == 0 && strcmp(g_ahead[i].file, file) == 0)
                g_ahead[i].percent = percent;
            LeaveCriticalSection(&g_helperLock);
        }
    }
}

bool PcHelperAheadPercent(const char *item, const char *file, int *percent)
{
    if (item == NULL || file == NULL || item[0] == '\0' || file[0] == '\0')
        return false;
    if (!PcHelperUrl(NULL, 0, NULL, 0, NULL, 0))
        return false;

    EnsureLock();
    EnterCriticalSection(&g_helperLock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < AHEAD_SLOTS; ++i)
    {
        if (strcmp(g_ahead[i].item, item) == 0 && strcmp(g_ahead[i].file, file) == 0)
        {
            slot = i;
            break;
        }
        if (g_ahead[i].askedAt < g_ahead[oldest].askedAt)
            oldest = i;
    }
    if (slot < 0)
    {
        slot = oldest;
        CopyOut(g_ahead[slot].item, sizeof(g_ahead[slot].item), item);
        CopyOut(g_ahead[slot].file, sizeof(g_ahead[slot].file), file);
        g_ahead[slot].percent = -1;
    }
    g_ahead[slot].askedAt = GetTickCount();
    const int known = g_ahead[slot].percent;
    const bool start = !g_aheadThread;
    g_aheadThread = true;
    LeaveCriticalSection(&g_helperLock);

    if (start)
    {
        HANDLE h = CreateThread(NULL, 0, AheadEntryThread, NULL, 0, NULL);
        if (h != NULL)
            CloseHandle(h);
    }
    if (known < 0)
        return false;
    *percent = known;
    return true;
}

// --- Whether a routed transfer is going -----------------------------------

static volatile LONG g_transfers = 0;
static DWORD g_transferOkAt = 0;

void PcHelperTransferBegin() { InterlockedIncrement(&g_transfers); }

void PcHelperTransferEnd(bool ok)
{
    InterlockedDecrement(&g_transfers);
    if (ok)
        g_transferOkAt = GetTickCount();
}

// A routed request in flight, or one that finished well in the last few
// seconds - a DLC member's small header reads come and go between them.
bool PcHelperTransferActive()
{
    return g_transfers > 0 || (g_transferOkAt != 0 && GetTickCount() - g_transferOkAt < 5000);
}

void PcHelperRouteFailed(int status)
{
    EnterCriticalSection(&g_helperLock);
    const bool first = !g_routeFailed;
    g_routeFailed = true;
    g_routeFailedAt = GetTickCount();
    g_lastRouted[0] = '\0';
    LeaveCriticalSection(&g_helperLock);
    if (first)
        dprintf("[helper] the PC helper answered HTTP %d - archive.org for the next minute\n", status);
}
