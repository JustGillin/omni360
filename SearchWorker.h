#ifndef SEARCH_WORKER_H
#define SEARCH_WORKER_H

#include <xtl.h> // DWORD
#include "ArchiveOrgDLC.h"

// ---------------------------------------------------------------------------
// Searches for a game's DLC and title updates, off the UI thread
// ---------------------------------------------------------------------------
//
// A search is a request to archive.org plus a match against a whole
// collection's listing, which used to hold the screen for the length of it.
// Here the picker opens at once, says it's searching, and fills in when the
// result arrives - and B backs out of it at any time.
//
// Its own thread rather than the download worker's: a search waiting behind
// a gigabyte of DLC would be no better than the frozen screen it replaces.
//
// One search at a time. A search can't be interrupted once its request is
// out, so backing out of one simply means its result is never collected. A
// new search asked for while one is running waits for it, then runs; if
// several are asked for in that time, only the newest runs.

enum SearchKind
{
    SEARCH_DLC,
    SEARCH_TITLE_UPDATES
};

struct SearchResult
{
    int requestId;
    SearchKind kind;
    int count; // matches found; -1 if archive.org couldn't be reached
    DlcRarMatch packs[MAX_DLC_RAR_MATCHES];             // SEARCH_DLC
    TitleUpdateMatch updates[MAX_TITLE_UPDATE_MATCHES]; // SEARCH_TITLE_UPDATES
};

// False if the thread couldn't be created - BeginSearch then returns 0.
bool StartSearchWorker();

// Waits up to timeoutMs for a search in progress to finish. See
// StopDownloadQueue for why it's bounded.
bool StopSearchWorker(DWORD timeoutMs);

// Asks for a search and returns its request id, or 0 if the worker isn't
// running. gameName is copied.
int BeginSearch(SearchKind kind, const char *gameName);

// True once the search with this id has finished, with its result in *out.
// A result is handed out once; asking again for the same id returns false.
bool TakeSearchResult(int requestId, SearchResult *out);

#endif
