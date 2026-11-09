#!/usr/bin/env python3
"""Hand-drawn occupancy pips for renders 1440p and taller: the members of !GXUIHD.big.

  make-gxuihd-archive.py <out-dir>

Writes <out-dir>/GXUIHD_Pips.tga (128x32, 32-bit, uncompressed) and <out-dir>/GXUIHD.ini (the MappedImages
SCPPipFull@133 / SCPPipEmpty@133 = 7x16 and SCPPipFull@200 / SCPPipEmpty@200 = 10x24, and the ammo pips SCPAmmoFull|Empty@133 = 7x14 / @200 = 10x20). The engine picks them by the
render height and draws them 1:1 (Core/GameEngine/Include/GameClient/GXImageVariant.h); scripts/tooling/
build-overlay-generated.py packs them as Art\\Textures\\GXUIHD_Pips.tga and Data\\INI\\MappedImages\\TextureSize_512\\GXUIHD.ini.

GeneralsX @feature Claude 03/10/2026 (owner, 03/10/2026: option C - 7x16 - with option E's black rim at 1440p, D -
10x24 - at 4K; original pure blue / pure green).

THE ART IS OURS AND IS DRAWN HERE, NOT READ FROM AN ARCHIVE. Every pixel is in the grids below: hand-placed in the retail
pip's palette (SCGameUserInterface512_001.tga 504,9 and 497,9) - the full pip in the three greys the engine tints
(white 255, light 205, mid 129: the tint multiplies, so black stays black and grey becomes the team's blue or green), the
empty pip in retail's black outline and brown fill (115,99,87). Nothing EA shipped is copied or committed, and the
generator needs no game install, so every packaging box writes the same bytes and peers agree on assetID.

Stdlib only: the Windows packaging leg runs this with whatever python it finds.
"""
import os
import struct
import sys

# Palette (r, g, b). '.' is transparent.
PAL = {
    'W': (255, 255, 255),   # full pip: highlight (retail grey, tinted)
    'L': (205, 205, 205),   # full pip: body
    'M': (129, 129, 129),   # full pip: shade
    'K': (0, 0, 0),         # the rim (owner's pick E): black stays black under the tint, so the colours are untouched
    'X': (0, 0, 0),         # empty pip: outline
    'b': (115, 99, 87),     # empty pip: fill
    # Ammo pips (rocket pods, bombs): retail's own palette, drawn UNtinted - SCPAmmoFull's orange/yellow, SCPAmmoEmpty's grey-brown.
    'O': (247, 148, 29),    # ammo full: body
    'Y': (255, 242, 0),     # ammo full: centre line
    'P': (251, 175, 93),    # ammo full: base highlight
    'E': (115, 98, 88),     # ammo empty: body
    # Rank chevrons (SCVeter1..3): retail's own three bands - outer yellow, middle orange, inner black.
    'y': (249, 244, 0),
    'o': (238, 137, 0),
    'k': (0, 0, 0),
}

# name -> (grid). Each pair (full, empty) shares one size; the full pip is tinted by the engine, the empty one is not.
ART = {
    'SCPPipFull@133': """
...K...
..KWK..
.KLWLK.
KLLWLLK
KLLWLLK
KLLWLLK
KMLLLMK
KLMMMLK
KLLMLLK
KLLWLLK
KLLWLLK
KLLWLLK
KMLLLMK
.KMMMK.
..KMK..
...K...""",
    'SCPPipEmpty@133': """
.XXXXX.
XXXbXXX
XXbbbXX
XbbbbbX
XbbbbbX
XbbbbbX
XbbbbbX
XbbbbbX
XbbbbbX
XbbbbbX
XbbbbbX
XbbbbbX
XbbbbbX
XXbbbXX
XXXbXXX
.XXXXX.""",
    'SCPPipFull@200': """
....KK....
...KWWK...
..KLWWLK..
.KLLWWLLK.
.KLLWWLLK.
.KLLWWLLK.
.KLLWWLLK.
.KLLWWLLK.
.KMLLLLMK.
.KMMLLMMK.
.KLMMMMLK.
.KLLMMLLK.
.KLLWWLLK.
.KLLWWLLK.
.KLLWWLLK.
.KLLWWLLK.
.KLLWWLLK.
.KLLWWLLK.
.KMLLLLMK.
.KMMLLMMK.
..KMMMMK..
...KMMK...
....KK....
..........""",
    'SCPPipEmpty@200': """
..XXXXXX..
.XXXXXXXX.
XXXXbbXXXX
XXXbbbbXXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXbbbbbbXX
XXXbbbbXXX
XXXXbbXXXX
.XXXXXXXX.
..XXXXXX..""",
    'SCPAmmoFull@133': """
..KKK..
.KOYOK.
KOOYOOK
KOOYOOK
KKKKKKK
KOOYOOK
KOOYOOK
KOOYOOK
KOOYOOK
KOOYOOK
KOOYOOK
KOPYPOK
KPPYPPK
.KKKKK.""",
    'SCPAmmoEmpty@133': """
..KKK..
.KEEEK.
KEEEEEK
KEEEEEK
KKKKKKK
KEEEEEK
KEEEEEK
KEEEEEK
KEEEEEK
KEEEEEK
KEEEEEK
KEEEEEK
KEEEEEK
.KKKKK.""",
    'SCPAmmoFull@200': """
...KKKK...
..KOYYOK..
.KOOYYOOK.
KOOOYYOOOK
KOOOYYOOOK
KKKKKKKKKK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOOOYYOOOK
KOPPYYPPOK
.KPPYYPPK.
..KKKKKK..""",
    'SCPAmmoEmpty@200': """
...KKKK...
..KEEEEK..
.KEEEEEEK.
KEEEEEEEEK
KEEEEEEEEK
KKKKKKKKKK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
KEEEEEEEEK
.KEEEEEEK.
..KKKKKK..""",
}
SIZES = {'133': (7, 16), '200': (10, 24)}   # the scale x 100 against retail's 5x12 -> the art's size
AMMO_SIZES = {'133': (7, 14), '200': (10, 20)}   # retail's 5x10 at the same two scales
ATLAS_W, ATLAS_H = 256, 32
TEXTURE = 'GXUIHD_Pips.tga'


def chevrons():
    """Retail's chevron is three diagonal bands (type = (row - |col - centre|) // thickness: 0 yellow, 1 orange, 2 black),
    a second and third one stacked 3 bands lower and painted over the one above (SCVeter1 5x5, SCVeter2 5x8, SCVeter3 5x11
    in the atlas, checked pixel for pixel). The same rule at arm length n and band thickness b gives the larger pair:
    133% n=3 b=1 -> 7x6 / 7x9 / 7x12, 200% n=4 b=2 -> 9x10 / 9x16 / 9x22."""
    out = {}
    for pct, n, b in (('133', 3, 1), ('200', 4, 2)):
        for count in (1, 2, 3):
            w, h = 2 * n + 1, n + 3 * b + 3 * b * (count - 1)
            g = [['.'] * w for _ in range(h)]
            for c in range(count):
                for r in range(n + 3 * b):
                    for x in range(w):
                        d = abs(x - n)
                        t = (r - d) // b if r - d >= 0 else -1
                        if d <= n and 0 <= t <= 2:
                            g[r + 3 * b * c][x] = 'yok'[t]
            out['SCVeter%d@%s' % (count, pct)] = '\n'.join(''.join(row) for row in g)
    return out


ART.update(chevrons())


def grid_rows(name):
    rows = ART[name].strip('\n').split('\n')
    if name.startswith('SCVeter'):
        w, h = len(rows[0]), len(rows)
    else:
        w, h = (AMMO_SIZES if name.startswith('SCPAmmo') else SIZES)[name.split('@')[1]]
    assert len(rows) == h and all(len(r) == w for r in rows), '%s is not %dx%d' % (name, w, h)
    assert all(c == '.' or c in PAL for r in rows for c in r), '%s uses a colour outside the palette' % name
    ok = {'SCPPipFull': '.WLMK', 'SCPPipEmpty': '.Xb', 'SCPAmmoFull': '.KOYP', 'SCPAmmoEmpty': '.KE', 'SCVeter1': '.yok', 'SCVeter2': '.yok', 'SCVeter3': '.yok'}[name.split('@')[0]]
    assert all(c in ok for r in rows for c in r), '%s mixes full and empty colours' % name
    return rows


def build_atlas():
    """Pack the four sprites left to right, each in a cell with a 1-texel border; returns (rgba rows, coords)."""
    pix = [[(0, 0, 0, 0)] * ATLAS_W for _ in range(ATLAS_H)]
    coords = {}
    x = 0
    for name in ART:
        rows = grid_rows(name)
        w, h = len(rows[0]), len(rows)
        cw, ch = w + 2, h + 2
        assert x + cw <= ATLAS_W and ch <= ATLAS_H, 'atlas too small'
        cell = [[None] * cw for _ in range(ch)]
        for yy, r in enumerate(rows):
            for xx, c in enumerate(r):
                if c != '.':
                    cell[yy + 1][xx + 1] = PAL[c]
        # Colour bleed: a transparent texel takes its nearest opaque neighbour's colour (BFS), so bilinear filtering -
        # the zoom-out shrink draws below 1:1 - never pulls black into an edge. Alpha stays 0.
        frontier = [(yy, xx) for yy in range(ch) for xx in range(cw) if cell[yy][xx] is not None]
        opaque = set(frontier)
        while frontier:
            nxt = []
            for yy, xx in frontier:
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    ny, nx = yy + dy, xx + dx
                    if 0 <= ny < ch and 0 <= nx < cw and cell[ny][nx] is None:
                        cell[ny][nx] = cell[yy][xx]
                        nxt.append((ny, nx))
            frontier = nxt
        for yy in range(ch):
            for xx in range(cw):
                rgb = cell[yy][xx]
                pix[yy][x + xx] = rgb + ((255,) if (yy, xx) in opaque else (0,))
        coords[name] = (x + 1, 1, x + 1 + w, 1 + h)   # Left, Top, Right, Bottom (right/bottom exclusive, as retail)
        x += cw
    return pix, coords


def tga_bytes(pix):
    """Uncompressed 32-bit true-colour TGA, bottom-left origin, 8 alpha bits (descriptor 0x08) - BGRA rows bottom-up."""
    head = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, ATLAS_W, ATLAS_H, 32, 0x08)
    body = bytearray()
    for row in reversed(pix):
        for r, g, b, a in row:
            body += bytes((b, g, r, a))
    return head + bytes(body)


def ini_text(coords):
    out = ['; GeneralsX: hand-drawn occupancy pips for renders 1440p and taller (GXImageVariant.h).',
           '; Generated by scripts/build/make-gxuihd-archive.py - edit the grids there, not this file.', '']
    for name, (l, t, r, b) in coords.items():
        out += ['MappedImage %s' % name,
                '  Texture = %s' % TEXTURE,
                '  TextureWidth = %d' % ATLAS_W,
                '  TextureHeight = %d' % ATLAS_H,
                '  Coords = Left:%d Top:%d Right:%d Bottom:%d' % (l, t, r, b),
                '  Status = NONE',
                'End', '']
    return '\r\n'.join(out)


def main(argv):
    if len(argv) != 1:
        sys.stderr.write(__doc__.split('\n\n')[1] + '\n')
        return 2
    pix, coords = build_atlas()
    # A pair draws at one size (Drawable::drawContained sizes both pips by the empty one, as retail does).
    for pct in SIZES:
        f, e = coords['SCPPipFull@' + pct], coords['SCPPipEmpty@' + pct]
        assert (f[2] - f[0], f[3] - f[1]) == (e[2] - e[0], e[3] - e[1]) == SIZES[pct], pct
        f, e = coords['SCPAmmoFull@' + pct], coords['SCPAmmoEmpty@' + pct]
        assert (f[2] - f[0], f[3] - f[1]) == (e[2] - e[0], e[3] - e[1]) == AMMO_SIZES[pct], pct
    os.makedirs(argv[0], exist_ok=True)
    with open(os.path.join(argv[0], TEXTURE), 'wb') as f:
        f.write(tga_bytes(pix))
    with open(os.path.join(argv[0], 'GXUIHD.ini'), 'wb') as f:
        f.write(ini_text(coords).encode('ascii'))
    for name, c in coords.items():
        print('  %-16s %dx%d at %d,%d' % (name, c[2] - c[0], c[3] - c[1], c[0], c[1]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
