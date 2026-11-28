"""Builds src/XboxInfo.h - the Original Xbox games' details for their Store
pages - and tools/cache/launchbox_xbox.json, their genres and ratings for
make_store_rows.py, from the LaunchBox Games Database.

    python tools/make_xbox_info.py [--refresh]

Xbox Live's catalog has nothing on Original Xbox games, so their pages had
no description, genre or rating. LaunchBox's database does: it's one file,
gamesdb.launchbox-app.com/Metadata.zip (about 110 MB, updated daily), kept
in tools/cache/ and fetched again with --refresh. Each Store game
(src/XboxTitles.h) is matched by name, or by one of LaunchBox's alternate
names (regional titles), the way make_launchbox_covers.py matches covers.

Run make_store_rows.py after this: it puts these genres and ratings beside
the 360's and Arcade's, and makes the Original Xbox page's rows.
"""
import json
import os
import re
import sys
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XBOX_TITLES = os.path.join(ROOT, "src", "XboxTitles.h")
OUT_HEADER = os.path.join(ROOT, "src", "XboxInfo.h")
CACHE_DIR = os.path.join(ROOT, "tools", "cache")
METADATA_ZIP = os.path.join(CACHE_DIR, "launchbox-metadata.zip")
OUT_JSON = os.path.join(CACHE_DIR, "launchbox_xbox.json")
METADATA_URL = "https://gamesdb.launchbox-app.com/Metadata.zip"

OVERVIEW_MAX = 900  # characters of a description kept: about what a game page shows

# LaunchBox's genres, as the Xbox Live catalog names its - which
# make_store_rows.py knows. The first that maps wins, in MAIN_GENRES' order
# there; ones with no counterpart (Compilation) are left out.
GENRES = {
    "Shooter": "Shooter",
    "Racing": "Racing & Flying", "Flight Simulator": "Racing & Flying", "Vehicle Simulation": "Racing & Flying",
    "Fighting": "Fighting", "Beat 'em Up": "Fighting",
    "Role-Playing": "Role Playing",
    "Sports": "Sports & Recreation",
    "Music": "Music", "Party": "Music",
    "Puzzle": "Puzzle & Trivia", "Quiz": "Puzzle & Trivia", "Board Game": "Puzzle & Trivia",
    "Casino": "Puzzle & Trivia", "Pinball": "Puzzle & Trivia", "Education": "Puzzle & Trivia",
    "Strategy": "Strategy & Simulation", "Construction and Management Simulation": "Strategy & Simulation",
    "Life Simulation": "Strategy & Simulation", "Sandbox": "Strategy & Simulation",
    "Platform": "Platformer",
    "Action": "Action & Adventure", "Adventure": "Action & Adventure", "Stealth": "Action & Adventure",
    "Horror": "Action & Adventure", "Visual Novel": "Action & Adventure",
}

GAME = re.compile(r'^\s*\{ "((?:[^"\\]|\\.)*)", 0x([0-9A-F]{8}), 0x[0-9A-F]+, \d+, \d+, \'.\', STORE_SYSTEM_XBOX \},')

# Indie games, the same way, from LaunchBox's Xbox 360 games: they all share
# the XNA launcher's title ID, so each is known by IndieGameKey(name) - the
# key their tiles and LaunchBox covers already go by (CoverArt.cpp).
XBLIG_TITLES = os.path.join(ROOT, "src", "XbligTitles.h")
INDIE_HEADER = os.path.join(ROOT, "src", "IndieInfo.h")
INDIE_JSON = os.path.join(CACHE_DIR, "launchbox_indie.json")
INDIE_ROW = re.compile(r'^\s*\{ "((?:[^"\\]|\\.)*)", "(?:[^"\\]|\\.)*\.rar", \d+ULL,')
INDIE_KEY_TAG = 0xED000000
INDIE_OVERVIEW_MAX = 600  # shorter: there are three times as many

PLATFORMS = ("Microsoft Xbox", "Microsoft Xbox 360")

# Screenshots for a game page, as many as a 360 game's: gameplay ones, the
# English-speaking regions' first, then any.
SHOTS_MAX = 4
SHOT_REGIONS = {"": 0, "North America": 1, "United States": 1, "World": 1, "Europe": 2, "United Kingdom": 2}


def screenshots(g):
    """Up to SHOTS_MAX of a LaunchBox game's screenshots' file names, space-separated."""
    return " ".join(name for _, name in sorted(g["shots"], key=lambda s: s[0])[:SHOTS_MAX])


def indie_key(name):
    """IndieGameKey in CoverArt.cpp: FNV-1a of the name, ASCII lower-cased."""
    h = 2166136261
    for b in name.encode("utf-8"):
        if 65 <= b <= 90:
            b += 32
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return INDIE_KEY_TAG | (h & 0x00FFFFFF)


def indie_games():
    """[name] of the Store's indie games, each once."""
    names, seen = [], set()
    with open(XBLIG_TITLES, encoding="latin-1") as f:
        for line in f:
            m = INDIE_ROW.match(line)
            if m:
                name = c_name(m.group(1))
                if name not in seen:
                    seen.add(name)
                    names.append(name)
    return names


def norm(s):
    s = s.lower().replace("&", "and")
    s = re.sub(r"\(.*?\)|\[.*?\]", "", s)
    s = re.sub(r"^the |, the\b", "", s)
    return re.sub(r"[^a-z0-9]", "", s)


def c_name(raw):
    return raw.encode("latin-1").decode("unicode_escape").encode("latin-1").decode("utf-8", "replace")


def c_string(s):
    out = []
    for b in s.encode("utf-8"):
        if b in (0x22, 0x5C):
            out.append("\\" + chr(b))
        elif b == 0x0A:
            out.append("\\n")
        elif 0x20 <= b < 0x7F:
            out.append(chr(b))
        else:
            out.append("\\x%02X" % b)
    # A hex escape runs on through hex digits, so one is cut off from them.
    return '"' + re.sub(r'(\\x[0-9A-F]{2})(?=[0-9A-Fa-f])', r'\1""', "".join(out)) + '"'


def store_games():
    """[(name, title ID)] of the Store's Original Xbox games, with one."""
    games = []
    with open(XBOX_TITLES, encoding="latin-1") as f:
        for line in f:
            m = GAME.match(line)
            if m and m.group(2) != "00000000":
                games.append((c_name(m.group(1)), int(m.group(2), 16)))
    return games


def fetch(refresh):
    if os.path.exists(METADATA_ZIP) and not refresh:
        return
    os.makedirs(CACHE_DIR, exist_ok=True)
    print("fetching %s ..." % METADATA_URL)
    req = urllib.request.Request(METADATA_URL, headers={"User-Agent": "Omni360 make_xbox_info.py"})
    with urllib.request.urlopen(req, timeout=120) as r, open(METADATA_ZIP + ".part", "wb") as out:
        while True:
            block = r.read(1 << 20)
            if not block:
                break
            out.write(block)
    os.replace(METADATA_ZIP + ".part", METADATA_ZIP)


def launchbox_games():
    """LaunchBox's Original Xbox and Xbox 360 games, {platform: {normalised
    name: fields}} - by their own names, then their alternate names where
    those aren't taken."""
    games, alternates = {}, {}
    with zipfile.ZipFile(METADATA_ZIP) as z, z.open("Metadata.xml") as f:
        depth, root = 0, None
        for ev, el in ET.iterparse(f, events=("start", "end")):
            if ev == "start":
                root = root if root is not None else el
                depth += 1
                continue
            depth -= 1
            if depth != 1:
                continue  # only the root's children, whole
            if el.tag == "Game" and el.findtext("Platform") in PLATFORMS:
                g = {k: (el.findtext(k) or "").strip() for k in
                     ("Name", "Platform", "Genres", "Overview", "CommunityRating", "CommunityRatingCount",
                      "MaxPlayers", "Developer", "Publisher", "ReleaseYear", "ReleaseDate")}
                g["shots"] = []
                games[el.findtext("DatabaseID")] = g
            elif el.tag == "GameAlternateName":
                alternates.setdefault(el.findtext("DatabaseID"), []).append(el.findtext("AlternateName") or "")
            elif el.tag == "GameImage" and el.findtext("Type") == "Screenshot - Gameplay":
                # Games come before their images, so the game is known by now.
                g = games.get(el.findtext("DatabaseID"))
                if g is not None:
                    g["shots"].append((SHOT_REGIONS.get(el.findtext("Region") or "", 9), el.findtext("FileName")))
            root.clear()
    by_platform = {p: {} for p in PLATFORMS}
    for dbid, g in games.items():
        by_platform[g["Platform"]].setdefault(norm(g["Name"]), g)
    for dbid, names in alternates.items():
        if dbid in games:
            for a in names:
                by_platform[games[dbid]["Platform"]].setdefault(norm(a), games[dbid])
    return by_platform


def overview(text, most=OVERVIEW_MAX):
    """A description, tidied: paragraphs kept, runs of spaces and blank lines
    closed up, and cut at a sentence's end near `most` characters."""
    paras = [re.sub(r"\s+", " ", p).strip() for p in re.split(r"\n\s*\n|\r?\n", text)]
    text = "\n".join(p for p in paras if p)
    if len(text) <= most:
        return text
    cut = text[:most]
    end = max(cut.rfind(". "), cut.rfind(".\n"), cut.rfind("! "), cut.rfind("? "))
    return cut[:end + 1] if end > most // 2 else cut.rsplit(" ", 1)[0] + "..."


def details(g, most):
    """(genres as Xbox Live names them, ratings, stars, players, year,
    overview) of a LaunchBox game."""
    genres = []
    for lg in (x.strip() for x in g["Genres"].split(";")):
        if lg in GENRES and GENRES[lg] not in genres:
            genres.append(GENRES[lg])
    try:
        stars = float(g["CommunityRating"] or 0)
        ratings = int(g["CommunityRatingCount"] or 0)
    except ValueError:
        stars, ratings = 0.0, 0
    players = int(g["MaxPlayers"]) if g["MaxPlayers"].isdigit() else 0
    year = int(g["ReleaseYear"]) if g["ReleaseYear"].isdigit() else (
        int(g["ReleaseDate"][:4]) if g["ReleaseDate"][:4].isdigit() else 0)
    return genres, ratings, round(stars, 2), min(players, 255), year, overview(g["Overview"], most)


def write_header(path, guard, struct, key_name, key_note, count_name, data_name, table, intro, rows):
    """rows: (key, name shown, developer, publisher, players, year, overview, screenshots), sorted by key."""
    L = ["// GENERATED by tools/make_xbox_info.py - do not edit by hand.", "//"] + ["// " + x for x in intro] + [
         "",
         "#ifndef %s" % guard,
         "#define %s" % guard,
         "",
         "struct %s" % struct,
         "{",
         "    unsigned long %s; // %s" % (key_name, key_note),
         "    const char *developer; // UTF-8; may be empty",
         "    const char *publisher;",
         "    unsigned char players; // the most at once; 0 if LaunchBox doesn't say",
         "    unsigned short year;   // released; 0 if it doesn't say",
         "    const char *overview;  // UTF-8, paragraphs on lines of their own; may be empty",
         "    const char *screenshots; // up to %d file names under https://images.launchbox-app.com/, space-separated" % SHOTS_MAX,
         "};",
         "",
         "#define %s %d" % (count_name, len(rows)),
         "",
         "#ifdef %s" % data_name,
         "",
         "// Sorted by %s." % key_name,
         "static const %s %s[%s] =" % (struct, table, count_name),
         "{"]
    for key, name, dev, pub, players, year, text, shots in rows:
        L.append("    // %s" % name.encode("ascii", "replace").decode("ascii"))
        L.append("    { 0x%08X, %s, %s, %d, %d," % (key, c_string(dev), c_string(pub), players, year))
        L.append("      %s," % c_string(text))
        L.append("      %s }," % c_string(shots))
    L += ["};", "", "#endif // %s" % data_name, "", "#endif", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(L))


def main():
    refresh = "--refresh" in sys.argv
    fetch(refresh)
    lb = launchbox_games()
    os.makedirs(CACHE_DIR, exist_ok=True)

    # The Original Xbox's, by title ID.
    games = store_games()
    found, rows, missing = {}, [], []
    for name, tid in games:
        g = lb["Microsoft Xbox"].get(norm(name))
        if g is None:
            missing.append(name)
            continue
        if tid in found:
            continue  # regional copies share a title ID: the first, a USA or World one
        genres, ratings, stars, players, year, text = details(g, OVERVIEW_MAX)
        found[tid] = {"ratings": ratings, "stars": stars, "genres": genres}
        rows.append((tid, name, g["Developer"], g["Publisher"], players, year, text, screenshots(g)))
    rows.sort()
    json.dump({"%08X" % t: v for t, v in sorted(found.items())}, open(OUT_JSON, "w", encoding="utf-8"), indent=0)
    write_header(OUT_HEADER, "XBOX_INFO_H", "XboxInfo", "titleId", "the game's", "XBOX_INFO_COUNT",
                 "XBOX_INFO_DATA", "kXboxInfo",
                 ["The Original Xbox games' details for their Store pages - who made them,",
                  "how many play, the year, and a description - from the LaunchBox Games",
                  "Database (gamesdb.launchbox-app.com), matched by name: %d of the Store's" % len(rows),
                  "%d games with a title ID." % len(set(t for _, t in games))], rows)
    text_bytes = sum(len(r[6].encode("utf-8")) for r in rows)
    print("%d of %d Original Xbox games matched; %d with genres, %d rated, %d with a description (%d KB)"
          % (len(rows), len(games), sum(1 for v in found.values() if v["genres"]),
             sum(1 for v in found.values() if v["ratings"] > 0), sum(1 for r in rows if r[6]), text_bytes // 1024))
    print("  not matched:", ", ".join(missing[:20]), "..." if len(missing) > 20 else "")
    print("  %d with screenshots" % sum(1 for r in rows if r[7]))

    # Indie games, by IndieGameKey.
    names = indie_games()
    found, rows, missing = {}, [], []
    for name in names:
        g = lb["Microsoft Xbox 360"].get(norm(name))
        if g is None:
            missing.append(name)
            continue
        key = indie_key(name)
        if key in found:
            continue  # two names, one key: rare, and the first keeps it
        genres, ratings, stars, players, year, text = details(g, INDIE_OVERVIEW_MAX)
        found[key] = {"ratings": ratings, "stars": stars, "genres": genres}
        rows.append((key, name, g["Developer"], g["Publisher"], players, year, text, screenshots(g)))
    rows.sort()
    json.dump({"%08X" % t: v for t, v in sorted(found.items())}, open(INDIE_JSON, "w", encoding="utf-8"), indent=0)
    write_header(INDIE_HEADER, "INDIE_INFO_H", "IndieInfo", "key", "IndieGameKey(name) - CoverArt.h",
                 "INDIE_INFO_COUNT", "INDIE_INFO_DATA", "kIndieInfo",
                 ["The indie games' details for their Store pages - who made them, how many",
                  "play, the year, and a description - from the LaunchBox Games Database",
                  "(gamesdb.launchbox-app.com), matched by name: %d of the Store's %d." % (len(rows), len(names)),
                  "Indie games share one title ID, so each is keyed by its name's IndieGameKey."], rows)
    text_bytes = sum(len(r[6].encode("utf-8")) for r in rows)
    print("%d of %d indie games matched; %d with genres, %d with a description (%d KB)"
          % (len(rows), len(names), sum(1 for v in found.values() if v["genres"]),
             sum(1 for r in rows if r[6]), text_bytes // 1024))
    print("  %d with screenshots" % sum(1 for r in rows if r[7]))
    print("wrote %s, %s, %s and %s" % (OUT_HEADER, OUT_JSON, INDIE_HEADER, INDIE_JSON))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
