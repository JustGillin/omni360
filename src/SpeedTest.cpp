/*
FILE : SpeedTest.cpp
PROJECT : Omni360
DESCRIPTION : Settings' Speed test - see SpeedTest.h.

The measuring runs on a thread of its own, on a connection's core (hardware
thread 4), while the UI thread draws its progress. Every transfer counts its
bytes into a slot; a measurement leaves out the first moments (connecting,
TLS handshakes) and times the bytes after, over MEASURE_MS.
*/

#include <xtl.h> // first: GameListUI.h uses its types
#include <winsockx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "SpeedTest.h"
#include "GameListUI.h"   // RenderStatusFrame, ShowNotesUI, PollUiInput
#include "UiSound.h"
#include "downloadFile.h" // httpRequestHTTPS
#include "dns.h"
#include "OutputConsole.h"
#include "XboxTLS.h"       // XboxTLS_SetThreadSuites
#include "inc\bearssl.h"

#define WARMUP_MAX_MS   5000  // waiting for every connection's first byte, at most
#define MEASURE_MS      10000
#define DECRYPT_MS      2000  // per cipher
#define MAX_SLOTS       3
#define PAUSE_MS        3000  // between archive.org runs, so one's connections are gone before the next's
#define RECV_CHUNK      (64 * 1024)
#define RECV_BUFFER     (4 * 1024 * 1024) // as XboxTLS asks for, so the two compare
#define XBOX_SO_BYPASS_SECURITY 0x5801

// CacheFly's test file is on a CDN, so it's near wherever the console is;
// thinkbroadband's (in the UK) is the fallback. Both serve plain HTTP - and
// archive.org doesn't, its servers send everything to HTTPS.
static const char *const kPlainHosts[] = { "cachefly.cachefly.net", "ipv4.download.thinkbroadband.com" };
static const char *const kPlainPaths[] = { "/100mb.test", "/1GB.zip" };

// Deadpool's RAR: 8.4GB, so no connection reaches its end in 10 seconds.
#define ARCHIVE_TEST_URL "https://archive.org/download/XBOX_360_1/Deadpool%20%5BRF%5D.rar"
#define ARCHIVE_SLOT_SPAN (1024ULL * 1024 * 1024) // each connection's part of it

// The archive.org runs: one connection with the cipher the server picks
// (AES-GCM, from archive.org's storage servers) and with ChaCha20 forced -
// which shows whether decrypting is what holds a connection back - then 2
// and 3 connections as downloads now connect, ChaCha20 for archive.org.
enum ArchiveRun
{
    RUN_ONE_SERVER_CHOICE,
    RUN_ONE_CHACHA,
    RUN_TWO,
    RUN_THREE,
    RUN_COUNT
};
static const int kRunConnections[RUN_COUNT] = { 1, 1, 2, 3 };
static const int kRunSuites[RUN_COUNT] = { XBOXTLS_SUITES_SERVER_CHOICE, XBOXTLS_SUITES_CHACHA,
                                           XBOXTLS_SUITES_DEFAULT, XBOXTLS_SUITES_DEFAULT };

struct SpeedResults
{
    double plain;           // MB/s; negative: failed
    const char *plainHost;
    double chacha, gcm;     // MB/s on one core; negative: not run
    double archive[RUN_COUNT];       // MB/s; negative: not run or failed
    unsigned short suite[RUN_COUNT]; // what the first connection settled on
};

static SpeedResults g_results;
static CRITICAL_SECTION g_lock;
static char g_phase[96];
static int g_step, g_steps;
static double g_live;              // MB/s over the last half second
static volatile long g_cancel;     // B on the progress screen
static volatile long g_finished;
static volatile long g_stopTransfers;
static volatile unsigned long long g_slotBytes[MAX_SLOTS];
static int g_slotStatus[MAX_SLOTS];            // how each archive.org connection's request ended
static unsigned short g_slotSuite[MAX_SLOTS];  // and the cipher suite it used
static int g_archiveSuites;                    // XBOXTLS_SUITES_* for the run
static const char *g_auth;
static char g_scratchDrive[16];

static const double MB = 1024.0 * 1024.0;

static void QuietPrint(const char *format, ...)
{
}

static void SetPhase(const char *phase)
{
    EnterCriticalSection(&g_lock);
    _snprintf(g_phase, sizeof(g_phase), "%s", phase);
    g_phase[sizeof(g_phase) - 1] = '\0';
    g_step++;
    g_live = 0.0;
    LeaveCriticalSection(&g_lock);
    dprintf("[speedtest] %s\n", phase);
}

static bool Cancelled()
{
    return InterlockedCompareExchange(&g_cancel, 0, 0) != 0;
}

static unsigned long long SlotTotal(int slots)
{
    unsigned long long total = 0;
    for (int i = 0; i < slots; ++i)
        total += g_slotBytes[i];
    return total;
}

// The transfers are running: waits for every slot's first byte (or
// WARMUP_MAX_MS), then times MEASURE_MS. Returns MB/s, or -1 if nothing came.
static double Measure(int slots)
{
    const DWORD start = GetTickCount();
    for (;;)
    {
        bool all = true;
        for (int i = 0; i < slots; ++i)
            all = all && g_slotBytes[i] > 0;
        if (all || GetTickCount() - start >= WARMUP_MAX_MS || Cancelled())
            break;
        Sleep(50);
    }

    const DWORD from = GetTickCount();
    const unsigned long long base = SlotTotal(slots);
    unsigned long long lastBytes = base;
    DWORD lastAt = from;
    while (GetTickCount() - from < MEASURE_MS && !Cancelled())
    {
        Sleep(500);
        const DWORD now = GetTickCount();
        const unsigned long long bytes = SlotTotal(slots);
        EnterCriticalSection(&g_lock);
        g_live = (now > lastAt) ? (bytes - lastBytes) / MB * 1000.0 / (now - lastAt) : 0.0;
        LeaveCriticalSection(&g_lock);
        lastBytes = bytes;
        lastAt = now;
    }

    const DWORD ms = GetTickCount() - from;
    const unsigned long long got = SlotTotal(slots) - base;
    InterlockedExchange(&g_stopTransfers, 1);
    if (got == 0 || ms == 0)
        return -1.0;
    return got / MB * 1000.0 / ms;
}

// ---------------------------------------------------------------------------
// 1. The network, with no encryption
// ---------------------------------------------------------------------------

// Connects to host:80. INVALID_SOCKET if it can't.
static SOCKET ConnectPlain(const char *host)
{
    char ip[64] = "";
    if (!ResolveDNS(host, ip, sizeof(ip)) && searchDnsCache(host, ip, sizeof(ip)) != 0)
        return INVALID_SOCKET;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return s;

    BOOL yes = TRUE;
    int recvBuffer = RECV_BUFFER;
    DWORD timeoutMs = 5000;
    setsockopt(s, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (PCSTR)&recvBuffer, sizeof(recvBuffer));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (PCSTR)&timeoutMs, sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (PCSTR)&timeoutMs, sizeof(timeoutMs));

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(80);
    sa.sin_addr.s_addr = inet_addr(ip);
    if (connect(s, (sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

struct PlainArgs
{
    int host; // into kPlainHosts
    bool failed;
};

// Fetches the file over and over until told to stop - the console may get
// through 100MB in under 10 seconds.
static DWORD WINAPI PlainEntry(LPVOID param)
{
    PlainArgs *a = (PlainArgs *)param;
    char *buffer = (char *)malloc(RECV_CHUNK);
    int failures = 0;

    while (buffer != NULL && !InterlockedCompareExchange(&g_stopTransfers, 0, 0) && failures < 3)
    {
        SOCKET s = ConnectPlain(kPlainHosts[a->host]);
        if (s == INVALID_SOCKET)
        {
            failures++;
            Sleep(300);
            continue;
        }

        char request[256];
        int len = _snprintf(request, sizeof(request),
                            "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Omni360\r\nConnection: close\r\n\r\n",
                            kPlainPaths[a->host], kPlainHosts[a->host]);
        bool any = false;
        if (len > 0 && send(s, request, len, 0) == len)
        {
            int got;
            while (!InterlockedCompareExchange(&g_stopTransfers, 0, 0) && (got = recv(s, buffer, RECV_CHUNK, 0)) > 0)
            {
                g_slotBytes[0] += (unsigned long long)got;
                any = true;
            }
        }
        closesocket(s);
        if (!any)
            failures++;
    }

    a->failed = (failures >= 3 && g_slotBytes[0] == 0);
    free(buffer);
    return 0;
}

static double TestPlain(const char **outHost)
{
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&xnsp) != 0)
        return -1.0;
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
    {
        XNetCleanup();
        return -1.0;
    }

    double rate = -1.0;
    for (int host = 0; host < 2 && rate < 0.0 && !Cancelled(); ++host)
    {
        g_slotBytes[0] = 0;
        InterlockedExchange(&g_stopTransfers, 0);
        PlainArgs args = { host, false };
        HANDLE t = CreateThread(NULL, 64 * 1024, PlainEntry, &args, CREATE_SUSPENDED, NULL);
        if (t == NULL)
            break;
        XSetThreadProcessor(t, 1); // a download connection's core
        ResumeThread(t);
        rate = Measure(1);
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        if (rate >= 0.0)
            *outHost = kPlainHosts[host];
        else
            dprintf("[speedtest] %s didn't answer\n", kPlainHosts[host]);
    }

    WSACleanup();
    XNetCleanup();
    return rate;
}

// ---------------------------------------------------------------------------
// 2. Decrypting, on one core
// ---------------------------------------------------------------------------

// Big, so not on the stack: only read for the implementations it picks.
static br_ssl_client_context g_sc;
static br_x509_minimal_context g_xc;

// A 16KB record at a time - TLS's largest - decrypted with the AEADs the
// download's TLS uses, as BearSSL's full client setup picks them.
static void TestDecrypt(double *outChacha, double *outGcm)
{
    *outChacha = *outGcm = -1.0;
    br_ssl_client_init_full(&g_sc, &g_xc, NULL, 0);

    const size_t RECORD = 16384;
    unsigned char *data = (unsigned char *)calloc(1, RECORD);
    if (data == NULL)
        return;
    unsigned char key[32] = {1}, iv[12] = {2}, aad[13] = {3}, tag[16];

    const br_chacha20_run ichacha = g_sc.eng.ichacha;
    const br_poly1305_run ipoly = g_sc.eng.ipoly;
    if (ichacha != NULL && ipoly != NULL && !Cancelled())
    {
        unsigned long long bytes = 0;
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < DECRYPT_MS)
        {
            ipoly(key, iv, data, RECORD, aad, sizeof(aad), tag, ichacha, 0);
            bytes += RECORD;
        }
        *outChacha = bytes / MB * 1000.0 / (GetTickCount() - start);
    }

    const br_block_ctr_class *iaes = g_sc.eng.iaes_ctr;
    const br_ghash ighash = g_sc.eng.ighash;
    void *aes = (iaes != NULL) ? malloc(iaes->context_size) : NULL;
    if (aes != NULL && ighash != NULL && !Cancelled())
    {
        iaes->init((const br_block_ctr_class **)aes, key, 16);
        br_gcm_context gc;
        br_gcm_init(&gc, (const br_block_ctr_class **)aes, ighash);
        unsigned long long bytes = 0;
        const DWORD start = GetTickCount();
        while (GetTickCount() - start < DECRYPT_MS)
        {
            br_gcm_reset(&gc, iv, sizeof(iv));
            br_gcm_aad_inject(&gc, aad, sizeof(aad));
            br_gcm_flip(&gc);
            br_gcm_run(&gc, 0, data, RECORD);
            br_gcm_get_tag(&gc, tag);
            bytes += RECORD;
        }
        *outGcm = bytes / MB * 1000.0 / (GetTickCount() - start);
    }
    free(aes);
    free(data);
}

// ---------------------------------------------------------------------------
// 3. archive.org, over HTTPS
// ---------------------------------------------------------------------------

static std::string g_archiveUrl; // the storage server's, the redirect followed once

// httpRequestHTTPS's progress has no context, so one function per slot.
static bool SlotProgress(int slot, unsigned long long done)
{
    g_slotBytes[slot] = done;
    return !InterlockedCompareExchange(&g_stopTransfers, 0, 0);
}
static bool SlotProgress0(unsigned long long d, unsigned long long, unsigned long long, unsigned long long) { return SlotProgress(0, d); }
static bool SlotProgress1(unsigned long long d, unsigned long long, unsigned long long, unsigned long long) { return SlotProgress(1, d); }
static bool SlotProgress2(unsigned long long d, unsigned long long, unsigned long long, unsigned long long) { return SlotProgress(2, d); }
static const DownloadProgressFn kSlotProgress[MAX_SLOTS] = { SlotProgress0, SlotProgress1, SlotProgress2 };

static void ScratchPath(int slot, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s\\Omni360-partial\\speedtest%d.bin", g_scratchDrive, slot);
    out[outSize - 1] = '\0';
}

static DWORD WINAPI ArchiveEntry(LPVOID param)
{
    const int slot = (int)(INT_PTR)param;
    XboxTLS_SetThreadSuites(g_archiveSuites);
    char headers[256];
    const unsigned long long from = slot * ARCHIVE_SLOT_SPAN;
    _snprintf(headers, sizeof(headers), "%sRange: bytes=%I64u-%I64u\r\n", g_auth, from, from + ARCHIVE_SLOT_SPAN - 1);
    headers[sizeof(headers) - 1] = '\0';

    char path[64];
    ScratchPath(slot, path, sizeof(path));
    g_slotStatus[slot] = httpRequestHTTPS(g_archiveUrl, HTTP_GET, NULL, headers, path, NULL, NULL, true, NULL, 0,
                                          QuietPrint, ARCHIVE_SLOT_SPAN, kSlotProgress[slot]);
    g_slotSuite[slot] = XboxTLS_LastSuiteForThisThread();
    DeleteFileA(path);
    return 0;
}

static const char *SuiteName(unsigned short suite)
{
    switch (suite)
    {
    case BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256:
    case BR_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256:
        return "ChaCha20";
    case BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
    case BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
    case BR_TLS_RSA_WITH_AES_128_GCM_SHA256:
        return "AES-GCM";
    case BR_TLS_RSA_WITH_AES_128_CBC_SHA:
        return "AES-CBC";
    case 0:
        return "no handshake";
    default:
        return "another suite";
    }
}

// How a connection's request ended: stopped by the test is the good ending.
// A 206 short of its range is the server closing it partway - the HTTP
// client reports the status it was answered with, and the body ending early
// only as "Download has likely failed".
static const char *EndingOf(int status, unsigned long long bytes)
{
    if (status == HTTP_STATUS_CANCELLED)
        return "ran until the test stopped it";
    if ((status == 206 || status == 200) && bytes >= ARCHIVE_SLOT_SPAN)
        return "got its whole range";
    return "dropped by the server";
}

// The storage server the file is on, so the connections go straight there.
static bool ResolveArchiveUrl()
{
    char redirect[2048] = "";
    unsigned long long len = sizeof(redirect) - 1;
    char headers[256];
    _snprintf(headers, sizeof(headers), "%sRange: bytes=0-0\r\n", g_auth);
    headers[sizeof(headers) - 1] = '\0';
    const int status = httpRequestHTTPS(ARCHIVE_TEST_URL, HTTP_GET, NULL, headers, "", redirect, &len, false,
                                        NULL, 0, QuietPrint);
    if (status == 302 && redirect[0] != '\0')
        g_archiveUrl = redirect;
    else if (status == 206 || status == 200)
        g_archiveUrl = ARCHIVE_TEST_URL;
    else
    {
        dprintf("[speedtest] archive.org answered HTTP %d\n", status);
        return false;
    }
    return true;
}

static double TestArchive(int connections, int suites, unsigned short *outSuite)
{
    static const DWORD kCores[MAX_SLOTS] = { 1, 4, 2 }; // the installer's two, then the spare core

    g_archiveSuites = suites;
    InterlockedExchange(&g_stopTransfers, 0);
    HANDLE threads[MAX_SLOTS];
    int started = 0;
    for (int i = 0; i < connections; ++i)
    {
        g_slotBytes[i] = 0;
        g_slotStatus[i] = 0;
        g_slotSuite[i] = 0;
        threads[i] = CreateThread(NULL, 256 * 1024, ArchiveEntry, (LPVOID)(INT_PTR)i, CREATE_SUSPENDED, NULL);
        if (threads[i] == NULL)
            break;
        XSetThreadProcessor(threads[i], kCores[i]);
        ResumeThread(threads[i]);
        started++;
    }
    if (started < connections)
    {
        InterlockedExchange(&g_stopTransfers, 1);
        WaitForMultipleObjects(started, threads, TRUE, INFINITE);
        for (int i = 0; i < started; ++i)
            CloseHandle(threads[i]);
        return -1.0;
    }

    const double rate = Measure(connections);
    WaitForMultipleObjects(started, threads, TRUE, INFINITE);
    for (int i = 0; i < started; ++i)
    {
        CloseHandle(threads[i]);
        dprintf("[speedtest]   connection %d: %.1f MB, %s, %s (HTTP %d)\n", i + 1, g_slotBytes[i] / MB,
                SuiteName(g_slotSuite[i]), EndingOf(g_slotStatus[i], g_slotBytes[i]), g_slotStatus[i]);
    }
    *outSuite = g_slotSuite[0];
    return rate;
}

// ---------------------------------------------------------------------------
// The run, and its screens
// ---------------------------------------------------------------------------

static DWORD WINAPI SpeedTestEntry(LPVOID)
{
    SetPhase("Checking your internet connection");
    g_results.plain = TestPlain(&g_results.plainHost);
    dprintf("[speedtest] network, no encryption: %.2f MB/s (%s)\n", g_results.plain,
            g_results.plainHost != NULL ? g_results.plainHost : "no server answered");

    if (!Cancelled())
    {
        SetPhase("Checking your console");
        TestDecrypt(&g_results.chacha, &g_results.gcm);
        dprintf("[speedtest] decrypting on one core: ChaCha20-Poly1305 %.2f MB/s, AES-128-GCM %.2f MB/s\n",
                g_results.chacha, g_results.gcm);
    }

    if (g_auth != NULL && !Cancelled() && ResolveArchiveUrl())
    {
        for (int run = 0; run < RUN_COUNT && !Cancelled(); ++run)
        {
            // On screen, just which of the runs; the log says what each one is.
            const int n = kRunConnections[run];
            char phase[96], what[96];
            _snprintf(phase, sizeof(phase), "Checking archive.org (%d of %d)", run + 1, RUN_COUNT);
            phase[sizeof(phase) - 1] = '\0';
            if (run == RUN_ONE_SERVER_CHOICE)
                _snprintf(what, sizeof(what), "archive.org, 1 connection, its choice of cipher");
            else if (run == RUN_ONE_CHACHA)
                _snprintf(what, sizeof(what), "archive.org, 1 connection, ChaCha20");
            else
                _snprintf(what, sizeof(what), "archive.org with %d connections", n);
            what[sizeof(what) - 1] = '\0';
            SetPhase(phase);

            // The last run's connections well gone first.
            for (DWORD waited = 0; waited < PAUSE_MS && !Cancelled(); waited += 100)
                Sleep(100);
            if (Cancelled())
                break;

            g_results.archive[run] = TestArchive(n, kRunSuites[run], &g_results.suite[run]);
            dprintf("[speedtest] %s: %.2f MB/s (%s)\n", what, g_results.archive[run],
                    SuiteName(g_results.suite[run]));
        }
    }

    InterlockedExchange(&g_finished, 1);
    return 0;
}

static void AppendRate(std::string &out, const char *label, double rate)
{
    char line[128];
    if (rate >= 0.0)
        _snprintf(line, sizeof(line), "%s: %.1f MB/s\n", label, rate);
    else
        _snprintf(line, sizeof(line), "%s: couldn't be checked\n", label);
    line[sizeof(line) - 1] = '\0';
    out += line;
}

// The fastest archive.org run, or -1 if none worked.
static double BestArchive(const SpeedResults &r)
{
    double best = -1.0;
    for (int i = 0; i < RUN_COUNT; ++i)
        if (r.archive[i] > best)
            best = r.archive[i];
    return best;
}

// What the numbers say, for anyone: which of the three is the slowest. The
// console's figure is ChaCha20's, which archive.org downloads now use.
static std::string Verdict(const SpeedResults &r)
{
    const double archive = BestArchive(r);
    if (archive < 0.0)
        return g_auth == NULL ? "Add your archive.org keys in Settings to check archive.org too."
                              : "archive.org couldn't be reached. Try again later.";

    const double internet = r.plain, console = r.chacha;
    double others = -1.0; // the slower of your internet and console that were measured
    if (internet > 0.0)
        others = internet;
    if (console > 0.0 && (others < 0.0 || console < others))
        others = console;

    if (others < 0.0 || archive < others * 0.8)
        return "archive.org is the slowest part: it only sends so fast to each home. Your console and internet "
               "can go faster, so downloads can't get any quicker than this.";
    if (internet > 0.0 && internet <= console)
        return "Your internet connection is the slowest part: downloads can't get quicker than it.";
    return "Your console is the slowest part: it can't keep up with any more than this.";
}

void RunSpeedTestUI(const char *authHeader, const char *scratchPath)
{
    static bool initialized = false;
    if (!initialized)
    {
        InitializeCriticalSection(&g_lock);
        initialized = true;
    }

    memset(&g_results, 0, sizeof(g_results));
    g_results.plain = g_results.chacha = g_results.gcm = -1.0;
    for (int i = 0; i < RUN_COUNT; ++i)
        g_results.archive[i] = -1.0;
    g_auth = authHeader;
    g_phase[0] = '\0';
    g_step = 0;
    g_steps = (authHeader != NULL) ? 2 + RUN_COUNT : 2;
    g_live = 0.0;
    g_cancel = 0;
    g_finished = 0;

    // The drive the downloads go to, for the archive.org test's data.
    const char *colon = (scratchPath != NULL) ? strchr(scratchPath, ':') : NULL;
    size_t driveLen = (colon != NULL) ? (size_t)(colon - scratchPath + 1) : 0;
    if (driveLen == 0 || driveLen >= sizeof(g_scratchDrive))
    {
        strcpy(g_scratchDrive, "Hdd1:");
    }
    else
    {
        memcpy(g_scratchDrive, scratchPath, driveLen);
        g_scratchDrive[driveLen] = '\0';
    }
    char staging[32];
    _snprintf(staging, sizeof(staging), "%s\\Omni360-partial", g_scratchDrive);
    staging[sizeof(staging) - 1] = '\0';
    CreateDirectoryA(staging, NULL); // fine if it's there

    dprintf("[speedtest] starting (%s)\n", authHeader != NULL ? "with archive.org" : "no keys - archive.org left out");

    HANDLE worker = CreateThread(NULL, 256 * 1024, SpeedTestEntry, NULL, CREATE_SUSPENDED, NULL);
    if (worker == NULL)
    {
        ShowNotesUI("Speed test", "Couldn't start", "The test couldn't be started.", NULL, NULL);
        return;
    }
    XSetThreadProcessor(worker, 4); // a download connection's core, for the decryption test
    ResumeThread(worker);

    ResyncUiInput();
    while (!InterlockedCompareExchange(&g_finished, 0, 0))
    {
        const UiInput input = PollUiInput();
        if ((input.pressed & XINPUT_GAMEPAD_B) && !Cancelled())
        {
            PlayUiSound(UI_SOUND_BACK);
            InterlockedExchange(&g_cancel, 1);
        }

        char phase[96], detail[96];
        EnterCriticalSection(&g_lock);
        _snprintf(phase, sizeof(phase), "%s", g_phase[0] != '\0' ? g_phase : "Starting");
        if (Cancelled())
            _snprintf(detail, sizeof(detail), "Stopping...");
        else if (g_live > 0.0)
            _snprintf(detail, sizeof(detail), "Step %d of %d  -  %.1f MB/s", g_step, g_steps, g_live);
        else
            _snprintf(detail, sizeof(detail), "Step %d of %d  -  B stops it", g_step, g_steps);
        LeaveCriticalSection(&g_lock);
        phase[sizeof(phase) - 1] = '\0';
        detail[sizeof(detail) - 1] = '\0';

        RenderStatusFrame("Speed test", phase, detail);
        Sleep(16);
    }
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
    ResyncUiInput();

    if (Cancelled())
    {
        dprintf("[speedtest] stopped\n");
        return;
    }

    // Three numbers anyone can read; the log has every run, and the ciphers.
    std::string body;
    AppendRate(body, "Your internet connection", g_results.plain);
    AppendRate(body, "What your console can keep up with", g_results.chacha);
    if (authHeader != NULL)
        AppendRate(body, "archive.org", BestArchive(g_results));
    const std::string verdict = Verdict(g_results);
    body += "\n";
    body += verdict;
    dprintf("[speedtest] %s\n", verdict.c_str());

    ShowNotesUI("Speed test", "Results", body.c_str(), "The details are in DebugInfo.txt", NULL);
}
