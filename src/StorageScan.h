#ifndef STORAGE_SCAN_H
#define STORAGE_SCAN_H

// ---------------------------------------------------------------------------
// What's taking the space on each drive, off the UI thread
// ---------------------------------------------------------------------------
//
// The data under the Storage page. Walks each drive's content folders and
// adds up what every title holds there, split by kind - the game itself, an
// installed disc's data, DLC, title updates, saves, the rest - plus
// Omni360's own partial downloads. Only sizes: no package header is opened
// except the shared Cache's uppercase TU_ files, which say whose update they
// are nowhere else. Names and the parts of one title are read when a page
// asks for them, not here.
//
// A title is listed once per drive it has anything on. Whether a title has a
// game anywhere is worked out across all drives (and the folder games the
// caller hands in), so DLC on Usb0 for a game on Hdd1 isn't a leftover.
// Leftover is only a hint: a game played from its disc has DLC and updates
// and no installed game, which the caller has to rule out.

#define STORAGE_MAX_DRIVES     4    // Hdd1, Usb0-2
#define STORAGE_MAX_ROOTS      12   // content-layout folders walked
#define STORAGE_MAX_FOLDERS    256  // folder games handed in
#define STORAGE_MAX_TITLES     1024 // (title, drive) records

enum StorageKind
{
    STORAGE_GAME,         // Games on Demand, Arcade, Original Xbox, demos, indie, folder games
    STORAGE_DISC_INSTALL, // 00004000 - a disc game installed to the drive
    STORAGE_DLC,          // 00000002
    STORAGE_UPDATE,       // 000B0000, and Cache\TU_ files
    STORAGE_SAVES,        // 00000001 - listed, never offered for removal
    STORAGE_OTHER,        // avatar items, themes, gamer pictures, videos...
    STORAGE_KIND_COUNT
};

struct StorageTitle
{
    unsigned long titleId;
    int drive;                                     // index into StorageSnapshot::drives
    unsigned long long bytes[STORAGE_KIND_COUNT];
    int items[STORAGE_KIND_COUNT];                 // packages / folders of each kind
    bool hasGameAnywhere;                          // a STORAGE_GAME or disc install on any drive

    // The console's own titles (FFFExxxx - the dashboard's update lives under
    // FFFE07D1) and the XNA runtime indie games need: never a leftover, and
    // never offered for removal, whether a game is installed or not.
    bool system;
};

// What the Storage page may call leftover: DLC or updates, no game for them
// on any drive, and not the console's own. Still only a hint - see above.
inline bool StorageTitleIsLeftover(const StorageTitle &t)
{
    return !t.hasGameAnywhere && !t.system && (t.items[STORAGE_DLC] > 0 || t.items[STORAGE_UPDATE] > 0);
}

struct StorageDrive
{
    char name[8];                 // "Hdd1"
    bool present;                 // capacity could be read
    unsigned long long capacity;
    unsigned long long freeBytes;
    unsigned long long bytes[STORAGE_KIND_COUNT]; // the titles' kinds, added up
    unsigned long long partialBytes;              // Omni360Staging + Omni360-partial
    int partialItems;
};

struct StorageSnapshot
{
    int generation;               // up by one with every finished scan; 0 = none yet
    double scanMs;
    int driveCount;
    StorageDrive drives[STORAGE_MAX_DRIVES];
    int titleCount;
    StorageTitle titles[STORAGE_MAX_TITLES];
    bool titlesFull;              // more than STORAGE_MAX_TITLES - the rest were dropped
};

struct StorageFolderGame
{
    unsigned long titleId;
    char path[512]; // "Hdd1:\Games\Halo 3"
};

struct StorageScanRequest
{
    // Content-layout roots besides each drive's own Content\0000000000000000,
    // which is always walked: the games folder and the more games folders
    // from settings.txt. Duplicates are fine.
    int rootCount;
    char roots[STORAGE_MAX_ROOTS][512];

    int folderCount;
    StorageFolderGame folders[STORAGE_MAX_FOLDERS];
};

// Starts a scan on a thread of its own, copying the request. Asked for while
// one is running, that one goes again with this request once it's done, so
// the snapshot after it always reflects the drive as of the ask. False only
// if the thread couldn't start.
bool StartStorageScan(const StorageScanRequest &request);

bool StorageScanRunning();

// The latest finished scan, held until StorageSnapshotRelease. NULL (and
// nothing to release) before the first scan has finished.
const StorageSnapshot *StorageSnapshotAcquire();
void StorageSnapshotRelease();

// The kinds a removal can take: never saves, never "other".
#define STORAGE_REMOVABLE_KINDS ((1u << STORAGE_GAME) | (1u << STORAGE_DISC_INSTALL) | (1u << STORAGE_DLC) | \
                                 (1u << STORAGE_UPDATE))

// What a title's content of these kinds is worth on a drive.
inline unsigned long long StorageRemovableBytes(const StorageTitle &t, unsigned kindMask = STORAGE_REMOVABLE_KINDS)
{
    unsigned long long total = 0;
    for (int k = 0; k < STORAGE_KIND_COUNT; ++k)
    {
        if (kindMask & STORAGE_REMOVABLE_KINDS & (1u << k))
            total += t.bytes[k];
    }
    return total;
}

// Deletes a title's content of the given kinds (STORAGE_REMOVABLE_KINDS at
// most) from one drive: its content-type folders in every content folder
// the last scan walked there, its Cache\TU_ files, and its folder game. Its
// saves stay, and so its title folder while they're in it. Refuses system
// titles. On the UI thread, between scans' reads of the same request - a
// scan running meanwhile only reads. Adds what was freed to *freed (if
// given); false if anything couldn't be deleted.
bool RemoveTitleContent(unsigned long titleId, int drive, unsigned kindMask, unsigned long long *freed);

// Deletes Omni360Staging and Omni360-partial on every drive, whole - the
// installer and the DLC downloader make them again when they need them. Only
// safe with nothing queued: a paused or waiting job resumes from these. Adds
// what was freed to *freed (if given); false if anything couldn't be deleted.
bool ClearPartialDownloads(unsigned long long *freed);

// Writes a summary of the latest scan to the log: each drive, the biggest
// titles, and the leftover candidates. nameOf may return NULL.
void LogStorageSnapshot(const char *(*nameOf)(unsigned long titleId));

#endif
