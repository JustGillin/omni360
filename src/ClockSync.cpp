/*
FILE : ClockSync.cpp
PROJECT : Omni360
DESCRIPTION : Sets the console's clock from pool.ntp.org - see ClockSync.h.
*/

#include "ClockSync.h"
#include "dns.h"
#include "OutputConsole.h"
#include <xtl.h>
#include <string.h>

// Not in any XDK header, but xboxkrnl.lib exports it, as it does
// HalSendSMCMessage. Takes the new time as a FILETIME (UTC, 100ns ticks since
// 1601) and hands back the old one.
extern "C" LONG NtSetSystemTime(PLARGE_INTEGER newTime, PLARGE_INTEGER oldTime);

#define XBOX_SO_BYPASS_SECURITY 0x5801

#define NTP_SERVER     "pool.ntp.org"
#define NTP_PORT       123
#define NTP_PACKET     48
#define NTP_WAIT_MS    3000
#define NTP_TRIES      2

// Seconds from 1601 (FILETIME's start) to 1900 (NTP's): 299 years, 72 of
// them leap years.
#define NTP_TO_FILETIME_SECONDS 9435484800ULL

// Closer than this and the clock is left alone - the answer has spent a
// network round trip getting here, so it isn't more right than that.
#define CLOCK_SYNC_SLACK_SECONDS 2

static unsigned long ReadBigEndian32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

// One question to the server; true with the server's transmit time as a
// FILETIME. Byte by byte rather than ntohl, which says nothing about the
// order on a console that is big-endian already.
static bool AskNtp(const char *ip, unsigned long long *fileTime)
{
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        dprintf("[clock] couldn't open a UDP socket: WSA error %d\n", WSAGetLastError());
        return false;
    }
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));

    sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(NTP_PORT);
    to.sin_addr.s_addr = inet_addr(ip);

    // Version 4, client mode, everything else zero.
    unsigned char packet[NTP_PACKET];
    memset(packet, 0, sizeof(packet));
    packet[0] = 0x23;

    bool ok = false;
    if (sendto(s, (const char *)packet, sizeof(packet), 0, (sockaddr *)&to, sizeof(to)) != SOCKET_ERROR)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        timeval wait;
        wait.tv_sec = NTP_WAIT_MS / 1000;
        wait.tv_usec = (NTP_WAIT_MS % 1000) * 1000;

        if (select(0, &readable, NULL, NULL, &wait) == 1)
        {
            unsigned char reply[NTP_PACKET];
            const int got = recv(s, (char *)reply, sizeof(reply), 0);
            const int mode = reply[0] & 0x07;
            const int stratum = reply[1];
            // A server answers in mode 4; stratum 0 is a "kiss of death"
            // telling the client to go away, not a time.
            if (got >= NTP_PACKET && mode == 4 && stratum != 0)
            {
                const unsigned long seconds = ReadBigEndian32(reply + 40);
                const unsigned long fraction = ReadBigEndian32(reply + 44);
                if (seconds != 0)
                {
                    *fileTime = ((unsigned long long)seconds + NTP_TO_FILETIME_SECONDS) * 10000000ULL +
                                (((unsigned long long)fraction * 10000000ULL) >> 32);
                    ok = true;
                }
            }
            else
                dprintf("[clock] odd reply from %s: %d bytes, mode %d, stratum %d\n", ip, got, mode, stratum);
        }
        else
            dprintf("[clock] no reply from %s within %d ms\n", ip, NTP_WAIT_MS);
    }
    else
        dprintf("[clock] couldn't send to %s: WSA error %d\n", ip, WSAGetLastError());

    closesocket(s);
    return ok;
}

ClockSyncResult SyncClock(long long *offsetSeconds)
{
    if (offsetSeconds != NULL)
        *offsetSeconds = 0;

    unsigned long long serverTime = 0;
    bool answered = false;
    // The pool hands out a different server each lookup, so a second try
    // is a second server rather than the same silent one again.
    for (int attempt = 0; attempt < NTP_TRIES && !answered; ++attempt)
    {
        char ip[32];
        if (!ResolveDNS(NTP_SERVER, ip, sizeof(ip)))
        {
            dprintf("[clock] no address for %s\n", NTP_SERVER);
            continue;
        }
        answered = AskNtp(ip, &serverTime);
    }
    if (!answered)
        return CLOCK_SYNC_NO_ANSWER;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const unsigned long long ours = ((unsigned long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    const long long offset = ((long long)serverTime - (long long)ours) / 10000000LL;
    if (offsetSeconds != NULL)
        *offsetSeconds = offset;

    if (offset <= CLOCK_SYNC_SLACK_SECONDS && offset >= -CLOCK_SYNC_SLACK_SECONDS)
    {
        dprintf("[clock] already right (off by %lld s)\n", offset);
        return CLOCK_SYNC_ALREADY_RIGHT;
    }

    LARGE_INTEGER newTime, oldTime;
    newTime.QuadPart = (LONGLONG)serverTime;
    oldTime.QuadPart = 0;
    const LONG status = NtSetSystemTime(&newTime, &oldTime);
    if (status < 0)
    {
        dprintf("[clock] the kernel refused the new time: status %08lX\n", (unsigned long)status);
        return CLOCK_SYNC_REFUSED;
    }

    dprintf("[clock] set from %s - it was off by %lld s\n", NTP_SERVER, offset);
    return CLOCK_SYNC_SET;
}

static DWORD WINAPI ClockSyncThread(LPVOID)
{
    SyncClock(NULL);
    return 0;
}

void StartClockSync()
{
    HANDLE thread = CreateThread(NULL, 0, ClockSyncThread, NULL, 0, NULL);
    // The housekeeping core, clear of the UI's core 0.
    if (thread != NULL)
        XSetThreadProcessor(thread, 5);
    if (thread != NULL)
        CloseHandle(thread);
    else
        dprintf("[clock] couldn't start the clock check\n");
}
