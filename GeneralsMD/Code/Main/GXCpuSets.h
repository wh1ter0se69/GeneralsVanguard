// GeneralsX: prefer the V-cache CCD on X3D CPUs.
//
// A Ryzen X3D part with two CCDs (7900X3D, 7950X3D, 9900X3D, 9950X3D) has a large L3 on one CCD and a normal one
// on the other. AMD's driver moves a game onto the cache CCD only when Windows Game Mode is on AND Windows knows
// the program is a game; GeneralsX is not on that list, so Windows ran it on the other CCD. Measured on the
// owner's 9950X3D with his own 6v6 replay (modernize/HANDBACK_perf-6v6-attribution.md PART 2): cache CCD
// 37.8-43.3 fps / 3.8-4.1 ms tick, unpinned (= the other CCD) 29.0-32.2 fps / 5.95-6.1 ms.
//
// So at startup, when the L3 domains are equally wide but the largest L3 is at least 2.5x the smallest (V-cache
// is 3x: 96 vs 32 MB), the process's default CPU sets become the logical processors under the LARGEST L3. CPU sets
// are a scheduler preference, not an affinity mask: every thread without its own CPU sets (the main thread, the
// engine's and DXVK's workers) prefers those processors, and an affinity mask set by the user or a launcher still
// wins (Microsoft, "CPU Sets"). A single L3 or L3s within 2.5x (Intel, non-X3D AMD, 7800X3D, dual-V-cache parts)
// and L3 domains of different widths (AMD hybrids like Strix Point) change nothing. GX_CPUSETS=0 turns it off.
//
// NOT IN THE SIMULATION DIGEST: GeneralsMD/Code/Main is not a digested root, and where a thread runs changes no
// game state. The decision is plain C++ so it is tested on any host by scripts/test/x3d-cpusets-test.cpp.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// One L3 cache: its size and the logical processors sharing it, one (processor group, mask) per group it spans.
struct GXL3Cache
{
	uint64_t bytes;
	std::vector<std::pair<uint16_t, uint64_t>> groupMasks;
};

/// One CPU set: its id and the logical processor it maps to.
struct GXCpuSet
{
	uint32_t id;
	uint16_t group;
	uint8_t logicalProcessor;
};

struct GXCpuSetChoice
{
	std::vector<uint32_t> ids;	///< empty = no preference
	std::string why;		///< for the [GXCPU] line
};

/// The CPU set ids under the largest L3, or none unless the L3 domains are equally wide and the largest L3 is at
/// least 2.5x the smallest.
GXCpuSetChoice gxChooseLargestL3CpuSets(const std::vector<GXL3Cache> &l3, const std::vector<GXCpuSet> &sets);

#ifdef _WIN32
/// Reads this machine's topology, applies the choice (unless GX_CPUSETS=0 or a restrictive affinity mask is
/// already set) and prints one [GXCPU] line. Call it before the engine starts its threads.
void gxPreferLargestL3();
#endif
