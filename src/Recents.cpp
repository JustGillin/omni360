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

// Whole days between two local dates, by a day number that ignores the time
// of day - so a game played at 11pm is "yesterday" at 1am, not "today".
static long DayNumber(const SYSTEMTIME &t)
{
    // Days since an arbitrary epoch, by the civil-from-days algorithm: good
    // for any date the console's clock can hold.
    long y = (long)t.wYear;
    const long m = (long)t.wMonth;
    const long d = (long)t.wDay;
    y -= (m <= 2) ? 1 : 0;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe;
}

static const char *const kMonths[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

const char *RecentWhen(const RecentGame &game, char *out, size_t outSize)
{
    if (out == NULL || outSize == 0)
        return NULL;
    if (game.playedLow == 0 && game.playedHigh == 0)
        return NULL;

    FILETIME played;
    played.dwLowDateTime = game.playedLow;
    played.dwHighDateTime = game.playedHigh;

    FILETIME localPlayed;
    SYSTEMTIME then;
    if (!FileTimeToLocalFileTime(&played, &localPlayed) || !FileTimeToSystemTime(&localPlayed, &then))
        return NULL;

    SYSTEMTIME now;
    GetLocalTime(&now);

    const long days = DayNumber(now) - DayNumber(then);

    // The clock was wrong then, or has been set back since: say nothing
    // rather than "played in 3 days".
    if (days < 0)
        return NULL;

    if (days == 0)
        _snprintf(out, outSize, "Played today");
    else if (days == 1)
        _snprintf(out, outSize, "Played yesterday");
    else if (days < 14)
        _snprintf(out, outSize, "Played %ld days ago", days);
    else
        _snprintf(out, outSize, "Played %d %s", (int)then.wDay, kMonths[(then.wMonth - 1) % 12]);

    out[outSize - 1] = '\0';
    return out;
}
