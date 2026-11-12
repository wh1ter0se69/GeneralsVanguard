#!/usr/bin/env python3
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""GeneralsX look for the control bar's side buttons and the General's experience ladder: the members of !GXUISide.big.

  make-gxsidebar-archive.py <out-dir>

Writes <out-dir>/GXSidebar.tga (2048x1024, 32-bit, uncompressed) and <out-dir>/zzGXSidebar.ini (128 button MappedImages, six
ladder-frame names and three ladder-fill tiles, all REDEFINED, none new).
scripts/tooling/build-overlay-generated.py packs them as Art\\Textures\\GXSidebar.tga and
Data\\INI\\MappedImages\\HandCreated\\zzGXSidebar.ini.

WHAT IT REPLACES (the owner, 08/10/2026, two screenshots: "this and this need to be rebranded with our new stuff for GeneralsX UI"):
  * the small buttons left of the command bar - Options (two windows), Idle Worker (the spanner), Chat, Beacon - and the wide
    General's-powers star button: retail SA/SN/SU<Kind>[H|P|I] (America / China / GLA, the green bevelled art), Control Bar Pro
    <Faction>Pro<Kind>[H|P|I] and ObserverPro<Kind>[H|P|I];
  * the General's experience ladder: the frame (SA/SN/SUExpBar for retail, <Faction>ProExpBar for Pro - two layouts, two sprites) and
    its fill tiles (GenExpBar1/Top1/Bottom1).
It draws them in the player HUD's own palette (GXSpectatorHud.inl: navy glass, the steel-blue frame, one blue accent, SQUARE corners)
so the in-game bar and the HUD read as one UI.

WHY REDEFINE AND NOT ADD. INI::parseMappedImageDefinition updates an image of the same name in place, and ImageCollection::load parses
TextureSize_<n>\\ first and HandCreated\\ second, files in sorted order, so a HandCreated\\zz... file is parsed after retail's SAControlBar512.ini
and after Control Bar Pro's *PowersPro.ini and wins both. Nothing is added to the name table (a new name key is a desync hazard,
memory namekey-minting-is-a-desync-hazard); MappedImages are outside the dataID fingerprint (UI_SHARPNESS_PLAN section on !GXUIHD).
Every sprite keeps its ORIGINAL aspect, so the scheme's UL/LR placement and the exp-bar geometry are untouched.

THE ART IS OURS AND IS DRAWN HERE (signed-distance shapes, anti-aliased), not read from an archive; no game install is needed and
every packaging box writes the same bytes. Stdlib only.
"""
import math
import os
import struct
import sys

ATLAS_W, ATLAS_H = 2048, 1024
TEXTURE = 'GXSidebar.tga'
GUTTER = 2

# --- the HUD palette (GXSpectatorHud.inl:64-78), 0..255 -------------------------------------------------------------
NAVY, NAVY_DEEP = (10, 26, 54), (4, 13, 30)
NAVY_HOT = (20, 48, 92)
STEEL, STEEL_DIM = (118, 158, 214), (64, 92, 134)
EDGE = (0, 0, 0)
ACCENT, WHITE, LABEL, DIM = (111, 168, 240), (238, 243, 250), (186, 210, 244), (140, 156, 180)
AMBER = (240, 192, 82)                 # the General's star medallion only

# --- what to draw: (name prefix, kinds with their ORIGINAL pixel size, draw scale) ----------------------------------------
# Original sizes are read off the retail / Control Bar Pro Coords; the scale only sets how many texels back the sprite.
KINDS = ('Options', 'Worker', 'Chat', 'Beacon', 'General')
STATES = ('', 'H', 'P', 'I')
FAMILIES = {
    # key: (original (w, h) of the four small buttons or None, of the General button or None, texel scale,
    #       prefixes of the small names, prefixes of the General names)
    # Retail has ONE small-button art per side (SA America, SN China, SU GLA) at the same 34x24 (SNOptionsP is 34x26 and the SUWorker* set
    # 34x25: a 2-8% stretch nobody can see), so the SA sprites serve all three; the General button differs per side (80x31, 58x31, 65x29).
    'SA': ((34, 24), (80, 31), 3, ('SA', 'SN', 'SU'), ('SA',)),
    'SN': (None, (58, 31), 3, (), ('SN',)),
    'SU': (None, (65, 29), 3, (), ('SU',)),
    # ObserverPro is 90x60 (1.50) against 108x70 (1.54): it shares the Pro sprites, a 3% stretch nobody can see, and saves 8 sprites.
    'Pro': ((108, 70), (178, 62), 2, ('AmericaPro', 'ChinaPro', 'GlaPro', 'ObserverPro'), ('AmericaPro', 'ChinaPro', 'GlaPro')),
}
OBS_KINDS = ('Options', 'Chat')                # the only kinds ObserverPro has
# Names the <prefix><kind><state> pattern makes that retail uses for something ELSE and no ControlBarScheme binds. SUWorker is the
# GLA Worker's portrait and build cameo (Command_ConstructGLAWorker's ButtonImage, the Community Patch Worker objects): redefining
# it put the spanner on every GLA Worker cameo, inside the cameo border. GLA's Idle Worker button is
# SUWorkerE/H/P/I, which stay redefined.
NOT_SIDEBAR = ('SUWorker',)
LADDER_PRO_NAMES = ('AmericaProExpBar', 'ChinaProExpBar', 'GlaProExpBar')
LADDER_RETAIL_NAMES = ('SAExpBar', 'SNExpBar', 'SUExpBar')
LADDER_SIZE = (108, 552)                    # Pro: 72x368 at 1.5x
LADDER_RETAIL_SIZE = (84, 428)              # retail: 21x107 at 4x (both are the same 0.196 aspect)
FILL_TILES = {'GenExpBar1': (7, 2), 'GenExpBarTop1': (7, 3), 'GenExpBarBottom1': (7, 3)}   # the fill: 1:1 texels like retail


# --- tiny SDF toolkit (distances in sprite pixels, negative inside) ---------------------------------------------------
def clamp01(v):
    return 0.0 if v < 0 else 1.0 if v > 1 else v


def sd_box(px, py, cx, cy, hw, hh):
    qx, qy = abs(px - cx) - hw, abs(py - cy) - hh
    return math.hypot(max(qx, 0.0), max(qy, 0.0)) + min(max(qx, qy), 0.0)


def sd_seg(px, py, ax, ay, bx, by):
    pax, pay, bax, bay = px - ax, py - ay, bx - ax, by - ay
    h = clamp01((pax * bax + pay * bay) / (bax * bax + bay * bay))
    return math.hypot(pax - bax * h, pay - bay * h)


def sd_poly(px, py, pts):
    d = (px - pts[0][0]) ** 2 + (py - pts[0][1]) ** 2
    s = 1.0
    j = len(pts) - 1
    for i in range(len(pts)):
        ex, ey = pts[j][0] - pts[i][0], pts[j][1] - pts[i][1]
        wx, wy = px - pts[i][0], py - pts[i][1]
        h = clamp01((wx * ex + wy * ey) / (ex * ex + ey * ey))
        bx, by = wx - ex * h, wy - ey * h
        d = min(d, bx * bx + by * by)
        c = (py >= pts[i][1], py < pts[j][1], ex * wy > ey * wx)
        if all(c) or not any(c):
            s = -s
        j = i
    return s * math.sqrt(d)


def star_pts(cx, cy, r):
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        rr = r if i % 2 == 0 else r * 0.40
        pts.append((cx + rr * math.cos(a), cy + rr * math.sin(a)))
    return pts


class Canvas:
    """A premultiplied RGBA float canvas that shapes are composited onto, bbox-limited so a glyph costs only its own pixels."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.p = [[(0.0, 0.0, 0.0, 0.0)] * w for _ in range(h)]

    def blend(self, x, y, rgb, a):
        if a <= 0.0:
            return
        r, g, b, al = self.p[y][x]
        k = 1.0 - a
        self.p[y][x] = (rgb[0] / 255.0 * a + r * k, rgb[1] / 255.0 * a + g * k, rgb[2] / 255.0 * a + b * k, a + al * k)

    def shape(self, sdf, bbox, rgb, alpha=1.0, strict=False):
        x0, y0, x1, y1 = bbox
        ya, yb, xa, xb = max(0, int(y0) - 1), min(self.h, int(y1) + 2), max(0, int(x0) - 1), min(self.w, int(x1) + 2)
        for y in range(ya, yb):
            for x in range(xa, xb):
                c = clamp01(0.5 - sdf(x + 0.5, y + 0.5))
                if c > 0.0:
                    self.blend(x, y, rgb, c * alpha)
        if strict:                                       # the raster WINDOW above, not the sprite edge: no pixel outside it may carry ink (a clipped stroke is silent otherwise)
            for y in range(self.h):
                for x in range(self.w):
                    if not (ya <= y < yb and xa <= x < xb):
                        assert clamp01(0.5 - sdf(x + 0.5, y + 0.5)) <= 0.0, 'shape clipped by its raster window at (%d,%d)' % (x, y)

    def plate(self, fill, fill2, fill_a, frame, frame_a, edge_a=190):
        """The HUD's plate(): a dark outer line, the frame line, navy glass (top lighter) - one shape: square."""
        w, h = self.w, self.h
        m = min(w, h)
        e = max(1.0, m / 44.0)               # outer dark line
        f = max(1.5, m / 34.0)               # frame line
        for y in range(h):
            t = (y + 0.5) / h
            col = tuple(fill[i] + (fill2[i] - fill[i]) * t for i in range(3))
            for x in range(w):
                d = min(x + 0.5, w - x - 0.5, y + 0.5, h - y - 0.5)      # inward distance to the square boundary
                if d < e - 0.5:
                    self.blend(x, y, EDGE, edge_a / 255.0)
                    continue
                cf = clamp01(d - e + 0.5)                                # frame coverage ramps in over the edge line
                if d < e + f + 0.5:
                    self.blend(x, y, EDGE, edge_a / 255.0)
                    self.blend(x, y, frame, cf * frame_a / 255.0)
                    ci = clamp01(d - e - f + 0.5)                        # glass takes over where the frame ends
                    self.blend(x, y, col, ci * fill_a / 255.0)
                else:
                    self.blend(x, y, col, fill_a / 255.0)

    def rows(self):
        out = []
        for row in self.p:
            r = []
            for (pr, pg, pb, pa) in row:
                if pa <= 1e-6:
                    r.append((0, 0, 0, 0))
                else:
                    r.append((min(255, int(pr / pa * 255 + 0.5)), min(255, int(pg / pa * 255 + 0.5)),
                              min(255, int(pb / pa * 255 + 0.5)), min(255, int(pa * 255 + 0.5))))
            out.append(r)
        return out


# --- glyphs: all in units of g = half the sprite height, centred on (cx, cy) ---------------------------------------------
def glyph(cv, kind, state, W, H):
    g = H / 2.0
    cx, cy = W / 2.0, H / 2.0 + (0.035 * g if state == 'P' else 0.0)   # pressed: the glyph sinks a hair
    col = {'': LABEL, 'H': WHITE, 'P': ACCENT, 'I': DIM}[state]
    ga = 0.42 if state == 'I' else 1.0
    t = 0.09 * g

    def ring(sdf_fn):
        return lambda x, y: abs(sdf_fn(x, y)) - t / 2.0

    if kind == 'Options':                                   # two windows, the retail Diplomacy/Options idiom
        bx, by, hw, hh = cx + 0.22 * g, cy - 0.20 * g, 0.50 * g, 0.30 * g
        fx, fy = cx - 0.22 * g, cy + 0.18 * g
        # the back window is cut where the front one (plus a gap) covers it, so no filler box is needed
        cv.shape(lambda x, y: max(abs(sd_box(x, y, bx, by, hw, hh)) - t / 2.0, -(sd_box(x, y, fx, fy, hw + t * 1.4, hh + t * 1.4))),
                 (bx - hw - t, by - hh - t, bx + hw + t, by + hh + t), col, ga)
        cv.shape(ring(lambda x, y: sd_box(x, y, fx, fy, hw, hh)), (fx - hw - t, fy - hh - t, fx + hw + t, fy + hh + t), col, ga)
    elif kind == 'Worker':                                  # the spanner (retail's double-ended bone, drawn as one)
        hx = 0.60 * g

        def spanner(x, y):
            d = sd_box(x, y, cx, cy, 0.62 * g, 0.105 * g)
            d = min(d, sd_box(x, y, cx - hx, cy, 0.22 * g, 0.34 * g), sd_box(x, y, cx + hx, cy, 0.22 * g, 0.34 * g))
            n = min(sd_box(x, y, cx - hx - 0.17 * g, cy, 0.17 * g, 0.12 * g), sd_box(x, y, cx + hx + 0.17 * g, cy, 0.17 * g, 0.12 * g))
            return max(d, -n)
        cv.shape(spanner, (cx - 0.9 * g, cy - 0.4 * g, cx + 0.9 * g, cy + 0.4 * g), col, ga)
    elif kind == 'Chat':                                    # a speech bubble with three lines of text
        bx, by, hw, hh = cx, cy - 0.10 * g, 0.64 * g, 0.40 * g
        cv.shape(ring(lambda x, y: sd_box(x, y, bx, by, hw, hh)), (bx - hw - t, by - hh - t, bx + hw + t, by + hh + t), col, ga)
        tail = [(cx - 0.40 * g, cy + 0.26 * g), (cx - 0.12 * g, cy + 0.26 * g), (cx - 0.40 * g, cy + 0.56 * g)]
        cv.shape(lambda x, y: sd_poly(x, y, tail), (cx - 0.42 * g, cy + 0.24 * g, cx - 0.1 * g, cy + 0.58 * g), col, ga)
        for i, (ly, lw) in enumerate(((-0.28, 0.42), (-0.10, 0.42), (0.08, 0.26))):
            lx0 = cx - 0.44 * g
            cv.shape(lambda x, y, ly=ly, lw=lw: sd_box(x, y, lx0 + lw * g, cy + ly * g, lw * g, 0.045 * g),
                     (lx0, cy + ly * g - 0.1 * g, lx0 + 2 * lw * g, cy + ly * g + 0.1 * g), col, ga)
    elif kind == 'Beacon':                                  # retail's "!" in a square
        hs = 0.50 * g
        cv.shape(ring(lambda x, y: sd_box(x, y, cx, cy, hs, hs)), (cx - hs - t, cy - hs - t, cx + hs + t, cy + hs + t), col, ga)
        cv.shape(lambda x, y: sd_box(x, y, cx, cy - 0.12 * g, 0.075 * g, 0.20 * g), (cx - 0.1 * g, cy - 0.35 * g, cx + 0.1 * g, cy + 0.1 * g), col, ga)
        cv.shape(lambda x, y: sd_box(x, y, cx, cy + 0.27 * g, 0.085 * g, 0.085 * g), (cx - 0.11 * g, cy + 0.17 * g, cx + 0.11 * g, cy + 0.37 * g), col, ga)
    elif kind == 'General':                                 # three stars: hollow at rest, filled when lit. The engine lights H both on hover AND as the 'points to spend' flash frame, so H must
        # not say more than 'lit' - an amber H would read as 'promote me' on every mouse-over
        u = min(H, 0.414 * W)                            # the narrow retail buttons (China 58x31, GLA 65x29) are not wide enough for height-sized stars
        r_c, r_s, dx = 0.43 * u, 0.31 * u, 0.80 * u
        assert dx + r_s <= W / 2.0 - 2, 'General stars clip at %dx%d' % (W, H)
        stars = [(cx - dx, cy + 0.05 * H, r_s), (cx, cy + 0.02 * H, r_c), (cx + dx, cy + 0.05 * H, r_s)]
        sw = 0.045 * H / 2.0                              # the hollow stars' stroke reaches sw past the polygon
        for sx, sy, sr in stars:
            pts = star_pts(sx, sy, sr)
            if state in ('H', 'P'):
                cv.shape(lambda x, y, pts=pts: sd_poly(x, y, pts), (sx - sr, sy - sr, sx + sr, sy + sr),
                         WHITE if state == 'H' else ACCENT, 1.0, strict=True)
            else:
                q = sr + sw + 1.5
                cv.shape(lambda x, y, pts=pts: abs(sd_poly(x, y, pts)) - sw, (sx - q, sy - q, sx + q, sy + q),
                         DIM if state == 'I' else LABEL, 0.45 if state == 'I' else 0.85, strict=True)


def draw_button(kind, state, W, H):
    cv = Canvas(W, H)
    if state == '':
        cv.plate(NAVY, (14, 36, 72), 228, STEEL, 235)
    elif state == 'H':
        cv.plate(NAVY_HOT, (28, 64, 118), 238, ACCENT, 255)
        if kind != 'General':
            cv.shape(lambda x, y: sd_box(x, y, W / 2.0, H * 0.12, W / 2.0 - H / 8.0, 0.5), (0, 0, W, H * 0.25), WHITE, 0.16)
    elif state == 'P':
        cv.plate(NAVY_DEEP, (8, 22, 46), 244, ACCENT, 255)
    else:
        cv.plate(NAVY, (12, 30, 60), 170, STEEL_DIM, 150, 120)
    glyph(cv, kind, state, W, H)
    return cv.rows()


# --- the ladder frame: star ring on top, the track below it where the engine's fill shows through -----------------------
# Fraction of the sprite that the GeneralsExp window (where the engine draws the fill, UNDER this image) covers inside ExpBarForeground.
# The two layouts differ, so there are two sprites: Control Bar Pro (ControlBar.wnd of 340_ControlBarProZH.big, 1920x1080:
# foreground 1875,887-1911,1071, GeneralsExp 1880,923-1906,1063) and retail (WindowZH.big ControlBar.wnd, 800x600:
# foreground 766,484-787,591, GeneralsExp 769,503-782,590).
TRACK_PRO = (0.139, 0.196, 0.861, 0.957)
TRACK_RETAIL = (0.143, 0.178, 0.762, 0.991)


def draw_ladder(W, H, TRACK):
    cv = Canvas(W, H)
    tx0, ty0, tx1, ty1 = TRACK[0] * W, TRACK[1] * H, TRACK[2] * W, TRACK[3] * H
    cv.plate(NAVY_DEEP, (6, 16, 36), 150, STEEL, 235)
    # the track is a window: clear it so the fill (drawn beneath this image) reads at full strength
    for y in range(int(ty0), int(ty1) + 1):
        for x in range(int(tx0), int(tx1) + 1):
            cv.p[y][x] = (0.0, 0.0, 0.0, 0.0) if (tx0 <= x < tx1 and ty0 <= y < ty1) else cv.p[y][x]
    t = max(2.0, W / 36.0)
    cv.shape(lambda x, y: abs(sd_box(x, y, (tx0 + tx1) / 2, (ty0 + ty1) / 2, (tx1 - tx0) / 2, (ty1 - ty0) / 2)) - t / 2,
             (tx0 - t, ty0 - t, tx1 + t, ty1 + t), STEEL, 0.95)
    n = 35
    for i in range(1, n):
        y = ty0 + (ty1 - ty0) * i / n
        major = (i % 5 == 0)
        ln = (tx1 - tx0) * (1.0 if major else 0.28)
        tt = t * (0.9 if major else 0.8)
        a = 0.95 if major else 1.0
        if major:
            cv.shape(lambda px, py, y=y, tt=tt: sd_box(px, py, (tx0 + tx1) / 2, y, ln / 2, tt / 2), (tx0, y - tt, tx1, y + tt), LABEL, a * 0.8)
        else:
            for xa, xb in ((tx0, tx0 + ln), (tx1 - ln, tx1)):
                cv.shape(lambda px, py, y=y, tt=tt, xa=xa, xb=xb: sd_box(px, py, (xa + xb) / 2, y, (xb - xa) / 2, tt / 2),
                         (xa, y - tt, xb, y + tt), LABEL, a * 0.8)
    # the General's rank medallion
    cx, cy, R = W / 2.0, TRACK[1] * H * 0.5, W * 0.40
    cv.shape(lambda x, y: abs(math.hypot(x - cx, y - cy) - R) - t * 0.7, (cx - R - t, cy - R - t, cx + R + t, cy + R + t), STEEL, 1.0)
    pts = star_pts(cx, cy + R * 0.04, R * 0.72)
    cv.shape(lambda x, y: sd_poly(x, y, pts), (cx - R, cy - R, cx + R, cy + R), AMBER, 1.0)
    return cv.rows()


def draw_fill_tile(name):
    w, h = FILL_TILES[name]
    rows = []
    for y in range(h):
        if name == 'GenExpBarTop1':                      # the leading edge of the fill: a bright cap
            col = WHITE if y == 0 else (150, 192, 246)
        elif name == 'GenExpBarBottom1':
            col = (74, 138, 224) if y < h - 1 else (40, 76, 132)
        else:                                            # body: a barely-there two-row scanline so a tall fill is not flat
            col = (96, 156, 236) if y == 0 else (70, 132, 220)
        rows.append([col + (236,)] * w)
    return rows


# --- atlas ---------------------------------------------------------------------------------------------------------------
def build():
    sprites = []                                         # (key, rows)
    for fam, (small, wide, k, _sp, _wp) in FAMILIES.items():
        for kind in KINDS:
            size = wide if kind == 'General' else small
            if size is None:
                continue
            for st in STATES:
                sprites.append(('%s/%s/%s' % (fam, kind, st), draw_button(kind, st, size[0] * k, size[1] * k)))
    for name in FILL_TILES:
        sprites.append(('fill/' + name, draw_fill_tile(name)))
    sprites.append(('ladder_retail', draw_ladder(LADDER_RETAIL_SIZE[0], LADDER_RETAIL_SIZE[1], TRACK_RETAIL)))
    sprites.sort(key=lambda kr: -len(kr[1]))
    sprites.insert(0, ('ladder', draw_ladder(LADDER_SIZE[0], LADDER_SIZE[1], TRACK_PRO)))          # the tallest first: it owns the left column, the rest shelve beside it
    pix = [[(0, 0, 0, 0)] * ATLAS_W for _ in range(ATLAS_H)]
    coords = {}
    x = y = shelf = 0
    x0 = 0
    for key, rows in sprites:
        h, w = len(rows), len(rows[0])
        cw, ch = w + 2 * GUTTER, h + 2 * GUTTER
        if key == 'ladder':
            x0 = cw
        elif x + cw > ATLAS_W:
            x, y, shelf = x0, y + shelf, 0
        assert y + ch <= ATLAS_H, 'atlas too small at %s' % key
        for yy in range(ch):                             # gutter = the nearest edge texel, so bilinear never pulls in black
            sy = min(max(yy - GUTTER, 0), h - 1)
            for xx in range(cw):
                pix[y + yy][x + xx] = rows[sy][min(max(xx - GUTTER, 0), w - 1)]
        coords[key] = (x + GUTTER, y + GUTTER, x + GUTTER + w, y + GUTTER + h)
        x += cw
        shelf = max(shelf, ch)
    return pix, coords


def mapped_names(coords):
    """name -> coords for every MappedImage this archive redefines."""
    out = {}
    for fam, (_s, _w, _k, small_pre, wide_pre) in FAMILIES.items():
        for kind in KINDS:
            for st in STATES:
                key = '%s/%s/%s' % (fam, kind, st)
                if key not in coords:
                    continue
                for pre in (wide_pre if kind == 'General' else small_pre):
                    if pre == 'ObserverPro' and kind not in OBS_KINDS:
                        continue
                    out['%s%s%s' % (pre, kind, st)] = coords[key]
    out['SUWorkerE'] = coords['SA/Worker/']          # GLA's enabled Worker is SUWorkerE
    for n in NOT_SIDEBAR:
        del out[n]                                   # the loop made it; retail's own art stays (see NOT_SIDEBAR)
    for n in LADDER_PRO_NAMES:
        out[n] = coords['ladder']
    for n in LADDER_RETAIL_NAMES:
        out[n] = coords['ladder_retail']
    for n in FILL_TILES:
        out[n] = coords['fill/' + n]
    return out


def tga_bytes(pix):
    head = struct.pack('<BBBHHBHHHHBB', 0, 0, 2, 0, 0, 0, 0, 0, ATLAS_W, ATLAS_H, 32, 0x08)
    body = bytearray()
    for row in reversed(pix):
        for r, g, b, a in row:
            body += bytes((b, g, r, a))
    return head + bytes(body)


def ini_text(names):
    out = ['; GeneralsX: the control bar side buttons and the General\'s experience ladder in the player HUD\'s own look.',
           '; Generated by scripts/build/make-gxsidebar-archive.py - edit the drawing there, not this file.',
           '; Every name below already exists in retail or Control Bar Pro; this file is parsed last (HandCreated\\zz...) and wins.', '']
    for name in sorted(names):
        l, t, r, b = names[name]
        out += ['MappedImage %s' % name, '  Texture = %s' % TEXTURE, '  TextureWidth = %d' % ATLAS_W,
                '  TextureHeight = %d' % ATLAS_H, '  Coords = Left:%d Top:%d Right:%d Bottom:%d' % (l, t, r, b),
                '  Status = NONE', 'End', '']
    return '\r\n'.join(out)


def main(argv):
    if len(argv) != 1:
        sys.stderr.write(__doc__.split('\n\n')[1] + '\n')
        return 2
    pix, coords = build()
    names = mapped_names(coords)
    assert len(names) == 128 + 6 + 3, len(names)
    os.makedirs(argv[0], exist_ok=True)
    with open(os.path.join(argv[0], TEXTURE), 'wb') as f:
        f.write(tga_bytes(pix))
    with open(os.path.join(argv[0], 'zzGXSidebar.ini'), 'wb') as f:
        f.write(ini_text(names).encode('ascii'))
    print('  %d MappedImages, %d sprites' % (len(names), len(coords)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
