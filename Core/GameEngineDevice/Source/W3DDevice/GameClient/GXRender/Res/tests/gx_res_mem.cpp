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

// gx_res_mem - phys_footprint A/B of upload-then-drop vs classic DXVK MANAGED on the SAME texture set,
// one arm per process (macOS only).
//   gx_res_mem --arm classic|classic-empty|native-drop|native-keep|native-empty --dir <dir of .dds/.tga>
// native-keep is the FAIL arm (the host share must come back); *-empty is the null arm (delta ~0).
#include "gx_res_mem.h"
#include "gx_res_build_sha.h"

#include "GxD3D.h"
#include "GxResources.h"
#include "GxTexture.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <thread>

#include <mach/mach.h>
#include <malloc/malloc.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;

namespace GXRender { namespace ResMem {

uint64_t TexFile::bytes() const
{
	uint64_t b = 0;
	for (const std::vector<uint8_t>& l : levels)
		b += l.size();
	return b;
}

Footprint footprint()
{
	task_vm_info_data_t vmi = {};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	Footprint f;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vmi), &count) == KERN_SUCCESS) {
		f.now = vmi.phys_footprint;
		f.peak = vmi.ledger_phys_footprint_peak;
	}
	return f;
}

}} // namespace GXRender::ResMem

using namespace GXRender;
using namespace GXRender::Res;
using namespace GXRender::ResMem;

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

double mb(uint64_t b) { return double(b) / (1024.0 * 1024.0); }
double dmb(uint64_t a, uint64_t b) { return (double(a) - double(b)) / (1024.0 * 1024.0); } // a - b, signed

bool readFile(const std::string& path, std::vector<uint8_t>* out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	out->resize(n > 0 ? size_t(n) : 0);
	const bool ok = n > 0 && std::fread(out->data(), 1, out->size(), f) == out->size();
	std::fclose(f);
	return ok;
}

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

uint64_t levelBytes(uint32_t fmt, uint32_t w, uint32_t h)
{
	if (fmt == d3d::DXT1 || fmt == d3d::DXT3 || fmt == d3d::DXT5)
		return uint64_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * (fmt == d3d::DXT1 ? 8 : 16);
	return uint64_t(w) * h * 4;
}

bool loadDds(const std::string& name, const std::vector<uint8_t>& f, TexFile* t, std::string* why)
{
	if (f.size() < 128 || rd32(&f[0]) != 0x20534444) { // "DDS "
		*why = "not a DDS";
		return false;
	}
	const uint8_t* h = &f[4];
	t->height = rd32(h + 8);
	t->width = rd32(h + 12);
	const uint32_t mips = std::max(1u, rd32(h + 24));
	const uint8_t* pf = h + 72;
	const uint32_t pfFlags = rd32(pf + 4), fourcc = rd32(pf + 8), bits = rd32(pf + 12), amask = rd32(pf + 28);
	if (pfFlags & 0x4) { // DDPF_FOURCC
		if (fourcc != d3d::DXT1 && fourcc != d3d::DXT3 && fourcc != d3d::DXT5) {
			*why = "unsupported FourCC";
			return false;
		}
		t->format = fourcc;
	} else if ((pfFlags & 0x40) && bits == 32) // DDPF_RGB
		t->format = (pfFlags & 0x1) && amask ? d3d::A8R8G8B8 : d3d::X8R8G8B8;
	else {
		*why = "unsupported pixel format";
		return false;
	}
	size_t off = 128;
	uint32_t w = t->width, hh = t->height;
	for (uint32_t l = 0; l < mips; ++l) {
		const uint64_t n = levelBytes(t->format, w, hh);
		if (off + n > f.size()) {
			*why = "truncated at level " + std::to_string(l);
			return false;
		}
		t->levels.emplace_back(f.begin() + off, f.begin() + off + n);
		off += n;
		w = std::max(1u, w / 2);
		hh = std::max(1u, hh / 2);
	}
	t->name = name;
	return true;
}

bool loadTga(const std::string& name, const std::vector<uint8_t>& f, TexFile* t, std::string* why)
{
	if (f.size() < 18 || f[2] != 2 || (f[16] != 24 && f[16] != 32)) {
		*why = "TGA is not uncompressed 24/32-bit";
		return false;
	}
	const uint32_t w = f[12] | f[13] << 8, h = f[14] | f[15] << 8, bpp = f[16] / 8;
	const bool topDown = f[17] & 0x20;
	const size_t off = 18 + f[0];
	if (off + size_t(w) * h * bpp > f.size()) {
		*why = "truncated TGA";
		return false;
	}
	t->name = name;
	t->format = bpp == 4 ? d3d::A8R8G8B8 : d3d::X8R8G8B8;
	t->width = w;
	t->height = h;
	std::vector<uint8_t> l0(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t* src = &f[off + size_t(topDown ? y : h - 1 - y) * w * bpp];
		uint8_t* dst = &l0[size_t(y) * w * 4];
		for (uint32_t x = 0; x < w; ++x, src += bpp, dst += 4) { // TGA is B,G,R(,A) = A8R8G8B8's byte order
			dst[0] = src[0];
			dst[1] = src[1];
			dst[2] = src[2];
			dst[3] = bpp == 4 ? src[3] : 0xFF;
		}
	}
	t->levels.push_back(std::move(l0));
	for (uint32_t lw = w, lh = h; lw > 1 || lh > 1;) { // full chain, 2x2 box
		const uint32_t nw = std::max(1u, lw / 2), nh = std::max(1u, lh / 2);
		const std::vector<uint8_t>& p = t->levels.back();
		std::vector<uint8_t> n(size_t(nw) * nh * 4);
		for (uint32_t y = 0; y < nh; ++y)
			for (uint32_t x = 0; x < nw; ++x)
				for (uint32_t c = 0; c < 4; ++c) {
					const uint32_t x0 = std::min(2 * x, lw - 1), x1 = std::min(2 * x + 1, lw - 1);
					const uint32_t y0 = std::min(2 * y, lh - 1), y1 = std::min(2 * y + 1, lh - 1);
					const uint32_t s = p[(size_t(y0) * lw + x0) * 4 + c] + p[(size_t(y0) * lw + x1) * 4 + c] +
						p[(size_t(y1) * lw + x0) * 4 + c] + p[(size_t(y1) * lw + x1) * 4 + c];
					n[(size_t(y) * nw + x) * 4 + c] = uint8_t((s + 2) / 4);
				}
		t->levels.push_back(std::move(n));
		lw = nw;
		lh = nh;
	}
	return true;
}

bool loadDir(const std::string& dir, std::vector<TexFile>* out, std::string* why)
{
	DIR* d = opendir(dir.c_str());
	if (!d) {
		*why = "cannot open " + dir;
		return false;
	}
	std::vector<std::string> names;
	while (dirent* e = readdir(d)) {
		std::string n = e->d_name;
		std::string lower = n;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		if (lower.size() > 4 && (lower.compare(lower.size() - 4, 4, ".dds") == 0 || lower.compare(lower.size() - 4, 4, ".tga") == 0))
			names.push_back(n);
	}
	closedir(d);
	std::sort(names.begin(), names.end()); // one order for every arm
	for (const std::string& n : names) {
		std::vector<uint8_t> f;
		TexFile t;
		std::string w;
		if (!readFile(dir + "/" + n, &f)) {
			*why = "cannot read " + n;
			return false;
		}
		const bool dds = n.size() > 4 && (n[n.size() - 3] == 'd' || n[n.size() - 3] == 'D');
		if (!(dds ? loadDds(n, f, &t, &w) : loadTga(n, f, &t, &w))) {
			*why = n + ": " + w; // every file must load: a skipped file would make the arms load different sets
			return false;
		}
		out->push_back(std::move(t));
	}
	return true;
}

// The native arms. Returns false with *why. drop: upload-then-drop.
bool nativeLoad(const std::vector<TexFile>& set, bool drop, std::vector<std::unique_ptr<GxTexture>>* live, GxResources& gx,
	std::string* why)
{
	const VkDeviceSize kSubmitEvery = 16ull << 20;
	for (const TexFile& f : set) {
		std::unique_ptr<GxTexture> t(new GxTexture);
		if (!t->init(gx, {f.width, f.height, uint32_t(f.levels.size()), f.format, 0, d3d::POOL_MANAGED}, why)) {
			*why = f.name + ": " + *why;
			return false;
		}
		for (uint32_t l = 0; l < f.levels.size(); ++l) {
			LockedRect lr;
			if (!t->lockRect(l, nullptr, 0, &lr)) {
				*why = f.name + ": lock refused";
				return false;
			}
			std::memcpy(lr.bits, f.levels[l].data(), f.levels[l].size()); // both sides tight pitch (GxTexture.h)
			t->unlockRect(l);
		}
		if (drop && !t->dropHostCopy()) {
			*why = f.name + ": dropHostCopy refused";
			return false;
		}
		live->push_back(std::move(t));
		if (gx.pendingBytes() >= kSubmitEvery && !gx.submitPending(true)) {
			*why = "submitPending failed";
			return false;
		}
	}
	return gx.submitPending(true) || (*why = "final submitPending failed", false);
}

// `footprint <pid>` (the category split) into out. False when it was asked for and did not run cleanly (a
// spawn failure used to print "exit 0").
bool runFootprintTool(const char* out)
{
	if (!out)
		return true;
	const std::string pid = std::to_string(getpid());
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	posix_spawn_file_actions_adddup2(&fa, 1, 2);
	char* argv[] = {const_cast<char*>("footprint"), const_cast<char*>(pid.c_str()), nullptr};
	pid_t child = 0;
	int st = 0;
	const int sp = posix_spawnp(&child, "footprint", &fa, nullptr, argv, environ);
	const bool waited = sp == 0 && waitpid(child, &st, 0) == child;
	posix_spawn_file_actions_destroy(&fa);
	const bool ok = waited && WIFEXITED(st) && WEXITSTATUS(st) == 0;
	say("[GXMEM] footprint tool -> %s: %s", out, sp != 0 ? ("spawn failed: " + std::string(std::strerror(sp))).c_str() :
		!waited ? "wait failed" : WIFEXITED(st) ? ("exit " + std::to_string(WEXITSTATUS(st))).c_str() : "killed by a signal");
	return ok;
}

} // namespace

int main(int argc, char** argv)
{
	say("[GXMEM] build sha=%s", GX_RES_BUILD_SHA);
	std::string arm, dir;
	const char* fpOut = nullptr;
	int settle = 5;
	for (int i = 1; i < argc; ++i) {
		if (!std::strcmp(argv[i], "--arm") && i + 1 < argc)
			arm = argv[++i];
		else if (!std::strcmp(argv[i], "--dir") && i + 1 < argc)
			dir = argv[++i];
		else if (!std::strcmp(argv[i], "--settle") && i + 1 < argc)
			settle = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--footprint-out") && i + 1 < argc)
			fpOut = argv[++i];
		else {
			std::fprintf(stderr, "usage: %s --arm classic|classic-empty|native-drop|native-keep|native-empty --dir D [--settle S] [--footprint-out F]\n", argv[0]);
			return 2;
		}
	}
	const bool classic = arm == "classic" || arm == "classic-empty";
	const bool native = arm == "native-drop" || arm == "native-keep" || arm == "native-empty";
	const bool empty = arm == "classic-empty" || arm == "native-empty";
	if ((!classic && !native) || dir.empty()) {
		std::fprintf(stderr, "bad --arm or missing --dir\n");
		return 2;
	}
	// Every arm reads the whole set (same file I/O and parse heap), the empty arms just do not create textures from it.
	std::vector<TexFile> set;
	std::string why;
	if (!loadDir(dir, &set, &why) || set.empty()) {
		say("[GXMEM] RESULT arm=%s FAIL load: %s", arm.c_str(), why.empty() ? "no .dds/.tga files" : why.c_str());
		return 1;
	}
	uint64_t setBytes = 0;
	for (const TexFile& t : set)
		setBytes += t.bytes();
	say("[GXMEM] set dir=%s textures=%zu bytes=%.1f MB (all levels, as created)", dir.c_str(), set.size(), mb(setBytes));
	// Warm-up, loaded by EVERY arm before the baseline: the first upload of each kind pays one-time device costs (+138 MB graphics
	// for a 10 MB set without it), which belong to neither arm's textures.
	std::vector<TexFile> warm(2);
	for (int i = 0; i < 2; ++i) {
		warm[i].name = i ? "warm-dxt1" : "warm-argb";
		warm[i].format = i ? d3d::DXT1 : d3d::A8R8G8B8;
		warm[i].width = warm[i].height = 64;
		for (uint32_t w = 64; w >= 1; w /= 2)
			warm[i].levels.emplace_back(size_t(levelBytes(warm[i].format, w, w)), uint8_t(0x5A));
	}

	// The device first, then the baseline footprint: the delta below is the textures' alone.
	Vk::VkCtx ctx;
	GxResources gx;
	std::vector<std::unique_ptr<GxTexture>> live;
	uint32_t kept = 0;
	Footprint init, loaded;
	std::chrono::steady_clock::time_point t0; // set when the baseline is taken: load_s is the set's load alone
	if (native) {
		Vk::VkCtxDesc desc; // no validation: the layer's own memory would ride on the measurement
		if (!ctx.init(desc, &why) || !gx.init(ctx, 32ull << 20, &why)) {
			say("[GXMEM] RESULT arm=%s FAIL device: %s", arm.c_str(), why.c_str());
			return 1;
		}
		say("[GXMEM] device %s", ctx.properties().deviceName);
		if (!nativeLoad(warm, arm == "native-drop", &live, gx, &why)) {
			say("[GXMEM] RESULT arm=%s FAIL native warm-up: %s", arm.c_str(), why.c_str());
			return 1;
		}
		std::this_thread::sleep_for(std::chrono::seconds(settle)); // the baseline settles too (Mini: -41 MB in 5 s)
		init = footprint();
		t0 = std::chrono::steady_clock::now();
		if (!empty && !nativeLoad(set, arm == "native-drop", &live, gx, &why)) {
			say("[GXMEM] RESULT arm=%s FAIL native load: %s", arm.c_str(), why.c_str());
			return 1;
		}
		kept = uint32_t(live.size()) - uint32_t(warm.size());
	} else {
		if (!classicLoad(warm, 0, &kept, &why)) { // device + window + the warm-up
			say("[GXMEM] RESULT arm=%s FAIL classic device: %s", arm.c_str(), why.c_str());
			return 1;
		}
		std::this_thread::sleep_for(std::chrono::seconds(settle)); // the baseline settles too (Mini: -41 MB in 5 s)
		init = footprint();
		t0 = std::chrono::steady_clock::now();
		if (!empty && !classicLoad(set, 16ull << 20, &kept, &why)) {
			say("[GXMEM] RESULT arm=%s FAIL classic load: %s", arm.c_str(), why.c_str());
			return 1;
		}
	}
	const double loadS = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	loaded = footprint();
	std::this_thread::sleep_for(std::chrono::seconds(settle));
	const Footprint settled = footprint();
	malloc_zone_pressure_relief(nullptr, 0);
	const Footprint relieved = footprint();
	if (classic)
		kept -= uint32_t(warm.size());
	say("[GXMEM] arm=%s textures=%u init=%.1f loaded=%.1f settled=%.1f relieved=%.1f peak=%.1f MB load_s=%.2f (peak is the process lifetime peak: it speaks for the load only above the null arm's)",
		arm.c_str(), kept, mb(init.now), mb(loaded.now), mb(settled.now), mb(relieved.now), mb(settled.peak), loadS);
	if (native) {
		// Attribution: VMA's device blocks (capacity) vs what the images actually use, and the heap the host copies live in.
		VmaTotalStatistics vs = {};
		vmaCalculateStatistics(gx.allocator(), &vs);
		say("[GXMEM] gxres stagingBytes=%.1f MB imageCopies=%llu dropped=%llu vma_blocks=%u vma_blockBytes=%.1f vma_allocBytes=%.1f MB",
			mb(gx.stats().stagingBytes), (unsigned long long)gx.stats().imageCopies, (unsigned long long)gx.stats().dropped,
			vs.total.statistics.blockCount, mb(vs.total.statistics.blockBytes), mb(vs.total.statistics.allocationBytes));
	}
	malloc_statistics_t ms = {};
	malloc_zone_statistics(nullptr, &ms);
	say("[GXMEM] malloc in_use=%.1f allocated=%.1f MB (all zones, after relief)", mb(ms.size_in_use), mb(ms.size_allocated));
	say("[GXMEM] RESULT arm=%s delta_settled=%.1f delta_relieved=%.1f delta_peak=%.1f MB (vs init %.1f)", arm.c_str(),
		dmb(settled.now, init.now), dmb(relieved.now, init.now), dmb(settled.peak, init.now), mb(init.now));
	const bool fpOk = runFootprintTool(fpOut);
	if (!fpOk)
		say("[GXMEM] RESULT arm=%s FAIL the requested footprint capture did not run", arm.c_str());
	if (native) {
		live.clear();
		gx.shutdown();
		ctx.shutdown();
	} else
		classicShutdown();
	return fpOk ? 0 : 1;
}
