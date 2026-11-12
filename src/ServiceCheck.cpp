/*
FILE : ServiceCheck.cpp
PROJECT : Omni360
DESCRIPTION : Tells a slow archive.org apart from a slow line, by measuring a
              second host while a download crawls. See ServiceCheck.h.
*/

#include "ServiceCheck.h"
#include "downloadFile.h" // HttpsSession
#include "OutputConsole.h"
#include "dns.h" // ResolveDNS, for the stall probe's bare connects

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// A megabyte of nothing in particular, kept in the repository for this.
//
// raw.githubusercontent.com rather than the contents API that Featured.cpp
// uses: api.github.com allows 60 requests an hour from one address without a
// token, and a long slow download would spend a fifth of that on probing,
// on top of the update check and the featured fetch. Raw content isn't on
// that limit. It chains to ISRG Root X1, already in TrustAnchors.h, so it
// costs no new anchor.
//
// The file's bytes are random so that nothing in the path can usefully
// compress it - we ask for identity encoding, and GitHub honours that, but a
// megabyte that squeezed down to nothing would read as a fast line and the
// mistake would be invisible.
//
// Worth knowing when reading a verdict: this is a CDN edge and usually
// nearer than archive.org's one origin, so the comparison leans a little
// towards naming archive.org. CLEARLY_FASTER is set wide enough to live
// with that.
#define PROBE_URL    "https://raw.githubusercontent.com/JustGillin/omni360/master/probe.bin"
#define PROBE_ACCEPT ""
#define PROBE_BYTES  (1024 * 1024)

// The small file fetched first, to pay for DNS, the connect and the TLS
// handshake before anything is timed. On this console the handshake is the
// slow part of a short request, and it would swamp the measurement.
#define WARM_URL    "https://raw.githubusercontent.com/JustGillin/omni360/master/featured.json"
#define WARM_ACCEPT PROBE_ACCEPT
#define WARM_MAX    (64 * 1024)

// Below this, one connection is doing badly enough to be worth asking about.
// A healthy connection on this console runs 1-2 MB/s of the 2-4 the hardware
// can take at all; the downloads that prompted this were managing 0.2.
#define WORTH_ASKING (1024ULL * 1024)

// GitHub has to be this many times quicker before archive.org is named. The
// two hosts sit at different distances and both squeeze through the same
// 255 KB receive buffer, so this is a coarse comparison - good for telling
// four times apart, not for telling 0.9 from 1.1.
#define CLEARLY_FASTER 3

#define PROBE_EVERY_MS  (10 * 60 * 1000)
#define VERDICT_FOR_MS  (15 * 60 * 1000)

#define VERDICT_MAX 200

static const char kBlameArchive[] =
    "archive.org is slow right now, not your connection. Downloads will take longer than usual but will still finish.";

static const char kBlameLine[] =
    "Downloads are slow right now, and so is everything else - it looks like your connection rather than archive.org.";

static CRITICAL_SECTION g_lock;
static bool g_ready = false;

static unsigned long long g_worstSeen = 0; // the best a connection managed while doing badly
static bool g_sawBad = false;
static DWORD g_lastProbe = 0;
static bool g_everProbed = false;
static bool g_probing = false;
static char g_verdict[VERDICT_MAX] = "";
static DWORD g_verdictAt = 0;

// ---------------------------------------------------------------------------
// Probing
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

static void PublishVerdict(const char *text)
{
    EnterCriticalSection(&g_lock);
    _snprintf(g_verdict, sizeof(g_verdict), "%s", text != NULL ? text : "");
    g_verdict[sizeof(g_verdict) - 1] = '\0';
    g_verdictAt = GetTickCount();
    LeaveCriticalSection(&g_lock);
}

static DWORD WINAPI ProbeEntry(LPVOID)
{
    EnterCriticalSection(&g_lock);
    const unsigned long long theirs = g_worstSeen;
    LeaveCriticalSection(&g_lock);

    char *warm = (char *)malloc(WARM_MAX + 1);
    char *body = (char *)malloc(PROBE_BYTES + 1);
    HttpsSession *session = HttpsSessionOpen(QuietPrint);
    if (warm == NULL || body == NULL || session == NULL)
    {
        free(warm);
        free(body);
        HttpsSessionClose(session);
        EnterCriticalSection(&g_lock);
        g_probing = false;
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    // Connect, handshake and a small reply, none of it timed.
    unsigned long long warmLen = WARM_MAX;
    const int warmStatus = HttpsSessionGet(session, WARM_URL, WARM_ACCEPT, warm, &warmLen);

    unsigned long long len = PROBE_BYTES;
    int status = 0;
    DWORD took = 0;
    if (warmStatus == 200)
    {
        const DWORD began = GetTickCount();
        status = HttpsSessionGet(session, PROBE_URL, PROBE_ACCEPT, body, &len);
        took = GetTickCount() - began;
    }
    HttpsSessionClose(session);
    free(warm);
    free(body);

    if (warmStatus != 200)
        dprintf("[check] couldn't reach GitHub to compare (HTTP %d) - saying nothing\n", warmStatus);
    else if (status != 200 || len != PROBE_BYTES)
        dprintf("[check] the probe came back HTTP %d, %I64u of %d bytes - saying nothing\n", status, len,
                PROBE_BYTES);
    else
    {
        if (took == 0)
            took = 1; // quicker than the clock can see is still quick
        const unsigned long long mine = len * 1000 / took;
        const bool archiveIsSlow = (mine >= theirs * CLEARLY_FASTER);
        dprintf("[check] archive.org %.2f MB/s a connection, GitHub %.2f MB/s - %s\n", theirs / 1048576.0,
                mine / 1048576.0, archiveIsSlow ? "archive.org is the slow one" : "the line here is the slow one");
        PublishVerdict(archiveIsSlow ? kBlameArchive : kBlameLine);
    }

    EnterCriticalSection(&g_lock);
    g_probing = false;
    g_sawBad = false;
    g_worstSeen = 0;
    LeaveCriticalSection(&g_lock);
    return 0;
}

// ---------------------------------------------------------------------------
// The same question, asked the moment a connection goes silent
// ---------------------------------------------------------------------------
//
// The rate probe above compares speeds over minutes. A stall is a different
// thing: both connections receive nothing at all for ten seconds and more,
// and a new connection to the same server handshakes at once and then gets
// nothing either. Whether GitHub's megabyte arrives during that says where
// the silence is.

#define STALL_PROBE_EVERY_MS (60 * 1000)
#define STALL_PROBES_MAX     12

static bool g_stallProbing = false;
static DWORD g_lastStallProbe = 0;
static int g_stallProbes = 0;
static char g_stallHost[128] = "";
static unsigned long g_stallSilentMs = 0;

// How long a bare TCP connect to host takes, the lookup timed apart from it.
// A connect is one round trip - tens of milliseconds. One that takes seconds
// is a SYN that was lost and sent again, which is packet loss between this
// console and that host, seen directly. -1 for a part that failed.
#define XBOX_SO_BYPASS_SECURITY 0x5801

static void TimeConnect(const char *host, unsigned short port, long *dnsMs, long *connectMs)
{
    *dnsMs = -1;
    *connectMs = -1;

    char ip[64] = "";
    DWORD from = GetTickCount();
    if (!ResolveDNS(host, ip, sizeof(ip)) || ip[0] == '\0')
        return;
    *dnsMs = (long)(GetTickCount() - from);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr(ip);
    from = GetTickCount();
    if (connect(s, (sockaddr *)&sa, sizeof(sa)) != SOCKET_ERROR)
        *connectMs = (long)(GetTickCount() - from);
    closesocket(s);
}

static DWORD WINAPI StallProbeEntry(LPVOID)
{
    char host[sizeof(g_stallHost)];
    EnterCriticalSection(&g_lock);
    memcpy(host, g_stallHost, sizeof(host));
    const unsigned long silentMs = g_stallSilentMs;
    LeaveCriticalSection(&g_lock);

    char *warm = (char *)malloc(WARM_MAX + 1);
    char *body = (char *)malloc(PROBE_BYTES + 1);
    HttpsSession *session = HttpsSessionOpen(QuietPrint);
    if (warm != NULL && body != NULL && session != NULL)
    {
        // First, bare connects: the silent server, and two hosts that have
        // nothing to do with it or each other. All three quick, and the
        // packets are getting through - the server just isn't sending. Slow
        // to all three, and they're being lost near this console.
        static const char *const kOthers[2] = { "cachefly.cachefly.net", "raw.githubusercontent.com" };
        static const unsigned short kOtherPorts[2] = { 80, 443 };
        long dns[3], conn[3];
        TimeConnect(host, 443, &dns[0], &conn[0]);
        for (int i = 0; i < 2; ++i)
            TimeConnect(kOthers[i], kOtherPorts[i], &dns[i + 1], &conn[i + 1]);
        dprintf("[check] stall: connects in ms (lookup + connect, -1 failed) - %s %ld + %ld, CacheFly %ld + %ld, "
                "GitHub %ld + %ld\n",
                host, dns[0], conn[0], dns[1], conn[1], dns[2], conn[2]);

        // The connect is timed here, unlike the rate probe's: a lookup or a
        // handshake that hangs is an answer too.
        unsigned long long warmLen = WARM_MAX;
        const DWORD began = GetTickCount();
        const int warmStatus = HttpsSessionGet(session, WARM_URL, WARM_ACCEPT, warm, &warmLen);
        const DWORD connectMs = GetTickCount() - began;

        if (warmStatus != 200)
            dprintf("[check] stall: %s silent for %lus, and GitHub couldn't be reached either (HTTP %d after %.1fs) - "
                    "the console's network stopped too\n",
                    host, silentMs / 1000, warmStatus, connectMs / 1000.0);
        else
        {
            unsigned long long len = PROBE_BYTES;
            const DWORD from = GetTickCount();
            const int status = HttpsSessionGet(session, PROBE_URL, PROBE_ACCEPT, body, &len);
            DWORD took = GetTickCount() - from;
            if (took == 0)
                took = 1;
            if (status != 200 || len != PROBE_BYTES)
                dprintf("[check] stall: %s silent for %lus; GitHub connected in %.1fs but its megabyte didn't come "
                        "(HTTP %d, %I64u bytes in %.1fs) - the console's network stopped too\n",
                        host, silentMs / 1000, connectMs / 1000.0, status, len, took / 1000.0);
            else
                dprintf("[check] stall: %s silent for %lus; meanwhile GitHub connected in %.1fs and sent 1 MB in "
                        "%.1fs (%.2f MB/s)\n",
                        host, silentMs / 1000, connectMs / 1000.0, took / 1000.0,
                        len * 1000 / took / 1048576.0);
        }
    }
    HttpsSessionClose(session);
    free(warm);
    free(body);

    EnterCriticalSection(&g_lock);
    g_stallProbing = false;
    LeaveCriticalSection(&g_lock);
    return 0;
}

void ReportStall(const char *host, unsigned long silentMs)
{
    if (!g_ready)
        return;

    const DWORD now = GetTickCount();
    bool probe = false;
    EnterCriticalSection(&g_lock);
    if (!g_stallProbing && g_stallProbes < STALL_PROBES_MAX &&
        (g_stallProbes == 0 || now - g_lastStallProbe >= STALL_PROBE_EVERY_MS))
    {
        g_stallProbing = true;
        g_stallProbes++;
        g_lastStallProbe = now;
        _snprintf(g_stallHost, sizeof(g_stallHost), "%s", host != NULL ? host : "the server");
        g_stallHost[sizeof(g_stallHost) - 1] = '\0';
        g_stallSilentMs = silentMs;
        probe = true;
    }
    LeaveCriticalSection(&g_lock);
    if (!probe)
        return;

    HANDLE thread = CreateThread(NULL, 128 * 1024, StallProbeEntry, NULL, CREATE_SUSPENDED, NULL);
    if (thread == NULL)
    {
        EnterCriticalSection(&g_lock);
        g_stallProbing = false;
        LeaveCriticalSection(&g_lock);
        return;
    }
#ifdef _XBOX
    XSetThreadProcessor(thread, 5); // as the rate probe
#endif
    ResumeThread(thread);
    CloseHandle(thread);
}

static void StartProbe()
{
    HANDLE thread = CreateThread(NULL, 128 * 1024, ProbeEntry, NULL, CREATE_SUSPENDED, NULL);
    if (thread == NULL)
    {
        EnterCriticalSection(&g_lock);
        g_probing = false;
        LeaveCriticalSection(&g_lock);
        return;
    }
#ifdef _XBOX
    XSetThreadProcessor(thread, 5); // the housekeeping core, clear of the download threads
#endif
    ResumeThread(thread);
    CloseHandle(thread);
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void StartServiceCheck()
{
    if (g_ready)
        return;
    InitializeCriticalSection(&g_lock);
    g_ready = true;
}

void ReportConnectionRate(unsigned long long bytesPerSec)
{
    if (!g_ready)
        return;

    const DWORD now = GetTickCount();
    bool probe = false;

    EnterCriticalSection(&g_lock);
    if (bytesPerSec >= WORTH_ASKING)
    {
        // Something here is moving properly, so nothing on this page is true
        // any more - whatever the last probe decided.
        g_sawBad = false;
        g_worstSeen = 0;
        g_verdict[0] = '\0';
    }
    else
    {
        // Compare archive.org at its best against GitHub, not its worst.
        if (!g_sawBad || bytesPerSec > g_worstSeen)
            g_worstSeen = bytesPerSec;
        g_sawBad = true;
        if (!g_probing && (!g_everProbed || now - g_lastProbe >= PROBE_EVERY_MS))
        {
            g_probing = true;
            g_everProbed = true;
            g_lastProbe = now;
            probe = true;
        }
    }
    LeaveCriticalSection(&g_lock);

    if (probe)
        StartProbe();
}

void GetProbeNotice(char *out, size_t outSize)
{
    if (out == NULL || outSize == 0)
        return;
    out[0] = '\0';
    if (!g_ready)
        return;

    EnterCriticalSection(&g_lock);
    if (g_verdict[0] != '\0' && GetTickCount() - g_verdictAt < VERDICT_FOR_MS)
    {
        _snprintf(out, outSize, "%s", g_verdict);
        out[outSize - 1] = '\0';
    }
    LeaveCriticalSection(&g_lock);
}
