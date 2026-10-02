"""
Renders the controller button icons for the UI footer into one small atlas,
and writes it out as ButtonAtlas.h (the PNG bytes plus where each sprite sits)
so the art is compiled into the XEX with nothing extra to deploy.

    python tools/make_button_atlas.py

Re-run after changing anything below; commit the regenerated ButtonAtlas.h and
Media/ButtonAtlas.png together with this file.

Why offline, not at runtime: the look being matched - the glossy "jewel"
buttons the 360 dashboard and Aurora use - needs a bold letter composited into
shaded art, and rasterising text into a texture on the console means render
targets and EDRAM resolves. Here it is a few lines of PIL. Nothing in the XDK
can stand in for it: XUI's skin only carries blank coloured discs (marked
internal-use-only in its .rights file), and the redistributable Xbox font's
private-use glyphs are chat smileys, not buttons.

Each button is rendered at 8x and box-filtered down, so every edge is
antialiased; the atlas itself is 2x the size the footer draws at on a 720p
screen, so on the console bilinear filtering does a clean 2:1 reduction.

The letters are Selawik Bold (Media/Fonts/selawkb.ttf, SIL OFL 1.1 - see
Media/Fonts/OFL.txt). It is used only here, at build time; the console never
loads it. Images rendered with a font are not the font itself under the OFL,
so the atlas carries no licence obligations of its own.
"""
import io
import math
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "Media", "Fonts", "selawkb.ttf")
OUT_PNG = os.path.join(ROOT, "Media", "ButtonAtlas.png")
OUT_HEADER = os.path.join(ROOT, "src", "ButtonAtlas.h")

SS = 8              # supersampling factor
CELL = 64           # atlas cell, in final pixels
FACE_D = 52         # face button diameter; the rest of the cell is shadow margin
BUMPER_H = 42       # shoulder button height - a little shorter than a face button, as on the pad
ATLAS_W, ATLAS_H = 256, 128   # powers of two, so there's no question of NPOT support on the GPU

# Face colours, sampled from the dashboard's own prompts and nudged to hold up
# at small sizes. The letter is a deep shade of the same hue - dark green on
# green, dark red on red - which is what the console does, and reads better
# than a neutral grey because it stays related to the colour around it.
FACES = [
    ("A", (132, 190, 22)),
    ("B", (206, 36, 34)),
    ("X", (28, 118, 214)),
    ("Y", (238, 190, 18)),
]
BUMPER_BASE = (74, 80, 84)      # graphite, like the shoulder buttons themselves
BUMPER_TEXT = (232, 235, 236)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def scale(c, k):
    return tuple(min(255.0, v * k) for v in c)


def clamp01(v):
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


def smoothstep(e0, e1, v):
    t = clamp01((v - e0) / (e1 - e0))
    return t * t * (3 - 2 * t)


def over(dst, src_rgb, src_a):
    """Straight-alpha 'over' onto a straight-alpha (r, g, b, a) pixel."""
    dr, dg, db, da = dst
    oa = src_a + da * (1 - src_a)
    if oa <= 0:
        return (0.0, 0.0, 0.0, 0.0)
    k = da * (1 - src_a)
    return ((src_rgb[0] * src_a + dr * k) / oa,
            (src_rgb[1] * src_a + dg * k) / oa,
            (src_rgb[2] * src_a + db * k) / oa,
            oa)


def render_button(w, h, radius, half_len, base, letter, letter_rgb, engrave):
    """One button, at SS x resolution, as an RGBA image of w x h final pixels.

    The shape is a stadium: a disc when half_len is 0, a capsule otherwise.
    Everything is described by its signed distance, so both shapes share one
    set of shading rules.
    """
    W, H = w * SS, h * SS
    R = radius * SS
    L = half_len * SS
    cx, cy = W / 2.0, H / 2.0

    rim_w = R * 0.085                    # thin dark rim, as on the dashboard buttons
    body_r = R - rim_w
    rim_rgb = scale(base, 0.42)
    top_rgb = mix(base, (255, 255, 255), 0.28)
    bottom_rgb = scale(base, 0.82)
    glow_rgb = mix(base, (255, 255, 255), 0.22)

    def dist(px, py, ox=0.0, oy=0.0, sx=1.0, sy=1.0):
        dx = max(abs(px - cx - ox) - L, 0.0) / sx
        dy = (py - cy - oy) / sy
        return math.hypot(dx, dy)

    # The letter, as a coverage mask at full supersampled resolution, centred
    # on its ink rather than its advance box so it sits optically centred.
    # A callable draws a symbol into the mask instead (START's arrow).
    mask = Image.new("L", (W, H), 0)
    if callable(letter):
        letter(ImageDraw.Draw(mask), cx, cy, R)
    elif letter:
        target_cap = (R * 2) * (0.40 if L == 0 else 0.44)
        font = ImageFont.truetype(FONT, 100)
        bb = font.getbbox("H")
        size = int(round(100 * target_cap / (bb[3] - bb[1])))
        font = ImageFont.truetype(FONT, size)
        bb = font.getbbox(letter)
        tx = cx - (bb[0] + bb[2]) / 2.0
        ty = cy - (bb[1] + bb[3]) / 2.0
        ImageDraw.Draw(mask).text((tx, ty), letter, font=font, fill=255)
    mpx = mask.load()
    eo = int(round(0.9 * SS))            # engrave offset: a faint light edge below the letter

    img = Image.new("RGBA", (W, H))
    out = img.load()

    for y in range(H):
        py = y + 0.5
        for x in range(W):
            px = x + 0.5
            d = dist(px, py)
            p = (0.0, 0.0, 0.0, 0.0)

            # Soft drop shadow, slightly below: gives the button its lift.
            ds = dist(px, py, oy=R * 0.05)
            sa = 0.55 * (1.0 - smoothstep(R * 0.90, R * 1.16, ds))
            if sa > 0:
                p = over(p, (0, 0, 0), sa)

            if d <= R:
                # Rim.
                p = over(p, rim_rgb, 1.0)

                if d <= body_r:
                    # Body: light at the top, deeper at the bottom, darkening
                    # towards the edge so it reads as a curved surface.
                    t = clamp01((py - (cy - body_r)) / (2 * body_r))
                    c = mix(top_rgb, bottom_rgb, t)

                    # Light coming back up through the lower half - the glow
                    # that makes the dashboard buttons look like coloured glass.
                    g = 1.0 - smoothstep(0.0, body_r * 0.75, dist(px, py, oy=body_r * 0.62, sx=1.25))
                    c = mix(c, glow_rgb, 0.55 * g)

                    edge = (d / body_r) ** 4
                    c = scale(c, 1.0 - 0.22 * edge)
                    p = over(p, c, 1.0)

                    # Gloss: the crisp-edged highlight across the upper half.
                    # An ellipse pushed up so its lower edge crosses just below
                    # the middle; bright at the top, fading towards that edge.
                    gd = dist(px, py, oy=-body_r * 0.40, sx=0.90, sy=0.62)
                    if gd <= body_r * 0.97:
                        top = cy - body_r
                        bottom = cy + body_r * 0.20
                        u = clamp01((py - top) / (bottom - top))
                        ga = 0.55 - 0.42 * u
                        # antialias the gloss edge a little wider than 1px so it
                        # stays smooth after the 8x reduction
                        ga *= 1.0 - smoothstep(body_r * 0.93, body_r * 0.97, gd)
                        p = over(p, (255, 255, 255), ga)

                    # The letter, with a faint light edge offset below it.
                    if letter:
                        if engrave and y - eo >= 0:
                            e = mpx[x, y - eo] / 255.0
                            if e > 0:
                                p = over(p, (255, 255, 255), 0.22 * e)
                        m = mpx[x, y] / 255.0
                        if m > 0:
                            p = over(p, letter_rgb, m)

            if p[3] > 0:
                out[x, y] = (int(round(p[0])), int(round(p[1])), int(round(p[2])), int(round(p[3] * 255)))

    # Pillow premultiplies RGBA while resampling, so the reduction doesn't
    # bleed the transparent black margin into the edges.
    return img.resize((w, h), Image.BOX)


def main():
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))
    sprites = []   # (name, cellX, cellY, cellW, cellH, boxX, boxY, boxW, boxH)

    margin = (CELL - FACE_D) // 2
    for i, (letter, base) in enumerate(FACES):
        img = render_button(CELL, CELL, FACE_D / 2.0, 0.0, base, letter, scale(base, 0.30), True)
        atlas.alpha_composite(img, (i * CELL, 0))
        sprites.append((letter, i * CELL, 0, CELL, CELL, margin, margin, FACE_D, FACE_D))
        print("rendered", letter)

    # Shoulder buttons: two capsules, LB and RB, drawn as one sprite - the
    # footer only ever shows them as a pair ("page left / right").
    r = BUMPER_H / 2.0
    cap_w = 62
    gap = 4
    bm = (CELL - BUMPER_H) // 2           # same vertical margin logic as the faces
    sprite_w = bm + cap_w + gap + cap_w + bm
    x = 0
    for j, text in enumerate(("LB", "RB")):
        w = cap_w + 2 * bm
        img = render_button(w, CELL, r, (cap_w - BUMPER_H) / 2.0, BUMPER_BASE, text, BUMPER_TEXT, False)
        atlas.alpha_composite(img, (x, CELL))
        x += cap_w + gap
        print("rendered", text)
    sprites.append(("LBRB", 0, CELL, sprite_w, CELL, bm, bm, cap_w + gap + cap_w, BUMPER_H))
    assert sprite_w <= ATLAS_W

    # START: a small graphite disc with the pad's right-pointing arrow. As on
    # the controller it is smaller than the face buttons - the height of the
    # shoulder buttons, so it sits in the footer like them.
    def start_arrow(draw, cx, cy, R):
        h = R * 0.78                      # arrow height
        w = h * 0.80
        ox = w * 0.12                     # nudged right: a triangle's ink sits left of its box centre
        draw.polygon([(cx - w / 2 + ox, cy - h / 2), (cx + w / 2 + ox, cy), (cx - w / 2 + ox, cy + h / 2)], fill=255)

    start_x = 176
    assert start_x >= sprite_w and start_x + CELL <= ATLAS_W
    img = render_button(CELL, CELL, BUMPER_H / 2.0, 0.0, BUMPER_BASE, start_arrow, BUMPER_TEXT, False)
    atlas.alpha_composite(img, (start_x, CELL))
    sprites.append(("START", start_x, CELL, CELL, CELL, bm, bm, BUMPER_H, BUMPER_H))
    print("rendered START")

    atlas.save(OUT_PNG, optimize=True)
    png = open(OUT_PNG, "rb").read()

    lines = []
    lines.append("// GENERATED by tools/make_button_atlas.py - do not edit by hand; edit the")
    lines.append("// script and re-run it. See the script for how the art is made and why.")
    lines.append("//")
    lines.append("// The footer's controller button icons, as one %dx%d RGBA PNG compiled into" % (ATLAS_W, ATLAS_H))
    lines.append("// the XEX. Each sprite records its cell in the atlas and, inside that cell,")
    lines.append("// the box the button itself occupies - the rest is drop-shadow margin. Lay")
    lines.append("// out by the box; draw the whole cell.")
    lines.append("")
    lines.append("#ifndef BUTTON_ATLAS_H")
    lines.append("#define BUTTON_ATLAS_H")
    lines.append("")
    lines.append("#define BUTTON_ATLAS_WIDTH   %d" % ATLAS_W)
    lines.append("#define BUTTON_ATLAS_HEIGHT  %d" % ATLAS_H)
    lines.append("#define BUTTON_ATLAS_FACE_D  %d  // a face button's diameter in atlas pixels; the unit every sprite scales by" % FACE_D)
    lines.append("")
    lines.append("struct ButtonSprite")
    lines.append("{")
    lines.append("    int cellX, cellY, cellW, cellH;   // in the atlas")
    lines.append("    int boxX, boxY, boxW, boxH;       // the button itself, relative to the cell")
    lines.append("};")
    lines.append("")
    lines.append("enum ButtonSpriteId")
    lines.append("{")
    for s in sprites:
        lines.append("    BUTTON_SPRITE_%s," % s[0])
    lines.append("    BUTTON_SPRITE_COUNT")
    lines.append("};")
    lines.append("")
    lines.append("static const ButtonSprite kButtonSprites[BUTTON_SPRITE_COUNT] =")
    lines.append("{")
    for s in sprites:
        lines.append("    { %3d, %3d, %3d, %3d,  %2d, %2d, %3d, %2d },   // %s" % (s[1:] + (s[0],)))
    lines.append("};")
    lines.append("")
    lines.append("static const unsigned long kButtonAtlasPngSize = %d;" % len(png))
    lines.append("static const unsigned char kButtonAtlasPng[%d] =" % len(png))
    lines.append("{")
    for i in range(0, len(png), 16):
        lines.append("    " + ", ".join("0x%02X" % b for b in png[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    with open(OUT_HEADER, "w", newline="\r\n") as f:
        f.write("\n".join(lines))

    print("atlas: %d bytes of PNG -> %s" % (len(png), os.path.relpath(OUT_HEADER, ROOT)))


if __name__ == "__main__":
    main()
