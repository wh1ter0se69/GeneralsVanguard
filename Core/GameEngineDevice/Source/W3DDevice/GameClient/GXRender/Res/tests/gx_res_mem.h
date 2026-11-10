// GeneralsX: WS-C week 2: the phys_footprint A/B (PLAN_native-renderer §1b "+" row) - shared types.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace GXRender { namespace ResMem {

// One texture file as the game would create it: D3D8 format number, size, and every level's bytes at tight pitch
// (block rows for DXTn) - the layout GXRES's lockRect and the classic arm's LockRect both receive.
struct TexFile
{
	std::string name;
	uint32_t format = 0;
	uint32_t width = 0, height = 0;
	std::vector<std::vector<uint8_t>> levels;
	uint64_t bytes() const;
};

// Memory as macOS accounts it for this process (task_vm_info): phys_footprint now and the lifetime peak.
struct Footprint
{
	uint64_t now = 0, peak = 0;
};
Footprint footprint();

// The CLASSIC arm: DXVK's d3d8 (dlopened like the game, from DYLD_LIBRARY_PATH) on a 64x64 SDL3 window. Creates every
// texture MANAGED, fills every level through LockRect, PreLoads it, and syncs the GPU every syncBytes and at the end.
// textures empty = the classic baseline (device only). Returns false with *why on failure. *kept receives the live
// texture count (released by classicShutdown).
bool classicLoad(const std::vector<TexFile>& textures, uint64_t syncBytes, uint32_t* kept, std::string* why);
void classicShutdown();

}} // namespace GXRender::ResMem
