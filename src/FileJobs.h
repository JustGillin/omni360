#ifndef FILE_JOBS_H
#define FILE_JOBS_H

// ---------------------------------------------------------------------------
// Copying, moving and deleting files, in the background
// ---------------------------------------------------------------------------
//
// The file manager's work, on a thread of its own so a game's worth of
// gigabytes doesn't hold the screen: each copy, move or delete is a job on
// the Queue page beside the downloads and installs, with its progress, its
// speed and a popup when it's done.
//
// One job runs at a time, in the order they were asked for - two copies at
// once to the same drive would only each go at half the speed.
//
// A copy or move never overwrites: if the destination already has something
// of that name, the job fails and says so. A copy that's cancelled or fails
// partway removes what it had copied; a move only removes the original once
// the copy is all there (or straight away, as a rename, on the same drive).

#include "DownloadQueue.h" // QueueJobSnapshot

#define FILE_JOB_ID_BASE 500000 // apart from the other queues' ids - see IsDiscJobId
inline bool IsFileJobId(int id) { return id >= FILE_JOB_ID_BASE && id < 1000000; }

enum FileJobKind
{
    FILE_JOB_COPY,
    FILE_JOB_MOVE,
    FILE_JOB_DELETE,
    FILE_JOB_TITLE_UPDATE // a title update from XboxUnity - see EnqueueTitleUpdateDownload
};

bool StartFileJobs();
// Cancels everything and waits up to timeoutMs. A copy cut off this way is
// removed, as a cancelled one is.
void StopFileJobs(unsigned long timeoutMs);

// source is a file or folder ("Hdd1:\Games\Halo 3"); destFolder the folder
// it goes into ("Usb0:\Games") - unused for a delete. False if the queue is
// full or the worker isn't running.
bool EnqueueFileJob(FileJobKind kind, const char *source, const char *destFolder);

// A title update from XboxUnity (XboxUnity.h): downloaded from url to a
// staging file on destFolder's drive (resuming with a Range if it's cut off),
// checked against md5 (hex; empty to skip) and size, which must be exact,
// moved to destFolder\fileName - replacing one of that name - and
// turned on, the title's others set aside (TitleUpdates.h). label names it on
// the Queue.
// Returns the job's id, or 0 if the queue is full or the worker isn't running.
int EnqueueTitleUpdateDownload(const char *url, const char *destFolder, const char *fileName, const char *md5,
                               unsigned long long size, unsigned long titleId, const char *label);

// A finished job whose result was shown already - in the progress dialog
// that followed it - so it doesn't pop up again as well.
void MarkFileJobSeen(int id);

// For the Queue page, as the other queues have them.
int SnapshotFileJobs(QueueJobSnapshot *out, int maxJobs);
int PendingFileJobCount();
void CancelFileJob(int id);
void RemoveFileJob(int id);
bool TakeFinishedFileJob(QueueJobSnapshot *out);

#endif
