// GeneralsX: WS-X hybrid spike: the DXVK-side interfaces GxInterop calls, mirrored.
//
// The game compiles against the DirectX 8 headers only (min-dx8-sdk on Windows, DXVK's native d3d8.h elsewhere), so the
// d3d9 interop declarations of references/fbraz3-dxvk/src/d3d9/d3d9_interfaces.h are not reachable from here. These are
// vtable-identical mirrors: ID3D9VkInteropDevice and ID3D9VkInteropTexture are DXVK's (upstream, frozen), IDxvkGxExternal
// is the GeneralsX extension in our fork. Any change to IDxvkGxExternal takes a new IID on both sides. Only the methods
// GxInterop calls are declared; the vtable prefix order is what matters, and it is copied verbatim.
#pragma once

#include "GxVulkan.h"

// {2eaa4b89-0107-4bdb-87f7-0f541c493ce0}
static const GUID GX_IID_ID3D9VkInteropDevice =
	{ 0x2eaa4b89, 0x0107, 0x4bdb, { 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0 } };
// {d56344f5-8d35-46fd-806d-94c351b472c1}
static const GUID GX_IID_ID3D9VkInteropTexture =
	{ 0xd56344f5, 0x8d35, 0x46fd, { 0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1 } };
// {703eff94-9fbc-4fce-a0aa-ed0a25773e95} (...93 until DrawRun + GetRunStats, ...92 until DxvkGxRecordInfo
// gained initCmd + inPass: design-r10 #4)
static const GUID GX_IID_IDxvkGxExternal =
	{ 0x703eff94, 0x9fbc, 0x4fce, { 0xa0, 0xaa, 0xed, 0x0a, 0x25, 0x77, 0x3e, 0x95 } };

struct GxIDxvkInteropTexture : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage *pHandle, VkImageLayout *pLayout, VkImageCreateInfo *pInfo) = 0;
};

struct GxIDxvkInteropDevice : public IUnknown {
	virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *pInstance, VkPhysicalDevice *pPhysDev, VkDevice *pDevice) = 0;
	virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *pQueue, uint32_t *pQueueIndex, uint32_t *pQueueFamilyIndex) = 0;
	virtual void STDMETHODCALLTYPE TransitionTextureLayout(GxIDxvkInteropTexture *pTexture,
		const VkImageSubresourceRange *pSubresources, VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
	virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
	virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
	virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};

struct DxvkGxImage {
	VkImage                 image;
	VkImageView             view;
	VkFormat                format;
	VkSampleCountFlagBits   samples;
	VkExtent3D              extent;
	VkImageSubresourceRange range;
	VkImageLayout           layout;
	VkPipelineStageFlags    stages;
	VkAccessFlags           access;
};

struct DxvkGxRecordInfo {
	VkCommandBuffer         cmd;
	DxvkGxImage             colour;
	DxvkGxImage             depth;
	uint32_t                readCount;
	const DxvkGxImage      *reads;
	VkCommandBuffer         initCmd;	// in-pass only: DXVK's init buffer, for uploads + their barrier
	uint32_t                inPass;		// 1: inside DXVK's bound pass - draws only (fork DXVK_GX_INPASS=1)
};

typedef void (*PFN_DxvkGxRecord)(const DxvkGxRecordInfo *pInfo, void *pUser);

struct DxvkGxDeviceInfo {
	PFN_vkGetInstanceProcAddr getInstanceProcAddr;
	VkInstance              instance;
	VkPhysicalDevice        physicalDevice;
	VkDevice                device;
	uint32_t                queueFamily;
	uint32_t                apiVersion;
	VkBool32                dynamicRendering;
	VkBool32                synchronization2;
	VkBool32                timelineSemaphore;
};

// DrawRun (is the prose definition): D3DLIGHT8 has D3DLIGHT9's layout.
struct DxvkGxLightSet {
	UINT                    enableMask;
	UINT                    reserved;
	D3DLIGHT8               lights[4];
};

struct DxvkGxTask {
	D3DMATRIX               world;
	DWORD                   ambient;
	UINT                    lightSet;
	UINT                    normalizeNormals;
	INT                     baseVertex;
	UINT                    minIndex;
	UINT                    numVertices;
	UINT                    startIndex;
	UINT                    primCount;
};

struct DxvkGxRunStats {
	uint64_t calls, refused, tasks, lightSetsApplied, fastTasks, checkedTasks, checkFailed, fastKeyTasks;
};

#define DXVK_GX_RUN_FAST          0x0001u
#define DXVK_GX_RUN_CHECK         0x0002u
#define DXVK_GX_RUN_MUTANT_NOKEY  0x0100u
#define DXVK_GX_RUN_MUTANT_NOAMB  0x0200u
#define DXVK_GX_RUN_VALIDATE_ONLY 0x8000u

struct GxIDxvkGxExternal : public IUnknown {
	virtual HRESULT STDMETHODCALLTYPE GetDeviceInfo(DxvkGxDeviceInfo *pInfo) = 0;
	virtual HRESULT STDMETHODCALLTYPE RecordExternal(UINT ReadCount, GxIDxvkInteropTexture *const *ppReads,
		PFN_DxvkGxRecord pfnRecord, void *pUser, uint64_t RetireValue) = 0;
	virtual uint64_t STDMETHODCALLTYPE GetRetiredValue() = 0;
	virtual HRESULT STDMETHODCALLTYPE DrawRun(D3DPRIMITIVETYPE PrimitiveType, UINT TaskCount, const DxvkGxTask *pTasks,
		UINT LightSetCount, const DxvkGxLightSet *pLightSets, UINT Flags) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetRunStats(DxvkGxRunStats *pStats) = 0;
	virtual HRESULT STDMETHODCALLTYPE DigestMark(UINT Begin, uint64_t Tag) = 0;	// C2b input digest bracket (DXVK_GX_INPUT_DIGEST)
};
