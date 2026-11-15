#include "PcHelper.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELPER_PORT        36060
#define HELPER_FRESH_MS    15000 // it announces every 3 seconds
#define XBOX_SO_BYPASS_SECURITY 0x5801

static CRITICAL_SECTION g_helperLock;
static char g_helperUrl[64];
static char g_helperName[64];
static DWORD g_heardAt = 0;
static bool g_heardEver = false;

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

        int version = 0, port = 0;
        if (sscanf(buf, "OMNI360-HELPER %d %d", &version, &port) != 2 || version != 1 ||
            port <= 0 || port > 65535)
            continue;
        // The PC's name is everything after the third space (the CRT's %n
        // left it empty).
        const char *pcName = buf;
        for (int spaces = 0; spaces < 3 && pcName != NULL; ++spaces)
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
        const bool changed = !g_heardEver || strcmp(url, g_helperUrl) != 0 ||
                             GetTickCount() - g_heardAt > HELPER_FRESH_MS;
        strcpy(g_helperUrl, url);
        strncpy(g_helperName, pcName != NULL ? pcName : "", sizeof(g_helperName) - 1);
        g_helperName[sizeof(g_helperName) - 1] = '\0';
        g_heardAt = GetTickCount();
        g_heardEver = true;
        LeaveCriticalSection(&g_helperLock);
        if (changed)
            dprintf("[helper] PC helper \"%s\" at %s\n", g_helperName, url);
    }
}

void StartPcHelperListener()
{
    InitializeCriticalSection(&g_helperLock);
    HANDLE h = CreateThread(NULL, 0, ListenEntry, NULL, 0, NULL);
    if (h != NULL)
        CloseHandle(h);
}

bool PcHelperUrl(char *url, unsigned int urlSize, char *name, unsigned int nameSize)
{
    EnterCriticalSection(&g_helperLock);
    const bool fresh = g_heardEver && GetTickCount() - g_heardAt <= HELPER_FRESH_MS;
    if (fresh)
    {
        if (url != NULL && urlSize > 0)
        {
            strncpy(url, g_helperUrl, urlSize - 1);
            url[urlSize - 1] = '\0';
        }
        if (name != NULL && nameSize > 0)
        {
            strncpy(name, g_helperName, nameSize - 1);
            name[nameSize - 1] = '\0';
        }
    }
    LeaveCriticalSection(&g_helperLock);
    return fresh;
}
