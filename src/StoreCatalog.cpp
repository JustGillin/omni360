#define STORE_TITLES_DATA
#include "StoreCatalog.h"

#include <string.h>

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

const char *StoreItemOf(const StoreDisc *disc)
{
    return (disc != NULL && disc->item < COUNT_OF(kStoreItems)) ? kStoreItems[disc->item] : "";
}

int StoreGamesForLetter(char letter, StoreGame *out, int maxGames)
{
    if (out == NULL || maxGames <= 0)
        return 0;

    // A letter's games are together, as the table is sorted.
    int count = 0;
    for (int i = 0; i < STORE_GAME_COUNT && count < maxGames; ++i)
    {
        if (kStoreGames[i].letter == letter)
            out[count++] = kStoreGames[i];
        else if (count > 0)
            break;
    }
    return count;
}

bool StoreGameByTitleId(unsigned long titleId, StoreGame *out)
{
    if (out == NULL || titleId == 0)
        return false;

    // The game it names first; else one with a disc that has it.
    for (int i = 0; i < STORE_GAME_COUNT; ++i)
    {
        if (kStoreGames[i].titleId == titleId)
        {
            *out = kStoreGames[i];
            return true;
        }
    }
    for (int i = 0; i < STORE_GAME_COUNT; ++i)
    {
        for (int v = 0; v < kStoreGames[i].versionCount; ++v)
        {
            const StoreRelease *release = StoreReleaseOf(&kStoreGames[i], v);
            for (int d = 0; release != NULL && d < release->discCount; ++d)
            {
                const StoreDisc *disc = StoreReleaseDisc(release, d);
                if (disc != NULL && disc->titleId == titleId)
                {
                    *out = kStoreGames[i];
                    return true;
                }
            }
        }
    }
    return false;
}

const StoreRelease *StoreReleaseOf(const StoreGame *game, int version)
{
    if (game == NULL || version < 0 || version >= game->versionCount)
        return NULL;
    const int at = game->firstVersion + version;
    return (at < COUNT_OF(kStoreReleases)) ? &kStoreReleases[at] : NULL;
}

const StoreDisc *StoreReleaseDisc(const StoreRelease *release, int disc)
{
    if (release == NULL || disc < 0 || disc >= release->discCount)
        return NULL;
    const int at = release->firstDisc + disc;
    if (at >= COUNT_OF(kStoreReleaseDiscs))
        return NULL;
    const int index = kStoreReleaseDiscs[at];
    return (index < COUNT_OF(kStoreDiscs)) ? &kStoreDiscs[index] : NULL;
}
