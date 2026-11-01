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
    // it could never be trusted for ordering; the file's own order is that.
    // Nothing reads it today: Home showed "Played yesterday" under the game's
    // name for a while and no longer does. It is still written, because it
    // can't be recovered later - a game played this week can only be known
    // to have been if it was recorded at the time - and it costs two numbers
    // in a line that is already there. Both halves zero when it wasn't.
    unsigned long playedLow, playedHigh;
};

// Moves a game to the front of the list and writes it. A game already in the
// list moves rather than repeats. Costs one small read and one small write,
// on the way out of the app.
void NoteGamePlayed(unsigned long titleId, unsigned long contentType);

// The list as last written, most recent first, into out (up to max). Returns
// how many were read - 0 if there's no file yet.
int LoadRecents(RecentGame *out, int max);

#endif
