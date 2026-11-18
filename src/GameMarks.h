#ifndef GAME_MARKS_H
#define GAME_MARKS_H

// ---------------------------------------------------------------------------
// Favourites and renamed games
// ---------------------------------------------------------------------------
//
// What the user has said about their own games, kept in game:\GameMarks.txt
// beside the XEX:
//
//   fav    4D5307E6  Halo 3
//   name   4D5307E6  Halo 3  Halo 3 (Campaign disc)
//
// Fields are separated by tabs. A game is known by its title ID and the
// name its package gives it, not the title ID alone: one title ID can be two
// games - GTA IV and Episodes from Liberty City share 545407F2 - and marking
// one shouldn't mark the other. Neither changes when a game is reinstalled
// or the library is rescanned, so the marks outlive both.
//
// A rename changes what Omni360 shows, nothing more: the package and its
// folder keep their name, and the Store, DLC and title updates still go by
// the original.

// Reads the file. Once at startup; the rest keeps it in memory and writes
// it back on each change.
void LoadGameMarks();

bool IsFavourite(unsigned long titleId, const char *name);
// False if the file couldn't be written - the change still holds until
// Omni360 closes.
bool SetFavourite(unsigned long titleId, const char *name, bool favourite);
int FavouriteCount();

// The name the user gave the game, or NULL for none.
const char *RenamedTo(unsigned long titleId, const char *name);
// newName NULL or empty goes back to the original.
bool SetRename(unsigned long titleId, const char *name, const char *newName);

#endif
