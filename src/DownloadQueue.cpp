/*
FILE : DownloadQueue.cpp
PROJECT : Omni360
DESCRIPTION : the download queue - DLC packs and title updates, installed one
              at a time on a worker thread so the UI stays usable while they
              download. See DownloadQueue.h.
*/

#include "DownloadQueue.h"
#include "downloadFile.h" // FormatBytes, DownloadProgressFn
#include "HttpPlain.h"    // HttpDownloadPlain, for a homebrew mirror
#include "SevenZip.h"
#include "QueueState.h"   // the jobs, kept between runs
#include "OutputConsole.h"
#include "zlib/zlib.h"    // crc32, to name a download's staging file

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
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
    bool pauseRequested; // stop like a cancel, but keep the job - see FinishJob
    bool handedOver;     // finished, and already returned by TakeFinishedQueueJob

    DlcRarMatch pack;          // QUEUE_JOB_DLC_PACK
    TitleUpdateMatch update;   // QUEUE_JOB_TITLE_UPDATE
    HomebrewJobSpec homebrew;  // QUEUE_JOB_HOMEBREW
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
// Where the job being run installs to. Written only by the worker, when it
// picks a job up, from g_settingsContentPath - so a path changed in Settings
// halfway through a download can't move the rest of that download, and the
// next job gets the new one.
static char g_contentBasePath[512] = "";

// The content path as Settings has it. Written by the UI thread under the
// lock; read by the worker at job pickup.
static char g_settingsContentPath[512] = "";

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
// The queue, kept between runs - see QueueState.h
// ---------------------------------------------------------------------------

#define QUEUE_STATE_FILE "game:\\QueuedDownloads.txt"
#define QUEUE_STATE_MAX  (96 * 1024) // 32 jobs, a homebrew spec being the longest

// How many finished rows are kept, the oldest dropped. The jobs still to do
// are written first and are never dropped for a record's sake.
#define MAX_FINISHED_KEPT 12

// Under the lock. Writes every job still to do, which is what the next run
// reads, and a record of each one that finished. Nothing is written while the
// app is closing: closing cancels every job, and the file has to keep them.
static void SaveQueueStateLocked()
{
    if (!g_running || g_shutdown)
        return;

    char *text = (char *)malloc(QUEUE_STATE_MAX);
    if (text == NULL)
        return;

    size_t at = 0;
    text[0] = '\0';
    AppendQueueStateText(text, QUEUE_STATE_MAX, &at,
                         "# Omni360 - the downloads still to do when this was written\nformat=1\n");

    int jobs = 0;
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        const QueueJob &job = g_jobs[i];
        if (!job.used || job.state == QUEUE_FINISHED)
            continue;

        // pauseRequested counts as paused even before the transfer has
        // noticed: the app closing in between must not restart it.
        const int paused = (job.state == QUEUE_PAUSED || job.pauseRequested) ? 1 : 0;

        if (job.kind == QUEUE_JOB_DLC_PACK)
        {
            AppendQueueStateText(text, QUEUE_STATE_MAX, &at,
                                 "job=dlc\ngame=%s\ntitleid=%08lX\nitem=%s\nonly=%s\nfile=%s\nsize=%I64u\nscore=%d\n",
                                 job.view.gameName, job.view.titleId, job.pack.item, job.pack.only,
                                 job.pack.filename, job.pack.size, job.pack.score);
        }
        else if (job.kind == QUEUE_JOB_TITLE_UPDATE)
        {
            AppendQueueStateText(text, QUEUE_STATE_MAX, &at,
                                 "job=update\ngame=%s\ntitleid=%08lX\nfile=%s\nsize=%I64u\nscore=%d\nver=%d\n"
                                 "region=%s\n",
                                 job.view.gameName, job.view.titleId, job.update.filename, job.update.size,
                                 job.update.score, job.update.version, job.update.region);
        }
        else if (job.kind == QUEUE_JOB_HOMEBREW)
        {
            AppendQueueStateText(text, QUEUE_STATE_MAX, &at, "job=homebrew\ntitle=%s\nkind=%s\nfolder=%s\nmirror=%s\n",
                                 job.homebrew.title, job.homebrew.kindFolder, job.homebrew.folder,
                                 job.homebrew.mirror);
            for (int u = 0; u < job.homebrew.urlCount && u < HOMEBREW_MAX_URLS; ++u)
                AppendQueueStateText(text, QUEUE_STATE_MAX, &at, "url=%s\n", job.homebrew.urls[u]);
        }
        else
        {
            continue; // disc and game installs are their own workers' to keep
        }

        if (paused)
            AppendQueueStateText(text, QUEUE_STATE_MAX, &at, "paused=1\n");

        jobs++;
    }

    // Then what finished, as records rather than as jobs. The difference is
    // the whole point. A job put back is a job run again, and one that failed
    // would fail again on every launch - which is why finished jobs were left
    // out of this file to begin with. A record is never run: it is the row
    // saying what happened, and nothing more.
    //
    // Without them, a night of downloads ends with the console off, a game
    // not installed and nothing on screen saying why. A crash or the power
    // going at the wall loses them the same way, so this is worth having
    // whether or not anything switches the console off on purpose.
    //
    // Cancelled ones are left out: the user did that one and knows.
    int order[MAX_QUEUE_JOBS];
    int records = 0;

    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        const QueueJob &job = g_jobs[i];
        if (!job.used || job.state != QUEUE_FINISHED || job.outcome == QUEUE_OUTCOME_CANCELLED)
            continue;
        if (job.kind != QUEUE_JOB_DLC_PACK && job.kind != QUEUE_JOB_TITLE_UPDATE && job.kind != QUEUE_JOB_HOMEBREW)
            continue;

        // Kept in the order they finished as they are found. There are 32
        // slots at the most, so an insertion sort is the whole of it.
        int pos = records;
        while (pos > 0 && g_jobs[order[pos - 1]].finishedSeq > job.finishedSeq)
        {
            order[pos] = order[pos - 1];
            pos--;
        }
        order[pos] = i;
        records++;
    }

    // The newest MAX_FINISHED_KEPT of them, written oldest first so that
    // reading the file back in order gives them their finished order again.
    int first = records - MAX_FINISHED_KEPT;
    if (first < 0)
        first = 0;

    for (int i = first; i < records; ++i)
    {
        const QueueJob &job = g_jobs[order[i]];
        AppendQueueStateText(text, QUEUE_STATE_MAX, &at,
                             "done=%s\ngame=%s\ntitleid=%08lX\ntitle=%s\noutcome=%d\nresult=%s\ndetail=%s\n",
                             job.kind == QUEUE_JOB_DLC_PACK       ? "dlc"
                             : job.kind == QUEUE_JOB_TITLE_UPDATE ? "update"
                                                                  : "homebrew",
                             job.view.gameName, job.view.titleId, job.view.title, (int)job.outcome,
                             job.view.resultText, job.view.resultDetail);
    }

    if (jobs > 0 || records > 0)
        WriteQueueStateFile(QUEUE_STATE_FILE, text);
    else
        DeleteQueueStateFile(QUEUE_STATE_FILE);

    free(text);
}

// ---------------------------------------------------------------------------
// Worker side - everything below until the public API runs on the worker
// ---------------------------------------------------------------------------

// Under the lock. Whether this job should stop what it is doing, for any
// reason. Every progress callback asks this and nothing else: a callback
// returning false is the only thing that stops a transfer, so one that tests
// a flag of its own instead is a pause or a cancel that does nothing but
// change the words on screen.
static bool JobShouldStop(const QueueJob &job)
{
    return job.cancelRequested || job.pauseRequested || g_shutdown;
}

// Why the job in hand is stopping: for now - the app closing, or the user
// pausing it - rather than for good. What a half-finished download leaves
// behind turns on this, the same way a game install's staging does.
static bool ActiveJobStoppingForNow()
{
    EnterCriticalSection(&g_lock);
    const bool forNow = g_shutdown || (g_activeSlot >= 0 && g_jobs[g_activeSlot].pauseRequested &&
                                       !g_jobs[g_activeSlot].cancelRequested);
    LeaveCriticalSection(&g_lock);
    return forNow;
}

// True once the job in hand should stop, whether for good or for now: a
// pause stops the transfer exactly as a cancel does, and FinishJob is where
// the two part company.
static bool ActiveJobCancelled()
{
    EnterCriticalSection(&g_lock);
    bool cancelled = g_shutdown || (g_activeSlot >= 0 && JobShouldStop(g_jobs[g_activeSlot]));
    LeaveCriticalSection(&g_lock);
    return cancelled;
}

// A DLC pack's two steps, and each one's share of the whole job's
// percentage - and of the Queue's bar (SnapshotQueue's kPackWidths): reading
// the file list walks the archive's headers, a request each; the files are
// the rest. A title update is one step.
enum PackStep
{
    PACK_STEP_LISTING,
    PACK_STEP_FILES,
    PACK_STEP_COUNT
};
static const float kPackStepShares[PACK_STEP_COUNT] = { 0.10f, 0.90f };

// The active job's progress, under the lock: as a pack's step, or as the
// whole of a title update.
static void SetJobProgress(QueueJob &job, float stepFraction)
{
    job.view.stepFraction = stepFraction;
    job.view.fraction = (job.view.stepCount > 1) ? JobFraction(kPackStepShares, job.view.step, stepFraction)
                                                 : stepFraction;
}

// stepCount is PACK_STEP_COUNT for a pack, or 1 for a title update.
static void SetActivePhase(const char *phase, int step, int stepCount, int fileIndex, int fileCount, float stepFraction)
{
    EnterCriticalSection(&g_lock);
    if (g_activeSlot >= 0)
    {
        QueueJob &job = g_jobs[g_activeSlot];
        job.fileIndex = fileIndex;
        job.fileCount = fileCount;

        // Asked to pause, and the transfer hasn't come back round to noticing
        // yet. Say so, rather than go on reporting a download the user has
        // already stopped.
        CopyText(job.view.phase, sizeof(job.view.phase),
                 (job.pauseRequested && !job.cancelRequested) ? "Pausing" : phase);
        job.view.bytesDone = 0;
        job.view.bytesTotal = 0;
        job.view.bytesPerSec = 0;
        job.view.secondsLeft = 0;
        job.view.step = step;
        job.view.stepCount = stepCount;
        SetJobProgress(job, stepFraction);
    }
    LeaveCriticalSection(&g_lock);
}

// From downloadFile.cpp's read loop, about every 100ms. Kept to a few stores
// under the lock - this runs between socket reads, so anything slow here
// costs download speed.
// A resumed transfer reports only the range it asked for. This is what was
// already on the drive before it started, added back so the bar carries on
// from where it stopped rather than starting over. Worker-only, like
// everything else in this section, and zero for a transfer from the start.
static unsigned long long g_progressBase = 0;

static bool WorkerDownloadProgress(unsigned long long bytesDone, unsigned long long bytesTotal,
                                   unsigned long long bytesPerSec, unsigned long long secondsRemaining)
{
    bool keepGoing = true;

    bytesDone += g_progressBase;
    if (bytesTotal > 0)
        bytesTotal += g_progressBase;

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
            SetJobProgress(job, ((float)job.fileIndex + withinFile) / (float)job.fileCount);
        }

        keepGoing = !JobShouldStop(job);
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
        SetJobProgress(job, (archiveSize > 0) ? (float)((double)bytesScanned / (double)archiveSize) : -1.0f);
        keepGoing = !JobShouldStop(job);
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

    SetActivePhase("Reading the file list", PACK_STEP_LISTING, PACK_STEP_COUNT, 0, 0, -1.0f);

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
    // already on the console don't count - they won't be fetched again. A
    // file unpacked here has its download on the drive beside it until it's
    // done, one file at a time, so the biggest of those counts too.
    unsigned long long needed = 0;
    unsigned long long biggestDownload = 0;
    for (int f = 0; f < memberCount; ++f)
    {
        if (DlcMemberIsInstalled(g_members[f], g_contentBasePath))
            continue;
        needed += g_members[f].unpSize;
        if (DlcMemberUnpackedHere(g_members[f]) && DlcMemberDownloadSize(g_members[f]) > biggestDownload)
            biggestDownload = DlcMemberDownloadSize(g_members[f]);
    }
    needed += biggestDownload;

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
    int tooLarge = 0;
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
        SetActivePhase(phase, PACK_STEP_FILES, PACK_STEP_COUNT, f, memberCount, (float)f / (float)memberCount);

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

        // Too big for archive.org to send - the rest of the pack may not be,
        // so carry on with it.
        if (ArchiveOrgDlcTooLarge())
            tooLarge++;

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
    else if (tooLarge == failures)
    {
        // Every failure was a file over 1 GiB that archive.org only serves
        // unpacked, and its unpacking gives out at 1 GiB for a console.
        char detail[160];
        if (failures == memberCount)
            _snprintf(detail, sizeof(detail), "This DLC is over 1 GB, too big for archive.org to send to a console.");
        else
            _snprintf(detail, sizeof(detail), "%d of %d files are over 1 GB, too big for archive.org to send to a console.",
                      failures, memberCount);
        detail[sizeof(detail) - 1] = '\0';
        SetResult(result, failures == memberCount ? QUEUE_OUTCOME_FAILED : QUEUE_OUTCOME_PARTIAL,
                  failures == memberCount ? "Too big to download" : "Finished with errors", detail);
    }
    else
    {
        char detail[160];
        _snprintf(detail, sizeof(detail), "%d of %d files failed to download. See the log.", failures, memberCount);
        detail[sizeof(detail) - 1] = '\0';
        SetResult(result, QUEUE_OUTCOME_PARTIAL, "Finished with errors", detail);
    }
}

// A title update is the zip downloaded and then unpacked here, so its bar has
// a segment for each. Which one it is on is the one thing the worker can't
// see from outside DownloadTitleUpdate, and this is the only code of ours
// that runs while it is in there - so it asks, and sets the phase when the
// answer changes rather than on every report.
static int g_updateStep = -1;

static bool TitleUpdateProgress(unsigned long long bytesDone, unsigned long long bytesTotal,
                                unsigned long long bytesPerSec, unsigned long long secondsRemaining)
{
    const int step = ArchiveOrgUnpacking() ? 1 : 0;
    if (step != g_updateStep)
    {
        g_updateStep = step;
        SetActivePhase(step == 1 ? "Unpacking" : "Downloading", step, 2, step, 2, (float)step / 2.0f);
    }

    return WorkerDownloadProgress(bytesDone, bytesTotal, bytesPerSec, secondsRemaining);
}

static void RunTitleUpdate(const QueueJob &job, JobResult &result)
{
    // The phase becomes "Downloading" with the first progress report, once
    // DownloadTitleUpdate has read the update's own file list.
    g_updateStep = -1;
    SetActivePhase("Reading the update", 0, 2, 0, 2, -1.0f);

    if (DownloadTitleUpdate(job.update, job.view.titleId, g_contentBasePath, job.authHeader, dprintf,
                            TitleUpdateProgress))
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

static bool WorkerUnpackProgress(unsigned long long, unsigned long long)
{
    return !ActiveJobCancelled();
}

// The last part of a URL: "RetroArch-360.7z".
static const char *UrlFileName(const char *url)
{
    const char *slash = strrchr(url, '/');
    return slash != NULL ? slash + 1 : url;
}

static unsigned long long FileSizeOf(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attrs))
        return 0;
    return ((unsigned long long)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow;
}

// Where one archive's download waits. A name of its own, from the URL, so a
// partial left behind can never be taken for another archive's - including
// the next part of this same install, which used to share the one name.
// FATX allows 42 characters, which these file names go past, so it is the
// URL's checksum rather than its name.
static void HomebrewStagingPath(const char *kindFolder, const char *url, char *out, size_t outSize)
{
    unsigned long key = crc32(0L, Z_NULL, 0);
    key = crc32(key, (const Bytef *)url, (uInt)strlen(url));
    _snprintf(out, outSize, "%s\\Omni360-%08lX.7z", kindFolder, key);
    out[outSize - 1] = '\0';
}

// Each archive downloaded beside where it's going, unpacked, and deleted.
//
// A download that stops part way is kept, and the next run of the job asks
// ConsoleMods for the rest with a Range request rather than fetching the
// whole thing again - measured at 287 MB for the largest of them, which is a
// couple of minutes at the speed these consoles get. consolemods.org answers
// a Range request with a 206 and the right Content-Range, so this is a plain
// resume; the LAN test mirror's client has none, and a folder served over
// the LAN doesn't need one.
static void RunHomebrew(const QueueJob &job, JobResult &result)
{
    const HomebrewJobSpec &hb = job.homebrew;
    char archive[600], dest[600];
    _snprintf(dest, sizeof(dest), "%s\\%s", hb.kindFolder, hb.folder);
    dest[sizeof(dest) - 1] = '\0';
    CreateDirectoryA(hb.kindFolder, NULL); // the first install of its kind; already there is fine
    int skippedNames = 0;

    // Before these had a name each, every homebrew download went through one
    // shared "Omni360-download.7z". A build from then that stopped part way
    // left one behind that nothing will ever pick up.
    _snprintf(archive, sizeof(archive), "%s\\Omni360-download.7z", hb.kindFolder);
    archive[sizeof(archive) - 1] = '\0';
    remove(archive);

    for (int i = 0; i < hb.urlCount; ++i)
    {
        char url[600];
        if (hb.mirror[0] != '\0')
            _snprintf(url, sizeof(url), "%s/%s", hb.mirror, UrlFileName(hb.urls[i]));
        else
            _snprintf(url, sizeof(url), "%s", hb.urls[i]);
        url[sizeof(url) - 1] = '\0';

        char phase[64];
        if (hb.urlCount > 1)
            _snprintf(phase, sizeof(phase), "Downloading part %d of %d", i + 1, hb.urlCount);
        else
            _snprintf(phase, sizeof(phase), "Downloading");
        phase[sizeof(phase) - 1] = '\0';
        SetActivePhase(phase, 0, 1, i, hb.urlCount, (float)i / (float)hb.urlCount);

        dprintf("[homebrew] %s: %s\n", hb.title, url);
        HomebrewStagingPath(hb.kindFolder, hb.urls[i], archive, sizeof(archive));

        int status = 0;
        if (_strnicmp(url, "http://", 7) == 0)
        {
            // The LAN mirror: no Range, so always from the start.
            remove(archive);
            status = HttpDownloadPlain(url, archive, WorkerDownloadProgress);
        }
        else
        {
            const unsigned long long staged = FileSizeOf(archive);
            char range[64];

            if (staged > 0)
            {
                _snprintf(range, sizeof(range), "Range: bytes=%I64u-\r\n", staged);
                range[sizeof(range) - 1] = '\0';
                dprintf("[homebrew] %I64u bytes of %s are already here - asking for the rest\n", staged,
                        UrlFileName(url));
            }

            g_progressBase = staged; // so the bar carries on rather than restarting
            status = httpRequestHTTPS(url, HTTP_GET, NULL, staged > 0 ? range : NULL, archive, NULL, NULL, true,
                                      NULL, 0, dprintf, 0, WorkerDownloadProgress, staged > 0);
            g_progressBase = 0;

            // 416 is the server saying there is nothing past what's here:
            // the archive was already whole when it stopped, and only the
            // unpacking is left. Anything else that isn't the range asked
            // for - a server that ignored Range and answered 200, which
            // httpRequestHTTPS refuses to append over a partial file -
            // starts the download over, once.
            if (staged > 0 && status == 416)
            {
                dprintf("[homebrew] %s was already complete\n", UrlFileName(url));
                status = 200;
            }
            else if (staged > 0 && status != 200 && status != 206 && status != HTTP_STATUS_CANCELLED &&
                     !ActiveJobCancelled())
            {
                dprintf("[homebrew] couldn't carry on (status %d) - fetching %s from the start\n", status,
                        UrlFileName(url));
                remove(archive);
                status = httpRequestHTTPS(url, HTTP_GET, NULL, NULL, archive, NULL, NULL, true, NULL, 0, dprintf, 0,
                                          WorkerDownloadProgress);
            }
        }

        if (status != 200 && status != 206)
        {
            // What came down is kept unless the user gave up on it, or the
            // server says this URL will never deliver: a pause, the app
            // closing or a dropped connection all leave a partial the next
            // run carries on from.
            const bool cancelled = (status == HTTP_STATUS_CANCELLED || ActiveJobCancelled());
            if (status == 403 || status == 404 || (cancelled && !ActiveJobStoppingForNow()))
                remove(archive);
            else
                dprintf("[homebrew] keeping %I64u bytes of %s for next time\n", FileSizeOf(archive),
                        UrlFileName(url));

            if (cancelled)
            {
                SetResult(result, QUEUE_OUTCOME_CANCELLED, "Cancelled",
                          i == 0 ? "Nothing was installed." : "Its first parts are installed.");
            }
            else if (status == 403 && hb.mirror[0] == '\0')
            {
                // Omni360 is allowed through the bot protection by its user
                // agent, so a 403 now means that arrangement has lapsed
                // rather than that it was never there.
                SetResult(result, QUEUE_OUTCOME_FAILED, "Refused by ConsoleMods",
                          "consolemods.org turned this download away. Please report it.");
            }
            else
            {
                char detail[160];
                _snprintf(detail, sizeof(detail), "%s - %s %d. See the log.", UrlFileName(url),
                          status > 0 ? "the server said" : "error", status);
                detail[sizeof(detail) - 1] = '\0';
                SetResult(result, QUEUE_OUTCOME_FAILED, "Download failed", detail);
            }
            return;
        }

        SetActivePhase("Unpacking", 0, 1, i + 1, hb.urlCount, (float)(i + 1) / (float)hb.urlCount);
        int leftOut = 0;
        const SevenZipResult unpacked = ExtractSevenZip(archive, dest, hb.folder, WorkerUnpackProgress, &leftOut);
        skippedNames += leftOut;

        if (unpacked == SEVEN_ZIP_CANCELLED)
        {
            // Stopped during the unpacking, with the whole archive on the
            // drive. Set aside for now, that is worth keeping: carrying on
            // asks for a range past its end, is told 416, and unpacks what is
            // already here without fetching any of it again.
            if (!ActiveJobStoppingForNow())
                remove(archive);
            SetResult(result, QUEUE_OUTCOME_CANCELLED, "Cancelled", "Some of its files may already be there.");
            return;
        }

        remove(archive); // unpacked, or no good - either way it is spent
        if (unpacked != SEVEN_ZIP_OK)
        {
            char detail[160];
            _snprintf(detail, sizeof(detail), "%s: %s.", UrlFileName(url), SevenZipResultText(unpacked));
            detail[sizeof(detail) - 1] = '\0';
            SetResult(result, QUEUE_OUTCOME_FAILED, "Couldn't unpack it", detail);
            return;
        }
    }

    if (skippedNames > 0)
    {
        // Names longer than FATX's 42 characters, usually sample ROMs.
        char detail[160];
        _snprintf(detail, sizeof(detail), "%s - %d file%s left out, as the drive can't hold their names.", dest,
                  skippedNames, skippedNames == 1 ? "" : "s");
        detail[sizeof(detail) - 1] = '\0';
        SetResult(result, QUEUE_OUTCOME_INSTALLED, "Installed", detail);
        return;
    }
    SetResult(result, QUEUE_OUTCOME_INSTALLED, "Installed", dest);
}

// Under the lock. The job's slot stays put while it's active - RemoveQueueJob
// refuses unfinished jobs - so the worker can write back to it afterwards.
//
// notify is whether it deserves a popup: not for a cancel, which the user did
// themselves, nor for the jobs failed alongside a refused one.
static void FinishJob(QueueJob &job, const JobResult &result, bool notify)
{
    // A pause stopped the transfer the same way a cancel would, but the job
    // isn't over: it goes back to the queue to wait for the word. Decided
    // here, under the lock, so no frame of the UI ever sees it finished -
    // one that did would raise a popup for a download that is only resting.
    if (result.outcome == QUEUE_OUTCOME_CANCELLED && job.pauseRequested && !job.cancelRequested && !g_shutdown)
    {
        job.state = QUEUE_PAUSED;
        job.view.phase[0] = '\0';
        job.view.fraction = -1.0f;
        SyncView(job);
        SaveQueueStateLocked();
        return;
    }

    job.state = QUEUE_FINISHED;
    job.outcome = result.outcome;
    job.finishedSeq = g_nextFinishedSeq++;
    job.handedOver = false;
    job.view.notify = notify;
    CopyText(job.view.resultText, sizeof(job.view.resultText), result.text);
    CopyText(job.view.resultDetail, sizeof(job.view.resultDetail), result.detail);
    job.view.phase[0] = '\0';
    SyncView(job);

    // One fewer job for the next run to pick up - unless this is the app
    // closing, which SaveQueueStateLocked sits out.
    SaveQueueStateLocked();
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
            CopyText(g_contentBasePath, sizeof(g_contentBasePath), g_settingsContentPath);
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
        else if (work.kind == QUEUE_JOB_HOMEBREW)
            RunHomebrew(work, result);
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
    CopyText(g_settingsContentPath, sizeof(g_settingsContentPath), contentBasePath);

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

void SetDownloadQueueContentPath(const char *contentBasePath)
{
    if (g_running)
        EnterCriticalSection(&g_lock);
    CopyText(g_settingsContentPath, sizeof(g_settingsContentPath),
             contentBasePath != NULL ? contentBasePath : "");
    if (!g_running)
        CopyText(g_contentBasePath, sizeof(g_contentBasePath), g_settingsContentPath);
    if (g_running)
        LeaveCriticalSection(&g_lock);
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

// paused is for jobs coming back from a previous run that were set aside
// then: everything else is queued ready to run.
static EnqueueResult Enqueue(QueueJobKind kind, const DlcRarMatch *pack, const TitleUpdateMatch *update,
                             const char *gameName, unsigned long titleId, const char *authHeader,
                             const HomebrewJobSpec *homebrew = NULL, bool paused = false)
{
    if (!g_running)
        return ENQUEUE_UNAVAILABLE;

    // What the row is called, and what's checked for being queued already:
    // the pack's file - or for one file of a pack, its own name (gameName),
    // so it doesn't stand in for the whole pack - or the homebrew's title.
    const char *filename = (kind == QUEUE_JOB_HOMEBREW) ? homebrew->title
                           : (kind == QUEUE_JOB_DLC_PACK) ? (pack->only[0] != '\0' ? gameName : pack->filename)
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
    job.state = paused ? QUEUE_PAUSED : QUEUE_WAITING;
    job.pauseRequested = paused;
    job.outcome = QUEUE_OUTCOME_NONE;

    if (pack != NULL)
        job.pack = *pack;
    if (update != NULL)
        job.update = *update;
    if (homebrew != NULL)
        job.homebrew = *homebrew;
    CopyText(job.authHeader, sizeof(job.authHeader), authHeader);

    job.view.titleId = titleId;
    CopyText(job.view.gameName, sizeof(job.view.gameName), gameName);
    CopyText(job.view.title, sizeof(job.view.title), filename);
    job.view.fraction = -1.0f;
    SyncView(job);
    SaveQueueStateLocked();

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

static EnqueueResult EnqueueHomebrewSpec(const HomebrewJobSpec &spec, bool paused)
{
    // The row's second line: where it's going.
    char where[600];
    _snprintf(where, sizeof(where), "%s\\%s", spec.kindFolder, spec.folder);
    where[sizeof(where) - 1] = '\0';
    return Enqueue(QUEUE_JOB_HOMEBREW, NULL, NULL, where, 0, "", &spec, paused);
}

EnqueueResult EnqueueHomebrew(const HomebrewJobSpec &spec)
{
    return EnqueueHomebrewSpec(spec, false);
}

// ---------------------------------------------------------------------------
// Picking the queue back up
// ---------------------------------------------------------------------------

// One block's worth of what the file said, filled in a field at a time:
// either a job still to do, or - with finished set - a record of one that is
// already over, which goes back as a row and is never run.
struct RestoredJob
{
    int kind; // -1 until a "job=" or "done=" line says
    bool finished;
    DlcRarMatch pack;
    TitleUpdateMatch update;
    HomebrewJobSpec homebrew;
    char gameName[128];
    unsigned long titleId;
    bool paused;

    // A record's own fields. A job builds its title from the pack or update
    // it names; a record has only what it was called at the time.
    char title[256];
    int outcome;
    char resultText[64];
    char resultDetail[160];
};

static void StartRestored(RestoredJob &job, int kind, bool finished)
{
    memset(&job, 0, sizeof(job));
    job.kind = kind;
    job.finished = finished;
}

// What a "job=" or "done=" line names, or -1 for a kind this build doesn't
// know - a file written by a newer one.
static int RestoredKind(const char *value)
{
    if (strcmp(value, "dlc") == 0)
        return (int)QUEUE_JOB_DLC_PACK;
    if (strcmp(value, "update") == 0)
        return (int)QUEUE_JOB_TITLE_UPDATE;
    if (strcmp(value, "homebrew") == 0)
        return (int)QUEUE_JOB_HOMEBREW;
    return -1;
}

// A record of something that finished last time, put back as a row and
// nothing else.
//
// handedOver is the trap. A finished job that hasn't been handed over is one
// TakeFinishedQueueJob will give main.cpp, which raises its popup, clears
// haveAuth on refused keys and sets a library rescan going. Restoring last
// night's results without it would re-announce every one of them over the
// startup screen and rescan for each. These have been seen already.
static void RestoreFinishedRecord(const RestoredJob &rec)
{
    EnterCriticalSection(&g_lock);

    // The jobs still to do have first claim on the slots: they were restored
    // before this, and a record is only there to be read.
    const int slot = FreeSlot();
    if (slot < 0)
    {
        LeaveCriticalSection(&g_lock);
        return;
    }

    QueueJob &job = g_jobs[slot];
    memset(&job, 0, sizeof(job));

    job.used = true;
    job.id = g_nextId++;
    job.sequence = g_nextSequence++;
    job.finishedSeq = g_nextFinishedSeq++;
    job.kind = (QueueJobKind)rec.kind;
    job.state = QUEUE_FINISHED;
    job.outcome = (QueueOutcome)rec.outcome;
    job.handedOver = true;

    job.view.titleId = rec.titleId;
    CopyText(job.view.gameName, sizeof(job.view.gameName), rec.gameName);
    CopyText(job.view.title, sizeof(job.view.title), rec.title);
    CopyText(job.view.resultText, sizeof(job.view.resultText), rec.resultText);
    CopyText(job.view.resultDetail, sizeof(job.view.resultDetail), rec.resultDetail);
    job.view.fraction = -1.0f;
    job.view.notify = false;
    SyncView(job);

    LeaveCriticalSection(&g_lock);

    NoteRestoredRecord(rec.outcome != QUEUE_OUTCOME_INSTALLED && rec.outcome != QUEUE_OUTCOME_ALREADY_INSTALLED);
}

// Queues what one block of the file described. The jobs go back in the order
// they were written, which is the order they were waiting in.
static void QueueRestored(const RestoredJob &job, const char *authHeader, int *queued)
{
    if (job.kind < 0)
        return;

    if (job.finished)
    {
        RestoreFinishedRecord(job);
        return;
    }

    EnqueueResult result = ENQUEUE_UNAVAILABLE;

    if (job.kind == QUEUE_JOB_DLC_PACK && job.pack.filename[0] != '\0')
        result = Enqueue(QUEUE_JOB_DLC_PACK, &job.pack, NULL, job.gameName, job.titleId, authHeader, NULL, job.paused);
    else if (job.kind == QUEUE_JOB_TITLE_UPDATE && job.update.filename[0] != '\0')
        result = Enqueue(QUEUE_JOB_TITLE_UPDATE, NULL, &job.update, job.gameName, job.titleId, authHeader, NULL,
                         job.paused);
    else if (job.kind == QUEUE_JOB_HOMEBREW && job.homebrew.urlCount > 0)
        result = EnqueueHomebrewSpec(job.homebrew, job.paused);
    else
        return;

    if (result == ENQUEUE_ADDED)
        (*queued)++;
    else
        dprintf("[queue] couldn't put a kept job back (%d)\n", (int)result);
}

void RestoreDownloadQueue(const char *authHeader)
{
    if (!g_running)
        return;

    char *text = ReadQueueStateFile(QUEUE_STATE_FILE);
    if (text == NULL)
        return;

    RestoredJob job;
    StartRestored(job, -1, false);

    int queued = 0;
    char *at = text, *key = NULL, *value = NULL;

    while (NextQueueStateLine(&at, &key, &value))
    {
        // A "job=" or "done=" line ends the block before it and starts the
        // next: one still to do, or a record of one that is over.
        if (strcmp(key, "job") == 0 || strcmp(key, "done") == 0)
        {
            QueueRestored(job, authHeader, &queued);
            StartRestored(job, RestoredKind(value), strcmp(key, "done") == 0);
            continue;
        }

        if (job.kind < 0)
            continue; // the header, or a kind this build doesn't know

        if (strcmp(key, "game") == 0)
            CopyText(job.gameName, sizeof(job.gameName), value);
        else if (strcmp(key, "titleid") == 0)
            job.titleId = strtoul(value, NULL, 16);
        else if (strcmp(key, "paused") == 0)
            job.paused = (atoi(value) != 0);
        else if (job.finished)
        {
            if (strcmp(key, "title") == 0)
                CopyText(job.title, sizeof(job.title), value);
            else if (strcmp(key, "outcome") == 0)
                job.outcome = atoi(value);
            else if (strcmp(key, "result") == 0)
                CopyText(job.resultText, sizeof(job.resultText), value);
            else if (strcmp(key, "detail") == 0)
                CopyText(job.resultDetail, sizeof(job.resultDetail), value);
        }
        else if (job.kind == QUEUE_JOB_DLC_PACK)
        {
            if (strcmp(key, "item") == 0)
                CopyText(job.pack.item, sizeof(job.pack.item), value);
            else if (strcmp(key, "only") == 0)
                CopyText(job.pack.only, sizeof(job.pack.only), value);
            else if (strcmp(key, "file") == 0)
                CopyText(job.pack.filename, sizeof(job.pack.filename), value);
            else if (strcmp(key, "size") == 0)
                job.pack.size = _strtoui64(value, NULL, 10);
            else if (strcmp(key, "score") == 0)
                job.pack.score = atoi(value);
        }
        else if (job.kind == QUEUE_JOB_TITLE_UPDATE)
        {
            if (strcmp(key, "file") == 0)
                CopyText(job.update.filename, sizeof(job.update.filename), value);
            else if (strcmp(key, "size") == 0)
                job.update.size = _strtoui64(value, NULL, 10);
            else if (strcmp(key, "score") == 0)
                job.update.score = atoi(value);
            else if (strcmp(key, "ver") == 0)
                job.update.version = atoi(value);
            else if (strcmp(key, "region") == 0)
                CopyText(job.update.region, sizeof(job.update.region), value);
        }
        else if (job.kind == QUEUE_JOB_HOMEBREW)
        {
            if (strcmp(key, "title") == 0)
                CopyText(job.homebrew.title, sizeof(job.homebrew.title), value);
            else if (strcmp(key, "kind") == 0)
                CopyText(job.homebrew.kindFolder, sizeof(job.homebrew.kindFolder), value);
            else if (strcmp(key, "folder") == 0)
                CopyText(job.homebrew.folder, sizeof(job.homebrew.folder), value);
            else if (strcmp(key, "mirror") == 0)
                CopyText(job.homebrew.mirror, sizeof(job.homebrew.mirror), value);
            else if (strcmp(key, "url") == 0 && job.homebrew.urlCount < HOMEBREW_MAX_URLS)
                CopyText(job.homebrew.urls[job.homebrew.urlCount++], sizeof(job.homebrew.urls[0]), value);
        }
    }

    QueueRestored(job, authHeader, &queued); // the last block has no "job=" after it
    free(text);

    // What is in memory now is what the file should say. Adding a job writes
    // it, but putting a record back doesn't - and the records come after the
    // jobs, so the last write of the restore would otherwise have left them
    // all out.
    EnterCriticalSection(&g_lock);
    SaveQueueStateLocked();
    LeaveCriticalSection(&g_lock);

    if (queued > 0)
        dprintf("[queue] picked %d job%s back up from last time\n", queued, queued == 1 ? "" : "s");
}

// Display order: active, then waiting by sequence, then finished newest first.
static int DisplayRank(const QueueJob &job)
{
    switch (job.state)
    {
    case QUEUE_ACTIVE:  return 0;
    case QUEUE_WAITING: return 1;
    case QUEUE_PAUSED:  return 2; // still the queue's, just not moving
    default:            return 3; // finished
    }
}

static bool ListsBefore(const QueueJob &a, const QueueJob &b)
{
    int ra = DisplayRank(a), rb = DisplayRank(b);
    if (ra != rb)
        return ra < rb;
    if (ra == 3)
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

int RunnableDownloadCount()
{
    if (!g_running)
        return 0;

    int running = 0;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        // An active job being paused is still counted: the transfer hasn't
        // stopped yet, and it drops out of this the moment it has.
        if (g_jobs[i].used && (g_jobs[i].state == QUEUE_WAITING || g_jobs[i].state == QUEUE_ACTIVE))
            running++;
    }
    LeaveCriticalSection(&g_lock);

    return running;
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

        // Set first, and in every case: it is what tells FinishJob this is a
        // cancel rather than a pause, for a job that was paused already.
        job.cancelRequested = true;

        if (job.state == QUEUE_WAITING || job.state == QUEUE_PAUSED)
        {
            // Never started, or not running now, so it can finish right here.
            JobResult result;
            SetResult(result, QUEUE_OUTCOME_CANCELLED, "Cancelled", "Nothing was installed.");
            FinishJob(job, result, false);
        }
        break; // an active job's worker finishes it
    }
    LeaveCriticalSection(&g_lock);
}

void PauseQueueJob(int id)
{
    if (!g_running)
        return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        QueueJob &job = g_jobs[i];
        if (!job.used || job.id != id || job.state == QUEUE_FINISHED || job.state == QUEUE_PAUSED)
            continue;

        job.pauseRequested = true;

        if (job.state == QUEUE_WAITING)
        {
            // Nothing is in flight, so it is paused as of now.
            job.state = QUEUE_PAUSED;
            job.view.fraction = -1.0f;
            SyncView(job);
            SaveQueueStateLocked();
        }
        else
        {
            // The transfer stops at its next progress report; until then it
            // is still downloading, and the row says so rather than claiming
            // a pause that hasn't happened yet. FinishJob sets the state.
            CopyText(job.view.phase, sizeof(job.view.phase), "Pausing");
            SaveQueueStateLocked(); // saved as paused in case the app closes meanwhile
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
}

void ResumeQueueJob(int id)
{
    if (!g_running)
        return;

    bool woken = false;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_QUEUE_JOBS; ++i)
    {
        QueueJob &job = g_jobs[i];
        if (!job.used || job.id != id)
            continue;

        if (job.state == QUEUE_PAUSED)
        {
            job.pauseRequested = false;
            job.state = QUEUE_WAITING;

            // Back of the queue, not its old place: the jobs that waited
            // their turn all along shouldn't be overtaken by one that was
            // set aside.
            job.sequence = g_nextSequence++;
            SyncView(job);
            SaveQueueStateLocked();
            woken = true;
        }
        else if (job.state == QUEUE_ACTIVE && job.pauseRequested)
        {
            // Changed their mind before the transfer noticed - it never
            // stopped, so there is nothing to start again.
            job.pauseRequested = false;
            job.view.phase[0] = '\0';
            SaveQueueStateLocked();
        }
        break;
    }
    LeaveCriticalSection(&g_lock);

    if (woken)
        SetEvent(g_wake);
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

            // Now that a finished job is kept, clearing its row is a change
            // to what the file holds. It wasn't before, which is why this
            // didn't write it.
            SaveQueueStateLocked();
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
