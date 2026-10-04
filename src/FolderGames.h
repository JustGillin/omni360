#ifndef FOLDER_GAMES_H
#define FOLDER_GAMES_H

#include "StfsParser.h" // InstalledGame

// ---------------------------------------------------------------------------
// Games kept as extracted disc folders
// ---------------------------------------------------------------------------
//
// The way Aurora, FreeStyle Dash and XeXMenu users often keep games: a folder
// holding a disc's files, default.xex at its top - or an Original Xbox
// game's default.xbe - rather than a Games on Demand package. Found by
// looking for those files a few folders down from a library folder, e.g.
//
//   Hdd1:\Games\Halo 3\default.xex
//   Hdd1:\Games\Shooters\Gears of War\default.xex
//   Usb0:\Games\Mass Effect 2\Disc1\default.xex, ...\Disc2\default.xex
//
// A game's title ID is read from its executable's header, which is plain on
// a retail game, so no keys are needed. A game in disc folders - Disc1,
// "Disc 2", DVD1 - is one game, its folder the one they're in. Its DLC and
// title updates are in the Content folder like any game's.

// Walks root and the folders under it, FOLDER_GAME_DEPTH deep, for games,
// into outGames (caller-allocated, up to maxGames). Each is marked folder,
// its packagePath the game's folder and its name the folder's - or an
// Original Xbox game's own, from default.xbe. A game's own folders aren't
// looked in, nor a Content folder's title folders.
#define FOLDER_GAME_DEPTH 3
int EnumerateFolderGames(const char *root, InstalledGame *outGames, int maxGames,
                         void printFunction(const char *_format, ...));

// Deletes a folder game's folder and everything in it - once it's checked the
// folder still holds that game. The number of files removed; 0 if it wasn't
// removed.
int RemoveFolderGame(const InstalledGame &game);

#endif
