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
the Store to show, and its title ID from the datfile.

XBOX_360_1 and XBOX_360_1_OTHER hold the same kind of games as RARs -
BioShock Infinite's only copy is one. A RAR is added if its disc image isn't
encrypted or split. One kept without compression is read straight out of the
download; a compressed one is unpacked on the console as it downloads, which
is slow - about 3 MB/s - so it's flagged STORE_FLAG_UNPACKS for the Store to
say so before it starts. Finding that out means reading its
headers, which are private, so it takes the archive.org keys (--keys, the
two-line ArchiveOrgKeys.txt the app uses); what each RAR held is kept in
tools/cache/store_rars.json, so a rerun asks only about new ones. Without
the keys, RARs not in that cache are left out.

Needs network access: redump.org for the datfile and archive.org's public
metadata API for each letter's file list. The datfile isn't pinned - Redump
publishes only the latest - so the header records which one it used.

    python tools/make_store_titles.py --keys path/to/ArchiveOrgKeys.txt

    python tools/make_store_titles.py --system xbox

builds XboxTitles.h the same way, for the Store's Original Xbox A-Z: the
Redump Original Xbox collection, microsoft_xbox_*, whose serials give title
IDs just as the 360's do ("MS-004" is 4D530004, Halo). Only the games the
360 can run are kept - those on Microsoft's backwards compatibility list,
tools/data/xbox_backcompat.txt - and there's no other set to fill gaps from.
"""
import argparse
import io
import json
import os
import re
import sys
import urllib.request
import zipfile
import xml.etree.ElementTree as ET

import make_xbla_titles as xbla  # its keyed fetch and RAR header reading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_HEADER = os.path.join(ROOT, "src", "StoreTitles.h")
TITLE_NAMES = os.path.join(ROOT, "src", "TitleNames.h")

DAT_URL = "http://redump.org/datfile/xbox360/serial,version"
FILES_URL = "https://archive.org/metadata/%s/files"

# The Original Xbox: its datfile, items and output, for --system xbox.
XBOX_DAT_URL = "http://redump.org/datfile/xbox/serial,version"
XBOX_OUT_HEADER = os.path.join(ROOT, "src", "XboxTitles.h")
XBOX_BACKCOMPAT = os.path.join(ROOT, "tools", "data", "xbox_backcompat.txt")
# What to call a game whose entry its discs name wrongly - see hand_names.
NAMES = os.path.join(ROOT, "tools", "data", "store-names.txt")
# Discs whose title ID no rule here finds - see hand_ids.
IDS = os.path.join(ROOT, "tools", "data", "store-ids.txt")
GROUPS = os.path.join(ROOT, "tools", "data", "store-groups.txt")
# From https://r-roms.github.io/Microsoft/microsoft-xbox.
XBOX_ITEMS = ["numberssymbols", "a", "b", "c_part1", "c_part2", "d_part1", "d_part2", "e", "f", "g", "h",
              "i", "j", "k", "l", "m_part1", "m_part2", "n_part1", "n_part2", "o_part1", "o_part2", "p", "q",
              "r", "s_part1", "s_part2", "t_part1", "t_part2", "u", "v", "w", "x", "y", "z"]

# The letters, from https://r-roms.github.io/Microsoft/microsoft-xbox360.
# The _digital_ and _title items aren't discs, so aren't here.
ITEMS = ["numberssymbols", "a_part1", "a_part2", "b_part1", "b_part2", "c_part1", "c_part2",
         "d_part1", "d_part2", "d_part3", "e", "f_part1", "f_part2", "g", "h", "i", "j", "k", "l",
         "m_part1", "m_part2", "n_part1", "n_part2", "o", "p", "q", "r", "s_part1", "s_part2",
         "t_part1", "t_part2", "u", "v", "w", "x_part1", "x_part2", "y", "z"]

# The other set, for games the Redump collection lacks: zips in XBOX_360_2 to
# _6, then RARs in XBOX_360_1 and _1_OTHER - a game the zips have isn't taken
# again from the RARs.
OTHER_ITEMS = ["XBOX_360_2", "XBOX_360_3", "XBOX_360_4", "XBOX_360_5", "XBOX_360_6",
               "XBOX_360_1", "XBOX_360_1_OTHER"]
RAR_CACHE = os.path.join(ROOT, "tools", "cache", "store_rars.json")

# Folder games: RARs of a game's extracted files, from the ZTM sets. Only the
# ones named in store-ids.txt - see hand_ids.
FOLDER_ITEMS = ["mx360gcxpt1-x360-ztm", "mx360gcpt2-x360-ztm", "mx360gcpt3-x360-ztm", "mx360gcpt4-x360-ztm",
                "mx360gcpt5-x360-ztm", "mx360gcpt6-x360-ztm"]
# Their names' region tags: "Crysis.1.USA.X360-ZTM.rar".
FOLDER_REGIONS = {"USA": "USA", "EUR": "Europe", "JPN": "Japan", "JAP": "Japan", "GER": "Germany",
                  "FRA": "France", "SPA": "Spain", "ITA": "Italy", "ASIA": "Asia", "KOR": "Korea"}

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


# Discs Redump gives no category that still aren't games - set-up and
# software discs, magazine coverdiscs, add-on content discs and the Xbox Live
# Arcade compilation discs - by file name, which keeps the bracketed parts
# ("Mafia II (Russia) (Add-On Content Disc)"). Tagged ones are left out by category.
NOT_GAMES = re.compile(r"Experience Disc|HD DVD Player|Software Disc|Magazine|Add-On|^Xbox Live Arcade \(")


# A special edition of a game: Game of the Year, Ultimate, Scholarship and
# the like. It's grouped under the game itself when the Store has it, and
# shown as the edition on its own versions.
EDITION = re.compile(r"(?: - |: )(?!.*(?: - |: ))[^-:]*(?:Edition|Director's Cut|Edizione|Edicion|Jubilaeumsausgabe|Gioco dell'Anno)[^-:]*$")


def base_name(display):
    """The game a special edition is of - itself when it isn't one."""
    return EDITION.sub("", display) or display


def hand_groups(path):
    """store-groups.txt: {disc's zip without its extension: title ID of the
    game it belongs to}."""
    out = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                zip_stem, _, tid = line.rpartition(" = ")
                out[zip_stem.strip()] = int(tid, 16)
    return out


def group_games(rows, joins=None):
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
    joined = []  # (disc index, title ID of its game) - see store-groups.txt
    for i, row in enumerate(rows):
        display, flags, cat, tid, regions = row[12], row[6], row[7], row[8], row[5]
        if flags & 0x03 or cat not in (0, games_cat):
            continue
        stem = re.sub(r"\.(zip|rar)$", "", row[10].split("/")[-1])
        if joins and stem in joins:
            joined.append((i, joins.pop(stem)))
            continue
        if cat == 0 and NOT_GAMES.search(row[10]):
            continue
        letter = letter_of(display)
        if not out or out[-1][0] != letter:
            letter_start = len(out)

        game = None
        edition = False  # joining the game as a special edition of it
        for g in reversed(out[letter_start:]):
            if g[1] == display or (tid and any(rows[v][8] == tid for v in g[4])):
                game = g
                break
            if base_name(g[1]) == base_name(display):
                game = g
                edition = True
                break
        if game is None:
            game = [letter, display, tid, 0, []]
            out.append(game)

        usa = bool(regions & usa_or_world)
        versions = game[4]
        if usa and versions and not rows[versions[0]][5] & usa_or_world and not edition:
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

    for i, target in joined:
        game = next((g for g in out if g[2] == target or any(rows[v][8] == target for v in g[4])), None)
        if game is None:
            print("store-groups.txt: no game has title ID %08X for %s" % (target, rows[i][10]))
            continue
        game[4].append(i)
        game[3] |= rows[i][5]
    for stem in joins or {}:
        print("store-groups.txt: no disc is named \"%s\"" % stem)
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
    stem = re.sub(r"\.iso$", "", name[:-4], flags=re.I)  # "Bulletstorm [RF].ISO.rar"
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


def is_disc_image(name, size):
    """The disc image among a RAR's files: an .iso, or anything a gigabyte or
    more. Some RARs hold a 30-byte "DVD1.dvd" layer-break note before it."""
    return name.lower().endswith(".iso") or size >= 1000000000


def rar_disc_image(reader):
    """What a RAR's disc image is: {format, name, size, stored, encrypted,
    split} - the same reading as RarHeaders.cpp, which the installer uses."""
    sig = reader.read(0, 8)
    if sig[:7] == b"Rar!\x1a\x07\x00":
        offset = 7
        for _ in range(32):
            h = reader.read(offset, 7)
            if len(h) < 7:
                break
            _, kind, flags, size = xbla.struct.unpack("<HBHH", h)
            if kind == 0x7B or size < 7:
                break
            if kind == 0x73 and flags & 0x80:
                return {"format": "rar4", "encrypted": True}
            head = reader.read(offset, size)
            if kind == 0x74:
                pack, unp = xbla.struct.unpack("<II", head[7:15])
                name_len = xbla.struct.unpack("<H", head[26:28])[0]
                name_at = 32
                if flags & 0x100:
                    hp, hu = xbla.struct.unpack("<II", head[32:40])
                    pack |= hp << 32
                    unp |= hu << 32
                    name_at = 40
                name = head[name_at:name_at + name_len].split(b"\x00")[0].decode("utf-8", "replace")
                if (flags & 0xE0) == 0xE0 or not is_disc_image(name, unp):
                    offset += size + pack
                    continue
                return {"format": "rar4", "name": name,
                        "size": unp, "stored": head[25] == 0x30 and pack == unp,
                        "encrypted": bool(flags & 0x04), "split": bool(flags & 0x03)}
            add = xbla.struct.unpack("<I", head[7:11])[0] if flags & 0x8000 else 0
            offset += size + add
        raise ValueError("no disc image in its first headers")
    if sig[:8] == b"Rar!\x1a\x07\x01\x00":
        offset = 8
        for _ in range(32):
            h = reader.read(offset, 16)
            size, i = xbla.vint(h, 4)
            head = reader.read(offset, i + size)
            end = i + size
            kind, j = xbla.vint(head, i)
            flags, j = xbla.vint(head, j)
            extra = data = 0
            if flags & 1:
                extra, j = xbla.vint(head, j)
            if flags & 2:
                data, j = xbla.vint(head, j)
            if kind == 5:
                break
            if kind == 4:
                return {"format": "rar5", "encrypted": True}
            if kind == 2:
                file_flags, j = xbla.vint(head, j)
                unp, j = xbla.vint(head, j)
                _, j = xbla.vint(head, j)
                j += (4 if file_flags & 2 else 0) + (4 if file_flags & 4 else 0)
                compression, j = xbla.vint(head, j)
                _, j = xbla.vint(head, j)
                name_len, j = xbla.vint(head, j)
                name = head[j:j + name_len].decode("utf-8", "replace")
                if file_flags & 1 or not is_disc_image(name, unp):
                    offset += end + data
                    continue
                encrypted = False
                k = end - extra
                while k < end:  # extra records: a size, a type; type 1 is encryption
                    rsize, body = xbla.vint(head, k)
                    rtype, _ = xbla.vint(head, body)
                    encrypted |= (rtype == 1)
                    if rsize == 0:
                        break
                    k = body + rsize
                return {"format": "rar5", "name": name, "size": unp,
                        "stored": ((compression >> 7) & 7) == 0 and data == unp,
                        "encrypted": encrypted, "split": bool(flags & 0x18)}
            offset += end + data
        raise ValueError("no disc image in its first headers")
    raise ValueError("not a RAR (starts %r)" % sig[:8])


# Edition words the other set adds to a name that Redump keeps apart, or
# leaves out: "Bioshock Infinite Complete Edition" is Redump's "BioShock
# Infinite". Tried, stripped, when the whole name matches nothing.
EDITION_WORDS = re.compile(r"\b(complete edition|game of the year edition|game of the year|goty|ultimate edition|"
                           r"limited edition|collectors edition|collector's edition|platinum hits|classics)\b", re.I)


NAME_LINE = re.compile(r"^([0-9A-Fa-f]{8})\s*=\s*(.+?)\s*$")


def hand_names(path):
    """The names from tools/data/store-names.txt: title ID -> what the Store
    should call that game instead.

    A Store entry is one title ID, named by group_games after whichever of its
    discs sorts first. That is right until one title ID is two games: 545407F2
    is Grand Theft Auto IV's, and Episodes from Liberty City shares it, so
    "Episodes" sorting before "IV" names the entry after the expansion. No
    rule gets this right by itself - the catalog's own name for that ID is the
    abbreviation "GTA IV" - so it is a list kept by hand.

    A missing file is not an error: this is optional, and a clone without one
    should still generate."""
    names = {}
    if not os.path.exists(path):
        return names
    with io.open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = NAME_LINE.match(line)
            if m:
                names[int(m.group(1), 16)] = m.group(2)
    return names


ID_LINE = re.compile(r"^(.+?)\s+=\s+([0-9A-Fa-f]{8})(?:\s+(.+?))?\s*$")


def hand_ids(path):
    """The matches from tools/data/store-ids.txt, for the discs whose names
    defeat every rule: a zip Redump's datfile no longer has by that name, and
    a disc of the other set spelled unlike the Redump title - "King of
    Fighters 13" for "XIII" - or one Redump has no retail entry for at all.

    Also every folder game - a ZTM RAR - the Store offers: those are taken
    from here alone.

    Returns {zip stem: title ID} for the Redump collection's,
    {"item/file": (title ID, Redump-style title)} for the other set's, and
    the same for the folder games. A missing file is not an error, as with
    hand_names."""
    zips, others, folders = {}, {}, {}
    if not os.path.exists(path):
        return zips, others, folders
    with io.open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = ID_LINE.match(line)
            if not m:
                print("store-ids.txt: can't read \"%s\"" % line)
                continue
            name, tid, title = m.group(1), int(m.group(2), 16), m.group(3)
            if title and any(name.startswith(item + "/") for item in OTHER_ITEMS):
                others[name] = (tid, title)
            elif title and any(name.startswith(item + "/") for item in FOLDER_ITEMS):
                folders[name] = (tid, title)
            else:
                zips[name] = tid
    return zips, others, folders


def rename_games(store_games, names):
    """Applies the hand-kept names, in place: a game keeps the slot its discs
    put it in. The list isn't sorted by the names it ends up with anyway - a
    game sits where its first disc sorted, and that disc can be a localized
    one the USA disc then renames the game over - so re-sorting it here would
    shuffle a hundred games that have nothing to do with this file.

    Returns what was renamed, the title IDs no game has, and the ones whose
    new name lands out of order among their neighbours - worth saying, since
    nothing else will notice.

    [(was, now)], [title ID], [(name, neighbour)]"""
    renamed, misplaced = [], []
    for i, g in enumerate(store_games):
        new = names.get(g[2])
        if new is None or new == g[1]:
            continue
        renamed.append((g[1], new))
        g[1] = new
        g[0] = letter_of(new)

        for j, other in ((i - 1, store_games[i - 1]) if i > 0 else (None, None), \
                         (i + 1, store_games[i + 1]) if i + 1 < len(store_games) else (None, None)):
            if other is None or other[0] != g[0]:
                continue
            before, after = (other, g) if j < i else (g, other)
            if sort_key(before[1]) > sort_key(after[1]):
                misplaced.append((new, other[1]))

    found = set(g[2] for g in store_games)
    return renamed, sorted(t for t in names if t not in found), misplaced


def load_backcompat():
    """The title IDs on the 360's backwards compatibility list."""
    ids = set()
    for line in open(XBOX_BACKCOMPAT, encoding="utf-8"):
        if line.strip() and not line.startswith("#"):
            ids.add(int(line.split("\t")[0], 16))
    return ids


def load_title_names():
    names = {}
    text = open(TITLE_NAMES, encoding="utf-8").read()
    for tid, lit in re.findall(r'\{ 0x([0-9A-F]{8}), "((?:[^"\\]|\\.)*)" \}', text):
        raw = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m.group(1), 8)), lit).replace('\\"', '"').replace("\\\\", "\\").replace("\\?", "?")
        name = raw.encode("latin-1", "replace").decode("utf-8", "replace")
        key = re.sub(r"[^a-z0-9]", "", name.replace(":", "").lower())
        names.setdefault(key, int(tid, 16))
    return names


def table_rows(rows, releases):
    """kXxxReleaseDiscs' and kXxxReleases' rows, and each game's first release."""
    disc_rows, release_rows, game_first = [], [], []
    disc_at = 0
    for rs in releases:
        game_first.append(len(release_rows))
        for label, detail, size, ids in rs:
            release_rows.append((label, detail, size, disc_at, len(ids)))
            disc_at += len(ids)
            disc_rows.append("    %s," % ", ".join(str(d) for d in ids))
    return disc_rows, release_rows, game_first


def xbox_header(version, total, how, not_compatible, rows, store_games, releases,
                most_in_letter, most_versions, most_discs):
    L = []
    L.append("// GENERATED by tools/make_store_titles.py --system xbox - do not edit by hand.")
    L.append("//")
    L.append("// The Original Xbox discs in archive.org's Redump collection that the 360")
    L.append("// can run - those on its backwards compatibility list - sorted by the name")
    L.append("// the Store shows, USA and World versions of a game first. Title IDs are")
    L.append("// from Redump's datfile %s, by each disc's serial." % version)
    L.append("// %d zips; title IDs matched: %s; %d not on the list." %
             (total, ", ".join("%s %d" % kv for kv in how.items()), not_compatible))
    L.append("")
    L.append("#ifndef XBOX_TITLES_H")
    L.append("#define XBOX_TITLES_H")
    L.append("")
    L.append('#include "StoreTitles.h" // StoreDisc, StoreRelease, StoreGame - the same shapes as the 360\'s')
    L.append("")
    L.append("#define XBOX_GAME_COUNT       %d" % len(store_games))
    L.append("#define XBOX_MAX_LETTER_GAMES %d // the most under one letter" % most_in_letter)
    L.append("#define XBOX_MAX_VERSIONS     %d // the most versions of one game" % most_versions)
    L.append("#define XBOX_MAX_DISCS        %d // the most discs in one version" % most_discs)
    L.append("")
    L.append("#ifdef XBOX_TITLES_DATA")
    L.append("")
    L.append("static const char *const kXboxItems[] =")
    L.append("{")
    for item in XBOX_ITEMS:
        L.append('    "microsoft_xbox_%s",' % item)
    L.append("};")
    L.append("")
    L.append("static const StoreDisc kXboxDiscs[] =")
    L.append("{")
    for _, _, _, item, disc_no, region_bits, flags, cat, tid, size, zn, name, _ in rows:
        L.append("    { %d, %d, 0x%04X, 0x%02X, %d, 0x%08X, %dULL, %s }," %
                 (item, disc_no, region_bits, flags, cat, tid, size, c_string(zn)))
    L.append("};")
    L.append("")
    disc_rows, release_rows, game_first = table_rows(rows, releases)
    L.append("static const unsigned short kXboxReleaseDiscs[] =")
    L.append("{")
    L.extend(disc_rows)
    L.append("};")
    L.append("")
    L.append("static const StoreRelease kXboxReleases[] =")
    L.append("{")
    for label, detail, size, at, count in release_rows:
        L.append("    { %s, %s, %dULL, %d, %d }," % (c_string(label), c_string(detail), size, at, count))
    L.append("};")
    L.append("")
    L.append("static const StoreGame kXboxGames[] =")
    L.append("{")
    for g, at, rs in zip(store_games, game_first, releases):
        letter, display, tid, regions, _ = g
        L.append("    { %s, 0x%08X, 0x%04X, %d, %d, '%s', STORE_SYSTEM_XBOX }," %
                 (c_string(display), tid, regions, at, len(rs), letter))
    L.append("};")
    L.append("")
    L.append("#endif // XBOX_TITLES_DATA")
    L.append("")
    L.append("#endif // XBOX_TITLES_H")
    L.append("")
    return L


def main():
    parser = argparse.ArgumentParser(description="Build StoreTitles.h, or XboxTitles.h with --system xbox.")
    parser.add_argument("--system", choices=["xbox360", "xbox"], default="xbox360")
    parser.add_argument("--keys", default=xbla.DEFAULT_KEYS,
                        help="ArchiveOrgKeys.txt, for reading the RARs' headers (default: %(default)s)")
    args = parser.parse_args()
    xbox = (args.system == "xbox")
    items = XBOX_ITEMS if xbox else ITEMS
    item_prefix = "microsoft_xbox_" if xbox else "microsoft_xbox360_"
    other_items = [] if xbox else OTHER_ITEMS
    backcompat = load_backcompat() if xbox else None

    print("fetching the Redump datfile...")
    blob = fetch(XBOX_DAT_URL if xbox else DAT_URL)
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

    title_names = {} if xbox else load_title_names()  # 360 titles only
    hand_zips, hand_others, hand_folders = ({}, {}, {}) if xbox else hand_ids(IDS)
    hand_used = set()

    rows = []
    have = {}      # match key -> the regions the Redump collection has it in
    have_tid = {}  # title ID -> likewise
    how = {"exact": 0, "same title": 0, "region": 0, "TitleNames.h": 0, "by hand": 0, "none": 0}
    unmatched = []
    not_compatible = 0
    for index, item in enumerate(items):
        ident = item_prefix + item
        print("listing %s..." % ident)
        files = json.loads(fetch(FILES_URL % ident))["result"]
        for f in files:
            zn = f["name"]
            if not zn.lower().endswith(".zip") or "/" in zn:
                continue
            stem = zn[:-4]
            title, regions, disc, groups = parse_name(stem)

            tid, category = 0, ""
            if stem in hand_zips:
                tid, method = hand_zips[stem], "by hand"
                category = games[stem][1] if stem in games else "Games"
                hand_used.add(stem)
            elif stem in games and games[stem][0]:
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

            # The Original Xbox games the 360 can't run aren't listed: a
            # 6.5GB download that won't start.
            if xbox and tid not in backcompat:
                not_compatible += 1
                continue

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
            # What the other set is checked against. A demo or a beta of a
            # game isn't the game: the collection has only a demo of MX vs.
            # ATV Untamed, and that used to keep the real disc out.
            if not flags & 0x03 and category in ("", "Games"):
                for key in match_keys(title):
                    have.setdefault(key, set()).update(regions)
                if tid:
                    have_tid.setdefault(tid, set()).update(regions)

    total = sum(how.values())
    print("%d zips: %s" % (total, ", ".join("%s %d" % kv for kv in how.items())))
    if xbox:
        print("%d left out: not on the backwards compatibility list" % not_compatible)

    # The other set, for what the Redump collection lacks.
    added = []
    taken = set()  # (match key, disc) from an earlier item of the other set
    rar_cache = json.load(open(RAR_CACHE, encoding="utf-8")) if os.path.exists(RAR_CACHE) else {}
    auth = xbla.load_keys(args.keys) if os.path.exists(args.keys) else None
    if auth is None and not xbox:
        print("no keys at %s - only RARs already in %s are considered" % (args.keys, RAR_CACHE))
    rar_skipped = []
    for index, ident in enumerate(other_items, len(items)):
        print("listing %s..." % ident)
        meta = json.loads(fetch("https://archive.org/metadata/%s" % ident))
        files = meta["files"]
        for f in files:
            zn = f["name"]
            is_rar = zn.lower().endswith(".rar")
            if not (zn.lower().endswith(".zip") or is_rar) or "/" in zn or int(f.get("size", 0)) < 1000000000:
                continue  # the small ones are fan translations, not games
            title, regions, disc_no, other = parse_other_name(zn)
            key = match_key(title)
            by_hand = hand_others.get("%s/%s" % (ident, zn))
            if by_hand:
                hand_used.add("%s/%s" % (ident, zn))
                key = match_key(by_hand[1])
                versions = [(by_hand[1], regions, disc_no, by_hand[0], "Games")]
            else:
                versions = retail.get(key)
            if not versions:
                stripped = EDITION_WORDS.sub(" ", title)
                if stripped != title:
                    key = match_key(stripped)
                    versions = retail.get(key)
            if not versions or not regions:
                continue  # not a retail game Redump knows, or no region to go on
            # A one-disc game's "DVD1" is the game; a "DVD2" of it is
            # something else - a bonus disc - and isn't a disc of the game.
            if disc_no and not by_hand and all(v[2] == 0 for v in versions):
                if disc_no > 1:
                    continue
                disc_no = 0
            # What the Redump collection has of it, by name or by any of its
            # versions' title IDs - "FIFA Soccer 10 (USA)" is FIFA 10.
            held = set(have.get(key, set()))
            for v in versions:
                held |= have_tid.get(v[3], set()) if v[3] else set()
            if not by_hand and held and (held & {"USA", "World"} or not regions & {"USA", "World"}):
                continue  # the Redump collection has it already
            if (key, disc_no) in taken and is_rar:
                continue  # a zip of it was taken already

            # A RAR only if the disc image is in it as it is.
            if is_rar:
                cache_key = "%s/%s" % (ident, zn)
                size = int(f.get("size", 0))
                entry = rar_cache.get(cache_key)
                # Asked again if it was read before RARs' notes were skipped.
                stale = entry is not None and "error" not in entry and not entry.get("encrypted") and \
                    not is_disc_image(entry.get("name", ""), entry.get("size", 0))
                if entry is None or entry.get("bytes") != size or stale:
                    if auth is None:
                        rar_skipped.append((zn, "not checked - no keys"))
                        continue
                    url = "https://%s%s/%s" % (meta["server"], meta["dir"], urllib.parse.quote(zn))
                    try:
                        entry = rar_disc_image(xbla.RarReader(url, auth))
                    except Exception as e:
                        entry = {"error": str(e)}
                    entry["bytes"] = size
                    rar_cache[cache_key] = entry
                    os.makedirs(os.path.dirname(RAR_CACHE), exist_ok=True)
                    json.dump(rar_cache, open(RAR_CACHE, "w", encoding="utf-8"), indent=1)
                    print("  %s: %s" % (zn, entry))
                why = ("unreadable: %s" % entry["error"] if "error" in entry
                       else "encrypted" if entry.get("encrypted")
                       else "split" if entry.get("split")
                       else None)
                if why:
                    rar_skipped.append((zn, why))
                    continue

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
            unpacks = 0x08 if is_rar and not entry.get("stored") else 0  # STORE_FLAG_UNPACKS
            rows.append((sort_key(display), 0 if regions & {"USA", "World"} else 1, name,
                         index, disc_no, region_bits, unpacks, cat, tid, int(f.get("size", 0)), zn, name, display))
            added.append((name, ident, zn))
            taken.add((key, disc_no))
    print("%d added from the other set (%d RARs)" % (len(added), sum(1 for a in added if a[2].lower().endswith(".rar"))))

    # Folder games, from store-ids.txt. Each one's a compressed RAR, so it's
    # flagged STORE_FLAG_UNPACKS: the page warns, and Install asks first.
    folder_added = 0
    folder_index = len(items) + len(other_items)
    for index, ident in enumerate(FOLDER_ITEMS, folder_index):
        wanted = {k.split("/", 1)[1]: v for k, v in hand_folders.items() if k.startswith(ident + "/")}
        if not wanted:
            continue
        print("listing %s..." % ident)
        files = {f["name"]: f for f in json.loads(fetch(FILES_URL % ident))["result"]}
        for zn, (tid, title) in sorted(wanted.items()):
            if zn not in files:
                continue  # reported as unused below
            hand_used.add("%s/%s" % (ident, zn))
            tag = next((t for t in zn.split(".") if t in FOLDER_REGIONS), "USA")
            regions = {FOLDER_REGIONS[tag]}
            region_bits = 0
            for r in regions:
                region_bits |= 1 << REGIONS.index(r)
            name = "%s (%s)" % (title, FOLDER_REGIONS[tag])
            display = display_title(title)
            rows.append((sort_key(display), 0 if regions & {"USA", "World"} else 1, name, index, 0, region_bits,
                         0x08, CATEGORIES.index("Games") + 1, tid, int(files[zn].get("size", 0)), zn, name, display))
            folder_added += 1
    print("%d folder games added" % folder_added)

    for name in sorted((set(hand_zips) | set(hand_others) | set(hand_folders)) - hand_used):
        print("store-ids.txt: nothing is named \"%s\" any more" % name)
    if rar_skipped:
        print("%d RARs of games the Store lacks left out:" % len(rar_skipped))
        for zn, why in rar_skipped:
            print("    %s: %s" % (zn, why))

    rows.sort()
    store_games = group_games(rows, None if xbox else hand_groups(GROUPS))

    # Before the releases are built, so a disc whose title is no longer the
    # game's name shows as an edition on its own version - which is how the
    # console then finds its DLC. See hand_names.
    renamed, stale_names, misplaced = ([], [], []) if xbox else rename_games(store_games, hand_names(NAMES))
    for was, now in renamed:
        print("renamed by hand: %s -> %s" % (was, now))
    for t in stale_names:
        print("  %08X in %s is no game here" % (t, os.path.basename(NAMES)))
    for name, neighbour in misplaced:
        print("  %s now sits out of order beside %s" % (name, neighbour))

    releases = [build_releases(rows, g) for g in store_games]
    most_versions = max(len(r) for r in releases)
    most_discs = max(len(r[3]) for rs in releases for r in rs)
    most_in_letter = max(sum(1 for g in store_games if g[0] == c) for c in set(g[0] for g in store_games))
    print("%d games, %d versions; at most %d in a letter, %d versions of one, %d discs in one" %
          (len(store_games), sum(len(r) for r in releases), most_in_letter, most_versions, most_discs))

    if xbox:
        L = xbox_header(version, total, how, not_compatible, rows, store_games, releases,
                        most_in_letter, most_versions, most_discs)
        with open(XBOX_OUT_HEADER, "w", encoding="utf-8", newline="\n") as out:
            out.write("\n".join(L))
        print("wrote %s (%d discs)" % (XBOX_OUT_HEADER, len(rows)))
        if unmatched:
            print("no title ID for %d:" % len(unmatched))
            for u in unmatched:
                print("   ", u)
        return 0

    L = []
    L.append("// GENERATED by tools/make_store_titles.py - do not edit by hand.")
    L.append("//")
    L.append("// The discs in archive.org's Redump Xbox 360 collection, and some from")
    L.append("// another set it lacks (see make_store_titles.py), sorted by the")
    L.append("// name the Store shows, USA and World versions of a game first. Title IDs")
    L.append("// are from Redump's datfile %s, by each disc's serial." % version)
    L.append("// %d zips; title IDs matched: %s." % (total, ", ".join("%s %d" % kv for kv in how.items())))
    L.append("// Then %d from XBOX_360_1 to _6, for games the Redump collection lacks -" % len(added))
    L.append("// zips, and RARs that hold their disc image uncompressed.")
    L.append("// Then %d folder games from the ZTM sets, named in tools/data/store-ids.txt -" % folder_added)
    L.append("// RARs of a game's files, unpacked into <drive>:\\Games on the console.")
    if renamed:
        L.append("//")
        L.append("// %d game%s named by hand, where the discs sharing a title ID are not all" %
                 (len(renamed), "" if len(renamed) == 1 else "s"))
        L.append("// the same game. See tools/data/store-names.txt.")
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
    L.append("#define STORE_FLAG_UNPACKS  0x08 // a compressed RAR, unpacked on the console as it downloads - slow")
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
    L.append("    unsigned char system;        // STORE_SYSTEM_*: left out of this file's rows, so 360")
    L.append("};")
    L.append("")
    L.append("#define STORE_SYSTEM_XBOX360 0")
    L.append("#define STORE_SYSTEM_XBOX    1 // XboxTitles.h's games, the Original Xbox's")
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
    for item in OTHER_ITEMS + FOLDER_ITEMS:
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
