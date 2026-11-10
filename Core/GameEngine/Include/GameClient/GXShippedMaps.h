// GeneralsX: the shipped maps, installed by the ENGINE where no launcher runs (iOS).
//
// The owner, 07/11/2026: "also ship the maps we are making by default". Every payload carries the maps in
// scripts/build/overlay-maps.txt as loose files in <GeneralsXPatch>/ShippedMaps/<Name>/, and on the desktop the
// launcher's PLAY copies them into <userdata>/Maps (scripts/launcher-rs/gxio/src/shippedmaps.rs). An iPad has no
// launcher, so nothing did, and its player saw them only by lobby transfer. This is that copy, for the engine.
//
// INTO <userdata>/Maps AND NOWHERE ELSE, which is the whole design. MapCache scans that folder from disk on every
// updateCache (MapUtil.cpp) as user maps, and GameState::realMapPathToPortableMapPath names anything under it
// PORTABLE_USER_MAPS - the same lobby path a desktop that PLAYed gives the same map, so an iPad and a PC in one lobby
// agree on it. The OFFICIAL root (getMapDir, "Maps" in the game folder) is never touched: it is read from a prebuilt
// MapCache.ini, so a map put there would not even show, and if it did it would carry a different lobby path.
//
// NOT SHOWN TWICE: one map, one folder. The destination is <Maps>/<Name>/, exactly where the launcher writes, and a
// map whose .map bytes already match is left alone; MapCache keys a map by its path, so a folder the launcher (or an
// earlier start) already filled is the same entry, not a second one.
//
// THE RULE IS THE LAUNCHER'S, so the two can never disagree about what a player has:
//   * installed when <Maps>/<Name>/<Name>.map is missing, replaced when its bytes differ, left alone when equal
//     (equal bytes is shippedmaps.rs's "same sha256" without hashing);
//   * only the shipped folder's own regular files are written, the .map LAST, each through a temp name + rename, so an
//     interrupted install leaves the old .map and the next start sees the difference again;
//   * nothing is ever deleted; a folder the player made under any other name is never opened;
//   * a symlink, a folder without <Name>.map, or a name that is not one plain path segment is skipped;
//   * a map that cannot be written is listed in `failed` and the others still install. Nothing here throws.
//
// Header-only and engine-free on purpose, like GXUserDataDir.h: scripts/test/shipped-maps-test.cpp runs it on any host.
// Outside the simulation: maps are not in dataID or the sim digest (SimulationId.cpp), and this header is in no
// digested directory.

#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

struct GXShippedMapsReport
{
	std::vector<std::string> installed;
	std::vector<std::string> replaced;
	std::vector<std::string> unchanged;
	std::vector<std::string> failed;	///< one player-readable line per map that could not be installed
};

/// A folder name that is one plain path segment: it becomes a folder under the player's Maps.
inline bool gxShippedMapSafeName(const std::string &n)
{
	return !n.empty() && n != "." && n != ".." && n.find_first_of(std::string("/\\:\0", 4)) == std::string::npos;
}

/// Byte-equal regular files. False when either cannot be read.
inline bool gxShippedMapSameBytes(const std::filesystem::path &a, const std::filesystem::path &b)
{
	std::error_code ec;
	const std::uintmax_t sa = std::filesystem::file_size(a, ec);
	if (ec)
		return false;
	const std::uintmax_t sb = std::filesystem::file_size(b, ec);
	if (ec || sa != sb)
		return false;
	std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
	if (!fa || !fb)
		return false;
	std::vector<char> ba(1 << 16), bb(1 << 16);
	while (fa && fb)
	{
		fa.read(ba.data(), (std::streamsize)ba.size());
		fb.read(bb.data(), (std::streamsize)bb.size());
		if (fa.gcount() != fb.gcount() || !std::equal(ba.begin(), ba.begin() + fa.gcount(), bb.begin()))
			return false;
	}
	return fa.eof() && fb.eof();
}

/// Copy `src` over `dst` through a temp file beside it. On failure the temp is removed and `why` says what.
inline bool gxShippedMapCopyAtomic(const std::filesystem::path &src, const std::filesystem::path &dst, std::string &why)
{
	std::filesystem::path tmp = dst;
	tmp += ".gxtmp";
	std::error_code ec;
	std::filesystem::copy_file(src, tmp, std::filesystem::copy_options::overwrite_existing, ec);
	if (!ec)
		std::filesystem::rename(tmp, dst, ec);
	if (ec)
	{
		std::error_code ignored;
		std::filesystem::remove(tmp, ignored);
		why = dst.string() + ": " + ec.message();
		return false;
	}
	return true;
}

/// Install every shipped map folder under `srcRoot` (<GeneralsXPatch>/ShippedMaps) into `maps` (the player's
/// <userdata>/Maps). A missing `srcRoot` - an older payload - is an empty report and creates nothing.
inline GXShippedMapsReport gxInstallShippedMaps(const std::filesystem::path &srcRoot, const std::filesystem::path &maps)
{
	namespace fs = std::filesystem;
	GXShippedMapsReport rep;
	std::error_code ec;
	if (!fs::is_directory(srcRoot, ec))
		return rep;

	std::vector<std::pair<std::string, fs::path>> dirs;
	for (fs::directory_iterator it(srcRoot, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code sec;
		// symlink_status: a link is not a shipped folder, and following one leaves the payload.
		if (!fs::is_directory(fs::symlink_status(it->path(), sec)) || sec)
			continue;
		const std::string name = it->path().filename().string();
		if (gxShippedMapSafeName(name))
			dirs.emplace_back(name, it->path());
	}
	if (ec)
	{
		rep.failed.push_back("shipped maps: " + srcRoot.string() + ": " + ec.message());
		return rep;
	}
	std::sort(dirs.begin(), dirs.end());

	for (const std::pair<std::string, fs::path> &entry : dirs)
	{
		const std::string &name = entry.first;
		const fs::path &dir = entry.second;
		const fs::path srcMap = dir / (name + ".map");
		std::error_code mec;
		if (!fs::is_regular_file(fs::symlink_status(srcMap, mec)) || mec)
			continue;	// not a map folder; not ours to interpret
		const fs::path dest = maps / name;
		const fs::path destMap = dest / (name + ".map");
		const bool had = fs::is_regular_file(destMap, mec);
		if (had && gxShippedMapSameBytes(srcMap, destMap))
		{
			rep.unchanged.push_back(name);
			continue;
		}

		// Only our own file names, regular files only, the .map last.
		std::vector<fs::path> files;
		std::error_code lec;
		for (fs::directory_iterator f(dir, lec), fend; !lec && f != fend; f.increment(lec))
		{
			std::error_code s2;
			if (f->path() != srcMap && fs::is_regular_file(fs::symlink_status(f->path(), s2)) && !s2)
				files.push_back(f->path());
		}
		if (lec)
		{
			rep.failed.push_back("map " + name + " was not installed: cannot list the shipped copy (" + lec.message() + ")");
			continue;
		}
		std::sort(files.begin(), files.end());
		files.push_back(srcMap);

		std::string why;
		bool ok = true;
		std::error_code dec;
		fs::create_directories(dest, dec);
		if (dec)
		{
			ok = false;
			why = dest.string() + ": " + dec.message();
		}
		for (const fs::path &f : files)
		{
			if (!ok)
				break;
			ok = gxShippedMapCopyAtomic(f, dest / f.filename(), why);
		}
		if (!ok)
			rep.failed.push_back("map " + name + " was not installed: cannot write " + why);
		else
			(had ? rep.replaced : rep.installed).push_back(name);
	}
	return rep;
}
