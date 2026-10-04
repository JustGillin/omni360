/*
FILE : dns.cpp
PROJECT : Omni360
PROGRAMMER : 951261
DESCRIPTION : Resolves a domain name (example.com) to an IP (384.528.845.259)
*/

#include <xtl.h>
#include "XboxTLS.h"
#include "dns.h"
#include "OutputConsole.h"

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <iostream>

struct DnsCacheEntry
{
    const char *domain;
    const char *ip;
};

static const DnsCacheEntry dnsCache[] =
{
    // DNS fallback entries should use dotted IPv4 strings, e.g. "203.0.113.10".
    {"example.com", "0.0.0.0"}
};

/**
 * @brief Resolves a domain name to an IPv4 address string.
 *
 * @param domain The hostname to resolve (e.g., "example.com").
 * @param resolvedIP Output buffer to store the resolved IP address.
 * @param ipBufferSize Size of the resolvedIP buffer.
 * @return true if resolution succeeded, false otherwise.
 */
// Names the DNS server said don't exist, this session - DashLaunch's
// livestrong answers that way for every Xbox Live host. Asked about again,
// they fail straight away: each image would otherwise ask three times over,
// and an HTTPS request five, seconds apart. Only that answer is remembered -
// a timeout or any other failure is tried again.
#define MAX_MISSING_NAMES 16
static char g_missingNames[MAX_MISSING_NAMES][128];
static int g_missingCount = 0;
static CRITICAL_SECTION g_missingLock;
static volatile LONG g_missingLockState = 0; // 0 not made, 1 being made, 2 ready

// The lock, made by whichever thread gets here first - lookups come from
// several workers at once.
static CRITICAL_SECTION *MissingLock()
{
    if (g_missingLockState != 2)
    {
        if (InterlockedCompareExchange(&g_missingLockState, 1, 0) == 0)
        {
            InitializeCriticalSection(&g_missingLock);
            g_missingLockState = 2;
        }
        else
        {
            while (g_missingLockState != 2)
                Sleep(0);
        }
    }
    return &g_missingLock;
}

static bool KnownMissing(const char *domain)
{
    bool found = false;
    EnterCriticalSection(MissingLock());
    for (int i = 0; i < g_missingCount && !found; ++i)
        found = (_stricmp(g_missingNames[i], domain) == 0);
    LeaveCriticalSection(MissingLock());
    return found;
}

static void NoteMissing(const char *domain)
{
    EnterCriticalSection(MissingLock());
    bool found = false;
    for (int i = 0; i < g_missingCount && !found; ++i)
        found = (_stricmp(g_missingNames[i], domain) == 0);
    if (!found && g_missingCount < MAX_MISSING_NAMES && strlen(domain) < sizeof(g_missingNames[0]))
    {
        strcpy(g_missingNames[g_missingCount++], domain);
        dprintf("[dns] %s won't be looked up again this session - blocked, or no such name\n", domain);
    }
    LeaveCriticalSection(MissingLock());
}

bool ResolveDNS(const char *domain, char *resolvedIP, int ipBufferSize)
{
    XNDNS *pxndns = NULL;
    int lookupResult;
    int i;
    bool success = false;
    bool missing = false;

    if (domain == NULL || resolvedIP == NULL || ipBufferSize <= 0)
        return false;

    resolvedIP[0] = '\0';

    if (KnownMissing(domain))
        return false;

    // XNetDnsLookup or the returned XNDNS block can fault if the network stack
    // is not ready yet, so keep the whole interaction guarded and fail cleanly.
    __try
    {
        lookupResult = XNetDnsLookup(domain, NULL, &pxndns);
        if (lookupResult == 0 && pxndns != NULL)
        {
            // Wait up to ~5 seconds for DNS resolution.
            for (i = 0; i < 50; ++i)
            {
                if (pxndns->iStatus != WSAEINPROGRESS)
                    break;

                Sleep(100);
            }

            if (pxndns->iStatus == 0 && pxndns->cina > 0)
            {
                XNetInAddrToString(pxndns->aina[0], resolvedIP, ipBufferSize);
                success = (resolvedIP[0] != '\0');
            }
            else
            {
                // Why, for the log: no answer in time, no such name, or
                // something else from the console's DNS server.
                const int status = pxndns->iStatus;
                missing = (status == WSAHOST_NOT_FOUND);
                dprintf("[dns] %s: %s (status %d)\n", domain,
                        status == WSAEINPROGRESS ? "no answer in 5 seconds"
                        : status == WSAHOST_NOT_FOUND ? "the DNS server says there's no such name"
                        : status == 0 ? "no addresses came back"
                                      : "the lookup failed", status);
            }
        }
        else
        {
            dprintf("[dns] %s: the lookup couldn't start (%d) - is the network up?\n", domain, lookupResult);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (pxndns != NULL)
        {
            __try
            {
                XNetDnsRelease(pxndns);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        resolvedIP[0] = '\0';
        return false;
    }

    if (pxndns != NULL)
    {
        __try
        {
            XNetDnsRelease(pxndns);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    if (missing)
        NoteMissing(domain);
    return success;
}

int searchDnsCache(const char *domain, char *ip, int ipSize)
{
    int i;

    if (domain == NULL || ip == NULL || ipSize <= 0)
        return -1;

    for (i = 0; i < (int)(sizeof(dnsCache) / sizeof(dnsCache[0])); ++i)
    {
        if (strcmp(dnsCache[i].domain, domain) == 0)
        {
            const size_t cachedIpLength = strlen(dnsCache[i].ip);

            if ((int)(cachedIpLength + 1) > ipSize)
                return -1;

            strcpy(ip, dnsCache[i].ip);
            std::cout << "Resolved " << domain << " to IP " << ip << "\n";
            return 0;
        }
    }

    return -1; // failed to find DNS cache record
}

// ---------------------------------------------------------------------------
// The network, for the log
// ---------------------------------------------------------------------------

static DWORD WINAPI NetworkStatusEntry(LPVOID)
{
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&xnsp) != 0)
    {
        dprintf("[net] the network stack wouldn't start\n");
        return 0;
    }
    WSADATA wsadata;
    const bool wsa = (WSAStartup(MAKEWORD(2, 2), &wsadata) == 0);

    const DWORD link = XNetGetEthernetLinkStatus();
    if (link & XNET_ETHERNET_LINK_ACTIVE)
        dprintf("[net] link: up, %s%s%s\n", (link & XNET_ETHERNET_LINK_WIRELESS) ? "wireless" : "wired",
                (link & XNET_ETHERNET_LINK_100MBPS) ? ", 100 Mbps" : (link & XNET_ETHERNET_LINK_10MBPS) ? ", 10 Mbps" : "",
                (link & XNET_ETHERNET_LINK_FULL_DUPLEX) ? ", full duplex"
                : (link & XNET_ETHERNET_LINK_HALF_DUPLEX) ? ", half duplex" : "");
    else
        dprintf("[net] link: DOWN - no cable, or nothing at the other end (0x%08lX)\n", link);

    // An address can take a moment after starting; up to five seconds.
    XNADDR xna;
    memset(&xna, 0, sizeof(xna));
    DWORD flags = XNET_GET_XNADDR_PENDING;
    for (int i = 0; i < 50; ++i)
    {
        flags = XNetGetTitleXnAddr(&xna);
        if (flags != XNET_GET_XNADDR_PENDING)
            break;
        Sleep(100);
    }
    char ip[32] = "none";
    if (xna.ina.s_addr != 0)
        XNetInAddrToString(xna.ina, ip, sizeof(ip));
    if (flags == XNET_GET_XNADDR_PENDING)
        dprintf("[net] address: still waiting for one after 5 seconds\n");
    else
        dprintf("[net] address: %s (%s)%s%s%s\n", ip,
                (flags & XNET_GET_XNADDR_DHCP) ? "DHCP" : (flags & XNET_GET_XNADDR_STATIC) ? "static"
                : (flags & XNET_GET_XNADDR_PPPOE) ? "PPPoE" : "no IP",
                (flags & XNET_GET_XNADDR_GATEWAY) ? ", gateway" : ", NO gateway",
                (flags & XNET_GET_XNADDR_DNS) ? ", DNS servers" : ", NO DNS servers",
                (flags & XNET_GET_XNADDR_TROUBLESHOOT) ? ", needs troubleshooting" : "");

    // The clock HTTPS checks certificates against - BearSSL reads time().
    const time_t now = time(NULL);
    const struct tm *utc = gmtime(&now);
    if (utc != NULL)
        dprintf("[net] console clock (UTC): %04d-%02d-%02d %02d:%02d%s\n", utc->tm_year + 1900, utc->tm_mon + 1,
                utc->tm_mday, utc->tm_hour, utc->tm_min,
                utc->tm_year + 1900 < 2025 ? " - looks WRONG: secure connections will fail until it's set" : "");

    if (wsa)
        WSACleanup();
    XNetCleanup();
    return 0;
}

void LogNetworkStatus()
{
    HANDLE thread = CreateThread(NULL, 64 * 1024, NetworkStatusEntry, NULL, 0, NULL);
    if (thread != NULL)
        CloseHandle(thread);
    else
        dprintf("[net] couldn't start the network check\n");
}

// ---------------------------------------------------------------------------
// The network stack, for the whole session
// ---------------------------------------------------------------------------
//
// (XAuthStartup with XAUTH_FLAG_BYPASS_SECURITY - the XDK's way for a game
// to talk to servers outside Xbox Live - was tried here for a console that
// times out every connection. It fails on a modded console, 0x80158406, and
// XAuthInsecureSocketsAllowed says NO on one whose connections work, so
// neither tells anything; the bypass that matters is the exploit's.)

void StartNetwork()
{
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&xnsp) != 0)
    {
        dprintf("[net] the network stack wouldn't start\n");
        return;
    }
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
        dprintf("[net] Winsock wouldn't start\n");
}
