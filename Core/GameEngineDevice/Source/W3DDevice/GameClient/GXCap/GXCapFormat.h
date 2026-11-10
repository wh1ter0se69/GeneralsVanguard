// GeneralsX: GXCAP capture format (WS-I).
// Shared by the recorder (GXCapRecorder.cpp, inside the game) and gxcap-replay (Core/Tools/gxcap-replay). A window file
// is a header followed by call records; the window's start state is itself a run of records (CREATE / GET / UPLOAD / SET)
// flagged F_SNAPSHOT, so one decoder replays both. Bulk bytes live in a content-addressed blob store shared by the run.
// Little-endian; d3d8 structs are written as their in-memory bytes (LP64 macOS/Linux layouts are identical).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gxcap {

static const char MAGIC[8] = { 'G', 'X', 'C', 'A', 'P', 0, 'v', '1' };
enum : uint32_t { VERSION = 1, INLINE_MAX = 4096 };

// Object kinds and how an object came to exist (the replayer obtains it the same way).
enum Kind : uint8_t { K_NONE = 0, K_TEXTURE, K_CUBE, K_VOLTEX, K_SURFACE, K_VOLUME, K_VB, K_IB, K_SWAPCHAIN };
enum Origin : uint8_t {
	O_CREATE = 0,		// Create* on the device
	O_TEX_LEVEL,		// parent texture, level
	O_CUBE_FACE,		// parent cube, face, level
	O_VOL_LEVEL,		// parent volume texture, level
	O_BACKBUFFER,		// device back buffer <level>
	O_AUTODEPTH,		// the device's implicit depth-stencil surface
	O_SWAP_BACKBUFFER,	// parent swap chain, back buffer <level>
	O_UNKNOWN			// recorder could not classify it: the replayer refuses a window that uses it
};

// Record header flags.
enum : uint16_t {
	F_SNAPSHOT = 1,		// part of the window's start state
	F_GPU = 2,			// UPLOAD: content read back from the GPU at the snapshot (render target / GPU-written)
	F_SHOT = 4,			// COPY_RECTS: source is a back buffer (GX_SHOT / screenshot readback)
	F_NEW = 8,			// GET_*: the result was a new (or resurrected) object: bind its id
	F_REFRESH = 16		// UPLOAD: a CPU-pool copy source re-read right before the copy (writes through an escaped pointer)
};

enum Op : uint16_t {
	OP_END = 0,
	OP_SNAPSHOT_END,
	OP_PRESENT,
	OP_SWAP_PRESENT,
	OP_RESET,
	OP_CREATE_TEXTURE,
	OP_CREATE_VOLTEX,
	OP_CREATE_CUBE,
	OP_CREATE_VB,
	OP_CREATE_IB,
	OP_CREATE_RT,
	OP_CREATE_DS,
	OP_CREATE_IMAGE,
	OP_CREATE_SWAPCHAIN,
	OP_GET_BACKBUFFER,
	OP_GET_RENDERTARGET,
	OP_GET_DEPTHSTENCIL,
	OP_GET_SURFACE_LEVEL,
	OP_GET_CUBE_SURFACE,
	OP_GET_VOLUME_LEVEL,
	OP_SWAP_GET_BACKBUFFER,
	OP_GET_CONTAINER,
	OP_DESTROY,
	OP_UPLOAD,
	OP_LOCK_WRITE,
	OP_LOCK_READ,
	OP_COPY_RECTS,
	OP_UPDATE_TEXTURE,
	OP_GET_FRONT_BUFFER,
	OP_SET_RENDER_TARGET,
	OP_BEGIN_SCENE,
	OP_END_SCENE,
	OP_CLEAR,
	OP_SET_TRANSFORM,
	OP_MULTIPLY_TRANSFORM,
	OP_SET_VIEWPORT,
	OP_SET_MATERIAL,
	OP_SET_LIGHT,
	OP_LIGHT_ENABLE,
	OP_SET_CLIP_PLANE,
	OP_SET_RENDER_STATE,
	OP_SET_CLIP_STATUS,
	OP_SET_TEXTURE,
	OP_SET_TSS,
	OP_SET_PALETTE_ENTRIES,
	OP_SET_CURRENT_PALETTE,
	OP_DRAW_PRIMITIVE,
	OP_DRAW_INDEXED_PRIMITIVE,
	OP_DRAW_PRIMITIVE_UP,
	OP_DRAW_INDEXED_PRIMITIVE_UP,
	OP_PROCESS_VERTICES,
	OP_CREATE_VS,
	OP_SET_VS,
	OP_DELETE_VS,
	OP_SET_VS_CONST,
	OP_SET_STREAM_SOURCE,
	OP_SET_INDICES,
	OP_CREATE_PS,
	OP_SET_PS,
	OP_DELETE_PS,
	OP_SET_PS_CONST,
	OP_SET_GAMMA_RAMP,
	OP_SET_CURSOR_PROPS,
	OP_SET_CURSOR_POS,
	OP_SHOW_CURSOR,
	OP_RESOURCE_DISCARD,
	OP_SET_PRIORITY,
	OP_PRELOAD,
	OP_SET_LOD,
	OP_ADD_DIRTY_RECT,
	OP_ADD_DIRTY_BOX,
	OP_UNSUPPORTED,		// a call the format does not carry (state blocks, patches): payload = its name
	OP_SHOT_BEGIN,		// W3DScreenshot's readback starts: u32 logic frame, u8 requested by GX_SHOT, file name (rest)
	OP_SHOT_END,		// u8 the readback completed
	OP_ENV,				// snapshot: the recording environment (fenv bytes, arch, env vars, adapter) - see Env_Rec
	OP_DIRTY,			// snapshot: a SYSTEMMEM texture's dirty rect (level-0 coordinates), restored after its UPLOAD
	OP_GET_TEXTURE,		// a getter handed back a publicly released object (F_NEW): the replayer re-binds its id
	OP_GET_STREAM_SOURCE,
	OP_GET_INDICES,
	OP__COUNT
};

inline const char *Op_Name(uint16_t op)
{
	static const char *const names[OP__COUNT] = {
		"END", "SNAPSHOT_END", "PRESENT", "SWAP_PRESENT", "RESET", "CREATE_TEXTURE", "CREATE_VOLTEX", "CREATE_CUBE",
		"CREATE_VB", "CREATE_IB", "CREATE_RT", "CREATE_DS", "CREATE_IMAGE", "CREATE_SWAPCHAIN", "GET_BACKBUFFER",
		"GET_RENDERTARGET", "GET_DEPTHSTENCIL", "GET_SURFACE_LEVEL", "GET_CUBE_SURFACE", "GET_VOLUME_LEVEL",
		"SWAP_GET_BACKBUFFER", "GET_CONTAINER", "DESTROY", "UPLOAD", "LOCK_WRITE", "LOCK_READ", "COPY_RECTS",
		"UPDATE_TEXTURE", "GET_FRONT_BUFFER", "SET_RENDER_TARGET", "BEGIN_SCENE", "END_SCENE", "CLEAR", "SET_TRANSFORM",
		"MULTIPLY_TRANSFORM", "SET_VIEWPORT", "SET_MATERIAL", "SET_LIGHT", "LIGHT_ENABLE", "SET_CLIP_PLANE",
		"SET_RENDER_STATE", "SET_CLIP_STATUS", "SET_TEXTURE", "SET_TSS", "SET_PALETTE_ENTRIES", "SET_CURRENT_PALETTE",
		"DRAW_PRIMITIVE", "DRAW_INDEXED_PRIMITIVE", "DRAW_PRIMITIVE_UP", "DRAW_INDEXED_PRIMITIVE_UP", "PROCESS_VERTICES",
		"CREATE_VS", "SET_VS", "DELETE_VS", "SET_VS_CONST", "SET_STREAM_SOURCE", "SET_INDICES", "CREATE_PS", "SET_PS",
		"DELETE_PS", "SET_PS_CONST", "SET_GAMMA_RAMP", "SET_CURSOR_PROPS", "SET_CURSOR_POS", "SHOW_CURSOR",
		"RESOURCE_DISCARD", "SET_PRIORITY", "PRELOAD", "SET_LOD", "ADD_DIRTY_RECT", "ADD_DIRTY_BOX", "UNSUPPORTED",
		"SHOT_BEGIN", "SHOT_END", "ENV", "DIRTY", "GET_TEXTURE", "GET_STREAM_SOURCE", "GET_INDICES"
	};
	return op < OP__COUNT ? names[op] : "?";
}

#pragma pack(push, 1)
struct FileHeader
{
	char magic[8];
	uint32_t version;
	uint32_t logicFrame;	// the GXCAP frame this window was armed for
	uint32_t drawFrame;		// the logic frame at the window's first draw
	uint32_t span;			// updates in the window
	uint32_t flags;			// FH_*
	uint32_t adapter, deviceType, behavior;
	uint32_t ppBackBufferWidth, ppBackBufferHeight, ppBackBufferFormat, ppBackBufferCount;
	uint32_t ppMultiSampleType, ppSwapEffect, ppWindowed, ppEnableAutoDepthStencil, ppAutoDepthStencilFormat;
	uint32_t ppFlags, ppRefreshRate, ppPresentationInterval;
	char buildSha[48];
	char arch[16];			// recorder's CPU/OS (the fenv bytes in OP_ENV are only meaningful on the same one)
};
enum : uint32_t { FH_TRUNCATED = 1, FH_SHOT = 2, FH_UNSUPPORTED = 4 };

struct RecHeader
{
	uint16_t op;
	uint16_t flags;
	uint32_t len;	// payload bytes after this header
	int32_t hr;		// the HRESULT (or count) the call returned in the game
};

struct BlobRef
{
	uint64_t hash;
	uint64_t len;
};

// UPLOAD / LOCK_WRITE / LOCK_READ: where the bytes go. For textures sub = level (cube: face * levels + level); a
// buffer has sub 0 and x0 = byte offset, x1 = byte end. rowBytes/rows/slices describe the packed bytes that follow.
struct Region
{
	uint32_t id, sub, face, level;
	uint32_t x0, y0, x1, y1, z0, z1;	// pixels (buffers: x0/x1 = bytes)
	uint32_t lockFlags;
	uint32_t rowBytes, rows, slices;
};
#pragma pack(pop)

// 64-bit content hash (murmur-style mixing; non-cryptographic, collisions are a replay failure, never silent data).
inline uint64_t Hash64(const void *data, size_t len)
{
	const uint8_t *p = static_cast<const uint8_t *>(data);
	auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
	const uint64_t c1 = 0x87C37B91114253D5ull, c2 = 0x4CF5AD432745937Full;
	uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t(len) * 0xC2B2AE3D27D4EB4Full);
	size_t i = 0;
	for (; i + 8 <= len; i += 8) {
		uint64_t k;
		memcpy(&k, p + i, 8);
		k *= c1; k = rotl(k, 31); k *= c2;
		h ^= k; h = rotl(h, 27) * 5 + 0x52DCE729;
	}
	uint64_t k = 0;
	for (size_t j = 0; i + j < len; ++j)
		k |= uint64_t(p[i + j]) << (8 * j);
	k *= c1; k = rotl(k, 31); k *= c2; h ^= k;
	h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 33;
	return h;
}

inline std::string Blob_Path(const std::string &dir, uint64_t hash, uint64_t len)
{
	char name[64];
	snprintf(name, sizeof(name), "/blobs/%02x/%016llx-%llu.bin", unsigned(hash >> 56), (unsigned long long)hash,
		(unsigned long long)len);
	return dir + name;
}

// Packed layout of a w x h image of a d3d8 format: bytes per row (per block row for DXTn) and rows. ok = false for a
// format this table does not know (the caller marks the content UNDEFINED, never guesses a pitch).
struct Layout { uint32_t rowBytes, rows; bool ok; };
inline Layout Format_Layout(uint32_t format, uint32_t w, uint32_t h)
{
	auto fourcc = [](char a, char b, char c, char d) { return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 |
		uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24; };
	if (w == 0) w = 1;
	if (h == 0) h = 1;
	if (format == fourcc('D', 'X', 'T', '1'))
		return { ((w + 3) / 4) * 8, (h + 3) / 4, true };
	if (format == fourcc('D', 'X', 'T', '2') || format == fourcc('D', 'X', 'T', '3') || format == fourcc('D', 'X', 'T', '4') ||
		format == fourcc('D', 'X', 'T', '5'))
		return { ((w + 3) / 4) * 16, (h + 3) / 4, true };
	if (format == fourcc('U', 'Y', 'V', 'Y') || format == fourcc('Y', 'U', 'Y', '2'))
		return { ((w + 1) / 2) * 4, h, true };
	uint32_t bpp = 0;
	switch (format) {
	case 20: bpp = 3; break;											// R8G8B8
	case 21: case 22: case 31: case 34: bpp = 4; break;					// A8R8G8B8 X8R8G8B8 A2B10G10R10 G16R16
	case 23: case 24: case 25: case 26: case 29: case 30: bpp = 2; break;	// R5G6B5 X1R5G5B5 A1R5G5B5 A4R4G4B4 A8R3G3B2 X4R4G4B4
	case 27: case 28: bpp = 1; break;									// R3G3B2 A8
	case 40: bpp = 2; break;											// A8P8
	case 41: case 50: case 52: bpp = 1; break;							// P8 L8 A4L4
	case 51: bpp = 2; break;											// A8L8
	case 60: case 61: bpp = 2; break;									// V8U8 L6V5U5
	case 62: case 63: case 64: case 65: case 67: bpp = 4; break;		// X8L8V8U8 Q8W8V8U8 V16U16 W11V11U10 A2W10V10U10
	case 70: case 73: case 80: bpp = 2; break;							// D16_LOCKABLE D15S1 D16
	case 71: case 75: case 77: case 79: bpp = 4; break;					// D32 D24S8 D24X8 D24X4S4
	default: return { 0, 0, false };
	}
	return { w * bpp, h, true };
}

inline const char *Arch_Name()
{
#if defined(__APPLE__) && defined(__aarch64__)
	return "macos-arm64";
#elif defined(__APPLE__)
	return "macos-x86_64";
#elif defined(__linux__) && defined(__x86_64__)
	return "linux-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
	return "linux-arm64";
#else
	return "other";
#endif
}

inline bool Is_Depth_Format(uint32_t format)
{
	return format >= 70 && format <= 80;
}

// Growable byte buffer for building records.
struct Buf
{
	std::vector<uint8_t> b;
	void put(const void *p, size_t n) { const uint8_t *q = static_cast<const uint8_t *>(p); b.insert(b.end(), q, q + n); }
	template <class T> void v(const T &x) { put(&x, sizeof(T)); }
	void u32(uint32_t x) { v(x); }
};

// Reader over one record's payload. Every read is bounds-checked; a short payload sets bad and returns zeros.
struct Reader
{
	const uint8_t *p, *end;
	bool bad = false;
	Reader(const uint8_t *b, size_t n) : p(b), end(b + n) {}
	bool get(void *dst, size_t n)
	{
		if (size_t(end - p) < n) { bad = true; memset(dst, 0, n); return false; }
		memcpy(dst, p, n); p += n; return true;
	}
	template <class T> T v() { T x; get(&x, sizeof(T)); return x; }
	uint32_t u32() { return v<uint32_t>(); }
	const uint8_t *take(size_t n)
	{
		if (size_t(end - p) < n) { bad = true; return nullptr; }
		const uint8_t *q = p; p += n; return q;
	}
};

} // namespace gxcap
