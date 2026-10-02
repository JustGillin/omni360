#ifndef STORE_CATALOG_H
#define STORE_CATALOG_H

// The Store's list of games: the discs in archive.org's Redump collection,
// and some it lacks from another set, compiled in from StoreTitles.h
// (tools/make_store_titles.py). The generator has already gathered the discs
// into games, and each game's discs into versions - a region's Disc 1 and
// Disc 2, which install together - with their labels and sizes, and sorted
// the games by the name shown. This only reads its tables.

#include "StoreTitles.h" // StoreDisc, StoreRelease, StoreGame, STORE_REGION_*, STORE_MAX_*

// The archive.org item a disc is in, e.g. "microsoft_xbox360_b_part2".
const char *StoreItemOf(const StoreDisc *disc);

// The games under one letter tile ('A' to 'Z', or '#'), in the order shown.
// Only retail games - demos, betas, magazine discs and the like aren't
// listed. Returns how many were written.
int StoreGamesForLetter(char letter, StoreGame *out, int maxGames);

// One game by its title ID, any of its discs' - for the featured tiles.
// False if no game has it.
bool StoreGameByTitleId(unsigned long titleId, StoreGame *out);

// One of a game's versions, USA and World first; NULL out of range.
const StoreRelease *StoreReleaseOf(const StoreGame *game, int version);

// A version's discs, in disc order; NULL out of range.
const StoreDisc *StoreReleaseDisc(const StoreRelease *release, int disc);

// The Xbox Live Arcade games, from XblaTitles.h (tools/make_xbla_titles.py):
// one RAR each in archive.org's XBOX_360_XBLA, sorted by name.
#include "XblaTitles.h" // XblaGame, XBLA_GAME_COUNT

// The archive.org item a game's RAR is in.
const char *XblaItemOf(const XblaGame *game);

// The games under one letter tile ('A' to 'Z', or '#'), in the order shown.
// Returns how many were written.
int XblaGamesForLetter(char letter, const XblaGame **out, int maxGames);

// The first game with this title ID, or NULL.
const XblaGame *XblaGameByTitleId(unsigned long titleId);

#endif
