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
// The staging folder is removed when the job ends, however it ends. Space
// needed is the zip and the installed game; the 7-9GB image itself is never
// written out.
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

// gamesPath is where games install. Also clears out any staging folder an
// earlier session left.
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

// The active job first, then waiting ones in order, then finished, newest first.
int SnapshotGameJobs(QueueJobSnapshot *out, int maxJobs);
int PendingGameJobCount();
void CancelGameJob(int id);
void RemoveGameJob(int id);
bool TakeFinishedGameJob(QueueJobSnapshot *out);

#endif
