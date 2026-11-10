// GeneralsX: __uuidof for the d3d8 interfaces the native device answers QueryInterface for. DXVK's
// native d3d8.h declares the IIDs (DEFINE_GUID, d3d8.h:90-123) but no __uuidof helpers; these are the same values. Included only by
// the device's own .cpp files (an explicit specialization must not meet another one for the same type).
#pragma once
#include <d3d8.h>

#if defined(_MSC_VER)
// MSVC's __uuidof is the compiler's own. The DirectX 8 SDK headers (min-dx8-sdk d3d8.h) declare the
// IIDs with DEFINE_GUID but attach no uuid to the interfaces, so the same values are attached here; IUnknown has its own (unknwn.h).
struct __declspec(uuid("7385E5DF-8FE8-41D5-86B6-D7B48547B6CF")) IDirect3DDevice8;
struct __declspec(uuid("1B36BB7B-09B7-410A-B445-7D1430D7B33F")) IDirect3DResource8;
struct __declspec(uuid("8AEEEAC7-05F9-44D4-B591-000B0DF1CB95")) IDirect3DVertexBuffer8;
struct __declspec(uuid("B96EEBCA-B326-4EA5-882F-2FF5BAE021DD")) IDirect3DSurface8;
struct __declspec(uuid("0E689C9A-053D-44A0-9D92-DB0E3D750F86")) IDirect3DIndexBuffer8;
struct __declspec(uuid("B4211CFA-51B9-4A9F-AB78-DB99B2BB678E")) IDirect3DBaseTexture8;
struct __declspec(uuid("E4CDD575-2866-4F01-B12E-7EECE1EC9358")) IDirect3DTexture8;
#else
#ifdef GX_RENDER_NATIVE
// rw3-K 11/10/2026: inside the game <d3d8.h> comes through the game's CompatLib windows.h, whose IUnknown has no __uuidof helper
// (the link failed: undefined GUID __uuidof_helper<IUnknown>()). DXVK's own windows_base.h, used by the standalone build, has it.
DECLARE_UUIDOF_HELPER(IUnknown,               0x00000000,0x0000,0x0000,0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46)
#endif
DECLARE_UUIDOF_HELPER(IDirect3DDevice8,       0x7385E5DF,0x8FE8,0x41D5,0x86,0xB6,0xD7,0xB4,0x85,0x47,0xB6,0xCF)
DECLARE_UUIDOF_HELPER(IDirect3DResource8,     0x1B36BB7B,0x09B7,0x410A,0xB4,0x45,0x7D,0x14,0x30,0xD7,0xB3,0x3F)
DECLARE_UUIDOF_HELPER(IDirect3DVertexBuffer8, 0x8AEEEAC7,0x05F9,0x44D4,0xB5,0x91,0x00,0x0B,0x0D,0xF1,0xCB,0x95)
DECLARE_UUIDOF_HELPER(IDirect3DSurface8,      0xB96EEBCA,0xB326,0x4EA5,0x88,0x2F,0x2F,0xF5,0xBA,0xE0,0x21,0xDD)
DECLARE_UUIDOF_HELPER(IDirect3DIndexBuffer8,  0x0E689C9A,0x053D,0x44A0,0x9D,0x92,0xDB,0x0E,0x3D,0x75,0x0F,0x86)
DECLARE_UUIDOF_HELPER(IDirect3DBaseTexture8,  0xB4211CFA,0x51B9,0x4A9F,0xAB,0x78,0xDB,0x99,0xB2,0xBB,0x67,0x8E)
DECLARE_UUIDOF_HELPER(IDirect3DTexture8,      0xE4CDD575,0x2866,0x4F01,0xB1,0x2E,0x7E,0xEC,0xE1,0xEC,0x93,0x58)
#endif
