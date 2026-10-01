#ifndef DOWNLOAD_QUEUE_H
#define DOWNLOAD_QUEUE_H

#include <xtl.h> // DWORD
#include "ArchiveOrgDLC.h"

// ---------------------------------------------------------------------------
// The download queue: DLC packs and title updates, installed one at a time on
// a worker thread while the UI carries on
// ---------------------------------------------------------------------------
//
// Choosing a pack or an update in a picker adds a job here and returns at
// once. A single worker thread takes the jobs in the order they were added -
// one at a time, since two at once would only split the same connection - and
// the UI reads their progress through SnapshotDownloadQueue every frame.
//
// The worker never draws, never reads the controller and never shows a
// message: everything it has to say goes into its job, for the Queue page to
// show and for main.cpp to raise as a popup. That keeps D3D and XInput on the
// UI thread, where they have to stay.
//
// It is also the only thread that calls ListDlcMembers, DownloadDlcMember and
// DownloadTitleUpdate - see the threading note on ArchiveOrgDiskFull in
// ArchiveOrgDLC.h, which depends on that.

#define MAX_QUEUE_JOBS 32

enum QueueJobKind
{
    QUEUE_JOB_DLC_PACK,
    QUEUE_JOB_TITLE_UPDATE,
    QUEUE_JOB_DISC_INSTALL // not this queue's: DiscWorker's, in the same terms so the Queue page lists both
};

enum QueueJobState
{
    QUEUE_WAITING,
    QUEUE_ACTIVE,
    QUEUE_FINISHED // see outcome for how
};

enum QueueOutcome
{
    QUEUE_OUTCOME_NONE,              // not finished yet
    QUEUE_OUTCOME_INSTALLED,
    QUEUE_OUTCOME_ALREADY_INSTALLED, // every file was already on the console
    QUEUE_OUTCOME_PARTIAL,           // some files of a pack failed
    QUEUE_OUTCOME_FAILED,
    QUEUE_OUTCOME_KEYS_REJECTED,     // archive.org refused the keys - fixed in Settings
    QUEUE_OUTCOME_NO_SPACE,
    QUEUE_OUTCOME_CANCELLED
};

// One job, copied out under the queue's lock - safe to read at leisure.
struct QueueJobSnapshot
{
    int id;
    QueueJobKind kind;
    QueueJobState state;
    QueueOutcome outcome;

    unsigned long titleId;
    char gameName[128];
    char title[256]; // the pack's or update's filename

    // While active: what it's doing ("Reading the file list", "File 3 of
    // 12", "Downloading"), and the live numbers for the file in hand.
    char phase[64];
    unsigned long long bytesDone;
    unsigned long long bytesTotal;    // 0 when unknown
    unsigned long long bytesPerSec;
    unsigned long long secondsLeft;
    float fraction;                   // the whole job, 0..1; negative when unknown

    // Once finished: a short verdict ("Installed", "Not enough space") and
    // the line under it.
    char resultText[64];
    char resultDetail[160];

    // Whether finishing deserves a popup. False for a cancel - the user did
    // that themselves - and for jobs failed alongside one whose keys were
    // refused, which already raised one.
    bool notify;
};

// Starts the worker. contentBasePath is where DLC and title updates install.
// Returns false if the thread couldn't be created - every Enqueue call then
// returns ENQUEUE_UNAVAILABLE.
bool StartDownloadQueue(const char *contentBasePath);

// Cancels every job and waits up to timeoutMs for the worker to stop. A
// transfer stops at its next progress report, but a request already waiting
// on archive.org can't be interrupted, so this can take a few seconds.
// Returns false if the worker was still busy when the time ran out - the app
// can exit anyway; the transfer's staging file is simply left for the next
// download to overwrite.
bool StopDownloadQueue(DWORD timeoutMs);

enum EnqueueResult
{
    ENQUEUE_ADDED,
    ENQUEUE_ALREADY_QUEUED, // the same pack or update is waiting or downloading
    ENQUEUE_FULL,
    ENQUEUE_UNAVAILABLE     // the worker never started
};

// authHeader is copied into the job, so a job keeps the keys it was queued
// with even if they're changed in Settings while it waits.
EnqueueResult EnqueueDlcPack(const DlcRarMatch &pack, const char *gameName, unsigned long titleId,
                             const char *authHeader);
EnqueueResult EnqueueTitleUpdate(const TitleUpdateMatch &update, const char *gameName, unsigned long titleId,
                                 const char *authHeader);

// Copies the jobs out in the order the Queue page lists them: the active one,
// then the waiting ones in the order they'll run, then finished ones, most
// recent first. Returns how many were written.
int SnapshotDownloadQueue(QueueJobSnapshot *out, int maxJobs);

// Waiting and active jobs - the sidebar's count.
int PendingDownloadCount();

// A waiting job is cancelled on the spot; the active one stops at its next
// progress report. Files that already finished stay installed.
void CancelQueueJob(int id);

// Drops a finished job from the list. Does nothing to one that hasn't
// finished.
void RemoveQueueJob(int id);

// Hands back each job that has finished since the last call, oldest first,
// one per call; false when there are none. main.cpp drains this every frame
// to refresh what may have changed on disk and to raise the popup.
bool TakeFinishedQueueJob(QueueJobSnapshot *out);

#endif
