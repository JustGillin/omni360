#define STORE_TITLES_DATA
#define XBOX_TITLES_DATA
#define XBLA_TITLES_DATA
#define XBLIG_TITLES_DATA
#define STORE_ROWS_DATA
#define STORE_COLLECTIONS_DATA
#include "StoreCatalog.h"
#include "StoreRows.h"
#include "StoreCollections.h"

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
        if (letter == STORE_LETTER_ALL || kStoreGames[i].letter == letter)
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
        if (letter == STORE_LETTER_ALL || kXboxGames[i].letter == letter)
            out[count++] = kXboxGames[i];
        else if (count > 0)
            break;
    }
    return count;
}

// As StoreGameByTitleId, in one table.
static bool FindByTitleId(const StoreGame *games, int count, unsigned long titleId, StoreGame *out)
{
    // The game it names first - a USA or World one, of several: Viva
    // Pinata, not Japan's "Atsumare! Pinata" - else one with a disc that
    // has it.
    int found = -1;
    for (int i = 0; i < count; ++i)
    {
        if (games[i].titleId != titleId)
            continue;
        if (found < 0)
            found = i;
        if (games[i].regions & (STORE_REGION_USA | STORE_REGION_WORLD))
        {
            found = i;
            break;
        }
    }
    if (found >= 0)
    {
        *out = games[found];
        return true;
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

unsigned short StoreReleaseRegions(const StoreRelease *release)
{
    unsigned short regions = 0;
    for (int d = 0; release != NULL && d < release->discCount; ++d)
    {
        const StoreDisc *disc = StoreReleaseDisc(release, d);
        if (disc != NULL)
            regions |= disc->regions;
    }
    return regions;
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
        if (letter == STORE_LETTER_ALL || games[i].letter == letter)
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

// ---------------------------------------------------------------------------
// The front page's rows
// ---------------------------------------------------------------------------
// The letter pages' filters
// ---------------------------------------------------------------------------

bool StoreGameInfoOf(unsigned long titleId, StoreGameInfo *out)
{
    int lo = 0, hi = STORE_INFO_COUNT - 1;
    while (titleId != 0 && lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (kStoreGameInfo[mid].titleId == titleId)
        {
            *out = kStoreGameInfo[mid];
            return true;
        }
        if (kStoreGameInfo[mid].titleId < titleId)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

const char *StoreGenreName(int genre)
{
    return (genre >= 1 && genre <= STORE_GENRE_COUNT) ? kStoreGenres[genre - 1] : "";
}

// Redump's codes for them, and the regions whose releases have that one
// language when they list none.
static const struct { const char *code; const char *name; unsigned short regions; } kLanguages[STORE_LANGUAGE_COUNT] =
{
    { "En", "English",    STORE_REGION_USA | STORE_REGION_WORLD | STORE_REGION_EUROPE | STORE_REGION_UK |
                          STORE_REGION_AUSTRALIA },
    { "Fr", "French",     STORE_REGION_FRANCE },
    { "De", "German",     STORE_REGION_GERMANY },
    { "Es", "Spanish",    STORE_REGION_SPAIN },
    { "It", "Italian",    STORE_REGION_ITALY },
    { "Ja", "Japanese",   STORE_REGION_JAPAN },
    { "Pt", "Portuguese", 0 },
    { "Ru", "Russian",    STORE_REGION_RUSSIA },
    { "Pl", "Polish",     0 },
    { "Ko", "Korean",     STORE_REGION_KOREA },
    { "Nl", "Dutch",      0 },
    { "Zh", "Chinese",    STORE_REGION_CHINA | STORE_REGION_TAIWAN },
};

const char *StoreLanguageName(int language)
{
    return (language >= 0 && language < STORE_LANGUAGE_COUNT) ? kLanguages[language].name : "";
}

// One part of a detail, len long: if it's a list of languages - "En, Fr, De"
// - their bits, with LISTED; else 0. Codes not filtered by count toward a
// list but add no bit.
#define LANGUAGES_LISTED 0x80000000UL

static unsigned long LanguagesIn(const char *s, int len)
{
    unsigned long bits = LANGUAGES_LISTED;
    int i = 0;
    for (;;)
    {
        if (i + 2 > len || s[i] < 'A' || s[i] > 'Z' || s[i + 1] < 'a' || s[i + 1] > 'z')
            return 0;
        for (int l = 0; l < STORE_LANGUAGE_COUNT; ++l)
        {
            if (s[i] == kLanguages[l].code[0] && s[i + 1] == kLanguages[l].code[1])
                bits |= 1UL << l;
        }
        i += 2;
        if (i == len)
            return bits;
        if (i + 2 > len || s[i] != ',' || s[i + 1] != ' ')
            return 0;
        i += 2;
    }
}

unsigned long StoreReleaseLanguages(const StoreRelease *release)
{
    if (release == NULL)
        return 0;

    // The detail's parts are joined as make_store_titles.py's MIDDOT.
    static const char kMiddot[] = "  \xC2\xB7  ";
    const int middotLen = (int)sizeof(kMiddot) - 1;

    const char *part = release->detail != NULL ? release->detail : "";
    while (*part != '\0')
    {
        const char *end = strstr(part, kMiddot);
        const int len = end != NULL ? (int)(end - part) : (int)strlen(part);
        const unsigned long listed = LanguagesIn(part, len);
        if (listed != 0)
            return listed & ~LANGUAGES_LISTED;
        part = end != NULL ? end + middotLen : part + len;
    }

    unsigned long bits = 0;
    const unsigned short regions = StoreReleaseRegions(release);
    for (int l = 0; l < STORE_LANGUAGE_COUNT; ++l)
    {
        if (regions & kLanguages[l].regions)
            bits |= 1UL << l;
    }
    return bits;
}

unsigned long StoreGameLanguages(const StoreGame *game)
{
    unsigned long bits = 0;
    for (int v = 0; game != NULL && v < game->versionCount; ++v)
        bits |= StoreReleaseLanguages(StoreReleaseOf(game, v));
    return bits;
}

// ---------------------------------------------------------------------------

int StoreRowCount()
{
    return STORE_ROW_COUNT;
}

const char *StoreRowName(int row)
{
    return (row >= 0 && row < STORE_ROW_COUNT) ? kStoreRows[row].name : "";
}

int StoreRowGames(int row, StoreGame *out, int maxGames)
{
    if (row < 0 || row >= STORE_ROW_COUNT || out == NULL)
        return 0;
    int n = 0;
    for (int i = 0; i < kStoreRows[row].count && n < maxGames; ++i)
    {
        if (StoreGameByTitleId(kStoreRowGames[kStoreRows[row].first + i], &out[n]))
            n++;
    }
    return n;
}

int StoreCollectionCount()
{
    return STORE_COLLECTION_COUNT;
}

const char *StoreCollectionName(int collection)
{
    return (collection >= 0 && collection < STORE_COLLECTION_COUNT) ? kStoreCollections[collection].name : "";
}

unsigned long StoreCollectionArt(int collection)
{
    return (collection >= 0 && collection < STORE_COLLECTION_COUNT) ? kStoreCollections[collection].art : 0;
}

int StoreCollectionGames(int collection, StoreGame *out, int maxGames)
{
    if (collection < 0 || collection >= STORE_COLLECTION_COUNT || out == NULL)
        return 0;
    const StoreCollection &c = kStoreCollections[collection];
    int n = 0;
    for (int i = 0; i < c.count && n < maxGames; ++i)
    {
        if (StoreGameByTitleId(kStoreCollectionGames[c.first + i], &out[n]))
            n++;
    }
    return n;
}

// "More like this": of its publisher's games, this many go ahead of the
// rest - and a game rated under three stars isn't much of a suggestion.
#define SIMILAR_SAME_PUBLISHER 4
#define SIMILAR_MIN_STARS      30 // tenths

static bool HasTitle(const StoreGame *games, int count, unsigned long titleId)
{
    for (int i = 0; i < count; ++i)
    {
        if (games[i].titleId == titleId)
            return true;
    }
    return false;
}

// Whether a game is named for Kinect - "Kinect Adventures!", "Kinect Star
// Wars": made for it, where one only tagged Kinect may just support it, as
// Skyrim's voice commands do.
static bool NamedForKinect(unsigned long titleId)
{
    StoreGame game;
    return StoreGameByTitleId(titleId, &game) && game.name != NULL && strstr(game.name, "Kinect") != NULL;
}

// The most rated game like `me` that isn't in out yet - of its publisher's
// alone, with samePublisher - as an index into kStoreGameInfo; -1 for none.
// Like it: the same genre; a Kinect game only for a Kinect game, and one
// made for Kinect only for another; and for a family game, one for the
// family too.
static int MostRatedLike(const StoreGameInfo &me, bool meForKinect, bool samePublisher, const StoreGame *out,
                         int count)
{
    int best = -1;
    for (int i = 0; i < STORE_INFO_COUNT; ++i)
    {
        const StoreGameInfo &g = kStoreGameInfo[i];
        if (g.titleId == me.titleId || g.genre != me.genre || g.stars < SIMILAR_MIN_STARS)
            continue;
        if ((g.tags & STORE_TAG_KINECT) && !(me.tags & STORE_TAG_KINECT))
            continue;
        if ((me.tags & STORE_TAG_FAMILY) && !(g.tags & STORE_TAG_FAMILY))
            continue;
        // A title ID's first half is its publisher's.
        if (samePublisher && (g.titleId >> 16) != (me.titleId >> 16))
            continue;
        if (best >= 0 && g.ratings <= kStoreGameInfo[best].ratings)
            continue;
        if (HasTitle(out, count, g.titleId))
            continue;
        // Last, as it's a lookup: only for one that would do otherwise.
        if ((g.tags & STORE_TAG_KINECT) && !meForKinect && NamedForKinect(g.titleId))
            continue;
        best = i;
    }
    return best;
}

int StoreSimilarGames(unsigned long titleId, StoreGame *out, int maxGames)
{
    if (out == NULL || titleId == 0)
        return 0;

    // The rest of its series - of each collection it's in.
    int n = 0;
    for (int c = 0; c < STORE_COLLECTION_COUNT; ++c)
    {
        const StoreCollection &col = kStoreCollections[c];
        bool in = false;
        for (int i = 0; i < col.count && !in; ++i)
            in = (kStoreCollectionGames[col.first + i] == titleId);
        for (int i = 0; in && i < col.count && n < maxGames; ++i)
        {
            const unsigned long id = kStoreCollectionGames[col.first + i];
            if (id != titleId && !HasTitle(out, n, id) && StoreGameByTitleId(id, &out[n]))
                n++;
        }
    }

    // Then its genre's, its publisher's few first.
    StoreGameInfo me;
    if (!StoreGameInfoOf(titleId, &me) || me.genre == 0)
        return n;
    const bool meForKinect = NamedForKinect(titleId);
    for (int pass = 0; pass < 2; ++pass)
    {
        const int limit = (pass == 0) ? n + SIMILAR_SAME_PUBLISHER : maxGames;
        while (n < limit && n < maxGames)
        {
            const int best = MostRatedLike(me, meForKinect, pass == 0, out, n);
            if (best < 0 || !StoreGameByTitleId(kStoreGameInfo[best].titleId, &out[n]))
                break;
            n++;
        }
    }
    return n;
}
