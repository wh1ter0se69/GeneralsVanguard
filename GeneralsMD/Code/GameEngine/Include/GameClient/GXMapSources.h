/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
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

// FILE: GXMapSources.h ///////////////////////////////////////////////////////////////////////////
// Desc:   Where a user map came from, for the map pickers' Source filter. Display only.
///////////////////////////////////////////////////////////////////////////////////////////////////

/*
** GeneralsX @feature 14/11/2026 Every user map lives in <userdata>/Maps/<Name>/<Name>.map; this tells three kinds apart:
**   Generals Vanguard - a folder named like one under <GeneralsXPatch>/ShippedMaps (the launcher copies those in at PLAY);
**   Downloaded        - a folder the launcher's Maps page installed (gxmaps-installed.txt in its state folder), with the
**                       category its cached catalogue (gxmaps.txt) gives the map;
**   My maps           - anything else.
** Pure std:: so scripts/test/map-sources-test.cpp tests it without the engine. Nothing here decides what loads or joins:
** a wrong answer only puts a map under another Source item. Every reader is bounded and never throws.
** The formats are the launcher's (gxio/src/mapsinstall.rs read_record, gxcore/src/mapsdoc.rs parse).
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

/// The most catalogue categories offered; the launcher's placeholder list has six.
static const std::size_t GX_MAP_SOURCES_MAX_CATEGORIES = 32;

struct GXMapSources
{
	std::set<std::string> vanguard;										///< lowercased shipped folder names
	std::map<std::string, std::string> downloaded;		///< lowercased folder name -> category id ("" = not in the catalogue)
	std::vector<std::pair<std::string, std::string>> categories;	///< (id, UTF-8 title), catalogue order, non-empty only
};

/// ASCII lowercase, as AsciiString::toLower leaves a map path (bytes past 0x7F untouched).
inline std::string gxMapSourcesLower(std::string s)
{
	for (char &c : s)
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
	return s;
}

/// The whole file, or false when it is missing, unreadable or larger than `max`.
inline bool gxMapSourcesRead(const std::filesystem::path &p, std::string &out, std::size_t max)
{
	out.clear();
	if (p.empty())
		return false;
	std::error_code ec;
	const std::uintmax_t size = std::filesystem::file_size(p, ec);
	if (ec || size > max)
		return false;
	std::ifstream in(p, std::ios::binary);
	if (!in)
		return false;
	out.resize((std::size_t)size);
	in.read(out.data(), (std::streamsize)size);
	out.resize((std::size_t)in.gcount());
	return true;
}

/// Each line of `text` without its CR, split into (kind, '|'-separated fields).
template <typename F>
inline void gxMapSourcesRows(const std::string &text, F row)
{
	std::size_t at = 0;
	while (at < text.size())
	{
		std::size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at);
		at = end + 1;
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		const std::size_t sp = line.find(' ');
		if (sp == std::string::npos)
			continue;
		std::vector<std::string> f;
		std::size_t from = sp + 1;
		for (;;)
		{
			const std::size_t bar = line.find('|', from);
			f.push_back(line.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
			if (bar == std::string::npos)
				break;
			from = bar + 1;
		}
		row(line.substr(0, sp), f);
	}
}

/// Fill downloaded + categories from the launcher's record and its cached catalogue (either may be empty).
inline void gxMapSourcesParse(const std::string &record, const std::string &catalogue, GXMapSources &out)
{
	static const char kMagic[] = "gxmaps-installed 1";
	const std::size_t magicLen = sizeof(kMagic) - 1;
	const bool magicOk = record.compare(0, magicLen, kMagic) == 0
		&& (record.size() == magicLen || record[magicLen] == '\n' || record[magicLen] == '\r');

	std::map<std::string, std::string> nameToId;
	if (magicOk)
	{
		gxMapSourcesRows(record, [&](const std::string &kind, const std::vector<std::string> &f) {
			if (kind == "map" && f.size() == 5 && !f[0].empty() && !f[1].empty() && f[4] == "installed")
				nameToId[gxMapSourcesLower(f[1])] = f[0];
		});
	}

	std::vector<std::pair<std::string, std::string>> cats;
	std::set<std::string> catIds;
	std::map<std::string, std::string> idToCat;
	gxMapSourcesRows(catalogue, [&](const std::string &kind, const std::vector<std::string> &f) {
		// One row per id, and a bounded list: each category becomes a drop-down row (the launcher refuses a duplicate id).
		if (kind == "category" && f.size() == 3 && !f[0].empty() && !f[1].empty()
				&& cats.size() < GX_MAP_SOURCES_MAX_CATEGORIES && catIds.insert(f[0]).second)
			cats.emplace_back(f[0], f[1]);
		else if (kind == "map" && f.size() >= 3 && !f[0].empty())
			idToCat[f[0]] = f[2];
	});

	std::set<std::string> used;
	for (const auto &n : nameToId)
	{
		const auto c = idToCat.find(n.second);
		const std::string cat = (c != idToCat.end()) ? c->second : std::string();
		out.downloaded[n.first] = cat;
		if (!cat.empty())
			used.insert(cat);
	}
	for (const auto &c : cats)
		if (used.count(c.first))
			out.categories.push_back(c);
}

/// Lowercased names of the folders under `shippedMapsDir` that hold <Name>/<Name>.map.
inline std::set<std::string> gxMapSourcesShippedNames(const std::filesystem::path &shippedMapsDir)
{
	std::set<std::string> names;
	if (shippedMapsDir.empty())
		return names;
	std::error_code ec;
	std::filesystem::directory_iterator it(shippedMapsDir, ec), end;
	for (; !ec && it != end; it.increment(ec))
	{
		std::error_code ec2;
		if (!it->is_directory(ec2) || ec2)
			continue;
		std::string name;
		try
		{
			name = it->path().filename().string();	// throws on Windows for a name the ANSI code page cannot hold
		}
		catch (...)
		{
			continue;		// the engine lists maps by narrow paths too, so such a folder is no listed map's
		}
		if (std::filesystem::is_regular_file(it->path() / (name + ".map"), ec2) && !ec2)
			names.insert(gxMapSourcesLower(name));
	}
	return names;
}

/// Everything, from the launcher's state folder and <GeneralsXPatch>/ShippedMaps. Empty paths read nothing.
inline GXMapSources gxMapSourcesLoad(const std::filesystem::path &launcherStateDir, const std::filesystem::path &shippedMapsDir)
{
	GXMapSources s;
	s.vanguard = gxMapSourcesShippedNames(shippedMapsDir);
	std::string record, catalogue;
	if (!launcherStateDir.empty())
	{
		gxMapSourcesRead(launcherStateDir / "gxmaps-installed.txt", record, (std::size_t)16 << 20);	// the launcher's own cap
		gxMapSourcesRead(launcherStateDir / "gxmaps.txt", catalogue, (std::size_t)4 << 20);
	}
	gxMapSourcesParse(record, catalogue, s);
	return s;
}

/// The folder of a lowercased map path under the lowercased user map dir: "<dir>[/\]<folder>[/\]<file>". "" otherwise.
inline std::string gxMapSourcesFolderOf(const std::string &lowerPath, const std::string &lowerMapDir)
{
	if (lowerMapDir.empty() || lowerPath.compare(0, lowerMapDir.size(), lowerMapDir) != 0)
		return std::string();
	std::size_t at = lowerMapDir.size();
	const bool dirEndsInSep = lowerMapDir.back() == '/' || lowerMapDir.back() == '\\';
	if (!dirEndsInSep)
	{
		if (at >= lowerPath.size() || (lowerPath[at] != '/' && lowerPath[at] != '\\'))
			return std::string();
		++at;
	}
	const std::size_t sep = lowerPath.find_first_of("/\\", at);
	if (sep == std::string::npos || sep == at)
		return std::string();
	return lowerPath.substr(at, sep - at);
}
