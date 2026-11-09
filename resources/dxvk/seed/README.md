# DXVK pipeline seed (A2, 01/10/2026)

What a fresh macOS / iPadOS install warms its graphics pipelines from during its first load screen
(`SDL3Main.cpp` `gxSeedDxvkCache` copies both files into the player's DXVK folder when absent; the patched
DXVK in `references/fbraz3-dxvk` generates the recorded fixed-function shaders up front and its state cache
compiles every recorded pipeline; `LoadScreen::waitForPipelineWarmup` holds the load screen until done).

* `GeneralsXZH.dxvk-cache` - DXVK 2.6's own state cache (v18): every pipeline state seen.
* `GeneralsXZH.dxvk-ffkeys` - the fixed-function shader keys seen (`GXFF` v2: per-record FNV-1a checksum, `D3D9FFShaderModuleSet::GxLoad`; converted from v1 01/10).

Generated on the Mac Mini (M4, MoltenVK) from two 12-player Hard-AI matches that are NOT the A2 gate
workload: GXCircle12ZHv1 `-lanseed 5` (crashed at frame ~53266 in integration c9b3bfbc0's AI hunt-stop
code, so partial) and TwinRidgesZHv1 `-lanseed 7` (90000 frames), both at `GX_FAST_FORWARD=2`, the owner's
graphics keys at 1920x1080, one DXVK folder accumulating both (`modernize/a2/a2-drive.sh`, specs S1/S2).

Regenerate the same way after a DXVK change that moves the state-cache or key format (the files are then
ignored, never misread: both carry a version header). Apple GPUs only; other GPUs never read it.

## 04/10/2026: stealth looks added (fix/stealth-looks-in-seed)

Reason: a cold macOS Metal cache hitched 113-118 ms the first time USAF Comanches were drawn detected
(3/3 cold runs, 0/3 warm), because none of the stealth looks' pipelines were in the seed above.
The files now also hold the pipelines of a stealthed-translucent, a detected (heat-vision) and a
friendly-detected Comanche: the seed above plus `probe/comanche-stealth` arms D, G and F (24 stealth
Comanches + 4 Gattling-cannon detectors) accumulated in one DXVK folder on the Mac Mini
(`fixes/stealth-hitch2/tools/sh3-record.sh`, `SH_CARRY=1`). cache 103780 -> 114770 B, ffkeys 2033 -> 2321 B.
