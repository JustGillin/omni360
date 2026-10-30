#ifndef RECENTS_H
#define RECENTS_H

#include <stddef.h> // size_t

// ---------------------------------------------------------------------------
// The games played most recently, kept for the Home page
// ---------------------------------------------------------------------------
//
// The console tells no one when a game ends - starting one ends Omni360 -
// so the list is written just before the loader is handed the game, and read
// back at the next start. game:\Recents.txt holds one line per game, most
// recent first; the file's order is the order, so nothing has to be sorted.
//
// Only the title ID and content type are kept. The name, the cover and
// whether the game is still installed all come from the library the Home
// page is built against, so a game uninstalled since simply drops out.

#define MAX_RECENTS 12

struct RecentGame
{
    unsigned long titleId;
    unsigned long contentType;

    // When it was started, by the console's clock - which may be unset, so
    // this is for wording only, never for ordering. Both halves zero when it
    // wasn't recorded.
    unsigned long playedLow, playedHigh;
};

// Moves a game to the front of the list and writes it. A game already in the
// list moves rather than repeats. Costs one small read and one small write,
// on the way out of the app.
void NoteGamePlayed(unsigned long titleId, unsigned long contentType);

// The list as last written, most recent first, into out (up to max). Returns
// how many were read - 0 if there's no file yet.
int LoadRecents(RecentGame *out, int max);

// "Played today", "Played yesterday", "Played 8 days ago", or the date for
// anything older, into out. NULL when the time wasn't recorded or the clock
// has since gone backwards, so the caller can leave the line out.
const char *RecentWhen(const RecentGame &game, char *out, size_t outSize);

#endif
