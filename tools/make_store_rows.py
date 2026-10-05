"""Builds src/StoreRows.h: the Store front page's rows - Popular, Top rated,
and one for each genre - from the Xbox Live catalog.

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
            name = m.group(1).encode("latin-1").decode("unicode_escape").encode("latin-1").decode("utf-8", "replace")
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


def main():
    games = store_games()
    cache = json.load(open(CACHE, encoding="utf-8")) if os.path.exists(CACHE) else {}
    todo = [tid for tid in games if "%08X" % tid not in cache]
    print("%d games, %d to ask the catalog about" % (len(games), len(todo)))
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

    known = {tid: cache["%08X" % tid] for tid in games
             if "%08X" % tid in cache and not cache["%08X" % tid].get("none")}
    print("%d in the catalog" % len(known))

    def by_popular(tids):
        return sorted(tids, key=lambda t: (-known[t]["ratings"], games[t].lower()))

    rows = [("Popular", by_popular(known)[:ROW_GAMES])]
    rated = [t for t in known if known[t]["ratings"] >= TOP_RATED_MIN]
    rows.append(("Top rated", sorted(rated, key=lambda t: (-known[t]["stars"], -known[t]["ratings"]))[:ROW_GAMES]))

    # Genres, the most-played first: each game in its main genre's row, and
    # in Family's and Kinect's if it's tagged so.
    by_genre = {}
    for t, info in known.items():
        tags = [g for g in info["genres"] if g not in SKIP_GENRES]
        if "Fighting" in tags and "Platformer" in tags:
            tags = [g for g in tags if g not in ("Fighting", "Platformer")] + ["Action & Adventure"]
        main_genre = next((g for g in MAIN_GENRES if g in tags), None)
        if main_genre:
            by_genre.setdefault(main_genre, []).append(t)
        for g in TAG_ROWS:
            if g in tags:
                by_genre.setdefault(g, []).append(t)
    genre_rows = []
    for g, tids in by_genre.items():
        if len(tids) >= GENRE_MIN_GAMES:
            ranked = by_popular(tids)
            genre_rows.append((sum(known[t]["ratings"] for t in ranked[:ROW_GAMES]), GENRE_NAMES.get(g, g), ranked[:ROW_GAMES]))
    genre_rows.sort(reverse=True)
    rows += [(name, tids) for _, name, tids in genre_rows]

    L = ["// GENERATED by tools/make_store_rows.py - do not edit by hand.",
         "//",
         "// The Store front page's rows, under its A to Z: Popular - by how many",
         "// people rated a game on Xbox Live - Top rated, of games with %d ratings or" % TOP_RATED_MIN,
         "// more, then a row for each genre with %d games or more, the most-played" % GENRE_MIN_GAMES,
         "// first: a game in its main genre's, and Family's and Kinect's if tagged so.",
         "// From the Xbox Live catalog, for %d of the Store's %d 360 games." % (len(known), len(games)),
         "",
         "#ifndef STORE_ROWS_H",
         "#define STORE_ROWS_H",
         "",
         "struct StoreRow",
         "{",
         "    const char *name;",
         "    unsigned short first; // into kStoreRowGames",
         "    unsigned char count;",
         "};",
         "",
         "#define STORE_ROW_COUNT %d" % len(rows),
         "#define STORE_ROW_MAX_GAMES %d" % ROW_GAMES,
         "",
         "#ifdef STORE_ROWS_DATA",
         "",
         "// Title IDs, a row after another - each a game StoreGameByTitleId finds.",
         "static const unsigned long kStoreRowGames[] =",
         "{"]
    first = []
    at = 0
    for name, tids in rows:
        first.append(at)
        L.append("    // %s" % name)
        for t in tids:
            L.append("    0x%08X, // %s" % (t, games[t].encode("ascii", "replace").decode("ascii")))
        at += len(tids)
    L += ["};", "", "static const StoreRow kStoreRows[STORE_ROW_COUNT] =", "{"]
    for (name, tids), f in zip(rows, first):
        L.append("    { %s, %d, %d }," % (c_string(name), f, len(tids)))
    L += ["};", "", "#endif // STORE_ROWS_DATA", "", "#endif", ""]
    with open(OUT_HEADER, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(L))
    for name, tids in rows:
        print("%-26s %s" % (name, ", ".join(games[t] for t in tids[:5])))
    print("wrote %s: %d rows" % (OUT_HEADER, len(rows)))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
