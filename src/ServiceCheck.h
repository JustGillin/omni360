#ifndef SERVICE_CHECK_H
#define SERVICE_CHECK_H

#include <stddef.h> // size_t

// ---------------------------------------------------------------------------
// Is it archive.org, or is it this console's line?
// ---------------------------------------------------------------------------
//
// A slow download on its own says nothing about whose fault it is. archive.org
// having a bad day, a tired router, somebody else in the house streaming - they
// all look the same from inside one download, and the one people reach for
// first is "Omni360 is broken".
//
// One measurement can't tell them apart. Two can: while a download crawls, we
// fetch a megabyte from GitHub, which the app already talks to and already
// trusts, and compare. GitHub quick and archive.org slow is archive.org.
// Both slow is the line into this console, and saying so is more useful than
// blaming a server that's fine.
//
// The verdict becomes the Queue page's notice when featured.json hasn't set
// one - that file still wins, because it can say things no probe can know.
//
// The cost is a megabyte, only during a download that's already slow, and at
// most once every ten minutes.
//
// Not to be confused with Settings' Speed test (SpeedTest.h), which someone
// asks for and which takes the console's whole network apart. This one runs
// by itself, answers one question, and only ever shows a sentence.

// Starts the state this needs. Call once at startup, next to StartFeatured.
void StartServiceCheck();

// What one connection managed over its last full window, in bytes a second.
// Called by the downloader as each window closes. A healthy rate clears any
// standing verdict; a poor one may set a probe going. Returns at once.
void ReportConnectionRate(unsigned long long bytesPerSec);

// The last probe's verdict, or "" if there is none or it has gone stale.
// Safe before StartServiceCheck.
void GetProbeNotice(char *out, size_t outSize);

#endif
