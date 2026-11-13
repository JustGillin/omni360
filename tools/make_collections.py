"""Builds src/StoreCollections.h: the Store front page's Collections - a
series each, its games oldest first - from tools/data/collections.json.

    python tools/make_collections.py          check the file and write the header
    python tools/make_collections.py --art    ...and that each card's game has a wallpaper (asks Xbox Live)

"publishers" are a row of their own under them: a name and the title ID
prefixes that are its - a title ID's first half is its publisher's - and
its games are picked here, its most rated on Xbox Live first, as
src/StoreRows.h has them (so build that first). "art" is optional: its most
rated game's, when not given. "include" names games of its own by title ID
that another's prefix has - Batman: Arkham Asylum is Eidos's, but Warner
Bros.' - and takes them out of every other publisher's. A publisher's card is
"tint", its colour washed over off-white, and "shade" for its name, both
RRGGBB, with its "logos" - one or two of tools/make_publisher_logos.py's,
numbered as that atlas has them.

"spotlight", beside "collections", is one more the same way - "Only on Xbox
360" - drawn as a banner over them, and left out of a game page's "More like
this". It's optional.

A collection is a name, a game whose wallpaper is its card ("art": a 360
game - Xbox Live has none for Original Xbox ones), and its games' title IDs
in release order. Title IDs are as tools/featured.py search finds them, so
each is a game the Store can open; Original Xbox games may be in the list.
"""
import json
import os
import re
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import featured  # the Store's own lists, as its title IDs resolve
import make_publisher_logos

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "tools", "data", "collections.json")
OUT_HEADER = os.path.join(ROOT, "src", "StoreCollections.h")
ROWS_HEADER = os.path.join(ROOT, "src", "StoreRows.h")

MAX_COLLECTIONS = 32  # STORE_COLLECTIONS_MAX in GameListUI.h - and the publishers' most
MAX_GAMES = 24        # a collection's page holds this many at most


def c_string(s):
    """A C string literal for UTF-8 text, other bytes escaped."""
    out = []
    for b in s.encode("utf-8"):
        if b in (0x22, 0x5C):
            out.append(chr(92) + chr(b))
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append(chr(92) + "x%02X" % b)
    # A hex escape runs on into hex digits after it: split the literal there.
    return re.sub(r'(\\x[0-9A-F]{2})(?=[0-9A-Fa-f])', r'\1""', "".join(out))


def ratings():
    """Each title ID's count of ratings on Xbox Live, as StoreRows.h has them."""
    with open(ROWS_HEADER, encoding="utf-8") as f:
        text = f.read()
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"\{\s*0x([0-9A-F]{8}),\s*(\d+)", text)}


def publisher_games(p, shown, rated, claimed):
    """A publisher's 360 games the Store has, most rated first - none unrated.
    Those it includes by title ID are its; those another includes aren't."""
    prefixes = [x.upper() for x in p.get("prefixes", [])]
    own = set(x.upper() for x in p.get("include", []))
    games = [tid for tid, g in shown.items()
             if g[1] == "360" and rated.get(tid, 0) > 0
             and (tid in own or (tid[:4] in prefixes and tid not in claimed))]
    games.sort(key=lambda tid: (-rated[tid], tid))
    return games[:MAX_GAMES]


def main():
    shown = featured.by_id(featured.load_games())
    with open(SOURCE, encoding="utf-8") as f:
        data = json.load(f)
    collections = data["collections"]
    spotlight = data.get("spotlight")

    errors = []
    rated = ratings()
    publishers = []
    claimed = set()
    for p in data.get("publishers", []):
        for tid in p.get("include", []):
            if tid.upper() not in shown:
                errors.append('"%s": includes %s, not a game the Store has' % (p.get("name", ""), tid))
            claimed.add(tid.upper())
    for p in data.get("publishers", []):
        games = publisher_games(p, shown, rated, claimed)
        if not games:
            errors.append('"%s": no rated 360 games under %s' % (p.get("name", ""), p.get("prefixes")))
            continue
        publishers.append({"name": p.get("name", ""), "art": p.get("art", games[0]), "games": games,
                           "tint": p.get("tint", ""), "shade": p.get("shade", ""), "logos": p.get("logos", [])})
    entries = collections + publishers + ([spotlight] if spotlight else [])

    if not 1 <= len(collections) <= MAX_COLLECTIONS:
        errors.append("needs 1 to %d collections" % MAX_COLLECTIONS)
    if len(publishers) > MAX_COLLECTIONS:
        errors.append("needs at most %d publishers" % MAX_COLLECTIONS)
    logo_keys = make_publisher_logos.logo_keys(data)
    for p in publishers:
        for field in ("tint", "shade"):
            if not re.fullmatch(r"[0-9A-Fa-f]{6}", p[field]):
                errors.append('"%s": needs a %s, RRGGBB' % (p["name"], field))
        if not 1 <= len(p["logos"]) <= 2:
            errors.append('"%s": needs one or two logos' % p["name"])
    names = set()
    for c in entries:
        name, art, games = c.get("name", ""), c.get("art", ""), c.get("games", [])
        where = '"%s"' % name
        if not name or name in names:
            errors.append("%s: needs a name of its own" % where)
        names.add(name)
        if not 1 <= len(games) <= MAX_GAMES:
            errors.append("%s: needs 1 to %d games" % (where, MAX_GAMES))
        if len(set(games)) != len(games):
            errors.append("%s: a game is in it twice" % where)
        for tid in games:
            if tid not in shown:
                errors.append("%s: %s isn't a game the Store has" % (where, tid))
        if art not in shown or shown[art][1] != "360":
            errors.append("%s: its art, %s, needs to be a 360 game the Store has" % (where, art))
        elif art not in games:
            errors.append("%s: its art, %s, isn't one of its games" % (where, art))

    if "--art" in sys.argv:
        for c in entries:
            try:
                with urllib.request.urlopen(featured.ART_URL % c["art"].lower(), timeout=10) as r:
                    ok = r.status == 200
            except Exception:
                ok = False
            if not ok:
                errors.append('"%s": %s has no wallpaper on Xbox Live - pick another game' % (c["name"], c["art"]))

    for e in errors:
        print("ERROR: " + e)
    if errors:
        return 1

    L = ["// Generated by tools/make_collections.py from tools/data/collections.json - don't edit.",
         "#ifndef STORE_COLLECTIONS_H",
         "#define STORE_COLLECTIONS_H",
         "",
         "struct StoreCollection",
         "{",
         "    const char *name;",
         "    unsigned long art;    // the game whose wallpaper is its card",
         "    unsigned short first; // its games in kStoreCollectionGames",
         "    unsigned short count;",
         "};",
         "",
         "// A publisher's card: its colour washed over off-white, its name in its",
         "// shade, and one or two logos from PublisherLogos.h; -1 for none.",
         "struct StorePublisherStyle",
         "{",
         "    unsigned long tint;  // 0xRRGGBB",
         "    unsigned long shade; // 0xRRGGBB",
         "    signed char logos[2];",
         "};",
         "",
         "#define STORE_COLLECTION_COUNT %d" % len(collections),
         "// Then the publishers, a row of their own, each its most rated games first.",
         "#define STORE_PUBLISHER_COUNT %d" % len(publishers),
         "// With the spotlight, when there is one: kStoreCollections' last entry.",
         "#define STORE_COLLECTION_ENTRIES %d" % len(entries),
         "",
         "#ifdef STORE_COLLECTIONS_DATA",
         "",
         "static const StoreCollection kStoreCollections[] =",
         "{"]
    first = 0
    for c in entries:
        L.append('    { "%s", 0x%s, %d, %d },' % (c_string(c["name"]), c["art"], first, len(c["games"])))
        first += len(c["games"])
    L += ["};", "",
          "// Title IDs, a collection after another, oldest first - each a game",
          "// StoreGameByTitleId finds.",
          "static const unsigned long kStoreCollectionGames[] =",
          "{"]
    for c in entries:
        L.append("    // " + c["name"])
        for tid in c["games"]:
            # The compiler reads comments in the system's code page: ASCII only.
            L.append("    0x%s, // %s" % (tid, shown[tid][0].encode("ascii", "replace").decode()))
    L += ["};", "", "static const StorePublisherStyle kStorePublisherStyles[STORE_PUBLISHER_COUNT + 1] =", "{"]
    for p in publishers:
        logos = [logo_keys.index(k) for k in p["logos"]] + [-1, -1]
        L.append("    { 0x%s, 0x%s, { %d, %d } }, // %s" % (p["tint"].upper(), p["shade"].upper(), logos[0], logos[1],
                                                         p["name"].encode("ascii", "replace").decode()))
    L += ["    { 0, 0, { -1, -1 } }, // so the table is never empty",
          "};", "", "#endif // STORE_COLLECTIONS_DATA", "", "#endif", ""]

    with open(OUT_HEADER, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(L))
    print("%d collections, %d publishers%s, %d games -> %s" % (len(collections), len(publishers),
                                                           " and the spotlight" if spotlight else "", first, os.path.relpath(OUT_HEADER, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
