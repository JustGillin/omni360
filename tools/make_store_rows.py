"""Builds src/StoreRows.h: the Store front page's rows - Popular, Top rated,
and one for each genre - from the Xbox Live catalog, and every game's genre,
tags and rating for the letter pages' filters and sorting.

    python tools/make_store_rows.py

For every 360 game in the Store (src/StoreTitles.h) it asks the catalog -
the one the game pages read, catalog.xboxlive.com, public, plain HTTP - for
its genres, its star rating and how many people rated it. That count is the
measure of popular: Halo 3 has 424,088. No one's downloads are counted.

Answers are kept in tools/cache/catalog.json, so a rerun asks only about
games added since. Run it again after make_store_titles.py.
"""
import concurrent.futures
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE_TITLES = os.path.join(ROOT, "src", "StoreTitles.h")
OUT_HEADER = os.path.join(ROOT, "src", "StoreRows.h")
CACHE = os.path.join(ROOT, "tools", "cache", "catalog.json")

CATALOG_URL = ("http://catalog.xboxlive.com/Catalog/Catalog.asmx/Query?methodName=FindGames"
               "&Names=Locale&Values=en-US&Names=LegalLocale&Values=en-US&Names=Store&Values=1"
               "&Names=PageSize&Values=10&Names=PageNum&Values=1&Names=DetailView&Values=5"
               "&Names=OfferFilterLevel&Values=1&Names=MediaIds&Values=66acd000-77fe-1000-9115-d802%08x"
               "&Names=UserTypes&Values=2&Names=MediaTypes&Values=1&Names=MediaTypes&Values=21"
               "&Names=MediaTypes&Values=23&Names=MediaTypes&Values=37&Names=MediaTypes&Values=46")

ROW_GAMES = 24          # games in a row
GENRE_MIN_GAMES = 12    # a genre with fewer has no row
TOP_RATED_MIN = 2000    # ratings a game needs to be "top rated" - five stars from three people isn't
XBOX_TOP_RATED_MIN = 20 # the same for an Original Xbox game: LaunchBox's users, a few hundred at most
# Categories that aren't genres: the two every game has, and the video
# store's, which some games carry too ("Sports" is its; games' is "Sports &
# Recreation").
SKIP_GENRES = {"Game Genres", "Xbox LIVE Games", "Other", "Classics", "Xbox LIVE Marketplace",
               "Independent & Music Videos", "Music and Short Videos", "Sports"}

# A game's tags come alphabetically, so they don't say which matters most.
# Its main genre is the first of these it has - the most specific - with
# Action & Adventure, the catch-all, last. Fighting and Platformer together
# are a brawler like Batman's: Action & Adventure.
MAIN_GENRES = ["Shooter", "Racing & Flying", "Fighting", "Role Playing", "Sports & Recreation", "Music",
               "Puzzle & Trivia", "Strategy & Simulation", "Platformer", "Action & Adventure"]

# Tags that make rows of their own, as well as a game's main genre.
TAG_ROWS = ["Family", "Kinect"]

# Shown names for the catalog's genres, where they're clumsy.
GENRE_NAMES = {
    "Action & Adventure": "Action and adventure",
    "Racing & Flying": "Racing and flying",
    "Role Playing": "Role-playing",
    "Sports & Recreation": "Sports",
    "Strategy & Simulation": "Strategy and simulation",
    "Puzzle & Trivia": "Puzzle and trivia",
    "Card & Board": "Card and board",
    "Music": "Music and rhythm",
    "Shooter": "Shooters",
    "Fighting": "Fighting games",
    "Platformer": "Platformers",
    "Family": "For the family",
}

ROW = re.compile(r'^\s*\{ "((?:[^"\\]|\\.)*)", 0x([0-9A-F]{8}), 0x([0-9A-F]+), \d+, \d+, \'.\' \},')

XBLA_TITLES = os.path.join(ROOT, "src", "XblaTitles.h")
XBLA_ROW = re.compile(r'^\s*\{ "((?:[^"\\]|\\.)*)", "(?:[^"\\]|\\.)*", \d+ULL, 0x([0-9A-F]{8}), 0x000D0000,')


def c_name(raw):
    return raw.encode("latin-1").decode("unicode_escape").encode("latin-1").decode("utf-8", "replace")


XBOX_TITLES = os.path.join(ROOT, "src", "XboxTitles.h")
XBOX_ROW = re.compile(r'^\s*\{ "((?:[^"\\]|\\.)*)", 0x([0-9A-F]{8}), 0x[0-9A-F]+, \d+, \d+, \'.\', STORE_SYSTEM_XBOX \},')
LAUNCHBOX_XBOX = os.path.join(ROOT, "tools", "cache", "launchbox_xbox.json")


def xbox_games():
    """{title ID: name} of the Store's Original Xbox games - the first of
    several sharing one, a USA or World one."""
    games = {}
    with open(XBOX_TITLES, encoding="latin-1") as f:
        for line in f:
            m = XBOX_ROW.match(line)
            if m and m.group(2) != "00000000":
                games.setdefault(int(m.group(2), 16), c_name(m.group(1)))
    return games


def arcade_games():
    """{title ID: name} of the Store's Arcade games (src/XblaTitles.h) - the
    first, where several RARs hold the same game, as XblaGameByTitleId picks."""
    games = {}
    with open(XBLA_TITLES, encoding="latin-1") as f:
        for line in f:
            m = XBLA_ROW.match(line)
            if not m:
                continue
            tid = int(m.group(2), 16)
            if tid not in games:
                games[tid] = c_name(m.group(1))
    return games


def store_games():
    """{title ID: name} of the Store's 360 games - a USA or World one's name
    where several share an ID, as StoreGameByTitleId picks."""
    games = {}
    with open(STORE_TITLES, encoding="latin-1") as f:
        for line in f:
            m = ROW.match(line)
            if not m or m.group(2) == "00000000":
                continue
            tid, regions = int(m.group(2), 16), int(m.group(3), 16)
            name = c_name(m.group(1))
            if tid not in games or regions & 0x0003:
                games[tid] = name
    return games


def ask(tid):
    req = urllib.request.Request(CATALOG_URL % tid, headers={"User-Agent": "Omni360 make_store_rows.py"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                xml = r.read().decode("utf-8", "replace")
            break
        except Exception:
            if attempt == 2:
                return None
    total = re.search(r"<live:totalItems>(\d+)", xml)
    if not total or int(total.group(1)) == 0:
        return {"none": True}
    ratings = re.search(r"<live:numberOfRatings>(\d+)", xml)
    stars = re.search(r"<live:ratingAggregate>([\d.]+)", xml)
    cats = re.search(r"<live:categories>(.*?)</live:categories>", xml, re.S)
    genres = []
    if cats:
        for name in re.findall(r"<live:name>([^<]*)</live:name>", cats.group(1)):
            name = name.replace("&amp;", "&")
            if name not in genres:
                genres.append(name)
    return {"ratings": int(ratings.group(1)) if ratings else 0,
            "stars": float(stars.group(1)) if stars else 0.0, "genres": genres}


def c_string(s):
    out = []
    for b in s.encode("utf-8"):
        if b in (0x22, 0x5C):
            out.append("\\" + chr(b))
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append("\\x%02X" % b)
    return '"' + "".join(out) + '"'


def ask_all(tids, cache):
    """Asks the catalog about the title IDs it hasn't answered for yet."""
    todo = [tid for tid in tids if "%08X" % tid not in cache]
    print("%d games, %d to ask the catalog about" % (len(tids), len(todo)))
    done = 0
    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for tid, answer in zip(todo, pool.map(ask, todo)):
            if answer is not None:
                cache["%08X" % tid] = answer
            done += 1
            if done % 100 == 0:
                print("  %d of %d" % (done, len(todo)))
                os.makedirs(os.path.dirname(CACHE), exist_ok=True)
                json.dump(cache, open(CACHE, "w", encoding="utf-8"))
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    json.dump(cache, open(CACHE, "w", encoding="utf-8"))


def genre_tags(info):
    """A game's main genre (or None) and its TAG_ROWS tags."""
    tags = [g for g in info["genres"] if g not in SKIP_GENRES]
    if "Fighting" in tags and "Platformer" in tags:
        tags = [g for g in tags if g not in ("Fighting", "Platformer")] + ["Action & Adventure"]
    return next((g for g in MAIN_GENRES if g in tags), None), [g for g in TAG_ROWS if g in tags]


def build_rows(games, known, tag_rows, top_rated_min=TOP_RATED_MIN):
    """Popular, Top rated, then a row for each genre with enough games, the
    most-played first - of `games` ({title ID: name}), from `known`."""
    mine = {t: known[t] for t in games if t in known}

    def by_popular(tids):
        return sorted(tids, key=lambda t: (-mine[t]["ratings"], games[t].lower()))

    rows = [("Popular", by_popular(mine)[:ROW_GAMES])]
    rated = [t for t in mine if mine[t]["ratings"] >= top_rated_min]
    rows.append(("Top rated", sorted(rated, key=lambda t: (-mine[t]["stars"], -mine[t]["ratings"]))[:ROW_GAMES]))

    by_genre = {}
    for t, info in mine.items():
        main_genre, tags = genre_tags(info)
        if main_genre:
            by_genre.setdefault(main_genre, []).append(t)
        for g in tags:
            if g in tag_rows:
                by_genre.setdefault(g, []).append(t)
    genre_rows = []
    for g, tids in by_genre.items():
        if len(tids) >= GENRE_MIN_GAMES:
            ranked = by_popular(tids)
            genre_rows.append((sum(mine[t]["ratings"] for t in ranked[:ROW_GAMES]), GENRE_NAMES.get(g, g),
                               ranked[:ROW_GAMES]))
    genre_rows.sort(reverse=True)
    return rows + [(name, tids) for _, name, tids in genre_rows], len(mine)


def row_lines(prefix, rows, games):
    """kXxxRowGames and kXxxRows, for rows of (name, title IDs)."""
    L = ["// Title IDs, a row after another.", "static const unsigned long k%sRowGames[] =" % prefix, "{"]
    first, at = [], 0
    for name, tids in rows:
        first.append(at)
        L.append("    // %s" % name)
        for t in tids:
            L.append("    0x%08X, // %s" % (t, games[t].encode("ascii", "replace").decode("ascii")))
        at += len(tids)
    L += ["};", "", "static const StoreRow k%sRows[%s_ROW_COUNT] =" % (prefix, prefix.upper()), "{"]
    for (name, tids), f in zip(rows, first):
        L.append("    { %s, %d, %d }," % (c_string(name), f, len(tids)))
    return L + ["};", ""]


def main():
    games = store_games()
    arcade = arcade_games()
    cache = json.load(open(CACHE, encoding="utf-8")) if os.path.exists(CACHE) else {}
    ask_all(list(games) + [t for t in arcade if t not in games], cache)

    every = dict(arcade)
    every.update(games)  # a 360 game's name where an ID is both
    known = {tid: cache["%08X" % tid] for tid in every
             if "%08X" % tid in cache and not cache["%08X" % tid].get("none")}

    rows, known360 = build_rows(games, known, TAG_ROWS)
    arcade_rows, known_arcade = build_rows(arcade, known, ["Family"])

    # The Original Xbox's, from LaunchBox (make_xbox_info.py) - Xbox Live has
    # none of them. Their title IDs are their own, so they sit in the same
    # table. Their counts are LaunchBox users', so Top rated asks fewer.
    xbox = xbox_games()
    if os.path.exists(LAUNCHBOX_XBOX):
        for key, info in json.load(open(LAUNCHBOX_XBOX, encoding="utf-8")).items():
            tid = int(key, 16)
            if tid in xbox and tid not in known:
                known[tid] = info
    else:
        print("no %s - run make_xbox_info.py for the Original Xbox's" % LAUNCHBOX_XBOX)
    xbox_rows, known_xbox = build_rows(xbox, known, [], XBOX_TOP_RATED_MIN)
    print("%d of %d Original Xbox games from LaunchBox" % (known_xbox, len(xbox)))
    print("%d of %d 360 games and %d of %d Arcade games in the catalog"
          % (known360, len(games), known_arcade, len(arcade)))

    main_of, tags_of = {}, {}
    for t, info in known.items():
        main_genre, tags = genre_tags(info)
        if main_genre:
            main_of[t] = main_genre
        tags_of[t] = tags

    genre_names = sorted(GENRE_NAMES.get(g, g) for g in MAIN_GENRES)

    L = ["// GENERATED by tools/make_store_rows.py - do not edit by hand.",
         "//",
         "// The Store front page's rows, under its A to Z: Popular - by how many",
         "// people rated a game on Xbox Live - Top rated, of games with %d ratings or" % TOP_RATED_MIN,
         "// more, then a row for each genre with %d games or more, the most-played" % GENRE_MIN_GAMES,
         "// first: a game in its main genre's, and Family's and Kinect's if tagged so.",
         "// The Arcade page's rows, the same way, from its games. And each game's",
         "// genre, tags and rating, for the letter pages' filters - 360 and Arcade.",
         "// From the Xbox Live catalog, for %d of the Store's %d 360 games and %d of"
         % (known360, len(games), known_arcade),
         "// its %d Arcade games." % len(arcade),
         "",
         "#ifndef STORE_ROWS_H",
         "#define STORE_ROWS_H",
         "",
         "struct StoreRow",
         "{",
         "    const char *name;",
         "    unsigned short first; // into kStoreRowGames, or kXblaRowGames",
         "    unsigned char count;",
         "};",
         "",
         "#define STORE_ROW_COUNT %d" % len(rows),
         "#define XBLA_ROW_COUNT %d" % len(arcade_rows),
         "#define XBOX_ROW_COUNT %d" % len(xbox_rows),
         "#define STORE_ROW_MAX_GAMES %d" % ROW_GAMES,
         "",
         "// A game's genre, tags and rating, by title ID - every one the catalog has.",
         "struct StoreGameInfo",
         "{",
         "    unsigned long titleId;",
         "    unsigned long ratings;  // how many people rated it",
         "    unsigned char stars;    // their average, in tenths: 0 to 50",
         "    unsigned char genre;    // its main genre: 1 + an index into kStoreGenres; 0 for none",
         "    unsigned char tags;     // STORE_TAG_*",
         "};",
         "",
         "#define STORE_GENRE_COUNT %d" % len(genre_names),
         ] + ["#define STORE_TAG_%-7s 0x%02X" % (g.upper(), 1 << i) for i, g in enumerate(TAG_ROWS)] + [
         "#define STORE_INFO_COUNT %d" % len(known),
         "",
         "#ifdef STORE_ROWS_DATA",
         "",
         "// The main genres' shown names, A to Z.",
         "static const char *const kStoreGenres[STORE_GENRE_COUNT] =",
         "{"] + ["    %s," % c_string(name) for name in genre_names] + [
         "};",
         "",
         "// Sorted by title ID.",
         "static const StoreGameInfo kStoreGameInfo[STORE_INFO_COUNT] =",
         "{"] + ["    { 0x%08X, %d, %d, %d, 0x%02X }," % (
             t, known[t]["ratings"], int(round(known[t]["stars"] * 10)),
             genre_names.index(GENRE_NAMES.get(main_of[t], main_of[t])) + 1 if t in main_of else 0,
             sum(1 << i for i, g in enumerate(TAG_ROWS) if g in tags_of[t])) for t in sorted(known)] + [
         "};",
         "",
         "// The front page's - each a game StoreGameByTitleId finds."]
    L += row_lines("Store", rows, games)
    L += ["// The Arcade page's - each a game XblaGameByTitleId finds."]
    L += row_lines("Xbla", arcade_rows, arcade)
    L += ["// The Original Xbox page's - each a game StoreGameByTitleId finds."]
    L += row_lines("Xbox", xbox_rows, xbox)
    L += ["#endif // STORE_ROWS_DATA", "", "#endif", ""]
    with open(OUT_HEADER, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(L))
    for label, rs, names in (("360", rows, games), ("Arcade", arcade_rows, arcade), ("Original Xbox", xbox_rows, xbox)):
        print("--", label)
        for name, tids in rs:
            print("%-26s %s" % (name, ", ".join(names[t] for t in tids[:5])))
    print("wrote %s: %d rows, %d Arcade rows" % (OUT_HEADER, len(rows), len(arcade_rows)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
