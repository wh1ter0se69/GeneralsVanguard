// GeneralsX: WS-X hybrid spike - see W3DDevice/GameClient/GxInterop.h and
// (§1 protocol, §4 measurement, §8 gates).
//
// Threads: Begin_Site / the flush boundaries run on the main thread and only queue work; DX8Wrapper::Device_Call runs the d3d8 part on the
// thread that owns the device at that point of the stream (the d3d8 ring's consumer inside the object-flush bracket, the
// main thread otherwise); in P-L the recorder then runs on DXVK's CS thread inside DXVK's command list. A packet is
// written by the main thread (site, retire value), then by the device thread (projection, viewport), then only read.
#include "W3DDevice/GameClient/GxInterop.h"
#include "W3DDevice/GameClient/GxTestEnv.h"

#include "GxVulkan.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/dx8renderer.h"
#include "GameLogic/GameLogic.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>
#include <mutex>
#include <thread>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <pthread.h>
#endif

namespace GxInterop
{

int Mode()
{
	static int s_mode = -1;
	if (s_mode < 0)
	{
		const char *e = getenv("GX_RENDER_HYBRID");
		const int m = e != nullptr ? atoi(e) : 0;
		s_mode = (m == 1 || m == 2) ? m : 0;
	}
	return s_mode;
}

#if !GX_HAVE_VULKAN

void Install()
{
	if (Mode() != 0)
		fprintf(stderr, "[GXHYB] unavailable: built without DXVK's Vulkan headers (references/fbraz3-dxvk not checked out)\n");
}

#else // GX_HAVE_VULKAN

} // namespace GxInterop

#include "GxDxvkExternal.h"
#include "GxInteropTestVert.h"
#include "GxInteropTestFrag.h"
#include "GxInteropTexFrag.h"
#include "GxInteropWhiteFrag.h"

namespace GxInterop
{
namespace
{

// DXVK's entry points, all resolved through the vkGetInstanceProcAddr DXVK itself uses (criterion 5: provenance).
struct VkFns
{
	PFN_vkGetDeviceProcAddr getDeviceProcAddr;
	PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties;
	PFN_vkCreateBuffer createBuffer;
	PFN_vkGetBufferMemoryRequirements bufferRequirements;
	PFN_vkAllocateMemory allocateMemory;
	PFN_vkBindBufferMemory bindBufferMemory;
	PFN_vkMapMemory mapMemory;
	PFN_vkCreateShaderModule createShaderModule;
	PFN_vkCreatePipelineLayout createPipelineLayout;
	PFN_vkCreateGraphicsPipelines createGraphicsPipelines;
	PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2;
	PFN_vkCmdBeginRendering cmdBeginRendering;
	PFN_vkCmdEndRendering cmdEndRendering;
	PFN_vkCmdSetViewport cmdSetViewport;
	PFN_vkCmdSetScissor cmdSetScissor;
	PFN_vkCmdBindPipeline cmdBindPipeline;
	PFN_vkCmdBindVertexBuffers cmdBindVertexBuffers;
	PFN_vkCmdPushConstants cmdPushConstants;
	PFN_vkCmdDraw cmdDraw;
	PFN_vkCmdDrawIndirect cmdDrawIndirect;
	// P-Q only
	PFN_vkCreateCommandPool createCommandPool;
	PFN_vkAllocateCommandBuffers allocateCommandBuffers;
	PFN_vkBeginCommandBuffer beginCommandBuffer;
	PFN_vkEndCommandBuffer endCommandBuffer;
	PFN_vkResetCommandBuffer resetCommandBuffer;
	PFN_vkCreateFence createFence;
	PFN_vkWaitForFences waitForFences;
	PFN_vkResetFences resetFences;
	PFN_vkQueueSubmit2 queueSubmit2;
	PFN_vkCreateImageView createImageView;
	PFN_vkDestroyImageView destroyImageView;
	PFN_vkDestroyBuffer destroyBuffer;
	PFN_vkFreeMemory freeMemory;
	PFN_vkDestroyShaderModule destroyShaderModule;
	PFN_vkDestroyPipelineLayout destroyPipelineLayout;
	PFN_vkDestroyPipeline destroyPipeline;
	PFN_vkDestroyCommandPool destroyCommandPool;
	PFN_vkDestroyFence destroyFence;
	PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout;
	PFN_vkDestroyDescriptorPool destroyDescriptorPool;
	PFN_vkDestroySampler destroySampler;
	PFN_vkCmdUpdateBuffer cmdUpdateBuffer;
	// criterion 2 texture test
	PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout;
	PFN_vkCreateDescriptorPool createDescriptorPool;
	PFN_vkAllocateDescriptorSets allocateDescriptorSets;
	PFN_vkUpdateDescriptorSets updateDescriptorSets;
	PFN_vkCreateSampler createSampler;
	PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets;
};

enum { STATE_IDLE, STATE_QUEUED, STATE_READY, STATE_FAILED };
enum { MUTANT_NONE, MUTANT_NOBARRIER, MUTANT_NOPREP, MUTANT_NOOUT, MUTANT_EARLYREUSE, MUTANT_DEPTHBIAS, MUTANT_NOFEATURE,
	MUTANT_STENCILCLEAR, MUTANT_NOUPBARRIER, MUTANT_INPASSUPLOAD };

const int INSTANCES = 16;
const uint32_t PACKETS = 8192;	// GX_RENDER_HYBRID_SPLIT up to ~2,000 insertions/frame x DXVK's frames in flight		// a slot (and its descriptor set) is reused only after its retire value retired (P-L)
const int QSLOTS = 16;

struct Packet
{
	// Ownership: a slot is free when its last packet is DONE (refused, or consumed by P-Q's device
	// thread) or, in P-L, when its retire value retired on the GPU. epoch = the Present count when it was queued: a busy
	// slot of the current epoch is never waited for (its work cannot be submitted before this frame's Present).
	std::atomic<int> done;
	uint64_t epoch;
	uint64_t retire;
	int site;
	float proj[16];
	D3DVIEWPORT8 viewport;
	int textured;			// GX_HYBRID_TEXTEST: the cubes sample s_tex8 through the export contract
	int indexInFrame;		// the insertion's index within its render frame
	int noDraw;				// this recording is the boundary alone (the clear fixture's pass-ending first call)
	uint64_t frameSeq;		// render frames seen so far
	float offset[4];		// view-space shift of the whole test mesh (push constant)
	VkImageView rawView;	// MUTANT_NOPREP: the raw handle's view, no preparation, no tracking
	VkImageLayout rawLayout;
};

struct QSlot	// P-Q: one external submission
{
	VkCommandBuffer cmd;
	VkFence fence;
	bool busy;
	IUnknown *hold[4];	// the surfaces and their interop views, released once the fence signalled
	VkImageView views[2];	// colour + depth views made for THIS submission, destroyed with it (code-r3 #1)
};

std::atomic<int> s_state(STATE_IDLE);
int s_visible = 0;
int s_mutant = MUTANT_NONE;
bool s_draw = true;		// GX_RENDER_HYBRID_DRAW=0: the boundary alone - barriers + an empty LOAD/STORE rendering instance

IDirect3DDevice8 *s_dev8 = nullptr;	// the d3d8 device Device_Init ran on (not AddRef'd: DX8Wrapper owns it)
GxIDxvkGxExternal *s_ext = nullptr;
std::atomic<uint64_t> s_lastAccepted(0);	// the retire value of the last packet RecordExternal accepted (P-L)
std::vector<std::pair<VkBuffer, VkDeviceMemory> > s_buffers;	// every buffer we made, for Shutdown
VkDescriptorPool s_descPool = VK_NULL_HANDLE;
GxIDxvkInteropDevice *s_qdev = nullptr;
DxvkGxDeviceInfo s_info;
VkFns s_vk;
VkBuffer s_cubeBuffer = VK_NULL_HANDLE;
VkBuffer s_instanceBuffer = VK_NULL_HANDLE;
VkShaderModule s_vert = VK_NULL_HANDLE;
VkShaderModule s_frag = VK_NULL_HANDLE;
VkPipelineLayout s_layout = VK_NULL_HANDLE;
std::mutex s_pipelineLock;
std::map<uint64_t, VkPipeline> s_pipelines;

VkQueue s_queue = VK_NULL_HANDLE;
VkCommandPool s_pool = VK_NULL_HANDLE;
QSlot s_qslots[QSLOTS];
uint32_t s_qnext = 0;

Packet s_packets[PACKETS];

// GX_HYBRID_TEXTEST=1 (criterion 2): a 4x4 MANAGED A8R8G8B8 texture created by the device thread and never drawn by DXVK
// (first use = our draw), refilled with the next colour every 120 render frames by Lock/Unlock (a dirty update the draw of
// that same insertion must already see). [GXHYBTEX] logs every fill; the cubes in the pictures must show the last colour.
int s_texTest = 0;
IDirect3DTexture8 *s_tex8 = nullptr;
GxIDxvkInteropTexture *s_texInterop = nullptr;
unsigned s_texFrame = ~0u;
uint32_t s_texFills = 0;
VkShaderModule s_texFrag = VK_NULL_HANDLE;
VkDescriptorSetLayout s_texSetLayout = VK_NULL_HANDLE;
VkPipelineLayout s_texLayout = VK_NULL_HANDLE;
VkDescriptorSet s_texSets[PACKETS];
VkSampler s_sampler = VK_NULL_HANDLE;
std::map<VkImage, VkImageView> s_rawViews;

// GX_HYBRID_EQUALTEST (criterion 3, needs GX_RENDER_HYBRID_VISIBLE=1): the cubes are drawn natively WITH depth writes from
// pre-transformed view-space positions (identical inputs on both paths), then a second pass over the same triangles with
// ZFUNC EQUAL paints white where it passes. 1 = mixed path: that second pass is a classic d3d8 fixed-function draw through
// DXVK (the shroud pass's situation, W3DShaderManager.cpp:1304); 2 = same path: the second pass is ours too. Coloured cube
// pixels left in a picture = depth-EQUAL failures.
int s_equalTest = 0;
// GX_HYBRID_CLEARTEST=1 (VISIBLE runs): the first insertion of each frame is preceded, on the device thread, by a colour
// Clear of the bound target to blue - a clear DXVK defers into its next render pass. Our cubes are recorded after it, so they
// must show on the blue; a DXVK that kept the clear pending past our commands (fork fail arm DXVK_GX_SUSPEND=1) applies it
// as its next pass's load op and wipes them.
bool s_clearTest = false;
// In-pass mode (the fork's DXVK_GX_INPASS=1, read by the fork at device creation): an insertion inside a pass DXVK has bound
// records only its draws - no barrier, no rendering instance, no tile store. s_inPassAsked mirrors the
// switch so the rows can count which insertions really ran in-pass and which fell back to the break path.
bool s_inPassAsked = false;
int s_split = 1;	// GX_RENDER_HYBRID_SPLIT (1..1024; PACKETS covers 1024/frame x DXVK's frames in flight)
// GX_RENDER_HYBRID_SITES=<bit mask of Site> (default all): insert only at these sites - the census still counts every site. Bit 7 =
// PROJ_SHADOW, the flush on the MAIN thread outside the ring's bracket, where Device_Call runs the insertion inline (render-integration
// 10/10: its own per-switch number).
unsigned s_siteMask = ~0u;
// GX_RENDER_HYBRID_INTERLEAVE=1 (P-L toggle runs): after every insertion, on the device thread, one
// classic draw re-issues the device's CURRENT state - one triangle of the vertex buffer the segment's last classic draw bound, no
// state change - so DXVK re-binds its whole state after each insertion, as Mode A's native/classic alternation makes it do. The
// OFF periods issue the same classic draws at the same points without the insertions, so ON - OFF = the insertions + DXVK's
// re-binds. Each triangle is drawn twice: a timing arm, never a picture.
bool s_interleave = false;
// GX_HYBRID_UPLOADTEST=1 (in-pass rule: an in-pass draw's data is uploaded BEFORE the pass): the cube instances come from a
// device-local buffer region per packet, written by vkCmdUpdateBuffer into DXVK's init buffer (in-pass) or inline before
// our rendering instance (break path), each followed by a TRANSFER -> VERTEX_ATTRIBUTE barrier. Fail arm
// GX_HYBRID_MUTANT=noupbarrier drops that barrier: synchronization validation must report the read-after-write.
// OBSERVED (farm val3): in-pass, that arm does not fire - DXVK finalises its own uploads' TRANSFER -> vertex-input barrier at
// the end of the same init buffer, which covers ours. =2 uploads the DRAW ARGUMENTS instead (vkCmdDrawIndirect, barrier
// TRANSFER -> DRAW_INDIRECT): a D3D9 front end never reads a buffer at DRAW_INDIRECT, so no barrier of DXVK's covers that read
// and noupbarrier must fire there. Fail arm inpassupload: the upload and its barrier recorded INSIDE DXVK's open pass (the
// forbidden-inside rule) - core validation must reject it. =3 uploads the cube's VERTICES (a per-vertex
// binding) instead of the instances: INFERRED from val3 (WUPX, the break-path noupbarrier arm, did not fire either) that
// synchronization validation does not check instance-rate vertex reads - =3 against =1 under noupbarrier tests that.
int s_uploadTest = 0;
float s_cubeData[36 * 3];
VkBuffer s_upBuffer = VK_NULL_HANDLE, s_indBuffer = VK_NULL_HANDLE;
float s_instData[16 * 8];
const int EQUAL_VERTS = 36 * 16;
VkShaderModule s_whiteFrag = VK_NULL_HANDLE;
VkBuffer s_equalBuffer = VK_NULL_HANDLE, s_equalInstance = VK_NULL_HANDLE;
struct ClassicVertex { float x, y, z; DWORD diffuse; };
ClassicVertex s_classicVerts[EQUAL_VERTS];

enum { VARIANT_TEXTURED = 1, VARIANT_DEPTHWRITE = 2, VARIANT_EQUAL_WHITE = 4, VARIANT_DEPTHBIAS = 8 };
uint64_t s_nextRetire = 0;

// counters: main thread (insertions, frames), any thread (the atomics)
uint64_t s_bySite[SITE_COUNT];
uint64_t s_byBoundary[2], s_censusSegments[SITE_COUNT][2], s_censusDraws[SITE_COUNT][2];	// per site x boundary (design-r3 #5)
uint64_t s_censusNonEmpty[SITE_COUNT][2];	// ... segments that drew something: the exact per-site insertion lower bound (design-r4 #3)
uint64_t s_onRequested = 0;	// ON-period boundaries that drew something - before any capacity skip (design-r3 #3)
uint64_t s_ringSkips = 0;
int s_site = -1;	// the L2 producer flush running now (Site_Of(GxFlushSite) at its boundary), -1 = not an insertion site
uint64_t s_frames = 0, s_insertions = 0, s_frameIns = 0, s_frameMin = ~0ull, s_frameMax = 0, s_packetWaits = 0;
unsigned s_lastFrame = ~0u;
std::atomic<uint64_t> s_refused(0), s_recorded(0), s_pipelinesMade(0), s_pipelineUs(0), s_fenceWaits(0), s_noPipeline(0);
std::atomic<uint64_t> s_devNs(0), s_recNs(0);	// CPU time per insertion: device thread (capture + hand-off/submit), recorder
// per toggle window (reset at each [GXHYBT] row): attempts the producer queued, accepted by RecordExternal (or submitted, P-Q),
// recordings completed, refusals; a window whose recorded != attempts did not run the ON workload it claims. Design-r4 #2:
// recordings that should have drawn (draws_wanted) and did (draws_done: every pipeline present), and failed P-Q submits.
std::atomic<uint64_t> s_winAccepted(0), s_winRecorded(0), s_winRefused(0), s_winDrawWant(0), s_winDrawn(0), s_winSubmitFail(0);
std::atomic<uint64_t> s_winInPass(0), s_winFallback(0), s_inPassTotal(0), s_fallbackTotal(0);
std::atomic<uint64_t> s_winRedrawOn(0), s_winRedrawOff(0), s_winRedrawSkip(0);	// INTERLEAVE's classic draws after an insertion / in OFF periods / not drawable
std::atomic<uint64_t> s_classicEqualDraws(0);	// our classic draws (EQUALTEST=1, INTERLEAVE), issued on the device (ring) thread (code-r6 #1)	// in-pass mode: recordings inside DXVK's pass / on the break path

uint64_t Now_Ns()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}


// GX_RENDER_HYBRID_TOGGLE=k: insertions only in odd periods of k render frames (OFF, ON, OFF, ...), so one run gives a
// paired ON-vs-OFF frame period on the same content - meant for the replay's static hold window (GX_REPLAY_PAUSE).
// The first GX_RENDER_HYBRID_TOGGLE_DROP (default 3) frames of every period are dropped: with frames in flight a switch
// shows in the CPU frame period only ~2 frames later. GX_RENDER_HYBRID_TOGGLE_NULL=1 keeps the toggle bookkeeping with
// BOTH states off - the fail arm: its ON-minus-OFF must be ~0 within its spread, or the alternation itself is the artefact.
int s_toggleK = 0, s_toggleDrop = 3;
// GX_RENDER_HYBRID_THREADS=1 (macOS toggle runs; render-integration's criterion-4 (b), 10/10): the CPU time of every thread of
// the process per toggle state, counted from frame `drop` of a period to the period's end - DXVK's frame latency (<= drop) keeps
// each span's work its own state's. Printed per published row as [GXHYBTH] (rw1x/hybth.py): which thread pays the ON work, and
// how busy each thread is against the frame period (a thread with slack hides its share; one on the critical path does not).
bool s_threads = false;
struct ThreadAcc { char name[32]; uint64_t onNs, offNs; };
std::map<uint64_t, ThreadAcc> s_thrAcc;	// by Mach thread id
std::map<uint64_t, uint64_t> s_thrStart;	// CPU ns at the open span's start
uint64_t s_thrOnFrames = 0, s_thrOffFrames = 0;
unsigned s_thrPeriod = ~0u, s_thrFrame0 = 0;
bool s_thrSpan = false;
bool s_toggleNull = false;
struct Moments { double n, sum, sumsq; void add(double v) { n += 1; sum += v; sumsq += v * v; } };
Moments s_period, s_on, s_off, s_diff;
double s_lastOffMean = -1.0;
uint64_t s_lastOffPeriod = ~0ull, s_periods = 0, s_dropped = 0, s_onFrames = 0, s_onIns = 0, s_lastTsNs = 0;
unsigned s_winLogic0 = 0, s_winFrame0 = 0;
// a row whose statistics closed on an ON period, published when the following OFF period closes (design-r5 #1)
char s_rowHead[640];
bool s_rowPending = false;
double s_rowOnFrames = 1.0;
uint64_t s_rowRequested = 0, s_rowAttempts = 0;

bool Load(const char *name, PFN_vkVoidFunction fn, int &missing)
{
	if (fn == nullptr)
	{
		fprintf(stderr, "[GXHYB] missing entry point %s\n", name);
		++missing;
	}
	return fn != nullptr;
}

#define GX_DEV_FN(member, name) \
	s_vk.member = (PFN_##name)s_vk.getDeviceProcAddr(s_info.device, #name); Load(#name, (PFN_vkVoidFunction)s_vk.member, missing)

bool s_loaded = false;	// every Vulkan entry point resolved: the only way any object of ours exists (Shutdown's guard)

bool Load_Functions()
{
	int missing = 0;
	s_vk.getDeviceProcAddr = (PFN_vkGetDeviceProcAddr)s_info.getInstanceProcAddr(s_info.instance, "vkGetDeviceProcAddr");
	s_vk.getMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
		s_info.getInstanceProcAddr(s_info.instance, "vkGetPhysicalDeviceMemoryProperties");
	if (!Load("vkGetDeviceProcAddr", (PFN_vkVoidFunction)s_vk.getDeviceProcAddr, missing)
		|| !Load("vkGetPhysicalDeviceMemoryProperties", (PFN_vkVoidFunction)s_vk.getMemoryProperties, missing))
		return false;
	GX_DEV_FN(createBuffer, vkCreateBuffer);
	GX_DEV_FN(bufferRequirements, vkGetBufferMemoryRequirements);
	GX_DEV_FN(allocateMemory, vkAllocateMemory);
	GX_DEV_FN(bindBufferMemory, vkBindBufferMemory);
	GX_DEV_FN(mapMemory, vkMapMemory);
	GX_DEV_FN(createShaderModule, vkCreateShaderModule);
	GX_DEV_FN(createPipelineLayout, vkCreatePipelineLayout);
	GX_DEV_FN(createGraphicsPipelines, vkCreateGraphicsPipelines);
	GX_DEV_FN(cmdPipelineBarrier2, vkCmdPipelineBarrier2);
	GX_DEV_FN(cmdBeginRendering, vkCmdBeginRendering);
	GX_DEV_FN(cmdEndRendering, vkCmdEndRendering);
	GX_DEV_FN(cmdSetViewport, vkCmdSetViewport);
	GX_DEV_FN(cmdSetScissor, vkCmdSetScissor);
	GX_DEV_FN(cmdBindPipeline, vkCmdBindPipeline);
	GX_DEV_FN(cmdBindVertexBuffers, vkCmdBindVertexBuffers);
	GX_DEV_FN(cmdPushConstants, vkCmdPushConstants);
	GX_DEV_FN(cmdDraw, vkCmdDraw);
	GX_DEV_FN(cmdDrawIndirect, vkCmdDrawIndirect);
	GX_DEV_FN(createCommandPool, vkCreateCommandPool);
	GX_DEV_FN(allocateCommandBuffers, vkAllocateCommandBuffers);
	GX_DEV_FN(beginCommandBuffer, vkBeginCommandBuffer);
	GX_DEV_FN(endCommandBuffer, vkEndCommandBuffer);
	GX_DEV_FN(resetCommandBuffer, vkResetCommandBuffer);
	GX_DEV_FN(createFence, vkCreateFence);
	GX_DEV_FN(waitForFences, vkWaitForFences);
	GX_DEV_FN(resetFences, vkResetFences);
	GX_DEV_FN(queueSubmit2, vkQueueSubmit2);
	GX_DEV_FN(createImageView, vkCreateImageView);
	GX_DEV_FN(destroyImageView, vkDestroyImageView);
	GX_DEV_FN(destroyBuffer, vkDestroyBuffer);
	GX_DEV_FN(freeMemory, vkFreeMemory);
	GX_DEV_FN(destroyShaderModule, vkDestroyShaderModule);
	GX_DEV_FN(destroyPipelineLayout, vkDestroyPipelineLayout);
	GX_DEV_FN(destroyPipeline, vkDestroyPipeline);
	GX_DEV_FN(destroyCommandPool, vkDestroyCommandPool);
	GX_DEV_FN(destroyFence, vkDestroyFence);
	GX_DEV_FN(destroyDescriptorSetLayout, vkDestroyDescriptorSetLayout);
	GX_DEV_FN(destroyDescriptorPool, vkDestroyDescriptorPool);
	GX_DEV_FN(destroySampler, vkDestroySampler);
	GX_DEV_FN(createDescriptorSetLayout, vkCreateDescriptorSetLayout);
	GX_DEV_FN(createDescriptorPool, vkCreateDescriptorPool);
	GX_DEV_FN(allocateDescriptorSets, vkAllocateDescriptorSets);
	GX_DEV_FN(updateDescriptorSets, vkUpdateDescriptorSets);
	GX_DEV_FN(createSampler, vkCreateSampler);
	GX_DEV_FN(cmdBindDescriptorSets, vkCmdBindDescriptorSets);
	GX_DEV_FN(cmdUpdateBuffer, vkCmdUpdateBuffer);
	s_loaded = missing == 0;
	return s_loaded;
}

#undef GX_DEV_FN

// A host-visible, coherent buffer filled once; it is never written again, so it needs no retirement.
VkBuffer Make_Static_Buffer(const void *data, VkDeviceSize size, bool deviceLocal = false,
	VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
{
	VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bi.size = size;
	bi.usage = usage | (deviceLocal ? VK_BUFFER_USAGE_TRANSFER_DST_BIT : 0);
	bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VkBuffer buffer = VK_NULL_HANDLE;
	if (s_vk.createBuffer(s_info.device, &bi, nullptr, &buffer) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	VkMemoryRequirements req;
	s_vk.bufferRequirements(s_info.device, buffer, &req);
	VkPhysicalDeviceMemoryProperties props;
	s_vk.getMemoryProperties(s_info.physicalDevice, &props);
	// deviceLocal: written only by vkCmdUpdateBuffer (GX_HYBRID_UPLOADTEST), never mapped
	const VkMemoryPropertyFlags want = deviceLocal ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
		: VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type = ~0u;
	for (uint32_t i = 0; i < props.memoryTypeCount && type == ~0u; ++i)
		if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
			type = i;
	if (type == ~0u)
	{
		s_buffers.push_back(std::make_pair(buffer, (VkDeviceMemory)VK_NULL_HANDLE));
		return VK_NULL_HANDLE;
	}
	VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void *mapped = nullptr;
	const bool allocated = s_vk.allocateMemory(s_info.device, &ai, nullptr, &memory) == VK_SUCCESS;
	s_buffers.push_back(std::make_pair(buffer, allocated ? memory : VK_NULL_HANDLE));	// Shutdown destroys both
	if (!allocated
		|| s_vk.bindBufferMemory(s_info.device, buffer, memory, 0) != VK_SUCCESS
		|| (!deviceLocal && s_vk.mapMemory(s_info.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS))
		return VK_NULL_HANDLE;
	if (!deviceLocal)
		memcpy(mapped, data, (size_t)size);	// stays mapped; written before any command that reads it is submitted
	return buffer;
}

bool Make_Test_Mesh()
{
	// 36 vertices of a unit cube centred on the origin, and a 4x4 grid of instances in W3D camera space (x right, y up,
	// looking down -z) in front of the camera: depth-tested against the scene where they overlap it.
	static const float c[8][3] = {
		{ -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 }, { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } };
	static const int f[36] = {
		0, 1, 2, 0, 2, 3, 5, 4, 7, 5, 7, 6, 4, 0, 3, 4, 3, 7, 1, 5, 6, 1, 6, 2, 3, 2, 6, 3, 6, 7, 4, 5, 1, 4, 1, 0 };
	float cube[36 * 3];
	for (int i = 0; i < 36; ++i)
		memcpy(&cube[i * 3], c[f[i]], sizeof(c[0]));
	float inst[INSTANCES * 8];
	for (int i = 0; i < INSTANCES; ++i)
	{
		const int gx = i % 4, gy = i / 4;
		float *p = &inst[i * 8];
		p[0] = (gx - 1.5f) * 40.0f;
		p[1] = (gy - 1.5f) * 40.0f;
		p[2] = -250.0f;	// W3D camera space looks down -Z (measured 10/10: +Z placed the cubes behind the camera)
		p[3] = 12.0f;
		p[4] = (float)(gx + 1) / 4.0f;
		p[5] = (float)(gy + 1) / 4.0f;
		p[6] = 1.0f - (float)i / INSTANCES;
		p[7] = 1.0f;
	}
	s_cubeBuffer = Make_Static_Buffer(cube, sizeof(cube));
	s_instanceBuffer = Make_Static_Buffer(inst, sizeof(inst));
	if (s_cubeBuffer == VK_NULL_HANDLE || s_instanceBuffer == VK_NULL_HANDLE)
		return false;
	memcpy(s_instData, inst, sizeof(s_instData));
	memcpy(s_cubeData, cube, sizeof(s_cubeData));
	if ((s_uploadTest == 1 || s_uploadTest == 3) && (s_upBuffer = Make_Static_Buffer(nullptr,
		(VkDeviceSize)(s_uploadTest == 1 ? sizeof(inst) : sizeof(cube)) * PACKETS, true)) == VK_NULL_HANDLE)
		return false;
	if (s_uploadTest == 2 && (s_indBuffer = Make_Static_Buffer(nullptr, (VkDeviceSize)sizeof(VkDrawIndirectCommand) * PACKETS, true,
		VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT)) == VK_NULL_HANDLE)
		return false;

	VkShaderModuleCreateInfo si = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
	si.codeSize = sizeof(kGxHybTestVert);
	si.pCode = kGxHybTestVert;
	if (s_vk.createShaderModule(s_info.device, &si, nullptr, &s_vert) != VK_SUCCESS)
		return false;
	si.codeSize = sizeof(kGxHybTestFrag);
	si.pCode = kGxHybTestFrag;
	if (s_vk.createShaderModule(s_info.device, &si, nullptr, &s_frag) != VK_SUCCESS)
		return false;

	VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 80 };
	VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	li.pushConstantRangeCount = 1;
	li.pPushConstantRanges = &pr;
	if (s_vk.createPipelineLayout(s_info.device, &li, nullptr, &s_layout) != VK_SUCCESS)
		return false;
	if (s_equalTest)
	{
		float verts[EQUAL_VERTS * 3];
		for (int i = 0; i < INSTANCES; ++i)
			for (int v = 0; v < 36; ++v)
			{
				float *o = &verts[(i * 36 + v) * 3];
				for (int a = 0; a < 3; ++a)
					o[a] = cube[v * 3 + a] * inst[i * 8 + 3] + inst[i * 8 + a];
				ClassicVertex &cv = s_classicVerts[i * 36 + v];
				cv.x = o[0]; cv.y = o[1]; cv.z = o[2]; cv.diffuse = 0xFFFFFFFF;
			}
		// one instance at the origin with scale 1: inPos * 1.0 + 0.0 is exact, so both paths see identical positions
		const float one[8] = { 0, 0, 0, 1, 0.25f, 0.5f, 1.0f, 1.0f };
		VkShaderModuleCreateInfo wi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
		wi.codeSize = sizeof(kGxHybWhiteFrag);
		wi.pCode = kGxHybWhiteFrag;
		s_equalBuffer = Make_Static_Buffer(verts, sizeof(verts));
		s_equalInstance = Make_Static_Buffer(one, sizeof(one));
		if (s_equalBuffer == VK_NULL_HANDLE || s_equalInstance == VK_NULL_HANDLE
			|| s_vk.createShaderModule(s_info.device, &wi, nullptr, &s_whiteFrag) != VK_SUCCESS)
			return false;
	}
	if (!s_texTest)
		return true;

	si.codeSize = sizeof(kGxHybTexFrag);
	si.pCode = kGxHybTexFrag;
	VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
	dl.bindingCount = 1;
	dl.pBindings = &b;
	VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PACKETS };
	VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	pi.maxSets = PACKETS;
	pi.poolSizeCount = 1;
	pi.pPoolSizes = &ps;
	VkSamplerCreateInfo sc = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	sc.magFilter = sc.minFilter = VK_FILTER_NEAREST;
	sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sc.maxLod = VK_LOD_CLAMP_NONE;
	if (s_vk.createShaderModule(s_info.device, &si, nullptr, &s_texFrag) != VK_SUCCESS
		|| s_vk.createDescriptorSetLayout(s_info.device, &dl, nullptr, &s_texSetLayout) != VK_SUCCESS
		|| s_vk.createDescriptorPool(s_info.device, &pi, nullptr, &s_descPool) != VK_SUCCESS
		|| s_vk.createSampler(s_info.device, &sc, nullptr, &s_sampler) != VK_SUCCESS)
		return false;
	for (uint32_t i = 0; i < PACKETS; ++i)
	{
		VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		ai.descriptorPool = s_descPool;
		ai.descriptorSetCount = 1;
		ai.pSetLayouts = &s_texSetLayout;
		if (s_vk.allocateDescriptorSets(s_info.device, &ai, &s_texSets[i]) != VK_SUCCESS)
			return false;
	}
	li.setLayoutCount = 1;
	li.pSetLayouts = &s_texSetLayout;
	return s_vk.createPipelineLayout(s_info.device, &li, nullptr, &s_texLayout) == VK_SUCCESS;
}

VkImageAspectFlags Depth_Aspects(VkFormat format)
{
	switch (format)
	{
	case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
	case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
	case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	default: return 0;
	}
}

// The structural key of the test pipeline: attachment formats + sample count (plan §1 "a generic shader is not a generic
// pipeline"). Created on first use on the recording thread - logged, and excluded from timing windows by warm-up.
VkPipeline Pipeline_For(VkFormat colour, VkFormat depth, VkSampleCountFlagBits samples, int variant)
{
	const bool textured = (variant & VARIANT_TEXTURED) != 0;
	const uint64_t key = (uint64_t)colour | ((uint64_t)depth << 24) | ((uint64_t)samples << 48) | ((uint64_t)variant << 56);
	std::lock_guard<std::mutex> lock(s_pipelineLock);
	std::map<uint64_t, VkPipeline>::iterator it = s_pipelines.find(key);
	if (it != s_pipelines.end())
		return it->second;

	const auto t0 = std::chrono::steady_clock::now();
	const VkImageAspectFlags aspects = depth != VK_FORMAT_UNDEFINED ? Depth_Aspects(depth) : 0;

	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO }, { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO } };
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = s_vert;
	stages[0].pName = "main";
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = textured ? s_texFrag : (variant & VARIANT_EQUAL_WHITE) ? s_whiteFrag : s_frag;
	stages[1].pName = "main";

	VkVertexInputBindingDescription bindings[2] = {
		{ 0, 12, VK_VERTEX_INPUT_RATE_VERTEX }, { 1, 32, VK_VERTEX_INPUT_RATE_INSTANCE } };
	VkVertexInputAttributeDescription attributes[3] = {
		{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, { 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
		{ 2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16 } };
	VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
	vi.vertexBindingDescriptionCount = 2;
	vi.pVertexBindingDescriptions = bindings;
	vi.vertexAttributeDescriptionCount = 3;
	vi.pVertexAttributeDescriptions = attributes;
	VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
	vp.viewportCount = 1;
	vp.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.lineWidth = 1.0f;
	// depthbias mutant (criterion 3's fail arm): the base pass lands some depth units off, so EQUAL must fail everywhere
	rs.depthBiasEnable = (variant & VARIANT_DEPTHBIAS) ? VK_TRUE : VK_FALSE;
	rs.depthBiasConstantFactor = 64.0f;
	VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
	ms.rasterizationSamples = samples;
	VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
	ds.depthTestEnable = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_TRUE : VK_FALSE;
	ds.depthWriteEnable = (variant & VARIANT_DEPTHWRITE) ? VK_TRUE : VK_FALSE;	// otherwise never disturbs the classic depth
	ds.depthCompareOp = (variant & VARIANT_EQUAL_WHITE) ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_LESS_OR_EQUAL;
	VkPipelineColorBlendAttachmentState ba = {};
	ba.colorWriteMask = s_visible ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
		| VK_COLOR_COMPONENT_A_BIT) : 0;
	VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
	cb.attachmentCount = 1;
	cb.pAttachments = &ba;
	VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &colour;
	ri.depthAttachmentFormat = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? depth : VK_FORMAT_UNDEFINED;
	ri.stencilAttachmentFormat = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? depth : VK_FORMAT_UNDEFINED;

	VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
	pi.pNext = &ri;
	pi.stageCount = 2;
	pi.pStages = stages;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &cb;
	pi.pDynamicState = &dy;
	pi.layout = textured ? s_texLayout : s_layout;
	VkPipeline pipeline = VK_NULL_HANDLE;
	const VkResult r = s_vk.createGraphicsPipelines(s_info.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline);
	const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
	fprintf(stderr, "[GXHYB] pipeline colour=%d depth=%d samples=%d variant=%d -> %s in %llu us\n", (int)colour, (int)depth, (int)samples, variant,
		r == VK_SUCCESS ? "ok" : "FAILED", (unsigned long long)us);
	if (r != VK_SUCCESS)
		pipeline = VK_NULL_HANDLE;
	s_pipelines[key] = pipeline;
	++s_pipelinesMade;
	s_pipelineUs += us;
	return pipeline;
}

void Barrier(VkCommandBuffer cmd, const DxvkGxImage &img, VkImageLayout from, VkImageLayout to,
	VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
	VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
	b.srcStageMask = srcStage;
	b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage;
	b.dstAccessMask = dstAccess;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img.image;
	b.subresourceRange = img.range;
	VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	di.imageMemoryBarrierCount = 1;
	di.pImageMemoryBarriers = &b;
	s_vk.cmdPipelineBarrier2(cmd, &di);
}

const VkPipelineStageFlags2 DS_STAGES = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
const VkAccessFlags2 DS_ACCESS = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
const VkAccessFlags2 COLOUR_ACCESS = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;

// One insertion: a rendering instance on the bound attachments with LOAD/STORE at their real formats and sample count,
// one instanced draw, and every image back in the layout it came in, its writes available to what DXVK expects
// (step 4). Runs on DXVK's CS thread (P-L) or the device thread into our own buffer (P-Q).
void Record_Test(const DxvkGxRecordInfo *info, void *user)
{
	const uint64_t t0 = Now_Ns();
	const Packet *pk = (const Packet *)user;
	const DxvkGxImage &c = info->colour;
	const DxvkGxImage &d = info->depth;
	const bool hasDepth = d.image != VK_NULL_HANDLE;
	const bool textured = pk->textured != 0;
	const VkFormat df = hasDepth ? d.format : VK_FORMAT_UNDEFINED;
	const bool drawing = s_draw && !pk->noDraw && !(s_texTest && pk->indexInFrame >= 8)
		&& !(s_clearTest && pk->indexInFrame != 0);	// design-r3 #2: in CLEARTEST only the cleared insertion draws
	const VkPipeline pipeline = !drawing ? VK_NULL_HANDLE : Pipeline_For(c.format, df, c.samples,
		(textured ? VARIANT_TEXTURED : 0) | (s_equalTest ? VARIANT_DEPTHWRITE : 0)
		| (s_equalTest && s_mutant == MUTANT_DEPTHBIAS ? VARIANT_DEPTHBIAS : 0));
	const VkPipeline white = drawing && s_equalTest == 2 ? Pipeline_For(c.format, df, c.samples, VARIANT_EQUAL_WHITE) : VK_NULL_HANDLE;
	if (drawing && pipeline == VK_NULL_HANDLE)
		++s_noPipeline;	// the boundary (barriers + LOAD/STORE instance) is still recorded, without the draw
	const bool inPass = info->inPass != 0;
	if (s_inPassAsked)
	{
		inPass ? ++s_winInPass : ++s_winFallback;
		inPass ? ++s_inPassTotal : ++s_fallbackTotal;	// cumulative, for runs without the toggle ([GXHYB] f= rows)
	}
	if (drawing)
	{
		++s_winDrawWant;
		if (pipeline != VK_NULL_HANDLE && (s_equalTest != 2 || white != VK_NULL_HANDLE))
			++s_winDrawn;
	}

	// Incoming: DXVK's earlier commands in this command buffer (P-L) or queue (P-Q) are the first scope. The mutant keeps
	// the layout transition and drops the dependency - synchronization validation must call that a hazard (§8).
	const VkPipelineStageFlags2 inStage = s_mutant == MUTANT_NOBARRIER ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	const VkAccessFlags2 inAccess = s_mutant == MUTANT_NOBARRIER ? VK_ACCESS_2_NONE : VK_ACCESS_2_MEMORY_WRITE_BIT;
	// In-pass: the attachments are DXVK's bound ones, in attachment layout already; no barrier is allowed or needed.
	if (!inPass)
		Barrier(info->cmd, c, c.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, inStage, inAccess,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, COLOUR_ACCESS);
	if (hasDepth && !inPass)
		Barrier(info->cmd, d, d.layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, inStage, inAccess, DS_STAGES, DS_ACCESS);
	// GX_HYBRID_UPLOADTEST: this insertion's instance data, uploaded BEFORE any pass it is drawn in - into DXVK's init buffer
	// (submitted ahead of the whole list) in-pass, inline before our rendering instance otherwise - then its barrier.
	VkDeviceSize upOffset = 0;
	const bool indirect = s_uploadTest == 2 && !s_equalTest;
	if (s_uploadTest && pipeline != VK_NULL_HANDLE && !s_equalTest)
	{
		VkCommandBuffer up = inPass && s_mutant != MUTANT_INPASSUPLOAD ? info->initCmd : info->cmd;
		if (indirect)
		{
			const VkDrawIndirectCommand dc = { 36, INSTANCES, 0, 0 };
			upOffset = (VkDeviceSize)sizeof(dc) * (pk->retire % PACKETS);
			s_vk.cmdUpdateBuffer(up, s_indBuffer, upOffset, sizeof(dc), &dc);
		}
		else if (s_uploadTest == 3)
		{
			upOffset = (VkDeviceSize)sizeof(s_cubeData) * (pk->retire % PACKETS);
			s_vk.cmdUpdateBuffer(up, s_upBuffer, upOffset, sizeof(s_cubeData), s_cubeData);
		}
		else
		{
			upOffset = (VkDeviceSize)sizeof(s_instData) * (pk->retire % PACKETS);
			s_vk.cmdUpdateBuffer(up, s_upBuffer, upOffset, sizeof(s_instData), s_instData);
		}
		if (s_mutant != MUTANT_NOUPBARRIER)
		{
			VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			mb.dstStageMask = indirect ? VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT : VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT;
			mb.dstAccessMask = indirect ? VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT : VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT;
			VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			di.memoryBarrierCount = 1;
			di.pMemoryBarriers = &mb;
			s_vk.cmdPipelineBarrier2(up, &di);
		}
	}

	// A read texture: the fork prepared it in D3D order; our own acquire makes its last writes (an upload this list
	// recorded, at least) visible to our fragment reads, independent of DXVK's internal release barriers.
	VkDescriptorSet set = VK_NULL_HANDLE;
	if (textured && drawing)
	{
		VkDescriptorImageInfo ii = { s_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL };
		if (pk->rawView != VK_NULL_HANDLE)
		{
			ii.imageView = pk->rawView;	// MUTANT_NOPREP: no preparation, no barrier, no tracking
			ii.imageLayout = pk->rawLayout;
		}
		else if (info->readCount > 0)
		{
			ii.imageView = info->reads[0].view;
			ii.imageLayout = info->reads[0].layout;
			Barrier(info->cmd, info->reads[0], info->reads[0].layout, info->reads[0].layout, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
				VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
		}
		// earlyreuse: a 4-set window, so a set is rewritten while an earlier draw of this same command list still binds it
		set = s_texSets[pk->retire % (s_mutant == MUTANT_EARLYREUSE ? 4u : PACKETS)];
		VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		w.dstSet = set;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &ii;
		s_vk.updateDescriptorSets(s_info.device, 1, &w, 0, nullptr);
	}

	VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	ca.imageView = c.view;
	ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
	da.imageView = d.view;
	da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	const VkImageAspectFlags aspects = hasDepth ? d.range.aspectMask : 0;
	// stencilclear: the stencil attachment is CLEARED instead of loaded - the occlusion buckets' player marks and the shadow
	// volumes' counts are lost, so the masked =1 picture must then differ from =0 (stencil preservation's fail arm).
	VkRenderingAttachmentInfo sa = da;
	if (s_mutant == MUTANT_STENCILCLEAR)
		sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
	ri.renderArea.extent.width = c.extent.width;
	ri.renderArea.extent.height = c.extent.height;
	ri.layerCount = 1;
	ri.colorAttachmentCount = 1;
	ri.pColorAttachments = &ca;
	ri.pDepthAttachment = (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? &da : nullptr;
	ri.pStencilAttachment = (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? &sa : nullptr;
	if (!inPass)
		s_vk.cmdBeginRendering(info->cmd, &ri);

	// The boundary alone (DRAW=0, a no-draw call, CLEARTEST's other insertions, a missing pipeline) records nothing inside the
	// instance: no viewport, pipeline, buffers or push constants (design-r3 #4).
	if (pipeline != VK_NULL_HANDLE)
	{
		// D3D's viewport exactly as DXVK maps it (D3D9DeviceEx::BindViewportAndScissor, d3d9_device.cpp:6826-6860 at 954c5c692): y flipped
		// through a negative height, the +0.5 half-texel correction, and MaxZ raised to MinZ + 0.001 below MinZ 0.5. CLASSIC
		// identity of an L2 draw needs this exact mapping (a WS-G0 contract item).
		const D3DVIEWPORT8 &v = pk->viewport;
		const float minZ = v.MinZ < 0.0f ? 0.0f : v.MinZ > 1.0f ? 1.0f : v.MinZ;
		float maxZ = v.MaxZ > v.MinZ + (v.MinZ >= 0.5f ? 0.0f : 0.001f) ? v.MaxZ : v.MinZ + (v.MinZ >= 0.5f ? 0.0f : 0.001f);
		maxZ = maxZ < 0.0f ? 0.0f : maxZ > 1.0f ? 1.0f : maxZ;
		VkViewport vv = { (float)v.X + 0.5f, (float)(v.Y + v.Height) + 0.5f, (float)v.Width, -(float)v.Height, minZ, maxZ };
		VkRect2D sc = { { (int32_t)v.X, (int32_t)v.Y }, { v.Width, v.Height } };
		s_vk.cmdSetViewport(info->cmd, 0, 1, &vv);
		s_vk.cmdSetScissor(info->cmd, 0, 1, &sc);
		if (pipeline != VK_NULL_HANDLE)
		{
			s_vk.cmdBindPipeline(info->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			if (set != VK_NULL_HANDLE)
				s_vk.cmdBindDescriptorSets(info->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_texLayout, 0, 1, &set, 0, nullptr);
			const bool upVerts = s_uploadTest == 3 && !s_equalTest, upInst = s_uploadTest == 1 && !s_equalTest;
			VkBuffer buffers[2] = { s_equalTest ? s_equalBuffer : upVerts ? s_upBuffer : s_cubeBuffer,
				s_equalTest ? s_equalInstance : upInst ? s_upBuffer : s_instanceBuffer };
			VkDeviceSize offsets[2] = { upVerts ? upOffset : 0, upInst ? upOffset : 0 };
			s_vk.cmdBindVertexBuffers(info->cmd, 0, 2, buffers, offsets);
			s_vk.cmdPushConstants(info->cmd, textured ? s_texLayout : s_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, pk->proj);
			s_vk.cmdPushConstants(info->cmd, textured ? s_texLayout : s_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 16, pk->offset);
		}
		if (s_equalTest)
		{
			s_vk.cmdDraw(info->cmd, EQUAL_VERTS, 1, 0, 0);
			if (white != VK_NULL_HANDLE)
			{
				s_vk.cmdBindPipeline(info->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, white);
				s_vk.cmdPushConstants(info->cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, pk->proj);
				s_vk.cmdPushConstants(info->cmd, s_layout, VK_SHADER_STAGE_VERTEX_BIT, 64, 16, pk->offset);
				s_vk.cmdDraw(info->cmd, EQUAL_VERTS, 1, 0, 0);
			}
		}
		else if (indirect)
			s_vk.cmdDrawIndirect(info->cmd, s_indBuffer, upOffset, 1, sizeof(VkDrawIndirectCommand));
		else
			s_vk.cmdDraw(info->cmd, 36, INSTANCES, 0, 0);
	}
	if (!inPass)
		s_vk.cmdEndRendering(info->cmd);

	// Outgoing: our attachment writes (the STORE ops at least) made available to DXVK's expected stages/accesses. The
	// noout mutant drops it - DXVK's next access then races our STORE (and a back buffer stays in the wrong layout): the
	// planted hazard synchronization validation must report (DESIGN §8).
	if (s_mutant != MUTANT_NOOUT && !inPass)
		Barrier(info->cmd, c, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, c.layout,
		VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, c.stages, c.access);
	if (hasDepth && s_mutant != MUTANT_NOOUT && !inPass)
		Barrier(info->cmd, d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, d.layout,
			DS_STAGES, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, d.stages, d.access);
	++s_recorded;
	++s_winRecorded;
	if (Mode() == 1)
		s_recNs += Now_Ns() - t0;	// P-Q's recording is already inside its device-thread time
}

// Device thread, in stream order: what the classic draw at this point would use.
void Capture_State(IDirect3DDevice8 *dev, Packet *pk)
{
	D3DMATRIX proj;
	dev->GetTransform(D3DTS_PROJECTION, &proj);
	memcpy(pk->proj, &proj, sizeof(pk->proj));
	dev->GetViewport(&pk->viewport);
}

// Every option a producer-side decision reads, parsed on the MAIN thread before Device_Init is queued.
static const char *const s_mutantNames[] = { "none", "nobarrier", "noprep", "noout", "earlyreuse", "depthbias", "nofeature",
	"stencilclear", "noupbarrier", "inpassupload" };
void Parse_Options()
{
	s_visible = getenv("GX_RENDER_HYBRID_VISIBLE") != nullptr && atoi(getenv("GX_RENDER_HYBRID_VISIBLE")) != 0;
	const char *mut = GxTestEnv("GX_HYBRID_MUTANT");
	s_mutant = MUTANT_NONE;
	for (int i = 1; mut != nullptr && i < (int)(sizeof(s_mutantNames) / sizeof(s_mutantNames[0])); ++i)
		if (strcmp(mut, s_mutantNames[i]) == 0)
			s_mutant = i;
	const char *draw = getenv("GX_RENDER_HYBRID_DRAW");
	s_draw = draw == nullptr || atoi(draw) != 0;
	s_texTest = Mode() == 1 && GxTestEnv("GX_HYBRID_TEXTEST") != nullptr && atoi(GxTestEnv("GX_HYBRID_TEXTEST")) != 0;
	s_equalTest = Mode() == 1 && s_visible && GxTestEnv("GX_HYBRID_EQUALTEST") != nullptr ? atoi(GxTestEnv("GX_HYBRID_EQUALTEST")) : 0;
	s_clearTest = Mode() == 1 && s_visible && GxTestEnv("GX_HYBRID_CLEARTEST") != nullptr && atoi(GxTestEnv("GX_HYBRID_CLEARTEST")) != 0;
	s_inPassAsked = Mode() == 1 && getenv("DXVK_GX_INPASS") != nullptr && strcmp(getenv("DXVK_GX_INPASS"), "1") == 0;
	s_split = getenv("GX_RENDER_HYBRID_SPLIT") != nullptr ? atoi(getenv("GX_RENDER_HYBRID_SPLIT")) : 1;
	s_split = s_split < 1 ? 1 : s_split > 1024 ? 1024 : s_split;
	s_siteMask = getenv("GX_RENDER_HYBRID_SITES") != nullptr ? (unsigned)strtoul(getenv("GX_RENDER_HYBRID_SITES"), nullptr, 0) : ~0u;
	s_uploadTest = Mode() == 1 && GxTestEnv("GX_HYBRID_UPLOADTEST") != nullptr ? atoi(GxTestEnv("GX_HYBRID_UPLOADTEST")) : 0;
	if (s_uploadTest < 1 || s_uploadTest > 3)
		s_uploadTest = 0;
	s_interleave = Mode() == 1 && GxTestEnv("GX_RENDER_HYBRID_INTERLEAVE") != nullptr && atoi(GxTestEnv("GX_RENDER_HYBRID_INTERLEAVE")) != 0;
	s_threads = getenv("GX_RENDER_HYBRID_THREADS") != nullptr && atoi(getenv("GX_RENDER_HYBRID_THREADS")) != 0;
	if (s_equalTest != 1 && s_equalTest != 2)
		s_equalTest = 0;
	if (s_equalTest)
		s_texTest = 0;	// one test per run

}

void Device_Init(IDirect3DDevice8 *dev, void *)
{
	const char *why = nullptr;
	memset(&s_info, 0, sizeof(s_info));
	s_dev8 = dev;
	if (FAILED(dev->QueryInterface(GX_IID_IDxvkGxExternal, (void **)&s_ext)) || s_ext == nullptr)
		why = "the d3d8 device does not answer IDxvkGxExternal (not the patched DXVK fork)";
	else if (FAILED(s_ext->GetDeviceInfo(&s_info)) || s_info.getInstanceProcAddr == nullptr)
		why = "GetDeviceInfo failed";
	else if (!s_info.dynamicRendering || !s_info.synchronization2 || s_mutant == MUTANT_NOFEATURE)
		why = "DXVK's device lacks dynamicRendering or synchronization2";
	else if (!Load_Functions())
		why = "a Vulkan entry point is missing";
	else if (!Make_Test_Mesh())
		why = "test mesh/shader/layout creation failed";
	else if (s_texTest && (FAILED(dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_tex8)) || s_tex8 == nullptr
		|| FAILED(s_tex8->QueryInterface(GX_IID_ID3D9VkInteropTexture, (void **)&s_texInterop)) || s_texInterop == nullptr))
		why = "GX_HYBRID_TEXTEST: MANAGED texture creation / interop QI failed";
	else if (s_clearTest && s_inPassAsked)
		why = "GX_HYBRID_CLEARTEST tests the break path's deferred clear: its zero-read preamble would stay inside DXVK's pass under"
			" DXVK_GX_INPASS=1 and the clear would run immediately (Codex rw1x-code-r8) - run it without DXVK_GX_INPASS";
	else if (Mode() == 2)
	{
		uint32_t index = 0, family = 0;
		if (FAILED(dev->QueryInterface(GX_IID_ID3D9VkInteropDevice, (void **)&s_qdev)) || s_qdev == nullptr)
			why = "the d3d8 device does not answer ID3D9VkInteropDevice";
		else
		{
			s_qdev->GetSubmissionQueue(&s_queue, &index, &family);
			VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			pi.queueFamilyIndex = family;
			if (s_vk.createCommandPool(s_info.device, &pi, nullptr, &s_pool) != VK_SUCCESS)
				why = "command pool creation failed";
			for (int i = 0; i < QSLOTS && why == nullptr; ++i)
			{
				VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
				ai.commandPool = s_pool;
				ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
				ai.commandBufferCount = 1;
				VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
				if (s_vk.allocateCommandBuffers(s_info.device, &ai, &s_qslots[i].cmd) != VK_SUCCESS
					|| s_vk.createFence(s_info.device, &fi, nullptr, &s_qslots[i].fence) != VK_SUCCESS)
					why = "command buffer/fence creation failed";
			}
		}
	}

	if (why != nullptr)
	{
		fprintf(stderr, "[GXHYB] unavailable: %s - behaving as GX_RENDER_HYBRID=0\n", why);
		s_state = STATE_FAILED;
		return;
	}
	// the fork's own switches, read from the same environment the fork read at device creation (design-r9 #1)
	const bool forkEpoch = getenv("DXVK_GX_NEW_EPOCH") != nullptr && strcmp(getenv("DXVK_GX_NEW_EPOCH"), "1") == 0;
	const bool forkSuspend = getenv("DXVK_GX_SUSPEND") != nullptr && strcmp(getenv("DXVK_GX_SUSPEND"), "1") == 0;
	fprintf(stderr, "[GXHYB] init mode=%d (%s) visible=%d mutant=%s draw=%d textest=%d equaltest=%d inpass=%d uploadtest=%d"
		" split=%d sites=0x%x interleave=%d threads=%d fork_new_epoch=%d fork_suspend=%d api=%u.%u queue_family=%u timeline=%u\n", Mode(),
		Mode() == 1 ? "P-L record inside DXVK's command list" : "P-Q stock interop: flush + own submit", s_visible,
		s_mutantNames[s_mutant], (int)s_draw, s_texTest, s_equalTest, (int)s_inPassAsked, s_uploadTest, s_split,
		s_siteMask & 0xFFu, (int)s_interleave, (int)s_threads, (int)forkEpoch, (int)forkSuspend,
		VK_API_VERSION_MAJOR(s_info.apiVersion), VK_API_VERSION_MINOR(s_info.apiVersion), s_info.queueFamily,
		(unsigned)s_info.timelineSemaphore);
	s_state = STATE_READY;
}

// GX_HYBRID_EQUALTEST=1: the classic second pass - DXVK's fixed-function vertex path over the same view-space triangles
// (world = view = identity, the same projection), ZFUNC EQUAL, no depth write, white. The device state is saved and
// restored around it with a state block, so DX8Wrapper's cached state stays true.
void Classic_Equal_Pass(IDirect3DDevice8 *dev, const Packet *pk)
{
	DWORD block = 0;
	if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &block)))	// GX-WRAPSKIP-OK: equal-pass bracket: D3DSBT_ALL block, applied at the end
		return;
	D3DMATRIX id;
	memset(&id, 0, sizeof(id));
	id._11 = id._22 = id._33 = id._44 = 1.0f;
	dev->SetTransform(D3DTS_WORLD, &id);	// GX-WRAPSKIP-OK: equal-pass bracket: D3DSBT_ALL block, applied at the end
	dev->SetTransform(D3DTS_VIEW, &id);	// GX-WRAPSKIP-OK: equal-pass bracket: D3DSBT_ALL block, applied at the end
	dev->SetTransform(D3DTS_PROJECTION, (const D3DMATRIX *)pk->proj);
	dev->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	dev->SetPixelShader(0);
	dev->SetTexture(0, nullptr);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	const DWORD rs[][2] = {
		{ D3DRS_ZENABLE, TRUE }, { D3DRS_ZFUNC, D3DCMP_EQUAL }, { D3DRS_ZWRITEENABLE, FALSE }, { D3DRS_LIGHTING, FALSE },
		{ D3DRS_CULLMODE, D3DCULL_NONE }, { D3DRS_ALPHABLENDENABLE, FALSE }, { D3DRS_ALPHATESTENABLE, FALSE },
		{ D3DRS_STENCILENABLE, FALSE }, { D3DRS_FOGENABLE, FALSE }, { D3DRS_COLORWRITEENABLE, 0xF }, { D3DRS_FILLMODE, D3DFILL_SOLID } };
	for (size_t i = 0; i < sizeof(rs) / sizeof(rs[0]); ++i)
		dev->SetRenderState((D3DRENDERSTATETYPE)rs[i][0], rs[i][1]);
	dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, EQUAL_VERTS / 3, s_classicVerts, sizeof(ClassicVertex));
	++s_classicEqualDraws;	// -> DX8Wrapper::Record_Direct_Draw() on the main thread (Frame_Stats): Census is main-thread only
	dev->ApplyStateBlock(block);	// GX-WRAPSKIP-OK: equal-pass bracket: D3DSBT_ALL block, applied at the end
	dev->DeleteStateBlock(block);
}

// GX_RENDER_HYBRID_INTERLEAVE: one triangle from the vertex buffer and base vertex the last classic draw bound, with all of its
// state - DrawPrimitive changes no device state, so DX8Wrapper's cache stays true.
void Classic_Redraw(IDirect3DDevice8 *dev, bool on)
{
	IDirect3DVertexBuffer8 *vb = nullptr;
	IDirect3DIndexBuffer8 *ib = nullptr;
	UINT stride = 0, base = 0;
	D3DVERTEXBUFFER_DESC vd;
	bool drew = false;
	if (SUCCEEDED(dev->GetStreamSource(0, &vb, &stride)) && vb != nullptr && stride != 0 && SUCCEEDED(vb->GetDesc(&vd)))
	{
		if (FAILED(dev->GetIndices(&ib, &base)))
			base = 0;
		if (vd.Size / stride >= base + 3)
			drew = SUCCEEDED(dev->DrawPrimitive(D3DPT_TRIANGLELIST, base, 1));
	}
	if (vb != nullptr)
		vb->Release();
	if (ib != nullptr)
		ib->Release();
	if (!drew)
		++s_winRedrawSkip;
	else
	{
		++(on ? s_winRedrawOn : s_winRedrawOff);
		++s_classicEqualDraws;
	}
}

void Device_Redraw_Off(IDirect3DDevice8 *dev, void *)
{
	Classic_Redraw(dev, false);
}

// P-L: the device thread hands the packet to the fork; the recorder runs later on DXVK's CS thread.
void Device_Insert_PL(IDirect3DDevice8 *dev, void *ctx)
{
	if (s_state != STATE_READY)
	{
		((Packet *)ctx)->done.store(1, std::memory_order_release);
		return;
	}
	const uint64_t t0 = Now_Ns();
	Packet *pk = (Packet *)ctx;
	Capture_State(dev, pk);
	if (s_clearTest && pk->indexInFrame == 0)
	{
		// A Clear issued while DXVK's pass is bound is an immediate vkCmdClearAttachments (code-r2 #1). So: end the pass with a
		// boundary-only recording (no retire signal), clear with no pass bound - DXVK defers that clear into its next pass -
		// and only then record the cubes, with no classic draw in between.
		// The boundary-only packet is written ONCE and never again: DXVK's CS thread may still be running an earlier frame's
		// recorder on it (code-r3 #2). A no-draw recording reads none of the per-frame fields (projection, viewport, offsets).
		static Packet s_prep;
		static bool s_prepMade = false;
		if (!s_prepMade)
		{
			s_prep.done.store(1);
			s_prep.noDraw = 1;
			s_prepMade = true;
		}
		s_ext->RecordExternal(0, nullptr, &Record_Test, &s_prep, 0);
		dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF0000FF, 1.0f, 0);
	}
	UINT reads = 0;
	if (s_texTest)
	{
		// Every frame, at its 5th insertion (in stream order), the texture is refilled with the frame's colour: the cubes of
		// insertions 0-2 (drawn earlier in this command list) must still show the PREVIOUS frame's colour, 4-7 the new one,
		// and 3 its untextured vertex colours.
		// A DXVK that hoisted the upload into its init buffer (a new tracking epoch per insertion,
		// fork fail arm DXVK_GX_NEW_EPOCH=1) shows the new colour on all eight.
		static const uint32_t colours[6] = { 0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFF00, 0xFF00FFFF, 0xFFFF00FF };
		if (s_texFills == 0 || pk->indexInFrame == 4)
		{
			D3DLOCKED_RECT lr;
			const uint32_t c = colours[pk->frameSeq % 6];
			if (SUCCEEDED(s_tex8->LockRect(0, &lr, nullptr, 0)))
			{
				for (int y = 0; y < 4; ++y)
					for (int x = 0; x < 4; ++x)
						((uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch))[x] = c;
				s_tex8->UnlockRect(0);
				if (s_texFills < 20 || s_texFills % 600 == 0)
					fprintf(stderr, "[GXHYBTEX] fill n=%u frame_seq=%llu argb=%08X\n", s_texFills,
						(unsigned long long)pk->frameSeq, c);
			}
			++s_texFills;
		}
		// Insertion 3 reads nothing: it is the unrelated external work between the last read of the texture (2) and its refill
		// (4) under which the stock new-epoch insertion (DXVK_GX_NEW_EPOCH=1) leaves the texture untracked in the epoch the
		// upload sees - so only there can DXVK move the upload into its init buffer, ahead of insertions 0-2 (design-r2 #2).
		pk->textured = pk->indexInFrame == 3 ? 0 : 1;
		if (!pk->textured)
			;
		else if (s_mutant == MUTANT_NOPREP)
		{
			VkImage image = VK_NULL_HANDLE;
			VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			if (SUCCEEDED(s_texInterop->GetVulkanImageInfo(&image, &pk->rawLayout, &ci)) && image != VK_NULL_HANDLE)
			{
				std::map<VkImage, VkImageView>::iterator it = s_rawViews.find(image);
				if (it == s_rawViews.end())
				{
					VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
					vi.image = image;
					vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
					vi.format = ci.format;
					vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
					vi.subresourceRange.levelCount = 1;
					vi.subresourceRange.layerCount = 1;
					VkImageView view = VK_NULL_HANDLE;
					s_vk.createImageView(s_info.device, &vi, nullptr, &view);
					it = s_rawViews.insert(std::make_pair(image, view)).first;
				}
				pk->rawView = it->second;
			}
		}
		else
			reads = 1;
	}
	if (FAILED(s_ext->RecordExternal(reads, reads ? &s_texInterop : nullptr, &Record_Test, pk, pk->retire)))
	{
		++s_refused;	// e.g. the water reflection's 1x target over the MSAA depth (DESIGN §1.4)
		++s_winRefused;
		pk->done.store(1, std::memory_order_release);	// never in a command list: its slot is free now (code-r1 #1)
	}
	else
	{
		++s_winAccepted;
		s_lastAccepted.store(pk->retire);
		if (s_equalTest == 1)
			Classic_Equal_Pass(dev, pk);
	}
	if (s_interleave)
		Classic_Redraw(dev, true);	// refused or not: the OFF periods draw it at the same point
	s_devNs += Now_Ns() - t0;
}

// P-Q: a view per submission, never cached by raw VkImage handle - DXVK may relocate an image at endRecording and
// Vulkan may then hand the old handle to a new image (code-r2 #2, code-r3 #1). Retire_Slot destroys it after the fence.
VkImageView QView(VkImage image, VkFormat format, VkImageAspectFlags aspects)
{
	VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	vi.image = image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = format;
	vi.subresourceRange.aspectMask = aspects;
	vi.subresourceRange.levelCount = 1;
	vi.subresourceRange.layerCount = 1;
	VkImageView view = VK_NULL_HANDLE;
	if (s_vk.createImageView(s_info.device, &vi, nullptr, &view) != VK_SUCCESS)
		view = VK_NULL_HANDLE;
	return view;
}

bool Describe_Q(GxIDxvkInteropTexture *tex, bool depth, DxvkGxImage &out, VkImageView &slotView)
{
	VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImage image = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (FAILED(tex->GetVulkanImageInfo(&image, &layout, &ci)) || image == VK_NULL_HANDLE)
		return false;
	memset(&out, 0, sizeof(out));
	out.image = image;
	out.format = ci.format;
	out.samples = ci.samples;
	out.extent = ci.extent;
	out.range.aspectMask = depth ? Depth_Aspects(ci.format) : VK_IMAGE_ASPECT_COLOR_BIT;
	out.range.levelCount = 1;
	out.range.layerCount = 1;
	out.layout = layout;	// right only after the TransitionTextureLayout(L, L) below normalised the attachments (D2/D3)
	out.stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;	// the stock API does not say; be conservative
	out.access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	out.view = slotView = QView(image, ci.format, out.range.aspectMask);
	return out.view != VK_NULL_HANDLE;
}

void Device_Insert_PQ_Body(IDirect3DDevice8 *dev, void *ctx);

// P-Q: the stock interop protocol of DESIGN §1.3, on the device thread. The packet is done when it returns (code-r1 #5).
void Device_Insert_PQ(IDirect3DDevice8 *dev, void *ctx)
{
	Device_Insert_PQ_Body(dev, ctx);
	((Packet *)ctx)->done.store(1, std::memory_order_release);
}

// P-Q: everything one submission holds - the surfaces, their interop views and the image views made for it - released
// once its fence signalled, or at once when it never reached the queue (refused, failed submit).
void Retire_Slot(QSlot &slot)
{
	if (slot.busy)
	{
		if (s_vk.waitForFences(s_info.device, 1, &slot.fence, VK_TRUE, 0) != VK_SUCCESS)
		{
			++s_fenceWaits;
			s_vk.waitForFences(s_info.device, 1, &slot.fence, VK_TRUE, ~0ull);
		}
		s_vk.resetFences(s_info.device, 1, &slot.fence);
		slot.busy = false;
	}
	for (int i = 0; i < 4; ++i)
		if (slot.hold[i] != nullptr)
			slot.hold[i]->Release();
	memset(slot.hold, 0, sizeof(slot.hold));
	for (int i = 0; i < 2; ++i)
		if (slot.views[i] != VK_NULL_HANDLE)
			s_vk.destroyImageView(s_info.device, slot.views[i], nullptr);
	memset(slot.views, 0, sizeof(slot.views));
}

// P-Q: drop everything the submissions still hold before a Reset, which DXVK refuses while default-pool surfaces are
// alive (code-r1 #2).
void Pre_Reset()
{
	for (int i = 0; i < QSLOTS; ++i)
		Retire_Slot(s_qslots[i]);
}

void Device_Insert_PQ_Body(IDirect3DDevice8 *dev, void *ctx)
{
	if (s_state != STATE_READY)
		return;
	struct Timer { uint64_t t0; ~Timer() { s_devNs += Now_Ns() - t0; } } timer = { Now_Ns() };
	Packet *pk = (Packet *)ctx;
	Capture_State(dev, pk);

	QSlot &slot = s_qslots[s_qnext++ % QSLOTS];
	Retire_Slot(slot);

	IDirect3DSurface8 *rt = nullptr, *ds = nullptr;
	GxIDxvkInteropTexture *trt = nullptr, *tds = nullptr;
	dev->GetRenderTarget(&rt);
	dev->GetDepthStencilSurface(&ds);
	slot.hold[0] = rt;
	slot.hold[1] = ds;
	if (rt == nullptr || FAILED(rt->QueryInterface(GX_IID_ID3D9VkInteropTexture, (void **)&trt)) || trt == nullptr)
	{
		++s_refused;
		++s_winRefused;
		Retire_Slot(slot);
		return;
	}
	slot.hold[2] = trt;
	if (ds != nullptr && SUCCEEDED(ds->QueryInterface(GX_IID_ID3D9VkInteropTexture, (void **)&tds)))
		slot.hold[3] = tds;

	DxvkGxRecordInfo info;
	memset(&info, 0, sizeof(info));
	VkImageLayout l = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	trt->GetVulkanImageInfo(nullptr, &l, &ci);
	VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	s_qdev->TransitionTextureLayout(trt, &r, l, l);	// = spillRenderPass(false): attachments back in their default layouts
	s_qdev->FlushRenderingCommands();				// drains DXVK's CS thread and submits its list
	// re-query AFTER the flush: endRecording may have relocated either image (D7)
	const bool ok = Describe_Q(trt, false, info.colour, slot.views[0]) && (tds == nullptr || Describe_Q(tds, true, info.depth, slot.views[1]));
	if (!ok || (tds != nullptr && info.depth.samples != info.colour.samples))
	{
		++s_refused;
		++s_winRefused;
		Retire_Slot(slot);	// never submitted: its views and surfaces go now
		return;
	}

	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	s_vk.resetCommandBuffer(slot.cmd, 0);
	s_vk.beginCommandBuffer(slot.cmd, &bi);
	info.cmd = slot.cmd;
	Record_Test(&info, pk);
	s_vk.endCommandBuffer(slot.cmd);

	VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
	cbi.commandBuffer = slot.cmd;
	VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &cbi;
	s_qdev->LockSubmissionQueue();	// no D3D call until the release (D9)
	const VkResult res = s_vk.queueSubmit2(s_queue, 1, &si, slot.fence);
	if (res == VK_SUCCESS)
		++s_winAccepted;
	else
		++s_winSubmitFail;
	s_qdev->ReleaseSubmissionQueue();
	if (res != VK_SUCCESS)
		fprintf(stderr, "[GXHYB] vkQueueSubmit2 failed %d\n", (int)res);
	slot.busy = res == VK_SUCCESS;
	if (!slot.busy)
		Retire_Slot(slot);
}

double Sd(const Moments &m)
{
	if (m.n < 2)
		return 0.0;
	const double v = (m.sumsq - m.sum * m.sum / m.n) / (m.n - 1);
	return v > 0 ? std::sqrt(v) : 0.0;
}

// The frame period that just ended belongs to frame 'prev'; called at the first insertion point of the next frame.
void Thread_Print();	// below (GX_RENDER_HYBRID_THREADS)

void Toggle_Account(unsigned prev, unsigned frame, double periodMs)
{
	const uint64_t period = prev / (unsigned)s_toggleK;
	if ((int)(prev % (unsigned)s_toggleK) >= s_toggleDrop)
	{
		s_period.add(periodMs);
		((period & 1) ? s_on : s_off).add(periodMs);
	}
	else
		++s_dropped;
	if (frame / (unsigned)s_toggleK == period)
		return;
	// period closed: pair each ON period with the OFF period just before it
	if (s_period.n > 0)
	{
		const double mean = s_period.sum / s_period.n;
		if ((period & 1) == 0)
		{
			s_lastOffMean = mean;
			s_lastOffPeriod = period;
		}
		else if (s_lastOffPeriod == period - 1)
			s_diff.add(mean - s_lastOffMean);
	}
	s_period = Moments();
	// A row in two steps (design-r4 #2, r5 #1). Its STATISTICS close on an ON period after >= 20 periods, so every ON period
	// in it pairs with an OFF period inside the same row's logic and render-frame range. It is PUBLISHED when the following
	// OFF period closes: by then that window's ON work - queued on this thread, accepted on the ring/device thread, recorded
	// on DXVK's CS thread - has drained, and an OFF period does none of its own, so the counters read then are the row's alone.
	if (s_rowPending && (period & 1) == 0)
	{
		s_rowPending = false;
		char redraws[96] = "";
		if (s_interleave)
			snprintf(redraws, sizeof(redraws), " redraws_on=%llu redraws_off=%llu redraw_skips=%llu",
				(unsigned long long)s_winRedrawOn.exchange(0), (unsigned long long)s_winRedrawOff.exchange(0),
				(unsigned long long)s_winRedrawSkip.exchange(0));
		fprintf(stderr, "%s dev_us_per_on_frame=%.2f rec_us_per_on_frame=%.2f requested=%llu attempts=%llu accepted=%llu recorded=%llu"
			" refused=%llu draw=%d draws_wanted=%llu draws_done=%llu submit_fail=%llu inpass=%llu fallback=%llu%s\n", s_rowHead,
			(double)s_devNs.exchange(0) / 1000.0 / s_rowOnFrames, (double)s_recNs.exchange(0) / 1000.0 / s_rowOnFrames,
			(unsigned long long)s_rowRequested, (unsigned long long)s_rowAttempts,
			(unsigned long long)s_winAccepted.exchange(0), (unsigned long long)s_winRecorded.exchange(0),
			(unsigned long long)s_winRefused.exchange(0), (int)s_draw, (unsigned long long)s_winDrawWant.exchange(0),
			(unsigned long long)s_winDrawn.exchange(0), (unsigned long long)s_winSubmitFail.exchange(0),
			(unsigned long long)s_winInPass.exchange(0), (unsigned long long)s_winFallback.exchange(0), redraws);
		if (s_threads)
			Thread_Print();
		fflush(stderr);
	}
	if (++s_periods < 20 || (period & 1) == 0)
		return;
	s_periods = 0;
	const unsigned logic = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0;
	s_rowOnFrames = s_onFrames > 0 ? (double)s_onFrames : 1.0;
	snprintf(s_rowHead, sizeof(s_rowHead), "[GXHYBT] k=%d drop=%d null=%d logic=%u..%u frames=%u..%u off_n=%.0f off_ms=%.4f off_sd=%.4f"
		" on_n=%.0f on_ms=%.4f on_sd=%.4f pairs=%.0f diff_ms=%.4f diff_se=%.4f dropped=%llu ins_per_on_frame=%.2f",
		s_toggleK, s_toggleDrop, (int)s_toggleNull, s_winLogic0, logic, s_winFrame0, prev, s_off.n,
		s_off.n > 0 ? s_off.sum / s_off.n : 0.0, Sd(s_off), s_on.n, s_on.n > 0 ? s_on.sum / s_on.n : 0.0, Sd(s_on), s_diff.n,
		s_diff.n > 0 ? s_diff.sum / s_diff.n : 0.0, s_diff.n > 1 ? Sd(s_diff) / std::sqrt(s_diff.n) : 0.0,
		(unsigned long long)s_dropped, (double)s_onIns / s_rowOnFrames);
	s_rowRequested = s_onRequested;
	s_rowAttempts = s_onIns;
	s_rowPending = true;
	s_on = s_off = s_diff = Moments();
	s_dropped = s_onFrames = s_onIns = s_onRequested = 0;
	s_winLogic0 = logic;
	s_winFrame0 = frame;
}

unsigned long long Sum_Census(const uint64_t (&a)[SITE_COUNT][2], int boundary)
{
	unsigned long long t = 0;
	for (int i = 0; i < SITE_COUNT; ++i)
		t += a[i][boundary];
	return t;
}

// GX_RENDER_HYBRID_THREADS: every thread's CPU ns (user + system) by Mach thread id; names taken the first time a thread is seen.
void Thread_Times(std::map<uint64_t, uint64_t> &out)
{
#if defined(__APPLE__)
	thread_act_array_t list = nullptr;
	mach_msg_type_number_t n = 0;
	if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
		return;
	for (mach_msg_type_number_t i = 0; i < n; ++i)
	{
		thread_basic_info_data_t bi;
		mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
		thread_identifier_info_data_t ii;
		mach_msg_type_number_t ci = THREAD_IDENTIFIER_INFO_COUNT;
		if (thread_info(list[i], THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS
			&& thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &ci) == KERN_SUCCESS)
		{
			out[ii.thread_id] = ((uint64_t)bi.user_time.seconds + bi.system_time.seconds) * 1000000000ull
				+ ((uint64_t)bi.user_time.microseconds + bi.system_time.microseconds) * 1000ull;
			if (s_thrAcc.find(ii.thread_id) == s_thrAcc.end())
			{
				ThreadAcc a = {};
				pthread_t pt = pthread_from_mach_thread_np(list[i]);
				if (pt == nullptr || pthread_getname_np(pt, a.name, sizeof(a.name)) != 0 || a.name[0] == 0)
					snprintf(a.name, sizeof(a.name), "unnamed");
				s_thrAcc[ii.thread_id] = a;
			}
		}
		mach_port_deallocate(mach_task_self(), list[i]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
#else
	(void)out;
#endif
}

// Frame_Stats, every new render frame, BEFORE the toggle account (so a row published now includes the span closed now): a span
// closes when its period ends and opens at frame `drop` of the next period.
void Thread_Account(unsigned frame)
{
	const unsigned period = frame / (unsigned)s_toggleK;
	if (s_thrSpan && period != s_thrPeriod)
	{
		std::map<uint64_t, uint64_t> now;
		Thread_Times(now);
		const bool on = (s_thrPeriod & 1) != 0;	// the NOMINAL state: a null arm's "ON" periods stay its control
		for (std::map<uint64_t, uint64_t>::const_iterator t = now.begin(); t != now.end(); ++t)
		{
			std::map<uint64_t, uint64_t>::const_iterator st = s_thrStart.find(t->first);
			if (st == s_thrStart.end() || t->second < st->second)
				continue;	// born during the span: no baseline
			ThreadAcc &a = s_thrAcc[t->first];
			(on ? a.onNs : a.offNs) += t->second - st->second;
		}
		(on ? s_thrOnFrames : s_thrOffFrames) += (uint64_t)(s_thrPeriod + 1) * (unsigned)s_toggleK - s_thrFrame0;
		s_thrSpan = false;
	}
	if (!s_thrSpan && period != s_thrPeriod && (int)(frame % (unsigned)s_toggleK) >= s_toggleDrop)
	{
		s_thrStart.clear();
		Thread_Times(s_thrStart);
		s_thrPeriod = period;
		s_thrFrame0 = frame;
		s_thrSpan = true;
	}
}

// One [GXHYBTH] line per thread busy >= 0.05 ms per frame in either state, then the process total; the accumulators restart.
void Thread_Print()
{
	const double onF = s_thrOnFrames > 0 ? (double)s_thrOnFrames : 1.0, offF = s_thrOffFrames > 0 ? (double)s_thrOffFrames : 1.0;
	double onT = 0.0, offT = 0.0;
	for (std::map<uint64_t, ThreadAcc>::iterator t = s_thrAcc.begin(); t != s_thrAcc.end(); ++t)
	{
		const double on = (double)t->second.onNs / 1e6 / onF, off = (double)t->second.offNs / 1e6 / offF;
		onT += on;
		offT += off;
		if (on >= 0.05 || off >= 0.05)
			fprintf(stderr, "[GXHYBTH] thread=%s tid=%llu off_ms_per_frame=%.4f on_ms_per_frame=%.4f diff_ms=%+.4f\n", t->second.name,
				(unsigned long long)t->first, off, on, on - off);
		t->second.onNs = t->second.offNs = 0;
	}
	fprintf(stderr, "[GXHYBTH] total on_frames=%llu off_frames=%llu off_ms_per_frame=%.4f on_ms_per_frame=%.4f diff_ms=%+.4f\n",
		(unsigned long long)s_thrOnFrames, (unsigned long long)s_thrOffFrames, offT, onT, onT - offT);
	s_thrOnFrames = s_thrOffFrames = 0;
}

void Frame_Stats(unsigned frame)
{
	const uint64_t now = Now_Ns();
	if (s_threads && s_toggleK > 0)
		Thread_Account(frame);
	if (s_toggleK > 0 && s_lastFrame != ~0u && s_lastTsNs != 0)
	{
		if ((s_lastFrame / (unsigned)s_toggleK) & 1)
			++s_onFrames;	// before the account: a row that closes on this frame counts it (design-r6 #7)
		Toggle_Account(s_lastFrame, frame, (double)(now - s_lastTsNs) / 1e6);
	}
	else if (s_lastFrame == ~0u)
	{
		s_winFrame0 = frame;	// the first row starts at the first observed frame, not at 0 (design-r6 #7)
		s_winLogic0 = TheGameLogic != nullptr ? TheGameLogic->getFrame() : 0;
	}
	s_lastTsNs = now;
	for (uint64_t n = s_classicEqualDraws.exchange(0); n > 0; --n)
		DX8Wrapper::Record_Direct_Draw();	// the EQUALTEST classic draws counted where the draw census lives (code-r6 #1)
	if (s_lastFrame != ~0u)
	{
		++s_frames;
		if (s_frameIns < s_frameMin) s_frameMin = s_frameIns;
		if (s_frameIns > s_frameMax) s_frameMax = s_frameIns;
	}
	s_lastFrame = frame;
	s_frameIns = 0;
	static const unsigned long long every = getenv("GX_RENDER_HYBRID_STATS") != nullptr && atoi(getenv("GX_RENDER_HYBRID_STATS")) > 0
		? (unsigned long long)atoi(getenv("GX_RENDER_HYBRID_STATS")) : 600ull;	// short validation runs want a census early
	if (s_frames == 0 || s_frames % every != 0)
		return;
	fprintf(stderr, "[GXHYB] f=%u frames=%llu ins/frame=%.2f min=%llu max=%llu by_site=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu"
		" by_boundary=rigid:%llu,delayed:%llu census_segments=%llu,%llu census_draws=%llu,%llu"
		" recorded=%llu refused=%llu no_pipeline=%llu pipelines=%llu (%llu us) packet_waits=%llu ring_skips=%llu fence_waits=%llu"
		" inpass=%llu fallback=%llu\n",
		frame, (unsigned long long)s_frames, (double)s_insertions / (double)s_frames, (unsigned long long)s_frameMin,
		(unsigned long long)s_frameMax, (unsigned long long)s_bySite[0], (unsigned long long)s_bySite[1],
		(unsigned long long)s_bySite[2], (unsigned long long)s_bySite[3], (unsigned long long)s_bySite[4],
		(unsigned long long)s_bySite[5], (unsigned long long)s_bySite[6], (unsigned long long)s_bySite[7],
		(unsigned long long)s_byBoundary[0],
		(unsigned long long)s_byBoundary[1], Sum_Census(s_censusSegments, 0), Sum_Census(s_censusSegments, 1),
		Sum_Census(s_censusDraws, 0), Sum_Census(s_censusDraws, 1), (unsigned long long)s_recorded.load(),
		(unsigned long long)s_refused.load(), (unsigned long long)s_noPipeline.load(),
		(unsigned long long)s_pipelinesMade.load(), (unsigned long long)s_pipelineUs.load(),
		(unsigned long long)s_packetWaits, (unsigned long long)s_ringSkips, (unsigned long long)s_fenceWaits.load(),
		(unsigned long long)s_inPassTotal.load(), (unsigned long long)s_fallbackTotal.load());
	// per site, rigid then delayed boundary: segments / segments that drew (the insertion lower bound) / classic draws (the
	// upper bound) - cumulative, so a window's counts are the difference of two rows (design-r3 #5, r4 #3; rw1x/hybt.py)
	// one fprintf per row (design-r8 #3): a line built in pieces can be split by another thread's diagnostic
	char census[1024];
	int n = snprintf(census, sizeof(census), "[GXHYBC] f=%u frames=%llu", frame, (unsigned long long)s_frames);
	for (int i = 0; i < SITE_COUNT && n > 0 && n < (int)sizeof(census); ++i)
		n += snprintf(census + n, sizeof(census) - n, " s%d=%llu/%llu/%llu,%llu/%llu/%llu", i,
			(unsigned long long)s_censusSegments[i][0], (unsigned long long)s_censusNonEmpty[i][0],
			(unsigned long long)s_censusDraws[i][0], (unsigned long long)s_censusSegments[i][1],
			(unsigned long long)s_censusNonEmpty[i][1], (unsigned long long)s_censusDraws[i][1]);
	fprintf(stderr, "%s\n", census);
	fflush(stderr);
}

} // namespace

bool Slot_Free(const Packet *pk)
{
	if (pk->done.load(std::memory_order_acquire))
		return true;
	return Mode() == 1 && s_state == STATE_READY && s_ext->GetRetiredValue() >= pk->retire;
}

// One insertion at a flush boundary whose classic segment drew something: the census upper bound of where L2 would
// have native work (a native-only segment would have no classic draws: this is the census,
// reported beside the insertions, not L2's executable predicate).
// One insertion at boundary 'boundary' of the current site (the per-insertion half of Boundary).
void Insert_One(int boundary, unsigned frame)
{
	if (s_toggleK > 0)
	{
		++s_onIns;
		++s_onRequested;
	}
	++s_bySite[s_site];
	++s_byBoundary[boundary];
	++s_insertions;
	++s_frameIns;

	// A slot (packet + descriptor set) is reused only once the GPU finished the command list that read it. Values are per
	// packet and monotonic. P-Q retires through its own fences, so only P-L waits; earlyreuse is the planted
	// premature reuse (descriptor sets updated while in use - validation must say so).
	const uint64_t retire = ++s_nextRetire;
	Packet *pk = &s_packets[retire % PACKETS];
	if (pk->retire != 0 && s_mutant != MUTANT_EARLYREUSE && !Slot_Free(pk))
	{
		if (pk->epoch == DX8Wrapper::Get_Present_Count())
		{
			// queued since the last Present: it cannot be submitted before this frame's Present
			--s_nextRetire;
			--s_insertions;
			--s_frameIns;
			--s_bySite[s_site];
			--s_byBoundary[boundary];
			if (s_toggleK > 0)
				--s_onIns;
			++s_ringSkips;
			if (s_interleave)
				DX8Wrapper::Device_Call(&Device_Redraw_Off, nullptr);	// no insertion, but the classic draw keeps its place
			return;
		}
		const uint64_t t0 = Now_Ns();
		bool said = false;
		while (!Slot_Free(pk))
		{
			if (!said && Now_Ns() - t0 > 100000000ull)
			{
				said = true;
				fprintf(stderr, "[GXHYB] WARNING: packet slot busy for 100 ms: retire=%llu epoch=%llu now=%llu\n",
					(unsigned long long)pk->retire, (unsigned long long)pk->epoch,
					(unsigned long long)DX8Wrapper::Get_Present_Count());
			}
			std::this_thread::yield();
		}
		++s_packetWaits;
	}
	pk->done.store(0, std::memory_order_relaxed);
	pk->epoch = DX8Wrapper::Get_Present_Count();
	pk->retire = retire;
	pk->site = s_site;
	pk->textured = 0;
	pk->noDraw = 0;
	pk->rawView = VK_NULL_HANDLE;
	pk->indexInFrame = (int)s_frameIns - 1;
	pk->frameSeq = s_frames;
	const int k = pk->indexInFrame % 8;
	pk->offset[0] = s_texTest ? ((k % 4) - 1.5f) * 170.0f : 0.0f;	// insertion k's cubes at position k
	pk->offset[1] = s_texTest ? ((k / 4) - 0.5f) * 170.0f : 0.0f;
	pk->offset[2] = s_texTest ? -600.0f : 0.0f;
	pk->offset[3] = 0.0f;
	s_texFrame = frame;	// read by the device thread: ordered by the ring hand-off
	DX8Wrapper::Device_Call(Mode() == 1 ? &Device_Insert_PL : &Device_Insert_PQ, pk);
}

// WS-G's flush-site tag (dx8renderer.h GxFlushSite: DX8MeshRendererClass::Flush moves GxFlushSiteNext there) -> this spike's
// site. -1 = no insertion: UNSET (an untagged flush), OCCL_EMPTY (the stats-only flush that draws nothing) and STATIC_SORT (the
// static sort lists after the translucent flush) - none was an insertion site before the tags either.
int Site_Of(int fs)
{
	switch (fs)
	{
	case GXFS_MAIN_OPAQUE: return SITE_OPAQUE;
	case GXFS_OCCL_TRANSL_PRE:
	case GXFS_OCCL_TRANSL_STENCIL: return SITE_OCCL_TRANSL;
	case GXFS_OCCL_BUCKET: return SITE_OCCL_BUCKET;
	case GXFS_OCCL_OTHERS: return SITE_OCCL_OTHERS;
	case GXFS_OCCL_OCCLUDERS: return SITE_OCCL_OCCLUDERS;
	case GXFS_OCCL_PLAIN: return SITE_OCCL_ALL;
	case GXFS_TRANSLUCENT: return SITE_TRANSLUCENT;
	case GXFS_PROJ_SHADOW: return SITE_PROJ_SHADOW;
	default: return -1;
	}
}

void Shutdown();	// below: DX8Wrapper::Pre_Release_Hook
void (*s_prevRelease)() = nullptr;	// the Pre_Release_Hook Lazy_Init replaced, called at Shutdown's end

// The first boundary of the first tagged flush: options are parsed on this (main) thread and Device_Init is queued in stream
// order, before the first insertion.
void Lazy_Init()
{
	const char *k = GxTestEnv("GX_RENDER_HYBRID_TOGGLE");
	const char *drop = GxTestEnv("GX_RENDER_HYBRID_TOGGLE_DROP");
	const char *nul = GxTestEnv("GX_RENDER_HYBRID_TOGGLE_NULL");
	s_toggleK = k != nullptr ? atoi(k) : 0;
	s_toggleDrop = drop != nullptr ? atoi(drop) : 3;
	s_toggleNull = nul != nullptr && atoi(nul) != 0;
	if (s_toggleK > 0)
		fprintf(stderr, "[GXHYBT] toggle armed: k=%d frames, drop=%d per period, null=%d (both states off)\n",
			s_toggleK, s_toggleDrop, (int)s_toggleNull);
	Parse_Options();
	if (Mode() == 2)
		DX8Wrapper::Pre_Reset_Hook = &Pre_Reset;
	if (DX8Wrapper::Pre_Release_Hook != &Shutdown)
		s_prevRelease = DX8Wrapper::Pre_Release_Hook;	// chained (WS-X2): GxDrawRun installs one too
	DX8Wrapper::Pre_Release_Hook = &Shutdown;
	s_state = STATE_QUEUED;
	DX8Wrapper::Device_Call(&Device_Init, nullptr);	// in stream order, before the first insertion
}

void Boundary(int boundary, unsigned long long classicDraws)
{
	s_site = Site_Of(GxFlushSite);
	if (s_site < 0 || s_state == STATE_FAILED)
		return;
	if (s_state == STATE_IDLE)
		Lazy_Init();
	++s_censusSegments[s_site][boundary];
	s_censusDraws[s_site][boundary] += classicDraws;
	if (classicDraws == 0)
		return;
	++s_censusNonEmpty[s_site][boundary];

	const unsigned frame = WW3D::Get_Frame_Count();
	if (frame != s_lastFrame)
		Frame_Stats(frame);
	if ((s_siteMask & (1u << s_site)) == 0)
		return;	// GX_RENDER_HYBRID_SITES: counted, not inserted at
	const bool off = s_toggleK > 0 && (s_toggleNull || ((frame / (unsigned)s_toggleK) & 1) == 0);	// an OFF period of the toggle
	if (off && !s_interleave)
		return;
	// GX_RENDER_HYBRID_SPLIT=n: n insertions at this boundary - the density sweep that tests whether the cost per insertion stays
	// constant from ~28 to Mode A's ~1,700 per frame (design-r9 #2); back to back, or each followed by a classic draw (INTERLEAVE)
	for (int i = 0; i < s_split; ++i)
		if (off)
			DX8Wrapper::Device_Call(&Device_Redraw_Off, nullptr);
		else
			Insert_One(boundary, frame);
}

// DX8Wrapper::Pre_Release_Hook (code-r4 #1): main thread, inside DX8Wrapper::Shutdown after DX8Ring::Drain() - the device is
// idle on the d3d8 side and about to be released. Without this our COM references kept DXVK's device (and its CS thread) alive
// past the C++ runtime's destruction of the globals a pending recorder reads. Order: no insertion from here on; DXVK's CS thread
// runs every pending recorder and submits (FlushRenderingCommands); the GPU retires our accepted work; our Vulkan objects go;
// our references go - so DX8Wrapper's own Release leaves DXVK free to destroy its device.
void Shutdown()
{
	const int state = s_state.exchange(STATE_FAILED);
	// Only Device_Init sets FAILED: an init that failed part-way (code-r5 #1) submitted nothing, so whatever it made goes at once
	bool idle = state != STATE_READY;
	if (state == STATE_READY && s_dev8 != nullptr)
	{
		GxIDxvkInteropDevice *q = s_qdev;
		if (q == nullptr && FAILED(s_dev8->QueryInterface(GX_IID_ID3D9VkInteropDevice, (void **)&q)))
			q = nullptr;
		if (q != nullptr)
			q->FlushRenderingCommands();
		idle = q != nullptr;
		if (idle && Mode() == 1)
		{
			const uint64_t t0 = Now_Ns();
			while (s_ext->GetRetiredValue() < s_lastAccepted.load() && Now_Ns() - t0 < 2000000000ull)
				std::this_thread::yield();
			idle = s_ext->GetRetiredValue() >= s_lastAccepted.load();
		}
		if (Mode() == 2)
			Pre_Reset();	// every slot's fence waited, views and surfaces released
		if (q != nullptr && q != s_qdev)
			q->Release();
	}
	if (!idle)
		fprintf(stderr, "[GXHYB] WARNING: shutdown without a retired GPU - our Vulkan objects are left for DXVK's device teardown\n");
	else if (s_loaded && s_info.device != VK_NULL_HANDLE)
	{
		const VkDevice d = s_info.device;
		{
			std::lock_guard<std::mutex> lock(s_pipelineLock);
			for (std::map<uint64_t, VkPipeline>::iterator it = s_pipelines.begin(); it != s_pipelines.end(); ++it)
				if (it->second != VK_NULL_HANDLE)
					s_vk.destroyPipeline(d, it->second, nullptr);
			s_pipelines.clear();
		}
		for (std::map<VkImage, VkImageView>::iterator it = s_rawViews.begin(); it != s_rawViews.end(); ++it)
			if (it->second != VK_NULL_HANDLE)
				s_vk.destroyImageView(d, it->second, nullptr);
		s_rawViews.clear();
		for (size_t i = 0; i < s_buffers.size(); ++i)
		{
			s_vk.destroyBuffer(d, s_buffers[i].first, nullptr);
			if (s_buffers[i].second != VK_NULL_HANDLE)
				s_vk.freeMemory(d, s_buffers[i].second, nullptr);
		}
		s_buffers.clear();
		VkShaderModule mods[] = { s_vert, s_frag, s_whiteFrag, s_texFrag };
		for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); ++i)
			if (mods[i] != VK_NULL_HANDLE)
				s_vk.destroyShaderModule(d, mods[i], nullptr);
		if (s_layout != VK_NULL_HANDLE) s_vk.destroyPipelineLayout(d, s_layout, nullptr);
		if (s_texLayout != VK_NULL_HANDLE) s_vk.destroyPipelineLayout(d, s_texLayout, nullptr);
		if (s_texSetLayout != VK_NULL_HANDLE) s_vk.destroyDescriptorSetLayout(d, s_texSetLayout, nullptr);
		if (s_descPool != VK_NULL_HANDLE) s_vk.destroyDescriptorPool(d, s_descPool, nullptr);	// frees its sets
		if (s_sampler != VK_NULL_HANDLE) s_vk.destroySampler(d, s_sampler, nullptr);
		for (int i = 0; i < QSLOTS; ++i)
			if (s_qslots[i].fence != VK_NULL_HANDLE)
				s_vk.destroyFence(d, s_qslots[i].fence, nullptr);
		if (s_pool != VK_NULL_HANDLE) s_vk.destroyCommandPool(d, s_pool, nullptr);	// frees the slots' command buffers
	}
	if (s_texInterop != nullptr) { s_texInterop->Release(); s_texInterop = nullptr; }
	if (s_tex8 != nullptr) { s_tex8->Release(); s_tex8 = nullptr; }
	if (s_qdev != nullptr) { s_qdev->Release(); s_qdev = nullptr; }
	if (s_ext != nullptr) { s_ext->Release(); s_ext = nullptr; }
	DX8MeshRendererClass::GxBoundary = nullptr;
	if (s_prevRelease != nullptr)
		s_prevRelease();	// WS-X2: the hook this one replaced (GxDrawRun's cache invalidation) still runs
}

void Install()
{
	if (Mode() != 0)
		DX8MeshRendererClass::GxBoundary = &Boundary;
}

uint64_t Take_Retire_Value()
{
	return ++s_nextRetire;
}

#endif // GX_HAVE_VULKAN

} // namespace GxInterop
