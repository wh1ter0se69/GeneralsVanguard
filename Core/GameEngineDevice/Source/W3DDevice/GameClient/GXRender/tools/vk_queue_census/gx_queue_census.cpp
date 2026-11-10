// GeneralsX: VK_LAYER_GX_queue_census: a TEST-ONLY Vulkan layer for the DLSS Frame Generation
// queue contract (PREREG A39a). Never shipped; loaded only when a test names it
// (VK_ADD_LAYER_PATH + VK_INSTANCE_LAYERS=VK_LAYER_GX_queue_census). Not in the game build, not digested.
//
// Why a layer: NVIDIA Streamline is the game's Vulkan loader root, and Streamline's own Vulkan calls go straight to the system
// loader. The loader puts this layer BELOW both, so it sees the game's queue operations AND Streamline's. For every queue operation
// (submit, present, wait-idle, bind-sparse), every acquire and every device wait-idle it records:
//  * the calling thread;
//  * the queue (family:index);
//  * the CALLER MODULE: the first stack frame outside this layer, the Vulkan loader and other VkLayer_* modules
//    (generalszh.exe, sl.dlss_g.dll, nvngx_dlssg.dll, ...).
// It also counts OVERLAP: a second thread entering an operation on a VkQueue while another thread is inside one on that queue. That
// is an external-synchronisation breach in itself. Images and swapchains made by modules other than the executable are listed with
// their usage (DLSS-G's proxy images: rw3-W's §7a question).
//
// Env:
//   GX_VK_CENSUS=<file>        the summary (rewritten every 2000 operations, at vkDestroyDevice and at process exit)
//   GX_VK_CENSUS_PLANT=<n>     TEST plant: at the n-th operation on the executable's first queue, a helper thread makes ONE empty
//                              vkQueueSubmit on that queue (caller module "census-plant") once the queue is idle - census_verdict.py
//                              must then FAIL C1
// Exports (selftest only): gxCensusTestHoldNext(ms) - the next queue operation on any thread waits ms INSIDE the layer, before the
// driver call, so a second thread can be made to enter while it is held (no real driver race); gxCensusFlush() - write now.
#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define GX_CENSUS_EXPORT extern "C" __declspec(dllexport)
// The loader's three entry points are declared by vulkan.h / vk_layer.h WITHOUT dllexport, and MSVC refuses a dllexport redefinition
// (C2375): on Windows they are exported by VkLayer_gx_queue_census.def instead.
#define GX_CENSUS_VK_EXPORT extern "C"
#else
#include <dlfcn.h>
#include <execinfo.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#define GX_CENSUS_EXPORT extern "C" __attribute__((visibility("default")))
#define GX_CENSUS_VK_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void* key(const void* dispatchable) { return *reinterpret_cast<void* const*>(dispatchable); }

uint64_t threadId()
{
#if defined(_WIN32)
	return GetCurrentThreadId();
#else
	uint64_t t = 0;
	pthread_threadid_np(nullptr, &t);
	return t;
#endif
}

std::string baseName(std::string p)
{
	const size_t s = p.find_last_of("\\/");
	return s == std::string::npos ? p : p.substr(s + 1);
}

bool startsWithNoCase(const std::string& s, const char* prefix)
{
	const size_t n = std::strlen(prefix);
	if (s.size() < n)
		return false;
	for (size_t i = 0; i < n; ++i)
		if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
			return false;
	return true;
}

// The modules a call passes through on its way here: the loader and other layers. The caller is the first frame outside them.
bool passThrough(const std::string& name)
{
	return startsWithNoCase(name, "vulkan-1") || startsWithNoCase(name, "libvulkan") || startsWithNoCase(name, "VkLayer_") ||
		startsWithNoCase(name, "libVkLayer_");
}

thread_local const char* t_forcedModule = nullptr;	// the plant thread names itself

std::mutex g_moduleLock;
std::unordered_map<uintptr_t, std::pair<std::string, bool>> g_moduleCache;	// module base -> (name, skip)

#if defined(_WIN32)
HMODULE selfModule()
{
	HMODULE m = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(&selfModule), &m);
	return m;
}

// Up to three distinct modules above the loader and the layers, innermost first: "sl.dlss_g.dll<sl.interposer.dll<generalszh.exe"
// = Streamline work inside a call the game made; "sl.dlss_g.dll<KERNEL32.DLL<ntdll.dll" = Streamline's own thread.
std::string callerChain()
{
	if (t_forcedModule)
		return t_forcedModule;
	static const HMODULE self = selfModule();
	void* frames[48] = {};
	const USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
	std::string chain, last;
	int found = 0;
	for (USHORT i = 0; i < n && found < 3; ++i) {
		HMODULE m = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(frames[i]), &m) || !m || m == self)
			continue;
		std::string name;
		bool skip = false;
		{
			std::lock_guard<std::mutex> g(g_moduleLock);
			auto it = g_moduleCache.find(reinterpret_cast<uintptr_t>(m));
			if (it == g_moduleCache.end()) {
				char path[MAX_PATH] = {};
				GetModuleFileNameA(m, path, MAX_PATH);
				const std::string b = baseName(path);
				it = g_moduleCache.emplace(reinterpret_cast<uintptr_t>(m), std::make_pair(b, passThrough(b))).first;
			}
			name = it->second.first;
			skip = it->second.second;
		}
		if (skip || name == last)
			continue;
		chain += (found ? "<" : "") + name;
		last = name;
		++found;
	}
	return found ? chain : std::string("?");
}

std::string exeName()
{
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	return baseName(path);
}
#else
std::string callerChain()
{
	if (t_forcedModule)
		return t_forcedModule;
	static const std::string self = [] {
		Dl_info i = {};
		return dladdr(reinterpret_cast<void*>(&threadId), &i) && i.dli_fname ? std::string(i.dli_fname) : std::string();
	}();
	void* frames[48] = {};
	const int n = backtrace(frames, 48);
	std::string chain, last;
	int found = 0;
	for (int i = 1; i < n && found < 3; ++i) {
		Dl_info info = {};
		if (!dladdr(frames[i], &info) || !info.dli_fname || self == info.dli_fname)
			continue;
		const std::string name = baseName(info.dli_fname);
		if (passThrough(name) || name == last)
			continue;
		chain += (found ? "<" : "") + name;
		last = name;
		++found;
	}
	return found ? chain : std::string("?");
}

std::string exeName()
{
	char path[4096] = {};
	uint32_t size = sizeof(path);
	return _NSGetExecutablePath(path, &size) == 0 ? baseName(path) : std::string("?");
}
#endif

std::string callerModule()
{
	const std::string c = callerChain();
	const size_t lt = c.find('<');
	return lt == std::string::npos ? c : c.substr(0, lt);
}

struct InstanceData
{
	VkInstance instance = VK_NULL_HANDLE;
	PFN_vkGetInstanceProcAddr gipa = nullptr;
	PFN_vkDestroyInstance destroyInstance = nullptr;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties queueFamilies = nullptr;
};

struct DeviceData
{
	VkDevice device = VK_NULL_HANDLE;
	PFN_vkGetDeviceProcAddr gdpa = nullptr;
	PFN_vkDestroyDevice destroyDevice = nullptr;
	PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
	PFN_vkGetDeviceQueue2 getDeviceQueue2 = nullptr;
	PFN_vkQueueSubmit queueSubmit = nullptr;
	PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
	PFN_vkQueueSubmit2KHR queueSubmit2KHR = nullptr;
	PFN_vkQueuePresentKHR queuePresent = nullptr;
	PFN_vkQueueWaitIdle queueWaitIdle = nullptr;
	PFN_vkQueueBindSparse queueBindSparse = nullptr;
	PFN_vkAcquireNextImageKHR acquireNextImage = nullptr;
	PFN_vkAcquireNextImage2KHR acquireNextImage2 = nullptr;
	PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
	PFN_vkCreateImage createImage = nullptr;
	PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
};

struct QueueState
{
	uint32_t family = 0, index = 0;
	void* deviceKey = nullptr;	// its device's dispatch key (never dereferenced through the queue after the device goes)
	bool alive = true;	// cleared by vkDestroyDevice under g_plantSerial (the plant must not outlive it)
	std::atomic<int> depth{ 0 };
	std::atomic<uint64_t> holder{ 0 };
	std::atomic<uint64_t> ops{ 0 };
};

std::mutex g_mapLock;
std::unordered_map<void*, InstanceData> g_instances;	// instance (and its physical devices') dispatch key
std::unordered_map<void*, DeviceData> g_devices;	// device (and its queues') dispatch key
std::unordered_map<VkQueue, QueueState*> g_queues;
VkInstance g_lastInstance = VK_NULL_HANDLE;	// fallback for a physical device whose dispatch key is not an instance's

std::mutex g_countLock;
std::map<std::string, uint64_t> g_counts;	// "<kind> <fields>" -> count
std::map<std::string, uint64_t> g_acquireNs;	// acquire key -> total ns blocked
std::string g_header;
uint64_t g_creatorTid = 0;
VkQueue g_exeQueue = VK_NULL_HANDLE;	// the first queue the executable itself fetched (the plant's target)
std::atomic<uint64_t> g_totalOps{ 0 }, g_overlaps{ 0 };
std::atomic<unsigned> g_holdNextMs{ 0 };
std::atomic<bool> g_plantFired{ false };
// While the plant is armed, every operation on the plant's queue is serialised (recursive: Streamline's work nests inside the
// game's present on one thread), so the planted op is a FOREIGN-THREAD op (C1) and never a real concurrent driver call.
std::recursive_mutex g_plantSerial;

long plantAt()
{
	static const long at = [] {
		const char* e = std::getenv("GX_VK_CENSUS_PLANT");
		return e ? std::atol(e) : 0L;
	}();
	return at;
}

void count(const std::string& k, uint64_t n = 1)
{
	std::lock_guard<std::mutex> g(g_countLock);
	g_counts[k] += n;
}

DeviceData* deviceOf(const void* dispatchable)
{
	std::lock_guard<std::mutex> g(g_mapLock);
	auto it = g_devices.find(key(dispatchable));
	return it == g_devices.end() ? nullptr : &it->second;
}

InstanceData* instanceOf(const void* dispatchable)
{
	std::lock_guard<std::mutex> g(g_mapLock);
	auto it = g_instances.find(key(dispatchable));
	return it == g_instances.end() ? nullptr : &it->second;
}

QueueState* queueState(VkQueue q)
{
	std::lock_guard<std::mutex> g(g_mapLock);
	auto it = g_queues.find(q);
	return it == g_queues.end() ? nullptr : it->second;
}

std::string queueName(const QueueState* s)
{
	return s ? std::to_string(s->family) + ":" + std::to_string(s->index) : std::string("?:?");
}

void writeSummary();

// One queue operation: thread, module, overlap; the TEST hold happens here, before the driver call.
class QueueScope
{
public:
	QueueScope(VkQueue q, const char* fn) : m_state(queueState(q))
	{
		if (plantAt() > 0 && q == g_exeQueue)
			m_serial = std::unique_lock<std::recursive_mutex>(g_plantSerial);
		const uint64_t me = threadId();
		const std::string module = callerChain();
		if (m_state) {
			const int prev = m_state->depth.fetch_add(1);
			if (prev > 0) {
				const uint64_t holder = m_state->holder.load();
				if (holder != me) {
					g_overlaps.fetch_add(1);
					count("overlap q=" + queueName(m_state) + " holder_tid=" + std::to_string(holder) + " tid=" + std::to_string(me) +
						" fn=" + fn + " module=" + module);
				}
			} else {
				m_state->holder.store(me);
			}
			m_state->ops.fetch_add(1);
		}
		count("op q=" + queueName(m_state) + " fn=" + fn + " tid=" + std::to_string(me) + " module=" + module);
		const unsigned hold = g_holdNextMs.exchange(0);
		if (hold)
			std::this_thread::sleep_for(std::chrono::milliseconds(hold));
		if (g_totalOps.fetch_add(1) % 2000 == 1999)
			writeSummary();
	}
	~QueueScope()
	{
		if (m_state)
			m_state->depth.fetch_sub(1);
	}
	QueueScope(const QueueScope&) = delete;
	QueueScope& operator=(const QueueScope&) = delete;

private:
	std::unique_lock<std::recursive_mutex> m_serial;	// destroyed after ~QueueScope's body: the depth drops before the unlock
	QueueState* m_state;
};

void maybePlant(VkQueue q);

// ---- queue operations
VKAPI_ATTR VkResult VKAPI_CALL census_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* s, VkFence f)
{
	DeviceData* d = deviceOf(q);
	if (!d)
		return VK_ERROR_DEVICE_LOST;
	VkResult r;
	{
		QueueScope sc(q, "vkQueueSubmit");
		r = d->queueSubmit(q, n, s, f);
	}
	maybePlant(q);
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL census_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f)
{
	DeviceData* d = deviceOf(q);
	if (!d)
		return VK_ERROR_DEVICE_LOST;
	VkResult r;
	{
		QueueScope sc(q, "vkQueueSubmit2");
		r = d->queueSubmit2(q, n, s, f);
	}
	maybePlant(q);
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL census_QueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f)
{
	DeviceData* d = deviceOf(q);
	if (!d)
		return VK_ERROR_DEVICE_LOST;
	VkResult r;
	{
		QueueScope sc(q, "vkQueueSubmit2KHR");
		r = d->queueSubmit2KHR(q, n, s, f);
	}
	maybePlant(q);
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL census_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR* p)
{
	DeviceData* d = deviceOf(q);
	if (!d)
		return VK_ERROR_DEVICE_LOST;
	QueueScope sc(q, "vkQueuePresentKHR");
	return d->queuePresent(q, p);
}

VKAPI_ATTR VkResult VKAPI_CALL census_QueueWaitIdle(VkQueue q)
{
	DeviceData* d = deviceOf(q);
	if (!d)
		return VK_ERROR_DEVICE_LOST;
	QueueScope sc(q, "vkQueueWaitIdle");
	return d->queueWaitIdle(q);
}

VKAPI_ATTR VkResult VKAPI_CALL census_QueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo* b, VkFence f)
{
	DeviceData* d = deviceOf(q);
	if (!d)
		return VK_ERROR_DEVICE_LOST;
	QueueScope sc(q, "vkQueueBindSparse");
	return d->queueBindSparse(q, n, b, f);
}

// ---- the plant: ONE empty submit on the executable's first queue from a foreign thread, once that queue is idle
void maybePlant(VkQueue q)
{
	const long at = plantAt();
	if (at <= 0 || q != g_exeQueue || g_plantFired.load())
		return;
	QueueState* s = queueState(q);
	if (!s || s->ops.load() < uint64_t(at) || g_plantFired.exchange(true))
		return;
	std::thread([q, s] {
		t_forcedModule = "census-plant";
		// g_plantSerial first: vkDestroyDevice takes it too, so the device is either still alive for the whole op or already gone
		std::lock_guard<std::recursive_mutex> serial(g_plantSerial);
		if (!s->alive)
			return;
		PFN_vkQueueSubmit submit = nullptr;
		{
			std::lock_guard<std::mutex> g(g_mapLock);
			auto it = g_devices.find(s->deviceKey);
			if (it == g_devices.end())
				return;
			submit = it->second.queueSubmit;
		}
		QueueScope sc(q, "vkQueueSubmit");
		submit(q, 0, nullptr, VK_NULL_HANDLE);
		count("plant fired q=" + queueName(s));
	}).detach();
}

// ---- device-level operations
VKAPI_ATTR VkResult VKAPI_CALL census_AcquireNextImageKHR(VkDevice dev, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem,
	VkFence fence, uint32_t* index)
{
	DeviceData* d = deviceOf(dev);
	const std::string k = "acquire fn=vkAcquireNextImageKHR tid=" + std::to_string(threadId()) + " module=" + callerChain();
	const auto t0 = std::chrono::steady_clock::now();
	const VkResult r = d->acquireNextImage(dev, sc, timeout, sem, fence, index);
	const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
	{
		std::lock_guard<std::mutex> g(g_countLock);
		g_counts[k] += 1;
		g_acquireNs[k] += ns;
	}
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL census_AcquireNextImage2KHR(VkDevice dev, const VkAcquireNextImageInfoKHR* info, uint32_t* index)
{
	DeviceData* d = deviceOf(dev);
	count("acquire fn=vkAcquireNextImage2KHR tid=" + std::to_string(threadId()) + " module=" + callerModule());
	return d->acquireNextImage2(dev, info, index);
}

VKAPI_ATTR VkResult VKAPI_CALL census_DeviceWaitIdle(VkDevice dev)
{
	DeviceData* d = deviceOf(dev);
	count("dev fn=vkDeviceWaitIdle tid=" + std::to_string(threadId()) + " module=" + callerChain());
	return d->deviceWaitIdle(dev);
}

VKAPI_ATTR VkResult VKAPI_CALL census_CreateImage(VkDevice dev, const VkImageCreateInfo* ci, const VkAllocationCallbacks* a, VkImage* out)
{
	DeviceData* d = deviceOf(dev);
	static const std::string exe = exeName();
	const std::string module = callerModule();
	if (module != exe) {	// the game's own thousands of textures are not the question; Streamline's images are
		char line[256];
		std::snprintf(line, sizeof line, "image module=%s fmt=%d w=%u h=%u usage=0x%x flags=0x%x samples=%u", module.c_str(),
			int(ci->format), ci->extent.width, ci->extent.height, unsigned(ci->usage), unsigned(ci->flags), unsigned(ci->samples));
		count(line);
	}
	return d->createImage(dev, ci, a, out);
}

VKAPI_ATTR VkResult VKAPI_CALL census_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* a,
	VkSwapchainKHR* out)
{
	DeviceData* d = deviceOf(dev);
	char line[256];
	std::snprintf(line, sizeof line, "swapchain module=%s tid=%llu w=%u h=%u fmt=%d usage=0x%x images=%u mode=%d", callerChain().c_str(),
		(unsigned long long)threadId(), ci->imageExtent.width, ci->imageExtent.height, int(ci->imageFormat), unsigned(ci->imageUsage),
		ci->minImageCount, int(ci->presentMode));
	count(line);
	return d->createSwapchain(dev, ci, a, out);
}

void noteQueue(DeviceData* d, VkQueue q, uint32_t family, uint32_t index, const char* fn)
{
	if (!q)
		return;
	const std::string module = callerModule();
	{
		std::lock_guard<std::mutex> g(g_mapLock);
		if (g_queues.find(q) == g_queues.end()) {
			QueueState* s = new QueueState();	// lives to process exit (the summary reads it)
			s->family = family;
			s->index = index;
			s->deviceKey = key(d->device);
			g_queues[q] = s;
		}
	}
	static const std::string exe = exeName();
	if (module == exe && g_exeQueue == VK_NULL_HANDLE)
		g_exeQueue = q;
	count(std::string("getqueue fn=") + fn + " q=" + std::to_string(family) + ":" + std::to_string(index) + " tid=" +
		std::to_string(threadId()) + " module=" + module);
	(void)d;
}

VKAPI_ATTR void VKAPI_CALL census_GetDeviceQueue(VkDevice dev, uint32_t family, uint32_t index, VkQueue* q)
{
	DeviceData* d = deviceOf(dev);
	d->getDeviceQueue(dev, family, index, q);
	noteQueue(d, *q, family, index, "vkGetDeviceQueue");
}

VKAPI_ATTR void VKAPI_CALL census_GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* q)
{
	DeviceData* d = deviceOf(dev);
	d->getDeviceQueue2(dev, info, q);
	noteQueue(d, *q, info->queueFamilyIndex, info->queueIndex, "vkGetDeviceQueue2");
}

VKAPI_ATTR void VKAPI_CALL census_DestroyDevice(VkDevice dev, const VkAllocationCallbacks* a)
{
	writeSummary();
	std::lock_guard<std::recursive_mutex> serial(g_plantSerial);	// a plant op in flight finishes first; a later one sees alive=false
	DeviceData* d = deviceOf(dev);
	PFN_vkDestroyDevice destroy = d ? d->destroyDevice : nullptr;
	{
		std::lock_guard<std::mutex> g(g_mapLock);
		for (auto& kv : g_queues)
			if (kv.second->deviceKey == key(dev))
				kv.second->alive = false;
		g_devices.erase(key(dev));
	}
	if (destroy)
		destroy(dev, a);
}

// ---- creation and dispatch
template <typename T>
void load(T& out, PFN_vkGetDeviceProcAddr gdpa, VkDevice dev, const char* name)
{
	out = reinterpret_cast<T>(gdpa(dev, name));
}

VKAPI_ATTR VkResult VKAPI_CALL census_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* a,
	VkDevice* out)
{
	VkLayerDeviceCreateInfo* link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(ci->pNext));
	while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
		link = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(link->pNext));
	if (!link)
		return VK_ERROR_INITIALIZATION_FAILED;
	const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	const PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;
	InstanceData* id = instanceOf(phys);
	if (!id) {
		std::lock_guard<std::mutex> g(g_mapLock);
		if (g_lastInstance) {
			auto it = g_instances.find(key(g_lastInstance));
			if (it != g_instances.end())
				id = &it->second;
		}
	}
	// 's own pattern: the next layer's vkCreateDevice through a NULL instance
	PFN_vkCreateDevice create = reinterpret_cast<PFN_vkCreateDevice>(gipa(VK_NULL_HANDLE, "vkCreateDevice"));
	if (!create && id)
		create = reinterpret_cast<PFN_vkCreateDevice>(gipa(id->instance, "vkCreateDevice"));
	if (!create)
		return VK_ERROR_INITIALIZATION_FAILED;
	const VkResult r = create(phys, ci, a, out);
	if (r != VK_SUCCESS)
		return r;
	DeviceData d;
	d.device = *out;
	d.gdpa = gdpa;
	load(d.destroyDevice, gdpa, *out, "vkDestroyDevice");
	load(d.getDeviceQueue, gdpa, *out, "vkGetDeviceQueue");
	load(d.getDeviceQueue2, gdpa, *out, "vkGetDeviceQueue2");
	load(d.queueSubmit, gdpa, *out, "vkQueueSubmit");
	load(d.queueSubmit2, gdpa, *out, "vkQueueSubmit2");
	load(d.queueSubmit2KHR, gdpa, *out, "vkQueueSubmit2KHR");
	load(d.queuePresent, gdpa, *out, "vkQueuePresentKHR");
	load(d.queueWaitIdle, gdpa, *out, "vkQueueWaitIdle");
	load(d.queueBindSparse, gdpa, *out, "vkQueueBindSparse");
	load(d.acquireNextImage, gdpa, *out, "vkAcquireNextImageKHR");
	load(d.acquireNextImage2, gdpa, *out, "vkAcquireNextImage2KHR");
	load(d.deviceWaitIdle, gdpa, *out, "vkDeviceWaitIdle");
	load(d.createImage, gdpa, *out, "vkCreateImage");
	load(d.createSwapchain, gdpa, *out, "vkCreateSwapchainKHR");
	{
		std::lock_guard<std::mutex> g(g_mapLock);
		g_devices[key(*out)] = d;
	}
	// The queues as passed DOWN (after Streamline's device proxy added its own): family, count, flags.
	std::vector<VkQueueFamilyProperties> props;
	if (id && id->queueFamilies) {
		uint32_t n = 0;
		id->queueFamilies(phys, &n, nullptr);
		props.resize(n);
		id->queueFamilies(phys, &n, props.data());
	}
	g_creatorTid = threadId();
	const std::string module = callerModule();
	for (uint32_t i = 0; i < ci->queueCreateInfoCount; ++i) {
		const VkDeviceQueueCreateInfo& q = ci->pQueueCreateInfos[i];
		const unsigned flags = q.queueFamilyIndex < props.size() ? unsigned(props[q.queueFamilyIndex].queueFlags) : 0u;
		char line[200];
		std::snprintf(line, sizeof line, "device_queues family=%u count=%u family_flags=0x%x created_by=%s", q.queueFamilyIndex,
			q.queueCount, flags, module.c_str());
		count(line);
	}
	static std::once_flag once;
	std::call_once(once, [] { std::atexit(&writeSummary); });
	return r;
}

VKAPI_ATTR VkResult VKAPI_CALL census_CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* a, VkInstance* out)
{
	VkLayerInstanceCreateInfo* link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(ci->pNext));
	while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO))
		link = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(link->pNext));
	if (!link)
		return VK_ERROR_INITIALIZATION_FAILED;
	const PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;
	const PFN_vkCreateInstance create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
	if (!create)
		return VK_ERROR_INITIALIZATION_FAILED;
	const VkResult r = create(ci, a, out);
	if (r != VK_SUCCESS)
		return r;
	InstanceData d;
	d.instance = *out;
	d.gipa = gipa;
	d.destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*out, "vkDestroyInstance"));
	d.queueFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(*out, "vkGetPhysicalDeviceQueueFamilyProperties"));
	std::lock_guard<std::mutex> g(g_mapLock);
	g_instances[key(*out)] = d;
	g_lastInstance = *out;
	return r;
}

VKAPI_ATTR void VKAPI_CALL census_DestroyInstance(VkInstance inst, const VkAllocationCallbacks* a)
{
	InstanceData* d = instanceOf(inst);
	PFN_vkDestroyInstance destroy = d ? d->destroyInstance : nullptr;
	{
		std::lock_guard<std::mutex> g(g_mapLock);
		g_instances.erase(key(inst));
	}
	if (destroy)
		destroy(inst, a);
}

PFN_vkVoidFunction deviceIntercept(const char* name)
{
	struct Entry { const char* name; PFN_vkVoidFunction fn; };
	static const Entry table[] = {
		{ "vkGetDeviceQueue", reinterpret_cast<PFN_vkVoidFunction>(&census_GetDeviceQueue) },
		{ "vkGetDeviceQueue2", reinterpret_cast<PFN_vkVoidFunction>(&census_GetDeviceQueue2) },
		{ "vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(&census_QueueSubmit) },
		{ "vkQueueSubmit2", reinterpret_cast<PFN_vkVoidFunction>(&census_QueueSubmit2) },
		{ "vkQueueSubmit2KHR", reinterpret_cast<PFN_vkVoidFunction>(&census_QueueSubmit2KHR) },
		{ "vkQueuePresentKHR", reinterpret_cast<PFN_vkVoidFunction>(&census_QueuePresentKHR) },
		{ "vkQueueWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(&census_QueueWaitIdle) },
		{ "vkQueueBindSparse", reinterpret_cast<PFN_vkVoidFunction>(&census_QueueBindSparse) },
		{ "vkAcquireNextImageKHR", reinterpret_cast<PFN_vkVoidFunction>(&census_AcquireNextImageKHR) },
		{ "vkAcquireNextImage2KHR", reinterpret_cast<PFN_vkVoidFunction>(&census_AcquireNextImage2KHR) },
		{ "vkDeviceWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(&census_DeviceWaitIdle) },
		{ "vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(&census_CreateImage) },
		{ "vkCreateSwapchainKHR", reinterpret_cast<PFN_vkVoidFunction>(&census_CreateSwapchainKHR) },
		{ "vkDestroyDevice", reinterpret_cast<PFN_vkVoidFunction>(&census_DestroyDevice) },
	};
	for (const Entry& e : table)
		if (std::strcmp(name, e.name) == 0)
			return e.fn;
	return nullptr;
}

// A wrapper is handed out only where the next layer has the function (an extension the device did not enable stays null).
bool deviceHas(const DeviceData& d, const char* name)
{
	return d.gdpa(d.device, name) != nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL census_GetDeviceProcAddr(VkDevice dev, const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL census_GetInstanceProcAddr(VkInstance inst, const char* name)
{
	if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&census_GetInstanceProcAddr);
	if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&census_GetDeviceProcAddr);
	if (std::strcmp(name, "vkCreateInstance") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&census_CreateInstance);
	if (std::strcmp(name, "vkDestroyInstance") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&census_DestroyInstance);
	if (std::strcmp(name, "vkCreateDevice") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&census_CreateDevice);
	if (!inst)
		return nullptr;
	InstanceData* d = instanceOf(inst);
	if (!d)
		return nullptr;
	if (PFN_vkVoidFunction f = deviceIntercept(name))
		return d->gipa(inst, name) ? f : nullptr;
	return d->gipa(inst, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL census_GetDeviceProcAddr(VkDevice dev, const char* name)
{
	if (std::strcmp(name, "vkGetDeviceProcAddr") == 0)
		return reinterpret_cast<PFN_vkVoidFunction>(&census_GetDeviceProcAddr);
	DeviceData* d = deviceOf(dev);
	if (!d)
		return nullptr;
	if (PFN_vkVoidFunction f = deviceIntercept(name))
		return deviceHas(*d, name) ? f : nullptr;
	return d->gdpa(dev, name);
}

// ---- summary
std::mutex g_writeLock;

void writeSummary()
{
	const char* path = std::getenv("GX_VK_CENSUS");
	if (!path || !*path)
		return;
	std::lock_guard<std::mutex> w(g_writeLock);
	std::map<std::string, uint64_t> counts, acquireNs;
	{
		std::lock_guard<std::mutex> g(g_countLock);
		counts = g_counts;
		acquireNs = g_acquireNs;
	}
	const std::string tmp = std::string(path) + ".tmp";
	FILE* f = std::fopen(tmp.c_str(), "w");
	if (!f)
		return;
	std::fprintf(f, "census v1 exe=%s creator_tid=%llu ops=%llu overlaps=%llu\n", exeName().c_str(),
		(unsigned long long)g_creatorTid, (unsigned long long)g_totalOps.load(), (unsigned long long)g_overlaps.load());
	{
		std::lock_guard<std::mutex> g(g_mapLock);
		for (const auto& kv : g_queues)
			std::fprintf(f, "queue q=%u:%u ops=%llu exe_first=%d\n", kv.second->family, kv.second->index,
				(unsigned long long)kv.second->ops.load(), kv.first == g_exeQueue ? 1 : 0);
	}
	for (const auto& kv : counts) {
		auto ns = acquireNs.find(kv.first);
		if (ns != acquireNs.end())
			std::fprintf(f, "%s n=%llu blocked_ms=%.3f\n", kv.first.c_str(), (unsigned long long)kv.second, double(ns->second) / 1e6);
		else
			std::fprintf(f, "%s n=%llu\n", kv.first.c_str(), (unsigned long long)kv.second);
	}
	std::fclose(f);
	std::remove(path);
	std::rename(tmp.c_str(), path);
}

} // namespace

// ---- exports
GX_CENSUS_VK_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v)
{
	if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (v->loaderLayerInterfaceVersion > 2)
		v->loaderLayerInterfaceVersion = 2;
	v->pfnGetInstanceProcAddr = &census_GetInstanceProcAddr;
	v->pfnGetDeviceProcAddr = &census_GetDeviceProcAddr;
	v->pfnGetPhysicalDeviceProcAddr = nullptr;
	return VK_SUCCESS;
}

GX_CENSUS_VK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char* name)
{
	return census_GetInstanceProcAddr(inst, name);
}

GX_CENSUS_VK_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char* name)
{
	return census_GetDeviceProcAddr(dev, name);
}

GX_CENSUS_EXPORT void gxCensusTestHoldNext(unsigned ms)
{
	g_holdNextMs.store(ms);
}

GX_CENSUS_EXPORT void gxCensusFlush()
{
	writeSummary();
}
