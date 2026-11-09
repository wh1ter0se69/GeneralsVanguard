// GeneralsX @feature E · rw3-S 11/10/2026 L1 in the game: the IDirect3D8 proxy and the per-session native/classic decision
// (render-seam/DESIGN_l1-device.md §1, §2, §5.2, §8.1; RW3_PLAN_1710 brief S).
//
// GxNativeD3D8 wraps DXVK's own IDirect3D8. Every adapter query is forwarded, so caps / identity / modes / formats are DXVK's
// answers by construction. Only CreateDevice differs: rows S2-S10 are evaluated in order; the first "no" tears down every
// native object created so far (the half-built device's Release(), DESIGN §2.2) and forwards to DXVK's CreateDevice, logging ONE
// `[GXNATIVE] session=classic reason="..."` line. All "yes" -> `[GXNATIVE] session=native ...` and the native device is
// returned. A native session never hands back (DESIGN §2.3). Row S1 (the build has GX_RENDER_NATIVE and the process asked for native:
// Options.ini GXRenderer = native, or GX_RENDERER=native) is the installer's: when it does not hold, GxNativeWrapInterface is never
// called (GxNative::Requested() -> GXGraphics::nativeRendererRequested() answers it, GameClient/GXRendererKey.h).
//
// Render-only: nothing the simulation reads.
#pragma once

#include <d3d8.h>

#include "GxNativeDevice.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GXRender { namespace Dev {

// What the proxy needs beyond GxNativeHost (the device's). Every callback may be null.
struct GxNativeSessionHost
{
	// Row S6: reads a file through the GAME's file system (the one W3DShaderManager::LoadAndCreateD3DShader uses), the path as
	// the game spells it ("shaders\\terrain.pso"). false = not found. null = S6 refuses ("no file reader"): a mod's override
	// of a shader must be seen exactly as the game sees it, so the proxy never reads the disk by itself.
	bool (*readFile)(const char* path, std::vector<uint8_t>* bytes, void* user) = nullptr;
	void* user = nullptr;
	// null = ::getenv. Tests inject an environment; the product leaves it null.
	const char* (*getEnv)(const char* name) = nullptr;
};

// TEST-ONLY plants (gx_proxy_test's X- arms); all false in the product.
struct GxNativeSessionDebug
{
	bool skipPlatformRow = false;    // S2 accepts any platform
	bool skipEnvRow = false;         // S3 does not look at the conflicting-instrument variables
	bool skipPresentRow = false;     // S4 accepts any present parameters
	bool skipConfigRow = false;      // S5 accepts an unreproduced DXVK config key
	bool skipShaderRow = false;      // S6 accepts any shader set
	bool skipShadowRow = false;      // S7 ignores GX_SHADOW_GPU
	bool shadowPortMissing = false;  // S7 behaves as in a build whose catalogue lacks the GX_SHADOW_GPU vs (rw3-R11's arm)
	bool skipLoaderRow = false;      // S8 accepts any loader
	bool skipUuidRow = false;        // S9 does not compare the adapter UUID
	bool noTeardown = false;         // a refused session leaves the half-built device alive (never forwards to DXVK)
	bool platformUnqualified = false; // S2 behaves as on a platform that is not macOS
	uint32_t fakeSampleMask = 0;     // S4 MSAA parity uses this framebuffer sample-count mask instead of the device's (a sparse one)
	bool skipExtentRow = false;      // S9 does not compare the created swapchain's extent
	bool skipMsaaParity = false;     // S4 does not compare DXVK's MSAA decode with what the device can grant
	const char* fakeProcAddrImage = nullptr;   // S8 (before the device) sees the host's loader in this image instead of its own (bad provenance around a working loader)
	bool s8InterposerAnyPath = false;  // B · dlss-streamline-1: S8 waives the loader checks for ANY procAddr module once an interposer is declared
};

// The decision's outcome, also returned by GxNativeDecideAndCreate for tests and tools.
struct GxNativeSession
{
	bool native = false;
	std::string reason;     // classic: the row text (without the row number)
	int failedRow = 0;      // classic: 2..10
	std::string line;       // the `[GXNATIVE] session=...` line that was logged
	IDirect3DDevice8* leakedDevice = nullptr;   // TEST-ONLY (noTeardown plant): the refused device, the caller releases it
};

// The proxy. `dxvk` is the interface DXVK's Direct3DCreate8 returned; the proxy takes over that one reference (its own
// Release releases it) and returns itself with one reference. On any failure to build the proxy: returns `dxvk` unchanged.
bool GxNativeSessionLive();	// GeneralsX: the device this process created last is a native session
IDirect3D8* GxNativeWrapInterface(IDirect3D8* dxvk, const GxNativeHost& host, const GxNativeSessionHost& session,
	const GxNativeSessionDebug* debug = nullptr);

// The proxy behind an IDirect3D8 pointer (null for DXVK's own): for tools and tests.
class GxNativeD3D8;
GxNativeD3D8* GxNativeProxyFrom(IDirect3D8* d3d);
// The session the proxy last decided (valid after its CreateDevice).
const GxNativeSession& GxNativeLastSession(const GxNativeD3D8* proxy);

// --- S8, pure so tests can feed it fake images (DESIGN §2.1 row S8; Codex rw3L-design-r1 #8).
// `procAddrImage` = the image `dladdr` finds for the host's vkGetInstanceProcAddr ("" = the host gave none, the device searched
// itself); `images` = the process's loaded images, symlinks already resolved. Qualified = the proc-addr image is a Vulkan
// LOADER (libvulkan*, not MoltenVK) and the process holds exactly one distinct loader build and exactly one MoltenVK.
struct GxLoaderFacts
{
	std::string procAddrImage;
	std::vector<std::string> images;
	// The BUILD of each image, parallel to `images` ("uuid:<hex>" = the Mach-O LC_UUID of the loaded slice, "sha256:<hex>" = the file's
	// content when it has none, "path:<p>" = unreadable: only itself). Two paths with one build are ONE loader (the 0.9.63 bundle stages
	// libvulkan.1.dylib and libvulkan.dylib as identical copies). Empty / a different size = every image is its own build (its path).
	std::vector<std::string> builds;
	// B · dlss-streamline-1 13/10/2026 (PREREG A36.2): the module of the ONE vetted interposer allowed to own the host's
	// vkGetInstanceProcAddr (NVIDIA Streamline's sl.interposer.dll, NVIDIA-signature-checked and loaded by full path by the host;
	// it forwards to the process's vulkan-1.dll, which must still be the one loader build). Empty = none declared.
	std::string interposerImage;
	bool testInterposerAnyPath = false;   // TEST plant (GxNativeSessionDebug::s8InterposerAnyPath)
};
// requireLoader (D · native-windows-1, 13/10/2026): after creation off Apple - a loader must be loaded and be the host's, without
// requiring MoltenVK. requireMoltenVK implies it.
bool GxNativeQualifyLoader(const GxLoaderFacts& facts, bool requireMoltenVK, std::string* loaderPath, std::string* mvkPath,
	std::string* why, bool requireLoader = false);
// The facts of THIS process (macOS: dladdr + _dyld_image_*; Windows: GetModuleHandleEx + the Toolhelp module list, PE identity as
// the build); empty on a platform without them.
GxLoaderFacts GxNativeLoaderFacts(PFN_vkGetInstanceProcAddr getInstanceProcAddr, const void* interposerModule = nullptr);

// --- S4, pure: `why` = the field (without the "present parameters: " prefix). windowExtent 0x0 = unknown (no check).
bool GxNativeQualifyPresent(const D3DPRESENT_PARAMETERS& pp, std::string* why);

// --- S4's MSAA parity, pure: the sample count DXVK's DecodeMultiSampleType grants for `requested` under the framebuffer count `mask`
// (d3d9_util.cpp:41-65: max(n,1), halved while it exceeds the mask AS A NUMBER). The device can grant it iff (mask & count) != 0.
uint32_t GxNativeDxvkSampleCount(uint32_t requested, uint32_t mask);

// --- S9's UUID gate, pure: the Vulkan deviceUUID bytes vs D3DADAPTER_IDENTIFIER8::DeviceIdentifier (DXVK: bit::cast<GUID>(uuid)).
bool GxNativeUuidMatches(const uint8_t vkUuid[16], const GUID& dxvkDeviceIdentifier);

// --- S6, pure over a file reader: the shader files the game LOADS (DESIGN §10.1) = the catalogue entries with a port that ship
// as files; each is read through `host.readFile` as "shaders\\<name>" and judged by GxCsSessionVerdict. `why` empty on success.
bool GxNativeQualifyShaders(const GxNativeSessionHost& host, std::string* why);

}} // namespace GXRender::Dev
