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
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <string.h>

// The system controller, through the kernel. Command 0x0A asks for the disc
// tray's state, answered in the second byte of the reply. The XDK's
// xboxkrnl.lib exports it, though no header declares it.
extern "C" VOID HalSendSMCMessage(LPVOID input, LPVOID output);

#define TRAY_POLL_MS        500
#define SETTLE_MS           2500   // after the tray moves, before reading the disc
#define SPIN_UP_TIMEOUT_MS  20000  // how long a disc that won't open yet is retried
#define PROBE_RETRY_MS      2000

// Room for the package beyond its own size, as the install used to check.
#define INSTALL_SPACE_MARGIN (4ULL * 1024 * 1024)

struct DiscJob
{
    QueueJobSnapshot snap;
    bool cancel;
    bool handedOver; // finished, and given to TakeFinishedDiscJob already
    unsigned long mediaId;
};

static CRITICAL_SECTION g_lock;
static HANDLE g_wake = NULL;
static HANDLE g_thread = NULL;
static bool g_running = false;
static bool g_shutdown = false;

static char g_gamesPath[512] = "";
static DiscInfo g_disc;

static DiscJob g_jobs[MAX_DISC_JOBS];
static int g_jobCount = 0;
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

static void WriteInstallMarker(const char *gamesPath, const GodImageInfo &info, const char *name)
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
    char headerPath[600];
    _snprintf(headerPath, sizeof(headerPath), "%s\\%08lX\\00007000\\%08lX", gamesPath, titleId, mediaId);
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

// Reads the disc now in the drive. False if it wouldn't open - nothing there,
// or still spinning up - which leaves the state alone so it can be retried.
static bool ProbeDisc(bool logOpenFailure)
{
    DiscSource disc;
    if (!disc.Open(logOpenFailure ? dprintf : NULL))
        return false;

    EnterCriticalSection(&g_lock);
    SetDiscState(DISC_READING);
    LeaveCriticalSection(&g_lock);

    GodImageInfo info;
    GodResult result = GodInspect(&disc, &info);
    disc.Close();

    EnterCriticalSection(&g_lock);
    if (result == GOD_OK)
    {
        g_disc.titleId = info.title.titleId;
        g_disc.mediaId = info.title.mediaId;
        g_disc.discNumber = info.title.discNumber;
        g_disc.discCount = info.title.discCount;
        g_disc.outputSize = info.outputSize;
        g_disc.reason[0] = '\0';
        SetDiscState(DISC_READY);
        g_disc.changeCount++; // a different game is a change even from READY to READY
    }
    else
    {
        g_disc.titleId = 0;
        g_disc.mediaId = 0;
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
    WriteInstallMarker(gamesPath, info, job->snap.gameName);

    char packagePath[600] = "";
    GodTimings timings;
    memset(&timings, 0, sizeof(timings));
    {
        // The drive keeps reading while each group is hashed and written -
        // see ReadAhead.h. 8MB ahead, in 1MB reads. Scoped so its thread has
        // finished with the drive before the drive is closed.
        ReadAheadSource ahead(&disc, 1024 * 1024, 8);
        result = GodConvert(&ahead, info, gamesPath, job->snap.gameName, NULL, 0,
                            InstallProgressCallback, &progress, packagePath, sizeof(packagePath), &timings);
    }

    ClearInstallMarker();
    disc.Close();

    DWORD seconds = (GetTickCount() - progress.startTick) / 1000;
    dprintf("[disc] install %s after %lu:%02lu: %s\n", GodResultText(result),
            (unsigned long)(seconds / 60), (unsigned long)(seconds % 60), packagePath);
    if (seconds > 0)
        dprintf("[disc] %I64u MB at %.2f MB/s - waiting for the disc %.0fs, hashing %.0fs, writing %.0fs\n",
                info.usedSize / (1024 * 1024), (double)info.usedSize / (1024.0 * 1024.0) / (double)seconds,
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
    for (int i = 0; i < g_jobCount; ++i)
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

DiscInstallResult QueueDiscInstall(unsigned long titleId, unsigned long mediaId, const char *name)
{
    if (!g_running)
        return DISC_INSTALL_UNAVAILABLE;

    DiscInstallResult result = DISC_INSTALL_QUEUED;

    EnterCriticalSection(&g_lock);

    if (g_disc.installing)
    {
        result = DISC_INSTALL_BUSY;
    }
    else if (g_disc.state != DISC_READY || g_disc.titleId != titleId || g_disc.mediaId != mediaId)
    {
        result = DISC_INSTALL_NO_DISC;
    }
    else
    {
        // Full of finished jobs: the oldest finished one makes room.
        if (g_jobCount >= MAX_DISC_JOBS)
        {
            int oldest = -1;
            for (int i = 0; i < g_jobCount; ++i)
            {
                if (g_jobs[i].snap.state == QUEUE_FINISHED && (oldest < 0 || g_jobs[i].snap.id < g_jobs[oldest].snap.id))
                    oldest = i;
            }
            if (oldest >= 0)
            {
                for (int i = oldest + 1; i < g_jobCount; ++i)
                    g_jobs[i - 1] = g_jobs[i];
                g_jobCount--;
            }
        }

        DiscJob &job = g_jobs[g_jobCount];
        memset(&job, 0, sizeof(job));
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
        g_pendingInstall = g_jobCount;
        g_jobCount++;
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

    // Not finished first (there is at most one), then finished, newest first.
    for (int i = 0; i < g_jobCount && n < maxJobs; ++i)
    {
        if (g_jobs[i].snap.state != QUEUE_FINISHED)
            out[n++] = g_jobs[i].snap;
    }
    for (int i = g_jobCount - 1; i >= 0 && n < maxJobs; --i)
    {
        if (g_jobs[i].snap.state == QUEUE_FINISHED)
            out[n++] = g_jobs[i].snap;
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
    for (int i = 0; i < g_jobCount; ++i)
    {
        if (g_jobs[i].snap.state != QUEUE_FINISHED)
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
    for (int i = 0; i < g_jobCount; ++i)
    {
        if (g_jobs[i].snap.id == id && g_jobs[i].snap.state != QUEUE_FINISHED)
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
    for (int i = 0; i < g_jobCount; ++i)
    {
        // Only once main.cpp has seen it finish, so no popup goes missing.
        if (g_jobs[i].snap.id == id && g_jobs[i].snap.state == QUEUE_FINISHED && g_jobs[i].handedOver)
        {
            for (int j = i + 1; j < g_jobCount; ++j)
                g_jobs[j - 1] = g_jobs[j];
            g_jobCount--;
            break;
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
    for (int i = 0; i < g_jobCount; ++i)
    {
        if (g_jobs[i].snap.state == QUEUE_FINISHED && !g_jobs[i].handedOver)
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
