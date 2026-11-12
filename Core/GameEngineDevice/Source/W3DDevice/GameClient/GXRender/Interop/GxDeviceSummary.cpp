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

#include "W3DDevice/GameClient/GxDeviceSummary.h"

#include "GxVulkan.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined( _WIN32 )
#include <windows.h>
#elif defined( __APPLE__ )
#include <sys/sysctl.h>
#endif

int GxPhysicalCores( void )
{
#if defined( _WIN32 )
	DWORD len = 0;
	GetLogicalProcessorInformationEx( RelationProcessorCore, NULL, &len );
	if( len == 0 )
		return 0;
	char *buf = (char *)malloc( len );
	if( buf == NULL )
		return 0;
	int cores = 0;
	if( GetLogicalProcessorInformationEx( RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len ) )
	{
		for( DWORD off = 0; off < len; )
		{
			const PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)( buf + off );
			if( e->Size == 0 )
				break;
			++cores;
			off += e->Size;
		}
	}
	free( buf );
	return cores;
#elif defined( __APPLE__ )
	int n = 0;
	size_t size = sizeof( n );
	return sysctlbyname( "hw.physicalcpu", &n, &size, NULL, 0 ) == 0 && n > 0 ? n : 0;
#else
	// Distinct (physical id, core id) pairs.
	FILE *f = fopen( "/proc/cpuinfo", "r" );
	if( f == NULL )
		return 0;
	char line[ 256 ];
	int phys = -1, count = 0;
	int seen[ 1024 ];
	while( fgets( line, sizeof( line ), f ) != NULL )
	{
		const char *colon = strchr( line, ':' );
		if( colon == NULL )
			continue;
		if( strncmp( line, "physical id", 11 ) == 0 )
			phys = atoi( colon + 1 );
		else if( strncmp( line, "core id", 7 ) == 0 && phys >= 0 )
		{
			const int key = phys * 65536 + atoi( colon + 1 );
			bool dup = false;
			for( int i = 0; i < count && !dup; ++i )
				dup = seen[ i ] == key;
			if( !dup && count < 1024 )
				seen[ count++ ] = key;
		}
	}
	fclose( f );
	return count;
#endif
}

#if !GX_HAVE_VULKAN

bool GxDeviceSummaryRead( IDirect3DDevice8 *, GxDeviceSummary *out )
{
	if( out != nullptr )
		memset( out, 0, sizeof( *out ) );
	return false;
}

#else

#include "GxDxvkExternal.h"

namespace
{

// The vendor's own driverName is often a sentence ("AMD proprietary driver"); the id names it in a word.
const char *shortDriverName( const VkPhysicalDeviceDriverProperties &d )
{
	switch( d.driverID )
	{
		case VK_DRIVER_ID_NVIDIA_PROPRIETARY:			return "NVIDIA";
		case VK_DRIVER_ID_AMD_PROPRIETARY:				return "AMD";
		case VK_DRIVER_ID_AMD_OPEN_SOURCE:				return "AMDVLK";
		case VK_DRIVER_ID_MESA_RADV:					return "RADV";
		case VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS:	return "Intel";
		case VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA:		return "ANV";
		case VK_DRIVER_ID_MOLTENVK:						return "MoltenVK";
		case VK_DRIVER_ID_QUALCOMM_PROPRIETARY:			return "Qualcomm";
		case VK_DRIVER_ID_ARM_PROPRIETARY:				return "ARM";
		case VK_DRIVER_ID_MESA_LLVMPIPE:				return "llvmpipe";
		default:										return d.driverName;
	}
}

}

bool GxDeviceSummaryRead( IDirect3DDevice8 *dev, GxDeviceSummary *out )
{
	if( out == nullptr )
		return false;
	memset( out, 0, sizeof( *out ) );
	if( dev == nullptr )
		return false;

	GxIDxvkGxExternal *ext = nullptr;
	if( FAILED( dev->QueryInterface( GX_IID_IDxvkGxExternal, (void **)&ext ) ) || ext == nullptr )
		return false;
	DxvkGxDeviceInfo info;
	memset( &info, 0, sizeof( info ) );
	const bool got = SUCCEEDED( ext->GetDeviceInfo( &info ) ) && info.getInstanceProcAddr != nullptr && info.physicalDevice != VK_NULL_HANDLE;
	ext->Release();
	if( !got )
		return false;

	const PFN_vkGetPhysicalDeviceProperties props =
		(PFN_vkGetPhysicalDeviceProperties)info.getInstanceProcAddr( info.instance, "vkGetPhysicalDeviceProperties" );
	const PFN_vkGetPhysicalDeviceProperties2 props2 =
		(PFN_vkGetPhysicalDeviceProperties2)info.getInstanceProcAddr( info.instance, "vkGetPhysicalDeviceProperties2" );
	const PFN_vkGetPhysicalDeviceMemoryProperties memory =
		(PFN_vkGetPhysicalDeviceMemoryProperties)info.getInstanceProcAddr( info.instance, "vkGetPhysicalDeviceMemoryProperties" );
	if( props == nullptr )
		return false;

	VkPhysicalDeviceProperties p;
	memset( &p, 0, sizeof( p ) );
	props( info.physicalDevice, &p );
	out->apiVersion = p.apiVersion;

	// Driver properties are core from Vulkan 1.2; below it the struct must not be chained.
	if( props2 != nullptr && p.apiVersion >= VK_API_VERSION_1_2 )
	{
		VkPhysicalDeviceDriverProperties d;
		memset( &d, 0, sizeof( d ) );
		d.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
		VkPhysicalDeviceProperties2 p2;
		memset( &p2, 0, sizeof( p2 ) );
		p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		p2.pNext = &d;
		props2( info.physicalDevice, &p2 );
		d.driverName[ VK_MAX_DRIVER_NAME_SIZE - 1 ] = '\0';
		d.driverInfo[ VK_MAX_DRIVER_INFO_SIZE - 1 ] = '\0';
		char version[ VK_MAX_DRIVER_INFO_SIZE ];
		snprintf( version, sizeof( version ), "%s", d.driverInfo );
		if( char *aside = strstr( version, " (" ) )	// "24.10.1 (AMD proprietary shader compiler)": the version is enough
			*aside = '\0';
		snprintf( out->driver, sizeof( out->driver ), "%s%s%s", shortDriverName( d ), version[ 0 ] != '\0' ? " " : "", version );
	}

	if( memory != nullptr )
	{
		VkPhysicalDeviceMemoryProperties m;
		memset( &m, 0, sizeof( m ) );
		memory( info.physicalDevice, &m );
		unsigned long long bytes = 0;
		for( uint32_t i = 0; i < m.memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i )
			if( m.memoryHeaps[ i ].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT )
				bytes += m.memoryHeaps[ i ].size;
		out->vramMb = (unsigned)( bytes / ( 1024ull * 1024ull ) );
	}
	return true;
}

#endif // GX_HAVE_VULKAN
