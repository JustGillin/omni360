"""Generates StartupArt.h: the startup screen's disc and wordmark, baked.

The disc comes in three images rather than one, because they are layered
around the print that turns between them, and because the glare has to be
tinted on its own so it can bloom while the disc spins:

    face    the green circle
    (print, drawn by the console at whatever angle the spin is at)
    glare   two 35 degree wedges, white, at half alpha
    hub     the white clamping ring and the hole, with their outlines

The wordmark is OMNI360 in Lader - Medium for the letters, Black for the
digits - in two pieces, because PngDecode.cpp caps a side at 512 pixels and
the whole word is wider than that at this size. The disc's own print is the
same word, small.

Everything is baked at well over the size it is drawn at, so the console
only ever scales down.

Lader is NOT in this repository: it is a licensed font and the copies on the
machine this was first run on are trial versions. Put lader-medium.otf and
lader-black.otf in Media/Fonts to run this, or it falls back to an installed
copy and says so.

Run from the repo root:  python tools/make_startup_art.py
"""

import io
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTPUT = os.path.join(ROOT, 'src', 'StartupArt.h')

EM = 190          # the wordmark's em, in 1080p pixels
PRINT_EM = 58     # the disc's own print, baked at 4x what it is drawn at
R = 160           # the disc's baked radius
SS = 4            # supersampling for the disc, which is all curves
PAD = 2

# The settled proportions, as fractions of the radius.
HUB = 0.339
HOLE = 0.187
HUB_STROKE = 0.0434
HOLE_STROKE = 0.026
WEDGE_DEG = 35
WEDGE_AT = (-60, 120)   # upper right and lower left

FACE = (31, 160, 31, 255)      # #1FA01F, the app's green
HOLE_FILL = (26, 26, 26, 255)
INK = (0, 0, 0, 255)


def find_font(weight):
    local = os.path.join(ROOT, 'Media', 'Fonts', 'lader-%s.otf' % weight)
    if os.path.exists(local):
        return local, True
    installed = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Microsoft', 'Windows',
                             'Fonts', 'trial-lader-%s.otf' % weight)
    if os.path.exists(installed):
        return installed, False
    sys.exit('No Lader %s: put lader-%s.otf in Media/Fonts.' % (weight, weight))


def text_image(font, text, fill=(255, 255, 255)):
    """The text's ink on clear, plus where its pen and baseline sit in it."""
    left, top, right, bottom = font.getbbox(text, anchor='ls')
    w, h = right - left + PAD * 2, bottom - top + PAD * 2
    assert w <= 512 and h <= 512, '%r is %dx%d, over 512' % (text, w, h)
    pen_x, baseline_y = PAD - left, PAD - top
    mask = Image.new('L', (w, h), 0)
    ImageDraw.Draw(mask).text((pen_x, baseline_y), text, font=font, fill=255, anchor='ls')
    image = Image.new('RGBA', (w, h), fill + (0,))
    image.putalpha(mask)
    return image, pen_x, baseline_y


def disc_layers():
    """face, glare and hub, each 2R square with the disc centred."""
    n = 2 * R * SS
    mid = n / 2.0

    face = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    ImageDraw.Draw(face).ellipse([0, 0, n - 1, n - 1], fill=FACE)

    glare = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glare)
    for start in WEDGE_AT:
        # Full alpha: the console tints this down to the resting half, and
        # back up as the disc spins, which is the glare blooming with speed.
        gd.pieslice([0, 0, n - 1, n - 1], start, start + WEDGE_DEG, fill=(255, 255, 255, 255))

    hub = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hub)
    for radius, fill, stroke in ((HUB, (255, 255, 255, 255), HUB_STROKE),
                                 (HOLE, HOLE_FILL, HOLE_STROKE)):
        rr = radius * R * SS
        half = stroke * R * SS / 2.0
        hd.ellipse([mid - rr - half, mid - rr - half, mid + rr + half, mid + rr + half], fill=INK)
        hd.ellipse([mid - rr + half, mid - rr + half, mid + rr - half, mid + rr - half], fill=fill)

    size = (2 * R, 2 * R)
    return (face.resize(size, Image.LANCZOS),
            glare.resize(size, Image.LANCZOS),
            hub.resize(size, Image.LANCZOS))


def png_bytes(image):
    out = io.BytesIO()
    image.save(out, 'PNG', optimize=True)
    return out.getvalue()


def c_array(name, data):
    lines = ['static const unsigned char %s[] = {' % name]
    for i in range(0, len(data), 20):
        lines.append('    ' + ', '.join('0x%02X' % b for b in data[i:i + 20]) + ',')
    lines.append('};')
    lines.append('static const unsigned long %sSize = %d;' % (name, len(data)))
    return '\n'.join(lines)


def main():
    med_path, med_local = find_font('medium')
    blk_path, blk_local = find_font('black')
    if not (med_local and blk_local):
        print('NOTE: using the installed trial Lader. Licence it and put it in '
              'Media/Fonts before shipping anything built from this.')

    word_med = ImageFont.truetype(med_path, EM)
    word_blk = ImageFont.truetype(blk_path, EM)
    print_med = ImageFont.truetype(med_path, PRINT_EM)
    print_blk = ImageFont.truetype(blk_path, PRINT_EM)

    pieces, defines, arrays, report = [], [], [], []

    # The two wordmark pieces. OMNI is tracked out; 360 sits a little clear
    # of the I so the thin stem is not jammed against the heaviest form.
    omni, omni_pen, omni_base = text_image(word_med, 'OMNI')
    digits, dig_pen, dig_base = text_image(word_blk, '360')
    omni_adv = word_med.getlength('OMNI') + EM * 0.035 * 4
    gap = EM * 0.07
    word_total = omni_adv + gap + word_blk.getlength('360')

    # The disc's print, the same word at a size that reads on the face.
    p_omni, p_omni_pen, p_omni_base = text_image(print_med, 'OMNI', (228, 243, 228))
    p_digits, p_dig_pen, p_dig_base = text_image(print_blk, '360', (228, 243, 228))
    p_omni_adv = print_med.getlength('OMNI') + PRINT_EM * 0.035 * 4
    p_gap = PRINT_EM * 0.07
    print_total = p_omni_adv + p_gap + print_blk.getlength('360')

    face, glare, hub = disc_layers()

    for name, image in (('Face', face), ('Glare', glare), ('Hub', hub)):
        png = png_bytes(image)
        defines += ['#define STARTART_%s_W %d' % (name.upper(), image.size[0]),
                    '#define STARTART_%s_H %d' % (name.upper(), image.size[1]), '']
        arrays.append(c_array('kStartArt' + name + 'Png', png))
        report.append('%s %dx%d (%d B)' % (name, image.size[0], image.size[1], len(png)))

    for name, image, pen, base in (('Omni', omni, omni_pen, omni_base),
                                   ('Digits', digits, dig_pen, dig_base),
                                   ('PrintOmni', p_omni, p_omni_pen, p_omni_base),
                                   ('PrintDigits', p_digits, p_dig_pen, p_dig_base)):
        png = png_bytes(image)
        key = name.upper()
        defines += ['#define STARTART_%s_W        %d' % (key, image.size[0]),
                    '#define STARTART_%s_H        %d' % (key, image.size[1]),
                    '#define STARTART_%s_PEN_X    %d.0f' % (key, pen),
                    '#define STARTART_%s_BASELINE %d.0f' % (key, base), '']
        arrays.append(c_array('kStartArt' + name + 'Png', png))
        report.append('%s %dx%d (%d B)' % (name, image.size[0], image.size[1], len(png)))

    cap = word_med.getbbox('O', anchor='ls')[1] * -1
    header = '''// Generated by tools/make_startup_art.py - don't edit; run it again.
//
// The startup screen's disc and wordmark. The disc is three layers with the
// turning print drawn between face and glare; the wordmark is OMNI360 in
// Lader, Medium then Black, in two pieces. White or near-white with
// coverage as alpha, except the disc, which carries its own colour.
//
// A piece's _PEN_X and _BASELINE are in its own pixels, from its top-left.
// Everything is baked larger than it is drawn, so the console scales down.

#ifndef STARTUP_ART_H
#define STARTUP_ART_H

#define STARTART_EM        %d.0f  // the wordmark's em, in 1080p pixels
#define STARTART_CAP       %d.0f  // its cap height, which sets the disc
#define STARTART_OMNI_ADV  %.2ff  // OMNI's advance, tracking included
#define STARTART_GAP       %.2ff  // and the space before the digits
#define STARTART_WORD_W    %.2ff  // the whole wordmark, pen to pen
#define STARTART_PRINT_EM  %d.0f
#define STARTART_PRINT_ADV %.2ff
#define STARTART_PRINT_GAP %.2ff
#define STARTART_PRINT_W   %.2ff

// The disc, as fractions of its radius - see tools/make_startup_art.py.
#define STARTART_HUB       %.4ff
#define STARTART_HOLE      %.4ff
#define STARTART_PRINT_AT  0.63f  // where the print sits, out from the centre

%s
%s

#endif
''' % (EM, cap, omni_adv, gap, word_total, PRINT_EM, p_omni_adv, p_gap, print_total,
       HUB, HOLE, '\n'.join(defines), '\n\n'.join(arrays))

    with open(OUTPUT, 'w', newline='\r\n') as f:
        f.write(header)
    print(', '.join(report))
    print('-> %s' % os.path.relpath(OUTPUT, ROOT))


if __name__ == '__main__':
    main()
