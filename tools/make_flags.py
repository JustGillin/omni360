"""
Draws the flags Settings shows beside each language into one atlas and
writes it out as src/Flags.h (the PNG bytes plus where each flag sits), so
they're compiled into the XEX like the nav icons.

    python tools/make_flags.py

A language file picks its flag with "@flag = br"; the codes are the names in
FLAGS below. Re-run after adding one, and commit Flags.h and Media/Flags.png
with this file.

Every flag is drawn here from shapes - stripes, crosses, circles, a maple
leaf - rather than copied from anywhere, so there's no artwork licence to
carry. They're simplified where a real flag has detail too fine to read at
this size (Spain's and Portugal's arms, Mexico's eagle, the US's fifty
stars, which are dots here).

Each is drawn at 8x and box-filtered down, so edges are antialiased, with
rounded corners baked into the alpha. Cells are 64x48 (4:3); the atlas is
512x256, inside the console PNG decoder's 512px limit.
"""
import io
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PNG = os.path.join(ROOT, "Media", "Flags.png")
OUT_HEADER = os.path.join(ROOT, "src", "Flags.h")

SS = 8
CELL_W, CELL_H = 64, 48
COLS, ROWS = 8, 5
ATLAS_W, ATLAS_H = 512, 256
RADIUS = 5   # corner radius, final pixels

W, H = CELL_W * SS, CELL_H * SS


def hstripes(d, colours, weights=None):
    weights = weights or [1] * len(colours)
    total, y = float(sum(weights)), 0.0
    for c, w in zip(colours, weights):
        h = H * w / total
        d.rectangle([0, round(y), W, round(y + h)], fill=c)
        y += h


def vstripes(d, colours, weights=None):
    weights = weights or [1] * len(colours)
    total, x = float(sum(weights)), 0.0
    for c, w in zip(colours, weights):
        wd = W * w / total
        d.rectangle([round(x), 0, round(x + wd), H], fill=c)
        x += wd


def disc(d, cx, cy, r, fill):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill)


def star(d, cx, cy, r, fill, points=5, inner=0.38, rot=-90):
    pts = []
    for i in range(points * 2):
        rr = r if i % 2 == 0 else r * inner
        a = math.radians(rot + i * 180.0 / points)
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    d.polygon(pts, fill=fill)


def flag_us(d):
    hstripes(d, ["#B22234", "#FFFFFF"] * 6 + ["#B22234"])
    cw, ch = W * 0.42, H * 7 / 13.0
    d.rectangle([0, 0, cw, ch], fill="#3C3B6E")
    for row in range(5):
        for col in range(6):
            x = cw * (col + 0.5 + (0.5 if row % 2 else 0) * 0.85) / 6.4
            y = ch * (row + 0.6) / 5.2
            disc(d, x, y, SS * 1.6, "#FFFFFF")


MAPLE = [(0, -1), (.12, -.72), (.32, -.80), (.25, -.38), (.55, -.62), (.62, -.45), (.85, -.50),
         (.72, -.18), (.90, -.08), (.50, .20), (.55, .38), (.08, .30), (.06, .75), (-.06, .75),
         (-.08, .30), (-.55, .38), (-.50, .20), (-.90, -.08), (-.72, -.18), (-.85, -.50),
         (-.62, -.45), (-.55, -.62), (-.25, -.38), (-.32, -.80), (-.12, -.72)]


def flag_ca(d):
    vstripes(d, ["#D52B1E", "#FFFFFF", "#D52B1E"], [1, 2, 1])
    s = H * 0.40
    d.polygon([(W / 2 + x * s, H / 2 + y * s) for x, y in MAPLE], fill="#D52B1E")


def flag_gb(d):
    d.rectangle([0, 0, W, H], fill="#012169")
    d.line([(0, 0), (W, H)], fill="#FFFFFF", width=int(H * 0.2))
    d.line([(0, H), (W, 0)], fill="#FFFFFF", width=int(H * 0.2))
    d.line([(0, 0), (W, H)], fill="#C8102E", width=int(H * 0.07))
    d.line([(0, H), (W, 0)], fill="#C8102E", width=int(H * 0.07))
    d.rectangle([W / 2 - H * 0.17, 0, W / 2 + H * 0.17, H], fill="#FFFFFF")
    d.rectangle([0, H / 2 - H * 0.17, W, H / 2 + H * 0.17], fill="#FFFFFF")
    d.rectangle([W / 2 - H * 0.1, 0, W / 2 + H * 0.1, H], fill="#C8102E")
    d.rectangle([0, H / 2 - H * 0.1, W, H / 2 + H * 0.1], fill="#C8102E")


def flag_de(d):
    hstripes(d, ["#000000", "#DD0000", "#FFCE00"])


def flag_es(d):
    hstripes(d, ["#AA151B", "#F1BF00", "#AA151B"], [1, 2, 1])


def flag_fr(d):
    vstripes(d, ["#0055A4", "#FFFFFF", "#EF4135"])


def flag_it(d):
    vstripes(d, ["#009246", "#FFFFFF", "#CE2B37"])


def flag_pl(d):
    hstripes(d, ["#FFFFFF", "#DC143C"])


def flag_br(d):
    d.rectangle([0, 0, W, H], fill="#009C3B")
    m = H * 0.12
    d.polygon([(W / 2, m), (W - m * 1.2, H / 2), (W / 2, H - m), (m * 1.2, H / 2)], fill="#FFDF00")
    r = H * 0.25
    disc(d, W / 2, H / 2, r, "#002776")
    # The white band across the globe.
    d.arc([W / 2 - r * 2.2, H / 2 - r * 0.6, W / 2 + r * 1.2, H / 2 + r * 3.0], 235, 305,
          fill="#FFFFFF", width=int(SS * 1.6))


def flag_pt(d):
    vstripes(d, ["#006600", "#FF0000"], [2, 3])
    disc(d, W * 0.4, H / 2, H * 0.22, "#FFCC00")
    disc(d, W * 0.4, H / 2, H * 0.14, "#FF0000")


def flag_mx(d):
    vstripes(d, ["#006847", "#FFFFFF", "#CE1126"])
    disc(d, W / 2, H / 2, H * 0.12, "#8C6A34")


def flag_nl(d):
    hstripes(d, ["#AE1C28", "#FFFFFF", "#21468B"])


def flag_ru(d):
    hstripes(d, ["#FFFFFF", "#0039A6", "#D52B1E"])


def flag_se(d):
    d.rectangle([0, 0, W, H], fill="#006AA7")
    t = H * 0.2
    d.rectangle([W * 0.31, 0, W * 0.31 + t, H], fill="#FECC00")
    d.rectangle([0, H / 2 - t / 2, W, H / 2 + t / 2], fill="#FECC00")


def flag_jp(d):
    d.rectangle([0, 0, W, H], fill="#FFFFFF")
    disc(d, W / 2, H / 2, H * 0.3, "#BC002D")


def flag_tr(d):
    d.rectangle([0, 0, W, H], fill="#E30A17")
    disc(d, W * 0.40, H / 2, H * 0.25, "#FFFFFF")
    disc(d, W * 0.45, H / 2, H * 0.20, "#E30A17")
    star(d, W * 0.62, H / 2, H * 0.12, "#FFFFFF", rot=180)


def flag_cn(d):
    d.rectangle([0, 0, W, H], fill="#EE1C25")
    star(d, W * 0.17, H * 0.27, H * 0.15, "#FFFF00")
    for x, y in ((0.33, 0.12), (0.40, 0.22), (0.40, 0.37), (0.33, 0.47)):
        star(d, W * x, H * y, H * 0.05, "#FFFF00")


def flag_kr(d):
    d.rectangle([0, 0, W, H], fill="#FFFFFF")
    r = H * 0.25
    d.pieslice([W / 2 - r, H / 2 - r, W / 2 + r, H / 2 + r], 180, 360, fill="#CD2E3A")
    d.pieslice([W / 2 - r, H / 2 - r, W / 2 + r, H / 2 + r], 0, 180, fill="#0047A0")
    disc(d, W / 2 - r / 2, H / 2, r / 2, "#CD2E3A")
    disc(d, W / 2 + r / 2, H / 2, r / 2, "#0047A0")
    for cx, cy in ((0.2, 0.22), (0.8, 0.22), (0.2, 0.78), (0.8, 0.78)):
        for k in range(3):
            y = H * cy + (k - 1) * SS * 3
            d.line([(W * cx - SS * 5, y), (W * cx + SS * 5, y)], fill="#000000", width=SS * 2)


def flag_tw(d):
    d.rectangle([0, 0, W, H], fill="#FE0000")
    d.rectangle([0, 0, W / 2, H / 2], fill="#000095")
    star(d, W / 4, H / 4, H * 0.17, "#FFFFFF", points=12, inner=0.6)
    disc(d, W / 4, H / 4, H * 0.08, "#000095")
    disc(d, W / 4, H / 4, H * 0.065, "#FFFFFF")


FLAGS = [
    ("us", flag_us), ("ca", flag_ca), ("gb", flag_gb), ("de", flag_de), ("es", flag_es),
    ("fr", flag_fr), ("it", flag_it), ("pl", flag_pl), ("br", flag_br), ("pt", flag_pt),
    ("mx", flag_mx), ("nl", flag_nl), ("ru", flag_ru), ("se", flag_se), ("jp", flag_jp),
    ("tr", flag_tr), ("cn", flag_cn), ("kr", flag_kr), ("tw", flag_tw),
]


def render(fn):
    big = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    fn(ImageDraw.Draw(big))
    # Rounded corners, and a faint dark edge so white flags read on white.
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, H - 1], RADIUS * SS, fill=255)
    big.putalpha(mask)
    edge = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(edge).rounded_rectangle([0, 0, W - 1, H - 1], RADIUS * SS, outline=(0, 0, 0, 60), width=SS)
    big = Image.alpha_composite(big, edge)
    return big.resize((CELL_W, CELL_H), Image.BOX)


def main():
    assert len(FLAGS) <= COLS * ROWS
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))
    placed = []
    for i, (code, fn) in enumerate(FLAGS):
        x, y = (i % COLS) * CELL_W, (i // COLS) * CELL_H
        atlas.paste(render(fn), (x, y))
        placed.append((code, x, y))

    os.makedirs(os.path.dirname(OUT_PNG), exist_ok=True)
    atlas.save(OUT_PNG)
    buf = io.BytesIO()
    atlas.save(buf, format="PNG", optimize=True)
    png = buf.getvalue()

    out = [
        "// GENERATED by tools/make_flags.py - do not edit by hand; edit the script and",
        "// re-run it. Settings' flags beside each language, as one %dx%d RGBA PNG." % (ATLAS_W, ATLAS_H),
        "",
        "#ifndef FLAGS_H",
        "#define FLAGS_H",
        "",
        "#define FLAG_ATLAS_WIDTH  %d" % ATLAS_W,
        "#define FLAG_ATLAS_HEIGHT %d" % ATLAS_H,
        "#define FLAG_CELL_W       %d" % CELL_W,
        "#define FLAG_CELL_H       %d" % CELL_H,
        "#define FLAG_COUNT        %d" % len(placed),
        "",
        "// A language file's @flag, and where that flag's top-left sits in the atlas.",
        "static const struct { const char *code; int x, y; } kFlags[FLAG_COUNT] =",
        "{",
    ]
    out += ['    { "%s", %d, %d },' % p for p in placed]
    out += ["};", "", "static const unsigned int kFlagsPngSize = %d;" % len(png),
            "static const unsigned char kFlagsPng[%d] =" % len(png), "{"]
    for i in range(0, len(png), 20):
        out.append("    " + ",".join("0x%02X" % b for b in png[i:i + 20]) + ",")
    out += ["};", "", "#endif // FLAGS_H", ""]
    with open(OUT_HEADER, "w", newline="\n") as f:
        f.write("\n".join(out))
    print("wrote %s (%d flags, %d bytes of PNG) and %s" % (os.path.relpath(OUT_HEADER, ROOT), len(placed), len(png),
                                                         os.path.relpath(OUT_PNG, ROOT)))


if __name__ == "__main__":
    main()
