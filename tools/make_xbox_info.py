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
    """LaunchBox's Original Xbox games, {normalised name: fields} - by their
    own names, then their alternate names where those aren't taken."""
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
            if el.tag == "Game" and el.findtext("Platform") == "Microsoft Xbox":
                games[el.findtext("DatabaseID")] = {
                    k: (el.findtext(k) or "").strip() for k in
                    ("Name", "Genres", "Overview", "CommunityRating", "CommunityRatingCount", "MaxPlayers",
                     "Developer", "Publisher", "ReleaseYear", "ReleaseDate")}
            elif el.tag == "GameAlternateName":
                alternates.setdefault(el.findtext("DatabaseID"), []).append(el.findtext("AlternateName") or "")
            root.clear()
    by_name = {}
    for dbid, g in games.items():
        by_name.setdefault(norm(g["Name"]), g)
    for dbid, names in alternates.items():
        if dbid in games:
            for a in names:
                by_name.setdefault(norm(a), games[dbid])
    return by_name


def overview(text):
    """A description, tidied: paragraphs kept, runs of spaces and blank lines
    closed up, and cut at a sentence's end near OVERVIEW_MAX."""
    paras = [re.sub(r"\s+", " ", p).strip() for p in re.split(r"\n\s*\n|\r?\n", text)]
    text = "\n".join(p for p in paras if p)
    if len(text) <= OVERVIEW_MAX:
        return text
    cut = text[:OVERVIEW_MAX]
    end = max(cut.rfind(". "), cut.rfind(".\n"), cut.rfind("! "), cut.rfind("? "))
    return cut[:end + 1] if end > OVERVIEW_MAX // 2 else cut.rsplit(" ", 1)[0] + "..."


def main():
    refresh = "--refresh" in sys.argv
    fetch(refresh)
    lb = launchbox_games()
    games = store_games()

    found, info, missing = {}, [], []
    for name, tid in games:
        g = lb.get(norm(name))
        if g is None:
            missing.append(name)
            continue
        if tid in found:
            continue  # regional copies share a title ID: the first, a USA or World one
        genres = []
        for lg in (x.strip() for x in g["Genres"].split(";")):
            if lg in GENRES and GENRES[lg] not in genres:
                genres.append(GENRES[lg])
        try:
            stars = float(g["CommunityRating"] or 0)
            ratings = int(g["CommunityRatingCount"] or 0)
        except ValueError:
            stars, ratings = 0.0, 0
        found[tid] = {"ratings": ratings, "stars": round(stars, 2), "genres": genres}
        players = int(g["MaxPlayers"]) if g["MaxPlayers"].isdigit() else 0
        year = int(g["ReleaseYear"]) if g["ReleaseYear"].isdigit() else (
            int(g["ReleaseDate"][:4]) if g["ReleaseDate"][:4].isdigit() else 0)
        info.append((tid, name, g["Developer"], g["Publisher"], min(players, 255), year, overview(g["Overview"])))

    os.makedirs(CACHE_DIR, exist_ok=True)
    json.dump({"%08X" % t: v for t, v in sorted(found.items())}, open(OUT_JSON, "w", encoding="utf-8"), indent=0)

    info.sort()
    L = ["// GENERATED by tools/make_xbox_info.py - do not edit by hand.",
         "//",
         "// The Original Xbox games' details for their Store pages - who made them,",
         "// how many play, the year, and a description - from the LaunchBox Games",
         "// Database (gamesdb.launchbox-app.com), matched by name: %d of the Store's" % len(info),
         "// %d games with a title ID." % len(set(t for _, t in games)),
         "",
         "#ifndef XBOX_INFO_H",
         "#define XBOX_INFO_H",
         "",
         "struct XboxInfo",
         "{",
         "    unsigned long titleId;",
         "    const char *developer; // UTF-8; may be empty",
         "    const char *publisher;",
         "    unsigned char players; // the most at once; 0 if LaunchBox doesn't say",
         "    unsigned short year;   // released; 0 if it doesn't say",
         "    const char *overview;  // UTF-8, paragraphs on lines of their own; may be empty",
         "};",
         "",
         "#define XBOX_INFO_COUNT %d" % len(info),
         "",
         "#ifdef XBOX_INFO_DATA",
         "",
         "// Sorted by title ID.",
         "static const XboxInfo kXboxInfo[XBOX_INFO_COUNT] =",
         "{"]
    for tid, name, dev, pub, players, year, text in info:
        L.append("    // %s" % name.encode("ascii", "replace").decode("ascii"))
        L.append("    { 0x%08X, %s, %s, %d, %d," % (tid, c_string(dev), c_string(pub), players, year))
        L.append("      %s }," % c_string(text))
    L += ["};", "", "#endif // XBOX_INFO_DATA", "", "#endif", ""]
    with open(OUT_HEADER, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(L))

    text_bytes = sum(len(t[6].encode("utf-8")) for t in info)
    print("%d of %d Original Xbox games matched; %d with genres, %d rated, %d with a description (%d KB)"
          % (len(info), len(games), sum(1 for v in found.values() if v["genres"]),
             sum(1 for v in found.values() if v["ratings"] > 0), sum(1 for t in info if t[6]), text_bytes // 1024))
    print("not in LaunchBox by name:", ", ".join(missing[:40]), "..." if len(missing) > 40 else "")
    print("wrote %s and %s" % (OUT_HEADER, OUT_JSON))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
