"""
Renders Home's navigation icons into one small atlas and writes it out as
NavIcons.h (the PNG bytes plus where each icon sits), so the art is compiled
into the XEX with nothing extra to deploy.

    python tools/make_nav_icons.py

Re-run after changing anything below; commit the regenerated NavIcons.h and
Media/NavIcons.png together with this file.

Why offline, not at runtime: same reason as make_button_atlas.py - drawing
antialiased strokes on the console means render targets and EDRAM resolves,
where here it is a few lines of PIL. These are plain line icons, so unlike the
button atlas they carry no colour of their own: they are drawn white and the
UI tints them, which is how one atlas serves the focused and unfocused states.

Each icon is drawn at 8x into a 64px cell and box-filtered down, so every edge
is antialiased. The atlas is 2x the size Home draws at on a 720p screen, so
the console's bilinear filtering does a clean 2:1 reduction.

There are three styles, one atlas each, with the icons in the same places so
one table serves them all; the theme picks one as it picks the buttons:

    outline  thin outlines, the Series look (and Light's)
    filled   solid glyphs with their details cut out, for the Xbox 360 theme
    og       heavy strokes with a soft glow baked into the alpha, for the
             Original Xbox theme

The outline atlas is Media/NavIcons.png, unchanged; the others are
NavIcons-filled.png and NavIcons-og.png.
"""
import io
import math
import os

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PNG = os.path.join(ROOT, "Media", "NavIcons.png")
OUT_HEADER = os.path.join(ROOT, "src", "NavIcons.h")

SS = 8                        # supersampling factor
CELL = 64                     # atlas cell, in final pixels
COLS, ROWS = 4, 8             # 17 icons and room for more
ATLAS_W, ATLAS_H = CELL * COLS, CELL * ROWS   # 256x512, both powers of two (512 is the console's PNG limit)

STROKE = 4.0                  # line width in final pixels
INSET = 11.0                  # margin inside the cell, so the art is ~42px

STYLES = ("outline", "filled", "og")
WEIGHTS = {"outline": 1.0, "filled": 1.15, "og": 1.4}   # stroke width, times STROKE

# The style being drawn, set by render_cell: the helpers read it, so each icon
# is written once.
FILLED = False
WEIGHT = 1.0


def px(v):
    """A final-pixel measurement in supersampled pixels."""
    return v * SS


def box():
    """The drawing area inside one cell, supersampled: (x0, y0, x1, y1)."""
    return (px(INSET), px(INSET), px(CELL - INSET), px(CELL - INSET))


def wpx(w):
    """A stroke width in supersampled pixels, in the current style's weight."""
    return int(round(px(w * WEIGHT)))


def line(d, a, b, w=STROKE):
    d.line([a, b], fill=255, width=wpx(w), joint="curve")


def rrect(d, x0, y0, x1, y1, r, w=STROKE):
    """A rounded box: outlined, or solid in the filled style."""
    if FILLED:
        d.rounded_rectangle([x0, y0, x1, y1], radius=r, fill=255, outline=255, width=wpx(w))
    else:
        d.rounded_rectangle([x0, y0, x1, y1], radius=r, outline=255, width=wpx(w))


def ring(d, cx, cy, r, w=STROKE):
    """A circle: outlined, or solid in the filled style."""
    if FILLED:
        e = wpx(w) * 0.5
        d.ellipse([cx - r - e, cy - r - e, cx + r + e, cy + r + e], fill=255)
    else:
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=wpx(w))


def disc(d, cx, cy, r):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)


def path(d, pts, w=STROKE, closed=False):
    """A stroked path; a closed one is solid in the filled style."""
    if FILLED and closed:
        d.polygon(pts, fill=255)
    d.line(pts, fill=255, width=wpx(w), joint="curve")


# Details inside a shape: drawn in the outline style, cut out of the solid in
# the filled one.

def cut_line(d, a, b, w=STROKE):
    d.line([a, b], fill=0 if FILLED else 255, width=wpx(w), joint="curve")


def cut_disc(d, cx, cy, r):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=0 if FILLED else 255)


# --- the icons ------------------------------------------------------------
#
# Each takes a draw context over one supersampled cell. They are deliberately
# plain outlines at one weight: at 46 final pixels on a TV, anything more
# detailed turns to mush.


def icon_library(d):
    """A collection: four rounded tiles."""
    x0, y0, x1, y1 = box()
    gap = px(4.0)
    mx, my = (x0 + x1) * 0.5, (y0 + y1) * 0.5
    r = px(2.5)
    rrect(d, x0, y0, mx - gap * 0.5, my - gap * 0.5, r)
    rrect(d, mx + gap * 0.5, y0, x1, my - gap * 0.5, r)
    rrect(d, x0, my + gap * 0.5, mx - gap * 0.5, y1, r)
    rrect(d, mx + gap * 0.5, my + gap * 0.5, x1, y1, r)


def icon_apps(d):
    """An application window: a frame with a title bar."""
    x0, y0, x1, y1 = box()
    rrect(d, x0, y0, x1, y1, px(4.0))
    bar = y0 + (y1 - y0) * 0.30
    cut_line(d, (x0, bar), (x1, bar), STROKE * 0.85)


def icon_homebrew(d):
    """A joystick: a ball on a stick over a base."""
    x0, y0, x1, y1 = box()
    cx = (x0 + x1) * 0.5
    ballR = (x1 - x0) * 0.21
    ballY = y0 + ballR + px(1.0)
    ring(d, cx, ballY, ballR)
    baseY = y1 - px(10.0)
    line(d, (cx, ballY + ballR), (cx, baseY))
    half = (x1 - x0) * 0.46
    # A trapezoid base, closed - two bare diagonals meeting a line read as an
    # arrowhead instead.
    d.polygon([(cx - half, y1), (cx - half * 0.46, baseY), (cx + half * 0.46, baseY), (cx + half, y1)],
              fill=255 if FILLED else None, outline=255, width=wpx(STROKE))


def icon_emulators(d):
    """A chip: a square body with legs down each side."""
    x0, y0, x1, y1 = box()
    inset = (x1 - x0) * 0.17
    bx0, bx1 = x0 + inset, x1 - inset
    by0, by1 = y0 + px(3.0), y1 - px(3.0)
    rrect(d, bx0, by0, bx1, by1, px(3.0))
    if FILLED:
        inner = (bx1 - bx0) * 0.30
        d.rounded_rectangle([bx0 + inner, by0 + inner, bx1 - inner, by1 - inner], radius=px(1.5),
                            outline=0, width=wpx(STROKE * 0.7))
    for i in range(3):
        y = by0 + (by1 - by0) * (0.25 + 0.25 * i)
        line(d, (x0, y), (bx0, y), STROKE * 0.8)
        line(d, (bx1, y), (x1, y), STROKE * 0.8)


def icon_store(d):
    """A shopping bag: a body with a handle over it."""
    x0, y0, x1, y1 = box()
    top = y0 + (y1 - y0) * 0.33
    rrect(d, x0, top, x1, y1, px(3.5))
    # The handle, an arc rising out of the bag's mouth.
    w = (x1 - x0) * 0.46
    cx = (x0 + x1) * 0.5
    d.arc([cx - w * 0.5, top - w * 0.62, cx + w * 0.5, top + w * 0.38],
          start=180, end=360, fill=255, width=wpx(STROKE * 0.9))


def icon_queue(d):
    """A download: an arrow coming down onto a tray."""
    x0, y0, x1, y1 = box()
    cx = (x0 + x1) * 0.5
    trayY = y1 - px(2.0)
    tipY = y1 - (y1 - y0) * 0.34
    line(d, (cx, y0), (cx, tipY))
    head = (x1 - x0) * 0.26
    if FILLED:
        e = wpx(STROKE) * 0.5
        d.polygon([(cx - head - e, tipY - head), (cx + head + e, tipY - head), (cx, tipY + e)], fill=255)
    else:
        line(d, (cx - head, tipY - head), (cx, tipY))
        line(d, (cx + head, tipY - head), (cx, tipY))
    line(d, (x0, trayY), (x1, trayY))


def icon_settings(d):
    """A gear: a ring with teeth around it."""
    x0, y0, x1, y1 = box()
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5
    outer = (x1 - x0) * 0.5
    ringR = outer * 0.66
    # A thick ring whose own hole is the gear's; stubby teeth around it. Thin
    # spokes off a thin ring read as a sun instead.
    ring(d, cx, cy, ringR, STROKE * 1.25)
    toothW = px(STROKE * 1.9 * WEIGHT)
    for i in range(8):
        a = math.radians(i * 45.0)
        ca, sa = math.cos(a), math.sin(a)
        inner = ringR * 0.88
        d.line([(cx + ca * inner, cy + sa * inner), (cx + ca * outer, cy + sa * outer)],
               fill=255, width=int(round(toothW)))
    # Punch the hole back out: the thick ring and its teeth close it up, and a
    # solid gear would sit heavier than the outlines beside it.
    hole = ringR * 0.46
    d.ellipse([cx - hole, cy - hole, cx + hole, cy + hole], fill=0)


# --- list icons -----------------------------------------------------------
#
# Beside the rows of a list - the folder picker's drives and folders - so
# what each row is reads at a glance. Same weight and inset as the nav row's.


def icon_drive(d):
    """A hard drive: a slab with a light on its face."""
    x0, y0, x1, y1 = box()
    cy = (y0 + y1) * 0.5
    h = (y1 - y0) * 0.30
    rrect(d, x0, cy - h, x1, cy + h, px(4.0))
    cut_disc(d, x1 - px(9.0), cy, px(2.6))
    cut_line(d, (x0 + px(8.0), cy), (x0 + (x1 - x0) * 0.45, cy), STROKE * 0.8)


def icon_usb(d):
    """A USB stick: a body and its plug, standing up."""
    x0, y0, x1, y1 = box()
    cx = (x0 + x1) * 0.5
    bodyW = (x1 - x0) * 0.42
    plugW = bodyW * 0.62
    plugBottom = y0 + (y1 - y0) * 0.30
    rrect(d, cx - plugW * 0.5, y0, cx + plugW * 0.5, plugBottom, px(1.5), STROKE * 0.85)
    rrect(d, cx - bodyW * 0.5, plugBottom, cx + bodyW * 0.5, y1, px(4.0))
    if FILLED:
        cut_line(d, (cx - plugW * 0.5, plugBottom - px(1.0)), (cx + plugW * 0.5, plugBottom - px(1.0)), STROKE * 0.6)


def icon_folder(d):
    """A folder: a body with a tab on its top left."""
    x0, y0, x1, y1 = box()
    top = y0 + (y1 - y0) * 0.16
    tabTop = top - px(1.0)
    bodyTop = top + px(7.0)
    tabRight = x0 + (x1 - x0) * 0.42
    path(d, [(x0, y1 - px(4.0)), (x0, tabTop + px(3.0)), (x0 + px(3.0), tabTop), (tabRight - px(3.0), tabTop),
             (tabRight + px(3.0), bodyTop), (x1 - px(3.0), bodyTop), (x1, bodyTop + px(3.0)), (x1, y1 - px(4.0)),
             (x1 - px(4.0), y1), (x0 + px(4.0), y1), (x0, y1 - px(4.0))], closed=True)
    if FILLED:
        cut_line(d, (x0 + px(1.0), bodyTop + px(4.5)), (x1 - px(1.0), bodyTop + px(4.5)), STROKE * 0.6)


def icon_file(d):
    """A file: a page with its top right corner folded over."""
    x0, y0, x1, y1 = box()
    w = (x1 - x0) * 0.70
    left = (x0 + x1) * 0.5 - w * 0.5
    right = left + w
    fold = w * 0.34
    page = [(right - fold, y0), (left + px(3.0), y0), (left, y0 + px(3.0)), (left, y1 - px(3.0)),
            (left + px(3.0), y1), (right - px(3.0), y1), (right, y1 - px(3.0)), (right, y0 + fold),
            (right - fold, y0)]
    if FILLED:
        path(d, page, closed=True)
        d.line([(right - fold, y0 + px(2.0)), (right - fold, y0 + fold), (right - px(2.0), y0 + fold)],
               fill=0, width=wpx(STROKE * 0.6), joint="curve")
    else:
        path(d, page + [(right - fold, y0 + fold), (right, y0 + fold)])


def icon_disc(d):
    """A disc: a ring with the hole in its middle."""
    x0, y0, x1, y1 = box()
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5
    r = (x1 - x0) * 0.5
    if FILLED:
        disc(d, cx, cy, r)
        disc(d, cx, cy, r * 0.42)
        d.ellipse([cx - r * 0.42, cy - r * 0.42, cx + r * 0.42, cy + r * 0.42], fill=0)
        disc(d, cx, cy, r * 0.30)
        d.ellipse([cx - r * 0.14, cy - r * 0.14, cx + r * 0.14, cy + r * 0.14], fill=0)
    else:
        ring(d, cx, cy, r - px(STROKE * 0.5))
        ring(d, cx, cy, r * 0.28)


def icon_check(d):
    """A tick: this one."""
    x0, y0, x1, y1 = box()
    w = x1 - x0
    d.line([(x0 + w * 0.08, y0 + w * 0.52), (x0 + w * 0.38, y0 + w * 0.80), (x0 + w * 0.94, y0 + w * 0.20)],
           fill=255, width=wpx(STROKE * 1.15), joint="curve")


def icon_keyboard(d):
    """A keyboard: a frame with rows of keys and a space bar."""
    x0, y0, x1, y1 = box()
    cy = (y0 + y1) * 0.5
    h = (y1 - y0) * 0.34
    rrect(d, x0, cy - h, x1, cy + h, px(4.0))
    keyR = px(2.1)
    for row, y in enumerate((cy - h * 0.42, cy)):
        for i in range(5):
            x = x0 + (x1 - x0) * (0.18 + 0.16 * i)
            cut_disc(d, x, y, keyR)
    cut_line(d, (x0 + (x1 - x0) * 0.30, cy + h * 0.48), (x0 + (x1 - x0) * 0.70, cy + h * 0.48), STROKE * 0.8)


def icon_copy(d):
    """Copy: two pages, one over the other."""
    x0, y0, x1, y1 = box()
    w = (x1 - x0) * 0.62
    h = (y1 - y0) * 0.72
    off = (x1 - x0) * 0.20
    rrect(d, x0 + off, y0, x0 + off + w, y0 + h, px(4.0))
    # The front page, its own outline over a filled-out gap so the back one
    # reads as behind it.
    fx0, fy0 = x0 + off * 0.0 + px(1.0), y0 + off + px(2.0)
    d.rounded_rectangle([fx0 - px(STROKE), fy0 - px(STROKE), fx0 + w + px(STROKE), fy0 + h + px(STROKE)],
                        radius=px(5.0), fill=0)
    rrect(d, fx0, fy0, fx0 + w, fy0 + h, px(4.0))


def icon_move(d):
    """Move: an arrow out of a page, to the right."""
    x0, y0, x1, y1 = box()
    w = x1 - x0
    # The page's left side, open on the right where the arrow leaves.
    path(d, [(x0 + w * 0.45, y0 + w * 0.12), (x0 + px(3.0), y0 + w * 0.12), (x0, y0 + w * 0.12 + px(3.0)),
             (x0, y1 - w * 0.12 - px(3.0)), (x0 + px(3.0), y1 - w * 0.12), (x0 + w * 0.45, y1 - w * 0.12)])
    cy = (y0 + y1) * 0.5
    line(d, (x0 + w * 0.26, cy), (x1 - px(2.0), cy))
    head = w * 0.20
    path(d, [(x1 - head - px(2.0), cy - head), (x1 - px(2.0), cy), (x1 - head - px(2.0), cy + head)], closed=FILLED)


def icon_trash(d):
    """Delete: a bin with its lid."""
    x0, y0, x1, y1 = box()
    w = x1 - x0
    lidY = y0 + w * 0.18
    line(d, (x0 + w * 0.08, lidY), (x1 - w * 0.08, lidY))
    # The handle on the lid.
    path(d, [(x0 + w * 0.38, lidY), (x0 + w * 0.38, y0 + w * 0.04), (x0 + w * 0.62, y0 + w * 0.04),
             (x0 + w * 0.62, lidY)], STROKE * 0.85)
    # The body, a little narrower at the foot, with two ribs.
    path(d, [(x0 + w * 0.18, lidY + px(5.0)), (x0 + w * 0.25, y1 - px(3.0)), (x0 + w * 0.29, y1),
             (x1 - w * 0.29, y1), (x1 - w * 0.25, y1 - px(3.0)), (x1 - w * 0.18, lidY + px(5.0))], closed=FILLED)
    for t in (0.42, 0.58):
        cut_line(d, (x0 + w * t, lidY + w * 0.18), (x0 + w * t, y1 - w * 0.14), STROKE * 0.8)


def icon_star(d):
    """Favourites: a five-pointed star, outlined, its joins rounded."""
    x0, y0, x1, y1 = box()
    cx, cy = (x0 + x1) * 0.5, (y0 + y1) * 0.5 + px(1.5)
    outer = (x1 - x0) * 0.52
    inner = outer * 0.45
    pts = []
    for i in range(10):
        r = outer if i % 2 == 0 else inner
        a = -math.pi / 2 + i * math.pi / 5
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    if FILLED:
        d.polygon(pts, fill=255)
    d.line(pts + [pts[0], pts[1]], fill=255, width=wpx(STROKE), joint="curve")


ICONS = [
    ("LIBRARY", icon_library),
    ("APPS", icon_apps),
    ("HOMEBREW", icon_homebrew),
    ("EMULATORS", icon_emulators),
    ("STORE", icon_store),
    ("QUEUE", icon_queue),
    ("SETTINGS", icon_settings),
    ("DRIVE", icon_drive),
    ("USB", icon_usb),
    ("FOLDER", icon_folder),
    ("CHECK", icon_check),
    ("KEYBOARD", icon_keyboard),
    ("FILE", icon_file),
    ("DISC", icon_disc),
    ("COPY", icon_copy),
    ("MOVE", icon_move),
    ("TRASH", icon_trash),
    ("STAR", icon_star),  # added last, so the others keep their places
]


def render_cell(fn, style="outline"):
    """One icon, white with an alpha mask, at final size."""
    global FILLED, WEIGHT
    FILLED = style == "filled"
    WEIGHT = WEIGHTS[style]
    big = Image.new("L", (CELL * SS, CELL * SS), 0)
    fn(ImageDraw.Draw(big))
    small = big.resize((CELL, CELL), Image.LANCZOS)
    if style == "og":
        # The original dashboard's glow: a soft halo the same colour as the
        # icon, so the tint carries it too. Inside the cell's margin.
        halo = small.filter(ImageFilter.GaussianBlur(3.2)).point(lambda v: int(v * 0.85))
        small = ImageChops.lighter(small, halo)
    cell = Image.new("RGBA", (CELL, CELL), (255, 255, 255, 0))
    cell.putalpha(small)
    # White everywhere; the UI tints it.
    cell.paste((255, 255, 255), (0, 0, CELL, CELL), small)
    cell.putalpha(small)
    return cell


def png_path(style):
    return OUT_PNG if style == "outline" else OUT_PNG.replace(".png", "-%s.png" % style)


def build_atlas(style):
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (255, 255, 255, 0))
    placed = []
    for i, (name, fn) in enumerate(ICONS):
        cx = (i % COLS) * CELL
        cy = (i // COLS) * CELL
        atlas.paste(render_cell(fn, style), (cx, cy))
        placed.append((name, cx, cy))
    return atlas, placed


def main():
    os.makedirs(os.path.dirname(OUT_PNG), exist_ok=True)
    pngs = []
    for style in STYLES:
        atlas, placed = build_atlas(style)
        atlas.save(png_path(style))
        buf = io.BytesIO()
        atlas.save(buf, format="PNG", optimize=True)
        pngs.append((style, buf.getvalue()))

    lines = []
    lines.append("// GENERATED by tools/make_nav_icons.py - do not edit by hand; edit the")
    lines.append("// script and re-run it. See the script for how the art is made and why.")
    lines.append("//")
    lines.append("// Home's navigation icons, as %dx%d RGBA PNGs compiled into the XEX, one"
                 % (ATLAS_W, ATLAS_H))
    lines.append("// per style with every icon in the same place. Every icon is white with an")
    lines.append("// alpha mask, so the UI tints it: there is one copy, not one per state.")
    lines.append("")
    lines.append("#ifndef NAV_ICONS_H")
    lines.append("#define NAV_ICONS_H")
    lines.append("")
    lines.append("#define NAV_ICON_ATLAS_WIDTH  %d" % ATLAS_W)
    lines.append("#define NAV_ICON_ATLAS_HEIGHT %d" % ATLAS_H)
    lines.append("#define NAV_ICON_CELL         %d  // one icon's cell, square, in atlas pixels" % CELL)
    lines.append("")
    lines.append("enum NavIconStyle")
    lines.append("{")
    for style, _ in pngs:
        lines.append("    NAV_ICON_STYLE_%s," % style.upper())
    lines.append("    NAV_ICON_STYLE_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("enum NavIconId")
    lines.append("{")
    for name, _, _ in placed:
        lines.append("    NAV_ICON_%s," % name)
    lines.append("    NAV_ICON_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("// Each icon's top-left corner in the atlas; every cell is NAV_ICON_CELL square.")
    lines.append("static const int kNavIconXY[NAV_ICON_COUNT][2] =")
    lines.append("{")
    for name, cx, cy in placed:
        lines.append("    { %3d, %3d },   // %s" % (cx, cy, name))
    lines.append("};")
    lines.append("")
    for style, png in pngs:
        lines.append("static const unsigned char kNavIconsPng_%s[%d] =" % (style, len(png)))
        lines.append("{")
        for i in range(0, len(png), 16):
            lines.append("    " + ", ".join("0x%02X" % b for b in png[i:i + 16]) + ",")
        lines.append("};")
        lines.append("")
    lines.append("static const unsigned char *const kNavIconsPngs[NAV_ICON_STYLE_COUNT] =")
    lines.append("{")
    for style, _ in pngs:
        lines.append("    kNavIconsPng_%s," % style)
    lines.append("};")
    lines.append("static const unsigned long kNavIconsPngSizes[NAV_ICON_STYLE_COUNT] =")
    lines.append("{")
    for style, png in pngs:
        lines.append("    %d," % len(png))
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    with open(OUT_HEADER, "w", newline="\r\n") as f:
        f.write("\n".join(lines))

    print("nav icons: %s -> %s" % (", ".join("%s %d bytes" % (st, len(p)) for st, p in pngs),
                                    os.path.relpath(OUT_HEADER, ROOT)))


if __name__ == "__main__":
    main()
