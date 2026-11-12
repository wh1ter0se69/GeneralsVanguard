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

// prefer the V-cache CCD on X3D CPUs. See GXCpuSets.h.
#include "GXCpuSets.h"

#include <algorithm>

namespace
{
typedef std::pair<uint16_t, uint8_t> GXLp;	// (processor group, logical processor)

std::vector<GXLp> expand(const GXL3Cache &c)
{
	std::vector<GXLp> v;
	for (const auto &gm : c.groupMasks)
		for (int b = 0; b < 64; ++b)
			if (gm.second & (1ull << b))
				v.push_back(GXLp(gm.first, (uint8_t)b));
	return v;
}

/// "0-15" on a one-group machine, "1:16-31" (group:range) on a multi-group one; ranges joined by ','.
std::string ranges(std::vector<GXLp> v, bool multiGroup)
{
	std::sort(v.begin(), v.end());
	std::string s;
	for (size_t i = 0; i < v.size();)
	{
		size_t j = i;
		while (j + 1 < v.size() && v[j + 1].first == v[i].first && v[j + 1].second == v[j].second + 1)
			++j;
		if (!s.empty())
			s += ",";
		if (multiGroup)
			s += std::to_string(v[i].first) + ":";
		s += std::to_string(v[i].second);
		if (j > i)
			s += "-" + std::to_string(v[j].second);
		i = j + 1;
	}
	return s;
}

std::string mb(uint64_t bytes)
{
	return std::to_string(bytes / (1024 * 1024)) + " MB";
}
}

GXCpuSetChoice gxChooseLargestL3CpuSets(const std::vector<GXL3Cache> &l3In, const std::vector<GXCpuSet> &sets)
{
	GXCpuSetChoice c;
	std::vector<GXL3Cache> l3;
	for (const GXL3Cache &x : l3In)
		if (x.bytes != 0 && !expand(x).empty())
			l3.push_back(x);
	if (l3.empty())
	{
		c.why = "no L3 cache reported - no preference";
		return c;
	}
	if (sets.empty())
	{
		c.why = "no CPU sets reported - no preference";
		return c;
	}

	bool multiGroup = false;
	for (const GXCpuSet &s : sets)
		multiGroup |= s.group != 0;
	for (const GXL3Cache &x : l3)
		for (const auto &gm : x.groupMasks)
			multiGroup |= gm.first != 0;

	uint64_t big = 0, smallest = UINT64_MAX;
	std::string list = "L3";
	for (const GXL3Cache &x : l3)
	{
		big = (std::max)(big, x.bytes);	// parenthesised: immune to windows.h min/max macros
		smallest = (std::min)(smallest, x.bytes);
		list += (list.size() > 2 ? ", " : " ") + mb(x.bytes) + " on " + ranges(expand(x), multiGroup);
	}
	if (l3.size() == 1)
	{
		c.why = list + " (one L3) - no preference";
		return c;
	}
	// X3D CCDs are twins that differ only in cache. L3 domains of different widths are a hybrid (AMD Strix Point: 4 Zen 5
	// cores under 16 MB, 8 Zen 5c under 8 MB), where the bigger L3 would squeeze every thread onto a third of the CPU.
	for (const GXL3Cache &x : l3)
		if (expand(x).size() != expand(l3[0]).size())
		{
			c.why = list + " (L3 domains not the same size: not an X3D part) - no preference";
			return c;
		}
	// V-cache triples the CCD's L3 (96 vs 32 MB); a hybrid's big/compact gap is about 2x. Under 2.5x, not V-cache.
	if (big * 2 < smallest * 5)
	{
		c.why = list + " (largest L3 under 2.5x the smallest: not V-cache) - no preference";
		return c;
	}

	std::vector<GXLp> under;
	for (const GXL3Cache &x : l3)
		if (x.bytes == big)
			for (const GXLp &lp : expand(x))
				under.push_back(lp);
	std::vector<GXLp> chosen;
	for (const GXCpuSet &s : sets)
		if (std::find(under.begin(), under.end(), GXLp(s.group, s.logicalProcessor)) != under.end())
		{
			c.ids.push_back(s.id);
			chosen.push_back(GXLp(s.group, s.logicalProcessor));
		}
	if (c.ids.empty() || c.ids.size() == sets.size())
	{
		c.why = list + " - the " + mb(big) + " L3 covers " + (c.ids.empty() ? "no" : "every") + " CPU set - no preference";
		c.ids.clear();
		return c;
	}
	c.why = list + " -> prefer the " + std::to_string(c.ids.size()) + " logical processors under the " + mb(big) +
		" L3 (" + ranges(chosen, multiGroup) + ")";
	return c;
}

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

void gxPreferLargestL3()
{
	const char *env = getenv("GX_CPUSETS");
	if (env != nullptr && strcmp(env, "0") == 0)
	{
		fprintf(stderr, "[GXCPU] off (GX_CPUSETS=0)\n");
		return;
	}
	// A mask someone already set (a launcher's start /affinity, Task Manager, a test harness) wins over CPU sets anyway
	// (Microsoft, "CPU Sets"); leave it alone so this line does not claim a preference that cannot take effect.
	DWORD_PTR procMask = 0, sysMask = 0;
	if (GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) && procMask != sysMask)
	{
		fprintf(stderr, "[GXCPU] affinity already set (0x%llx of 0x%llx) - left alone\n", (unsigned long long)procMask,
			(unsigned long long)sysMask);
		return;
	}

	std::vector<GXL3Cache> l3;
	DWORD len = 0;
	GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
	std::vector<BYTE> buf(len);
	if (len != 0 && GetLogicalProcessorInformationEx(RelationCache, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
	{
		for (DWORD off = 0; off < len;)
		{
			const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p = (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(buf.data() + off);
			if (p->Size == 0)
				break;
			if (p->Relationship == RelationCache && p->Cache.Level == 3 && p->Cache.Type != CacheInstruction)
			{
				GXL3Cache c = { p->Cache.CacheSize, {} };
				// GroupCount is 0 before Windows 11 / Server 2022: then GroupMask alone is valid.
				const WORD n = p->Cache.GroupCount != 0 ? p->Cache.GroupCount : 1;
				for (WORD i = 0; i < n; ++i)
					c.groupMasks.push_back(std::make_pair((uint16_t)p->Cache.GroupMasks[i].Group, (uint64_t)p->Cache.GroupMasks[i].Mask));
				l3.push_back(c);
			}
			off += p->Size;
		}
	}

	// Windows 10+ only: looked up, not imported, so the exe still starts where they do not exist.
	typedef BOOL(WINAPI * GetInfoFn)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
	typedef BOOL(WINAPI * SetDefaultFn)(HANDLE, const ULONG *, ULONG);
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	GetInfoFn getInfo = k32 ? (GetInfoFn)(void *)GetProcAddress(k32, "GetSystemCpuSetInformation") : nullptr;
	SetDefaultFn setDefault = k32 ? (SetDefaultFn)(void *)GetProcAddress(k32, "SetProcessDefaultCpuSets") : nullptr;
	if (getInfo == nullptr || setDefault == nullptr)
	{
		fprintf(stderr, "[GXCPU] no CPU set API (before Windows 10) - no preference\n");
		return;
	}
	std::vector<GXCpuSet> sets;
	ULONG need = 0;
	getInfo(nullptr, 0, &need, GetCurrentProcess(), 0);
	std::vector<BYTE> sbuf(need);
	if (need != 0 && getInfo((PSYSTEM_CPU_SET_INFORMATION)sbuf.data(), need, &need, GetCurrentProcess(), 0))
	{
		for (ULONG off = 0; off < need;)
		{
			const SYSTEM_CPU_SET_INFORMATION *s = (const SYSTEM_CPU_SET_INFORMATION *)(sbuf.data() + off);
			if (s->Size == 0)
				break;
			if (s->Type == CpuSetInformation)
				sets.push_back({ s->CpuSet.Id, s->CpuSet.Group, s->CpuSet.LogicalProcessorIndex });
			off += s->Size;
		}
	}

	GXCpuSetChoice c = gxChooseLargestL3CpuSets(l3, sets);
	static_assert(sizeof(ULONG) == sizeof(uint32_t), "the ids are passed to SetProcessDefaultCpuSets as ULONGs");
	if (c.ids.empty())
	{
		fprintf(stderr, "[GXCPU] %s\n", c.why.c_str());
		return;
	}
	if (setDefault(GetCurrentProcess(), (const ULONG *)c.ids.data(), (ULONG)c.ids.size()))
		fprintf(stderr, "[GXCPU] %s: set\n", c.why.c_str());
	else
		fprintf(stderr, "[GXCPU] %s: SetProcessDefaultCpuSets FAILED (error %lu)\n", c.why.c_str(), GetLastError());
}
#endif
