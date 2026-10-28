#ifndef STORE_CATALOG_H
#define STORE_CATALOG_H

// The Store's list of games: the discs in archive.org's Redump collection,
// and some it lacks from another set, compiled in from StoreTitles.h
// (tools/make_store_titles.py). The generator has already gathered the discs
// into games, and each game's discs into versions - a region's Disc 1 and
// Disc 2, which install together - with their labels and sizes, and sorted
// the games by the name shown. This only reads its tables.

#include "StoreTitles.h" // StoreDisc, StoreRelease, StoreGame, STORE_REGION_*, STORE_MAX_*
#include "XboxTitles.h"  // the Original Xbox's, in the same shapes: XBOX_MAX_*

// The archive.org item a disc is in, e.g. "microsoft_xbox360_b_part2".
const char *StoreItemOf(const StoreDisc *disc);

// The A to Z tile that lists every game, for the ...ForLetter calls below.
#define STORE_LETTER_ALL '*'

// The games under one letter tile ('A' to 'Z', or '#'; STORE_LETTER_ALL for
// every one), in the order shown. Only retail games - demos, betas, magazine
// discs and the like aren't listed. Returns how many were written.
int StoreGamesForLetter(char letter, StoreGame *out, int maxGames);

// The same for the Original Xbox games the 360 runs, from XboxTitles.h
// (make_store_titles.py --system xbox). Their StoreGames say
// STORE_SYSTEM_XBOX, and everything below finds their versions and discs
// in that file's tables from it.
int XboxGamesForLetter(char letter, StoreGame *out, int maxGames);

// Whether a disc is an Original Xbox one - installed as one, content type
// 00005000 rather than Games on Demand's 00007000.
bool StoreDiscIsXbox(const StoreDisc *disc);

// Whether a title ID is an Original Xbox game's - one of XboxTitles.h's
// discs. For its tile's banner.
bool IsXboxTitleId(unsigned long titleId);

// One game by its title ID, any of its discs' - for the featured tiles, and
// a library game's page: the 360's, then the Original Xbox's; of several
// with it, a USA or World one first. False if no game has it.
bool StoreGameByTitleId(unsigned long titleId, StoreGame *out);

// The Store front page's rows under its A to Z - Popular, Top rated, a genre
// each - from StoreRows.h (tools/make_store_rows.py).
int StoreRowCount();
const char *StoreRowName(int row);

// A row's games, in order, as StoreGameByTitleId finds them. Returns how
// many were written.
int StoreRowGames(int row, StoreGame *out, int maxGames);

// The front page's Collections - a series each, from StoreCollections.h
// (tools/make_collections.py): its name, the game whose wallpaper is its
// card, and its games oldest first, as StoreGameByTitleId finds them.
int StoreCollectionCount();
const char *StoreCollectionName(int collection);
unsigned long StoreCollectionArt(int collection);
int StoreCollectionGames(int collection, StoreGame *out, int maxGames);

// A game page's "More like this": the rest of its series, oldest first, then
// the most rated games of its genre - Kinect ones for a Kinect game and only
// for one, family ones for a family game - with its publisher's few ahead.
// Returns how many were written.
int StoreSimilarGames(unsigned long titleId, StoreGame *out, int maxGames);

// One of a game's versions, USA and World first; NULL out of range.
const StoreRelease *StoreReleaseOf(const StoreGame *game, int version);

// A version's discs, in disc order; NULL out of range.
const StoreDisc *StoreReleaseDisc(const StoreRelease *release, int disc);

// ---------------------------------------------------------------------------
// The letter pages' filters
// ---------------------------------------------------------------------------

#include "StoreRows.h" // StoreGameInfo, STORE_GENRE_COUNT, STORE_TAG_*

// A 360 game's genre, tags and rating, by title ID. False for one the
// catalog hasn't got - and for every Original Xbox game, which it has none
// of.
bool StoreGameInfoOf(unsigned long titleId, StoreGameInfo *out);

// A main genre's shown name: 1 to STORE_GENRE_COUNT, as StoreGameInfo's.
const char *StoreGenreName(int genre);

// The languages filtered by, English first.
#define STORE_LANGUAGE_COUNT 12
const char *StoreLanguageName(int language);

// A version's languages, as bits: 1 << language. What its detail lists -
// "En, Fr, De" - or if it lists none, as Redump leaves them off a release
// with its region's one language, its regions': USA's English, Japan's
// Japanese and so on.
unsigned long StoreReleaseLanguages(const StoreRelease *release);

// Every one of a game's versions'.
unsigned long StoreGameLanguages(const StoreGame *game);

// A version's regions: every one of its discs', STORE_REGION_*.
unsigned short StoreReleaseRegions(const StoreRelease *release);

// The regions as the Store groups them, for its tiles' tags and its letter
// pages' filter. World is region free: it's tagged USA, and every filter
// lets it through.
#define STORE_REGIONS_USA  (STORE_REGION_USA | STORE_REGION_WORLD)
#define STORE_REGIONS_EUR  (STORE_REGION_EUROPE | STORE_REGION_UK | STORE_REGION_GERMANY | STORE_REGION_FRANCE | \
                            STORE_REGION_SPAIN | STORE_REGION_ITALY | STORE_REGION_AUSTRALIA | STORE_REGION_RUSSIA)
#define STORE_REGIONS_JPN  STORE_REGION_JAPAN
#define STORE_REGIONS_ASIA (STORE_REGION_ASIA | STORE_REGION_KOREA | STORE_REGION_CHINA | STORE_REGION_TAIWAN)

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

// The games under one letter tile ('A' to 'Z', or '#'; STORE_LETTER_ALL for
// every one), in the order shown. Returns how many were written.
int ArcadeGamesForLetter(ArcadeSet set, char letter, const XblaGame **out, int maxGames);

// The arcade game with this title ID, or NULL - the first, if several share
// it. Not for indie games: they all share one.
const XblaGame *XblaGameByTitleId(unsigned long titleId);

// The game from either list with this RAR name, or NULL.
const XblaGame *ArcadeGameByRar(const char *rar);

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

enum StoreHitKind
{
    STORE_HIT_XBOX360, // a disc game: game
    STORE_HIT_XBOX,    // an Original Xbox one: game
    STORE_HIT_XBLA,    // arcade
    STORE_HIT_XBLIG    // arcade, an indie game
};

struct StoreHit
{
    StoreHitKind kind;
    const StoreGame *game;  // for the disc games
    const XblaGame *arcade; // for the others
};

// Every list's games whose names have each of the query's words in them -
// ignoring case, spaces and punctuation, so "spiderman" finds Spider-Man and
// "halo3" Halo 3 - or, for eight hex digits, whose title ID it is. Best
// first: the name exactly, then names that start with it, then those with
// each word at a word's start, then the rest; within each, 360 discs,
// Original Xbox, arcade, then indie games, in their lists' order. Returns
// how many were written, at most maxHits; *outTotal, if given, how many
// matched.
int SearchStore(const char *query, StoreHit *out, int maxHits, int *outTotal);

#endif
