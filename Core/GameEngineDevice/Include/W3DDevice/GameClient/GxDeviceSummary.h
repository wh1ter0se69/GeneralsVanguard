/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

// What the Vulkan device behind DXVK is - driver, device-local memory, API version - for the hardware report
// (GameClient/GXHwReport.h). Read through the patched DXVK's IDxvkGxExternal, the same door GxDepthShot uses. Not digested.

#pragma once

struct IDirect3DDevice8;

struct GxDeviceSummary
{
	char driver[ 64 ];		///< short driver name and its version ("NVIDIA 566.36"); empty below Vulkan 1.2
	unsigned vramMb;		///< device-local heaps, MiB; 0 when unknown
	unsigned apiVersion;	///< the physical device's VK_MAKE_API_VERSION; 0 when unknown
};

/// False, with *out zeroed, on a stock DXVK, a native D3D8, no device, or a build without DXVK's Vulkan headers.
bool GxDeviceSummaryRead( IDirect3DDevice8 *dev, GxDeviceSummary *out );

/// Physical CPU cores (not threads), or 0 when the OS does not say (an ARM Linux /proc/cpuinfo has no core ids).
int GxPhysicalCores( void );
