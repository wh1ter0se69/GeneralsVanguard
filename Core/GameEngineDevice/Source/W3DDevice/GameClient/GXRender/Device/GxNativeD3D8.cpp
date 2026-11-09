// GeneralsX @feature E · rw3-S 11/10/2026 GxNativeD3D8: see GxNativeD3D8.h and render-seam/DESIGN_l1-device.md §1, §2.
#include "GxNativeD3D8.h"

#include "GxCustomCatalog.h"
#include "GxNativeConfig.h"
#include "GxNativeLoaderDirect.h"
#include "../../../../../Include/W3DDevice/GameClient/GxShadowVolumeVS.h"   // S7: the bytes the game creates (rw3-R11)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <CommonCrypto/CommonDigest.h>
#elif defined(_WIN32)
// GeneralsX @feature D · native-windows-1 13/10/2026 S8 on Windows: the loader images come from the process's module list
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace GXRender { namespace Dev {

static bool s_nativeSessionLive = false;
bool GxNativeSessionLive() { return s_nativeSessionLive; }

namespace {

const char* envOf(const GxNativeSessionHost& h, const char* name)
{
	return h.getEnv ? h.getEnv(name) : std::getenv(name);
}

bool sameGuid(REFIID a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }
const GUID kIidUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
const GUID kIidD3D8 = { 0x1DD9E8DA, 0x1C77, 0x4D40, { 0xB0, 0xCF, 0x98, 0xFE, 0xFD, 0xFF, 0x95, 0x12 } };

std::string baseName(const std::string& p)
{
	const size_t s = p.find_last_of("/\\");   // D · native-windows-1: a Windows module path separates with '\\'
	return s == std::string::npos ? p : p.substr(s + 1);
}
bool isMoltenVK(const std::string& p) { return baseName(p).find("MoltenVK") != std::string::npos; }
bool isLoader(const std::string& p)
{
	std::string b = baseName(p);
	if (b.compare(0, 9, "libvulkan") == 0 && b.find("MoltenVK") == std::string::npos)
		return true;
	// GeneralsX @feature D · native-windows-1 13/10/2026 the Windows loader names in the DXVK fork's search list (VkCtx.cpp
	// platformLoaderNames): vulkan-1.dll, winevulkan.dll. Module names are case-insensitive on Windows.
	std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return char(std::tolower(c)); });
	return b == "vulkan-1.dll" || b == "winevulkan.dll";
}

std::string quoteSafe(std::string s)
{
	std::replace(s.begin(), s.end(), '"', '\'');
	std::replace(s.begin(), s.end(), '\n', ' ');
	return s;
}

// C · native-ipad-1 13/10/2026: the swapchain's actual present mode for the session line (-1 = no swapchain)
const char* presentModeName(int m)
{
	switch (m) {
	case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
	case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
	case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
	case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
	case -1: return "none";
	default: return "other";
	}
}

std::string hex8(uint32_t v)
{
	char b[12];
	std::snprintf(b, sizeof(b), "%x", v);
	return b;
}

std::string hex16(const uint8_t* u)
{
	char b[40];
	for (int i = 0; i < 16; ++i)
		std::snprintf(b + i * 2, 3, "%02x", u[i]);
	return std::string(b, 32);
}

// GX_NATIVE_TEST_FAIL_AT=S<n> | <n>, n in 2..10; 0 = none
int failAtRow(const GxNativeSessionHost& h)
{
	const char* e = envOf(h, "GX_NATIVE_TEST_FAIL_AT");
	if (!e || !*e)
		return 0;
	if (*e == 'S' || *e == 's')
		++e;
	const int n = std::atoi(e);
	return n >= 2 && n <= 10 ? n : 0;
}

} // namespace

// ------------------------------------------------------------------------------------------------------------------- pure rows

bool GxNativeQualifyPresent(const D3DPRESENT_PARAMETERS& pp, std::string* why)
{
	char b[96];
	auto no = [&](const char* fmt, unsigned v) {
		std::snprintf(b, sizeof(b), fmt, v);
		*why = b;
		return false;
	};
	if (!pp.Windowed)
		return no("Windowed=FALSE (fullscreen is not qualified)%u", 0);
	if (pp.BackBufferFormat != D3DFMT_A8R8G8B8 && pp.BackBufferFormat != D3DFMT_X8R8G8B8)
		return no("BackBufferFormat=%u", unsigned(pp.BackBufferFormat));
	if (pp.EnableAutoDepthStencil) {
		const D3DFORMAT f = pp.AutoDepthStencilFormat;
		if (f != D3DFMT_D24S8 && f != D3DFMT_D24X8 && f != D3DFMT_D16 && f != D3DFMT_D32)
			return no("AutoDepthStencilFormat=%u", unsigned(f));
	}
	const unsigned ms = std::max<unsigned>(unsigned(pp.MultiSampleType), 1u);
	if (ms & (ms - 1))   // d3d9_util.cpp:48-50: DXVK answers INVALIDCALL; GXRES would map it to 1
		return no("MultiSampleType=%u (not a power of two)", unsigned(pp.MultiSampleType));
	if (pp.SwapEffect != D3DSWAPEFFECT_DISCARD && pp.SwapEffect != D3DSWAPEFFECT_COPY && pp.SwapEffect != D3DSWAPEFFECT_COPY_VSYNC)
		return no("SwapEffect=%u", unsigned(pp.SwapEffect));
	if (pp.BackBufferCount > 3)
		return no("BackBufferCount=%u", unsigned(pp.BackBufferCount));
	if (!GxD3D8PresentParamsValid(pp)) {   // the two d3d8 checks + d3d9's: DXVK answers INVALIDCALL
		*why = "invalid for d3d8 (DXVK answers INVALIDCALL)";
		return false;
	}
	return true;
}

uint32_t GxNativeDxvkSampleCount(uint32_t requested, uint32_t mask)
{
	uint32_t n = std::max<uint32_t>(requested, 1u);
	while (n > mask && n > 1)
		n >>= 1;
	return n;
}

bool GxNativeUuidMatches(const uint8_t vkUuid[16], const GUID& id)
{
	return std::memcmp(vkUuid, &id, 16) == 0;   // d3d9_adapter.cpp:73: bit::cast<GUID>(vk11.deviceUUID)
}

bool GxNativeQualifyLoader(const GxLoaderFacts& f, bool requireMoltenVK, std::string* loaderPath, std::string* mvkPath,
	std::string* why, bool requireLoader)
{
	std::set<std::string> loaders, mvks;
	std::map<std::string, std::string> loaderBuild;   // loader path -> its build
	const bool haveBuilds = f.builds.size() == f.images.size();
	for (size_t k = 0; k < f.images.size(); ++k) {
		const std::string& i = f.images[k];
		if (isMoltenVK(i))
			mvks.insert(i);
		else if (isLoader(i)) {
			loaders.insert(i);
			loaderBuild[i] = haveBuilds && !f.builds[k].empty() ? f.builds[k] : "path:" + i;
		}
	}
	// B · dlss-streamline-1 (A36.2): the declared interposer stands in front of the one loader (still required below), and only it
	const bool viaInterposer = !f.interposerImage.empty() && (f.procAddrImage == f.interposerImage || f.testInterposerAnyPath);
	if (!viaInterposer && !f.procAddrImage.empty() && !isLoader(f.procAddrImage)) {
		*why = "the host's vkGetInstanceProcAddr is in '" + f.procAddrImage + "', not a Vulkan loader (libvulkan*)";
		return false;
	}
	std::set<std::string> builds;   // DESIGN §2.1 S8: exactly one loader BUILD, at however many paths
	for (const auto& p : loaderBuild)
		builds.insert(p.second);
	if (builds.size() > 1) {
		std::string list;
		for (const auto& p : loaderBuild)
			list += (list.empty() ? "'" : ", '") + p.first + "' " + p.second;
		*why = "more than one Vulkan loader build is loaded (" + list + ")";
		return false;
	}
	if (mvks.size() > 1) {
		*why = "more than one MoltenVK is loaded ('" + *mvks.begin() + "' and '" + *mvks.rbegin() + "')";
		return false;
	}
	if (requireMoltenVK || requireLoader) {
		if (loaders.empty()) {
			*why = "no Vulkan loader image is loaded";
			return false;
		}
		if (requireMoltenVK && mvks.empty()) {
			*why = "no MoltenVK image is loaded after the device was created";
			return false;
		}
		if (!viaInterposer && !f.procAddrImage.empty() && !loaders.count(f.procAddrImage)) {
			*why = "the host's loader '" + f.procAddrImage + "' is not the process's loader '" + *loaders.begin() + "'";
			return false;
		}
	}
	if (loaderPath)   // the entry point actually in use when the host gave one, else the process's loader
		*loaderPath = viaInterposer ? f.procAddrImage + " -> " + (loaders.empty() ? std::string("?") : *loaders.begin())
			: !f.procAddrImage.empty() && loaders.count(f.procAddrImage) ? f.procAddrImage
			: loaders.empty() ? f.procAddrImage : *loaders.begin();
	if (mvkPath)
		*mvkPath = mvks.empty() ? std::string() : *mvks.begin();
	return true;
}

#if defined(__APPLE__)
// The build identity of a loaded image: its LC_UUID (the slice dyld loaded), else the file's sha256, else only its path.
static std::string imageBuildId(const struct mach_header* h, const std::string& path)
{
	char hex[80];
	if (h && (h->magic == MH_MAGIC_64 || h->magic == MH_MAGIC)) {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(h) + (h->magic == MH_MAGIC_64 ? sizeof(mach_header_64) : sizeof(mach_header));
		const uint8_t* const end = p + h->sizeofcmds;   // every command is bounds-checked against the header's own declared size
		for (uint32_t c = 0; c < h->ncmds && size_t(end - p) >= sizeof(load_command); ++c) {
			const load_command* lc = reinterpret_cast<const load_command*>(p);
			if (lc->cmdsize < sizeof(load_command) || lc->cmdsize > size_t(end - p))
				break;
			if (lc->cmd == LC_UUID && lc->cmdsize >= sizeof(uuid_command)) {
				const uuid_command* u = reinterpret_cast<const uuid_command*>(p);
				for (int b = 0; b < 16; ++b)
					std::snprintf(hex + b * 2, 3, "%02x", u->uuid[b]);
				return std::string("uuid:") + hex;
			}
			p += lc->cmdsize;
		}
	}
	if (FILE* fp = std::fopen(path.c_str(), "rb")) {
		CC_SHA256_CTX ctx;
		CC_SHA256_Init(&ctx);
		uint8_t buf[65536];
		size_t n;
		bool ok = true;
		while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
			CC_SHA256_Update(&ctx, buf, CC_LONG(n));
		ok = !std::ferror(fp);
		std::fclose(fp);
		if (ok) {
			uint8_t d[CC_SHA256_DIGEST_LENGTH];
			CC_SHA256_Final(d, &ctx);
			std::string s = "sha256:";
			for (uint8_t b : d) {
				std::snprintf(hex, sizeof(hex), "%02x", b);
				s += hex;
			}
			return s;
		}
	}
	return "path:" + path;
}
#endif

GxLoaderFacts GxNativeLoaderFacts(PFN_vkGetInstanceProcAddr gipa, const void* interposerModule)
{
	GxLoaderFacts f;
#if defined(__APPLE__)
	(void)interposerModule;   // B · dlss-streamline-1: Streamline (the only interposer) is Windows-only
	auto real = [](const char* p) {
		char buf[PATH_MAX];
		return std::string(::realpath(p, buf) ? buf : p);
	};
	if (gipa) {
		Dl_info info = {};
		if (::dladdr(reinterpret_cast<void*>(gipa), &info) && info.dli_fname)
			f.procAddrImage = real(info.dli_fname);
		else
			f.procAddrImage = "?";   // unattributable: never a loader
	}
	const uint32_t n = _dyld_image_count();
	for (uint32_t i = 0; i < n; ++i) {
		const char* name = _dyld_get_image_name(i);
		if (name && (std::strstr(name, "libvulkan") || std::strstr(name, "MoltenVK"))) {
			f.images.push_back(real(name));
			f.builds.push_back(imageBuildId(_dyld_get_image_header(i), f.images.back()));
		}
	}
#elif defined(_WIN32)
	// GeneralsX @feature D · native-windows-1 13/10/2026 the Windows facts: the module holding the host's entry point, and every
	// loaded module named like a loader. A module's BUILD = the sha256 of its file (the Mac fallback's identity; a content hash, so
	// a patched copy keeping its PE timestamp and size is still a second build - Codex nw1-r1). Each listed module is PINNED
	// (GetModuleHandleExW without UNCHANGED_REFCOUNT) while its path is read, so an unload between the snapshot and the read cannot
	// leave us reading a freed mapping; no PE header is parsed at all.
	auto utf8 = [](const std::wstring& w) {
		const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(size_t(len > 0 ? len : 0), '\0');
		if (len > 0)
			WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), &s[0], len, nullptr, nullptr);
		return s;
	};
	auto pathW = [](HMODULE m) {
		std::wstring w(32768, L'\0');
		const DWORD n = GetModuleFileNameW(m, &w[0], DWORD(w.size()));
		w.resize(n > 0 && n < w.size() ? n : 0);
		return w;
	};
	auto fileSha256 = [](const std::wstring& path) {
		std::string out;
		const HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (fh == INVALID_HANDLE_VALUE)
			return out;
		BCRYPT_ALG_HANDLE alg = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
			BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
		static thread_local uint8_t buf[65536];
		for (DWORD got = 0; ok;) {   // a failed read fails the hash: never an identity of a prefix (Codex nw1-r2)
			if (!ReadFile(fh, buf, sizeof(buf), &got, nullptr)) {
				ok = false;
				break;
			}
			if (got == 0)
				break;
			ok = BCryptHashData(hash, buf, got, 0) == 0;
		}
		uint8_t d[32];
		if (ok && BCryptFinishHash(hash, d, sizeof(d), 0) == 0) {
			out = "sha256:";
			char hx[3];
			for (uint8_t b : d) {
				std::snprintf(hx, sizeof(hx), "%02x", b);
				out += hx;
			}
		}
		if (hash)
			BCryptDestroyHash(hash);
		if (alg)
			BCryptCloseAlgorithmProvider(alg, 0);
		CloseHandle(fh);
		return out;
	};
	if (gipa) {
		HMODULE m = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(reinterpret_cast<void*>(gipa)), &m))
			f.procAddrImage = utf8(pathW(m));
		if (f.procAddrImage.empty())
			f.procAddrImage = "?";   // unattributable: never a loader
	}
	if (interposerModule)   // B · dlss-streamline-1: the same utf8(pathW()) as procAddrImage, so the two compare byte for byte
		f.interposerImage = utf8(pathW(static_cast<HMODULE>(const_cast<void*>(interposerModule))));
	HANDLE snap = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 8 && snap == INVALID_HANDLE_VALUE; ++attempt) {   // ERROR_BAD_LENGTH: the list changed, retry
		snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
		if (snap == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH)
			break;
	}
	if (snap != INVALID_HANDLE_VALUE) {
		MODULEENTRY32W me = {};
		me.dwSize = sizeof(me);
		for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
			HMODULE pinned = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(me.modBaseAddr), &pinned))
				continue;   // unloaded since the snapshot: not loaded any more
			const std::wstring w = pathW(pinned);
			const std::string path = utf8(w);
			if (!path.empty() && (isLoader(path) || isMoltenVK(path))) {
				f.images.push_back(path);
				const std::string id = fileSha256(w);
				f.builds.push_back(id.empty() ? "path:" + path : id);
			}
			FreeLibrary(pinned);
		}
		CloseHandle(snap);
	}   // no snapshot after the retries: an empty census, which the post-creation S8 refuses ("no Vulkan loader image is loaded")
#else
	(void)gipa;
	(void)interposerModule;
#endif
	return f;
}

bool GxNativeQualifyShaders(const GxNativeSessionHost& host, std::string* why)
{
	if (!host.readFile) {
		*why = "no file reader (the game's file system is the only truth for a shader override)";
		return false;
	}
	size_t count = 0;
	const GxCsEntry* entries = GxCsEntries(&count);
	std::vector<GxCsLoad> loads;
	for (size_t i = 0; i < count; ++i) {
		const GxCsEntry& e = entries[i];
		// unported: never loaded; water: compiled into the binary; gx: synthetic programs created by the game's own code
		// (Pipeline/GxSyntheticPrograms.h, THE rule for every synthetic program - render-dlss/DLSS_INTEGRATION.md 13/10)
		if (e.program == 0 || std::strncmp(e.files, "water:", 6) == 0 || std::strncmp(e.files, "gx:", 3) == 0)
			continue;
		std::string name = e.files;
		name = name.substr(0, name.find('|'));
		GxCsLoad l;
		l.path = "shaders\\" + name;
		l.pixel = e.pixel != 0;
		std::vector<uint8_t> bytes;
		l.found = host.readFile(l.path.c_str(), &bytes, host.user);
		if (l.found && (bytes.empty() || bytes.size() % 4 != 0)) {   // W3DShaderManager::LoadAndCreateD3DShader rejects such a file
			*why = "shader file " + l.path + " is not a whole number of DWORDs (" + std::to_string(bytes.size()) + " bytes)";
			return false;
		}
		if (l.found) {
			l.tokens.resize(bytes.size() / 4);
			if (!bytes.empty())
				std::memcpy(l.tokens.data(), bytes.data(), l.tokens.size() * 4);
		}
		if (!l.pixel && e.decl)
			l.decl.assign(e.decl, e.decl + e.declCount);
		loads.push_back(std::move(l));
	}
	const GxCsSession s = GxCsSessionVerdict(loads);
	if (s.classic) {
		*why = s.why;
		return false;
	}
	if (!s.complete) {
		*why = "a loaded shader has no port: " + (s.unported.empty() ? std::string("?") : s.unported.front());
		return false;
	}
	return true;
}

// -------------------------------------------------------------------------------------------------------------------- proxy

namespace {
std::mutex g_proxyMutex;
std::set<const IDirect3D8*> g_proxies;
}

class GxNativeD3D8 final : public IDirect3D8
{
public:
	GxNativeD3D8(IDirect3D8* dxvk, const GxNativeHost& host, const GxNativeSessionHost& session, const GxNativeSessionDebug* debug)
		: m_dxvk(dxvk), m_host(host), m_session(session)
	{
		if (debug)
			m_debug = *debug;
	}
	~GxNativeD3D8()
	{
		{
			std::lock_guard<std::mutex> g(g_proxyMutex);
			g_proxies.erase(this);   // a later object at this address must not be mistaken for a proxy
		}
		if (m_dxvk)
			m_dxvk->Release();
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
	{
		if (!pp)
			return E_POINTER;
		if (sameGuid(riid, kIidUnknown) || sameGuid(riid, kIidD3D8)) {
			AddRef();
			*pp = static_cast<IDirect3D8*>(this);
			return S_OK;
		}
		*pp = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG r = --m_ref;
		if (r == 0)
			delete this;
		return r;
	}

	// ---- forwarded: DXVK's answers by construction (DESIGN §4a)
	HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* f) override { return m_dxvk->RegisterSoftwareDevice(f); }
	UINT STDMETHODCALLTYPE GetAdapterCount() override { return m_dxvk->GetAdapterCount(); }
	HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT a, DWORD f, D3DADAPTER_IDENTIFIER8* id) override
	{
		return m_dxvk->GetAdapterIdentifier(a, f, id);
	}
	UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT a) override { return m_dxvk->GetAdapterModeCount(a); }
	HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT a, UINT m, D3DDISPLAYMODE* d) override { return m_dxvk->EnumAdapterModes(a, m, d); }
	HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT a, D3DDISPLAYMODE* d) override { return m_dxvk->GetAdapterDisplayMode(a, d); }
	HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT a, D3DDEVTYPE t, D3DFORMAT df, D3DFORMAT bf, WINBOOL w) override
	{
		return m_dxvk->CheckDeviceType(a, t, df, bf, w);
	}
	HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD u, D3DRESOURCETYPE r, D3DFORMAT cf) override
	{
		return m_dxvk->CheckDeviceFormat(a, t, af, u, r, cf);
	}
	HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT a, D3DDEVTYPE t, D3DFORMAT sf, WINBOOL w, D3DMULTISAMPLE_TYPE m) override
	{
		return m_dxvk->CheckDeviceMultiSampleType(a, t, sf, w, m);
	}
	HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df) override
	{
		return m_dxvk->CheckDepthStencilMatch(a, t, af, rf, df);
	}
	HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT a, D3DDEVTYPE t, D3DCAPS8* c) override { return m_dxvk->GetDeviceCaps(a, t, c); }
	HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT a) override { return m_dxvk->GetAdapterMonitor(a); }

	HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS* pp,
		IDirect3DDevice8** out) override
	{
		if (!pp || !out)
			return m_dxvk->CreateDevice(adapter, type, focus, behavior, pp, out);
		GxD3D8NormalizePresentParams(pp);   // as DXVK's CreateDevice does (d3d8_interface.cpp:138), before S4 reads it
		IDirect3DDevice8* dev = nullptr;
		GxNativeSession s = decide(adapter, type, focus, behavior, *pp, &dev);
		emit(s.line);
		m_last = s;
		if (s.native) {
			*out = dev;
			return D3D_OK;
		}
		// A refused session: the native objects are already gone (decide() released the half-built device, §2.2) - DXVK now owns the window.
		return m_dxvk->CreateDevice(adapter, type, focus, behavior, pp, out);
	}

	const GxNativeSession& last() const { return m_last; }

private:
	void emit(const std::string& line)
	{
		if (m_host.log)
			m_host.log(line.c_str());
		else
			std::fprintf(stderr, "%s\n", line.c_str());
	}

	GxNativeSession classic(int row, const std::string& reason)
	{
		GxNativeSession s;
		s.failedRow = row;
		s.reason = reason;
		s_nativeSessionLive = false;	// this device is classic, whatever an earlier one was
		s.line = "[GXNATIVE] session=classic reason=\"" + quoteSafe(reason) + "\" row=S" + std::to_string(row);
		return s;
	}

	// Rows S2..S10 in order. On a "no" the half-built device is Released BEFORE returning (DESIGN §2.2: the caller forwards after).
	GxNativeSession decide(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, const D3DPRESENT_PARAMETERS& pp,
		IDirect3DDevice8** outDev)
	{
		const int failAt = failAtRow(m_session);
		auto forced = [&](int row) { return failAt == row; };
		auto forcedReason = [&](int row) { return "forced by GX_NATIVE_TEST_FAIL_AT=S" + std::to_string(row); };

		// S2 platform
		{
			bool ok = false;
#if (defined(__APPLE__) && (TARGET_OS_OSX || (TARGET_OS_IOS && !TARGET_OS_SIMULATOR && !TARGET_OS_MACCATALYST))) || (defined(_WIN32) && defined(_WIN64))
			ok = true;   // D · native-windows-1 13/10/2026: Windows x64 qualifies (the MSVC build, GxNativeHostWin32); C · native-ipad-1 13/10/2026: iOS devices qualify (SDL3 UIKit host, MoltenVK bound directly: S8 below)
#endif
			if (m_debug.platformUnqualified)
				ok = false;
			if ((!ok && !m_debug.skipPlatformRow) || forced(2))
				return classic(2, forced(2) ? forcedReason(2) : "platform not qualified");
		}
		// S3 conflicting instruments
		if (!m_debug.skipEnvRow || forced(3)) {
			static const struct { const char* var; const char* why; } kVars[] = {
				{ "GXCAP", "GXCAP set: the capture recorder is a classic-only instrument" },
				{ "GX_RENDER_HYBRID", "GX_RENDER_HYBRID set: hybrid L2 draws into the classic device only" },
				{ "GX_VK_PRESENT_MODE", "GX_VK_PRESENT_MODE set: it overrides the present mode the device derives from the d3d8 parameters" },
				{ "GX_VK_DEVICE", "GX_VK_DEVICE set: the device must be DXVK's adapter (the UUID gate would refuse another)" },
			};
			for (const auto& v : kVars)
				if (!m_debug.skipEnvRow && envOf(m_session, v.var) != nullptr)
					return classic(3, v.why);
			// DXVK_HUD (set, not empty, not "0"): DXVK composites its HUD in the present; the native present has none (Codex rw3L-design-r3 #6)
			const char* hud = envOf(m_session, "DXVK_HUD");
			if (!m_debug.skipEnvRow && hud && *hud && std::strcmp(hud, "0") != 0)
				return classic(3, "DXVK_HUD set: the native present has no HUD");
			if (forced(3))
				return classic(3, forcedReason(3));
		}
		// S4 present parameters
		{
			std::string why;
			if ((!m_debug.skipPresentRow && !GxNativeQualifyPresent(pp, &why)) || forced(4))
				return classic(4, forced(4) ? forcedReason(4) : "present parameters: " + why);
		}
		// S5 the effective DXVK configuration (DESIGN §5.2)
		GxNativeConfig cfg = GxNativeLoadConfig();
		if ((!m_debug.skipConfigRow && !cfg.refused.empty()) || forced(5))
			return classic(5, forced(5) ? forcedReason(5) : cfg.refused);
		// S6 the shader files the game loads
		{
			std::string why;
			if ((!m_debug.skipShaderRow && !GxNativeQualifyShaders(m_session, &why)) || forced(6))
				return classic(6, forced(6) ? forcedReason(6) : why);
		}
		// S7 GeneralsX features the device does not draw: GX_SHADOW_GPU (W3DVolumetricShadow.cpp gxShadowGpuMode: '0'..'2' else the
		// game's default). C · rw3-R11 13/10/2026: the default is the game's PLATFORM default, 1 on _WIN32 (it read 0 everywhere, so a
		// Windows session with the variable unset qualified and then had every GPU shadow draw refused: missing shadows, not classic).
		{
#if defined(_WIN32)
			const int def = 1;
#else
			const int def = 0;
#endif
			const char* e = envOf(m_session, "GX_SHADOW_GPU");
			const int mode = (e && e[0] >= '0' && e[0] <= '2' && e[1] == '\0') ? e[0] - '0' : def;
			// rw3-R11: the port is in the catalogue (gx:shadowvol). S7 resolves the very bytes the game will create, so a shader edit
			// that was never re-catalogued sends the session to classic instead of dropping every GPU shadow draw.
			const bool ported = !m_debug.shadowPortMissing && GxCsResolveVertex(kGxShadowVS, sizeof(kGxShadowVS) / 4, kGxShadowDecl,
				sizeof(kGxShadowDecl) / 4).verdict == GxCsPorted;
			if ((!m_debug.skipShadowRow && mode != 0 && !ported) || forced(7))
				return classic(7, forced(7) ? forcedReason(7) : "GX_SHADOW_GPU=" + std::to_string(mode) + " (shadow VS not ported)");
		}
		// S8 (before the device): the host's loader must BE a Vulkan loader
		GxLoaderFacts pre = GxNativeLoaderFacts(m_host.getInstanceProcAddr, m_host.loaderInterposer);
		pre.testInterposerAnyPath = m_debug.s8InterposerAnyPath;
		if (m_debug.fakeProcAddrImage)
			pre.procAddrImage = m_debug.fakeProcAddrImage;
		{
			std::string why;
#if defined(__APPLE__) && TARGET_OS_IOS
			auto qualify = [&] { return GxNativeQualifyLoaderDirect(pre, false, nullptr, &why); };   // no loader on iOS (GxNativeLoaderDirect.h)
#else
			auto qualify = [&] { return GxNativeQualifyLoader(pre, false, nullptr, nullptr, &why); };
#endif
			if ((!m_debug.skipLoaderRow && !qualify()) || forced(8))
				return classic(8, forced(8) ? forcedReason(8) : "loader: " + why);
		}
		// S9 (extent, before the device is built): the swapchain extent must equal the back buffer's - v1 qualifies only the 1:1 case
		HWND window = pp.hDeviceWindow ? pp.hDeviceWindow : focus;
		if (m_host.drawableSize && window) {
			uint32_t w = 0, h = 0;
			m_host.drawableSize(window, &w, &h);
			if (pp.BackBufferWidth && pp.BackBufferHeight && (w != pp.BackBufferWidth || h != pp.BackBufferHeight))
				return classic(9, "extent " + std::to_string(w) + "x" + std::to_string(h) + " != back buffer " +
					std::to_string(pp.BackBufferWidth) + "x" + std::to_string(pp.BackBufferHeight));
		}

		// the DXVK adapter: its description picks the Vulkan device, its DeviceIdentifier is the UUID gate (§1)
		D3DADAPTER_IDENTIFIER8 id = {};
		const bool haveId = SUCCEEDED(m_dxvk->GetAdapterIdentifier(adapter, 0, &id));
		m_deviceSelect = haveId ? id.Description : "";
		GxNativeHost host = m_host;
		host.samplerAnisotropy = cfg.samplerAnisotropy;
		host.presentInterval = cfg.presentInterval;   // D · native-windows-1: the shipped Windows dxvk.conf's present keys
		host.tearFree = cfg.tearFree;
		if (haveId && !m_debug.skipUuidRow)
			host.requireDeviceUuid = reinterpret_cast<const uint8_t*>(&id.DeviceIdentifier);   // the device checks it right after VkCtx::init (S9)
		if (!host.deviceSelect && haveId && !m_deviceSelect.empty())
			host.deviceSelect = m_deviceSelect.c_str();

		// S9/S10: VkCtx, surface, swapchain, pipeline set - the device builds them in order and tears them down itself on failure
		std::string why;
		IDirect3DDevice8* dev = nullptr;
		const HRESULT hr = GxNativeCreateDevice(this, adapter, type, focus, behavior, pp, host, &dev, &why);
		if (FAILED(hr) || !dev)
			return classic(why.rfind("pipeline set", 0) == 0 ? 10 : 9, why.empty() ? "native device creation failed" : why);   // S9: VkCtx / uuid / surface / swapchain
		GxNativeDevice* nd = GxNativeFromDevice(dev);
		auto refuse = [&](int row, const std::string& reason) {
			GxNativeSession c = classic(row, reason);
			if (m_debug.noTeardown)
				c.leakedDevice = dev;   // TEST-ONLY: the caller releases it; the proxy keeps no reference cycle with it
			else
				dev->Release();   // swapchain, surface, GXRES, VkCtx - in that order (the device's destructor, §2.2)
			return c;
		};
		// S8 (after): exactly one loader and one MoltenVK, the host's loader being that loader
		std::string loaderPath, mvkPath;
		GxLoaderFacts post = GxNativeLoaderFacts(m_host.getInstanceProcAddr, m_host.loaderInterposer);
		post.testInterposerAnyPath = m_debug.s8InterposerAnyPath;
		if (post.procAddrImage.empty() && nd) {   // no host loader: attribute the library VkCtx actually loaded (DESIGN §2.1 S8; never unattributed)
			std::string used = nd->ctx().loaderPath();
			const size_t paren = used.find(" (");
			if (paren != std::string::npos)
				used.resize(paren);
			post.procAddrImage = used.empty() ? std::string("?") : used;
#if defined(__APPLE__)
			char buf[PATH_MAX];
			if (!used.empty() && ::realpath(used.c_str(), buf))
				post.procAddrImage = buf;
#endif
		}
		{
			std::string w;
#if defined(__APPLE__) && TARGET_OS_IOS
			auto qualify = [&] {
				const bool ok = GxNativeQualifyLoaderDirect(post, true, &mvkPath, &w);
				loaderPath = mvkPath;   // the entry point IS MoltenVK
				return ok;
			};
#else
#if defined(__APPLE__)
			const bool requireMvk = true;
#else
			const bool requireMvk = false;   // D · native-windows-1: no MoltenVK off Apple; the loader is still required
#endif
			auto qualify = [&] { return GxNativeQualifyLoader(post, requireMvk, &loaderPath, &mvkPath, &w, true); };
#endif
			if (!m_debug.skipLoaderRow && !qualify())
				return refuse(8, "loader: " + w);
		}
		// S4 (MSAA parity): DXVK decodes the request to max(n,1) and only halves while it exceeds the supported MASK as a number
		// (d3d9_util.cpp:41-65); GXRES picks a supported bit. On a sparse mask they differ: refuse unless the device grants DXVK's count.
		if (nd && !m_debug.skipMsaaParity) {
			const VkSampleCountFlags mask = m_debug.fakeSampleMask ? m_debug.fakeSampleMask :
				(nd->ctx().limits().framebufferColorSampleCounts & nd->ctx().limits().framebufferDepthSampleCounts);
			const uint32_t want = GxNativeDxvkSampleCount(uint32_t(pp.MultiSampleType), mask);
			if (!(mask & want))
				return refuse(4, "present parameters: MultiSampleType=" + std::to_string(unsigned(pp.MultiSampleType)) +
					" (DXVK grants " + std::to_string(want) + " samples, the device supports mask 0x" + hex8(mask) + ")");
		}
		// S9 (extent, actual): the swapchain VkSwapchain really created must be the back buffer's size (it may clamp or take the surface's)
		if (nd && !m_debug.skipExtentRow) {
			uint32_t sw = 0, sh = 0;
			if (nd->swapchainExtent(&sw, &sh) && pp.BackBufferWidth && pp.BackBufferHeight && (sw != pp.BackBufferWidth || sh != pp.BackBufferHeight))
				return refuse(9, "extent " + std::to_string(sw) + "x" + std::to_string(sh) + " != back buffer " +
					std::to_string(pp.BackBufferWidth) + "x" + std::to_string(pp.BackBufferHeight));
		}
		// S9 (UUID) was checked inside the device, before any surface existed; a missing DXVK identity cannot be compared
		const uint8_t* uuid = nd ? nd->deviceUuid() : nullptr;
		if (!m_debug.skipUuidRow && (!haveId || !nd))
			return refuse(9, "uuid mismatch: DXVK's adapter identity is unavailable");
		if (forced(9))
			return refuse(9, forcedReason(9));
		if (forced(10))
			return refuse(10, forcedReason(10));

		GxNativeSession s;
		s.native = true;
		s_nativeSessionLive = true;
		const bool fifo = cfg.presentInterval >= 0 ? cfg.presentInterval > 0 : pp.SwapEffect == D3DSWAPEFFECT_COPY_VSYNC;
		char line[2048];
		std::snprintf(line, sizeof(line),
			"[GXNATIVE] session=native device=\"%s\" uuid=%s loader=%s mvk=%s vk_images=%zu msaa=%u present=%s aniso=%d config=\"%s\" extent=%ux%u vk_present=%s",
			quoteSafe(nd ? std::string(nd->ctx().properties().deviceName) : std::string()).c_str(), (uuid ? hex16(uuid) : std::string()).c_str(), loaderPath.c_str(), mvkPath.c_str(),
			post.images.size(), unsigned(pp.MultiSampleType), fifo ? "FIFO" : cfg.tearFree == 1 ? "MAILBOX" : "IMMEDIATE", cfg.samplerAnisotropy,
			quoteSafe(cfg.sources).c_str(), unsigned(pp.BackBufferWidth), unsigned(pp.BackBufferHeight),
			nd ? presentModeName(nd->swapchainPresentMode()) : "none");   // C · native-ipad-1: the mode VkSwapchain chose, not the request
		s.line = line;
		*outDev = dev;
		return s;
	}

	std::atomic<ULONG> m_ref { 1 };
	IDirect3D8* m_dxvk;
	GxNativeHost m_host;
	GxNativeSessionHost m_session;
	GxNativeSessionDebug m_debug;
	GxNativeSession m_last;
	std::string m_deviceSelect;
};

IDirect3D8* GxNativeWrapInterface(IDirect3D8* dxvk, const GxNativeHost& host, const GxNativeSessionHost& session,
	const GxNativeSessionDebug* debug)
{
	if (!dxvk)
		return dxvk;
	GxNativeD3D8* p = new GxNativeD3D8(dxvk, host, session, debug);
	std::lock_guard<std::mutex> g(g_proxyMutex);
	g_proxies.insert(p);
	return p;
}

GxNativeD3D8* GxNativeProxyFrom(IDirect3D8* d3d)
{
	std::lock_guard<std::mutex> g(g_proxyMutex);
	return g_proxies.count(d3d) ? static_cast<GxNativeD3D8*>(d3d) : nullptr;
}

const GxNativeSession& GxNativeLastSession(const GxNativeD3D8* proxy) { return proxy->last(); }

}} // namespace GXRender::Dev
