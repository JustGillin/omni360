#ifndef LIBRARY_CACHE_H
#define LIBRARY_CACHE_H

#include "StfsParser.h" // InstalledGame

// ---------------------------------------------------------------------------
// The library as last read, kept for the next start
// ---------------------------------------------------------------------------
//
// Reading the library takes seconds on a hard drive - every directory open is
// about 20ms - so startup shows the list kept from last time, one file read,
// and reads the drive behind it. The file holds each game as the scan found
// it, images included, and how many games each library folder had.
//
// folders names the folders it was read from, one string; a cache read with
// different folders is ignored, as the list would be another library's.

bool SaveLibraryCache(const char *folders, const InstalledGame *games, int count, const int *folderFound,
                      int folderCount);

// The games kept, into games (up to maxGames; their images malloc'd, as a
// scan's are), or -1 if there's no cache, it's for other folders, or it
// doesn't read back whole.
int LoadLibraryCache(const char *folders, InstalledGame *games, int maxGames, int *folderFound, int folderCount);

#endif
