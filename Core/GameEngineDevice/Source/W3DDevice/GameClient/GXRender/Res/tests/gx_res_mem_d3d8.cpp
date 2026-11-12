/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 GeneralsX contributors
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

// The CLASSIC arm of gx_res_mem - DXVK's d3d8 exactly as the game boots it
// on macOS (SDL3Main.cpp: SDL video, DXVK_WSI_DRIVER=SDL3, SDL_Vulkan_LoadLibrary, an SDL window; dx8wrapper.cpp: dlopen
// libdxvk_d3d8.dylib by bare name through DYLD_LIBRARY_PATH, Direct3DCreate8). SDL3 and DXVK are dlopened, so this
// tool links neither; the window is a visible 64x64 one. Compiled in its own TU
// against the fork's native D3D8 headers.
#include "gx_res_mem.h"

#include <d3d8.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

namespace GXRender { namespace ResMem {

namespace {

// SDL3's ABI (SDL_init.h, SDL_video.h): the few calls the boot needs, by value.
typedef bool (*SdlInitFn)(uint32_t);
typedef bool (*SdlVkLoadFn)(const char*);
typedef void* (*SdlCreateWindowFn)(const char*, int, int, uint64_t);
typedef void (*SdlDestroyWindowFn)(void*);
typedef void (*SdlQuitFn)();
typedef const char* (*SdlErrFn)();
typedef void* (*SdlGetGipaFn)();
const uint32_t kSdlInitVideo = 0x20u;                              // SDL_INIT_VIDEO
const uint64_t kSdlWindowVulkan = 0x10000000ull;                  // SDL_WINDOW_VULKAN
typedef IDirect3D8* (WINAPI* Create8Fn)(UINT);

void* g_sdl = nullptr;
void* g_d3d8lib = nullptr;
void* g_window = nullptr;
IDirect3D8* g_d3d = nullptr;
IDirect3DDevice8* g_dev = nullptr;
IDirect3DSurface8* g_sys = nullptr;
std::vector<IDirect3DTexture8*> g_tex;

template <typename T> T sym(void* lib, const char* n) { return reinterpret_cast<T>(dlsym(lib, n)); }

bool fail(std::string* why, const std::string& w)
{
	*why = w;
	return false;
}

// Wait for the GPU: the back buffer copied into a SYSTEMMEM surface (DXVK: GetRenderTargetData) and locked.
bool gpuSync(std::string* why)
{
	IDirect3DSurface8* bb = nullptr;
	if (FAILED(g_dev->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &bb)))
		return fail(why, "GetBackBuffer failed");
	const HRESULT hr = g_dev->CopyRects(bb, nullptr, 0, g_sys, nullptr);
	bb->Release();
	D3DLOCKED_RECT lr;
	if (FAILED(hr) || FAILED(g_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
		return fail(why, "sync copy/lock failed");
	g_sys->UnlockRect();
	return true;
}

bool boot(std::string* why)
{
	if (g_dev)
		return true;
	// ONE Vulkan implementation, as in the game: GeneralsXZH links @rpath/libvulkan.1.dylib, so SDL's in-process probe takes the
	// LOADER, the same one DXVK dlopens and creates its VkInstance with. This tool links no Vulkan library; without the
	// preload SDL took libMoltenVK's vkGetInstanceProcAddr and called it with the loader's instance inside DXVK's
	// CreateDevice (SIGSEGV in MVKInstance::getEntryPoint, Mini runs rwm002104 / rwm002711). GX_MEM_PRELOAD_LOADER=0 = that case.
	const char* pre = std::getenv("GX_MEM_PRELOAD_LOADER");
	if (!(pre && pre[0] == '0') && !dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_GLOBAL))
		return fail(why, std::string("dlopen libvulkan.1.dylib: ") + dlerror());
	g_sdl = dlopen("libSDL3.0.dylib", RTLD_NOW | RTLD_GLOBAL);
	if (!g_sdl)
		return fail(why, std::string("dlopen libSDL3.0.dylib: ") + dlerror());
	SdlErrFn err = sym<SdlErrFn>(g_sdl, "SDL_GetError");
	if (!sym<SdlInitFn>(g_sdl, "SDL_Init")(kSdlInitVideo))
		return fail(why, std::string("SDL_Init: ") + err());
	setenv("DXVK_WSI_DRIVER", "SDL3", 1);
	if (!sym<SdlVkLoadFn>(g_sdl, "SDL_Vulkan_LoadLibrary")(nullptr))
		return fail(why, std::string("SDL_Vulkan_LoadLibrary: ") + err());
	// A visible 64x64 window, like the game's. (Hiding it was suspected for the CreateDevice crash and REFUTED - it crashed
	// visible too; the cause was the two Vulkan implementations above. Hidden has not been tried since.)
	{ // which Vulkan SDL took (the game's [GXMVK] line)
		void* gipa = sym<SdlGetGipaFn>(g_sdl, "SDL_Vulkan_GetVkGetInstanceProcAddr")();
		Dl_info di = {};
		std::printf("[GXMEM] classic: SDL vkGetInstanceProcAddr in %s (preload %s)\n",
			gipa && dladdr(gipa, &di) && di.dli_fname ? di.dli_fname : "(unknown)", pre && pre[0] == '0' ? "off" : "on");
		std::fflush(stdout);
	}
	g_window = sym<SdlCreateWindowFn>(g_sdl, "SDL_CreateWindow")("gx_res_mem", 64, 64, kSdlWindowVulkan);
	if (!g_window)
		return fail(why, std::string("SDL_CreateWindow: ") + err());
	g_d3d8lib = dlopen("libdxvk_d3d8.dylib", RTLD_NOW | RTLD_LOCAL);
	if (!g_d3d8lib)
		return fail(why, std::string("dlopen libdxvk_d3d8.dylib: ") + dlerror());
	Create8Fn create = sym<Create8Fn>(g_d3d8lib, "Direct3DCreate8");
	g_d3d = create ? create(D3D_SDK_VERSION) : nullptr;
	if (!g_d3d)
		return fail(why, "Direct3DCreate8 failed");
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = 64;
	pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_X8R8G8B8;
	pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.hDeviceWindow = reinterpret_cast<HWND>(g_window);
	pp.Windowed = TRUE;
	// The game's flags on a hardware-T&L device (dx8wrapper.cpp:1849-1858): mixed vertex processing, multithreaded.
	if (FAILED(g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, reinterpret_cast<HWND>(g_window),
			D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &g_dev)))
		return fail(why, "CreateDevice failed");
	if (FAILED(g_dev->CreateImageSurface(64, 64, D3DFMT_X8R8G8B8, &g_sys)))
		return fail(why, "CreateImageSurface failed");
	return gpuSync(why);
}

} // namespace

bool classicLoad(const std::vector<TexFile>& textures, uint64_t syncBytes, uint32_t* kept, std::string* why)
{
	if (!boot(why))
		return false;
	uint64_t sinceSync = 0;
	for (const TexFile& f : textures) {
		IDirect3DTexture8* t = nullptr;
		if (FAILED(g_dev->CreateTexture(f.width, f.height, UINT(f.levels.size()), 0, D3DFORMAT(f.format), D3DPOOL_MANAGED, &t)))
			return fail(why, f.name + ": CreateTexture failed");
		g_tex.push_back(t);
		for (UINT l = 0; l < f.levels.size(); ++l) {
			D3DLOCKED_RECT lr;
			if (FAILED(t->LockRect(l, &lr, nullptr, 0)))
				return fail(why, f.name + ": LockRect failed");
			// Tight source rows into the driver's pitch. Rows = block rows for DXTn.
			const bool bc = f.format == D3DFMT_DXT1 || f.format == D3DFMT_DXT3 || f.format == D3DFMT_DXT5;
			const uint32_t h = std::max(1u, f.height >> l);
			const uint32_t rows = bc ? std::max(1u, (h + 3) / 4) : h;
			const size_t rowBytes = f.levels[l].size() / rows;
			for (uint32_t r = 0; r < rows; ++r)
				std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(r) * lr.Pitch, &f.levels[l][r * rowBytes], rowBytes);
			t->UnlockRect(l);
		}
		t->PreLoad();
		sinceSync += f.bytes();
		if (syncBytes && sinceSync >= syncBytes) {
			if (!gpuSync(why))
				return false;
			sinceSync = 0;
		}
	}
	*kept = uint32_t(g_tex.size());
	return gpuSync(why);
}

void classicShutdown()
{
	for (IDirect3DTexture8* t : g_tex)
		t->Release();
	g_tex.clear();
	if (g_sys)
		g_sys->Release();
	if (g_dev)
		g_dev->Release();
	if (g_d3d)
		g_d3d->Release();
	g_sys = nullptr;
	g_dev = nullptr;
	g_d3d = nullptr;
	if (g_window)
		sym<SdlDestroyWindowFn>(g_sdl, "SDL_DestroyWindow")(g_window);
	if (g_sdl)
		sym<SdlQuitFn>(g_sdl, "SDL_Quit")();
}

}} // namespace GXRender::ResMem
