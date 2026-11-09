"""Builds src/CatalogTitles.h: the retail Xbox 360 games the Xbox Live
catalog knows about that the Store has no download for - what's missing.

    python tools/make_catalog_titles.py

The Store's A-Z is built from what's on archive.org (make_store_titles.py),
so it can only ever show what someone uploaded. This asks the catalog -
catalog.xboxlive.com, the same public one make_store_rows.py reads - for
every retail 360 game it lists, and takes away the ones the Store already
has. What's left is the shortfall, compiled in so a letter page can show
those games greyed out behind a filter.

Unlike make_store_rows.py, which asks about one game at a time by its media
ID, this pages through FindGames with no media ID at all: PAGE_SIZE at a
time until it has the totalItems the first answer promised. Pages are kept
in tools/cache/catalog_all.json, so a rerun re-asks only for what's absent.
DetailView=5 is asked for, not make_store_rows.py's 3, because only it
carries fullTitle: the reducedTitle the lower views give is cut short -
"Barbie Puppy" for "Barbie and Her Sisters Puppy Rescue" - which is no good
for matching names against the Store's.

MediaType 1 is nominally the retail games, but a lot else rides along with
them: avatar items, dashboard themes, apps (Halo Waypoint, State Farm),
demos, betas and offer bundles. An entry is taken to be a game when it has
a real genre from make_store_rows.py's MAIN_GENRES, carries none of the
video marketplace's categories, and isn't named like a demo or a theme.
The test is deliberately a little greedy: a missing game shown by mistake
is a smaller fault than one quietly left out.

A game counts as had when the Store, the Original Xbox list or XBLA has its
title ID, its effectiveTitleId, or its name. Name matching is needed because
the catalog and Redump disagree about IDs more often than you'd think:
BioShock 2 is 0x54540867 in the catalog and 0x54540861 on its disc. It also
catches the Xbox Originals re-releases, which sit among the catalog's 360
entries under IDs of their own - Max Payne is downloadable from the Store's
Original Xbox section, so it isn't missing. match_key is
make_store_titles.py's, the same boiling-down it uses to match the two
archive.org sets to each other.

XBLIG is left out of the comparison entirely: every indie game shares the
one XNA title ID (0x584E07D2), so there's nothing to match on.

What's left over after all that is still not quite a list of retail discs:
the catalog is the whole marketplace, and a shortened second name for a game
already in the list has no tell in the data at all. That part is hand-kept.
tools/data/missing-games.txt is the file Omni360's own Review screen writes
(Settings > Games we don't have, see src/MissingGames.h) - copy it off the
console into tools/data and every title ID in it marked as anything but
"keep" is left out of the generated list.

Run it after make_store_titles.py. Needs network access.
"""
import io
import json
import os
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_store_rows import (SKIP_GENRES, MAIN_GENRES, GENRE_NAMES, TAG_ROWS,
                             store_games)
from make_store_titles import sort_key, letter_of, match_key

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STORE_TITLES = os.path.join(ROOT, "src", "StoreTitles.h")
XBOX_TITLES = os.path.join(ROOT, "src", "XboxTitles.h")
XBLA_TITLES = os.path.join(ROOT, "src", "XblaTitles.h")
OUT_HEADER = os.path.join(ROOT, "src", "CatalogTitles.h")
CACHE = os.path.join(ROOT, "tools", "cache", "catalog_all.json")
# Written by the console, copied here by hand - see the note above.
MARKS = os.path.join(ROOT, "tools", "data", "missing-games.txt")

PAGE_SIZE = 100
MEDIA_TYPE = 1   # retail 360 games
DETAIL_VIEW = 5  # the only one with fullTitle
CACHE_FORM = 2   # bumped when an entry's fields change, to refetch

FIND_GAMES = ("http://catalog.xboxlive.com/Catalog/Catalog.asmx/Query?methodName=FindGames"
              "&Names=Locale&Values=en-US&Names=LegalLocale&Values=en-US&Names=Store&Values=1"
              "&Names=PageSize&Values=%d&Names=PageNum&Values=%d&Names=DetailView&Values=%d"
              "&Names=OfferFilterLevel&Values=1&Names=UserTypes&Values=2"
              "&Names=MediaTypes&Values=%d")

ENTRY = re.compile(r"<entry .*?</entry>", re.S)
# A StoreGame row in StoreTitles.h or XboxTitles.h. make_store_rows.py's ROW
# won't do for both: an Original Xbox row carries a seventh field for its
# system, so it stops short of the closing brace.
GAME_ROW = re.compile(r'^\s*\{ "((?:[^"\\]|\\.)*)", 0x[0-9A-F]{8}, 0x[0-9A-F]+, \d+, \d+, \'.\'')
XBLA_ROW = re.compile(r'^\s*\{ "((?:[^"\\]|\\.)*)", "(?:[^"\\]|\\.)*", \d+ULL, 0x([0-9A-F]{8}),')

# Apps and TV shows that claim a real genre, so nothing else here catches
# them. Short on purpose: the video marketplace's categories look like they
# would do the job - an app carries them and a game doesn't - but plenty of
# real games carry them too, for their trailers and making-of videos, and
# dropping on that loses Too Human, Ninja Gaiden II and WWE '12 to be rid of
# a dozen apps. Matched on the name, case-insensitively.
NOT_GAMES = {"halo waypoint", "major league gaming", "rock band music store",
             "kinect for xbox 360", "kinect fun labs", "mlb.tv content", "state farm",
             "world of tanks public test", "spartacus: blood and sand", "game room",
             "xbox live labs", "video kinect"}

# Named like something that isn't the game itself.
JUNK_NAME = re.compile(
    r"(^download\b|\bavatar\s+(item|pack|bundle)|\btheme$|\bthemes$"
    r"|\b(demo|trial|beta|preview|bundle|pack)$"
    r"|\[parent|\btech\s+beta\b|\btrailer\b|\bgamerpic)", re.I)

ESCAPES = (("&lt;", "<"), ("&gt;", ">"), ("&quot;", '"'),
           ("&apos;", "'"), ("&#39;", "'"), ("&amp;", "&"))


def unescape(s):
    for entity, ch in ESCAPES:
        s = s.replace(entity, ch)
    return s


HEX_DIGITS = "0123456789abcdefABCDEF"


def c_string(s):
    """A C string literal of a UTF-8 name. make_store_rows.py has one of
    these too, but this one closes the literal and opens another when a hex
    digit follows a \\xNN escape: C reads as many hex digits as it can, so
    "WALL\\xE2\\x80\\xA2E" would be the one character \\xA2E, which doesn't
    fit in one. Its names never happen to do that; these do."""
    out = []
    escaped = False
    for b in s.encode("utf-8"):
        ch = chr(b)
        if b in (0x22, 0x5C):
            out.append("\\" + ch)
            escaped = False
        elif 0x20 <= b < 0x7F:
            if escaped and ch in HEX_DIGITS:
                out.append('" "')
            out.append(ch)
            escaped = False
        else:
            out.append("\\x%02X" % b)
            escaped = True
    return '"' + "".join(out) + '"'


def tidy(name):
    """The catalog's name as the Store would show it: its registered-mark
    clutter gone, its spacing tidied."""
    name = name.replace("®", "").replace("™", "").replace("©", "")
    return re.sub(r"\s+", " ", name).strip(" :-")


def fetch_page(page):
    """One page of the catalog, as a list of entry dicts, and the catalog's
    total. (None, None) if it failed."""
    url = FIND_GAMES % (PAGE_SIZE, page, DETAIL_VIEW, MEDIA_TYPE)
    req = urllib.request.Request(url, headers={"User-Agent": "Omni360 make_catalog_titles.py"})
    for attempt in range(3):
        try:
            with urllib.request.urlopen(req, timeout=40) as r:
                xml = r.read().decode("utf-8", "replace")
            break
        except Exception as e:
            if attempt == 2:
                print("  page %d failed: %s" % (page, type(e).__name__))
                return None, None
            time.sleep(2 * (attempt + 1))
    total = re.search(r"<live:totalItems>(\d+)", xml)
    total = int(total.group(1)) if total else 0
    out = []
    for raw in ENTRY.findall(xml):
        tid = re.search(r"<live:titleId>(\d+)</live:titleId>", raw)
        full = re.search(r"<live:fullTitle>(.*?)</live:fullTitle>", raw, re.S)
        reduced = re.search(r"<live:reducedTitle>(.*?)</live:reducedTitle>", raw, re.S)
        if not tid or not (full or reduced):
            continue
        eff = re.search(r"<live:effectiveTitleId>(\d+)", raw)
        cats = re.search(r"<live:categories>(.*?)</live:categories>", raw, re.S)
        genres = []
        if cats:
            for g in re.findall(r"<live:name>([^<]*)</live:name>", cats.group(1)):
                g = unescape(g)
                if g not in genres:
                    genres.append(g)
        ratings = re.search(r"<live:numberOfRatings>(\d+)", raw)
        stars = re.search(r"<live:ratingAggregate>([\d.]+)", raw)
        release = re.search(r"<live:releaseDate>(\d{4})", raw)
        out.append({"titleId": int(tid.group(1)),
                    "effectiveTitleId": int(eff.group(1)) if eff else 0,
                    "name": tidy(unescape((full or reduced).group(1))),
                    "genres": genres,
                    "ratings": int(ratings.group(1)) if ratings else 0,
                    "stars": float(stars.group(1)) if stars else 0.0,
                    "year": int(release.group(1)) if release else 0})
    return out, total


def save(total, pages):
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE, "w", encoding="utf-8") as f:
        json.dump({"form": CACHE_FORM, "total": total, "pages": pages}, f)


def catalog():
    """Every entry the catalog lists, by title ID, cached across runs."""
    cache = {}
    if os.path.exists(CACHE):
        cache = json.load(open(CACHE, encoding="utf-8"))
        if cache.get("form") != CACHE_FORM:
            print("cached pages are an older form; fetching them again")
            cache = {}
    pages = cache.get("pages", {})
    first, total = fetch_page(1)
    if first is None:
        if not pages:
            sys.exit("the catalog didn't answer and there's nothing cached")
        print("the catalog didn't answer; using the %d cached pages" % len(pages))
        total = cache.get("total", 0)
    else:
        pages["1"] = first
    last = (total + PAGE_SIZE - 1) // PAGE_SIZE if total else max(int(p) for p in pages)
    print("%d items, %d pages of %d" % (total, last, PAGE_SIZE))
    for page in range(2, last + 1):
        if str(page) in pages:
            continue
        got, _ = fetch_page(page)
        if got is not None:
            pages[str(page)] = got
        if page % 5 == 0 or page == last:
            print("  page %d of %d" % (page, last))
            save(total, pages)
    save(total, pages)

    by_id = {}
    for page in pages.values():
        for e in page:
            # the catalog repeats a game across pages now and then; the one
            # with the most ratings is the live entry
            old = by_id.get(e["titleId"])
            if old is None or e["ratings"] > old["ratings"]:
                by_id[e["titleId"]] = e
    return by_id, len(pages), total


def game_names(path):
    """Every game one of the Store's disc tables lists, by name. For
    StoreTitles.h this is wider than store_games(), which is keyed by title
    ID and so leaves out the 45 whose disc Redump's datfile couldn't be
    matched to one - they're still games the Store can download, and their
    names are all there is to recognise them by."""
    out = []
    with open(path, encoding="latin-1") as f:
        for line in f:
            m = GAME_ROW.match(line)
            if m:
                out.append(m.group(1).encode("latin-1").decode("unicode_escape")
                           .encode("latin-1").decode("utf-8", "replace"))
    return out


def xbla_games():
    """{title ID: name} of the XBLA games."""
    out = {}
    with open(XBLA_TITLES, encoding="latin-1") as f:
        for line in f:
            m = XBLA_ROW.match(line)
            if m:
                out[int(m.group(2), 16)] = m.group(1)
    return out


MARK_LINE = re.compile(r"^([0-9A-Fa-f]{8})\s*=\s*([a-z]+)")


def hand_marks(path):
    """The marks from the console's missing-games.txt: title ID -> the one
    word saying what the entry is. Everything but "keep" is a row to leave
    out. A missing file is not an error - this list is optional, and a clone
    that hasn't curated anything should still generate."""
    marks = {}
    if not os.path.exists(path):
        print("no %s - nothing left out by hand" % os.path.basename(path))
        return marks
    with io.open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            m = MARK_LINE.match(line)
            if m:
                marks[int(m.group(1), 16)] = m.group(2)
    return marks


def is_game(e):
    if not e["name"] or JUNK_NAME.search(e["name"]):
        return False
    if e["name"].lower() in NOT_GAMES:
        return False
    return any(g in MAIN_GENRES for g in e["genres"])


def words(title):
    """A title as lowercase alphanumeric words, for prefix matching."""
    s = title.lower().replace("&", " and ").replace("'", "")
    return [w for w in re.split(r"[^a-z0-9]+", s) if w and w != "the"]


ORDINAL = re.compile(r"^(\d+|i{1,3}|iv|v|vi{1,3}|ix|x{1,2})$")


def is_prefix_of(short, long):
    """Whether a catalog name is the Store's name cut short: "Harry Potter
    and the Deathly Hallows" for "... - Part 1". The word that follows must
    not be a number or a numeral, or Crysis would match Crysis 2."""
    if not short or len(short) >= len(long) or long[:len(short)] != short:
        return False
    return not ORDINAL.match(long[len(short)])


def main():
    entries, got_pages, total = catalog()
    print("%d distinct title IDs from %d pages" % (len(entries), got_pages))

    games = {t: e for t, e in entries.items() if is_game(e)}
    print("%d of them are games; %d are apps, themes, avatar items and the like"
          % (len(games), len(entries) - len(games)))

    store, xbla = store_games(), xbla_games()
    have_ids = set(store) | set(xbla)
    # Redump joins two titles on one disc with " ~ " - "Resident Evil:
    # Revelations ~ Biohazard - Revelations" - so index each side as well.
    # Original Xbox games count as had too. The catalog carries the Xbox
    # Originals re-releases among its 360 entries, under their own title IDs,
    # so Max Payne came out as missing while sitting in the Store's Original
    # Xbox section all along. Their names go in; their IDs don't, as the two
    # systems' ID ranges aren't formally apart and a collision would hide a
    # 360 game that really is missing.
    have_names = []
    for n in game_names(STORE_TITLES) + game_names(XBOX_TITLES) + list(xbla.values()):
        have_names.append(n)
        if " ~ " in n:
            have_names += [part.strip() for part in n.split(" ~ ")]
    have_keys = {match_key(n) for n in have_names}
    # grouped by first word, so a prefix check looks at a handful of names
    have_words = {}
    for n in have_names:
        w = words(n)
        if w:
            have_words.setdefault(w[0], []).append(w)

    def had_by_id(e):
        return e["titleId"] in have_ids or e["effectiveTitleId"] in have_ids

    def had_by_name(e):
        if match_key(e["name"]) in have_keys:
            return True
        w = words(e["name"])
        return bool(w) and any(is_prefix_of(w, s) for s in have_words.get(w[0], ()))

    missing, by_id, by_name = {}, 0, 0
    for t, e in games.items():
        if had_by_id(e):
            by_id += 1
        elif had_by_name(e):
            by_name += 1
        else:
            missing[t] = e
    print("%d already had - %d by title ID, %d by name alone; %d missing"
          % (by_id + by_name, by_id, by_name, len(missing)))

    # The hand-kept part: duplicates under a shortened name, title update
    # discs, test products - whatever someone sat down and marked on the
    # console. "keep" says a row was looked at and really is a missing game,
    # so it stays.
    marks = hand_marks(MARKS)
    excluded = {t: m for t, m in marks.items() if m != "keep" and t in missing}
    for t in excluded:
        del missing[t]
    stale = [t for t in marks if t not in missing and t not in excluded]
    by_mark = {}
    for m in excluded.values():
        by_mark[m] = by_mark.get(m, 0) + 1
    if marks:
        print("%d left out by hand%s; %d missing after that"
              % (len(excluded),
                 (" (" + ", ".join("%s %d" % (m, n) for m, n in sorted(by_mark.items())) + ")") if by_mark else "",
                 len(missing)))
    if stale:
        print("  %d mark%s for a title ID that isn't in the list any more - already "
              "left out, or the Store has it now" % (len(stale), "" if len(stale) == 1 else "s"))

    genre_names = sorted(GENRE_NAMES.get(g, g) for g in MAIN_GENRES)

    def genre_of(e):
        tags = [g for g in e["genres"] if g not in SKIP_GENRES]
        if "Fighting" in tags and "Platformer" in tags:
            tags = [g for g in tags if g not in ("Fighting", "Platformer")] + ["Action & Adventure"]
        main = next((g for g in MAIN_GENRES if g in tags), None)
        return genre_names.index(GENRE_NAMES.get(main, main)) + 1 if main else 0

    def tags_of(e):
        return sum(1 << i for i, g in enumerate(TAG_ROWS) if g in e["genres"])

    rows = sorted(missing.values(), key=lambda e: (sort_key(e["name"]), e["titleId"]))
    per_letter = {}
    for e in rows:
        letter = letter_of(e["name"])
        per_letter[letter] = per_letter.get(letter, 0) + 1
    most = max(per_letter.values()) if per_letter else 0

    L = ["// GENERATED by tools/make_catalog_titles.py - do not edit by hand.",
         "//",
         "// The retail Xbox 360 games the Xbox Live catalog lists that the Store has",
         "// no download for - the shortfall between what was released and what's on",
         "// archive.org. Sorted the way the Store sorts, so a letter page can merge",
         "// them into its grid behind a filter.",
         "// Of the catalog's %d media-type-1 items, %d are games, not apps or avatar" % (total, len(games)),
         "// items; the Store, the Original Xbox list and XBLA have %d of those,"
         % (len(games) - len(missing) - len(excluded)),
         "// leaving %d here." % len(missing),
         "//"]
    if excluded:
        L += ["// %d more were marked by hand as not games anyone could own - duplicates" % len(excluded),
              "// under a shortened name, title update discs, test products - and left out.",
              "// See tools/data/missing-games.txt.",
              "//"]
    L += ["// Names are the catalog's fullTitle. XBLIG isn't compared: every indie game",
         "// shares the one XNA title ID.",
         "",
         "#ifndef CATALOG_TITLES_H",
         "#define CATALOG_TITLES_H",
         "",
         "// A game nobody has uploaded. Genre and tags are StoreRows.h's, so the",
         "// letter pages' filters work on these rows too.",
         "struct CatalogTitle",
         "{",
         "    const char *name;       // the catalog's name, UTF-8",
         "    unsigned long titleId;",
         "    unsigned long ratings;  // how many people rated it on Xbox Live",
         "    unsigned short year;    // it released; 0 if the catalog didn't say",
         "    unsigned char stars;    // their average, in tenths: 0 to 50",
         "    unsigned char genre;    // 1 + an index into kStoreGenres; 0 for none",
         "    unsigned char tags;     // STORE_TAG_*",
         "    char letter;            // its A-Z tile: 'A' to 'Z', or '#'",
         "};",
         "",
         "#define CATALOG_TITLE_COUNT       %d" % len(rows),
         "#define CATALOG_MAX_LETTER_TITLES %d // the most under one letter" % most,
         "",
         "#ifdef CATALOG_TITLES_DATA",
         "",
         "static const CatalogTitle kCatalogTitles[CATALOG_TITLE_COUNT] =",
         "{"]
    for e in rows:
        L.append("    { %s, 0x%08X, %d, %d, %d, %d, 0x%02X, '%s' }," % (
            c_string(e["name"]), e["titleId"], e["ratings"], e["year"],
            int(round(e["stars"] * 10)), genre_of(e), tags_of(e), letter_of(e["name"])))
    L += ["};", "", "#endif // CATALOG_TITLES_DATA", "", "#endif", ""]

    with open(OUT_HEADER, "w", encoding="utf-8", newline="\n") as out:
        out.write("\n".join(L))
    print("wrote %s: %d games" % (OUT_HEADER, len(rows)))
    print("per letter:", " ".join("%s%d" % (c, n) for c, n in sorted(per_letter.items())))
    print("most-rated missing:")
    for e in sorted(rows, key=lambda e: -e["ratings"])[:15]:
        print("  %-45s %6d ratings" % (e["name"][:45], e["ratings"]))


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main()
