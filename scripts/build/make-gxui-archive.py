#!/usr/bin/env python3
# Copyright 2026 GeneralsX contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Add a Confirm Password field to PopupHostGame.wnd.

Reads the stock layout straight out of WindowZH.big, adds two children (a label and an entry
field), grows the popup and shifts the controls below the insertion point down. Everything is
done by transforming the retail bytes rather than by hand-authoring a layout, so every property
we do not care about - fonts, draw data, colours, header templates - stays exactly as EA had it.
"""
import os, re, sys

# GX_WINDOW_BIG overrides the source archive.
#
# The default is the Mac's install and was the only path this script could ever read, which is
# precisely why the archive it produces existed on one machine. The Windows and Linux packagers
# have a WindowZH.big too - just not at a path spelled with this $HOME - so the delivery path in
# scripts/tooling/build-overlay-generated.py passes the location instead of assuming it. Default
# unchanged, so a hand run on this Mac still works with no argument.
SRC = os.environ.get('GX_WINDOW_BIG') or os.path.expanduser('~/GeneralsX/GeneralsZH/WindowZH.big')
OFF, LEN = 5323212, 40101
GROW = 48                      # vertical space for the new label + entry

raw = open(SRC, 'rb').read()[OFF:OFF + LEN].decode('latin1')

# --- split into head / children / tail -------------------------------------------------------
mark = '\r\n  CHILD\r\n'
head, *kids = raw.split(mark)
tailmark = '\r\n  ENDALLCHILDREN\r\n'
assert tailmark in kids[-1], 'layout tail not where expected'
kids[-1], tail = kids[-1].split(tailmark, 1)
tail = tailmark + tail

def name_of(b):
    m = re.search(r'NAME\s*=\s*"([^"]*)"', b)
    return m.group(1) if m else ''

def shift_y(b, dy):
    def f(m):
        return f'UPPERLEFT: {m.group(1)} {int(m.group(2)) + dy},'
    b = re.sub(r'UPPERLEFT:\s*(\d+)\s+(\d+),', f, b)
    def g(m):
        return f'BOTTOMRIGHT: {m.group(1)} {int(m.group(2)) + dy},'
    return re.sub(r'BOTTOMRIGHT:\s*(\d+)\s+(\d+),', g, b)

def set_rect(b, x1, y1, x2, y2):
    b = re.sub(r'UPPERLEFT:\s*\d+\s+\d+,', f'UPPERLEFT: {x1} {y1},', b, count=1)
    return re.sub(r'BOTTOMRIGHT:\s*\d+\s+\d+,', f'BOTTOMRIGHT: {x2} {y2},', b, count=1)

# --- templates -------------------------------------------------------------------------------
pw    = next(k for k in kids if name_of(k).endswith('TextEntryGamePassword'))
label = next(k for k in kids if 'GUI:Password' in k and 'STATICTEXT' in k)

confirm_entry = set_rect(pw, 257, 284, 535, 308)
confirm_entry = confirm_entry.replace('PopupHostGame.wnd:TextEntryGamePassword',
                                      'PopupHostGame.wnd:TextEntryGamePasswordConfirm', 1)

confirm_label = set_rect(label, 257, 258, 535, 284)   # full entry width: the error message lives here
# Name it so the C++ can find it, and let the code set the string via FETCH_OR_SUBSTITUTE - the
# house pattern, and it means no .csf has to be edited to add one line of English.
confirm_label = re.sub(r'NAME\s*=\s*"[^"]*"',
                       'NAME = "PopupHostGame.wnd:StaticTextConfirmPassword"', confirm_label, count=1)

# --- shift everything that sits below the insertion point ------------------------------------
MOVE = ('CheckBoxUseStats', 'CheckBoxLimitArmies', 'ButtonCreateGame', 'ButtonCancel')
out = []
for k in kids:
    n = name_of(k)
    if any(n.endswith(m) for m in MOVE):
        k = shift_y(k, GROW)
    out.append(k)

out.append(confirm_label)
out.append(confirm_entry)

# --- grow the parent -------------------------------------------------------------------------
head = re.sub(r'(BOTTOMRIGHT:\s*(\d+)\s+)(\d+),',
              lambda m: f'{m.group(1)}{int(m.group(3)) + GROW},', head, count=1)

res = head + mark + mark.join(out) + tail
dst = sys.argv[1]
os.makedirs(os.path.dirname(dst), exist_ok=True)
open(dst, 'wb').write(res.encode('latin1'))

# --- report ----------------------------------------------------------------------------------
pat = re.compile(r'WINDOWTYPE\s*=\s*(\w+);.*?UPPERLEFT:\s*(\d+)\s+(\d+),\s*BOTTOMRIGHT:\s*(\d+)\s+(\d+).*?NAME\s*=\s*"([^"]*)"', re.S)
print(f'wrote {dst}  ({len(res)} bytes, was {LEN}); children {len(kids)} -> {len(out)}')
for m in pat.finditer(res):
    t, x1, y1, x2, y2, nm = m.groups()
    print(f'  {t:<11} ({x1:>3},{y1:>3})-({x2:>3},{y2:>3})  {nm.split(":")[-1] or "(unnamed)"}')
