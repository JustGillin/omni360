"""
Packs the Store's publisher logos into one atlas and writes it out as
PublisherLogos.h (the PNG bytes plus where each logo sits), so the art is
compiled into the XEX with nothing extra to deploy - the way make_nav_icons.py
does Home's icons.

    python tools/make_publisher_logos.py           pack the logos in tools/data/publisher-logos
    python tools/make_publisher_logos.py --fetch   ...first fetching any missing from Wikipedia

Run make_collections.py after: it numbers each publisher's logos as this
atlas does.

The logos are the ones in each company's Wikipedia infobox, as PNGs that
Wikimedia renders from the SVGs - the console has no SVG renderer. --fetch
saves them with SOURCES.txt beside them: the Commons file each came from and
its licence there. All the ones in use are public domain on Commons (too
simple to be copyrighted); they're still their owners' trademarks.

Which logos, and which Wikipedia page each comes from, is "logo_sources" in
tools/data/collections.json; a publisher's "logos" name one or two of them.
"""
import io
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCE = os.path.join(ROOT, "tools", "data", "collections.json")
LOGO_DIR = os.path.join(ROOT, "tools", "data", "publisher-logos")
OUT_PNG = os.path.join(ROOT, "Media", "PublisherLogos.png")
OUT_HEADER = os.path.join(ROOT, "src", "PublisherLogos.h")

# The console's PNG decoder takes nothing over PNG_MAX_DIMENSION (512) a
# side, so the atlas has to fit in 512x512: two columns of seven. A card's
# logo room on an HD screen is about 160x70, so a cell is close to 1:1.
MAX_ATLAS = 512             # PNG_MAX_DIMENSION in src/PngDecode.h
CELL_W, CELL_H = 256, 72    # a cell; a logo fits inside it, aspect kept
PAD = 4                     # clear around each, so bilinear filtering doesn't bleed
COLS = 2
FETCH_WIDTH = 512           # Wikimedia renders the SVG this wide

# Wikimedia asks automated clients for a descriptive User-Agent.
USER_AGENT = "omni360-build/1.0 (publisher logo fetch)"


def logo_keys(data):
    """The logos in use, in atlas order: each the first time a publisher names it."""
    keys = []
    for p in data.get("publishers", []):
        for key in p.get("logos", []):
            if key not in keys:
                keys.append(key)
    return keys


def get(url):
    for attempt in range(4):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
            return urllib.request.urlopen(req, timeout=20).read()
        except urllib.error.HTTPError as e:
            if e.code != 429 or attempt == 3:
                raise
            wait = int(e.headers.get("Retry-After", "0") or 0) or 20 * (attempt + 1)
            print("  rate limited, waiting %ds" % wait)
            time.sleep(min(wait, 60))


def api(**params):
    params["format"] = "json"
    return json.loads(get("https://en.wikipedia.org/w/api.php?" + urllib.parse.urlencode(params)))


def fetch(key, page):
    """Saves the logo in `page`'s infobox as <key>.png; returns its SOURCES.txt line."""
    wikitext = api(action="parse", page=page, prop="wikitext", section=0, redirects=1)["parse"]["wikitext"]["*"]
    m = re.search(r"\|\s*logo\s*=\s*(?:\[\[)?(?:File:|Image:)?([^|\]\n{}]+\.(?:svg|png))", wikitext, re.I)
    if not m:
        raise RuntimeError("no logo in the infobox of %s" % page)
    name = m.group(1).strip()
    info = api(action="query", titles="File:" + name, prop="imageinfo", iiprop="url|extmetadata",
               iiurlwidth=FETCH_WIDTH)
    ii = next(iter(info["query"]["pages"].values()))["imageinfo"][0]
    licence = ii.get("extmetadata", {}).get("LicenseShortName", {}).get("value", "unknown")
    with open(os.path.join(LOGO_DIR, key + ".png"), "wb") as f:
        f.write(get(ii.get("thumburl") or ii["url"]))
    return "%s\t%s\tFile:%s\t%s" % (key, page, name, licence)


def main():
    with open(SOURCE, encoding="utf-8") as f:
        data = json.load(f)
    sources = data.get("logo_sources", {})
    keys = logo_keys(data)

    errors = [k for k in keys if k not in sources]
    for k in errors:
        print("ERROR: logo %s has no entry in logo_sources" % k)
    if errors:
        return 1

    os.makedirs(LOGO_DIR, exist_ok=True)
    if "--fetch" in sys.argv:
        lines_path = os.path.join(LOGO_DIR, "SOURCES.txt")
        lines = {}
        if os.path.exists(lines_path):
            with open(lines_path, encoding="utf-8") as f:
                for line in f:
                    if line.strip() and not line.startswith("#"):
                        lines[line.split("\t", 1)[0]] = line.rstrip("\n")
        for key in keys:
            if os.path.exists(os.path.join(LOGO_DIR, key + ".png")):
                continue
            print("fetching %s from %s" % (key, sources[key]))
            lines[key] = fetch(key, sources[key])
            time.sleep(3)  # Wikimedia rate-limits bursts
        with open(lines_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("# key\tWikipedia page\tCommons file\tlicence on Commons\n")
            for key in sorted(lines):
                f.write(lines[key] + "\n")

    missing = [k for k in keys if not os.path.exists(os.path.join(LOGO_DIR, k + ".png"))]
    for k in missing:
        print("ERROR: no %s.png - run with --fetch" % k)
    if missing:
        return 1

    rows = (len(keys) + COLS - 1) // COLS
    atlas_h = 1
    while atlas_h < rows * CELL_H:
        atlas_h *= 2
    if CELL_W * COLS > MAX_ATLAS or atlas_h > MAX_ATLAS:
        print("ERROR: %d logos need a %dx%d atlas; the console decodes %d a side at most"
              % (len(keys), CELL_W * COLS, atlas_h, MAX_ATLAS))
        return 1
    atlas = Image.new("RGBA", (CELL_W * COLS, atlas_h), (0, 0, 0, 0))
    rects = []
    for i, key in enumerate(keys):
        img = Image.open(os.path.join(LOGO_DIR, key + ".png")).convert("RGBA")
        img = img.crop(img.getchannel("A").getbbox() or (0, 0, img.width, img.height))
        scale = min((CELL_W - PAD * 2) / img.width, (CELL_H - PAD * 2) / img.height)
        w, h = max(1, round(img.width * scale)), max(1, round(img.height * scale))
        img = img.resize((w, h), Image.LANCZOS)
        x = (i % COLS) * CELL_W + (CELL_W - w) // 2
        y = (i // COLS) * CELL_H + (CELL_H - h) // 2
        atlas.paste(img, (x, y))
        rects.append((x, y, w, h))

    buf = io.BytesIO()
    atlas.save(buf, "PNG", optimize=True)
    png = buf.getvalue()
    with open(OUT_PNG, "wb") as f:
        f.write(png)

    L = ["// Generated by tools/make_publisher_logos.py - don't edit.",
         "#ifndef PUBLISHER_LOGOS_H",
         "#define PUBLISHER_LOGOS_H",
         "",
         "#define PUBLISHER_LOGO_ATLAS_WIDTH  %d" % atlas.width,
         "#define PUBLISHER_LOGO_ATLAS_HEIGHT %d" % atlas.height,
         "#define PUBLISHER_LOGO_COUNT        %d" % len(keys),
         "",
         "// Where each logo sits in the atlas, trimmed to its art: x, y, w, h.",
         "static const unsigned short kPublisherLogoRect[PUBLISHER_LOGO_COUNT][4] =",
         "{"]
    for key, r in zip(keys, rects):
        L.append("    { %d, %d, %d, %d }, // %s" % (r + (key,)))
    L += ["};", "", "static const unsigned char kPublisherLogosPng[%d] =" % len(png), "{"]
    for i in range(0, len(png), 16):
        L.append("    " + ", ".join("0x%02X" % b for b in png[i:i + 16]) + ",")
    L += ["};", "#define kPublisherLogosPngSize sizeof(kPublisherLogosPng)", "", "#endif", ""]
    with open(OUT_HEADER, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(L))
    print("%d logos, %dx%d atlas, %d bytes -> %s" % (len(keys), atlas.width, atlas.height, len(png),
                                                    os.path.relpath(OUT_HEADER, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
