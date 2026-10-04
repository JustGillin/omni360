/*
FILE : DownloadQueue.cpp
PROJECT : Omni360
DESCRIPTION : the download queue - DLC packs and title updates, installed one
              at a time on a worker thread so the UI stays usable while they
              download. See DownloadQueue.h.
*/

#include "DownloadQueue.h"
#include "downloadFile.h" // FormatBytes, DownloadProgressFn
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <string.h>

// Enough for the largest pack seen so far with room to spare; ListDlcMembers
// stops at this many.
#define MAX_DLC_MEMBERS 128

// Free space kept in hand beyond a pack's own size, so a pack that only just
// fits doesn't leave the drive at zero.
#define PACK_SPACE_MARGIN (4ULL * 1024 * 1024)

struct QueueJob
{
    bool used;
    int id;
    unsigned long sequence;    // order added - the order jobs run in
    unsigned long finishedSeq; // order finished - newest listed first

    QueueJobKind kind;
    QueueJobState state;
    QueueOutcome outcome;
    bool cancelRequested;
    bool handedOver; // finished, and already returned by TakeFinishedQueueJob

    DlcRarMatch pack;          // QUEUE_JOB_DLC_PACK
    TitleUpdateMatch update;   // QUEUE_JOB_TITLE_UPDATE
    char authHeader[IAS3_AUTH_HEADER_MAX];

    // The worker's own counters, kept here so its progress callbacks - which
    // get nothing but byte counts - can turn them into a fraction for the
    // whole job.
    int fileIndex;
    int fileCount;

    QueueJobSnapshot view; // everything the UI reads; id/kind/state/outcome mirrored in
};

static QueueJob g_jobs[MAX_QUEUE_JOBS];
static CRITICAL_SECTION g_lock;
static HANDLE g_wake = NULL;   // set when a job is added, or to shut down
static HANDLE g_thread = NULL;
static bool g_running = false;
static bool g_shutdown = false;
static int g_nextId = 1;
static unsigned long g_nextSequence = 1;
static unsigned long g_nextFinishedSeq = 1;
static char g_contentBasePath[512] = "";

// The job the worker is running, as a slot in g_jobs; -1 when idle. Written
// only by the worker, under the lock.
static int g_activeSlot = -1;

// Worker-only. Static rather than on the worker's stack: 128 members is 66KB.
static DlcMember g_members[MAX_DLC_MEMBERS];

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Bounded, always NUL-terminated - _snprintf on this toolchain doesn't
// terminate on truncation, and strncpy doesn't either.
static void CopyText(char *out, size_t outSize, const char *in)
{
    if (outSize == 0)
        return;
    strncpy(out, in != NULL ? in : "", outSize - 1);
    out[outSize - 1] = '\0';
}

// "Hdd1:" from "Hdd1:\Content\...", or the fallback if the path names no drive.
static void DriveLabel(const char *path, char *out, size_t outSize, const char *fallback)
{
    CopyText(out, outSize, fallback);

    const char *colon = strchr(path, ':');
    if (colon != NULL && (size_t)(colon - path) < outSize - 1)
    {
        memcpy(out, path, (size_t)(colon - path) + 1);
        out[colon - path + 1] = '\0';
    }
}

static void NoSpaceDetail(char *out, size_t outSize, unsigned long long needed, unsigned long long freeSpace)
{
    char drive[16];
    DriveLabel(g_contentBasePath, drive, sizeof(drive), "the drive");

    char neededText[64] = "", freeText[64] = "";
    FormatBytes(needed, neededText, sizeof(neededText));
    FormatBytes(freeSpace, freeText, sizeof(freeText));

    _snprintf(out, outSize, "This needs %s, but %s has %s free.", neededText, drive, freeText);
    out[outSize - 1] = '\0';
}

static void SyncView(QueueJob &job)
{
    job.view.id = job.id;
    job.view.kind = job.kind;
    job.view.state = job.state;
    job.view.outcome = job.outcome;
}

// ---------------------------------------------------------------------------
// Worker side - everything below until the public API runs on the worker
// ---------------------------------------------------------------------------

static bool ActiveJobCancelled()
{
    EnterCriticalSection(&g_lock);
    bool cancelled = g_shutdown || (g_activeSlot >= 0 && g_jobs[g_activeSlot].cancelRequested);
    LeaveCriticalSection(&g_lock);
    return cancelled;
}

static void SetActivePhase(const char *phase, int fileIndex, int fileCount, float fraction)
{
    EnterCriticalSection(&g_lock);
    if (g_activeSlot >= 0)
    {
        QueueJob &job = g_jobs[g_activeSlot];
        job.fileIndex = fileIndex;
        job.fileCount = fileCount;
        CopyText(job.view.phase, sizeof(job.view.phase), phase);
        job.view.bytesDone = 0;
        job.view.bytesTotal = 0;
        job.view.bytesPerSec = 0;
        job.view.secondsLeft = 0;
        job.view.fraction = fraction;
    }
    LeaveCriticalSection(&g_lock);
}

// From downloadFile.cpp's read loop, about every 100ms. Kept to a few stores
// under the lock - this runs between socket reads, so anything slow here
// costs download speed.
static bool WorkerDownloadProgress(unsigned long long bytesDone, unsigned long long bytesTotal,
                                   unsigned long long bytesPerSec, unsigned long long secondsRemaining)
{
    bool keepGoing = true;

    EnterCriticalSection(&g_lock);
    if (g_activeSlot >= 0)
    {
        QueueJob &job = g_jobs[g_activeSlot];

        job.view.bytesDone = bytesDone;
        job.view.bytesTotal = bytesTotal;
        job.view.bytesPerSec = bytesPerSec;
        job.view.secondsLeft = secondsRemaining;

        // Across the whole job rather than the current file, so the bar moves
        // forward steadily instead of snapping back to zero on each of a
        // dozen files.
        if (job.fileCount > 0)
        {
            float withinFile = (bytesTotal > 0) ? (float)((double)bytesDone / (double)bytesTotal) : 0.0f;
            job.view.fraction = ((float)job.fileIndex + withinFile) / (float)job.fileCount;
        }

        if (job.kind == QUEUE_JOB_TITLE_UPDATE)
            CopyText(job.view.phase, sizeof(job.view.phase), "Downloading");

        keepGoing = !job.cancelRequested && !g_shutdown;
    }
    LeaveCriticalSection(&g_lock);

    return keepGoing;
}

// After each header ListDlcMembers reads. The fraction is how far through the
// archive the walk is - it moves in uneven jumps, since each step skips a
// whole member's data.
static bool WorkerListProgress(unsigned long long bytesScanned, unsigned long long archiveSize,
                               int filesToInstall, int avatarItemsSkipped)
{
    (void)filesToInstall;
    (void)avatarItemsSkipped;

    bool keepGoing = true;

    EnterCriticalSection(&g_lock);
    if (g_activeSlot >= 0)
    {
        QueueJob &job = g_jobs[g_activeSlot];
        job.view.fraction = (archiveSize > 0) ? (float)((double)bytesScanned / (double)archiveSize) : -1.0f;
        keepGoing = !job.cancelRequested && !g_shutdown;
    }
    LeaveCriticalSection(&g_lock);

    return keepGoing;
}

// The result of one job, filled in by the Run functions below and stored into
// the job by the worker loop.
struct JobResult
{
    QueueOutcome outcome;
    char text[64];
    char detail[160];
};

static void SetResult(JobResult &result, QueueOutcome outcome, const char *text, const char *detail)
{
    result.outcome = outcome;
    CopyText(result.text, sizeof(result.text), text);
    CopyText(result.detail, sizeof(result.detail), detail);
}

static void SetKeysRejected(JobResult &result)
{
    // Not "could not read the pack" or "the file failed" - those send someone
    // to try a different one, which will be refused the same way.
    SetResult(result, QUEUE_OUTCOME_KEYS_REJECTED, "Keys not accepted",
              "archive.org turned down your keys. Check them in Settings.");
}

// Every file inside one pack. What DownloadOnePack in main.cpp used to do on
// the UI thread, with every message it showed turned into a result instead.
static void RunDlcPack(const QueueJob &job, JobResult &result)
{
    const DlcRarMatch &pack = job.pack;
    const char *auth = job.authHeader;

    SetActivePhase("Reading the file list", 0, 0, -1.0f);

    int memberCount = ListDlcMembers(pack, g_members, MAX_DLC_MEMBERS, auth, dprintf, WorkerListProgress);

    if (ActiveJobCancelled())
    {
        SetResult(result, QUEUE_OUTCOME_CANCELLED, "Cancelled", "Nothing was installed.");
        return;
    }

    // Just one file of the pack - the indie games' runtime update, out of
    // the RAR that carries it: the rest of the list is left out.
    if (pack.only[0] != '\0' && memberCount > 0)
    {
        int kept = 0;
        for (int f = 0; f < memberCount; ++f)
        {
            const char *slash = strrchr(g_members[f].contentPath, '\\');
            const char *name = (slash != NULL) ? slash + 1 : g_members[f].contentPath;
            if (_stricmp(name, pack.only) == 0)
                g_members[kept++] = g_members[f];
        }
        if (kept == 0)
            dprintf("%s doesn't hold %s\n", pack.filename, pack.only);
        memberCount = kept;
    }

    if (memberCount <= 0)
    {
        // Reading the file list is the first thing that sends the keys, so
        // this is where wrong ones usually show up.
        if (ArchiveOrgKeysRejected())
            SetKeysRejected(result);
        else
            SetResult(result, QUEUE_OUTCOME_FAILED, "Couldn't read the pack",
                      "The file list for this pack could not be read.");
        return;
    }

    // Checked once for the whole pack, before any of it downloads: running out
    // partway would leave a pack half installed, after a long wait. Files
    // already on the console don't count - they won't be fetched again.
    unsigned long long needed = 0;
    for (int f = 0; f < memberCount; ++f)
    {
        if (!DlcMemberIsInstalled(g_members[f], g_contentBasePath))
            needed += g_members[f].unpSize;
    }

    unsigned long long freeSpace = 0;
    if (needed > 0 && DriveFreeSpace(g_contentBasePath, &freeSpace) && freeSpace < needed + PACK_SPACE_MARGIN)
    {
        dprintf("Not enough space for %s: needs %I64u bytes, %I64u free\n", pack.filename, needed, freeSpace);
        char detail[160];
        NoSpaceDetail(detail, sizeof(detail), needed, freeSpace);
        SetResult(result, QUEUE_OUTCOME_NO_SPACE, "Not enough space", detail);
        return;
    }

    int failures = 0;
    int alreadyThere = 0;
    int installedNow = 0;

    for (int f = 0; f < memberCount; ++f)
    {
        // Already on disk at the right size - skip it. This is what makes
        // queueing a pack again cheap instead of a full re-download, and makes
        // a cancelled pack resume from the file it stopped on. The size check
        // inside DlcMemberIsInstalled is what stops a half-written file from
        // being mistaken for a finished one.
        if (DlcMemberIsInstalled(g_members[f], g_contentBasePath))
        {
            alreadyThere++;
            continue;
        }

        char phase[64];
        _snprintf(phase, sizeof(phase), "File %d of %d", f + 1, memberCount);
        phase[sizeof(phase) - 1] = '\0';
        SetActivePhase(phase, f, memberCount, (float)f / (float)memberCount);

        if (DownloadDlcMember(pack, g_members[f], g_contentBasePath, auth, dprintf,
                              WorkerDownloadProgress))
        {
            installedNow++;
            continue;
        }

        if (ActiveJobCancelled())
        {
            SetResult(result, QUEUE_OUTCOME_CANCELLED, "Cancelled",
                      installedNow > 0 ? "Files that already finished stay installed." : "Nothing was installed.");
            return;
        }

        dprintf("  Failed: %s\n", g_members[f].internalPath);

        // Stop at the first refusal instead of trying the rest - they'd all
        // be refused, one slow round trip each.
        if (ArchiveOrgKeysRejected())
        {
            SetKeysRejected(result);
            return;
        }

        // The rest can't fit either, so stop and say why.
        unsigned long long fileNeeded = 0, fileFree = 0;
        if (ArchiveOrgDiskFull(&fileNeeded, &fileFree))
        {
            char detail[160];
            NoSpaceDetail(detail, sizeof(detail), fileNeeded, fileFree);
            SetResult(result, QUEUE_OUTCOME_NO_SPACE, "Not enough space", detail);
            return;
        }

        failures++;
    }

    if (alreadyThere > 0)
        dprintf("  %d of %d file(s) were already installed\n", alreadyThere, memberCount);

    if (failures == 0 && alreadyThere == memberCount)
    {
        // Nothing was transferred. "Installed" would be true but misleading -
        // it reads as though work happened.
        SetResult(result, QUEUE_OUTCOME_ALREADY_INSTALLED, "Already installed",
                  "Every file in this pack was already on the console.");
    }
    else if (failures == 0)
    {
        // An arcade game shows up in the library straight away, and an indie
        // game is ready to play; DLC needs the game to be started again.
        const char *detail = "Restart your dashboard to pick up the new content.";
        if (strncmp(pack.item, "XBOX_360_XBLIG", 14) == 0)
            detail = "It's on the hard drive, ready to play.";
        else if (pack.item[0] != '\0')
            detail = "It's in your library now.";
        SetResult(result, QUEUE_OUTCOME_INSTALLED, "Installed", detail);
    }
    else
    {
        char detail[160];
        _snprintf(detail, sizeof(detail), "%d of %d files failed to download. See the log.", failures, memberCount);
        detail[sizeof(detail) - 1] = '\0';
        SetResult(result, QUEUE_OUTCOME_PARTIAL, "Finished with errors", detail);
    }
}

static void RunTitleUpdate(const QueueJob &job, JobResult &result)
{
    // One file. The phase becomes "Downloading" with the first progress
    // report, once DownloadTitleUpdate has read the update's own file list.
    SetActivePhase("Reading the update", 0, 1, -1.0f);

    if (DownloadTitleUpdate(job.update, job.view.titleId, g_contentBasePath, job.authHeader, dprintf,
                            WorkerDownloadProgress))
    {
        SetResult(result, QUEUE_OUTCOME_INSTALLED, "Installed",
                  "Restart your dashboard to pick up the update.");
        return;
    }

    unsigned long long needed = 0, freeSpace = 0;

    if (ActiveJobCancelled())
    {
        SetResult(result, QUEUE_OUTCOME_CANCELLED, "Cancelled", "Nothing was installed.");
    }
    else if (ArchiveOrgKeysRejected())
    {
        SetKeysRejected(result);
    }
    else if (ArchiveOrgDiskFull(&needed, &freeSpace))
    {
        char detail[160];
        NoSpaceDetail(detail, sizeof(detail), needed, freeSpace);
        SetResult(result, QUEUE_OUTCOME_NO_SPACE, "Not enough space", detail);
    }
    else
    {
        SetResult(result, QUEUE_OUTCOME_FAILED, "Failed",
                  "See the log - archive.org may not serve this file directly.");
    }
}

// Under the lock. The job's slot stays put while it's active - RemoveQueueJob
// refuses unfinished jobs - so the worker can write back to it afterwards.
//
// notify is whether it deserves a popup: not for a cancel, which the user did
// themselves, nor for the jobs failed alongside a refused one.
static void FinishJob(QueueJob &job, const JobResult &result, bool notify)
{
    job.state = QUEUE_FINISHED;
    job.outcome = result.outcome;
    job.finishedSeq = g_nextFinishedSeq++;
    job.handedOver = false;
    job.view.notify = notify;
    CopyText(job.view.resultText, sizeof(job.view.resultText), result.text);
    CopyText(job.view.resultDetail, sizeof(job.view.resultDetail), result.detail);
    job.view.phase[0] = '\0';
    SyncView(job);
}

static DWORD WINAPI WorkerEntry(LPVOID)
{
    // A copy of the job being run, so the long-running work reads its inputs
    // without holding the lock. Static, like g_members: only this thread
    // touches it, and it keeps a couple of KB off the worker's stack.
    static QueueJob work;

    for (;;)
    {
        int slot = -1;

        EnterCriticalSection(&g_lock);

        if (g_shutdown)
        {
            LeaveCriticalSection(&g_lock);
            break;
        }

        // The oldest waiting job.
        for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
        {
            if (g_jobs[i].used && g_jobs[i].state == QUEUE_WAITING &&
                (slot < 0 || g_jobs[i].sequence < g_jobs[slot].sequence))
                slot = i;
        }

        if (slot >= 0)
        {
            g_jobs[slot].state = QUEUE_ACTIVE;
            SyncView(g_jobs[slot]);
            g_activeSlot = slot;
            work = g_jobs[slot];
        }

        LeaveCriticalSection(&g_lock);

        if (slot < 0)
        {
            // Nothing to do until a job is added (or the app is leaving).
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }

        dprintf("[queue] starting job %d: %s\n", work.id, work.view.title);

        JobResult result;
        SetResult(result, QUEUE_OUTCOME_FAILED, "Failed", "");

        if (work.kind == QUEUE_JOB_DLC_PACK)
            RunDlcPack(work, result);
        else
            RunTitleUpdate(work, result);

        dprintf("[queue] job %d finished: %s - %s\n", work.id, result.text, result.detail);

        EnterCriticalSection(&g_lock);

        // A cancel is the user's own doing - it gets no popup.
        FinishJob(g_jobs[slot], result, result.outcome != QUEUE_OUTCOME_CANCELLED);
        g_activeSlot = -1;

        // Refused keys: everything still waiting was queued with the same
        // keys and would be refused the same way, one slow round trip each.
        // Failed now, with one popup for the lot - the fix is in Settings,
        // and they can be queued again afterwards.
        if (result.outcome == QUEUE_OUTCOME_KEYS_REJECTED)
        {
            for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
            {
                if (g_jobs[i].used && g_jobs[i].state == QUEUE_WAITING &&
                    strcmp(g_jobs[i].authHeader, work.authHeader) == 0)
                    FinishJob(g_jobs[i], result, false);
            }
        }

        LeaveCriticalSection(&g_lock);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Public API - called from the UI thread
// ---------------------------------------------------------------------------

bool StartDownloadQueue(const char *contentBasePath)
{
    if (g_running)
        return true;

    InitializeCriticalSection(&g_lock);
    memset(g_jobs, 0, sizeof(g_jobs));
    CopyText(g_contentBasePath, sizeof(g_contentBasePath), contentBasePath);

    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL); // auto-reset
    if (g_wake == NULL)
    {
        dprintf("[queue] CreateEvent failed (%lu)\n", GetLastError());
        return false;
    }

    // 256KB of stack. The HTTP and TLS layers keep several KB of buffers on
    // the stack per request (DumpResponse's request text alone is 5KB), and
    // BearSSL's handshake needs its own; the default would be cutting it
    // close for no saving worth having.
    g_thread = CreateThread(NULL, 256 * 1024, WorkerEntry, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread == NULL)
    {
        dprintf("[queue] CreateThread failed (%lu)\n", GetLastError());
        CloseHandle(g_wake);
        g_wake = NULL;
        return false;
    }

#ifdef _XBOX
    // Off the UI thread's core, so drawing a frame never holds up a socket
    // read. Hardware thread 4 is core 2's first; the disc install's
    // read-ahead uses 2, so the two never share a core either.
    XSetThreadProcessor(g_thread, 4);
#endif

    g_running = true;
    ResumeThread(g_thread);

    dprintf("[queue] download worker started\n");
    return true;
}

bool StopDownloadQueue(DWORD timeoutMs)
{
    if (!g_running)
        return true;

    EnterCriticalSection(&g_lock);
    g_shutdown = true;
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].state != QUEUE_FINISHED)
            g_jobs[i].cancelRequested = true;
    }
    LeaveCriticalSection(&g_lock);

    SetEvent(g_wake);

    bool stopped = (WaitForSingleObject(g_thread, timeoutMs) == WAIT_OBJECT_0);
    if (stopped)
    {
        CloseHandle(g_thread);
        g_thread = NULL;
        g_running = false;
        dprintf("[queue] download worker stopped\n");
    }
    else
    {
        // Left running rather than killed: it's blocked on archive.org, and
        // the app is about to end anyway, which takes the thread with it.
        dprintf("[queue] download worker still busy after %lums - leaving anyway\n", timeoutMs);
    }

    return stopped;
}

// Under the lock. True if the same file is already waiting or downloading.
static bool AlreadyQueued(QueueJobKind kind, const char *filename)
{
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        const QueueJob &job = g_jobs[i];
        if (job.used && job.kind == kind && job.state != QUEUE_FINISHED &&
            strcmp(job.view.title, filename) == 0)
            return true;
    }
    return false;
}

// Under the lock. A free slot - or, when every slot is taken, the oldest
// finished job's, since a finished job is only there to be read.
static int FreeSlot()
{
    int oldestFinished = -1;

    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        if (!g_jobs[i].used)
            return i;

        if (g_jobs[i].state == QUEUE_FINISHED && g_jobs[i].handedOver &&
            (oldestFinished < 0 || g_jobs[i].finishedSeq < g_jobs[oldestFinished].finishedSeq))
            oldestFinished = i;
    }

    return oldestFinished;
}

static EnqueueResult Enqueue(QueueJobKind kind, const DlcRarMatch *pack, const TitleUpdateMatch *update,
                             const char *gameName, unsigned long titleId, const char *authHeader)
{
    if (!g_running)
        return ENQUEUE_UNAVAILABLE;

    // What the row is called, and what's checked for being queued already:
    // the pack's file - or for one file of a pack, its own name (gameName),
    // so it doesn't stand in for the whole pack.
    const char *filename = (kind == QUEUE_JOB_DLC_PACK) ? (pack->only[0] != '\0' ? gameName : pack->filename)
                                                        : update->filename;

    EnterCriticalSection(&g_lock);

    if (AlreadyQueued(kind, filename))
    {
        LeaveCriticalSection(&g_lock);
        return ENQUEUE_ALREADY_QUEUED;
    }

    int slot = FreeSlot();
    if (slot < 0)
    {
        LeaveCriticalSection(&g_lock);
        return ENQUEUE_FULL;
    }

    QueueJob &job = g_jobs[slot];
    memset(&job, 0, sizeof(job));

    job.used = true;
    job.id = g_nextId++;
    job.sequence = g_nextSequence++;
    job.kind = kind;
    job.state = QUEUE_WAITING;
    job.outcome = QUEUE_OUTCOME_NONE;

    if (pack != NULL)
        job.pack = *pack;
    if (update != NULL)
        job.update = *update;
    CopyText(job.authHeader, sizeof(job.authHeader), authHeader);

    job.view.titleId = titleId;
    CopyText(job.view.gameName, sizeof(job.view.gameName), gameName);
    CopyText(job.view.title, sizeof(job.view.title), filename);
    job.view.fraction = -1.0f;
    SyncView(job);

    LeaveCriticalSection(&g_lock);

    SetEvent(g_wake);
    return ENQUEUE_ADDED;
}

EnqueueResult EnqueueDlcPack(const DlcRarMatch &pack, const char *gameName, unsigned long titleId,
                             const char *authHeader)
{
    return Enqueue(QUEUE_JOB_DLC_PACK, &pack, NULL, gameName, titleId, authHeader);
}

EnqueueResult EnqueueTitleUpdate(const TitleUpdateMatch &update, const char *gameName, unsigned long titleId,
                                 const char *authHeader)
{
    return Enqueue(QUEUE_JOB_TITLE_UPDATE, NULL, &update, gameName, titleId, authHeader);
}

// Display order: active, then waiting by sequence, then finished newest first.
static int DisplayRank(const QueueJob &job)
{
    switch (job.state)
    {
    case QUEUE_ACTIVE:  return 0;
    case QUEUE_WAITING: return 1;
    default:            return 2;
    }
}

static bool ListsBefore(const QueueJob &a, const QueueJob &b)
{
    int ra = DisplayRank(a), rb = DisplayRank(b);
    if (ra != rb)
        return ra < rb;
    if (ra == 2)
        return a.finishedSeq > b.finishedSeq;
    return a.sequence < b.sequence;
}

int SnapshotDownloadQueue(QueueJobSnapshot *out, int maxJobs)
{
    if (!g_running || out == NULL || maxJobs <= 0)
        return 0;

    int order[MAX_QUEUE_JOBS];
    int count = 0;

    EnterCriticalSection(&g_lock);

    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        if (!g_jobs[i].used)
            continue;

        // Insertion sort - there are at most 32.
        int at = count;
        while (at > 0 && ListsBefore(g_jobs[i], g_jobs[order[at - 1]]))
        {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
        count++;
    }

    if (count > maxJobs)
        count = maxJobs;

    for (int i = 0; i < count; ++i)
        out[i] = g_jobs[order[i]].view;

    LeaveCriticalSection(&g_lock);

    return count;
}

int PendingDownloadCount()
{
    if (!g_running)
        return 0;

    int pending = 0;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].state != QUEUE_FINISHED)
            pending++;
    }
    LeaveCriticalSection(&g_lock);

    return pending;
}

void CancelQueueJob(int id)
{
    if (!g_running)
        return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        QueueJob &job = g_jobs[i];
        if (!job.used || job.id != id)
            continue;

        if (job.state == QUEUE_WAITING)
        {
            // Never started, so it can finish right here.
            JobResult result;
            SetResult(result, QUEUE_OUTCOME_CANCELLED, "Cancelled", "Nothing was installed.");
            FinishJob(job, result, false);
        }
        else if (job.state == QUEUE_ACTIVE)
        {
            job.cancelRequested = true; // the worker finishes it
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
}

void RemoveQueueJob(int id)
{
    if (!g_running)
        return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        QueueJob &job = g_jobs[i];
        // Only once it's been handed over, so its popup and the refresh it
        // triggers can't be lost. main.cpp takes finished jobs at the top of
        // every frame, before it reads the controller, so in practice that is
        // always true by the time anyone can press the button.
        if (job.used && job.id == id && job.state == QUEUE_FINISHED && job.handedOver)
        {
            job.used = false;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
}

bool TakeFinishedQueueJob(QueueJobSnapshot *out)
{
    if (!g_running || out == NULL)
        return false;

    int found = -1;

    EnterCriticalSection(&g_lock);

    // Oldest first - the quiet ones included. They get no popup, but their
    // files may still have changed what's on disk.
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        const QueueJob &job = g_jobs[i];
        if (job.used && job.state == QUEUE_FINISHED && !job.handedOver &&
            (found < 0 || job.finishedSeq < g_jobs[found].finishedSeq))
            found = i;
    }

    if (found >= 0)
    {
        *out = g_jobs[found].view;
        g_jobs[found].handedOver = true;
    }

    LeaveCriticalSection(&g_lock);

    return found >= 0;
}
