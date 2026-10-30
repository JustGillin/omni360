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
"""
import io
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PNG = os.path.join(ROOT, "Media", "NavIcons.png")
OUT_HEADER = os.path.join(ROOT, "src", "NavIcons.h")

SS = 8                        # supersampling factor
CELL = 64                     # atlas cell, in final pixels
COLS, ROWS = 4, 2             # 7 icons and a spare
ATLAS_W, ATLAS_H = CELL * COLS, CELL * ROWS   # 256x128, both powers of two

STROKE = 4.0                  # line width in final pixels
INSET = 11.0                  # margin inside the cell, so the art is ~42px


def px(v):
    """A final-pixel measurement in supersampled pixels."""
    return v * SS


def box():
    """The drawing area inside one cell, supersampled: (x0, y0, x1, y1)."""
    return (px(INSET), px(INSET), px(CELL - INSET), px(CELL - INSET))


def line(d, a, b, w=STROKE):
    d.line([a, b], fill=255, width=int(round(px(w))), joint="curve")


def rrect(d, x0, y0, x1, y1, r, w=STROKE):
    d.rounded_rectangle([x0, y0, x1, y1], radius=r, outline=255, width=int(round(px(w))))


def ring(d, cx, cy, r, w=STROKE):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=255, width=int(round(px(w))))


def disc(d, cx, cy, r):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=255)


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
    line(d, (x0, bar), (x1, bar), STROKE * 0.85)


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
              outline=255, width=int(round(px(STROKE))))


def icon_emulators(d):
    """A chip: a square body with legs down each side."""
    x0, y0, x1, y1 = box()
    inset = (x1 - x0) * 0.17
    bx0, bx1 = x0 + inset, x1 - inset
    by0, by1 = y0 + px(3.0), y1 - px(3.0)
    rrect(d, bx0, by0, bx1, by1, px(3.0))
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
          start=180, end=360, fill=255, width=int(round(px(STROKE * 0.9))))


def icon_queue(d):
    """A download: an arrow coming down onto a tray."""
    x0, y0, x1, y1 = box()
    cx = (x0 + x1) * 0.5
    trayY = y1 - px(2.0)
    tipY = y1 - (y1 - y0) * 0.34
    line(d, (cx, y0), (cx, tipY))
    head = (x1 - x0) * 0.26
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
    toothW = px(STROKE * 1.9)
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


ICONS = [
    ("LIBRARY", icon_library),
    ("APPS", icon_apps),
    ("HOMEBREW", icon_homebrew),
    ("EMULATORS", icon_emulators),
    ("STORE", icon_store),
    ("QUEUE", icon_queue),
    ("SETTINGS", icon_settings),
]


def render_cell(fn):
    """One icon, white with an alpha mask, at final size."""
    big = Image.new("L", (CELL * SS, CELL * SS), 0)
    fn(ImageDraw.Draw(big))
    small = big.resize((CELL, CELL), Image.LANCZOS)
    cell = Image.new("RGBA", (CELL, CELL), (255, 255, 255, 0))
    cell.putalpha(small)
    # White everywhere; the UI tints it.
    cell.paste((255, 255, 255), (0, 0, CELL, CELL), small)
    cell.putalpha(small)
    return cell


def main():
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (255, 255, 255, 0))
    placed = []
    for i, (name, fn) in enumerate(ICONS):
        cx = (i % COLS) * CELL
        cy = (i // COLS) * CELL
        atlas.paste(render_cell(fn), (cx, cy))
        placed.append((name, cx, cy))

    os.makedirs(os.path.dirname(OUT_PNG), exist_ok=True)
    atlas.save(OUT_PNG)

    buf = io.BytesIO()
    atlas.save(buf, format="PNG", optimize=True)
    png = buf.getvalue()

    lines = []
    lines.append("// GENERATED by tools/make_nav_icons.py - do not edit by hand; edit the")
    lines.append("// script and re-run it. See the script for how the art is made and why.")
    lines.append("//")
    lines.append("// Home's navigation icons, as one %dx%d RGBA PNG compiled into the XEX."
                 % (ATLAS_W, ATLAS_H))
    lines.append("// Every icon is white with an alpha mask, so the UI tints it: there is one")
    lines.append("// copy, not one per state.")
    lines.append("")
    lines.append("#ifndef NAV_ICONS_H")
    lines.append("#define NAV_ICONS_H")
    lines.append("")
    lines.append("#define NAV_ICON_ATLAS_WIDTH  %d" % ATLAS_W)
    lines.append("#define NAV_ICON_ATLAS_HEIGHT %d" % ATLAS_H)
    lines.append("#define NAV_ICON_CELL         %d  // one icon's cell, square, in atlas pixels" % CELL)
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
    lines.append("static const unsigned long kNavIconsPngSize = %d;" % len(png))
    lines.append("static const unsigned char kNavIconsPng[%d] =" % len(png))
    lines.append("{")
    for i in range(0, len(png), 16):
        lines.append("    " + ", ".join("0x%02X" % b for b in png[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    with open(OUT_HEADER, "w", newline="\r\n") as f:
        f.write("\n".join(lines))

    print("nav icons: %d bytes of PNG -> %s" % (len(png), os.path.relpath(OUT_HEADER, ROOT)))


if __name__ == "__main__":
    main()
