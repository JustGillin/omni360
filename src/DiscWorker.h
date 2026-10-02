#ifndef DISC_WORKER_H
#define DISC_WORKER_H

#include <xtl.h> // DWORD
#include "DownloadQueue.h" // QueueJobSnapshot - a disc install is a row on the Queue page too

// ---------------------------------------------------------------------------
// The disc drive: what's in it, and installing it as Games on Demand
// ---------------------------------------------------------------------------
//
// One worker thread owns the drive. While nothing is installing it watches the
// tray and, when a disc goes in, reads which game it is - so the library can
// show the disc as its first tile. Asked to install, it copies the disc with
// GodConvert and reports progress the way the download queue does, as a job
// the Queue page lists. Downloads and a disc install run side by side: they
// use different hardware, and a 7GB copy shouldn't wait behind a 20MB update.
//
// Noticing a disc: the system controller is asked for the tray's state
// (HalSendSMCMessage, command 0x0A) every half second - cheap, and it never
// spins the drive. Which value means what isn't documented, so any change in
// it is taken to mean the disc may have changed: the disc is forgotten, and a
// few seconds later, once the drive has had time to spin up, it is read
// again. The raw values go to the log.
//
// The worker never draws or reads the controller; main.cpp asks for the name
// with the keyboard before queueing the install, and raises the popups.

enum DiscState
{
    DISC_NONE,        // nothing in the drive, or the tray is open
    DISC_READING,     // something went in; working out what
    DISC_READY,       // an Xbox 360 game disc - see DiscInfo
    DISC_UNREADABLE   // a disc, but not one that can be installed - see reason
};

struct DiscInfo
{
    DiscState state;
    int changeCount;          // goes up each time the disc changes, to notice it
    unsigned long titleId;
    unsigned long mediaId;
    unsigned int discNumber;
    unsigned int discCount;
    unsigned long long outputSize; // what the installed package takes
    char reason[96];          // DISC_UNREADABLE: why

    // No name: main.cpp looks it up in the bundled title list (TitleNames.h),
    // which is a static table and better included in one place.
    bool installing;          // a job for this disc is waiting or running
};

// gamesPath is where games install, and where the library is read from.
bool StartDiscWorker(const char *gamesPath);

// Stops an install in progress - GodConvert removes what it copied - and
// waits up to timeoutMs for the worker to finish.
bool StopDiscWorker(DWORD timeoutMs);

// After the games folder is changed in Settings.
void SetDiscWorkerGamesPath(const char *gamesPath);

void GetDiscInfo(DiscInfo *out);

// Whether this disc is installed in the games folder already - its header is
// there. The UI thread asks; it's one file open.
bool IsDiscInstalled(const char *gamesPath, unsigned long titleId, unsigned long mediaId);

enum DiscInstallResult
{
    DISC_INSTALL_QUEUED,
    DISC_INSTALL_BUSY,        // one is installing already
    DISC_INSTALL_NO_DISC,     // the disc changed, or there isn't one
    DISC_INSTALL_UNAVAILABLE  // the worker never started
};

// Installs the disc now in the drive, if it's still the one with this title
// and media ID, under name (UTF-8, what the dashboard shows).
DiscInstallResult QueueDiscInstall(unsigned long titleId, unsigned long mediaId, const char *name);

// Disc install jobs, in the download queue's terms, so the Queue page can
// list them with the downloads. Their ids start at DISC_JOB_ID_BASE, which no
// download id reaches.
#define DISC_JOB_ID_BASE 1000000
#define MAX_DISC_JOBS 8

// Game installs (GameInstaller.h) number theirs from 2000000, above these.
inline bool IsDiscJobId(int id) { return id >= DISC_JOB_ID_BASE && id < 2000000; }

// The active one first, then finished ones, most recent first.
int SnapshotDiscJobs(QueueJobSnapshot *out, int maxJobs);
int PendingDiscJobCount();
void CancelDiscJob(int id);
void RemoveDiscJob(int id);
bool TakeFinishedDiscJob(QueueJobSnapshot *out);

// An install that ended too abruptly to clean up after itself - the console
// switched off, or the Guide button back to the dashboard - leaves this file
// behind. main.cpp reads it at startup to remove the partial package.
//
// Lines: games folder, title ID, media ID, the finished package's size, and
// the name.
#define INSTALL_MARKER_FILE "game:\\InstallInProgress.txt"
void WriteInstallMarker(const char *gamesPath, const struct GodImageInfo &info, const char *name);
void ClearInstallMarker();

#endif
