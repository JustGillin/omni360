"""
Builds StoreTitles.h: every disc in archive.org's Redump Xbox 360 collection,
with its title ID, compiled into the XEX for the Store's A-Z lists - and the
games those discs make up, already gathered and sorted (group_games), so the
console only reads the tables.

    python tools/make_store_titles.py

Title IDs come from Redump's own datfile, fetched with its serials: a 360
disc's serial IS its title ID - "MW-2004" is publisher code "MW" (0x4D57)
and title 2004 (0x07D4), so 4D5707D4, Blitz: The League. A zip is matched
to the datfile by name:

  1. exactly, which covers about three in four;
  2. by the title before its first "(" and its disc number, when every
     version of it in the datfile has the same title ID;
  3. as 2, picking the version whose regions overlap the zip's most;
  4. against TitleNames.h, by the cleaned-up title alone.

The collection's zips were named from an older datfile, which is why the
later steps are needed at all: Redump has since merged and renamed entries
("Blitz - The League (USA)" is now "(USA, Europe)").

The Redump collection is missing some games - a few hundred of Redump's
discs were never uploaded. Some of those are in another archive.org set,
XBOX_360_2 to _6, as zips named "Naruto The Broken Bond [RF].zip". One of
those is added when it's a retail game the Redump collection doesn't have
at all, or a USA or region-free release of one it has only from elsewhere.
It's given a Redump-style name - "Naruto - The Broken Bond (World)" - for
the Store to show, and its title ID from the datfile. That set's RARs
aren't added: the installer reads zips only.

Needs network access: redump.org for the datfile and archive.org's public
metadata API for each letter's file list. The datfile isn't pinned - Redump
publishes only the latest - so the header records which one it used.
"""
import io
import json
import os
import re
import sys
import urllib.request
import zipfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_HEADER = os.path.join(ROOT, "src", "StoreTitles.h")
TITLE_NAMES = os.path.join(ROOT, "src", "TitleNames.h")

DAT_URL = "http://redump.org/datfile/xbox360/serial,version"
FILES_URL = "https://archive.org/metadata/%s/files"

# The letters, from https://r-roms.github.io/Microsoft/microsoft-xbox360.
# The _digital_ and _title items aren't discs, so aren't here.
ITEMS = ["numberssymbols", "a_part1", "a_part2", "b_part1", "b_part2", "c_part1", "c_part2",
         "d_part1", "d_part2", "d_part3", "e", "f_part1", "f_part2", "g", "h", "i", "j", "k", "l",
         "m_part1", "m_part2", "n_part1", "n_part2", "o", "p", "q", "r", "s_part1", "s_part2",
         "t_part1", "t_part2", "u", "v", "w", "x_part1", "x_part2", "y", "z"]

# The other set, for games the Redump collection lacks. XBOX_360_1 and
# _1_OTHER hold only RARs.
OTHER_ITEMS = ["XBOX_360_2", "XBOX_360_3", "XBOX_360_4", "XBOX_360_5", "XBOX_360_6"]

# Its names' region tags, as Redump's regions. "RF" is region free.
OTHER_REGIONS = {"RF": "World", "NTSCU": "USA", "PAL": "Europe", "NTSCJ": "Japan"}

# Region bits, from a name's first parenthesised group. Kept in step with
# STORE_REGION_* in StoreTitles.h's preamble below.
REGIONS = ["USA", "World", "Europe", "Japan", "Asia", "Australia", "Korea", "China", "Taiwan",
           "Russia", "Germany", "France", "Spain", "Italy", "UK", "Other"]

# Redump categories worth telling apart. Everything else is "other".
CATEGORIES = ["Games", "Demos", "Bonus Discs", "Applications", "Coverdiscs", "Add-Ons", "Preproduction"]

# Name tags that mark a disc as not the plain retail game.
FLAG_TAGS = [("Demo", 0x01), ("Beta", 0x02), ("Proto", 0x02), ("Kiosk", 0x01), ("Rev ", 0x04)]


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "Omni360 make_store_titles.py"})
    return urllib.request.urlopen(req, timeout=120).read()


def c_string(s):
    """A C string literal for UTF-8 text, as make_title_names.py does it."""
    out = []
    for b in s.encode("utf-8"):
        ch = chr(b)
        if ch in '"\\?':
            out.append("\\" + ch)
        elif 0x20 <= b < 0x7F:
            out.append(ch)
        else:
            out.append("\\%03o" % b)
    return '"' + "".join(out) + '"'


def serial_to_title_id(serial):
    m = re.match(r"^([A-Z0-9]{2})-(\d+)", serial or "")
    if not m or int(m.group(2)) > 0xFFFF:
        return 0
    code = m.group(1)
    return (ord(code[0]) << 24) | (ord(code[1]) << 16) | int(m.group(2))


def parse_name(name):
    """(title, regions, disc, groups) from a Redump name."""
    title = name.split(" (")[0]
    groups = re.findall(r"\(([^)]*)\)", name)
    regions = set(r.strip() for r in groups[0].split(",")) if groups else set()
    disc = next((g for g in groups if g.startswith("Disc ")), "")
    return title, regions, disc, groups


def display_title(title):
    """What the Store shows: "Elder Scrolls IV, The - Oblivion" becomes
    "The Elder Scrolls IV: Oblivion"."""
    head, sep, tail = title.partition(" - ")
    for article in (", The", ", A", ", An"):
        if head.endswith(article):
            head = article[2:] + " " + head[: -len(article)]
            break
    return head + (": " + tail if sep else "")


def sort_key(display):
    s = display.lower()
    for article in ("the ", "a ", "an "):
        if s.startswith(article):
            s = s[len(article):]
            break
    return re.sub(r"[^a-z0-9 ]", "", s).strip()  # letter_of takes the first character


def clean_for_lookup(title):
    s = display_title(title.split(" ~ ")[0]).lower()
    return re.sub(r"[^a-z0-9]", "", s)


def match_key(title):
    """A title boiled down for matching across the two sets' naming:
    "Naruto - The Broken Bond" and "Naruto The Broken Bond" are both
    "narutobrokenbond". Spaces don't count, as the two spell some names
    differently: "Eiyuu-tachi" and "Eiyuutachi"."""
    s = title.lower().replace("&", " and ").replace("'", "")
    s = re.sub(r"\bthe\b", " ", s)
    return re.sub(r"[^a-z0-9]+", "", s)


def letter_of(display):
    """Which A-Z tile a name is under: its sort key's first character, or
    '#' for a digit or nothing."""
    key = sort_key(display)
    c = key[:1]
    return c.upper() if "a" <= c <= "z" else "#"


def group_games(rows):
    """The Store's games: the discs of each letter, in the table's order,
    gathered into games. A disc is a version of a game listed before it with
    the same name shown, or with a title ID one of its versions has -
    regional releases often have their own, so the name is what mostly holds
    them together. USA and World versions go ahead of the rest, and the first
    of them names the game and gives it its title ID. Demos, betas and the
    like aren't games here; nor are Redump's other categories.

    Returns [(letter, name, title ID, regions, [disc indexes])]."""
    usa_or_world = (1 << REGIONS.index("USA")) | (1 << REGIONS.index("World"))
    games_cat = CATEGORIES.index("Games") + 1
    out = []
    letter_start = 0
    for i, row in enumerate(rows):
        display, flags, cat, tid, regions = row[12], row[6], row[7], row[8], row[5]
        if flags & 0x03 or cat not in (0, games_cat):
            continue
        letter = letter_of(display)
        if not out or out[-1][0] != letter:
            letter_start = len(out)

        game = None
        for g in reversed(out[letter_start:]):
            if g[1] == display or (tid and any(rows[v][8] == tid for v in g[4])):
                game = g
                break
        if game is None:
            game = [letter, display, tid, 0, []]
            out.append(game)

        usa = bool(regions & usa_or_world)
        versions = game[4]
        if usa and versions and not rows[versions[0]][5] & usa_or_world:
            game[1] = display
            if tid:
                game[2] = tid
        if not game[2]:
            game[2] = tid
        game[3] |= regions
        if usa:
            at = len(versions)
            while at > 0 and not rows[versions[at - 1]][5] & usa_or_world:
                at -= 1
            versions.insert(at, i)
        else:
            versions.append(i)
    return out


MIDDOT = "  ·  "  # STORE_MIDDOT in main.cpp
DISC_GROUP = re.compile(r"^(?:Disc|Disco|Disque|Dysk) (\d+)$")
LANGUAGES = re.compile(r"^[A-Z][a-z](,[A-Z][a-z])*$")


def disc_parts(name):
    """(title, groups before the disc number, disc number, groups after it)
    from a Redump name: "Halo 3 - ODST (USA) (En,Ja) (Disc 2) (Multiplayer)"
    is ("Halo 3 - ODST", ["USA", "En,Ja"], 2, ["Multiplayer"]). 0 and no
    groups after for a single disc."""
    if name.lower().endswith(".zip"):
        name = name[:-4]
    title = name.split(" (")[0]
    groups = re.findall(r"\(([^)]*)\)", name[len(title):])
    for i, g in enumerate(groups):
        m = DISC_GROUP.match(g)
        if m:
            return title, groups[:i], int(m.group(1)), groups[i + 1:]
    return title, groups, 0, []


def build_releases(rows, game):
    """A game's versions: its discs gathered into the sets that install
    together - a region's Disc 1 and Disc 2. Discs of one release share an
    edition, regions, languages and revision; what else follows the disc
    number ("Play Disc", "Multiplayer") is the disc's own.

    A release with its own Disc 1 borrows any disc it's missing from another
    of the same edition, regions in common first - ODST's German campaign disc
    goes with the World multiplayer disc. One without its own Disc 1 is left
    as it is, and only listed if no other release has its discs already:
    ODST's "USA, Brazil" multiplayer disc is part of the USA release, while
    Skyrim Legendary Edition's lone Disc 2 is listed as "Disc 2 only".
    Releases that come out the same are listed once.

    Returns [(label, detail, size, [disc indexes in disc order])], in the
    order of their own first disc in the game's versions: USA and World
    first."""
    display_name = game[1]
    order = {d: i for i, d in enumerate(game[4])}

    sets = {}  # key -> {disc number: [disc indexes]}
    for d in game[4]:
        title, before, number, after = disc_parts(rows[d][2])
        rev = tuple(g for g in before + after if g.startswith("Rev "))
        key = (title, tuple(g for g in before if not g.startswith("Rev ")), rev)
        sets.setdefault(key, {}).setdefault(number, []).append(d)

    # Two discs with one number in one set - Call of Duty: Ghosts' dubbed and
    # subtitled Japanese game discs - are two releases sharing the rest.
    releases = []  # [key, {number: disc}, its own numbers, its own first disc's place]
    for key, by_number in sets.items():
        variants = max(len(v) for v in by_number.values())
        for k in range(variants):
            discs = {n: v[min(k, len(v) - 1)] for n, v in by_number.items()}
            releases.append([key, discs, set(discs), min(order[d] for d in discs.values())])

    # The discs of each edition, by number, to borrow from.
    edition_discs = {}
    for key, discs, _, _ in releases:
        for n, d in discs.items():
            if n > 0:
                edition_discs.setdefault(key[0], {}).setdefault(n, set()).add(d)

    def popcount(x):
        return bin(x).count("1")

    usa = 1 << REGIONS.index("USA")
    world = 1 << REGIONS.index("World")
    whole = [r for r in releases if 0 in r[2] or 1 in r[2]]
    parts = [r for r in releases if not (0 in r[2] or 1 in r[2])]
    for key, discs, own, _ in whole:
        if 1 not in own:
            continue
        regions = rows[discs[1]][5]
        available = edition_discs.get(key[0], {})
        for n in range(2, max(available) + 1):
            if n in discs or n not in available:
                continue
            discs[n] = max(sorted(available[n]), key=lambda d: (popcount(rows[d][5] & regions),
                                                                bool(rows[d][5] & world),
                                                                bool(rows[d][5] & usa), -order[d]))
    used = set(d for r in whole for d in r[1].values())
    kept = whole + [r for r in parts if not set(r[1].values()) <= used]

    out = []
    seen = set()
    kept.sort(key=lambda r: r[3])
    for key, discs, own, _ in kept:
        numbers = sorted(discs)
        ids = tuple(discs[n] for n in numbers)
        if ids in seen:
            continue
        seen.add(ids)

        # Named from its own first disc, not one it borrowed.
        title, before, number, after = disc_parts(rows[discs[min(own)]][2])
        label = before[0] if before else ""
        if len(ids) > 1:
            label += "%s%d discs" % (MIDDOT if label else "", len(ids))
        elif number > 0:
            label += "%sDisc %d only" % (MIDDOT if label else "", number)

        detail = []
        edition = display_title(title)
        if edition != display_name:
            if edition.startswith(display_name + ": "):
                edition = edition[len(display_name) + 2:]
            detail.append(edition)
        extra = before[1:] + (after if len(ids) == 1 else [g for g in after if g.startswith("Rev ")])
        for g in extra:
            detail.append(", ".join(g.split(",")) if LANGUAGES.match(g) else g)

        size = sum(rows[d][9] for d in ids)
        out.append((label, MIDDOT.join(detail), size, list(ids)))
    return out


def match_keys(title):
    """match_key for each of a Redump title's names - "Resident Evil 5 ~
    Biohazard 5" is both."""
    parts = [p.strip() for p in title.split(" ~ ")]
    return set(match_key(p) for p in [title] + parts)


def parse_other_name(name):
    """(title, regions, disc number, other tags) from the other set's
    "Title [PAL][DVD1].zip". The other tags are mostly languages - "ENG-FR-MX"
    - and tell apart versions of the same region."""
    stem = name[:-4]
    tags = [t.strip() for t in re.findall(r"\[([^\]]*)\]", stem)]
    title = re.sub(r"\s*\[[^\]]*\]", "", stem).strip()
    regions = set()
    disc = 0
    other = []
    for t in tags:
        found = False
        for part in re.split(r"[-,]", t):
            part = part.strip().upper()
            if part in OTHER_REGIONS:
                regions.add(OTHER_REGIONS[part])
                found = True
        m = re.match(r"DVD\s*(\d+)$", t, re.I)
        if m:
            disc = int(m.group(1))
        elif not found and t.lower() != "kinect":
            other.append(t)
    return title, regions, disc, other


def load_title_names():
    names = {}
    text = open(TITLE_NAMES, encoding="utf-8").read()
    for tid, lit in re.findall(r'\{ 0x([0-9A-F]{8}), "((?:[^"\\]|\\.)*)" \}', text):
        raw = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), lit).replace('\\"', '"').replace("\\\\", "\\").replace("\\?", "?")
        name = raw.encode("latin-1", "replace").decode("utf-8", "replace")
        key = re.sub(r"[^a-z0-9]", "", name.replace(":", "").lower())
        names.setdefault(key, int(tid, 16))
    return names


def main():
    print("fetching the Redump datfile...")
    blob = fetch(DAT_URL)
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        dat_name = [n for n in z.namelist() if n.endswith(".dat")][0]
        dat = ET.fromstring(z.read(dat_name))
    version = dat.findtext("header/version") or "?"

    games = {}
    by_title = {}
    retail = {}  # match key -> [(title, regions, disc, title ID, category)], retail games only
    for g in dat.findall("game"):
        name = g.get("name")
        tids = [t for t in (serial_to_title_id(s.text) for s in g.findall("serial")) if t]
        entry = (tids[0] if tids else 0, g.findtext("category") or "")
        games[name] = entry
        title, regions, disc, groups = parse_name(name)
        by_title.setdefault((title.lower(), disc), []).append((regions, entry))
        not_retail = any(grp.startswith(tag) for grp in groups[1:] for tag, bit in FLAG_TAGS if bit != 0x04)
        if entry[1] == "Games" and not not_retail:
            disc_no = int(disc[5:]) if disc[5:].isdigit() else 0
            for key in match_keys(title):
                retail.setdefault(key, []).append((title, regions, disc_no, entry[0], entry[1]))

    title_names = load_title_names()

    rows = []
    have = {}      # match key -> the regions the Redump collection has it in
    have_tid = {}  # title ID -> likewise
    how = {"exact": 0, "same title": 0, "region": 0, "TitleNames.h": 0, "none": 0}
    unmatched = []
    for index, item in enumerate(ITEMS):
        ident = "microsoft_xbox360_" + item
        print("listing %s..." % ident)
        files = json.loads(fetch(FILES_URL % ident))["result"]
        for f in files:
            zn = f["name"]
            if not zn.lower().endswith(".zip") or "/" in zn:
                continue
            stem = zn[:-4]
            title, regions, disc, groups = parse_name(stem)

            tid, category = 0, ""
            if stem in games and games[stem][0]:
                (tid, category), method = games[stem], "exact"
            else:
                cands = by_title.get((title.lower(), disc), [])
                ids = set(e[0] for _, e in cands if e[0])
                if len(ids) == 1:
                    tid = ids.pop()
                    category = next(e[1] for _, e in cands if e[0] == tid)
                    method = "same title"
                else:
                    best = max(cands, key=lambda c: len(c[0] & regions), default=None)
                    if best is not None and best[1][0] and best[0] & regions:
                        (tid, category), method = best[1], "region"
                    else:
                        tid = title_names.get(clean_for_lookup(title), 0)
                        method = "TitleNames.h" if tid else "none"
                        if not tid:
                            unmatched.append(stem)
            how[method] += 1

            region_bits = 0
            for r in regions:
                bit = REGIONS.index(r) if r in REGIONS else REGIONS.index("Other")
                region_bits |= 1 << bit
            flags = 0
            for g in groups[1:]:
                for tag, bit in FLAG_TAGS:
                    if g.startswith(tag):
                        flags |= bit
            cat = CATEGORIES.index(category) + 1 if category in CATEGORIES else 0
            disc_no = disc_parts(stem)[2]  # "Disco 1" and "Disque 1" too

            display = display_title(title)
            rows.append((sort_key(display), 0 if regions & {"USA", "World"} else 1, stem,
                         index, disc_no, region_bits, flags, cat, tid, int(f.get("size", 0)), zn, None, display))
            for key in match_keys(title):
                have.setdefault(key, set()).update(regions)
            if tid:
                have_tid.setdefault(tid, set()).update(regions)

    total = sum(how.values())
    print("%d zips: %s" % (total, ", ".join("%s %d" % kv for kv in how.items())))

    # The other set, for what the Redump collection lacks.
    added = []
    for index, ident in enumerate(OTHER_ITEMS, len(ITEMS)):
        print("listing %s..." % ident)
        files = json.loads(fetch(FILES_URL % ident))["result"]
        for f in files:
            zn = f["name"]
            if not zn.lower().endswith(".zip") or "/" in zn or int(f.get("size", 0)) < 1000000000:
                continue  # the small ones are fan translations, not games
            title, regions, disc_no, other = parse_other_name(zn)
            key = match_key(title)
            versions = retail.get(key)
            if not versions or not regions:
                continue  # not a retail game Redump knows, or no region to go on
            # What the Redump collection has of it, by name or by any of its
            # versions' title IDs - "FIFA Soccer 10 (USA)" is FIFA 10.
            held = set(have.get(key, set()))
            for v in versions:
                held |= have_tid.get(v[3], set()) if v[3] else set()
            if held and (held & {"USA", "World"} or not regions & {"USA", "World"}):
                continue  # the Redump collection has it already

            # Its datfile entry: the same disc, with regions in common if any.
            # Region free is World in Redump, or USA where there's no World.
            wanted = regions | ({"USA"} if "World" in regions else set())
            same_disc = [v for v in versions if v[2] == disc_no] or versions
            best = max(same_disc, key=lambda v: (len(v[1] & wanted), v[3] != 0))
            redump_title, _, _, tid, category = best

            name = "%s (%s)" % (redump_title, ", ".join(r for r in REGIONS if r in regions))
            if disc_no:
                name += " (Disc %d)" % disc_no
            for t in other:
                name += " (%s)" % t.replace("(", "").replace(")", "")
            region_bits = 0
            for r in regions:
                region_bits |= 1 << REGIONS.index(r)
            cat = CATEGORIES.index(category) + 1 if category in CATEGORIES else 0
            display = display_title(redump_title)
            rows.append((sort_key(display), 0 if regions & {"USA", "World"} else 1, name,
                         index, disc_no, region_bits, 0, cat, tid, int(f.get("size", 0)), zn, name, display))
            added.append((name, ident, zn))
    print("%d zips added from the other set" % len(added))

    rows.sort()
    store_games = group_games(rows)
    releases = [build_releases(rows, g) for g in store_games]
    most_versions = max(len(r) for r in releases)
    most_discs = max(len(r[3]) for rs in releases for r in rs)
    most_in_letter = max(sum(1 for g in store_games if g[0] == c) for c in set(g[0] for g in store_games))
    print("%d games, %d versions; at most %d in a letter, %d versions of one, %d discs in one" %
          (len(store_games), sum(len(r) for r in releases), most_in_letter, most_versions, most_discs))

    L = []
    L.append("// GENERATED by tools/make_store_titles.py - do not edit by hand.")
    L.append("//")
    L.append("// The discs in archive.org's Redump Xbox 360 collection, and some from")
    L.append("// another set it lacks (see make_store_titles.py), sorted by the")
    L.append("// name the Store shows, USA and World versions of a game first. Title IDs")
    L.append("// are from Redump's datfile %s, by each disc's serial." % version)
    L.append("// %d zips; title IDs matched: %s." % (total, ", ".join("%s %d" % kv for kv in how.items())))
    L.append("// Then %d from XBOX_360_2 to _6, for games the Redump collection lacks." % len(added))
    L.append("")
    L.append("#ifndef STORE_TITLES_H")
    L.append("#define STORE_TITLES_H")
    L.append("")
    for i, r in enumerate(REGIONS):
        L.append("#define STORE_REGION_%-10s 0x%04X" % (re.sub(r"[^A-Z]", "", r.upper()), 1 << i))
    L.append("")
    L.append("#define STORE_FLAG_DEMO     0x01 // a demo or kiosk disc")
    L.append("#define STORE_FLAG_PREVIEW  0x02 // a beta or prototype")
    L.append("#define STORE_FLAG_REVISION 0x04 // a later pressing (\"Rev 1\")")
    L.append("")
    L.append("enum StoreCategory // Redump's")
    L.append("{")
    L.append("    STORE_CATEGORY_OTHER,")
    for c in CATEGORIES:
        L.append("    STORE_CATEGORY_%s," % re.sub(r"[^A-Z]", "_", c.upper()).strip("_"))
    L.append("};")
    L.append("")
    L.append("struct StoreDisc")
    L.append("{")
    L.append("    unsigned char item;      // into kStoreItems")
    L.append("    unsigned char disc;      // 1, 2... of a multi-disc game; 0 for a single disc")
    L.append("    unsigned short regions;  // STORE_REGION_*")
    L.append("    unsigned char flags;     // STORE_FLAG_*")
    L.append("    unsigned char category;  // StoreCategory")
    L.append("    unsigned long titleId;   // 0 if it couldn't be matched")
    L.append("    unsigned long long zipSize;")
    L.append("    const char *zip;         // the file in the item")
    L.append("};")
    L.append("")
    L.append("// One version of a game: the discs that install together - a region's")
    L.append("// Disc 1 and Disc 2 - as build_releases in make_store_titles.py describes.")
    L.append("struct StoreRelease")
    L.append("{")
    L.append("    const char *label;           // \"USA, Europe  \\xC2\\xB7  2 discs\", UTF-8")
    L.append("    const char *detail;          // its edition and languages; may be empty")
    L.append("    unsigned long long size;     // every disc's zip")
    L.append("    unsigned short firstDisc;    // into kStoreReleaseDiscs")
    L.append("    unsigned char discCount;")
    L.append("};")
    L.append("")
    L.append("// The Store's games: the discs gathered up, as group_games in")
    L.append("// make_store_titles.py describes, in the order shown.")
    L.append("struct StoreGame")
    L.append("{")
    L.append("    const char *name;            // the name shown, UTF-8")
    L.append("    unsigned long titleId;       // a USA or World version's where one has it; 0 if none matched")
    L.append("    unsigned short regions;      // every version's, STORE_REGION_*")
    L.append("    unsigned short firstVersion; // into kStoreReleases")
    L.append("    unsigned char versionCount;")
    L.append("    char letter;                 // its A-Z tile: 'A' to 'Z', or '#'")
    L.append("};")
    L.append("")
    L.append("#define STORE_GAME_COUNT       %d" % len(store_games))
    L.append("#define STORE_MAX_LETTER_GAMES %d // the most under one letter" % most_in_letter)
    L.append("#define STORE_MAX_VERSIONS     %d // the most versions of one game" % most_versions)
    L.append("#define STORE_MAX_DISCS        %d // the most discs in one version" % most_discs)
    L.append("")
    L.append("// The data, for StoreCatalog.cpp alone - it's a few hundred KB.")
    L.append("#ifdef STORE_TITLES_DATA")
    L.append("")
    L.append("static const char *const kStoreItems[] =")
    L.append("{")
    for item in ITEMS:
        L.append('    "microsoft_xbox360_%s",' % item)
    for item in OTHER_ITEMS:
        L.append('    "%s",' % item)
    L.append("};")
    L.append("")
    L.append("static const StoreDisc kStoreDiscs[] =")
    L.append("{")
    for _, _, _, item, disc_no, region_bits, flags, cat, tid, size, zn, name, _ in rows:
        L.append("    { %d, %d, 0x%04X, 0x%02X, %d, 0x%08X, %dULL, %s }," %
                 (item, disc_no, region_bits, flags, cat, tid, size, c_string(zn)))
    L.append("};")
    L.append("")
    L.append("// Each version's discs, into kStoreDiscs, in disc order.")
    L.append("static const unsigned short kStoreReleaseDiscs[] =")
    L.append("{")
    disc_at = 0
    release_rows = []
    game_first = []
    for rs in releases:
        game_first.append(len(release_rows))
        for label, detail, size, ids in rs:
            release_rows.append((label, detail, size, disc_at, len(ids)))
            disc_at += len(ids)
            L.append("    %s," % ", ".join(str(d) for d in ids))
    L.append("};")
    L.append("")
    L.append("// Each game's versions, USA and World first.")
    L.append("static const StoreRelease kStoreReleases[] =")
    L.append("{")
    for label, detail, size, at, count in release_rows:
        L.append("    { %s, %s, %dULL, %d, %d }," % (c_string(label), c_string(detail), size, at, count))
    L.append("};")
    L.append("")
    L.append("static const StoreGame kStoreGames[] =")
    L.append("{")
    for g, at, rs in zip(store_games, game_first, releases):
        letter, display, tid, regions, _ = g
        L.append("    { %s, 0x%08X, 0x%04X, %d, %d, '%s' }," % (c_string(display), tid, regions, at, len(rs), letter))
    L.append("};")
    L.append("")
    L.append("#endif // STORE_TITLES_DATA")
    L.append("")
    L.append("#endif // STORE_TITLES_H")
    L.append("")

    with open(OUT_HEADER, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(L))
    print("wrote %s (%d discs)" % (OUT_HEADER, len(rows)))
    for name, ident, zn in sorted(added):
        print("   + %s  <-  %s/%s" % (name, ident, zn))
    if unmatched:
        print("no title ID for %d:" % len(unmatched))
        for u in unmatched:
            print("   ", u)


if __name__ == "__main__":
    sys.exit(main())
