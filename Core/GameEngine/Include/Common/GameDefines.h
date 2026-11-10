/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 TheSuperHackers
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "WWLib/WWDefines.h"

// Note: Retail compatibility must not be broken before this project officially does.
// Use RETAIL_COMPATIBLE_CRC, RETAIL_COMPATIBLE_DATA and RETAIL_COMPATIBLE_XFER_SAVE to guard breaking changes.

// GeneralsX: EVERY PRESERVE_* AND RETAIL_COMPATIBLE_* SWITCH BELOW MUST
// HAVE A SIMBIT IN computeEngineID (Core/GameEngine/Source/Common/Diagnostic/SimulationId.cpp).
// These switches change simulation behaviour, and engineID is what refuses a peer that disagrees.
// A switch that is declared here but missing there does not fail loudly: both peers compute the
// same engineID, the join succeeds, and they simulate different games. That happened once already
// with PRESERVE_SNIPING_EMPTY_STINGER_SITES. Add the macro and its SIMBIT in the same commit.
//
// Note also that the common call-site form `#if RETAIL_COMPATIBLE_CRC || PRESERVE_X` guards the
// OLD behaviour, so a PRESERVE_X set to 0 only takes effect while RETAIL_COMPATIBLE_CRC is 0.
//
// GeneralsX: THE UPSTREAM FIXES ARE BUILT IN, ALWAYS (wh1ter0se, 30/09/2026: "the whole
// point is to fix all bugs ... i do not want a switch to enable bug fixes"). CRC, XFER_SAVE, PATHFINDING,
// PATHFINDING_ALLOCATION, NETWORKING and AIGROUP below default to 0, as Generals Online ships them. GeneralsX
// never plays against retail peers or loads retail saves (engineID refuses the one, the save format moved long
// ago for the other), so retail compatibility bought nothing and kept ~110 TheSuperHackers fixes compiled out.
// The defaults live HERE and not in cmake: the SimID source digest hashes files, not the build cache, so a
// -D override would change the simulation without changing the digest. Still at 1, on purpose:
// RETAIL_COMPATIBLE_DATA (the shipped data IS retail data) and RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM (the
// retail fill is the cheaper one; performance first, and Generals Online keeps it too).

#ifndef PRESERVE_BUILDING_RESUMPTION_DELAY
#define PRESERVE_BUILDING_RESUMPTION_DELAY (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_CHINOOK_PASSENGER_DUMPING
#define PRESERVE_CHINOOK_PASSENGER_DUMPING (1)
#endif

#ifndef PRESERVE_HARDCODED_BLACK_LOTUS_CASH_HACK
#define PRESERVE_HARDCODED_BLACK_LOTUS_CASH_HACK (1)
#endif

#ifndef PRESERVE_MULTI_CRATE_PICKUP
#define PRESERVE_MULTI_CRATE_PICKUP (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_NO_XP_FROM_FLAME_KILLS
#define PRESERVE_NO_XP_FROM_FLAME_KILLS (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_NO_XP_FROM_OCL_KILLS
#define PRESERVE_NO_XP_FROM_OCL_KILLS (1)
#endif

#ifndef PRESERVE_NO_XP_FROM_POISON_KILLS
#define PRESERVE_NO_XP_FROM_POISON_KILLS (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_OCCUPANT_DETECTION_VIA_DRAG_SELECTION
#define PRESERVE_OCCUPANT_DETECTION_VIA_DRAG_SELECTION (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_PERPETUAL_HORDE_BONUS
#define PRESERVE_PERPETUAL_HORDE_BONUS (1)
#endif

#ifndef PRESERVE_PREMATURE_BATTLE_BUS_DEATH
#define PRESERVE_PREMATURE_BATTLE_BUS_DEATH (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_RADAR_WARNING_SUPPRESSION
#define PRESERVE_RADAR_WARNING_SUPPRESSION (1)
#endif

#ifndef PRESERVE_STRUCTURE_STEALTH_DURING_REPAIR
#define PRESERVE_STRUCTURE_STEALTH_DURING_REPAIR (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_TUNNEL_HEAL_STACKING
#define PRESERVE_TUNNEL_HEAL_STACKING (1)
#endif

#ifndef PRESERVE_UNRELIABLE_FIRESTORMS
#define PRESERVE_UNRELIABLE_FIRESTORMS (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_SNIPING_EMPTY_STINGER_SITES
#define PRESERVE_SNIPING_EMPTY_STINGER_SITES (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_RETAIL_SCRIPTED_CAMERA
#define PRESERVE_RETAIL_SCRIPTED_CAMERA (1) // Retain scripted camera behavior present in retail Generals 1.08 and Zero Hour 1.04
#endif

#ifndef PRESERVE_RETAIL_PARTICLES
#define PRESERVE_RETAIL_PARTICLES (1) // Preserve original look of particles present in retail Generals 1.08 and Zero Hour 1.04
#endif

#ifndef PRESERVE_RETAIL_NUKE_MISSILE_OUTER_RADIUS_SEARCH
#define PRESERVE_RETAIL_NUKE_MISSILE_OUTER_RADIUS_SEARCH (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef PRESERVE_RETAIL_NUKE_MISSILE_OUTER_RADIUS_DAMAGE
#define PRESERVE_RETAIL_NUKE_MISSILE_OUTER_RADIUS_DAMAGE (0) // The fix for this unfavorable behavior was approved by the Game Design Committee.
#endif

#ifndef RETAIL_COMPATIBLE_CRC
#define RETAIL_COMPATIBLE_CRC (0) // Game is expected to be CRC compatible with retail Generals 1.08, Zero Hour 1.04
#endif

#ifndef RETAIL_COMPATIBLE_DATA
#define RETAIL_COMPATIBLE_DATA (1) // Game and tool data files are expected to be compatible with retail Generals 1.08 and Zero Hour 1.04
#endif

#ifndef RETAIL_COMPATIBLE_XFER_SAVE
#define RETAIL_COMPATIBLE_XFER_SAVE (0) // Game is expected to be Xfer Save compatible with retail Generals 1.08, Zero Hour 1.04
#endif

// This is here to easily toggle between the retail compatible with fixed pathfinding fallback and pure fixed pathfinding mode
#ifndef RETAIL_COMPATIBLE_PATHFINDING
#define RETAIL_COMPATIBLE_PATHFINDING (0)
#endif

// This is here to easily toggle between the retail compatible pathfinding memory allocation and the new static allocated data mode
#ifndef RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION
#define RETAIL_COMPATIBLE_PATHFINDING_ALLOCATION (0)
#endif

#ifndef RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM
#define RETAIL_COMPATIBLE_CIRCLE_FILL_ALGORITHM (1) // Use the original circle fill algorithm, which is more efficient but less accurate
#endif

// Disable non retail fixes in the networking, such as putting more data per UDP packet
#ifndef RETAIL_COMPATIBLE_NETWORKING
#define RETAIL_COMPATIBLE_NETWORKING (0)
#endif

// This is essentially synonymous for RETAIL_COMPATIBLE_CRC. There is a lot wrong with AIGroup, such as use-after-free, double-free, leaks,
// but we cannot touch it much without breaking retail compatibility. Do not shy away from using massive hacks when fixing issues with AIGroup,
// but put them behind this macro.

#ifndef RETAIL_COMPATIBLE_AIGROUP
#define RETAIL_COMPATIBLE_AIGROUP (0) // AIGroup logic is expected to be CRC compatible with retail Generals 1.08, Zero Hour 1.04
#endif

// GeneralsX:
// Manual escape hatch for the join-time compatibility identity (SimID).
// The automatic source digest is the primary detector and you should normally never
// touch this; bump it only when you knowingly change simulation behaviour from a file
// OUTSIDE the digested paths (Core/GameEngine's Common, GameLogic and GameNetwork
// trees, Core/Libraries/Include, WWMath, the two game trees' equivalents, and the
// build configuration under cmake/, triplets/, CMakePresets.json and the vcpkg files).
// Nothing depends on you remembering to bump it.
//
// 2 (29/07/2026): dataID stopped being m_iniCRC alone and became m_iniCRC mixed with a
// digest of the loose simulation-relevant files (Data\Scripts\*.scb). Bumped so an
// epoch-1 peer refuses at engineID with ENGINE_DIFFERS - "these are different
// generations of the check" - instead of at dataID with DATA_DIFFERS, which would have
// told an honest player their INI files disagreed when in fact only the recipe changed.
//
// 3 (30/07/2026): engineID now hashes LOGICFRAMES_PER_SECOND, the simulation tick rate.
// It resolves to WWSyncPerSecond in Core/Libraries/Source/WWVegas/WWLib/WWCommon.h, and
// WWLib is NOT one of the digested paths listed above - so editing that single enum
// changed the rate of every logic frame while leaving BOTH sourceID and engineID
// untouched. A 30 Hz and a 60 Hz build reported the same SimID, passed SimIdCompare and
// desynced on contact, which is precisely the failure class this identity exists to turn
// into a refused join. Latent rather than live in this tree - WWSyncPerSecond is 30 and
// WW3D2/htree.cpp:614 static_asserts it - but Generals Online ships the 60 Hz variant
// behind GENERALS_ONLINE_HIGH_FPS_SERVER (see references/generals-online-client), so the
// pairing is reachable in practice, not hypothetical. Bumped rather than left implicit
// because the new field changes engineID for every existing build, and the epoch is the
// one place that records WHY two generations of the check disagree.
// 4 (01/08/2026): the loose scan now covers CommandButton.ini and CommandSet.ini, and its
// hash key changed from the file's basename to its canonical virtual path. Both halves are
// TheSuperHackers/GeneralsGameCode#80: ControlBar::init loads those two with a nullptr Xfer
// (ControlBar.cpp:1082-1086) so they never reach m_iniCRC, while CommandButton's WeaponSlot
// and MaxShotsToFire reach the simulation via GUICommandTranslator.cpp:149,151 - an edited
// CommandButton.ini injected chosen integers into everyone's simulation while both tier-1
// gates reported a match. The key had to move at the same time because the basename form
// folds Data\INI\CommandButton.ini and Data\INI\Default\CommandButton.ini into one entry, and
// ControlBar::init loads BOTH. Measured on the deployed macOS game dir: 24 basename
// collisions under Data\INI, commandbutton.ini among them. Bumped because the recipe changed
// for every existing build - an epoch-3 peer now refuses at engineID with ENGINE_DIFFERS
// rather than at dataID with DATA_DIFFERS, which would have told an honest player their INI
// files disagreed when in fact only the coverage changed.
// GeneralsX @bugfix wh1ter0se 11/08/2026 Epoch 5: two formerly CRC-gated fixes were ungated
// (hacker re-order, dead-aircraft locomotor), so a peer built WITHOUT git metadata - whose
// sourceID is the reserved 0 sentinel and whose comparison is therefore SKIPPED - could join a
// mismatched build and desync mid-game instead of being refused at the door. The epoch bump
// closes that hole for every peer built from here on: an epoch-4 binary refuses an epoch-5 one
// at engineID outright, git metadata or not. Deliberately shipped WITH the first post-ungate
// digest batch rather than alone, because the bump itself forces the atomic 4-platform rebuild.
// GeneralsX: Epoch 6: the dynamic Game LOD fields that GameLogic reads
// (DebrisSkipMask via isDebrisSkipped, SlowDeathScale) are now pinned to their neutral values
// whenever TheRecorder->isMultiplayer(), because the LOD level selecting them is chosen from the
// local render rate and can never be made to agree between peers. Same class as epoch 5 and bumped
// for the same reason, not because any digest recipe changed - this one did not touch dataID at
// all. On stock data the guard is inert, but against a mod that restores EA's commented-out
// values a guarded and an unguarded peer simulate different games, and sourceID cannot refuse that
// pairing when either side was built without git metadata: SimulationId.cpp skips the sourceID
// comparison outright when either value is the reserved 0 sentinel. engineID is the only gate that
// still fires for those peers, and the epoch is what moves it.
// GeneralsX: Epoch 8: Pathfinder::findClosestPath now has an expansion
// and allocation bound (MAX_CLOSEST_PATH_CELL_COUNT / MAX_CLOSEST_PATH_INFO_COUNT in
// AIPathfind.cpp), and a bounded search can return a different "closest" cell than the unbounded
// one it replaces. Same class as epochs 5 and 6 and bumped for the same reason rather than for any
// recipe change: it moves sourceID and nothing else - not dataID - and SimulationId.cpp:800 skips
// the sourceID comparison outright when either side is the reserved 0 sentinel, which is what a
// peer built without git metadata carries. For those peers engineID is the only gate left, and the
// epoch is what moves it.
// GeneralsX: Epoch 9: Pathfinder::internalFindPath now has the same
// allocation bound (MAX_INTERNAL_PATH_INFO_COUNT and the renamed PATHFIND_SEARCH_FREE_RESERVE in
// AIPathfind.cpp), and a bounded search can FAIL where an unbounded one succeeded - a sharper
// divergence than epoch 8's, because the caller then takes the findClosestPath fallback and the
// two peers path differently from that frame on. Same class as epochs 5, 6 and 8 and bumped for
// the same reason rather than for any recipe change: it moves sourceID and nothing else - not
// dataID - and SimulationId.cpp:800 skips the sourceID comparison outright when either side is
// the reserved 0 sentinel, which is what a peer built without git metadata carries. For those
// peers engineID is the only gate left, and the epoch is what moves it.
//
// INTEGRATOR: this bump is deliberately isolated in its own commit so it can be dropped. If
// another branch in the same batch also bumps, resolve the conflict to a SINGLE (9) and keep both
// reason paragraphs - the epoch counts generations of the check, not commits, so (10) would be
// wrong. If nothing else in the batch bumps, this must ship, because the change above is a
// simulation-behaviour change.
//
// GeneralsX @simid /08/2026 EPOCH 10 - checkPathCost's cost return.
//
// ONE of the two fixes above needs this; the other does not, and the difference is the whole
// reason to read this paragraph rather than pattern-match it.
//
//   * The processHierarchicalCell hasInfo() guard needs NO epoch. It is a no-op in Zero Hour (the
//     cell always has an info by then), and in Generals it turns a segfault into a skip. A peer
//     that crashes is not a peer that diverges, so there is no silent disagreement to gate.
//
//   * checkPathCost returning MAX_COST instead of 0 DOES need it. That path is live in both
//     builds, and two peers at the same pool state on the same frame - which lockstep guarantees -
//     would answer checkForAdjust differently: the patched one rejects the adjusted destination,
//     the unpatched one accepts it. Different destinations, same frame, no error. That is exactly
//     the silent desync the epoch exists to refuse.
//
// INTEGRATOR, CONDITIONAL. Epoch 9 is in batch/2026-08-25b and has NOT shipped. The epoch counts
// generations of the check, not commits: if this ships in the SAME release as that batch, resolve
// back to (9); if the batch ships first, as it is queued to, this must be (10). Isolated in its
// own commit so it can be dropped either way.
#ifndef SIMID_EPOCH
#define SIMID_EPOCH (10)
#endif

#ifndef ENABLE_GAMETEXT_SUBSTITUTES
#define ENABLE_GAMETEXT_SUBSTITUTES (1) // The code can provide substitute texts when labels and strings are missing in the STR or CSF translation file
#endif

#ifndef ALLOW_MONEY_PER_MINUTE_FOR_PLAYER
#define ALLOW_MONEY_PER_MINUTE_FOR_PLAYER (0) // When enabled, a money-per-minute stat is calculated and displayed in-game
#endif

// Previously the configurable shroud sat behind #if defined(RTS_DEBUG)
// Enable the configurable shroud to properly draw the terrain in World Builder without RTS_DEBUG compiled in.
// Disable the configurable shroud to make shroud hacking a bit less accessible in Release game builds.
#ifndef ENABLE_CONFIGURABLE_SHROUD
#define ENABLE_CONFIGURABLE_SHROUD (1) // When enabled, the GlobalData contains a field to turn on/off the shroud, otherwise shroud is always enabled
#endif

// Enable buffered IO in File System. Was disabled in retail game.
// Buffered IO generally is much faster than unbuffered for small reads and writes.
#ifndef USE_BUFFERED_IO
#define USE_BUFFERED_IO (1)
#endif

// Enable cache for local file existence. Reduces amount of disk accesses for better performance,
// but decreases file existence correctness and runtime stability, if a cached file is deleted on runtime.
#ifndef ENABLE_FILESYSTEM_EXISTENCE_CACHE
#define ENABLE_FILESYSTEM_EXISTENCE_CACHE (1)
#endif

// Enable prioritization of textures by size. This will improve the texture quality of 481 textures in Zero Hour
// by using the larger resolution textures from Generals. Content wise these textures are identical.
#ifndef PRIORITIZE_TEXTURES_BY_SIZE
#define PRIORITIZE_TEXTURES_BY_SIZE (1)
#endif

// Enable obsolete code. This mainly refers to code that existed in Generals but was removed in GeneralsMD.
// Disable and remove this when Generals and GeneralsMD are merged.
#if RTS_GENERALS
#ifndef USE_OBSOLETE_GENERALS_CODE
#define USE_OBSOLETE_GENERALS_CODE (1)
#endif
#endif

// Overwrite window settings until wnd data files are adapted or fixed.
#ifndef ENABLE_GUI_HACKS
#define ENABLE_GUI_HACKS (1)
#endif

// Tell our computer identity in the LAN lobby. Disable for privacy.
// Was enabled in the retail game and exposed the computer login and host names.
#ifdef RTS_DEBUG
#ifndef TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY
#define TELL_COMPUTER_IDENTITY_IN_LAN_LOBBY (1)
#endif
#endif

#define MIN_DISPLAY_BIT_DEPTH       16
#define DEFAULT_DISPLAY_BIT_DEPTH   32
#define DEFAULT_DISPLAY_WIDTH      800 // The standard resolution this game was designed for
#define DEFAULT_DISPLAY_HEIGHT     600 // The standard resolution this game was designed for
