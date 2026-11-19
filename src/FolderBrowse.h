#ifndef FOLDER_BROWSE_H
#define FOLDER_BROWSE_H

// ---------------------------------------------------------------------------
// What's on the console's drives, a folder at a time
// ---------------------------------------------------------------------------
//
// The listing half of browsing - which drives are there, what a folder
// holds - with nothing about how it's shown. Settings' folder picker uses
// it for Add Folder; a file manager would list the same way, with files.
//
// Paths are the app's usual form: a drive alias and backslashes, no
// trailing separator - "Hdd1:", "Hdd1:\Games\Halo 3". "" is the top level:
// the list of drives.

#define FOLDER_ENTRY_NAME_SIZE 64 // FATX names stop at 42 characters

struct FolderEntry
{
    char name[FOLDER_ENTRY_NAME_SIZE]; // a drive's alias ("Usb0:") or a folder's or file's name
    bool folder;                       // false for a file
    unsigned long long size;           // a file's bytes; 0 for folders and drives
};

// The drives worth browsing that are there right now: the hard drive and
// each USB port with a FAT32 drive in it - and, with all, the Original Xbox
// emulator's partition (HddX:) and the disc in the drive (Dvd:), which can
// be read but not written. The folder picker leaves those two out: neither
// is somewhere to keep a library. Returns how many.
int ListDrives(FolderEntry *out, int max, bool all = false);

// Dvd: - the one drive nothing can be written to.
bool IsDiscPath(const char *path);

// What's in path, folders first and then (with files) files, each A to Z
// ignoring case. Returns how many, or -1 if the folder couldn't be read.
// Past max, the rest are left off.
int ListFolder(const char *path, bool files, FolderEntry *out, int max);

// path with name added ("Hdd1:" + "Games" = "Hdd1:\Games"), or path's
// parent ("Hdd1:\Games" -> "Hdd1:", "Hdd1:" -> ""). False if out is too small.
bool FolderChild(const char *path, const char *name, char *out, size_t outSize);
void FolderParent(const char *path, char *out, size_t outSize);

// "Hard drive", "USB drive" or "Disc", for a drive's alias - the words a list puts
// under it. NULL for one it doesn't know.
const char *DriveKind(const char *drive);

#endif
