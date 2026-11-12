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

// GxNativeLoaderDirect.h.
#include "GxNativeLoaderDirect.h"

#include <set>

namespace GXRender { namespace Dev {

namespace {
std::string leaf(const std::string& p)
{
	const size_t s = p.rfind('/');
	return s == std::string::npos ? p : p.substr(s + 1);
}
bool isMvk(const std::string& p) { return leaf(p).find("MoltenVK") != std::string::npos; }
bool isLoaderImage(const std::string& p) { return leaf(p).compare(0, 9, "libvulkan") == 0 && !isMvk(p); }
}

bool GxNativeQualifyLoaderDirect(const GxLoaderFacts& f, bool afterCreate, std::string* mvkPath, std::string* why)
{
	std::set<std::string> mvks;
	for (const std::string& i : f.images) {
		if (isMvk(i))
			mvks.insert(i);
		else if (isLoaderImage(i)) {
			*why = "a Vulkan loader '" + i + "' is loaded (this platform binds MoltenVK directly)";
			return false;
		}
	}
	if (mvks.size() > 1) {
		*why = "more than one MoltenVK is loaded ('" + *mvks.begin() + "' and '" + *mvks.rbegin() + "')";
		return false;
	}
	if (!f.procAddrImage.empty() && !isMvk(f.procAddrImage)) {
		*why = "the host's vkGetInstanceProcAddr is in '" + f.procAddrImage + "', not a MoltenVK image";
		return false;
	}
	if (afterCreate) {
		if (mvks.empty()) {
			*why = "no MoltenVK image is loaded after the device was created";
			return false;
		}
		if (!f.procAddrImage.empty() && !mvks.count(f.procAddrImage)) {
			*why = "the host's MoltenVK '" + f.procAddrImage + "' is not the process's MoltenVK '" + *mvks.begin() + "'";
			return false;
		}
	}
	if (mvkPath)
		*mvkPath = mvks.empty() ? f.procAddrImage : *mvks.begin();
	return true;
}

} }
