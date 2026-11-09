# Launcher frontend

Eleven screens, static HTML and CSS. No framework, no build step, no IPC, no
`window.__TAURI__`. Every screen opens standalone in a browser, which is the point:
the Rust and IPC side is being built in parallel and this has to be reviewable
without any of it.

```
launcher/
  tokens.css          the one place a colour, size or easing is written down
  index.html          the eleven screens, linked
  screens/*.html      a screen and its scoped stylesheet
  reference/*.html    generated; inputs to the fidelity check, not screens
  verify.html         open it — it proves the conversion changed nothing
```

Regenerate with:

```bash
cd design && python3 ../scripts/design/convert-artboards.py launcher/screens launcher/reference
```

## Eleven, not fifteen

`design/` holds fifteen artboards and only eleven are screens.

`StatusStates`, `Sidebar` and `BottomBar` are **component sheets** — they show
several variants of one control side by side to argue a design point, so building
them as screens would produce a page no user can ever navigate to. `Main.dc.html`
is the superseded WinForms first draft.

## Why every rule is scoped

Each screen's stylesheet is scoped under a root class (`.gx-s-optionshud`), and
keyframes are suffixed per screen. Nothing but tokens is global.

That is not caution, it is a measurement. Of the 68 selectors defined in more than
one artboard, **58 are byte-identical and 10 genuinely conflict**:

| selector | conflict |
|---|---|
| `.toggle`, `.toggle .knob`, `.toggle.on`, `.toggle.off`, `.toggle.on .knob` | 42×23 with a 12px radius on three screens, 40×22 with 11px on the denser HUD page |
| `.row` | `14px 16px` padding on Graphics, `9px 14px` on HUD |
| `.rel` | a 66px changelog row, a 52px changelog row, and on About a text badge that is not a row at all |
| `.gx-save`, `.item svg`, `:root` | differ across 9, 7 and 11 screens |

A single flat stylesheet resolves each of those to whichever file was concatenated
last, and the result is a HUD page whose toggles silently grow by two pixels. The
`gx-breathe` keyframe differs between the two main screens for the same reason.

The 58 identical ones were left duplicated rather than promoted to a shared layer.
Identical *today* is not a contract, and the cost of duplication in static review
files is zero while the cost of a future edit to `.item` silently changing seven
screens is not.

## Tokens

`tokens.css` holds 124 tokens, every one measured out of the artboards. The
extraction found **118 distinct colours, 22 font sizes, 3 radii** and exactly two
CSS variables already declared — `--ease-out` and `--ease-press`.

Two things that fall out of the numbers and are easy to get wrong later:

**The design is square.** Three radii exist across eleven screens: `50%` for dots,
and `12px`/`11px` for the two toggle pills. There is no radius scale to complete.

**Near-identical greys are not duplicates.** `#1B1B1E`, `#1E1E22`, `#202024`,
`#232329` and `#26262B` are five deliberate steps of one surface stack. Collapsing
any two of them flattens a panel the design separates on purpose.

Sizes are named by value (`--gx-fs-125` is 12.5px), colours by role. The asymmetry
is deliberate: a size token maps mechanically and a machine can check it, so 246
substitutions across eleven screens carry no judgement; a colour needs a role name
to be worth having, so those were assigned by hand and are checked by comparing
computed styles instead.

## The HUD preview keeps the engine's fonts

`--gx-font-hud` is Arial and `--gx-font-hud-alt` is Tahoma, and neither is a
mistake. The preview panel on Options → HUD is a picture of what the **game**
draws, and the game draws those. Seventeen colour literals inside it are likewise
left untokenised — they are the engine's palette, not the launcher's. Restyling any
of it in Barlow would make the preview a picture of something the player never sees.

## What is deliberately not here

**Interaction.** The Keys page's key-capture control is a live component in the
artboard: its script owns the hard clash (two toggles cannot share a key, refused)
and the soft clash (a key the game already uses is allowed, and the cost is named).
None of that is reproduced. The screens render its initial state — `F10` and `F11`,
no note rows — read directly off the component's own constructor. The behaviour is
specified in `design/OptionsHud.dc.html` and belongs with the IPC wiring.

**Live data.** Build numbers, dates and paths are the artboards' sample content.

## Verifying it

`verify.html` loads every screen beside a generated reference — the same artboard
with only its design-canvas wrappers and bindings resolved, every literal untouched
— walks both element trees in document order and compares 29 computed properties on
each element.

Last run: **1,360 elements, 39,440 computed properties, 3 differences.**

All three are one intentional change, on About: that screen declared
`'Barlow Condensed', sans-serif` where the other ten declared the full three-step
stack. Unified, because the difference is invisible while the webfont loads and
becomes an inconsistency only when it fails — those three headings would drop to
generic sans while every other heading dropped to Barlow. The check reports them
identical in width, height and every other compared property.

The comparison is against a reference rather than the raw artboard for a reason.
The artboards do not render standalone — `support.js` is not in this repository, so
OptionsHud prints `{{ gameLabel }}` at you and its unmatched `class="{{ gameBoxClass }}"`
leaves the key boxes completely unstyled. Comparing against that conflated "did
resolving the template change the rendering?" (yes, deliberately) with "did
tokenising change the rendering?" (must be no), and produced 43 differences that
were all correct behaviour. Splitting them is what makes zero mean something.
