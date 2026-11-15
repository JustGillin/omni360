#include "PcHelper.h"
#include "HttpPlain.h"
#include "OutputConsole.h"

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
        const bool paired = strcmp(id, g_pairedId) == 0;
        LeaveCriticalSection(&g_helperLock);
        if (changed)
            dprintf("[helper] PC helper \"%s\" at %s (%s)\n", pcName != NULL ? pcName : "", url,
                    paired ? "paired" : "not paired");
    }
}

void StartPcHelperListener()
{
    EnsureLock();
    HANDLE h = CreateThread(NULL, 0, ListenEntry, NULL, 0, NULL);
    if (h != NULL)
        CloseHandle(h);
}

void SetPcHelperPairing(const char *id, const char *token, bool enabled)
{
    EnsureLock();
    EnterCriticalSection(&g_helperLock);
    CopyOut(g_pairedId, sizeof(g_pairedId), id != NULL ? id : "");
    CopyOut(g_pairedToken, sizeof(g_pairedToken), token != NULL ? token : "");
    g_enabled = enabled;
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

bool PcHelperPair(const char *code, char *token, unsigned int tokenSize, char *id, unsigned int idSize,
                  char *message, unsigned int messageSize)
{
    EnsureLock();
    char base[64];
    EnterCriticalSection(&g_helperLock);
    const bool fresh = FreshLocked();
    CopyOut(base, sizeof(base), g_heardUrl);
    LeaveCriticalSection(&g_helperLock);
    if (!fresh)
    {
        CopyOut(message, messageSize, "The helper isn't running on your PC - start it and try again.");
        return false;
    }

    // Digits only, so it goes in the URL as it is.
    char digits[8];
    int n = 0;
    for (const char *c = code; *c != '\0' && n < 7; ++c)
    {
        if (*c >= '0' && *c <= '9')
            digits[n++] = *c;
    }
    digits[n] = '\0';

    char url[128];
    _snprintf(url, sizeof(url), "%s/pair?code=%s", base, digits);
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
        return true;
    }
    if (status == 403)
        CopyOut(message, messageSize, "That code didn't match. Check the code in the helper's window.");
    else
        CopyOut(message, messageSize, "Couldn't reach the helper. Check your PC's firewall allows it.");
    dprintf("[helper] pairing failed: HTTP %d\n", status);
    return false;
}
