#ifndef GAME_INSTALLER_H
#define GAME_INSTALLER_H

#include <xtl.h> // DWORD
#include "DownloadQueue.h" // QueueJobSnapshot - a game install is a row on the Queue page too

// ---------------------------------------------------------------------------
// Installing a game from archive.org as Games on Demand
// ---------------------------------------------------------------------------
//
// The Redump games are zips holding one deflate-compressed disc image. archive.org
// will only hand out the image from its start, and the GoD converter reads the
// disc's file system out of order, so a game is installed in two halves:
//
//   1. The zip is downloaded into a staging folder on the games drive, in
//      32MB pieces, two at a time - one connection runs at about 2.7 MB/s,
//      two at about 5, and more don't add anything. As the pieces land in
//      order, the image is decompressed once to index it (InflateSource.h)
//      and to check its CRC.
//   2. The converter reads the image out of the downloaded zip through that
//      index, and writes the game where the dashboard and Aurora find it.
//
// The staging folder is removed once the game is installed, and when the user
// stops a job or what came down turns out to be no good. It is kept when the
// app closes part way through, when archive.org gives up and when the drive
// fills: choosing that game again then carries on from the pieces already
// there rather than fetching 7-9GB a second time. Space needed is the zip and
// the installed game; the image itself is never written out.
//
// What is kept is tied to the file it came from by a note beside the pieces,
// so a file replaced on archive.org since can't be stitched onto an old
// download. A compressed RAR is the exception: its pieces are consumed as it
// unpacks, and the unpacking has no restart point, so those start over.
//
// One game at a time, on a worker of its own, so a game doesn't hold up DLC
// and title updates or a disc install. Jobs are reported in the download
// queue's terms, for the Queue page.

#define GAME_JOB_ID_BASE 2000000
#define MAX_GAME_JOBS 8

inline bool IsGameJobId(int id) { return id >= GAME_JOB_ID_BASE; }

struct GameRequest
{
    char item[96];      // the archive.org item, e.g. "microsoft_xbox360_b_part2"
    char zipName[256];  // the file in it, e.g. "Blitz - The League (USA).zip"
    unsigned long long zipSize;
    char name[128];     // what the dashboard shows, UTF-8
    unsigned long titleId; // for the Queue's box art; 0 if not known yet
};

// gamesPath is where games install. Also clears out staging folders no
// download can pick up again - the ones from before downloads were kept, and
// any whose note is missing.
bool StartGameInstaller(const char *gamesPath);
bool StopGameInstaller(DWORD timeoutMs);
void SetGameInstallerGamesPath(const char *gamesPath);

enum GameEnqueueResult
{
    GAME_QUEUED,
    GAME_ALREADY_QUEUED,
    GAME_QUEUE_FULL,
    GAME_INSTALLER_UNAVAILABLE
};

// authHeader is copied into the job.
GameEnqueueResult EnqueueGameInstall(const GameRequest &request, const char *authHeader);

// Queues again whatever install was still to do when the app last closed -
// see QueueState.h. Its download is still on the drive, so it carries on
// rather than starting over. Call it once, after StartGameInstaller.
void RestoreGameInstalls(const char *authHeader);

// The active job first, then waiting ones in order, then finished, newest first.
// A game job's snapshot has the zip's name as its title.
int SnapshotGameJobs(QueueJobSnapshot *out, int maxJobs);

// Whether this zip has been installed from, and the game it made is still
// there. Every install is noted in game:\Store\Installed.txt with the title
// and media ID read from its disc; a note is believed while that disc's
// package is in the games folder. Cheap - the notes are read once, at start.
bool IsGameZipInstalled(const char *zipName);

// The same, with the title and media ID of the package it made - where it is,
// for uninstalling: <games folder>\TITLEID\00007000\MEDIAID - or for an
// Original Xbox game, ...\00005000\TITLEID, its media ID being its title ID.
bool GameZipInstalledAs(const char *zipName, unsigned long *outTitleId, unsigned long *outMediaId);

// After an uninstall: the zip's note is dropped, here and in Installed.txt.
void ForgetInstalledZip(const char *zipName);
int PendingGameJobCount();
void CancelGameJob(int id);
void RemoveGameJob(int id);
bool TakeFinishedGameJob(QueueJobSnapshot *out);

#endif
