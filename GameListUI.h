#ifndef GAME_LIST_UI_H
#define GAME_LIST_UI_H

#include "StfsParser.h"

// Sets up the icon/quad-rendering shader + vertex declaration + a 1x1 white
// texture (for solid-color rects), and this UI's own Font instance (Console
// keeps its own privately, so we can't reuse it - see the fork's plan notes
// on why a from-scratch render loop was needed here). Call once after
// MakeConsole(...) has run, before any Show*/Render* call below.
bool InitGameListUI();
void ShutdownGameListUI();

struct GameListUIResult
{
    bool selected;
    int selectedIndex; // valid only when selected is true

    // Which of the two actions was taken on that row: A chooses DLC, Y chooses
    // title updates. Having both on the game list rather than behind a submenu
    // keeps the common path one press, and lets someone who already has a
    // game's DLC fetch just its update without walking through the DLC screens.
    bool titleUpdates;
};

// Draws an icon + name list of games, with D-pad up/down to move, A to
// select, B to cancel - blocks until one of those happens.
//
// initialSelection is the row to start on, clamped to the list. This screen is
// the app's root, so the user returns here after every download; starting them
// back at the top of a 27-game library each time would undo the navigation.
//
// Cover art is loaded on the first call and kept until ShutdownGameListUI, so
// returning here is instant rather than re-decoding the whole library.
//
// hasDlcInstalled and hasUpdateInstalled, when non-NULL, are caller-owned
// arrays of gameCount flags marking which titles already have DLC, and which
// already have a title update, on the console. Those rows get an "INSTALLED"
// marker naming whichever is present. The caller works this out rather than
// this screen doing it, because it is a question about content paths on disk
// and this file has no business knowing where content lives.
//
// Either pointer may be NULL, which simply suppresses that marker.
GameListUIResult ShowGameListUI(const InstalledGame *games, int gameCount, int initialSelection,
                                const bool *hasDlcInstalled, const bool *hasUpdateInstalled);

// Draws one progress-bar frame. Intended to be called repeatedly from inside
// a download loop - see main.cpp's DlcProgressCallback, which drives it from
// downloadFile.cpp's own read loop via DownloadProgressFn.
//
//   title      - what's being downloaded (the game or pack name)
//   statusLine - which piece, e.g. "file 3 of 12"
//   detailLine - live numbers, e.g. "12.4 MB / 48.0 MB - 1.2 MB/s - 3:21 left".
//                May be NULL/empty when there's nothing to say yet.
//   fraction0to1 - bar fill; clamped internally. Pass a negative value for an
//                indeterminate download (unknown total size), which draws an
//                empty trough rather than a misleading 0%.
void RenderProgressFrame(const char *title, const char *statusLine,
                         const char *detailLine, float fraction0to1);

// Draws one non-interactive status frame and returns immediately - for the
// blocking phases between screens (scanning the library, talking to
// archive.org, reading a pack's file list), which otherwise leave the last
// screen frozen or fall back to raw console text.
//
// detailLine may be NULL. Call it before starting the slow work, not after.
void RenderStatusFrame(const char *heading, const char *message, const char *detailLine);

// Blocks on a simple list picker sharing the game list's look, but with no
// artwork - D-pad to move, A to choose, B to cancel. Used for choosing between
// DLC packs when a game matches more than one.
//
// labels and sublabels are caller-owned arrays of `count` strings; sublabels
// may be NULL entirely, or hold NULL for an individual row.
//
// Returns the chosen index, or -1 if cancelled.
// initialSelection works the same way as ShowGameListUI's - the user comes
// back here after each download, and should land on the pack they just took
// rather than at the top.
int ShowChoiceUI(const char *heading, const char **labels, const char **sublabels,
                 int count, int initialSelection);

// Draws a message and waits for B. For terminal states (nothing found, an
// error, finished) that previously just printed a line and dropped the user
// back to a console screen.
void ShowMessageUI(const char *heading, const char *message, const char *detailLine);

#endif
