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

// GeneralsX @feature 13/11/2026 Our folders' two names. Every folder of ours that sits in a shared place (Documents, AppData,
// Application Support, ~/.local/share, ~/.config) was called GeneralsX and is renamed GeneralsVanguard by a later launcher
// Until then every program reads BOTH, by one rule, so the move can happen under any
// of them. The same rule lives in gxlaunch.c (product_dir_name), gxio installname.rs (product_dir_under) and the Linux
// run.sh (gx_product_dir); scripts/test/productdir-test.cpp holds the case table.
//
//   1. GeneralsVanguard holds an entry whose name does not start with '.'  -> GeneralsVanguard
//   2. else GeneralsX is a folder                                          -> GeneralsX
//   3. else GeneralsVanguard is a folder (empty: a launcher claimed it)    -> GeneralsVanguard
//   4. else nothing is there yet                                           -> GeneralsX, created by the caller as before
//
// Rule 4 keeps every fresh profile (a new player, a harness sandbox) on the old name: this release never creates the new
// one. A dot entry is ignored because Finder writes .DS_Store into any folder a player merely looks at.
#pragma once

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

#define GX_PRODUCT_DIR_NEW "GeneralsVanguard"
#define GX_PRODUCT_DIR_OLD "GeneralsX"

/// In every engine that follows the rule: the R2 launcher byte-scans the installed engine for it before it moves a folder,
/// so an engine that knows only GeneralsX never loses its profile to a move.
#define GX_PRODUCT_DIR_MARKER "GX_READS_BOTH_PRODUCT_DIRS"

/// `dir` holds at least one entry whose name does not start with '.'. A folder that cannot be read holds nothing.
inline bool gxDirHoldsSomething(const std::filesystem::path &dir)
{
	std::error_code ec;
	for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
	{
		// native(), never string(): on Windows string() converts to the ANSI code page and THROWS on a name it cannot
		// represent (a file a player named with an emoji), which here would stop the engine before it has a profile.
		const std::filesystem::path name = it->path().filename();
		if (!name.native().empty() && name.native()[0] != '.')
			return true;
	}
	return false;
}

/// GX_PRODUCT_DIR_NEW or GX_PRODUCT_DIR_OLD: which name to use under `base`. Touches nothing on disk.
inline const char *gxProductDirName(const std::filesystem::path &base)
{
	std::error_code ec;
	const std::filesystem::path fresh = base / GX_PRODUCT_DIR_NEW;
	const bool full = gxDirHoldsSomething(fresh);
	const bool old = std::filesystem::is_directory(base / GX_PRODUCT_DIR_OLD, ec);
	if (full && old)
	{
		// Both: said once per process (callers run on more than one thread), because it is the state an R2 move refused
		// or a player made by hand.
		static std::atomic<bool> s_said{false};
		if (!s_said.exchange(true))
			fprintf(stderr, "[gx] " GX_PRODUCT_DIR_MARKER ": %s holds both " GX_PRODUCT_DIR_NEW " and " GX_PRODUCT_DIR_OLD
				"; using " GX_PRODUCT_DIR_NEW "\n", base.string().c_str());
	}
	if (full)
		return GX_PRODUCT_DIR_NEW;
	if (old)
		return GX_PRODUCT_DIR_OLD;
	if (std::filesystem::is_directory(fresh, ec))
		return GX_PRODUCT_DIR_NEW;
	return GX_PRODUCT_DIR_OLD;
}
