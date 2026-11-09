# How Zero Hour got onto iPhone and iPad

The iOS/iPadOS port in this repository was made by **[ammaarreshi](https://github.com/ammaarreshi)** in
[Generals-Mac-iOS-iPad](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad), the project GeneralsX was forked
from. The text below is an excerpt of that project's README (commit
[`c5c8c4d3`](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad/tree/c5c8c4d3e757033d9ab464f6bd6e15e91e0e742f),
5 July 2026), kept here because it is a good account of what the port involved. Its links point at that
repository, where the full engineering log lives. Build steps for this repository are in
[BUILD/IOS.md](BUILD/IOS.md).

---

## What this port actually involved

"Porting" undersells how weird this journey was, so here's the honest shape of it.
The lineage below built the foundation: EA's source release, the community's
modernization, Fighter19's original Unix port, GeneralsX's macOS/Linux work.
What did *not* exist was any of this on iOS — and iOS is a hostile place for a
2003 Windows RTS:

- **The engine assumes a writable filesystem wherever it lives.** iOS apps live in a
  read-only, code-signed bundle. Every config write, cache, and save path had to be
  rerouted — and the working directory bootstrapped from the bundle itself.
- **The renderer speaks DirectX 8. The iPad speaks Metal.** In between: DXVK
  translating D3D8→Vulkan, MoltenVK translating Vulkan→Metal — and DXVK had never
  been built for iPhoneOS. That took a Meson cross-build and a patch to its Vulkan
  loader, because iOS confines `dlopen` to the app bundle ([`Patches/dxvk-ios.patch`](../Patches/dxvk-ios.patch)).
- **iOS owns your process.** Open the app switcher and the OS seizes the Metal
  drawable *without backgrounding you* — draw one more frame and you're dead on
  resume. The whole render/sim loop learned to hold its breath.
- **An RTS needs a mouse.** SDL3 (from the lineage below) delivers raw touch events;
  the RTS semantics on top are new. Taps defer until the 2003 GUI has processed
  hover (or menu buttons never highlight), a drag has to decide "selection box or
  camera pan," long-press became right-click, and a cancelled touch must never
  ghost-click a rally point.
- **And then the bug hunts** — the best part. The minimap that rendered black
  because a 2003 texture-format fallback silently dropped the alpha channel. The
  EVA voice that went randomly mute because one zombie audio stream held a global
  "don't talk over speech" flag while chirping forever. Every one chased to root
  cause on a real device, fixed, and offered upstream.

**→ The war stories: [Porting Playbook §8 — the bug archaeology](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad/blob/c5c8c4d3e757033d9ab464f6bd6e15e91e0e742f/docs/port/PORTING_PLAYBOOK.md#8-post-ship-bug-hunts-junejuly-2026--the-archaeology-section)**
**→ The complete engineering log: [docs/port/PORTING_PLAYBOOK.md](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad/blob/c5c8c4d3e757033d9ab464f6bd6e15e91e0e742f/docs/port/PORTING_PLAYBOOK.md)**
**→ How to do this to another game: [docs/port/PORTING_PATTERNS.md](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad/blob/c5c8c4d3e757033d9ab464f6bd6e15e91e0e742f/docs/port/PORTING_PATTERNS.md)**

---

*End of excerpt. The original README, with its macOS and iOS quick starts, its lineage section and its plans
for Renegade, is at
[ammaarreshi/Generals-Mac-iOS-iPad](https://github.com/ammaarreshi/Generals-Mac-iOS-iPad/blob/c5c8c4d3e757033d9ab464f6bd6e15e91e0e742f/README.md).*
