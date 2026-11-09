#ifndef MISSING_GAMES_H
#define MISSING_GAMES_H

// ---------------------------------------------------------------------------
// Marking the missing-games list by hand
// ---------------------------------------------------------------------------
//
// "Show games we don't have" puts CatalogTitles.h's rows into the letter
// pages: the Xbox Live catalog's retail games minus everything the Store,
// the Original Xbox list and XBLA can download. The catalog is the whole
// marketplace rather than a register of retail discs, so a few of those rows
// are not games anybody could own - the same game again under a shortened
// name, a title update disc, an Xbox test product, a game that was listed
// and then cancelled, something with no real name at all.
//
// None of that has a tell in the data, which is why the filter ships off
// behind a warning. Someone has to look at the list and say. This is the
// screen that lets them: a mark a row, kept in a small text file beside the
// app, which is then read by tools/make_catalog_titles.py so the next
// generated CatalogTitles.h leaves the marked ones out.
//
// The file is the point of the whole thing - it is meant to be taken off the
// console and read, so it is written grouped by mark, with each game's name
// in a comment beside its title ID. Nothing in the app reads a name back:
// the title ID is the record, and the name is there for whoever opens it.
//
// Marks are saved the moment one is given. A console is as often switched
// off at the wall as closed politely, and an evening of marking lost to that
// would not be done again.

enum MissingMark
{
    MISSING_MARK_NONE = 0,   // not looked at yet
    MISSING_MARK_KEEP,       // a real game: it belongs in the list
    MISSING_MARK_DUPLICATE,  // the same game again, or a regional variant
    MISSING_MARK_BAD_NAME,   // no name, or a name that isn't a game's
    MISSING_MARK_TEST,       // an Xbox test product
    MISSING_MARK_DLC_DISC,   // a DLC or title update disc, not a game
    MISSING_MARK_UNRELEASED, // announced, listed, then never released
    MISSING_MARK_NOT_360,    // a PC, Original Xbox or Xbox One game
    MISSING_MARK_APP,        // an app or a store page, not a game
    MISSING_MARK_OTHER,      // something else that doesn't belong
    MISSING_MARK_COUNT
};

// How a mark reads: its full name for the picker and the file's headings,
// something short enough to sit on a row beside a game's details, the one
// word the file records, and the line under it in the picker.
const char *MissingMarkName(int mark);
const char *MissingMarkShort(int mark);
const char *MissingMarkKey(int mark);
const char *MissingMarkHelp(int mark);

// A game's mark, MISSING_MARK_NONE for one not marked. Reads the file the
// first time it is asked.
int MissingMarkOf(unsigned long titleId);

// Marks a game, or unmarks it with MISSING_MARK_NONE, and writes the file.
// False only if the file could not be written - the mark is kept in memory
// either way, so a session carries on and says so once.
bool SetMissingMark(unsigned long titleId, int mark);

// How many of the catalog's rows carry a mark.
int MissingMarkedCount();

// Where the file is written, for the screen to name.
const char *MissingMarksPath();

#endif
