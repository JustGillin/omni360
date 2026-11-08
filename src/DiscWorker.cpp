/*
FILE : DiscWorker.cpp
PROJECT : Omni360
DESCRIPTION : Watches the disc drive and installs discs as Games on Demand, on
              a worker thread. See DiscWorker.h.
*/

#include "DiscWorker.h"
#include "DiscSource.h"
#include "GodConvert.h"
#include "ReadAhead.h"
#include "ArchiveOrgDLC.h" // DriveFreeSpace
#include "downloadFile.h"  // FormatBytes
#include "StoreArt.h"      // FetchTitleIcon
#include "QueueState.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The system controller, through the kernel. Command 0x0A asks for the disc
// tray's state, answered in the second byte of the reply. The XDK's
// xboxkrnl.lib exports it, though no header declares it.
extern "C" VOID HalSendSMCMessage(LPVOID input, LPVOID output);

// XAM's drive speed control, exported by the XDK's xapilib.lib but declared
// in no header; one DWORD, by community headers. What the values do isn't
// documented anywhere reliable, so it was measured on hardware - 48MB read
// from the inner edge of a game partition after each:
//
//   nothing set, 0, 1   about 1.6 MB/s - the drive's quiet range
//   2                   2.99 MB/s
//   3                   4.89 MB/s
//   4                   6.59 MB/s - the XDK's figure for the inner edge at
//                       full speed is 6.8 MB/s, so this is the top
//
// Left alone, a raw read of a game disc runs in that quiet range, which is
// why installs averaged 2.3 MB/s - slower than the XDK's 2.4x minimum for a
// game disc, and a third of Aurora's Disc to GOD.
extern "C" DWORD XamSetDvdSpindleSpeed(DWORD speed);

#define SPINDLE_SPEED_NORMAL   0 // measured the same as never setting it

// The speed for installs, from Settings - full unless lowered for a disc that
// won't read at it.
static volatile LONG g_spindleSpeed = DISC_SPEED_FULL;

void SetDiscReadSpeed(int speed)
{
    if (speed < DISC_SPEED_SLOWEST) speed = DISC_SPEED_SLOWEST;
    if (speed > DISC_SPEED_FULL) speed = DISC_SPEED_FULL;
    InterlockedExchange(&g_spindleSpeed, speed);
}
#define SPINDLE_SETTLE_MS      3000

// Back to the drive's quiet range. Only ever called with the drive open; see
// where the install puts it back. when says what for, in the log. settle
// waits for the drive to actually change speed, which the install does
// before closing the drive out from under the command - a probe about to
// read the disc has no need to wait, and three seconds on every disc going
// in would be felt.
static void ResetSpindleSpeed(const char *when, bool settle)
{
    const DWORD result = XamSetDvdSpindleSpeed(SPINDLE_SPEED_NORMAL);
    dprintf("[disc] spindle back to %d %s -> 0x%08lX\n", SPINDLE_SPEED_NORMAL, when, (unsigned long)result);
    if (settle)
        Sleep(SPINDLE_SETTLE_MS);
}

#define TRAY_POLL_MS        500
#define SETTLE_MS           2500   // after the tray moves, before reading the disc
// How long after the tray moves a disc that won't read yet keeps being tried.
// A disc that has just gone in can read at its start while its game partition
// still can't be - the console is still checking the disc - so a failure in
// that time means "not yet", not "can't".
#define SPIN_UP_TIMEOUT_MS  30000
#define PROBE_RETRY_MS      2000

// Room for the package beyond its own size, as the install used to check.
#define INSTALL_SPACE_MARGIN (4ULL * 1024 * 1024)

struct DiscJob
{
    bool used;       // the slot holds a job
    QueueJobSnapshot snap;
    bool cancel;
    bool handedOver; // finished, and given to TakeFinishedDiscJob already
    bool told;       // a record, and already counted in a startup summary
    unsigned long mediaId;
};

static CRITICAL_SECTION g_lock;
static HANDLE g_wake = NULL;
static HANDLE g_thread = NULL;
static bool g_running = false;
static bool g_shutdown = false;

static char g_gamesPath[512] = "";
static DiscInfo g_disc;

// Slots, not a list: a job stays where it is for its whole life, since the
// worker holds a pointer to the one it's running.
static DiscJob g_jobs[MAX_DISC_JOBS];
static int g_nextJobId = DISC_JOB_ID_BASE;
static int g_pendingInstall = -1; // index of a job waiting to start, or -1

// ---------------------------------------------------------------------------
// The install marker
// ---------------------------------------------------------------------------
//
// Written before the first byte is copied and removed once the install has
// either finished or cleaned up after itself. Anything that ends the app
// mid-copy without either - the Guide button to the dashboard, the power, a
// crash - leaves it behind, and the next launch removes the partial package:
// half a game is never worth keeping, since an install can't be resumed.

void WriteInstallMarker(const char *gamesPath, const GodImageInfo &info, const char *name)
{
    FILE *f = fopen(INSTALL_MARKER_FILE, "w");
    if (f == NULL)
    {
        dprintf("[disc] couldn't write %s - an interrupted install won't be cleaned up automatically\n",
                INSTALL_MARKER_FILE);
        return;
    }
    fprintf(f, "%s\n%08lX\n%08lX\n%I64u\n%s\n", gamesPath, info.title.titleId, info.title.mediaId,
            info.outputSize, name);
    fclose(f);
}

void ClearInstallMarker()
{
    remove(INSTALL_MARKER_FILE);
}

bool IsDiscInstalled(const char *gamesPath, unsigned long titleId, unsigned long mediaId)
{
    // An Original Xbox package is named for its title ID, in 00005000 (see
    // GodConvert.h); a 360 disc's for its media ID, in 00007000.
    char headerPath[600];
    _snprintf(headerPath, sizeof(headerPath), "%s\\%08lX\\%s\\%08lX", gamesPath, titleId,
              mediaId == titleId ? "00005000" : "00007000", mediaId);
    headerPath[sizeof(headerPath) - 1] = '\0';

    FILE *f = fopen(headerPath, "rb");
    if (f == NULL)
        return false;
    fclose(f);
    return true;
}

// ---------------------------------------------------------------------------
// Watching the drive
// ---------------------------------------------------------------------------

static int ReadTrayState()
{
    unsigned char request[16];
    unsigned char reply[16];
    memset(request, 0, sizeof(request));
    memset(reply, 0, sizeof(reply));
    request[0] = 0x0A;
    HalSendSMCMessage(request, reply);
    return reply[1];
}

// Under the lock.
static void SetDiscState(DiscState state)
{
    if (g_disc.state != state)
        g_disc.changeCount++;
    g_disc.state = state;
}

// Reads the disc now in the drive. True once there's an answer: a game disc,
// or - on the last chance - one that can't be installed. False to try again:
// nothing would open (no disc, or still spinning up), or the game partition
// couldn't be read yet.
static bool ProbeDisc(bool lastChance)
{
    // Quietly: an empty drive fails every read, and its sector-by-sector
    // details only ever said "no disc". One line says it on the last try.
    DiscSource disc;
    if (!disc.Open(NULL))
    {
        if (lastChance)
            dprintf("[disc] nothing readable in the drive\n");
        return false;
    }

    // Nothing is installing here - the worker runs one thing at a time - so
    // whatever speed the drive is at isn't wanted. An install that ended
    // without putting it back leaves it raised until something opens the
    // drive again, which is here.
    ResetSpindleSpeed("for a disc going in", false);

    EnterCriticalSection(&g_lock);
    SetDiscState(DISC_READING);
    LeaveCriticalSection(&g_lock);

    GodImageInfo info;
    GodResult result = GodInspect(&disc, &info);
    disc.Close();

    if (result != GOD_OK && !lastChance)
    {
        dprintf("[disc] not readable yet (%s); trying again\n", GodResultText(result));
        return false; // still DISC_READING
    }

    EnterCriticalSection(&g_lock);
    if (result == GOD_OK)
    {
        g_disc.titleId = info.title.titleId;
        g_disc.mediaId = info.title.mediaId;
        g_disc.discNumber = info.title.discNumber;
        g_disc.discCount = info.title.discCount;
        g_disc.outputSize = info.outputSize;
        memcpy(g_disc.name, info.title.name, sizeof(g_disc.name));
        g_disc.name[sizeof(g_disc.name) - 1] = '\0';
        g_disc.original = (info.title.contentType == GOD_CONTENT_XBOX_ORIGINAL);
        g_disc.reason[0] = '\0';
        SetDiscState(DISC_READY);
        g_disc.changeCount++; // a different game is a change even from READY to READY
    }
    else
    {
        g_disc.titleId = 0;
        g_disc.mediaId = 0;
        g_disc.name[0] = '\0';
        _snprintf(g_disc.reason, sizeof(g_disc.reason), "%s",
                  result == GOD_NOT_A_DISC_IMAGE ? "Not an Xbox 360 game disc" : GodResultText(result));
        g_disc.reason[sizeof(g_disc.reason) - 1] = '\0';
        SetDiscState(DISC_UNREADABLE);
    }
    LeaveCriticalSection(&g_lock);

    if (result == GOD_OK)
        dprintf("[disc] in the drive: %s, title %08lX, media %08lX, disc %u of %u, %I64u bytes to install\n",
                info.imageType, info.title.titleId, info.title.mediaId, info.title.discNumber,
                info.title.discCount, info.outputSize);
    else
        dprintf("[disc] in the drive, but not installable: %s\n", GodResultText(result));

    return true;
}

// ---------------------------------------------------------------------------
// Installing
// ---------------------------------------------------------------------------

struct InstallProgress
{
    DiscJob *job;
    DWORD startTick;
};

// Called by GodConvert after every 816KB: the numbers for the Queue page, and
// whether to carry on.
static bool InstallProgressCallback(unsigned long long done, unsigned long long total, void *context)
{
    InstallProgress *p = (InstallProgress *)context;

    DWORD elapsedMs = GetTickCount() - p->startTick;
    unsigned long long bytesPerSec = (elapsedMs > 0) ? done * 1000ULL / elapsedMs : 0;

    EnterCriticalSection(&g_lock);
    QueueJobSnapshot &s = p->job->snap;
    s.bytesDone = done;
    s.bytesTotal = total;
    s.fraction = (total > 0) ? (float)((double)done / (double)total) : -1.0f;

    // The first seconds' rate is mostly the drive spinning up.
    if (elapsedMs > 3000 && bytesPerSec > 0)
    {
        s.bytesPerSec = bytesPerSec;
        s.secondsLeft = (total > done) ? (total - done) / bytesPerSec : 0;
    }

    bool carryOn = !p->job->cancel && !g_shutdown;
    LeaveCriticalSection(&g_lock);
    return carryOn;
}

// ---------------------------------------------------------------------------
// What finished, kept between runs - see QueueState.h
// ---------------------------------------------------------------------------
//
// Only what finished. A disc install can't be picked back up the way a
// download can: the disc may not be in the drive next time, and what
// GodConvert had copied is removed when an install stops. So unlike the other
// two queues this file holds records and nothing else - the row saying an
// install worked, or why it didn't, which would otherwise be gone the moment
// the console is switched off.
//
// See the note on records in DownloadQueue.cpp for why they are records
// rather than jobs, and why they come back already handed over.

#define DISC_STATE_FILE "game:\\FinishedDiscInstalls.txt"
#define DISC_STATE_MAX  (8 * 1024)
#define MAX_FINISHED_KEPT 4

// Under the lock. Cancelled ones are left out: the user did that and knows.
static void SaveDiscStateLocked()
{
    if (!g_running || g_shutdown)
        return;

    int order[MAX_DISC_JOBS];
    int records = 0;

    for (int i = 0; i < MAX_DISC_JOBS; ++i)
    {
        const DiscJob &job = g_jobs[i];
        if (!job.used || job.snap.state != QUEUE_FINISHED || job.snap.outcome == QUEUE_OUTCOME_CANCELLED)
            continue;

        // By id, which is the order they finished in. Eight slots at the
        // most, so an insertion sort is the whole of it.
        int pos = records;
        while (pos > 0 && g_jobs[order[pos - 1]].snap.id > job.snap.id)
        {
            order[pos] = order[pos - 1];
            pos--;
        }
        order[pos] = i;
        records++;
    }

    if (records == 0)
    {
        DeleteQueueStateFile(DISC_STATE_FILE);
        return;
    }

    char text[DISC_STATE_MAX];
    size_t at = 0;
    text[0] = '\0';
    AppendQueueStateText(text, sizeof(text), &at, "# Omni360 - the disc installs that finished\nformat=1\n");

    int first = records - MAX_FINISHED_KEPT;
    if (first < 0)
        first = 0;

    // Oldest first, so reading them back in order gives them their order.
    for (int i = first; i < records; ++i)
    {
        const DiscJob &job = g_jobs[order[i]];
        AppendQueueStateText(text, sizeof(text), &at,
                             "done=disc\nname=%s\ntitleid=%08lX\nmediaid=%08lX\ntitle=%s\noutcome=%d\n"
                             "result=%s\ndetail=%s\n",
                             job.snap.gameName, job.snap.titleId, job.mediaId, job.snap.title,
                             (int)job.snap.outcome, job.snap.resultText, job.snap.resultDetail);

        if (job.told)
            AppendQueueStateText(text, sizeof(text), &at, "told=1\n");
    }

    WriteQueueStateFile(DISC_STATE_FILE, text);
}

// Under the lock.
static void FinishJob(DiscJob *job, QueueOutcome outcome, const char *text, const char *detail, bool notify)
{
    QueueJobSnapshot &s = job->snap;
    s.state = QUEUE_FINISHED;
    s.outcome = outcome;
    s.notify = notify;
    _snprintf(s.resultText, sizeof(s.resultText), "%s", text);
    s.resultText[sizeof(s.resultText) - 1] = '\0';
    _snprintf(s.resultDetail, sizeof(s.resultDetail), "%s", detail != NULL ? detail : "");
    s.resultDetail[sizeof(s.resultDetail) - 1] = '\0';
    g_disc.installing = false;

    SaveDiscStateLocked();
}

static void RunInstall(DiscJob *job, const char *gamesPath)
{
    DiscSource disc;
    GodImageInfo info;
    GodResult result = GOD_READ_FAILED;

    bool opened = disc.Open(dprintf);
    if (opened)
        result = GodInspect(&disc, &info);

    if (!opened || result != GOD_OK || info.title.titleId != job->snap.titleId || info.title.mediaId != job->mediaId)
    {
        if (opened)
            disc.Close();
        EnterCriticalSection(&g_lock);
        FinishJob(job, QUEUE_OUTCOME_FAILED, "Couldn't install",
                  "The disc in the drive changed, or couldn't be read.", true);
        LeaveCriticalSection(&g_lock);
        return;
    }

    unsigned long long freeSpace = 0;
    if (DriveFreeSpace(gamesPath, &freeSpace) && freeSpace < info.outputSize + INSTALL_SPACE_MARGIN)
    {
        disc.Close();

        char needText[64] = "", freeText[64] = "", detail[160];
        FormatBytes(info.outputSize, needText, sizeof(needText));
        FormatBytes(freeSpace, freeText, sizeof(freeText));
        _snprintf(detail, sizeof(detail), "It needs %s, and %s is free.", needText, freeText);
        detail[sizeof(detail) - 1] = '\0';

        EnterCriticalSection(&g_lock);
        FinishJob(job, QUEUE_OUTCOME_NO_SPACE, "Not enough space", detail, true);
        LeaveCriticalSection(&g_lock);
        return;
    }

    // Full speed for the copy - about four times what the drive does left
    // alone - unless Settings lowered it for a disc that won't read at that,
    // and back to normal afterwards, so a film or a game played next isn't
    // any louder for it.
    const DWORD speed = (DWORD)g_spindleSpeed;
    DWORD spindleResult = XamSetDvdSpindleSpeed(speed);
    dprintf("[disc] spindle to speed %lu of %d -> 0x%08lX\n", (unsigned long)speed, DISC_SPEED_FULL,
            (unsigned long)spindleResult);
    Sleep(SPINDLE_SETTLE_MS);

    EnterCriticalSection(&g_lock);
    job->snap.bytesTotal = info.usedSize;
    _snprintf(job->snap.phase, sizeof(job->snap.phase), "Copying from the disc");
    LeaveCriticalSection(&g_lock);

    InstallProgress progress;
    progress.job = job;
    progress.startTick = GetTickCount();

    // From here until GodConvert returns, the marker is what cleans up if the
    // app is ended mid-copy. GodConvert removes its own output on every
    // failure it sees, so once it returns there is nothing left to clean.
    // The game's icon for the package, from Xbox Live - none if it can't be
    // had, offline say, and the dashboard shows its placeholder.
    static unsigned char icon[GOD_ICON_MAX];
    unsigned long iconSize = 0;
    if (!FetchTitleIcon(info.title.titleId, icon, sizeof(icon), &iconSize))
        iconSize = 0;

    WriteInstallMarker(gamesPath, info, job->snap.gameName);

    char packagePath[600] = "";
    GodTimings timings;
    memset(&timings, 0, sizeof(timings));
    {
        // The drive keeps reading while each group is hashed and written -
        // see ReadAhead.h. 8MB ahead, in 1MB reads. Scoped so its thread has
        // finished with the drive before the drive is closed.
        //
        // An Original Xbox disc is read through its filesystem - only the
        // sectors its files are in - as the drive refuses its security
        // ranges; see UsedSectorsSource.
        UsedSectorsSource used(&disc, info.rootOffset);
        GodSource *source = &disc;
        if (info.title.contentType == GOD_CONTENT_XBOX_ORIGINAL)
        {
            const GodResult built = used.Build();
            if (built == GOD_OK)
            {
                source = &used;
                dprintf("[disc] Original Xbox disc: reading only its files - %I64u MB in %lu ranges\n",
                        used.UsedBytes() / (1024 * 1024), used.Extents());
            }
            else
            {
                dprintf("[disc] Original Xbox disc: couldn't map its files (%s) - reading it all\n",
                        GodResultText(built));
            }
        }
        ReadAheadSource ahead(source, 1024 * 1024, 8);
        result = GodConvert(&ahead, info, gamesPath, job->snap.gameName, iconSize > 0 ? icon : NULL, iconSize,
                            InstallProgressCallback, &progress, packagePath, sizeof(packagePath), &timings);
    }

    ClearInstallMarker();

    // Back to the quiet speed BEFORE the drive is closed. The drive only
    // takes a spindle speed while it is open - raising it above works
    // because disc.Open() has already happened - and a speed set after the
    // close is dropped, which left the drive howling at full speed until
    // something else opened it. It needs the same moment to settle as
    // raising it did, or the close can overtake it.
    ResetSpindleSpeed("after the install", true);
    disc.Close();

    DWORD seconds = (GetTickCount() - progress.startTick) / 1000;
    dprintf("[disc] install %s after %lu:%02lu: %s\n", GodResultText(result),
            (unsigned long)(seconds / 60), (unsigned long)(seconds % 60), packagePath);
    // What was actually copied - all of it for a finished install, but only
    // as far as it got for a stopped or failed one.
    EnterCriticalSection(&g_lock);
    const unsigned long long copied = job->snap.bytesDone;
    LeaveCriticalSection(&g_lock);

    if (seconds > 0)
        dprintf("[disc] %I64u of %I64u MB at %.2f MB/s - waiting for the disc %.0fs, hashing %.0fs, writing %.0fs\n",
                copied / (1024 * 1024), info.usedSize / (1024 * 1024),
                (double)copied / (1024.0 * 1024.0) / (double)seconds,
                timings.readMs / 1000.0, timings.hashMs / 1000.0, timings.writeMs / 1000.0);

    char detail[160];
    EnterCriticalSection(&g_lock);
    switch (result)
    {
    case GOD_OK:
        if (info.title.discCount > 1 && info.title.discNumber < info.title.discCount)
            _snprintf(detail, sizeof(detail), "That was disc %u of %u - put in disc %u to install it.",
                      (unsigned)info.title.discNumber, (unsigned)info.title.discCount,
                      (unsigned)info.title.discNumber + 1);
        else
            _snprintf(detail, sizeof(detail), "Play it from the dashboard or Aurora - the disc isn't needed.");
        detail[sizeof(detail) - 1] = '\0';
        FinishJob(job, QUEUE_OUTCOME_INSTALLED, "Installed", detail, true);
        break;

    case GOD_CANCELLED:
        // The user did it, so no popup.
        FinishJob(job, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
        break;

    case GOD_READ_FAILED:
        FinishJob(job, QUEUE_OUTCOME_FAILED, "Couldn't read the disc",
                  "It may be dirty or scratched - clean it and try again.", true);
        break;

    case GOD_WRITE_FAILED:
        FinishJob(job, QUEUE_OUTCOME_FAILED, "Couldn't write the game",
                  "Check the drive the games folder is on, then try again.", true);
        break;

    default:
        FinishJob(job, QUEUE_OUTCOME_FAILED, "Couldn't install", GodResultText(result), true);
        break;
    }
    LeaveCriticalSection(&g_lock);
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static DWORD WINAPI DiscEntry(LPVOID)
{
    int lastTray = ReadTrayState();
    dprintf("[disc] tray state 0x%02X at start\n", lastTray);

    // Read whatever is in the drive at launch, as if it had just gone in.
    DWORD changedAt = GetTickCount() - SETTLE_MS;
    bool probing = true;
    DWORD lastAttempt = 0;
    bool attempted = false;

    for (;;)
    {
        WaitForSingleObject(g_wake, TRAY_POLL_MS);

        char gamesPath[512];
        DiscJob *install = NULL;

        EnterCriticalSection(&g_lock);
        if (g_shutdown)
        {
            LeaveCriticalSection(&g_lock);
            break;
        }
        if (g_pendingInstall >= 0)
        {
            install = &g_jobs[g_pendingInstall];
            g_pendingInstall = -1;
            install->snap.state = QUEUE_ACTIVE;
        }
        memcpy(gamesPath, g_gamesPath, sizeof(gamesPath));
        LeaveCriticalSection(&g_lock);

        if (install != NULL)
        {
            if (install->cancel)
            {
                EnterCriticalSection(&g_lock);
                FinishJob(install, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
                LeaveCriticalSection(&g_lock);
            }
            else
            {
                RunInstall(install, gamesPath);
            }

            // The tray may have moved during a long install; look again.
            lastTray = ReadTrayState();
            continue;
        }

        int tray = ReadTrayState();
        if (tray != lastTray)
        {
            dprintf("[disc] tray state 0x%02X -> 0x%02X\n", lastTray, tray);
            lastTray = tray;

            // Whatever was in the drive may not be any more.
            EnterCriticalSection(&g_lock);
            SetDiscState(DISC_NONE);
            g_disc.titleId = 0;
            g_disc.mediaId = 0;
            LeaveCriticalSection(&g_lock);

            changedAt = GetTickCount();
            probing = true;
            attempted = false;
        }

        if (!probing)
            continue;

        DWORD now = GetTickCount();
        if (now - changedAt < SETTLE_MS)
            continue;
        if (attempted && now - lastAttempt < PROBE_RETRY_MS)
            continue;

        bool lastChance = (now - changedAt >= SPIN_UP_TIMEOUT_MS);
        lastAttempt = now;
        attempted = true;

        if (ProbeDisc(lastChance))
        {
            probing = false;
        }
        else if (lastChance)
        {
            // Nothing to read - an empty drive, or the tray left open.
            probing = false;
            EnterCriticalSection(&g_lock);
            SetDiscState(DISC_NONE);
            LeaveCriticalSection(&g_lock);
        }
    }

    return 0;
}

bool StartDiscWorker(const char *gamesPath)
{
    if (g_running)
        return true;

    InitializeCriticalSection(&g_lock);
    memset(&g_disc, 0, sizeof(g_disc));
    SetDiscWorkerGamesPath(gamesPath);

    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
    if (g_wake == NULL)
    {
        dprintf("[disc] CreateEvent failed (%lu)\n", GetLastError());
        return false;
    }

    // GodConvert's hashing and the read-ahead's buffers want more than the
    // default; the same stack as the other workers.
    g_thread = CreateThread(NULL, 256 * 1024, DiscEntry, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread == NULL)
    {
        dprintf("[disc] CreateThread failed (%lu)\n", GetLastError());
        CloseHandle(g_wake);
        g_wake = NULL;
        return false;
    }

#ifdef _XBOX
    // Hardware thread 1, core 0's second. The UI on thread 0 draws a frame
    // and sleeps; an install's work is hashing what the read-ahead on thread
    // 2 brings in from the drive.
    XSetThreadProcessor(g_thread, 1);
#endif

    g_running = true;
    ResumeThread(g_thread);

    dprintf("[disc] disc worker started\n");
    return true;
}

bool StopDiscWorker(DWORD timeoutMs)
{
    if (!g_running)
        return true;

    EnterCriticalSection(&g_lock);
    g_shutdown = true;
    for (int i = 0; i < MAX_DISC_JOBS; ++i)
        g_jobs[i].cancel = true;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);

    bool stopped = (WaitForSingleObject(g_thread, timeoutMs) == WAIT_OBJECT_0);
    if (!stopped)
    {
        dprintf("[disc] still running after %lums - leaving anyway\n", timeoutMs);
        return false;
    }

    CloseHandle(g_thread);
    g_thread = NULL;
    g_running = false;
    return true;
}

void SetDiscWorkerGamesPath(const char *gamesPath)
{
    if (g_running)
        EnterCriticalSection(&g_lock);
    strncpy(g_gamesPath, gamesPath != NULL ? gamesPath : "", sizeof(g_gamesPath) - 1);
    g_gamesPath[sizeof(g_gamesPath) - 1] = '\0';
    if (g_running)
        LeaveCriticalSection(&g_lock);
}

void GetDiscInfo(DiscInfo *out)
{
    if (!g_running)
    {
        memset(out, 0, sizeof(*out));
        out->state = DISC_NONE;
        return;
    }

    EnterCriticalSection(&g_lock);
    *out = g_disc;
    LeaveCriticalSection(&g_lock);
}

// A free slot for a new job, or -1. Under the lock. Full of finished jobs,
// the oldest one already handed over makes room. A job never moves once it
// has a slot - the worker holds a pointer to the one it's running.
static int FreeJobSlot()
{
    int oldest = -1;
    for (int i = 0; i < MAX_DISC_JOBS; ++i)
    {
        if (!g_jobs[i].used)
            return i;
        if (g_jobs[i].snap.state == QUEUE_FINISHED && g_jobs[i].handedOver &&
            (oldest < 0 || g_jobs[i].snap.id < g_jobs[oldest].snap.id))
            oldest = i;
    }
    if (oldest >= 0)
        g_jobs[oldest].used = false;
    return oldest;
}

// Under the lock. Already handed over, which is the trap: a finished job that
// hasn't been would be given to main.cpp, which raises its popup and sets a
// library rescan going - so restoring last night's results without it would
// re-announce every one of them over the startup screen.
static void RestoreFinishedDiscLocked(const char *name, unsigned long titleId, unsigned long mediaId,
                                      const char *title, int outcome, const char *resultText,
                                      const char *resultDetail, bool told)
{
    const int slot = FreeJobSlot();
    if (slot < 0)
        return;

    DiscJob &job = g_jobs[slot];
    memset(&job, 0, sizeof(job));

    job.used = true;
    job.handedOver = true;
    job.told = true; // counted below if it wasn't already, and now it has been
    job.mediaId = mediaId;
    job.snap.id = g_nextJobId++;
    job.snap.kind = QUEUE_JOB_DISC_INSTALL;
    job.snap.state = QUEUE_FINISHED;
    job.snap.outcome = (QueueOutcome)outcome;
    job.snap.titleId = titleId;
    job.snap.fraction = -1.0f;
    job.snap.notify = false;
    _snprintf(job.snap.gameName, sizeof(job.snap.gameName), "%s", name);
    job.snap.gameName[sizeof(job.snap.gameName) - 1] = '\0';
    _snprintf(job.snap.title, sizeof(job.snap.title), "%s", title);
    job.snap.title[sizeof(job.snap.title) - 1] = '\0';
    _snprintf(job.snap.resultText, sizeof(job.snap.resultText), "%s", resultText);
    job.snap.resultText[sizeof(job.snap.resultText) - 1] = '\0';
    _snprintf(job.snap.resultDetail, sizeof(job.snap.resultDetail), "%s", resultDetail);
    job.snap.resultDetail[sizeof(job.snap.resultDetail) - 1] = '\0';

    if (!told)
        NoteRestoredRecord(outcome != QUEUE_OUTCOME_INSTALLED && outcome != QUEUE_OUTCOME_ALREADY_INSTALLED);
}

void RestoreDiscRecords()
{
    if (!g_running)
        return;

    char *text = ReadQueueStateFile(DISC_STATE_FILE);
    if (text == NULL)
        return;

    char name[128] = "", title[256] = "", resultText[64] = "", resultDetail[160] = "";
    unsigned long titleId = 0, mediaId = 0;
    int outcome = 0;
    bool inDone = false;
    bool told = false;

    EnterCriticalSection(&g_lock);

    char *at = text, *key = NULL, *value = NULL;
    while (NextQueueStateLine(&at, &key, &value))
    {
        // A "done=" line ends the record before it and starts the next.
        if (strcmp(key, "done") == 0)
        {
            if (inDone)
                RestoreFinishedDiscLocked(name, titleId, mediaId, title, outcome, resultText, resultDetail, told);

            name[0] = title[0] = resultText[0] = resultDetail[0] = '\0';
            titleId = mediaId = 0;
            outcome = 0;
            told = false;
            inDone = (strcmp(value, "disc") == 0);
            continue;
        }

        if (!inDone)
            continue;

        if (strcmp(key, "name") == 0)
            _snprintf(name, sizeof(name), "%s", value);
        else if (strcmp(key, "titleid") == 0)
            titleId = strtoul(value, NULL, 16);
        else if (strcmp(key, "mediaid") == 0)
            mediaId = strtoul(value, NULL, 16);
        else if (strcmp(key, "title") == 0)
            _snprintf(title, sizeof(title), "%s", value);
        else if (strcmp(key, "outcome") == 0)
            outcome = atoi(value);
        else if (strcmp(key, "result") == 0)
            _snprintf(resultText, sizeof(resultText), "%s", value);
        else if (strcmp(key, "detail") == 0)
            _snprintf(resultDetail, sizeof(resultDetail), "%s", value);
        else if (strcmp(key, "told") == 0)
            told = (atoi(value) != 0);

        name[sizeof(name) - 1] = '\0';
        title[sizeof(title) - 1] = '\0';
        resultText[sizeof(resultText) - 1] = '\0';
        resultDetail[sizeof(resultDetail) - 1] = '\0';
    }

    if (inDone) // the last record has no "done=" after it
        RestoreFinishedDiscLocked(name, titleId, mediaId, title, outcome, resultText, resultDetail, told);

    // The records are all marked told now, and nothing else here writes the
    // file - a disc install has to happen for FinishJob to save it - so
    // without this they would be counted again on every launch.
    SaveDiscStateLocked();

    LeaveCriticalSection(&g_lock);
    free(text);
}

DiscInstallResult QueueDiscInstall(unsigned long titleId, unsigned long mediaId, const char *name)
{
    if (!g_running)
        return DISC_INSTALL_UNAVAILABLE;

    DiscInstallResult result = DISC_INSTALL_QUEUED;
    int slot = -1;

    EnterCriticalSection(&g_lock);

    if (g_disc.installing)
        result = DISC_INSTALL_BUSY;
    else if (g_disc.state != DISC_READY || g_disc.titleId != titleId || g_disc.mediaId != mediaId)
        result = DISC_INSTALL_NO_DISC;
    else if ((slot = FreeJobSlot()) < 0)
        result = DISC_INSTALL_BUSY;

    if (result == DISC_INSTALL_QUEUED)
    {
        DiscJob &job = g_jobs[slot];
        memset(&job, 0, sizeof(job));
        job.used = true;
        job.snap.id = g_nextJobId++;
        job.snap.kind = QUEUE_JOB_DISC_INSTALL;
        job.snap.state = QUEUE_WAITING;
        job.snap.outcome = QUEUE_OUTCOME_NONE;
        job.snap.titleId = titleId;
        job.snap.fraction = -1.0f;
        _snprintf(job.snap.gameName, sizeof(job.snap.gameName), "%s", name != NULL ? name : "");
        job.snap.gameName[sizeof(job.snap.gameName) - 1] = '\0';

        if (g_disc.discCount > 1)
            _snprintf(job.snap.title, sizeof(job.snap.title), "Installing disc %u of %u",
                      g_disc.discNumber, g_disc.discCount);
        else
            _snprintf(job.snap.title, sizeof(job.snap.title), "Installing from the disc");
        job.snap.title[sizeof(job.snap.title) - 1] = '\0';

        job.mediaId = mediaId;
        g_pendingInstall = slot;
        g_disc.installing = true;
    }

    LeaveCriticalSection(&g_lock);

    if (result == DISC_INSTALL_QUEUED)
        SetEvent(g_wake);
    return result;
}

int SnapshotDiscJobs(QueueJobSnapshot *out, int maxJobs)
{
    if (!g_running)
        return 0;

    int n = 0;
    EnterCriticalSection(&g_lock);

    // Not finished first (there is at most one), then finished, newest
    // first - by id, since slots aren't in any order.
    for (int i = 0; i < MAX_DISC_JOBS && n < maxJobs; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].snap.state != QUEUE_FINISHED)
            out[n++] = g_jobs[i].snap;
    }

    int lastId = 0x7FFFFFFF;
    for (;;)
    {
        int pick = -1;
        for (int i = 0; i < MAX_DISC_JOBS; ++i)
        {
            if (g_jobs[i].used && g_jobs[i].snap.state == QUEUE_FINISHED && g_jobs[i].snap.id < lastId &&
                (pick < 0 || g_jobs[i].snap.id > g_jobs[pick].snap.id))
                pick = i;
        }
        if (pick < 0 || n >= maxJobs)
            break;
        out[n++] = g_jobs[pick].snap;
        lastId = g_jobs[pick].snap.id;
    }

    LeaveCriticalSection(&g_lock);
    return n;
}

int PendingDiscJobCount()
{
    if (!g_running)
        return 0;

    int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_DISC_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].snap.state != QUEUE_FINISHED)
            n++;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

void CancelDiscJob(int id)
{
    if (!g_running)
        return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_DISC_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].snap.id == id && g_jobs[i].snap.state != QUEUE_FINISHED)
            g_jobs[i].cancel = true;
    }
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

void RemoveDiscJob(int id)
{
    if (!g_running)
        return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_DISC_JOBS; ++i)
    {
        // Only once main.cpp has seen it finish, so no popup goes missing.
        if (g_jobs[i].used && g_jobs[i].snap.id == id && g_jobs[i].snap.state == QUEUE_FINISHED &&
            g_jobs[i].handedOver)
        {
            g_jobs[i].used = false;

            // Now that a finished install is kept, clearing its row is a
            // change to what the file holds.
            SaveDiscStateLocked();
        }
    }
    LeaveCriticalSection(&g_lock);
}

bool TakeFinishedDiscJob(QueueJobSnapshot *out)
{
    if (!g_running)
        return false;

    bool found = false;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_DISC_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].snap.state == QUEUE_FINISHED && !g_jobs[i].handedOver)
        {
            g_jobs[i].handedOver = true;
            *out = g_jobs[i].snap;
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}
