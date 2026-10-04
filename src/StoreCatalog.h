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

// The Xbox Live Arcade and Indie Games lists, from XblaTitles.h and
// XbligTitles.h (tools/make_xbla_titles.py): one RAR a game, in archive.org's
// XBOX_360_XBLA and XBOX_360_XBLIG_1 to _5, sorted by name.
#include "XblaTitles.h"  // XblaGame, XBLA_GAME_COUNT - both lists' games are XblaGames
#include "XbligTitles.h" // XBLIG_GAME_COUNT

enum ArcadeSet
{
    ARCADE_XBLA,
    ARCADE_XBLIG
};

// The most games under one letter of either list.
#define ARCADE_MAX_LETTER_GAMES (XBLIG_MAX_LETTER_GAMES > XBLA_MAX_LETTER_GAMES ? XBLIG_MAX_LETTER_GAMES : XBLA_MAX_LETTER_GAMES)

// Which list a game is from, and the archive.org item its RAR is in.
ArcadeSet ArcadeSetOf(const XblaGame *game);
const char *ArcadeItemOf(const XblaGame *game);

// The games under one letter tile ('A' to 'Z', or '#'), in the order shown.
// Returns how many were written.
int ArcadeGamesForLetter(ArcadeSet set, char letter, const XblaGame **out, int maxGames);

// The arcade game with this title ID, or NULL - the first, if several share
// it. Not for indie games: they all share one.
const XblaGame *XblaGameByTitleId(unsigned long titleId);

// The game from either list with this RAR name, or NULL.
const XblaGame *ArcadeGameByRar(const char *rar);

#endif
