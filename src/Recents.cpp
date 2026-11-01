/*
FILE : Recents.cpp
PROJECT : Omni360
DESCRIPTION : The games played most recently, for the Home page. See Recents.h.
*/

#include "Recents.h"
#include "OutputConsole.h" // dprintf

#include <xtl.h>
#include <stdio.h>
#include <string.h>

#define RECENTS_FILE "game:\\Recents.txt"

// One line a game: title ID, content type and the FILETIME halves, all hex.
// Plain text so it can be read - and deleted - over FTP without a tool.
#define RECENTS_LINE_FORMAT "%08lX %08lX %08lX %08lX\n"

int LoadRecents(RecentGame *out, int max)
{
    if (out == NULL || max <= 0)
        return 0;

    FILE *f = fopen(RECENTS_FILE, "r");
    if (f == NULL)
        return 0;

    int count = 0;
    char line[128];
    while (count < max && fgets(line, sizeof(line), f) != NULL)
    {
        RecentGame game;
        memset(&game, 0, sizeof(game));
        if (sscanf(line, "%lX %lX %lX %lX", &game.titleId, &game.contentType, &game.playedLow,
                   &game.playedHigh) < 2)
            continue; // a blank or half-written line; skip it rather than stop
        if (game.titleId == 0)
            continue;

        // A title ID twice would show the same game twice on the page.
        bool seen = false;
        for (int i = 0; i < count && !seen; ++i)
            seen = (out[i].titleId == game.titleId);
        if (seen)
            continue;

        out[count++] = game;
    }

    fclose(f);
    return count;
}

void NoteGamePlayed(unsigned long titleId, unsigned long contentType)
{
    if (titleId == 0)
        return;

    RecentGame games[MAX_RECENTS];
    const int had = LoadRecents(games, MAX_RECENTS);

    RecentGame now;
    memset(&now, 0, sizeof(now));
    now.titleId = titleId;
    now.contentType = contentType;

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    now.playedLow = ft.dwLowDateTime;
    now.playedHigh = ft.dwHighDateTime;

    FILE *f = fopen(RECENTS_FILE, "w");
    if (f == NULL)
    {
        // Not worth stopping the game starting over.
        dprintf("[recents] %s wouldn't open to write\n", RECENTS_FILE);
        return;
    }

    fprintf(f, RECENTS_LINE_FORMAT, now.titleId, now.contentType, now.playedLow, now.playedHigh);

    // The rest behind it, this game's old entry dropped, oldest trimmed off
    // the end by the count.
    int written = 1;
    for (int i = 0; i < had && written < MAX_RECENTS; ++i)
    {
        if (games[i].titleId == titleId)
            continue;
        fprintf(f, RECENTS_LINE_FORMAT, games[i].titleId, games[i].contentType, games[i].playedLow,
                games[i].playedHigh);
        written++;
    }

    fclose(f);
}
