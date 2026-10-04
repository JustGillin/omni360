#define STORE_TITLES_DATA
#define XBOX_TITLES_DATA
#define XBLA_TITLES_DATA
#define XBLIG_TITLES_DATA
#include "StoreCatalog.h"

#include <stdlib.h>
#include <string.h>

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

bool StoreDiscIsXbox(const StoreDisc *disc)
{
    return disc >= kXboxDiscs && disc < kXboxDiscs + COUNT_OF(kXboxDiscs);
}

bool IsXboxTitleId(unsigned long titleId)
{
    for (int i = 0; titleId != 0 && i < COUNT_OF(kXboxDiscs); ++i)
    {
        if (kXboxDiscs[i].titleId == titleId)
            return true;
    }
    return false;
}

static bool IsXboxRelease(const StoreRelease *release)
{
    return release >= kXboxReleases && release < kXboxReleases + COUNT_OF(kXboxReleases);
}

const char *StoreItemOf(const StoreDisc *disc)
{
    if (disc == NULL)
        return "";
    if (StoreDiscIsXbox(disc))
        return disc->item < COUNT_OF(kXboxItems) ? kXboxItems[disc->item] : "";
    return disc->item < COUNT_OF(kStoreItems) ? kStoreItems[disc->item] : "";
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

int XboxGamesForLetter(char letter, StoreGame *out, int maxGames)
{
    if (out == NULL || maxGames <= 0)
        return 0;

    int count = 0;
    for (int i = 0; i < XBOX_GAME_COUNT && count < maxGames; ++i)
    {
        if (kXboxGames[i].letter == letter)
            out[count++] = kXboxGames[i];
        else if (count > 0)
            break;
    }
    return count;
}

// As StoreGameByTitleId, in one table.
static bool FindByTitleId(const StoreGame *games, int count, unsigned long titleId, StoreGame *out)
{
    // The game it names first; else one with a disc that has it.
    for (int i = 0; i < count; ++i)
    {
        if (games[i].titleId == titleId)
        {
            *out = games[i];
            return true;
        }
    }
    for (int i = 0; i < count; ++i)
    {
        for (int v = 0; v < games[i].versionCount; ++v)
        {
            const StoreRelease *release = StoreReleaseOf(&games[i], v);
            for (int d = 0; release != NULL && d < release->discCount; ++d)
            {
                const StoreDisc *disc = StoreReleaseDisc(release, d);
                if (disc != NULL && disc->titleId == titleId)
                {
                    *out = games[i];
                    return true;
                }
            }
        }
    }
    return false;
}

bool StoreGameByTitleId(unsigned long titleId, StoreGame *out)
{
    if (out == NULL || titleId == 0)
        return false;
    return FindByTitleId(kStoreGames, STORE_GAME_COUNT, titleId, out) ||
           FindByTitleId(kXboxGames, XBOX_GAME_COUNT, titleId, out);
}

const StoreRelease *StoreReleaseOf(const StoreGame *game, int version)
{
    if (game == NULL || version < 0 || version >= game->versionCount)
        return NULL;
    const int at = game->firstVersion + version;
    if (game->system == STORE_SYSTEM_XBOX)
        return (at < COUNT_OF(kXboxReleases)) ? &kXboxReleases[at] : NULL;
    return (at < COUNT_OF(kStoreReleases)) ? &kStoreReleases[at] : NULL;
}

const StoreDisc *StoreReleaseDisc(const StoreRelease *release, int disc)
{
    if (release == NULL || disc < 0 || disc >= release->discCount)
        return NULL;
    const int at = release->firstDisc + disc;
    if (IsXboxRelease(release))
    {
        if (at >= COUNT_OF(kXboxReleaseDiscs))
            return NULL;
        const int index = kXboxReleaseDiscs[at];
        return (index < COUNT_OF(kXboxDiscs)) ? &kXboxDiscs[index] : NULL;
    }
    if (at >= COUNT_OF(kStoreReleaseDiscs))
        return NULL;
    const int index = kStoreReleaseDiscs[at];
    return (index < COUNT_OF(kStoreDiscs)) ? &kStoreDiscs[index] : NULL;
}

static bool IsXblig(const XblaGame *game)
{
    return game >= kXbligGames && game < kXbligGames + XBLIG_GAME_COUNT;
}

ArcadeSet ArcadeSetOf(const XblaGame *game)
{
    return IsXblig(game) ? ARCADE_XBLIG : ARCADE_XBLA;
}

const char *ArcadeItemOf(const XblaGame *game)
{
    if (game == NULL)
        return "";
    if (IsXblig(game))
        return game->item < COUNT_OF(kXbligItems) ? kXbligItems[game->item] : "";
    return game->item < COUNT_OF(kXblaItems) ? kXblaItems[game->item] : "";
}

int ArcadeGamesForLetter(ArcadeSet set, char letter, const XblaGame **out, int maxGames)
{
    if (out == NULL || maxGames <= 0)
        return 0;

    // The whole table, not just a run: its order is by name, ignoring case,
    // which keeps a letter's games together - but a few thousand rows is
    // nothing to read.
    const XblaGame *games = (set == ARCADE_XBLIG) ? kXbligGames : kXblaGames;
    const int total = (set == ARCADE_XBLIG) ? XBLIG_GAME_COUNT : XBLA_GAME_COUNT;
    int count = 0;
    for (int i = 0; i < total && count < maxGames; ++i)
    {
        if (games[i].letter == letter)
            out[count++] = &games[i];
    }
    return count;
}

const XblaGame *XblaGameByTitleId(unsigned long titleId)
{
    for (int i = 0; i < XBLA_GAME_COUNT; ++i)
    {
        if (kXblaGames[i].titleId == titleId)
            return &kXblaGames[i];
    }
    return NULL;
}

const XblaGame *ArcadeGameByRar(const char *rar)
{
    if (rar == NULL)
        return NULL;
    for (int i = 0; i < XBLA_GAME_COUNT; ++i)
    {
        if (strcmp(kXblaGames[i].rar, rar) == 0)
            return &kXblaGames[i];
    }
    for (int i = 0; i < XBLIG_GAME_COUNT; ++i)
    {
        if (strcmp(kXbligGames[i].rar, rar) == 0)
            return &kXbligGames[i];
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

#define SEARCH_TEXT_MAX 256

// Lowercase letters and digits with one space between words - "Spider-Man:
// Web of Shadows" is "spider man web of shadows". Bytes past ASCII (UTF-8)
// are kept, so an accented name still matches itself. *compact: the same
// without the spaces.
static void NormaliseForSearch(const char *text, char *spaced, char *compact)
{
    size_t s = 0, c = 0;
    bool gap = false;
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0' && s + 2 < SEARCH_TEXT_MAX; ++p)
    {
        unsigned char ch = *p;
        if (ch >= 'A' && ch <= 'Z')
            ch = (unsigned char)(ch - 'A' + 'a');
        const bool word = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch >= 0x80;
        if (ch == '\'')
            continue; // "Assassin's" and "Assassins" alike
        if (!word)
        {
            gap = (s > 0);
            continue;
        }
        if (gap)
            spaced[s++] = ' ';
        gap = false;
        spaced[s++] = (char)ch;
        compact[c++] = (char)ch;
    }
    spaced[s] = '\0';
    compact[c] = '\0';
}

struct SearchQuery
{
    char spaced[SEARCH_TEXT_MAX];
    char compact[SEARCH_TEXT_MAX];
    const char *words[16];
    int wordCount;
    unsigned long titleId; // non-zero for a query of eight hex digits
};

// How well a name matches: 0 exactly, 1 from its start, 2 each word at a
// word's start, 3 somewhere; -1 not at all.
static int MatchName(const SearchQuery &q, const char *name)
{
    char spaced[SEARCH_TEXT_MAX], compact[SEARCH_TEXT_MAX];
    NormaliseForSearch(name, spaced, compact);
    if (strcmp(compact, q.compact) == 0)
        return 0;
    if (strncmp(compact, q.compact, strlen(q.compact)) == 0)
        return 1;

    bool all = true, allAtStarts = true;
    for (int w = 0; w < q.wordCount && all; ++w)
    {
        bool found = false, atStart = false;
        for (const char *at = strstr(spaced, q.words[w]); at != NULL; at = strstr(at + 1, q.words[w]))
        {
            found = true;
            if (at == spaced || at[-1] == ' ')
            {
                atStart = true;
                break;
            }
        }
        all = found;
        allAtStarts = allAtStarts && atStart;
    }
    if (all)
        return allAtStarts ? 2 : 3;

    // Typed without its spaces - "halo3", "callofduty" - whole words of the
    // name, so "gears" doesn't find Metal Gear Solid ("gear s...").
    for (const char *start = spaced; *start != '\0'; ++start)
    {
        if (start != spaced && start[-1] != ' ')
            continue;
        const char *want = q.compact;
        const char *p = start;
        for (; *p != '\0' && *want != '\0'; ++p)
        {
            if (*p == ' ')
                continue;
            if (*p != *want)
                break;
            want++;
        }
        if (*want == '\0' && (*p == '\0' || *p == ' '))
            return 3;
    }
    return -1;
}

struct RankedHit
{
    StoreHit hit;
    int rank;
    int order; // its place in the lists, for ties
};

static int CompareRankedHits(const void *a, const void *b)
{
    const RankedHit *x = (const RankedHit *)a;
    const RankedHit *y = (const RankedHit *)b;
    if (x->rank != y->rank)
        return x->rank - y->rank;
    return x->order - y->order;
}

#define SEARCH_MAX_MATCHES (STORE_GAME_COUNT + XBOX_GAME_COUNT + XBLA_GAME_COUNT + XBLIG_GAME_COUNT)
static RankedHit g_matches[SEARCH_MAX_MATCHES];

int SearchStore(const char *query, StoreHit *out, int maxHits, int *outTotal)
{
    if (outTotal != NULL)
        *outTotal = 0;
    if (query == NULL || out == NULL || maxHits <= 0)
        return 0;

    static SearchQuery q;
    memset(&q, 0, sizeof(q));
    NormaliseForSearch(query, q.spaced, q.compact);
    if (q.compact[0] == '\0')
        return 0;
    for (char *p = q.spaced; *p != '\0' && q.wordCount < 16;)
    {
        q.words[q.wordCount] = p;
        char *space = strchr(p, ' ');
        q.wordCount++;
        if (space == NULL)
            break;
        *space = '\0'; // the words are read as strings from here on
        p = space + 1;
    }
    if (strlen(q.compact) == 8)
    {
        char *end = NULL;
        const unsigned long id = strtoul(q.compact, &end, 16);
        if (end != NULL && *end == '\0')
            q.titleId = id;
    }

    int n = 0, order = 0;
    for (int i = 0; i < STORE_GAME_COUNT; ++i, ++order)
    {
        const int rank = (q.titleId != 0 && kStoreGames[i].titleId == q.titleId) ? 0 : MatchName(q, kStoreGames[i].name);
        if (rank < 0)
            continue;
        RankedHit &m = g_matches[n++];
        m.hit.kind = STORE_HIT_XBOX360;
        m.hit.game = &kStoreGames[i];
        m.hit.arcade = NULL;
        m.rank = rank;
        m.order = order;
    }
    for (int i = 0; i < XBOX_GAME_COUNT; ++i, ++order)
    {
        const int rank = (q.titleId != 0 && kXboxGames[i].titleId == q.titleId) ? 0 : MatchName(q, kXboxGames[i].name);
        if (rank < 0)
            continue;
        RankedHit &m = g_matches[n++];
        m.hit.kind = STORE_HIT_XBOX;
        m.hit.game = &kXboxGames[i];
        m.hit.arcade = NULL;
        m.rank = rank;
        m.order = order;
    }
    for (int set = 0; set < 2; ++set)
    {
        const XblaGame *games = (set == 1) ? kXbligGames : kXblaGames;
        const int total = (set == 1) ? XBLIG_GAME_COUNT : XBLA_GAME_COUNT;
        for (int i = 0; i < total; ++i, ++order)
        {
            // Indie games share one title ID, so it finds none of them.
            const bool byId = (q.titleId != 0 && set == 0 && games[i].titleId == q.titleId);
            const int rank = byId ? 0 : MatchName(q, games[i].name);
            if (rank < 0)
                continue;
            RankedHit &m = g_matches[n++];
            m.hit.kind = (set == 1) ? STORE_HIT_XBLIG : STORE_HIT_XBLA;
            m.hit.game = NULL;
            m.hit.arcade = &games[i];
            m.rank = rank;
            m.order = order;
        }
    }

    qsort(g_matches, n, sizeof(RankedHit), CompareRankedHits);
    if (outTotal != NULL)
        *outTotal = n;
    const int written = n < maxHits ? n : maxHits;
    for (int i = 0; i < written; ++i)
        out[i] = g_matches[i].hit;
    return written;
}
