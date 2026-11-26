#ifndef SCAN_PROGRESS_H
#define SCAN_PROGRESS_H

// ---------------------------------------------------------------------------
// How far a library read has got
// ---------------------------------------------------------------------------
//
// A read of the library's folders runs on a thread of its own; this is what
// it tells the UI as it goes - which folder, how many games so far, how many
// packages or executables couldn't be read - for the label under the clock,
// the folder's row in Settings, and the summary when it's done (issue #21).
//
// Only the thread that called ScanProgressBegin counts: the same scanners
// run elsewhere too (the Storage page, removing a folder game), and those
// mustn't add to it.

// A read starts, of folderCount library folders - or, apps, of the Apps,
// Homebrew and Emulators pages' folders, whose apps aren't counted.
void ScanProgressBegin(int folderCount, bool apps = false);

// The next library folder: index from 0, and the games kept so far.
void ScanProgressFolder(int index, const char *path, int gamesSoFar);

// Called by the scanners: a game found, or something that couldn't be read.
void ScanProgressGame();
void ScanProgressSkipped();

// The read is done.
void ScanProgressEnd();

struct ScanProgressSnapshot
{
    bool running;
    bool apps;       // the Apps, Homebrew and Emulators folders, not the library's
    int folder;      // from 0
    int folderCount;
    int games;       // found so far - before duplicates across folders are dropped
    int folderGames; // of them, in the folder being read
    int skipped;     // couldn't be read, this read
    char path[256];  // the folder being read
};

// Safe from any thread.
void ScanProgressGet(ScanProgressSnapshot *out);

#endif
