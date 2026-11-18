#ifndef CLOCK_SYNC_H
#define CLOCK_SYNC_H

// ---------------------------------------------------------------------------
// Setting the console's clock from the internet
// ---------------------------------------------------------------------------
//
// One SNTP question to pool.ntp.org over UDP, and the kernel's clock set
// from the answer. A console whose clock battery has died boots into 2005,
// and HTTPS checks certificates against that clock, so a wrong one fails
// every secure connection while plain HTTP still works - this is the fix
// for that without going into the dashboard's settings.
//
// The clock is UTC underneath; the time zone stays whatever the console's
// own settings say, so the time shown follows that as it always did.

enum ClockSyncResult
{
    CLOCK_SYNC_ALREADY_RIGHT, // within a couple of seconds - left alone
    CLOCK_SYNC_SET,           // was off, and has been set
    CLOCK_SYNC_NO_ANSWER,     // no address for the server, or no reply
    CLOCK_SYNC_REFUSED        // the kernel wouldn't take the new time
};

// Asks and sets, blocking for up to a few seconds. offsetSeconds, when not
// NULL, gets how far the clock was off: positive when it was behind.
ClockSyncResult SyncClock(long long *offsetSeconds);

// SyncClock on a thread of its own, for the check at start, so starting
// isn't held up by a server that doesn't answer. The result goes to the log.
void StartClockSync();

#endif
