#ifndef TITLE_UPDATES_H
#define TITLE_UPDATES_H

// ---------------------------------------------------------------------------
// A game's installed title updates, and which one is on
// ---------------------------------------------------------------------------
//
// The console applies a title update from either of two places on a drive:
// Content\0000000000000000\<title ID>\000B0000\ (lowercase "tu..." files) or
// Cache\ (uppercase "TU_..." files, whose title ID is only in their header).
// More than one for the same game, and which wins isn't something to rely on
// - so, as Aurora does, the ones not in use are set aside where the console
// doesn't look:
//
//   <drive>:\Omni360\Title updates\<title ID>\Content\tu00000008_00000000
//   <drive>:\Omni360\Title updates\<title ID>\Cache\TU_16L61V6_...
//
// on the same drive, so turning one on or off is a rename, and the folder
// it's under says where it goes back to.
//
// The header (StfsParser.h) gives each one's name - "Halo 3 ... Title
// Update #3" - and at 0x358 its version, which always says which is newer
// but isn't the update's number (Halo 3's #3 is version 42, Crysis 3's #1
// is 2; checked on hardware 2026-11-19). The number comes from XboxUnity,
// whose "hash" is the header's content ID at 0x32C: remembered for the
// ones installed whenever their list is fetched.

#define MAX_TITLE_UPDATES 16

struct InstalledTitleUpdate
{
    char path[260];
    char name[128];         // the header's display name
    unsigned long version;  // the header's version at 0x358
    char contentId[41];     // the header's content ID at 0x32C, in hex
    unsigned long number;   // "Title Update 3"'s 3, if remembered; 0 if not
    unsigned long long size;
    bool on;                // where the console looks, rather than set aside
    char drive[8];          // "Hdd1"
};

// The title's updates on every drive, on ones first, newest first within.
int ListTitleUpdates(unsigned long titleId, InstalledTitleUpdate *out, int max);

// Turns update on: every other one of the title's that's on is set aside
// first. False (with what's on left as it was, as far as it can be) if a
// file couldn't be moved.
bool TurnOnTitleUpdate(unsigned long titleId, const InstalledTitleUpdate &update);

// Sets it aside. False if it couldn't be moved.
bool TurnOffTitleUpdate(unsigned long titleId, const InstalledTitleUpdate &update);

bool DeleteTitleUpdate(const InstalledTitleUpdate &update);

// The media ID of an installed game - which disc release it is, what
// XboxUnity matches updates by. From a package's header (Games on Demand,
// Arcade) at 0x354, or from a folder game's default.xex. False if neither
// could be read.
bool ReadGameMediaId(const char *packagePath, bool folder, unsigned long *mediaId);

// Where a new update for titleId goes, by its file name: Cache for a "TU_"
// one (always the hard drive, as the console reads those from nowhere
// else), the title's 000B0000 under contentBase otherwise.
void TitleUpdateFolderFor(unsigned long titleId, const char *fileName, const char *contentBase, char *out,
                          size_t outSize);

// Remembers the number of the update with this content ID (XboxUnity's
// "hash"), for ListTitleUpdates to give it. Kept on Hdd1.
void RememberTitleUpdateNumber(const char *contentId, unsigned long number);

// Turns on the update now at path - after a download lands it - setting the
// title's others aside.
bool TurnOnTitleUpdateAt(unsigned long titleId, const char *path);

#endif
