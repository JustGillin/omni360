"""
Builds src/XblaTitles.h: the Xbox Live Arcade games in archive.org's
XBOX_360_XBLA item, with each one's title ID, for the Store's XBLA A to Z.

    python tools/make_xbla_titles.py --keys path\\to\\ArchiveOrgKeys.txt

Each game is a RAR laid out like the console's Content folder, under a
folder of the game's name:

    Banjo Kazooie/58410954/000D0000/DA78E477AA5E31A7D01AE8F84109FD4BF89E49E8

so the title ID is in the RAR's file list - which is all this reads. The
RARs are private, so that takes the archive.org IAS3 keys (the same
two-line ArchiveOrgKeys.txt the app uses: access key, then secret key);
they're sent only to archive.org, as the app sends them. Each RAR's
headers are walked with small Range requests, as the console's DLC walk
does (RarHeaders.cpp) - RAR4 and RAR5 both - without downloading the games.

What each RAR held is cached (tools/cache/xbla_listings.json, not committed),
so a rerun only asks about RARs that are new or changed size.

    --keys PATH   the keys file (default: %USERPROFILE%\\.omni360\\ArchiveOrgKeys.txt)
    --limit N     only the first N RARs, for a trial run
"""
import argparse, json, os, re, struct, sys, time, urllib.parse, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_HEADER = os.path.join(ROOT, "src", "XblaTitles.h")
CACHE = os.path.join(ROOT, "tools", "cache", "xbla_listings.json")
DEFAULT_KEYS = os.path.join(os.path.expanduser("~"), ".omni360", "ArchiveOrgKeys.txt")

ITEMS = ["XBOX_360_XBLA"]
CHUNK = 64 * 1024        # bytes asked for at a time; most RARs' headers fit in the first
MAX_ENTRIES = 400        # a sanity limit on one RAR's list
HEX8 = re.compile(r"^[0-9A-Fa-f]{8}$")

CONTENT_TYPES = {0x000D0000: "arcade", 0x00000002: "DLC", 0x000B0000: "title update",
                 0x000E0000: "indie", 0x00007000: "GoD", 0x00080000: "demo", 0x00090000: "video",
                 0x00009000: "avatar item", 0x00030000: "theme", 0x00020000: "gamer picture"}


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def fetch(url, headers=None, tries=4):
    for attempt in range(tries):
        req = urllib.request.Request(url, headers=dict({"User-Agent": "Omni360 make_xbla_titles.py"}, **(headers or {})))
        try:
            with OPENER.open(req, timeout=60) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            if e.code in (401, 403):
                sys.exit("archive.org refused the keys (HTTP %d) - check the keys file" % e.code)
            if e.code < 500 or attempt == tries - 1:
                return e.code, e.read()
        except Exception as e:
            if attempt == tries - 1:
                raise
        time.sleep(2 * (attempt + 1))


def load_keys(path):
    if not os.path.exists(path):
        sys.exit("no keys file at %s - pass --keys" % path)
    lines = [l.strip() for l in open(path, encoding="utf-8-sig").read().splitlines() if l.strip()]
    if len(lines) < 2:
        sys.exit("%s should have the access key on line 1 and the secret key on line 2" % path)
    return "LOW %s:%s" % (lines[0], lines[1])


class RarReader:
    """Byte ranges of one RAR on its storage server, in CHUNK-sized reads."""

    def __init__(self, url, auth):
        self.url, self.auth = url, auth
        self.base, self.data = -1, b""
        self.requests = 0

    def read(self, offset, length):
        if not (self.base <= offset and offset + length <= self.base + len(self.data)):
            want = max(length, CHUNK)
            status, body = fetch(self.url, {"Authorization": self.auth,
                                            "Range": "bytes=%d-%d" % (offset, offset + want - 1)})
            self.requests += 1
            if status not in (200, 206):
                raise IOError("HTTP %d" % status)
            if status == 200:
                body = body[offset:offset + want]
            self.base, self.data = offset, body
        start = offset - self.base
        return self.data[start:start + length]


# ---------------------------------------------------------------------------
# RAR headers - the same formats as RarHeaders.cpp
# ---------------------------------------------------------------------------

def vint(b, i):
    """A RAR5 variable-length integer at b[i]: (value, index after)."""
    value, shift = 0, 0
    while i < len(b):
        byte = b[i]
        value |= (byte & 0x7F) << shift
        i += 1
        if not byte & 0x80:
            return value, i
        shift += 7
    raise ValueError("truncated vint")


def list_rar4(r, start):
    entries, offset = [], start
    for _ in range(MAX_ENTRIES):
        h = r.read(offset, 7)
        if len(h) < 7:
            break
        _, kind, flags, size = struct.unpack("<HBHH", h)
        if kind == 0x7B or size < 7:  # end of archive
            break
        head = r.read(offset, size)
        add = 0
        if kind == 0x74:  # a file
            pack, unp = struct.unpack("<II", head[7:15])
            name_len = struct.unpack("<H", head[26:28])[0]
            name_at = 32
            if flags & 0x100:  # 64-bit sizes
                high_pack, high_unp = struct.unpack("<II", head[32:40])
                pack |= high_pack << 32
                unp |= high_unp << 32
                name_at = 40
            raw = head[name_at:name_at + name_len]
            if flags & 0x200 and b"\x00" in raw:
                raw = raw.split(b"\x00")[0]  # the ASCII form before the Unicode one
            name = raw.decode("utf-8", "replace").replace("/", "\\")
            directory = (flags & 0xE0) == 0xE0
            entries.append((name, unp, directory))
            add = pack
        elif flags & 0x8000:
            add = struct.unpack("<I", head[7:11])[0]
        offset += size + add
    return entries


def list_rar5(r, start):
    entries, offset = [], start
    for _ in range(MAX_ENTRIES):
        h = r.read(offset, 16)
        if len(h) < 7:
            break
        size, i = vint(h, 4)
        total = i + size
        head = r.read(offset, total)
        kind, j = vint(head, i)
        flags, j = vint(head, j)
        extra = data = 0
        if flags & 1:
            extra, j = vint(head, j)
        if flags & 2:
            data, j = vint(head, j)
        if kind == 5:  # end of archive
            break
        if kind == 4:
            raise ValueError("headers encrypted")
        if kind == 2:  # a file
            file_flags, j = vint(head, j)
            unp, j = vint(head, j)
            _, j = vint(head, j)  # attributes
            if file_flags & 2:
                j += 4  # mtime
            if file_flags & 4:
                j += 4  # data CRC
            _, j = vint(head, j)  # compression
            _, j = vint(head, j)  # host OS
            name_len, j = vint(head, j)
            name = head[j:j + name_len].decode("utf-8", "replace").replace("/", "\\")
            entries.append((name, unp, bool(file_flags & 1)))
        offset += total + data
    return entries


def list_rar(r):
    sig = r.read(0, 8)
    if sig[:7] == b"Rar!\x1a\x07\x00":
        return "rar4", list_rar4(r, 7)
    if sig[:8] == b"Rar!\x1a\x07\x01\x00":
        return "rar5", list_rar5(r, 8)
    raise ValueError("not a RAR (starts %r)" % sig[:8])


# ---------------------------------------------------------------------------
# What a RAR's list says
# ---------------------------------------------------------------------------

def packages(entries):
    """The packages in a list: (title ID, content type, path, size), from
    paths like "Banjo Kazooie\\58410954\\000D0000\\DA78...", wherever in the
    path the two 8-digit folders are."""
    out = []
    for name, size, directory in entries:
        if directory:
            continue
        parts = name.split("\\")
        for k in range(len(parts) - 2):
            if HEX8.match(parts[k]) and HEX8.match(parts[k + 1]):
                out.append((int(parts[k], 16), int(parts[k + 1], 16), name, size))
                break
    return out


def main_package(pkgs):
    """The game itself: the biggest arcade package, else the biggest of any."""
    games = [p for p in pkgs if p[1] == 0x000D0000] or pkgs
    return max(games, key=lambda p: p[3]) if games else None


def c_string(s):
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


def sort_key(name):
    s = name.lower()
    for article in ("the ", "a ", "an "):
        if s.startswith(article):
            s = s[len(article):]
            break
    return re.sub(r"[^a-z0-9 ]", "", s).strip()


def letter_of(name):
    c = sort_key(name)[:1]
    return c.upper() if "a" <= c <= "z" else "#"


# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Build src/XblaTitles.h from archive.org's XBLA RARs.")
    parser.add_argument("--keys", default=DEFAULT_KEYS)
    parser.add_argument("--limit", type=int, default=0)
    args = parser.parse_args()
    auth = load_keys(args.keys)

    cache = json.load(open(CACHE, encoding="utf-8")) if os.path.exists(CACHE) else {}
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)

    rows = []
    for index, item in enumerate(ITEMS):
        status, body = fetch("https://archive.org/metadata/%s" % item)
        meta = json.loads(body)
        server, folder = meta["server"], meta["dir"]
        rars = sorted((f for f in meta["files"] if f["name"].lower().endswith(".rar") and "/" not in f["name"]),
                      key=lambda f: f["name"].lower())
        if args.limit:
            rars = rars[:args.limit]
        print("%s: %d RARs on %s" % (item, len(rars), server))

        asked = 0
        for n, f in enumerate(rars, 1):
            key = "%s/%s" % (item, f["name"])
            size = int(f.get("size", 0))
            entry = cache.get(key)
            if entry is None or entry.get("size") != size:
                url = "https://%s%s/%s" % (server, folder, urllib.parse.quote(f["name"]))
                reader = RarReader(url, auth)
                try:
                    fmt, entries = list_rar(reader)
                    entry = {"size": size, "format": fmt, "entries": entries}
                except Exception as e:
                    entry = {"size": size, "error": str(e)}
                cache[key] = entry
                asked += 1
                pk = packages(entry.get("entries", []))
                print("  [%d/%d] %s: %s, %d packages, %d requests%s" % (
                    n, len(rars), f["name"], entry.get("format", "?"), len(pk), reader.requests,
                    "  ERROR " + entry["error"] if "error" in entry else ""))
                if asked % 20 == 0:
                    json.dump(cache, open(CACHE, "w", encoding="utf-8"))
            pk = packages(entry.get("entries", []))
            rows.append((index, f["name"], size, entry, pk))
        json.dump(cache, open(CACHE, "w", encoding="utf-8"))
        print("%s: asked about %d, %d from the cache" % (item, asked, len(rars) - asked))

    # The table: one game per RAR that holds a package.
    games, skipped = [], []
    for index, rar, size, entry, pk in rows:
        main_pkg = main_package(pk)
        if main_pkg is None:
            skipped.append((rar, entry.get("error") or "no packages in its list"))
            continue
        name = rar[:-4]
        kinds = {}
        for p in pk:
            kinds[p[1]] = kinds.get(p[1], 0) + 1
        games.append((sort_key(name), name, index, rar, size, main_pkg[0], main_pkg[1], len(pk), kinds))
    games.sort()

    by_type = {}
    for g in games:
        for t, c in g[8].items():
            by_type[t] = by_type.get(t, 0) + c
    print("%d games; %d RARs skipped" % (len(games), len(skipped)))
    print("packages by content type: " + ", ".join("%08X %s: %d" % (t, CONTENT_TYPES.get(t, "?"), c)
                                                  for t, c in sorted(by_type.items(), key=lambda x: -x[1])))
    for rar, why in skipped[:30]:
        print("  skipped %s: %s" % (rar, why))

    L = []
    L.append("// GENERATED by tools/make_xbla_titles.py - do not edit by hand.")
    L.append("//")
    L.append("// The Xbox Live Arcade games in archive.org's %s, one RAR each, sorted by" % ", ".join(ITEMS))
    L.append("// name. Title IDs are from each RAR's own file list, which keeps the")
    L.append("// packages under TITLEID\\CONTENTTYPE\\ folders.")
    L.append("// %d games; %d RARs held no package and are left out." % (len(games), len(skipped)))
    L.append("")
    L.append("#ifndef XBLA_TITLES_H")
    L.append("#define XBLA_TITLES_H")
    L.append("")
    L.append("struct XblaGame")
    L.append("{")
    L.append("    const char *name;            // the RAR's name, less .rar - the name shown")
    L.append("    const char *rar;             // the file in the item")
    L.append("    unsigned long long size;     // the RAR's")
    L.append("    unsigned long titleId;       // the game package's")
    L.append("    unsigned long contentType;   // the game package's: 000D0000 for an arcade game")
    L.append("    unsigned short packages;     // every package in the RAR - the game, and any DLC or updates with it")
    L.append("    unsigned char item;          // into kXblaItems")
    L.append("    char letter;                 // its A-Z tile: 'A' to 'Z', or '#'")
    L.append("};")
    L.append("")
    L.append("#define XBLA_GAME_COUNT %d" % len(games))
    L.append("")
    L.append("#ifdef XBLA_TITLES_DATA")
    L.append("")
    L.append("static const char *const kXblaItems[] = { %s };" % ", ".join('"%s"' % i for i in ITEMS))
    L.append("")
    L.append("static const XblaGame kXblaGames[] =")
    L.append("{")
    for _, name, index, rar, size, tid, ctype, count, _ in games:
        L.append("    { %s, %s, %dULL, 0x%08X, 0x%08X, %d, %d, '%s' }," % (
            c_string(name), c_string(rar), size, tid, ctype, count, index, letter_of(name)))
    L.append("};")
    L.append("")
    L.append("#endif // XBLA_TITLES_DATA")
    L.append("")
    L.append("#endif // XBLA_TITLES_H")
    L.append("")
    open(OUT_HEADER, "w", encoding="utf-8", newline="\n").write("\n".join(L))
    print("wrote %s" % OUT_HEADER)


if __name__ == "__main__":
    main()
