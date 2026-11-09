// A23-B (D · rw3-X-2 13/10/2026): GX_EXEC_DIGEST=<file> writes each arm's EXEC digest and creation ledger there, one line per
// arm, for the suite-level equality gate (run the suite with GX_NATIVE_CS=0, again 0, then 1 and compare the files).
// GeneralsX @test E · rw3-S 11/10/2026 gx_proxy_test: the IDirect3D8 proxy and the session rows S2-S10 (DESIGN_l1-device.md §2),
// offscreen, on a stub "DXVK" parent that counts the calls forwarded to it. One case per row; every case has an X- arm that turns
// on the matching GxNativeSessionDebug plant in the PRODUCT proxy and must make exactly that case fail.
//
//   gx_proxy_test [--arm NAME[,NAME...]|all]
#include "GxNativeD3D8.h"
#include "GxNativeLoaderDirect.h"

#include "GxCustomCatalog.h"
#include "gx_exec_digest.h"

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace GXRender;
using namespace GXRender::Dev;

namespace {

void say(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stdout, fmt, ap);
	va_end(ap);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

// The "DXVK" interface: forwards are counted; CreateDevice returns D3D_OK with no device (a "classic frame presented" marker).
bool g_seen = false;   // the X- arm's PLANT was what changed the outcome (a native session / a live device), not some other failure

struct Stub final : IDirect3D8
{
	int forwardedCreate = 0, adapterCount = 0, caps = 0, liveAtForward = -1;
	GUID identifier = {};
	char description[MAX_DEVICE_IDENTIFIER_STRING] = "stub";
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** pp) override { *pp = nullptr; return E_NOINTERFACE; }
	ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
	ULONG STDMETHODCALLTYPE Release() override { return --ref; }
	HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override { return D3DERR_INVALIDCALL; }
	UINT STDMETHODCALLTYPE GetAdapterCount() override { ++adapterCount; return 7; }
	HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER8* id) override
	{
		std::memset(id, 0, sizeof(*id));
		std::snprintf(id->Description, sizeof(id->Description), "%s", description);
		id->DeviceIdentifier = identifier;
		return D3D_OK;
	}
	UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT) override { return 1; }
	HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT, UINT, D3DDISPLAYMODE* m) override { return GetAdapterDisplayMode(0, m); }
	HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT, D3DDISPLAYMODE* m) override
	{
		m->Width = 640, m->Height = 480, m->RefreshRate = 60, m->Format = D3DFMT_X8R8G8B8;
		return D3D_OK;
	}
	HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
	HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS8* c) override
	{
		++caps;
		std::memset(c, 0, sizeof(*c));
		c->MaxTextureBlendStages = 8, c->MaxSimultaneousTextures = 8, c->MaxStreams = 16;
		return D3D_OK;
	}
	HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT) override { return nullptr; }
	HRESULT STDMETHODCALLTYPE CreateDevice(UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice8** out) override
	{
		++forwardedCreate;
		liveAtForward = g_gxNativeLiveDevices.load();   // native objects alive WHEN DXVK is asked (teardown must precede it)
		*out = nullptr;
		return D3D_OK;
	}
	ULONG ref = 1;
};

// ---- the injected environment and the shader "file system" (the catalogue's own bytes: the ported set the game ships)
std::map<std::string, std::string> g_env;
const char* getEnv(const char* n)
{
	auto it = g_env.find(n);
	return it == g_env.end() ? nullptr : it->second.c_str();
}

enum class Files { Good, MissingTerrain, OverrideTerrain, NoReader, TrailingBytes };
Files g_files = Files::Good;

std::string lower(std::string s)
{
	for (char& c : s)
		c = char(std::tolower((unsigned char)c));
	return s;
}

bool readFile(const char* path, std::vector<uint8_t>* out, void*)
{
	std::string p = lower(path);
	if (p.compare(0, 8, "shaders\\") == 0)
		p = p.substr(8);
	if (g_files == Files::MissingTerrain && p == "terrain.pso")
		return false;
	size_t n = 0;
	const GxCsEntry* es = GxCsEntries(&n);
	for (size_t i = 0; i < n; ++i) {
		std::string name = lower(es[i].files);
		name = name.substr(0, name.find('|'));
		if (name != p)
			continue;
		out->resize(size_t(es[i].count) * 4);
		std::memcpy(out->data(), es[i].dwords, out->size());
		if (g_files == Files::OverrideTerrain && p == "terrain.pso")
			(*out)[8] ^= 0x01;   // a modded terrain.pso: same name, one bit off
		if (g_files == Files::TrailingBytes && p == "terrain.pso")
			out->push_back(0);   // the catalogue's bytes plus one: the game's loader rejects it
		return true;
	}
	return false;
}

struct Fixture
{
	std::string deviceName;
	uint8_t uuid[16] = {};
	bool ok = false;
};

// The real Vulkan device's name and UUID (a throwaway VkCtx that is shut down before any proxy session creates its own).
Fixture probeDevice()
{
	Fixture f;
	Vk::VkCtx ctx;
	Vk::VkCtxDesc d;
	std::string why;
	if (!ctx.init(d, &why)) {
		say("probe: VkCtx init failed: %s", why.c_str());
		return f;
	}
	VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
	VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	p2.pNext = &idp;
	vkGetPhysicalDeviceProperties2(ctx.physicalDevice(), &p2);
	std::memcpy(f.uuid, idp.deviceUUID, 16);
	f.deviceName = ctx.properties().deviceName;
	f.ok = true;
	ctx.shutdown();
	return f;
}

Fixture g_dev;

struct Run
{
	GxNativeSession session;
	int forwarded = 0, liveAtForward = -1;
	int liveAfter = 0;   // live native devices after the call returned (before the case releases anything)
	std::vector<std::string> lines;
	D3DPRESENT_PARAMETERS after = {};   // the caller's struct after CreateDevice (DXVK writes its conversion back into it)
};
std::vector<std::string>* g_lines = nullptr;
void collect(const char* l) { if (g_lines && std::strncmp(l, "[GXNATIVE] session=", 19) == 0) g_lines->push_back(l); }   // the device logs through the same host.log

D3DPRESENT_PARAMETERS goodPP()
{
	D3DPRESENT_PARAMETERS pp = {};
	pp.BackBufferWidth = 64;
	pp.BackBufferHeight = 64;
	pp.BackBufferFormat = D3DFMT_A8R8G8B8;
	pp.BackBufferCount = 1;
	pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
	pp.Windowed = TRUE;
	pp.EnableAutoDepthStencil = TRUE;
	pp.AutoDepthStencilFormat = D3DFMT_D24S8;
	return pp;
}

// One CreateDevice through a fresh proxy over a fresh stub; releases the native device it may have returned (one live device at a time).
Run run(const D3DPRESENT_PARAMETERS& pp, const GxNativeSessionDebug& dbg, bool matchUuid = true, PFN_vkGetInstanceProcAddr gipa = nullptr)
{
	Run r;
	Stub stub;
	std::memcpy(&stub.identifier, g_dev.uuid, 16);
	if (!matchUuid)
		stub.identifier.Data1 ^= 0xdeadbeef;
	std::snprintf(stub.description, sizeof(stub.description), "%s", g_dev.deviceName.c_str());
	GxNativeHost host;
	host.prebuildGenericSet = false;   // the generic-set prebuild is S10's real work: covered by the positive case and the Mini
	host.getInstanceProcAddr = gipa;
	host.log = collect;
	GxNativeSessionHost sh;
	sh.readFile = g_files == Files::NoReader ? nullptr : readFile;
	sh.getEnv = getEnv;
	g_lines = &r.lines;
	IDirect3D8* proxy = GxNativeWrapInterface(&stub, host, sh, &dbg);
	stub.AddRef();   // the proxy owns one reference to the stub; keep our own across proxy destruction
	D3DPRESENT_PARAMETERS p = pp;
	IDirect3DDevice8* dev = nullptr;
	proxy->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, 0, &p, &dev);
	r.after = p;
	r.session = GxNativeLastSession(GxNativeProxyFrom(proxy));
	r.forwarded = stub.forwardedCreate;
	r.liveAtForward = stub.liveAtForward;
	r.liveAfter = g_gxNativeLiveDevices.load();
	if (dev)
		dev->Release();
	if (r.session.leakedDevice)
		r.session.leakedDevice->Release();   // the noTeardown plant's refused device
	proxy->Release();
	g_lines = nullptr;
	return r;
}

#define CHECK(cond, what) do { if (!(cond)) { *d += std::string(" ") + (what); ok = false; } } while (0)

bool classicAt(const Run& r, int row, const char* needle, std::string* d)
{
	bool ok = true;
	CHECK(!r.session.native, ("session is native, want classic at S" + std::to_string(row)).c_str());
	CHECK(r.session.failedRow == row, ("failed row " + std::to_string(r.session.failedRow) + ", want " + std::to_string(row) + " (" + r.session.reason + ")").c_str());
	CHECK(needle == nullptr || r.session.reason.find(needle) != std::string::npos, ("reason '" + r.session.reason + "' lacks '" + (needle ? needle : "") + "'").c_str());
	CHECK(r.forwarded == 1, "DXVK's CreateDevice was not called exactly once (no classic frame would present)");
	CHECK(r.liveAtForward == 0, "a native device was still live when DXVK's CreateDevice ran (teardown must precede it)");
	CHECK(r.liveAfter == 0, "a native device is still live after the refusal (teardown)");
	if (r.session.native || r.liveAtForward > 0 || r.liveAfter > 0 || r.session.leakedDevice)   // strictly positive: 'not sampled' (-1) is no evidence
		g_seen = true;
	CHECK(r.lines.size() == 1 && r.lines[0].find("session=classic") != std::string::npos, "not exactly one session=classic line");
	return ok;
}

void setEnvVar(const char* k, const char* v)   // D · native-windows-1: MSVC has no setenv
{
#if defined(_WIN32)
	_putenv_s(k, v);
#else
	setenv(k, v, 1);
#endif
}

void clean()
{
	g_env.clear();
	g_files = Files::Good;
#if defined(_WIN32)
	_putenv_s("DXVK_CONFIG", "");        // MSVC: an empty value removes the variable
	_putenv_s("DXVK_CONFIG_FILE", "");
	g_env["GX_SHADOW_GPU"] = "0";        // the game's Windows default is 1 (S7 refuses it): every other row's arm runs with it off;
	                                     // caseS7 tests the unset variable itself
#else
	unsetenv("DXVK_CONFIG");
	unsetenv("DXVK_CONFIG_FILE");
#endif
}

// ------------------------------------------------------------------------------------------------------------ cases

typedef bool (*CaseFn)(GxNativeSessionDebug& plant, std::string* d);
struct Case
{
	const char* name;
	CaseFn fn;
	void (*setPlant)(GxNativeSessionDebug&);   // the X- arm's plant; null = no arm
};

bool casePositive(GxNativeSessionDebug& p, std::string* d)
{
	bool ok = true;
	GxNativeSessionDebug dbg = p;
	D3DPRESENT_PARAMETERS pp = goodPP();
	// the real thing once: the generic-set prebuild included
	Run r;
	{
		Stub stub;
		std::memcpy(&stub.identifier, g_dev.uuid, 16);
		std::snprintf(stub.description, sizeof(stub.description), "%s", g_dev.deviceName.c_str());
		GxNativeHost host;
		host.log = collect;
		GxNativeSessionHost sh;
		sh.readFile = readFile;
		sh.getEnv = getEnv;
		g_lines = &r.lines;
		stub.AddRef();
		IDirect3D8* proxy = GxNativeWrapInterface(&stub, host, sh, &dbg);
		IDirect3DDevice8* dev = nullptr;
		proxy->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, 0, &pp, &dev);
		r.session = GxNativeLastSession(GxNativeProxyFrom(proxy));
		r.forwarded = stub.forwardedCreate;
		r.liveAfter = g_gxNativeLiveDevices.load();
		CHECK(!r.session.native || (dev && GxNativeFromDevice(dev)), "native session without a native device");
		if (dev)
			dev->Release();
		proxy->Release();
		g_lines = nullptr;
	}
	if (!r.session.native && r.session.failedRow == 2)
		g_seen = true;
	CHECK(r.session.native, ("session is not native: row S" + std::to_string(r.session.failedRow) + " " + r.session.reason).c_str());
	CHECK(r.forwarded == 0, "DXVK's CreateDevice was called in a native session");
	CHECK(r.liveAfter == 1, "no live native device");
	CHECK(g_gxNativeLiveDevices.load() == 0, "device not destroyed by its last Release");
	CHECK(r.lines.size() == 1 && r.lines[0].find("session=native") != std::string::npos && r.lines[0].find("uuid=") != std::string::npos,
		("session line: " + (r.lines.empty() ? std::string("none") : r.lines[0])).c_str());
	return ok;
}
void plantPositive(GxNativeSessionDebug& p) { p.platformUnqualified = true; }   // S2 refuses -> not native

bool caseS2(GxNativeSessionDebug& p, std::string* d)
{
	GxNativeSessionDebug dbg = p;
	dbg.platformUnqualified = true;
	return classicAt(run(goodPP(), dbg), 2, "platform", d);
}
void plantS2(GxNativeSessionDebug& p) { p.skipPlatformRow = true; }

bool caseS3(GxNativeSessionDebug& p, std::string* d)
{
	bool ok = true;
	for (const char* var : { "GXCAP", "GX_RENDER_HYBRID", "GX_VK_PRESENT_MODE", "GX_VK_DEVICE", "DXVK_HUD" }) {
		clean();
		g_env[var] = "1";
		ok &= classicAt(run(goodPP(), p), 3, var, d);
	}
	for (const char* off : { "", "0" }) {   // DXVK_HUD empty or "0" shows no HUD: not a reason to go classic
		clean();
		g_env["DXVK_HUD"] = off;
		const Run r = run(goodPP(), p);
		CHECK(r.session.native, (std::string("DXVK_HUD='") + off + "' not native: " + r.session.reason).c_str());
	}
	clean();
	return ok;
}
void plantS3(GxNativeSessionDebug& p) { p.skipEnvRow = true; }

bool caseS4(GxNativeSessionDebug& p, std::string* d)
{
	bool ok = true;
	struct { const char* name; void (*mod)(D3DPRESENT_PARAMETERS&); const char* needle; } bad[] = {
		{ "fullscreen", [](D3DPRESENT_PARAMETERS& q) { q.Windowed = FALSE; }, "Windowed" },
		{ "back buffer format", [](D3DPRESENT_PARAMETERS& q) { q.BackBufferFormat = D3DFMT_R5G6B5; }, "BackBufferFormat" },
		{ "depth format", [](D3DPRESENT_PARAMETERS& q) { q.AutoDepthStencilFormat = D3DFMT_D15S1; }, "AutoDepthStencilFormat" },
		{ "msaa 3", [](D3DPRESENT_PARAMETERS& q) { q.MultiSampleType = D3DMULTISAMPLE_TYPE(3); }, "MultiSampleType" },
		{ "swap flip", [](D3DPRESENT_PARAMETERS& q) { q.SwapEffect = D3DSWAPEFFECT_FLIP; }, "SwapEffect" },
		{ "windowed interval", [](D3DPRESENT_PARAMETERS& q) { q.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE; }, "INVALIDCALL" },
	};
	for (auto& b : bad) {
		clean();
		D3DPRESENT_PARAMETERS pp = goodPP();
		b.mod(pp);
		std::string dd;
		if (!classicAt(run(pp, p), 4, b.needle, &dd)) {
			*d += std::string(" [") + b.name + ":" + dd + " ]";
			ok = false;
		}
	}
	// MSAA parity (Codex r1 P2): a sparse sample mask where DXVK's decode grants a count the device cannot
	CHECK(GxNativeDxvkSampleCount(2, 0xd) == 2 && GxNativeDxvkSampleCount(16, 0xf) == 8 && GxNativeDxvkSampleCount(0, 0x1) == 1 &&
		GxNativeDxvkSampleCount(4, 0x1) == 1, "GxNativeDxvkSampleCount");
	{
		clean();
		GxNativeSessionDebug dbg = p;
		dbg.fakeSampleMask = 0xd;
		D3DPRESENT_PARAMETERS pp = goodPP();
		pp.MultiSampleType = D3DMULTISAMPLE_2_SAMPLES;
		std::string dd;
		if (!classicAt(run(pp, dbg), 4, "grants 2", &dd)) {
			*d += " [sparse msaa mask:" + dd + " ]";
			ok = false;
		}
	}
	// An UNKNOWN back buffer and no back-buffer count (the iOS game, iPad smoke-X1 13/10: the SDL3 path presents Windowed=TRUE and the
	// exclusive mode search finds no mode): DXVK converts both in the caller's struct before anything reads them (d3d8_d3d9_util.h:89-94),
	// so the session must come up native with the struct converted - and stay untouched when d3d8 refuses first
	{
		clean();
		D3DPRESENT_PARAMETERS pp = goodPP();
		pp.BackBufferFormat = D3DFMT_UNKNOWN;
		pp.BackBufferCount = 0;
		const Run r = run(pp, p);
		CHECK(r.session.native, ("UNKNOWN back buffer not native: row S" + std::to_string(r.session.failedRow) + " " + r.session.reason).c_str());
		CHECK(r.after.BackBufferFormat == D3DFMT_X8R8G8B8 && r.after.BackBufferCount == 1, "the caller's struct was not converted as DXVK converts it");
		clean();
		pp.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;   // d3d8's windowed-interval refusal comes before the conversion
		const Run r2 = run(pp, p);
		CHECK(r2.after.BackBufferFormat == D3DFMT_UNKNOWN && r2.after.BackBufferCount == 0, "a call d3d8 refuses converted the caller's struct");
		clean();
		D3DPRESENT_PARAMETERS cp = goodPP();   // COPY with two buffers: d3d8 refuses before converting (Codex native-ipad-r3 P3)
		cp.BackBufferFormat = D3DFMT_UNKNOWN;
		cp.BackBufferCount = 2;
		cp.SwapEffect = D3DSWAPEFFECT_COPY;
		const Run r4 = run(cp, p);
		CHECK(r4.after.BackBufferFormat == D3DFMT_UNKNOWN && r4.after.BackBufferCount == 2, "a COPY refusal converted the caller's struct");
		GxD3D8NormalizePresentParams(nullptr);   // a null struct is ignored (CreateDevice forwards a null pp before it gets here)
		// every other platform's parameters pass through untouched (render-integration-5 ruling 13/10): a stated format and count stay
		for (D3DFORMAT f : { D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_R5G6B5 }) {
			clean();
			D3DPRESENT_PARAMETERS q = goodPP();
			q.BackBufferFormat = f;
			q.BackBufferCount = 2;
			const Run r3 = run(q, p);
			CHECK(std::memcmp(&r3.after, &q, sizeof q) == 0, ("a stated back buffer (format " + std::to_string(int(f)) + ", 2 buffers) was changed").c_str());
		}
	}
	// COPY_VSYNC and COPY are qualified: the session must come up native
	for (D3DSWAPEFFECT se : { D3DSWAPEFFECT_COPY, D3DSWAPEFFECT_COPY_VSYNC }) {
		D3DPRESENT_PARAMETERS pp = goodPP();
		pp.SwapEffect = se;
		const Run r = run(pp, p);
		CHECK(r.session.native, (std::string("swap effect ") + std::to_string(int(se)) + " not native: row S" + std::to_string(r.session.failedRow) + " " + r.session.reason).c_str());
	}
	return ok;
}
void plantS4(GxNativeSessionDebug& p) { p.skipPresentRow = true; p.skipMsaaParity = true; }

bool caseS5(GxNativeSessionDebug& p, std::string* d)
{
	clean();
	setEnvVar("DXVK_CONFIG", "d3d9.samplerAnisotropy = 8; d3d9.samplerLodBias = 1");
	const bool ok = classicAt(run(goodPP(), p), 5, "d3d9.samplerLodBias", d);
	clean();
	return ok;
}
void plantS5(GxNativeSessionDebug& p) { p.skipConfigRow = true; }

bool caseS6(GxNativeSessionDebug& p, std::string* d)
{
	bool ok = true;
	for (Files f : { Files::MissingTerrain, Files::OverrideTerrain, Files::NoReader, Files::TrailingBytes }) {
		clean();
		g_files = f;
		std::string dd;
		if (!classicAt(run(goodPP(), p), 6, f == Files::NoReader ? "no file reader" : f == Files::MissingTerrain ? "missing" : f == Files::TrailingBytes ? "DWORDs" : "catalogue", &dd)) {
			*d += " [files=" + std::to_string(int(f)) + ":" + dd + " ]";
			ok = false;
		}
	}
	clean();
	return ok;
}
void plantS6(GxNativeSessionDebug& p) { p.skipShaderRow = true; }

bool caseS7(GxNativeSessionDebug& p, std::string* d)
{
	// GX_SHADOW_GPU: unset, and a value the game does not read ("7", "11"), mean the game's platform default: 1 on _WIN32, 0
	// elsewhere (rw3-R11). With the shadow vs ported (gx:shadowvol) every mode is native; in a build whose catalogue lacks it
	// (shadowPortMissing) a mode the game turns on must refuse at S7 - and mode 0 never does.
#if defined(_WIN32)
	const bool defaultOn = true;
#else
	const bool defaultOn = false;
#endif
	bool ok = true;
	for (const bool missing : { false, true }) {
		p.shadowPortMissing = missing;
		for (const char* v : { "0", "1", "2", (const char*)nullptr, "7", "11" }) {
			clean();
			if (v)
				g_env["GX_SHADOW_GPU"] = v;
			else
				g_env.erase("GX_SHADOW_GPU");   // really unset: clean() presets "0" on Windows for every other arm
			const bool valid = v && v[0] >= '0' && v[0] <= '2' && v[1] == '\0';   // what gxShadowGpuMode reads
			const bool on = valid ? v[0] != '0' : defaultOn;
			const bool wantClassic = missing && on;
			std::string dd;
			const Run r = run(goodPP(), p);
			const bool good = wantClassic ? classicAt(r, 7, "GX_SHADOW_GPU", &dd) : r.session.native;
			if (!good) {
				*d += std::string(" [") + (missing ? "port missing, " : "") + (v ? v : "unset") +
					(wantClassic ? ": not refused at S7 " + dd : ": not native " + r.session.reason) + " ]";
				ok = false;
			}
		}
	}
	p.shadowPortMissing = false;
	clean();
	return ok;
}
void plantS7(GxNativeSessionDebug& p) { p.skipShadowRow = true; }

bool caseS8(GxNativeSessionDebug& p, std::string* d)
{
	bool ok = true;
	// the pure qualifier on fake process images
	GxLoaderFacts f;
	std::string why, ld, mvk;
	f.procAddrImage = "/opt/vk/lib/libvulkan.1.dylib";
	f.images = { f.procAddrImage, "/opt/vk/lib/libMoltenVK.dylib" };
	CHECK(GxNativeQualifyLoader(f, true, &ld, &mvk, &why) && ld == f.procAddrImage && mvk == f.images[1], ("good facts refused: " + why).c_str());
	GxLoaderFacts bad = f;
	bad.procAddrImage = "/opt/vk/lib/libMoltenVK.dylib";
	CHECK(!GxNativeQualifyLoader(bad, false, nullptr, nullptr, &why) && why.find("not a Vulkan loader") != std::string::npos, "MoltenVK as the host loader accepted");
	bad = f;
	bad.images.push_back("/usr/local/lib/libvulkan.1.3.0.dylib");
	CHECK(!GxNativeQualifyLoader(bad, false, nullptr, nullptr, &why) && why.find("more than one Vulkan loader") != std::string::npos, "two loader builds accepted");
	bad = f;
	bad.images.push_back("/x/libMoltenVK.dylib");
	CHECK(!GxNativeQualifyLoader(bad, true, nullptr, nullptr, &why) && why.find("more than one MoltenVK") != std::string::npos, "two MoltenVK accepted");
	bad = f;
	bad.images.pop_back();
	CHECK(!GxNativeQualifyLoader(bad, true, nullptr, nullptr, &why) && why.find("no MoltenVK") != std::string::npos, "no MoltenVK accepted after creation");
	CHECK(GxNativeQualifyLoader(bad, false, nullptr, nullptr, &why), "before creation MoltenVK is not required");
	// S8 identifies a loader by BUILD: the 0.9.63 bundle stages libvulkan.1.dylib and libvulkan.dylib as identical copies
	GxLoaderFacts two = f;
	two.images = { "/opt/vk/lib/libvulkan.1.dylib", "/opt/vk/lib/libvulkan.dylib", "/opt/vk/lib/libMoltenVK.dylib" };
	two.builds = { "uuid:aaaa", "uuid:aaaa", "uuid:mvk" };
	two.procAddrImage = two.images[1];
	CHECK(GxNativeQualifyLoader(two, true, &ld, &mvk, &why) && ld == two.images[1] && mvk == two.images[2], ("two paths, one build refused: " + why).c_str());
	two.procAddrImage = two.images[0];
	CHECK(GxNativeQualifyLoader(two, true, &ld, nullptr, &why) && ld == two.images[0], ("two paths, one build refused (other entry): " + why).c_str());
	two.builds[1] = "uuid:bbbb";   // same two paths, different builds: the X-arm the path-count test missed
	CHECK(!GxNativeQualifyLoader(two, false, nullptr, nullptr, &why) && why.find("more than one Vulkan loader build") != std::string::npos
		&& why.find("uuid:aaaa") != std::string::npos && why.find("uuid:bbbb") != std::string::npos && why.find("libvulkan.dylib") != std::string::npos, ("two builds accepted / reason lacks the paths and UUIDs: " + why).c_str());
	two.builds = { "uuid:aaaa", "uuid:aaaa" };   // a builds vector of the wrong size = every image its own build (paths)
	CHECK(!GxNativeQualifyLoader(two, false, nullptr, nullptr, &why), "mismatched builds vector accepted two paths");
	// C · native-ipad-1: S8 where there is no loader (iOS). Good = one MoltenVK framework holding the host's entry point.
	GxLoaderFacts io;
	io.procAddrImage = "/b/GeneralsXZH.app/Frameworks/MoltenVK.framework/MoltenVK";
	io.images = { io.procAddrImage };
	CHECK(GxNativeQualifyLoaderDirect(io, true, &mvk, &why) && mvk == io.procAddrImage, ("iOS good facts refused: " + why).c_str());
	CHECK(!GxNativeQualifyLoader(io, false, nullptr, nullptr, &why), "the loader rule accepted iOS facts (the direct rule is not needed)");
	GxLoaderFacts iob = io;
	iob.procAddrImage = "/b/GeneralsXZH.app/GeneralsXZH";   // SDL fell back to the executable's static copy
	CHECK(!GxNativeQualifyLoaderDirect(iob, false, nullptr, &why) && why.find("not a MoltenVK image") != std::string::npos, "iOS: the static copy accepted as the host's");
	iob = io;
	iob.images.push_back("/b/GeneralsXZH.app/Frameworks/libMoltenVK.dylib");
	CHECK(!GxNativeQualifyLoaderDirect(iob, false, nullptr, &why) && why.find("more than one MoltenVK") != std::string::npos, "iOS: two MoltenVK accepted");
	iob = io;
	iob.images.push_back("/b/GeneralsXZH.app/Frameworks/libvulkan.1.dylib");
	CHECK(!GxNativeQualifyLoaderDirect(iob, false, nullptr, &why) && why.find("loader") != std::string::npos, "iOS: a loader image accepted");
	iob = io;
	iob.images.clear();
	CHECK(GxNativeQualifyLoaderDirect(iob, false, nullptr, &why), ("iOS: before creation the image is not required: " + why).c_str());
	CHECK(!GxNativeQualifyLoaderDirect(iob, true, nullptr, &why) && why.find("no MoltenVK") != std::string::npos, "iOS: no MoltenVK accepted after creation");
	iob = io;
	iob.images = { "/other/MoltenVK.framework/MoltenVK" };
	CHECK(!GxNativeQualifyLoaderDirect(iob, true, nullptr, &why) && why.find("is not the process's MoltenVK") != std::string::npos, "iOS: host MoltenVK != process MoltenVK accepted");
	GxLoaderFacts live = GxNativeLoaderFacts(nullptr);   // this process's real images: a UUID (or a hash) for each, never empty
	CHECK(live.builds.size() == live.images.size(), "live facts: builds not parallel to images");
	for (const std::string& b : live.builds)
		CHECK(b.rfind("uuid:", 0) == 0 || b.rfind("sha256:", 0) == 0 || b.rfind("path:", 0) == 0, ("live build id is not a uuid/sha/path: " + b).c_str());
	// in the proxy: bad provenance around a WORKING loader (the host's entry point reported as living in MoltenVK)
	clean();
	GxNativeSessionDebug dbg = p;
	dbg.fakeProcAddrImage = "/opt/vk/lib/libMoltenVK.dylib";
	ok &= classicAt(run(goodPP(), dbg), 8, "not a Vulkan loader", d);
	return ok;
}
void plantS8(GxNativeSessionDebug& p) { p.skipLoaderRow = true; }

// B · dlss-streamline-1 (PREREG A36.2): the ONE declared interposer (NVIDIA Streamline's sl.interposer.dll) may own the host's
// vkGetInstanceProcAddr in front of the process's one loader build; any other non-loader module is still refused.
bool caseS8Interposer(GxNativeSessionDebug& p, std::string* d)
{
	bool ok = true;
	std::string why, ld;
	GxLoaderFacts f;
	f.interposerImage = "C:\\Game\\Streamline\\sl.interposer.dll";
	f.procAddrImage = f.interposerImage;
	f.images = { "C:\\WINDOWS\\SYSTEM32\\vulkan-1.dll", f.interposerImage };
	f.builds = { "sha256:loader", "sha256:sl" };
	f.testInterposerAnyPath = p.s8InterposerAnyPath;
	CHECK(GxNativeQualifyLoader(f, false, &ld, nullptr, &why, true) && ld == f.interposerImage + " -> " + f.images[0],
		("the declared interposer in front of one loader refused: " + why + " ld=" + ld).c_str());
	GxLoaderFacts bad = f;
	bad.procAddrImage = "C:\\Game\\other\\sl.interposer.dll";   // a non-loader module that is NOT the declared one
	const bool otherAccepted = GxNativeQualifyLoader(bad, false, nullptr, nullptr, &why);
	g_seen |= otherAccepted && p.s8InterposerAnyPath;   // X-S8-interposer: the plant is what let the other module through
	CHECK(!otherAccepted && why.find("not a Vulkan loader") != std::string::npos,
		"a non-loader procAddr module that is not the declared interposer accepted");
	bad = f;
	bad.interposerImage.clear();   // nothing declared: the interposer is just a non-loader module
	CHECK(!GxNativeQualifyLoader(bad, false, nullptr, nullptr, &why) && why.find("not a Vulkan loader") != std::string::npos,
		"an undeclared interposer accepted");
	bad = f;
	bad.images.push_back("C:\\Other\\vulkan-1.dll");
	bad.builds.push_back("sha256:other");
	CHECK(!GxNativeQualifyLoader(bad, false, nullptr, nullptr, &why) && why.find("more than one Vulkan loader build") != std::string::npos,
		"two loader builds accepted behind the interposer");
	bad = f;
	bad.images = { f.interposerImage };
	bad.builds = { "sha256:sl" };
	CHECK(!GxNativeQualifyLoader(bad, false, nullptr, nullptr, &why, true) && why.find("no Vulkan loader image") != std::string::npos,
		"the interposer with no loader behind it accepted after creation");
	return ok;
}
void plantS8Interposer(GxNativeSessionDebug& p) { p.s8InterposerAnyPath = true; }

bool caseS9(GxNativeSessionDebug& p, std::string* d)
{
	clean();
	return classicAt(run(goodPP(), p, /*matchUuid=*/false), 9, "uuid mismatch", d);
}
void plantS9(GxNativeSessionDebug& p) { p.skipUuidRow = true; }

bool caseTeardown(GxNativeSessionDebug& p, std::string* d)
{
	bool bad = false;
	for (const char* at : { "S9", "S10" }) {   // a refusal AFTER a good device was built (the half-built device must be destroyed first, §2.2)
		clean();
		g_env["GX_NATIVE_TEST_FAIL_AT"] = at;
		std::string dd;
		if (!classicAt(run(goodPP(), p), at[1] == '9' ? 9 : 10, "forced", &dd)) {
			*d += std::string(" [") + at + ":" + dd + " ]";
			bad = true;
		}
	}
	clean();
	return !bad;
}
void plantTeardown(GxNativeSessionDebug& p) { p.noTeardown = true; }

bool caseRetry(GxNativeSessionDebug& p, std::string* d)
{
	// the game calls CreateDevice again after a failure (dx8wrapper.cpp:1902): a refused first call must not poison the second
	bool ok = true;
	clean();
	Stub stub;
	std::memcpy(&stub.identifier, g_dev.uuid, 16);
	std::snprintf(stub.description, sizeof(stub.description), "%s", g_dev.deviceName.c_str());
	GxNativeHost host;
	host.prebuildGenericSet = false;
	GxNativeSessionHost sh;
	sh.readFile = readFile;
	sh.getEnv = getEnv;
	stub.AddRef();
	IDirect3D8* proxy = GxNativeWrapInterface(&stub, host, sh, &p);
	D3DPRESENT_PARAMETERS bad = goodPP();
	bad.Windowed = FALSE;
	IDirect3DDevice8 *a = nullptr, *b = nullptr;
	proxy->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, 0, &bad, &a);
	const GxNativeSession first = GxNativeLastSession(GxNativeProxyFrom(proxy));
	if (first.native)
		g_seen = true;   // the plant made the first call native
	if (a) {
		a->Release();   // one live native device per process
		a = nullptr;
	}
	D3DPRESENT_PARAMETERS good = goodPP();
	proxy->CreateDevice(0, D3DDEVTYPE_HAL, nullptr, 0, &good, &b);
	const GxNativeSession second = GxNativeLastSession(GxNativeProxyFrom(proxy));
	CHECK(!first.native && first.failedRow == 4, "first call was not refused at S4");
	CHECK(second.native && b && stub.forwardedCreate == 1, "second call did not come up native after a refused first call");
	if (b)
		b->Release();
	proxy->Release();
	return ok;
}
void plantRetry(GxNativeSessionDebug& p) { p.skipPresentRow = true; p.skipMsaaParity = true; }   // the first call goes native too: no refusal to retry from

bool caseForward(GxNativeSessionDebug& p, std::string* d)
{
	bool ok = true;
	Stub stub;
	GxNativeHost host;
	GxNativeSessionHost sh;
	sh.getEnv = getEnv;
	stub.AddRef();
	IDirect3D8* proxy = GxNativeWrapInterface(&stub, host, sh, &p);
	CHECK(proxy->GetAdapterCount() == 7 && stub.adapterCount == 1, "GetAdapterCount not forwarded");
	D3DCAPS8 c = {};
	CHECK(SUCCEEDED(proxy->GetDeviceCaps(0, D3DDEVTYPE_HAL, &c)) && c.MaxStreams == 16 && stub.caps == 1, "GetDeviceCaps not forwarded");
	proxy->Release();
	CHECK(stub.ref == 1, "the proxy did not release DXVK's interface");
	return ok;
}

const Case kCases[] = {
	{ "native-positive", casePositive, plantPositive },
	{ "S2-platform", caseS2, plantS2 },
	{ "S3-instruments", caseS3, plantS3 },
	{ "S4-present", caseS4, plantS4 },
	{ "S5-config", caseS5, plantS5 },
	{ "S6-shaders", caseS6, plantS6 },
	{ "S7-shadow", caseS7, plantS7 },
	{ "S8-loader", caseS8, plantS8 },
	{ "S8-interposer", caseS8Interposer, plantS8Interposer },
	{ "S9-uuid", caseS9, plantS9 },
	{ "S9S10-teardown", caseTeardown, plantTeardown },
	{ "retry-after-refusal", caseRetry, plantRetry },
	{ "forward", caseForward, nullptr },
};

} // namespace

int main(int argc, char** argv)
{
	std::string armList = "all";
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--arm") && i + 1 < argc)
			armList = argv[++i];
		else {
			std::fprintf(stderr, "usage: %s [--arm NAME[,NAME...]|all]\n", argv[0]);
			return 2;
		}
	}
	clean();
	g_dev = probeDevice();
	if (!g_dev.ok) {
		say("SUMMARY no Vulkan device FAIL");
		return 1;
	}
	say("device %s", g_dev.deviceName.c_str());
	std::vector<std::string> arms;
	if (armList == "all") {
		for (const Case& c : kCases)
			arms.push_back(c.name);
		for (const Case& c : kCases)
			if (c.setPlant)
				arms.push_back(std::string("X-") + c.name);
	} else
		for (size_t p = 0, q; p <= armList.size(); p = q + 1) {
			q = armList.find(',', p);
			if (q == std::string::npos)
				q = armList.size();
			if (q > p)
				arms.push_back(armList.substr(p, q - p));
		}
	if (arms.empty()) {
		say("SUMMARY no arms selected FAIL");
		return 2;
	}
	int pass = 0, fail = 0;
	FILE* digests = nullptr;
	if (const char* path = std::getenv("GX_EXEC_DIGEST"))
		digests = std::fopen(path, "w");
	for (const std::string& a : arms) {
		const bool x = a.compare(0, 2, "X-") == 0;
		const std::string name = x ? a.substr(2) : a;
		const Case* c = nullptr;
		for (const Case& k : kCases)
			if (name == k.name)
				c = &k;
		if (!c || (x && !c->setPlant)) {
			say("ARM %-18s FAIL (unknown arm)", a.c_str());
			++fail;
			continue;
		}
		clean();
		GxNativeSessionDebug plant;
		if (x)
			c->setPlant(plant);
		std::string detail;
		g_seen = false;
		ExecDigest dg;
		if (digests) {
			GXRender::Dev::g_csExecSink.store(&dg);
			GXRender::Rec::g_trInputSink.store(&dg);   // rw3-X-3: the translator-input digest too
		}
		const bool held = c->fn(plant, &detail);
		GXRender::Dev::g_csExecSink.store(nullptr);
		GXRender::Rec::g_trInputSink.store(nullptr);
		if (digests)
			std::fprintf(digests, "%s %s\n", a.c_str(), dg.text().c_str());
		const bool armOk = x ? (!held && g_seen) : held;
		say("ARM %-18s %s (%s)%s", a.c_str(), armOk ? "PASS" : "FAIL",
			x ? (held ? "PLANT NOT DETECTED" : g_seen ? "case broken as planted" : "FAILED FOR ANOTHER REASON") : (held ? "holds" : "BROKEN"), x && !armOk ? "" : detail.c_str());
		armOk ? ++pass : ++fail;
		clean();
	}
	if (digests)
		std::fclose(digests);
	say("SUMMARY arms=%d pass=%d fail=%d %s", pass + fail, pass, fail, fail ? "FAIL" : "PASS");
	return fail ? 1 : 0;
}
