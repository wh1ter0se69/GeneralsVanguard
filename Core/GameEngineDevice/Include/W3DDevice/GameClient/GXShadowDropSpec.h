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

// TEST-ONLY: the drop spec GX_SHADOW_DROP_MESH /
// GX_SHADOWMAP_DROP_CAST name - pure parsing and matching, host-tested (scripts/test/sm-fit/sm-fit-test.cpp A73-drop-*, with the
// compile-time mutants sm-fit-test.sh builds), used by W3DVolumetricShadowManager::gxShadowDropMesh. Render-only diagnostics.
//
// A value is ONE entry, or a comma list of up to 8. An entry is <CONTAINERPREFIX>.<MESH>, case-insensitive: a mesh name matches
// when its name after the last '.' is MESH and the part before starts with CONTAINERPREFIX; an entry ending in '.'
// matches every mesh of that container prefix. Any invalid entry leaves the whole spec off. A single entry with a mesh parses exactly as
// the single-entry parser does (the host test compares the two).
#pragma once

#include <cctype>
#include <cstddef>
#include <cstring>

namespace GXShadowDrop {

struct Entry
{
	char prefix[64] = {}, mesh[64] = {};   // mesh empty = every mesh of the prefix (A73)
};

struct Spec
{
	bool on = false;
	unsigned count = 0;
	Entry entries[8];

	// value = the environment string (null = off)
	void parse(const char *value)
	{
		on = false;
		count = 0;
		if (value == nullptr || *value == '\0')
			return;
		const char *p = value;
		for (;;)
		{
			const char *end = strchr(p, ',');
#ifdef GX_SM_DROP_MUTANT_NOLIST
			end = nullptr;   // X-: the list is read as one entry
#endif
			const size_t n = end ? size_t(end - p) : strlen(p);
			if (count == 8 || !parseEntry(p, n, entries[count]))
			{
				count = 0;
				return;
			}
			++count;
			if (end == nullptr)
				break;
			p = end + 1;
		}
		on = true;
	}

	bool matches(const char *name) const
	{
		if (!on || name == nullptr)
			return false;
		for (unsigned i = 0; i < count; ++i)
			if (matchEntry(entries[i], name))
				return true;
		return false;
	}

private:
	// A60's rule on one entry of n chars: the LAST '.' splits it; a non-empty prefix under 64 chars, a mesh under 64 chars (A60 also
	// needed a non-empty mesh; A73: empty = every mesh)
	static bool parseEntry(const char *s, size_t n, Entry &e)
	{
		const char *dot = nullptr;
		for (size_t i = 0; i < n; ++i)
			if (s[i] == '.')
				dot = s + i;
		if (dot == nullptr || dot == s)
			return false;
		const size_t np = size_t(dot - s), nm = n - np - 1;
		if (np >= sizeof(e.prefix) || nm >= sizeof(e.mesh))
			return false;
		memcpy(e.prefix, s, np);
		e.prefix[np] = '\0';
		memcpy(e.mesh, dot + 1, nm);
		e.mesh[nm] = '\0';
		return true;
	}

	static bool matchEntry(const Entry &e, const char *name)
	{
		const char *dot = strrchr(name, '.');
		const size_t np = strlen(e.prefix);
		if (dot == nullptr || size_t(dot - name) < np)
			return false;
		for (size_t i = 0; i < np; ++i)
			if (tolower((unsigned char)name[i]) != tolower((unsigned char)e.prefix[i]))
				return false;
		if (e.mesh[0] == '\0')
			return true;
		const char *a = dot + 1, *b = e.mesh;
		for (; *a != '\0' && *b != '\0'; ++a, ++b)
			if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
				return false;
#ifdef GX_SM_DROP_MUTANT_MESHPREFIX
		return *b == '\0';   // X-: the mesh compared as a prefix (PLANE0 would match PLANE01)
#else
		return *a == '\0' && *b == '\0';
#endif
	}
};

} // namespace GXShadowDrop
