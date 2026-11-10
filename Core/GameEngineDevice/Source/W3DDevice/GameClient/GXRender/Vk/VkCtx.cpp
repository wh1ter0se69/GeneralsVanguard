// GeneralsX: WS-B VkCtx v0 - see VkCtx.h.
#include "VkCtx.h"

#include "vk_mem_alloc.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <cerrno>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

namespace GXRender { namespace Vk {

TestMutants g_testMutants;
void (*g_testAfterLoadDevice)(VkDevice) = nullptr;

namespace {

void* openLibrary(const char* name)
{
#if defined(_WIN32)
	return reinterpret_cast<void*>(LoadLibraryA(name));
#else
	return dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* librarySymbol(void* lib, const char* name)
{
#if defined(_WIN32)
	return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(lib), name));
#else
	return dlsym(lib, name);
#endif
}

void closeLibrary(void* lib)
{
#if defined(_WIN32)
	FreeLibrary(reinterpret_cast<HMODULE>(lib));
#else
	dlclose(lib);
#endif
}

// Where a function pointer actually lives - the provenance of "which Vulkan did we get", never assumed from the name.
std::string modulePathOf(void* fn)
{
#if defined(_WIN32)
	HMODULE mod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(fn), &mod)) {
		char path[MAX_PATH] = {};
		if (GetModuleFileNameA(mod, path, MAX_PATH))
			return path;
	}
	return "?";
#else
	Dl_info info = {};
	if (dladdr(fn, &info) && info.dli_fname)
		return info.dli_fname;
	return "?";
#endif
}

// The DXVK fork's search list (references/fbraz3-dxvk src/vulkan/vulkan_loader.cpp), kept in the same order so the
// renderer and DXVK resolve the same library on every platform.
const char* const* platformLoaderNames(size_t* count)
{
#if defined(_WIN32)
	static const char* const names[] = { "winevulkan.dll", "vulkan-1.dll" };
#elif defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	static const char* const names[] = {
		"@executable_path/Frameworks/MoltenVK.framework/MoltenVK",
		"@executable_path/Frameworks/libMoltenVK.dylib",
		"libvulkan.dylib",
		"libvulkan.1.dylib",
		"libMoltenVK.dylib",
	};
#elif defined(__APPLE__)
	static const char* const names[] = { "libvulkan.dylib", "libvulkan.1.dylib", "libMoltenVK.dylib" };
#else
	static const char* const names[] = { "libvulkan.so", "libvulkan.so.1" };
#endif
	*count = sizeof(names) / sizeof(names[0]);
	return names;
}

bool hasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
{
	for (const VkExtensionProperties& e : list)
		if (std::strcmp(e.extensionName, name) == 0)
			return true;
	return false;
}

const char* deviceTypeName(VkPhysicalDeviceType t)
{
	switch (t) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
	case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
	default: return "other";
	}
}

int deviceTypeScore(VkPhysicalDeviceType t)
{
	switch (t) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
	case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
	default: return 0;
	}
}

std::string lower(std::string s)
{
	for (char& c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

// A line on fd 2 that the caller never waits on for more than 1 s: the (blocking) write runs on a detached thread of its own, so a
// write that blocks - a full stderr pipe whose reader stopped draining, even right after a poll reported room (another writer can
// fill it in between) - cannot hold the caller; the std::_Exit that follows ends that thread with the
// process. Delivery is best effort: exit code 3 is the signal that never depends on it. Each call has its own heap copy (leaked:
// the process is ending, or it is a test marker).
struct OwnLine
{
	char text[320];
	size_t len = 0;
	std::atomic<bool> done { false };
};

#if !defined(_WIN32)
void* writeOwnLine(void* p)
{
	OwnLine* l = static_cast<OwnLine*>(p);
	size_t done = 0;
	while (done < l->len) {
		const ssize_t n = write(2, l->text + done, l->len - done);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		done += static_cast<size_t>(n);
	}
	l->done.store(true);
	return nullptr;
}
#endif

void writeFatal(const char* line, size_t len)
{
#if defined(_WIN32)
	(void)_write(2, line, static_cast<unsigned>(len));   // Trade-off: unbounded on a stuck Windows pipe; the native renderer runs on Apple/Linux
#else
	OwnLine* l = new (std::nothrow) OwnLine;
	if (!l)
		return;
	l->len = std::min(len, sizeof(l->text));
	std::memcpy(l->text, line, l->len);
	pthread_t t;
	if (pthread_create(&t, nullptr, writeOwnLine, l) != 0)
		return;   // no thread, no line: the exit code still says it
	pthread_detach(t);
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);   // wall time, not a count of sleeps (r6 P2-2)
	while (!l->done.load() && std::chrono::steady_clock::now() < end)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
}

void defaultLog(LogLevel, const char* line, void*)
{
	std::fprintf(stderr, "%s\n", line);
	std::fflush(stderr);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

uint64_t Timeline::completed() const
{
	uint64_t value = 0;
	if (m_semaphore != VK_NULL_HANDLE) {
		const VkResult r = vkGetSemaphoreCounterValue(m_device, m_semaphore, &value);
		if (r != VK_SUCCESS) {   // the output is undefined on an error: report nothing completed
			if (m_owner && !m_owner->m_deviceLost.load())
				m_owner->noteResult(r, "vkGetSemaphoreCounterValue");
			return 0;
		}
	}
	return value;
}

bool Timeline::wait(uint64_t value, uint64_t timeoutNs) const
{
	if (value == 0)
		return true;
	if (value > m_lastSubmitted)
		return false; // never submitted: waiting would hang forever
	if (m_owner && m_owner->m_threaded && !g_testMutants.waitSkipsSync) {
		// R19: the op must have run on the submit thread first - a value refused or skipped behind a refusal is never signalled,
		// and a vkWaitSemaphores on it would hang. An EARLIER accepted value may still run on the GPU: it gets the real wait below
		// (a refusal latches no driver loss)
		const auto t0 = std::chrono::steady_clock::now();
		if (m_owner->syncExecuted(m_index, value, timeoutNs) != VkCtx::Sync::Executed)
			return false;
		if (timeoutNs != UINT64_MAX) {
			const uint64_t spent = static_cast<uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
			timeoutNs = spent >= timeoutNs ? 0 : timeoutNs - spent;
		}
	}
	// already reached: no vkWaitSemaphores (MoltenVK builds and tears down an MTLSharedEventListener per call - render-perf-audit,
	// ~0.036 ms of main per light frame). After the sync above, so a value not yet run never takes it; a counter >= value proves
	// the op ran anyway (a refused value and everything behind it is never submitted).
	if (completed() >= (g_testMutants.waitFastPathOffByOne ? value - 1 : value)) {
		if (m_owner)
			m_owner->noteFastPath();
		return true;
	}
	if (m_owner && m_owner->m_deviceLost.load())   // a DRIVER loss only: a refusal under the submit thread keeps real waits
		return completed() >= value;   // lost (a driver error was latched): read, never block
	VkSemaphoreWaitInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
	info.semaphoreCount = 1;
	info.pSemaphores = &m_semaphore;
	info.pValues = &value;
	if (m_owner) {
		m_owner->m_blockedWaits.fetch_add(1);   // TEST boundary: a waiter is inside the driver wait (VkCtx::testBlockedWaits)
		m_owner->m_semWaitEntries.fetch_add(1);   // ... and a persistent count of entries (testSemWaitEntries)
	}
	const VkResult r = g_testMutants.waitDeviceLost ? VK_ERROR_DEVICE_LOST : vkWaitSemaphores(m_device, &info, timeoutNs);
	if (m_owner)
		m_owner->m_blockedWaits.fetch_sub(1);
	if (r == VK_SUCCESS)
		return true;
	if (r != VK_TIMEOUT && m_owner)
		m_owner->noteResult(r, "vkWaitSemaphores");
	return false;
}

// ---------------------------------------------------------------------------------------------------------------------

void VkCtx::logf(LogLevel level, const char* fmt, ...) const
{
	char line[8192];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(line, sizeof(line), fmt, args);
	va_end(args);
	(m_desc.log ? m_desc.log : defaultLog)(level, line, m_desc.logUser);
}

void VkCtx::noteResult(VkResult r, const char* what) const
{
	if (r == VK_SUCCESS)
		return;
	if (r == VK_ERROR_DEVICE_LOST)
		m_deviceLost.store(true);
	logf(LogLevel::Error, "[GXVK] %s failed: VkResult %d", what, static_cast<int>(r));
}

VKAPI_ATTR VkBool32 VKAPI_CALL VkCtx::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
	VkCtx* self = static_cast<VkCtx*>(user);
	const char* id = data && data->pMessageIdName ? data->pMessageIdName : "?";
	const char* msg = data && data->pMessage ? data->pMessage : "";
	const bool isError = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
	{
		std::lock_guard<std::mutex> lock(self->m_counterMutex);
		if (isError) {
			++self->m_counters.errors;
			if (std::strncmp(id, "SYNC-HAZARD", 11) == 0)
				++self->m_counters.syncHazards;
			if (self->m_counters.firstError.empty())
				self->m_counters.firstError = std::string(id) + ": " + std::string(msg).substr(0, 600);
		} else {
			++self->m_counters.warnings;
		}
	}
	self->logf(isError ? LogLevel::Error : LogLevel::Warn, "[GXVK] validation %s %s: %.1500s", isError ? "ERROR" : "WARNING", id, msg);
	return (isError && self->m_desc.abortCallsOnValidationError) ? VK_TRUE : VK_FALSE;
}

ValidationCounters VkCtx::validation() const
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	return m_counters;
}

void VkCtx::resetValidationCounters()
{
	std::lock_guard<std::mutex> lock(m_counterMutex);
	m_counters = ValidationCounters();
}

// ---------------------------------------------------------------------------------------------------------------------

bool VkCtx::loadLibrary(const VkCtxDesc& desc, std::string* why)
{
	PFN_vkGetInstanceProcAddr gipa = desc.getInstanceProcAddr;
	if (gipa) {
		m_loaderPath = modulePathOf(reinterpret_cast<void*>(gipa)) + " (caller-supplied)";
	} else {
		size_t count = 0;
		const char* const* names = desc.loaderPath ? &desc.loaderPath : platformLoaderNames(&count);
		if (desc.loaderPath)
			count = 1;
		for (size_t i = 0; i < count && !gipa; ++i) {
			void* lib = openLibrary(names[i]);
			if (!lib)
				continue;
			gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(librarySymbol(lib, "vkGetInstanceProcAddr"));
			if (!gipa) {
				closeLibrary(lib);
				continue;
			}
			m_library = lib;
			m_loaderPath = modulePathOf(reinterpret_cast<void*>(gipa));
		}
	}
	if (!gipa) {
		*why = desc.loaderPath ? std::string("no vkGetInstanceProcAddr in ") + desc.loaderPath
		                       : "no Vulkan loader found (searched the DXVK fork's platform list)";
		return false;
	}
	volkInitializeCustom(gipa);
	return true;
}

bool VkCtx::createInstance(const VkCtxDesc& desc, std::string* why)
{
	uint32_t loaderVersion = volkGetInstanceVersion();
	if (loaderVersion < VK_API_VERSION_1_1) {
		*why = "Vulkan loader is older than 1.1";
		return false;
	}

	uint32_t count = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());

	const char* layerName = "VK_LAYER_KHRONOS_validation";
	std::vector<const char*> layers;
	bool validationFeaturesExt = false;
	if (m_validation) {
		vkEnumerateInstanceLayerProperties(&count, nullptr);
		std::vector<VkLayerProperties> layerProps(count);
		vkEnumerateInstanceLayerProperties(&count, layerProps.data());
		bool found = false;
		for (const VkLayerProperties& l : layerProps)
			found = found || std::strcmp(l.layerName, layerName) == 0;
		if (!found && m_validationOptional) {
			logf(LogLevel::Warn, "[GXVK] debug build: VK_LAYER_KHRONOS_validation not installed - running WITHOUT validation");
			m_validation = m_syncValidation = false;
		} else if (!found) {
			*why = "validation requested but VK_LAYER_KHRONOS_validation is not installed (VK_ADD_LAYER_PATH / VK_LAYER_PATH)";
			return false;
		}
	}
	if (m_validation) {
		layers.push_back(layerName);
		vkEnumerateInstanceExtensionProperties(layerName, &count, nullptr);
		std::vector<VkExtensionProperties> layerExts(count);
		vkEnumerateInstanceExtensionProperties(layerName, &count, layerExts.data());
		validationFeaturesExt = hasExtension(layerExts, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
		if (m_syncValidation && !validationFeaturesExt) {
			*why = "synchronization validation requested but the layer has no VK_EXT_validation_features";
			return false;
		}
		for (const VkExtensionProperties& e : layerExts)
			if (!hasExtension(available, e.extensionName))
				available.push_back(e);
	}

	std::vector<const char*> extensions;
	for (const char* e : desc.instanceExtensions) {
		if (!hasExtension(available, e)) {
			*why = std::string("required instance extension missing: ") + e;
			return false;
		}
		extensions.push_back(e);
	}
	auto addOptional = [&](const char* e) {
		if (!hasExtension(available, e))
			return false;
		for (const char* have : extensions)
			if (std::strcmp(have, e) == 0)
				return true;
		extensions.push_back(e);
		return true;
	};
	const bool debugUtils = addOptional(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
	if (m_validation && !debugUtils) {
		*why = "validation requested but VK_EXT_debug_utils is unavailable (messages could not be counted)";
		return false;
	}
	const bool portability = addOptional(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	if (m_validation && m_syncValidation)
		addOptional(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);

	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = desc.appName;
	app.applicationVersion = desc.appVersion;
	app.pEngineName = "GXRender";
	app.engineVersion = 1;
	app.apiVersion = VK_API_VERSION_1_3;

	VkDebugUtilsMessengerCreateInfoEXT messengerInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
	messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
	messengerInfo.pfnUserCallback = &VkCtx::debugCallback;
	messengerInfo.pUserData = this;

	const VkValidationFeatureEnableEXT syncEnable = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
	VkValidationFeaturesEXT validationFeatures = { VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT };
	validationFeatures.enabledValidationFeatureCount = 1;
	validationFeatures.pEnabledValidationFeatures = &syncEnable;

	VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	info.pApplicationInfo = &app;
	info.enabledLayerCount = static_cast<uint32_t>(layers.size());
	info.ppEnabledLayerNames = layers.data();
	info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();
	if (portability)
		info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	if (m_validation) {
		info.pNext = &messengerInfo; // catches messages from vkCreateInstance itself
		if (m_syncValidation) {
			messengerInfo.pNext = &validationFeatures;
		}
	}

	VkResult r = vkCreateInstance(&info, nullptr, &m_instance);
	if (r != VK_SUCCESS) {
		*why = "vkCreateInstance failed: VkResult " + std::to_string(static_cast<int>(r));
		m_instance = VK_NULL_HANDLE;
		return false;
	}
	volkLoadInstance(m_instance);

	if (debugUtils && m_validation) {
		messengerInfo.pNext = nullptr;
		r = vkCreateDebugUtilsMessengerEXT(m_instance, &messengerInfo, nullptr, &m_messenger);
		if (r != VK_SUCCESS) {
			*why = "vkCreateDebugUtilsMessengerEXT failed";
			return false;
		}
	}
	return true;
}

bool VkCtx::pickDevice(const VkCtxDesc& desc, std::string* why)
{
	uint32_t count = 0;
	vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
	std::vector<VkPhysicalDevice> devices(count);
	vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
	if (devices.empty()) {
		*why = "no Vulkan physical devices";
		return false;
	}

	const char* select = std::getenv("GX_VK_DEVICE");
	if (!select || !*select)
		select = desc.deviceSelect;

	int best = -1;
	int bestScore = -1;
	std::string reasons;
	for (uint32_t i = 0; i < count; ++i) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(devices[i], &props);

		std::string reject;
		if (props.apiVersion < VK_API_VERSION_1_3)
			reject = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
			         std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + " < 1.3";
		if (reject.empty()) {
			VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
			VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
			v12.pNext = &v13;
			VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
			f2.pNext = &v12;
			vkGetPhysicalDeviceFeatures2(devices[i], &f2);
			if (!v12.timelineSemaphore)
				reject = "no timelineSemaphore";
			else if (!v13.synchronization2)
				reject = "no synchronization2";
		}
		if (reject.empty()) {
			uint32_t qc = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qc, nullptr);
			std::vector<VkQueueFamilyProperties> families(qc);
			vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qc, families.data());
			bool ok = false;
			for (uint32_t f = 0; f < qc && !ok; ++f) {
				const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
				ok = (families[f].queueFlags & need) == need &&
				     (!desc.presentSupport || desc.presentSupport(m_instance, devices[i], f));
			}
			if (!ok)
				reject = desc.presentSupport ? "no graphics+compute family that can present" : "no graphics+compute family";
		}

		bool chosenByName = false;
		if (select && *select) {
			const bool isIndex = std::all_of(select, select + std::strlen(select), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
			chosenByName = isIndex ? static_cast<uint32_t>(std::atoi(select)) == i
			                       : lower(props.deviceName).find(lower(select)) != std::string::npos;
		}
		logf(LogLevel::Info, "[GXVK] physical %u \"%s\" type=%s api=%u.%u.%u%s%s", i, props.deviceName,
			deviceTypeName(props.deviceType), VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
			VK_API_VERSION_PATCH(props.apiVersion), reject.empty() ? "" : " REJECTED: ", reject.c_str());

		if (select && *select) {
			if (chosenByName) {
				if (!reject.empty()) {
					*why = std::string("selected device \"") + props.deviceName + "\" does not qualify: " + reject;
					return false;
				}
				if (best < 0)
					best = static_cast<int>(i);
			}
			continue;
		}
		if (!reject.empty()) {
			reasons += std::string(props.deviceName) + ": " + reject + "; ";
			continue;
		}
		const int score = deviceTypeScore(props.deviceType);
		if (score > bestScore) {
			bestScore = score;
			best = static_cast<int>(i);
		}
	}
	if (best < 0) {
		*why = select && *select ? std::string("no device matches GX_VK_DEVICE/deviceSelect \"") + select + "\""
		                         : "no device meets the floor (Vulkan 1.3, timeline, sync2, graphics): " + reasons;
		return false;
	}
	m_physical = devices[best];
	vkGetPhysicalDeviceProperties(m_physical, &m_props);
	m_apiVersion = m_props.apiVersion;
	return true;
}

bool VkCtx::createDevice(const VkCtxDesc& desc, std::string* why)
{
	uint32_t qc = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qc, nullptr);
	std::vector<VkQueueFamilyProperties> families(qc);
	vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qc, families.data());

	uint32_t graphicsFamily = UINT32_MAX;
	for (uint32_t f = 0; f < qc && graphicsFamily == UINT32_MAX; ++f) {
		const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
		if ((families[f].queueFlags & need) == need && (!desc.presentSupport || desc.presentSupport(m_instance, m_physical, f)))
			graphicsFamily = f;
	}
	// A dedicated copy engine: transfer without graphics (prefer one without compute too). None on lavapipe/MoltenVK.
	uint32_t transferFamily = UINT32_MAX;
	for (int pass = 0; pass < 2 && transferFamily == UINT32_MAX; ++pass) {
		for (uint32_t f = 0; f < qc && transferFamily == UINT32_MAX; ++f) {
			const VkQueueFlags flags = families[f].queueFlags;
			if (f == graphicsFamily || !(flags & VK_QUEUE_TRANSFER_BIT) || (flags & VK_QUEUE_GRAPHICS_BIT))
				continue;
			if (pass == 0 && (flags & VK_QUEUE_COMPUTE_BIT))
				continue;
			transferFamily = f;
		}
	}

	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, nullptr);
	std::vector<VkExtensionProperties> available(count);
	vkEnumerateDeviceExtensionProperties(m_physical, nullptr, &count, available.data());

	std::vector<const char*> extensions;
	for (const char* e : desc.deviceExtensions) {
		if (!hasExtension(available, e)) {
			*why = std::string("required device extension missing: ") + e;
			return false;
		}
		extensions.push_back(e);
	}
	auto addOptional = [&](const char* e) {
		if (!hasExtension(available, e))
			return false;
		for (const char* have : extensions)
			if (std::strcmp(have, e) == 0)
				return true;
		extensions.push_back(e);
		return true;
	};
	m_features.portabilitySubset = addOptional(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME); // REQUIRED when listed
	const bool memoryBudget = addOptional(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
	m_features.calibratedTimestamps = addOptional(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ||
	                                  addOptional(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
	const bool eds2 = addOptional(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
	const bool eds3 = addOptional(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
	const bool vertexInput = addOptional(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
	const bool gpl = hasExtension(available, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) &&
	                 hasExtension(available, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
	                 addOptional(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && addOptional(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);

	// Enable what the device supports, minus what costs performance or changes memory behaviour when merely enabled.
	VkPhysicalDevicePortabilitySubsetFeaturesKHR portability = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR };
	VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	VkPhysicalDeviceVulkan11Features v11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
	VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
	f2.pNext = &v11;
	v11.pNext = &v12;
	v12.pNext = &v13;
	VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT };
	VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT };
	VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vif = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT };
	VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
	void** tail = &v13.pNext;
	auto chain = [&tail](void* next, VkBaseOutStructure* base) {
		*tail = next;
		tail = reinterpret_cast<void**>(&base->pNext);
	};
	if (m_features.portabilitySubset)
		chain(&portability, reinterpret_cast<VkBaseOutStructure*>(&portability));
	if (eds2)
		chain(&eds2f, reinterpret_cast<VkBaseOutStructure*>(&eds2f));
	if (eds3)
		chain(&eds3f, reinterpret_cast<VkBaseOutStructure*>(&eds3f));
	if (vertexInput)
		chain(&vif, reinterpret_cast<VkBaseOutStructure*>(&vif));
	if (gpl)
		chain(&gplf, reinterpret_cast<VkBaseOutStructure*>(&gplf));
	vkGetPhysicalDeviceFeatures2(m_physical, &f2);

	VkPhysicalDeviceFeatures& f = f2.features;
	f.robustBufferAccess = VK_FALSE;
	f.sparseBinding = VK_FALSE;
	f.sparseResidencyBuffer = VK_FALSE;
	f.sparseResidencyImage2D = VK_FALSE;
	f.sparseResidencyImage3D = VK_FALSE;
	f.sparseResidency2Samples = VK_FALSE;
	f.sparseResidency4Samples = VK_FALSE;
	f.sparseResidency8Samples = VK_FALSE;
	f.sparseResidency16Samples = VK_FALSE;
	f.sparseResidencyAliased = VK_FALSE;
	v11.protectedMemory = VK_FALSE;
	v12.bufferDeviceAddressCaptureReplay = VK_FALSE;
	v12.bufferDeviceAddressMultiDevice = VK_FALSE;
	v13.robustImageAccess = VK_FALSE;

	m_features.samplerAnisotropy = f.samplerAnisotropy;
	m_features.textureCompressionBC = f.textureCompressionBC;
	m_features.textureCompressionASTC_LDR = f.textureCompressionASTC_LDR;
	m_features.textureCompressionETC2 = f.textureCompressionETC2;
	m_features.fillModeNonSolid = f.fillModeNonSolid;
	m_features.independentBlend = f.independentBlend;
	m_features.depthClamp = f.depthClamp;
	m_features.depthBiasClamp = f.depthBiasClamp;
	m_features.dualSrcBlend = f.dualSrcBlend;
	m_features.multiDrawIndirect = f.multiDrawIndirect;
	m_features.drawIndirectFirstInstance = f.drawIndirectFirstInstance;
	m_features.shaderClipDistance = f.shaderClipDistance;
	m_features.timelineSemaphore = v12.timelineSemaphore;
	m_features.synchronization2 = v13.synchronization2;
	m_features.dynamicRendering = v13.dynamicRendering;
	m_features.hostQueryReset = v12.hostQueryReset;
	m_features.descriptorIndexing = v12.descriptorIndexing;
	m_features.scalarBlockLayout = v12.scalarBlockLayout;
	m_features.maintenance4 = v13.maintenance4;
	m_features.extendedDynamicState2LogicOp = eds2 && eds2f.extendedDynamicState2LogicOp;
	m_features.extendedDynamicState2PatchControlPoints = eds2 && eds2f.extendedDynamicState2PatchControlPoints;
	m_features.eds3ColorBlendEnable = eds3 && eds3f.extendedDynamicState3ColorBlendEnable;
	m_features.eds3ColorBlendEquation = eds3 && eds3f.extendedDynamicState3ColorBlendEquation;
	m_features.eds3ColorWriteMask = eds3 && eds3f.extendedDynamicState3ColorWriteMask;
	m_features.eds3RasterizationSamples = eds3 && eds3f.extendedDynamicState3RasterizationSamples;
	m_features.eds3AlphaToCoverageEnable = eds3 && eds3f.extendedDynamicState3AlphaToCoverageEnable;
	m_features.eds3PolygonMode = eds3 && eds3f.extendedDynamicState3PolygonMode;
	m_features.eds3DepthClampEnable = eds3 && eds3f.extendedDynamicState3DepthClampEnable;
	m_features.vertexInputDynamicState = vertexInput && vif.vertexInputDynamicState;
	m_features.graphicsPipelineLibrary = gpl && gplf.graphicsPipelineLibrary;

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo queues[2] = {};
	uint32_t queueCount = 0;
	queues[queueCount] = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	queues[queueCount].queueFamilyIndex = graphicsFamily;
	queues[queueCount].queueCount = 1;
	queues[queueCount].pQueuePriorities = &priority;
	++queueCount;
	if (transferFamily != UINT32_MAX) {
		queues[queueCount] = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queues[queueCount].queueFamilyIndex = transferFamily;
		queues[queueCount].queueCount = 1;
		queues[queueCount].pQueuePriorities = &priority;
		++queueCount;
	}

	VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	info.pNext = &f2;
	info.queueCreateInfoCount = queueCount;
	info.pQueueCreateInfos = queues;
	info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();
	VkResult r = vkCreateDevice(m_physical, &info, nullptr, &m_device);
	if (r != VK_SUCCESS) {
		*why = "vkCreateDevice failed: VkResult " + std::to_string(static_cast<int>(r));
		m_device = VK_NULL_HANDLE;
		return false;
	}
	volkLoadDevice(m_device);
	if (g_testAfterLoadDevice)
		g_testAfterLoadDevice(m_device);
	for (const char* e : extensions)
		m_enabledDeviceExtensions.push_back(e);

	auto setupQueue = [&](QueueInfo& q, uint32_t family) -> bool {
		q.family = family;
		vkGetDeviceQueue(m_device, family, 0, &q.queue);
		q.timestampValidBits = families[family].timestampValidBits;
		VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
		type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		type.initialValue = 0;
		VkSemaphoreCreateInfo semInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		semInfo.pNext = &type;
		q.timeline.m_owner = this;
		q.timeline.m_index = &q == &m_transfer ? 1 : 0;
		q.timeline.m_device = m_device;
		q.timeline.m_lastSubmitted = 0;
		return vkCreateSemaphore(m_device, &semInfo, nullptr, &q.timeline.m_semaphore) == VK_SUCCESS;
	};
	if (!setupQueue(m_graphics, graphicsFamily) || (transferFamily != UINT32_MAX && !setupQueue(m_transfer, transferFamily))) {
		*why = "timeline semaphore creation failed";
		return false;
	}

	VmaVulkanFunctions functions = {};
	functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
	functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
	VmaAllocatorCreateInfo allocInfo = {};
	allocInfo.flags = memoryBudget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;
	allocInfo.physicalDevice = m_physical;
	allocInfo.device = m_device;
	allocInfo.instance = m_instance;
	allocInfo.vulkanApiVersion = VK_API_VERSION_1_3;
	allocInfo.pVulkanFunctions = &functions;
	r = vmaCreateAllocator(&allocInfo, &m_allocator);
	if (r != VK_SUCCESS) {
		*why = "vmaCreateAllocator failed: VkResult " + std::to_string(static_cast<int>(r));
		m_allocator = nullptr;
		return false;
	}
	return true;
}

void VkCtx::buildDeviceLine()
{
	VkPhysicalDeviceDriverProperties& p12 = m_driverProps;
	p12 = VkPhysicalDeviceDriverProperties();
	p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
	VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
	p2.pNext = &p12;
	vkGetPhysicalDeviceProperties2(m_physical, &p2);
	p12.pNext = nullptr;
	VkPhysicalDeviceMemoryProperties mem;
	vkGetPhysicalDeviceMemoryProperties(m_physical, &mem);

	const VkPhysicalDeviceLimits& l = m_props.limits;
	const DeviceFeatures& f = m_features;
	char uuid[2 * VK_UUID_SIZE + 1];
	for (int i = 0; i < VK_UUID_SIZE; ++i)
		std::snprintf(uuid + 2 * i, 3, "%02x", m_props.pipelineCacheUUID[i]);
	char line[8192];
	int n = std::snprintf(line, sizeof(line),
		"[GXVK] device \"%s\" type=%s vendor=0x%04x id=0x%04x api=%u.%u.%u driver=\"%s %s\" loader=%s "
		"gfxFamily=%u transfer=%s tsBits=%u tsPeriod=%.3fns validation=%s "
		"limits: image2D=%u pushConst=%u boundSets=%u stageSamplers=%u stageImages=%u stageUBO=%u stageSSBO=%u "
		"setSamplers=%u aniso=%.0f vtxAttribs=%u vtxBindings=%u colorAtt=%u colorMS=0x%x depthMS=0x%x "
		"uboAlign=%llu ssboAlign=%llu atom=%llu granularity=%llu memAllocs=%u indirectCount=%u viewports=%u "
		"features: aniso=%d bc=%d astc=%d etc2=%d nonSolid=%d indepBlend=%d depthClamp=%d biasClamp=%d dualSrc=%d "
		"mdi=%d firstInst=%d clipDist=%d timeline=%d sync2=%d dynRender=%d hostQueryReset=%d descIndexing=%d "
		"scalarBlock=%d maint4=%d portability=%d calibratedTs=%d eds2LogicOp=%d eds2PatchCP=%d eds3BlendEnable=%d "
		"eds3BlendEq=%d eds3WriteMask=%d eds3Samples=%d eds3A2C=%d eds3PolyMode=%d eds3DepthClamp=%d vertexInputDyn=%d "
		"gpl=%d driverID=%d driverVersion=0x%08x cacheUUID=%s heaps:",
		m_props.deviceName, deviceTypeName(m_props.deviceType), m_props.vendorID, m_props.deviceID,
		VK_API_VERSION_MAJOR(m_apiVersion), VK_API_VERSION_MINOR(m_apiVersion), VK_API_VERSION_PATCH(m_apiVersion),
		p12.driverName, p12.driverInfo, m_loaderPath.c_str(), m_graphics.family,
		m_transfer.valid() ? std::to_string(m_transfer.family).c_str() : "none", m_graphics.timestampValidBits,
		l.timestampPeriod, m_validation ? (m_syncValidation ? "core+sync" : "core") : "off",
		l.maxImageDimension2D, l.maxPushConstantsSize, l.maxBoundDescriptorSets, l.maxPerStageDescriptorSamplers,
		l.maxPerStageDescriptorSampledImages, l.maxPerStageDescriptorUniformBuffers, l.maxPerStageDescriptorStorageBuffers,
		l.maxDescriptorSetSamplers, l.maxSamplerAnisotropy, l.maxVertexInputAttributes, l.maxVertexInputBindings,
		l.maxColorAttachments, static_cast<unsigned>(l.framebufferColorSampleCounts),
		static_cast<unsigned>(l.framebufferDepthSampleCounts), static_cast<unsigned long long>(l.minUniformBufferOffsetAlignment),
		static_cast<unsigned long long>(l.minStorageBufferOffsetAlignment), static_cast<unsigned long long>(l.nonCoherentAtomSize),
		static_cast<unsigned long long>(l.bufferImageGranularity), l.maxMemoryAllocationCount, l.maxDrawIndirectCount,
		l.maxViewports, f.samplerAnisotropy, f.textureCompressionBC, f.textureCompressionASTC_LDR, f.textureCompressionETC2,
		f.fillModeNonSolid, f.independentBlend, f.depthClamp, f.depthBiasClamp, f.dualSrcBlend, f.multiDrawIndirect,
		f.drawIndirectFirstInstance, f.shaderClipDistance, f.timelineSemaphore, f.synchronization2, f.dynamicRendering,
		f.hostQueryReset, f.descriptorIndexing, f.scalarBlockLayout, f.maintenance4, f.portabilitySubset,
		f.calibratedTimestamps, f.extendedDynamicState2LogicOp, f.extendedDynamicState2PatchControlPoints,
		f.eds3ColorBlendEnable, f.eds3ColorBlendEquation, f.eds3ColorWriteMask, f.eds3RasterizationSamples,
		f.eds3AlphaToCoverageEnable, f.eds3PolygonMode, f.eds3DepthClampEnable, f.vertexInputDynamicState,
		f.graphicsPipelineLibrary, static_cast<int>(p12.driverID), m_props.driverVersion, uuid);
	for (uint32_t h = 0; h < mem.memoryHeapCount && n > 0 && n < static_cast<int>(sizeof(line)) - 64; ++h)
		n += std::snprintf(line + n, sizeof(line) - n, " %lluMiB%s", static_cast<unsigned long long>(mem.memoryHeaps[h].size >> 20),
			(mem.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "(local)" : "");
	m_deviceLine = line;
}

bool VkCtx::init(const VkCtxDesc& desc, std::string* why)
{
	std::string localWhy;
	if (!why)
		why = &localWhy;
	shutdown();
	m_desc = desc;
	m_validation = desc.validation;
	m_syncValidation = desc.validation && desc.syncValidation;
	m_validationOptional = false;
#if !defined(NDEBUG)
	if (!m_validation) {
		m_validation = m_syncValidation = true; // debug default: on when the layer is there, a warning when it is not
		m_validationOptional = true;
	}
#endif
	if (const char* env = std::getenv("GX_VK_VALIDATION")) {
		m_validationOptional = false;
		if (std::strcmp(env, "0") == 0) {
			m_validation = m_syncValidation = false;
		} else if (std::strcmp(env, "core") == 0) {
			m_validation = true;
			m_syncValidation = false;
		} else if (*env) {
			m_validation = m_syncValidation = true;
		}
	}
	resetValidationCounters();
	m_deviceLost.store(false);
	m_latched = m_stop = m_busy = false;
	m_latchedFlag.store(false);
	m_drained = false;
	m_refusal = SubmitRefusal();
	m_threadStats = SubmitThreadStats();
	m_waitFastPath.store(0);
	m_executed[0] = m_executed[1] = 0;
	m_seqNext = m_busySeq = m_holdAt = m_presentCount = 0;
	m_drivenPresents.store(0);   // with the context's other counters

	const bool ok = loadLibrary(desc, why) && createInstance(desc, why) && pickDevice(desc, why) && createDevice(desc, why);
	if (!ok) {
		logf(LogLevel::Error, "[GXVK] init failed: %s (loader: %s)", why->c_str(), m_loaderPath.empty() ? "none" : m_loaderPath.c_str());
		shutdown();
		return false;
	}
	buildDeviceLine();
	logf(LogLevel::Info, "%s", m_deviceLine.c_str());
	bool threaded = desc.submitThread == SubmitThreadMode::On;
	if (const char* env = std::getenv("GX_VK_SUBMIT_THREAD"))
		if (*env && desc.submitThread != SubmitThreadMode::Off)
			threaded = std::strcmp(env, "0") != 0;
	if (threaded)
		startThread();
	logf(LogLevel::Info, "[GXVK] submit thread %s", m_threaded ? "on (gx-vk-submit owns the queues)" : "off");
	return true;
}

void VkCtx::shutdown()
{
	stopThread();   // every queued op runs (or is skipped, once latched) before the device goes idle
	if (m_device != VK_NULL_HANDLE) {
		idleDevice(g_testMutants.idleResult);   // a proven idle (SUCCESS or DEVICE_LOST), or the process has ended
		std::vector<Retirement> pending;
		pending.swap(m_retirements);
		for (Retirement& r : pending)
			r.fn();
		if (m_allocator) {
			vmaDestroyAllocator(m_allocator);
			m_allocator = nullptr;
		}
		if (m_graphics.timeline.m_semaphore)
			vkDestroySemaphore(m_device, m_graphics.timeline.m_semaphore, nullptr);
		if (m_transfer.timeline.m_semaphore)
			vkDestroySemaphore(m_device, m_transfer.timeline.m_semaphore, nullptr);
		vkDestroyDevice(m_device, nullptr);
		m_device = VK_NULL_HANDLE;
	}
	m_graphics = QueueInfo();
	m_transfer = QueueInfo();
	if (m_messenger != VK_NULL_HANDLE) {
		vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
		m_messenger = VK_NULL_HANDLE;
	}
	if (m_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(m_instance, nullptr);
		m_instance = VK_NULL_HANDLE;
	}
	if (m_library) {
		closeLibrary(m_library);
		m_library = nullptr;
	}
	m_physical = VK_NULL_HANDLE;
	m_enabledDeviceExtensions.clear();
	m_features = DeviceFeatures();
}

bool VkCtx::deviceExtensionEnabled(const char* name) const
{
	for (const std::string& e : m_enabledDeviceExtensions)
		if (e == name)
			return true;
	return false;
}

uint64_t VkCtx::submit(QueueKind kind, const SubmitDesc& desc)
{
	if (m_deviceLost.load())
		return 0; // after a loss nothing is submitted until the context is rebuilt
	QueueInfo& q = queue(kind);
	if (m_threaded) {
		// the value is the one the synchronous path would give, in the same order; the thread submits it later, FIFO
		std::lock_guard<std::mutex> g(m_opMutex);
		if (m_latched || m_stop)
			return 0;
		const uint64_t value = q.timeline.m_lastSubmitted + 1;
		q.timeline.m_lastSubmitted = value;
		SubmitOp op;
		op.kind = SubmitOp::Submit;
		op.queue = kind;
		op.desc = desc;
		op.value = value;
		op.seq = ++m_seqNext;
		m_ops.push_back(std::move(op));
		++m_threadStats.enqueued;
		m_opCv.notify_one();
		return value;
	}
	const uint64_t value = q.timeline.m_lastSubmitted + 1;
	const VkResult r = submitDriver(q, desc, value);
	if (r != VK_SUCCESS) {
		noteResult(r, "vkQueueSubmit2");
		return 0;
	}
	q.timeline.m_lastSubmitted = value;
	return value;
}

// The driver call itself: desc's buffers and semaphores plus the signal of q's timeline at value.
VkResult VkCtx::submitDriver(QueueInfo& q, const SubmitDesc& desc, uint64_t value)
{
	std::vector<VkCommandBufferSubmitInfo> cmds(desc.commandBuffers.size());
	for (size_t i = 0; i < cmds.size(); ++i) {
		cmds[i] = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO };
		cmds[i].commandBuffer = desc.commandBuffers[i];
	}
	auto toInfo = [](const SemaphoreWait& w) {
		VkSemaphoreSubmitInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO };
		s.semaphore = w.semaphore;
		s.value = w.value;
		s.stageMask = w.stages;
		return s;
	};
	std::vector<VkSemaphoreSubmitInfo> waits;
	for (const SemaphoreWait& w : desc.waits)
		waits.push_back(toInfo(w));
	std::vector<VkSemaphoreSubmitInfo> signals;
	for (const SemaphoreWait& s : desc.signals)
		signals.push_back(toInfo(s));
	SemaphoreWait own;
	own.semaphore = q.timeline.m_semaphore;
	own.value = value;
	signals.push_back(toInfo(own));

	VkSubmitInfo2 info = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2 };
	info.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
	info.pWaitSemaphoreInfos = waits.data();
	info.commandBufferInfoCount = static_cast<uint32_t>(cmds.size());
	info.pCommandBufferInfos = cmds.data();
	info.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size());
	info.pSignalSemaphoreInfos = signals.data();
	return vkQueueSubmit2(q.queue, 1, &info, desc.fence);
}

void VkCtx::retire(QueueKind kind, uint64_t value, std::function<void()> fn)
{
	if (value == 0 || queue(kind).timeline.completed() >= value) {
		fn();
		return;
	}
	m_retirements.push_back({ kind, value, std::move(fn) });
}

void VkCtx::collect()
{
	if (m_retirements.empty())
		return;
	const uint64_t done[2] = { queue(QueueKind::Graphics).timeline.completed(), queue(QueueKind::Transfer).timeline.completed() };
	// a refused / skipped value never completes; its retirements run once the latch's drain has PROVED the device idle - then no
	// accepted batch still runs, so nothing a refused op shared with an earlier batch is freed under it (PREREG_T A21 am. 2 (1))
	bool settleAll = false;
	if (m_threaded && !g_testMutants.latchNoRetire) {
		std::lock_guard<std::mutex> g(m_opMutex);
		settleAll = m_latched && m_drained;
	}
	std::vector<Retirement> keep;
	std::vector<Retirement> run;
	for (Retirement& r : m_retirements)
		(settleAll || r.value <= done[queueIndex(r.kind)] ? run : keep).push_back(std::move(r));
	m_retirements.swap(keep);
	for (Retirement& r : run)
		r.fn();
}

void VkCtx::settleRetirements()
{
	std::vector<Retirement> run;
	run.swap(m_retirements);
	for (Retirement& r : run)
		r.fn();
}

// ---------------------------------------------------------------------------------------------------- the submit thread (R19)

void VkCtx::startThread()
{
	std::fegetenv(&m_fenv);   // the d3d8 ring does the same (dx8wrapper.cpp:1103): MoltenVK's encode-time float work as on main
	m_threaded = true;
	m_thread = std::thread(&VkCtx::threadMain, this);
}

void VkCtx::stopThread()
{
	if (!m_thread.joinable()) {
		m_threaded = false;   // never started, or already joined by quiesce(): the next init() chooses again
		return;
	}
	{
		std::lock_guard<std::mutex> g(m_opMutex);
		m_stop = true;
		m_holdAt = 0;
		if (g_testMutants.threadShutdownDrops)
			m_ops.clear();   // the plant: queued ops vanish, neither run nor counted as skipped
	}
	m_opCv.notify_all();
	m_thread.join();
	m_threaded = false;
}

void VkCtx::quiesce()
{
	if (!m_thread.joinable())
		return;
	{
		std::lock_guard<std::mutex> g(m_opMutex);
		m_stop = true;   // submit() / present() refuse from here; m_threaded stays set, so neither falls back to the driver inline
		++m_quiesceWaiters;
	}
	m_opCv.notify_all();
	m_thread.join();   // threadMain returns once the queue is empty (a TEST hold still holds it: the arms release it)
	std::lock_guard<std::mutex> g(m_opMutex);
	--m_quiesceWaiters;
}

void VkCtx::threadMain()
{
	std::fesetenv(&m_fenv);
#if defined(__APPLE__)
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   // as the d3d8 ring (dx8wrapper.cpp:1105)
	pthread_setname_np("gx-vk-submit");
#elif defined(__linux__)
	pthread_setname_np(pthread_self(), "gx-vk-submit");
#endif
	std::unique_lock<std::mutex> lk(m_opMutex);
	for (;;) {
		auto held = [this] {
			const SubmitOp& f = m_ops.front();
			return m_holdAt && f.kind == SubmitOp::Submit && queueIndex(f.queue) == 0 && f.value == m_holdAt;
		};
		m_opCv.wait(lk, [&] { return (!m_ops.empty() && !held()) || (m_stop && m_ops.empty()); });
		if (m_ops.empty())
			return;   // stopped, everything queued ran
		auto it = m_ops.begin();
		if (g_testMutants.threadPresentFirst && it->kind == SubmitOp::Submit)
			for (auto p = m_ops.begin(); p != m_ops.end(); ++p)
				if (p->kind == SubmitOp::Present) {   // the plant: a later present jumps the submit that signals its semaphore
					it = p;
					break;
				}
		SubmitOp op = std::move(*it);
		m_ops.erase(it);
		op.test.failAt = g_testMutants.submitFailAt;   // under the op lock: VkCtx::testSet writes them under it
		op.test.failResult = g_testMutants.submitFailResult;
		op.test.presentAt = g_testMutants.presentResultAt;
		op.test.presentResult = g_testMutants.presentResult;
		op.test.skippedOk = g_testMutants.skippedPresentOk;
		op.test.noLatch = g_testMutants.threadNoLatch;
		op.test.latchLost = g_testMutants.threadLatchLost;
		op.test.latchNoDrain = g_testMutants.latchNoDrain;
		op.test.initLast = g_testMutants.threadInitLast;
		op.test.hookOnInjected = g_testMutants.presentHookOnInjected;
		op.test.resultAfterDriverAt = g_testMutants.presentResultAfterDriverAt;
		op.test.resultAfterDriver = g_testMutants.presentResultAfterDriver;
		op.test.idleResult = g_testMutants.idleResult;
		m_busy = true;
		m_busySeq = op.seq;
		lk.unlock();
		runOp(op);
		lk.lock();
		m_busy = false;
		m_doneCv.notify_all();
	}
}

void VkCtx::runOp(SubmitOp& op)
{
	bool latched;
	{
		std::lock_guard<std::mutex> g(m_opMutex);
		latched = m_latched;
	}
	if (op.kind == SubmitOp::Present) {
		VkResult r = op.test.skippedOk ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;   // skipped: its frame never ran
		bool drove = false;
		if (!latched) {
			uint64_t n;
			{
				std::lock_guard<std::mutex> g(m_opMutex);
				n = ++m_presentCount;
				m_threadStats.presents = n;
			}
			if (op.test.presentAt && n == op.test.presentAt) {
				const PresentHook* h = op.test.hookOnInjected && op.hasHook ? &op.hook : nullptr;   // the X- plant (H2)
				if (h && h->start)
					h->start(h->ctx, h->token);
				r = op.test.presentResult;   // TEST: as the driver would answer; nothing is enqueued
				if (h && h->end)
					h->end(h->ctx, h->token, r);
			} else {
				VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
				info.waitSemaphoreCount = 1;
				info.pWaitSemaphores = &op.wait;
				info.swapchainCount = 1;
				info.pSwapchains = &op.swapchain;
				info.pImageIndices = &op.image;
				std::unique_lock<std::mutex> host;
				if (op.hostLock)
					host = std::unique_lock<std::mutex>(*op.hostLock);
				std::lock_guard<std::mutex> q(m_queueMutex);
				r = doPresent(info, op.hasHook ? &op.hook : nullptr, op.test.resultAfterDriverAt, op.test.resultAfterDriver);
				drove = true;
			}
			if (r == VK_ERROR_DEVICE_LOST)
				m_deviceLost.store(true);
		}
		std::lock_guard<std::mutex> g(m_opMutex);
		*op.result = r;
		m_threadStats.driverPresents += drove ? 1 : 0;
		m_threadStats.skipped += latched ? 1 : 0;
		return;
	}
	const int qi = queueIndex(op.queue);
	if (latched) {
		std::lock_guard<std::mutex> g(m_opMutex);
		++m_threadStats.skipped;
		return;
	}
	QueueInfo& q = queue(op.queue);
	VkResult r;
	if (op.test.failAt && qi == 0 && op.value == op.test.failAt) {
		r = op.test.failResult;   // TEST: a refusal at the real choke point; the driver is never called
	} else {
		if (op.test.initLast && op.desc.commandBuffers.size() > 1)   // the plant: a frame's init buffer runs after its main buffer
			std::rotate(op.desc.commandBuffers.begin(), op.desc.commandBuffers.begin() + 1, op.desc.commandBuffers.end());
		std::lock_guard<std::mutex> g(m_queueMutex);
		r = submitDriver(q, op.desc, op.value);
	}
	// A refusal first DRAINS the device - every earlier ACCEPTED batch completes - and only then becomes visible (PREREG_T A21
	// amendment 1 (1) / 2 (1); DXVK dxvk_queue.cpp:207-208): a caller whose resource was last used by the refused value, and before
	// it by a batch still on the GPU, cannot act on the refusal while that batch runs (GxNativeDevice::endFrameForHost ignores its
	// wait's answer). The latch is terminal (deviceLost() true, Present answers DEVICELOST) but publishes no driver loss.
	// VK_ERROR_DEVICE_LOST is a driver loss as today, with no drain (lost work never completes).
	const bool refused = r != VK_SUCCESS && !op.test.noLatch;
	if (refused && r != VK_ERROR_DEVICE_LOST && !op.test.latchLost && !op.test.latchNoDrain)
		idleDevice(op.test.idleResult);   // proves idle (a DEVICE_LOST answer latches a driver loss) or ends the process
	bool first = false;
	{
		std::lock_guard<std::mutex> g(m_opMutex);
		if (r == VK_SUCCESS) {
			m_executed[qi] = op.value;
			++m_threadStats.submits;
		} else if (refused) {
			first = !m_latched;
			if (first) {
				m_refusal = { op.desc.label, op.value, r };
				m_drained = r != VK_ERROR_DEVICE_LOST && !op.test.latchLost && !op.test.latchNoDrain && !m_deviceLost.load();
			}
			m_latched = true;
			m_latchedFlag.store(true);
			if (r == VK_ERROR_DEVICE_LOST || op.test.latchLost)
				m_deviceLost.store(true);
		}
	}
	// waiters on this value (syncExecuted / waitExecuted) wake NOW, not after the log lines below: a stuck stderr must not hold
	// them. threadMain notifies again when the op is done.
	if (r != VK_SUCCESS)
		m_doneCv.notify_all();
	// ONE log call per refusal, where the synchronous path makes its own (VkCtx::submit's noteResult, VkCtx.cpp:881 @33d36f1cc): the
	// thread adds no blocking point the synchronous path lacks (PREREG_T A21 am. 2 (6); stderr liveness itself is process-wide, R24).
	// Not "FATAL": the device's own FATAL line names this refusal (one FATAL line per session).
	if (r != VK_SUCCESS && (r != VK_ERROR_DEVICE_LOST || first)) {
		char what[160];
		if (first)
			std::snprintf(what, sizeof(what), "vkQueueSubmit2 (%s value=%llu - submissions stopped)", op.desc.label ? op.desc.label : "?",
				static_cast<unsigned long long>(op.value));
		noteResult(r, first ? what : "vkQueueSubmit2");
	}
}

VkCtx::Sync VkCtx::syncExecuted(int index, uint64_t value, uint64_t timeoutNs) const
{
	std::unique_lock<std::mutex> lk(m_opMutex);
	auto ready = [&] { return m_executed[index] >= value || m_latched; };
	if (timeoutNs >= 1000000000000000ull)   // ~11.6 days: forever (and no overflow in wait_for)
		m_doneCv.wait(lk, ready);
	else if (!m_doneCv.wait_for(lk, std::chrono::nanoseconds(timeoutNs), ready))
		return Sync::Timeout;
	return m_executed[index] >= value ? Sync::Executed : Sync::Refused;
}

VkResult VkCtx::doPresent(const VkPresentInfoKHR& info, const PresentHook* hook, uint64_t overrideAt, VkResult overrideResult)
{
	if (hook && hook->start)
		hook->start(hook->ctx, hook->token);
	VkResult r = vkQueuePresentKHR(m_graphics.queue, &info);
	const uint64_t driven = m_drivenPresents.fetch_add(1) + 1;   // EVERY driven present (not only armed ones)
	if (overrideAt && driven == overrideAt)
		r = overrideResult;   // TEST (not a defect): the driver ran; its answer is reported as this one
	if (hook && hook->end)
		hook->end(hook->ctx, hook->token, r);
	return r;
}

bool VkCtx::testQueueMutexHeld()
{
	bool held = false;
	std::thread t([this, &held] {
		if (m_queueMutex.try_lock())
			m_queueMutex.unlock();
		else
			held = true;
	});
	t.join();
	return held;
}

uint64_t VkCtx::present(VkSwapchainKHR swapchain, uint32_t image, VkSemaphore wait, std::mutex* hostLock, VkResult* result,
	const PresentHook* hook)
{
	if (!m_threaded) {
		VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &wait;
		info.swapchainCount = 1;
		info.pSwapchains = &swapchain;
		info.pImageIndices = &image;
		*result = doPresent(info, hook, g_testMutants.presentResultAfterDriverAt, g_testMutants.presentResultAfterDriver);
		return 0;
	}
	std::lock_guard<std::mutex> g(m_opMutex);
	if (m_latched || m_stop) {   // its frame's submission never ran: the semaphore it would wait on is never signalled
		*result = g_testMutants.skippedPresentOk ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
		++m_threadStats.skipped;
		return 0;
	}
	SubmitOp op;
	op.kind = SubmitOp::Present;
	op.seq = ++m_seqNext;
	op.swapchain = swapchain;
	op.image = image;
	op.wait = wait;
	op.hostLock = hostLock;
	op.result = result;
	if (hook) {
		op.hook = *hook;
		op.hasHook = true;
	}
	const uint64_t seq = op.seq;
	m_ops.push_back(std::move(op));
	++m_threadStats.enqueued;
	m_opCv.notify_one();
	return seq;
}

void VkCtx::waitOp(uint64_t seq) const
{
	if (!m_threaded || !seq)
		return;
	std::unique_lock<std::mutex> lk(m_opMutex);
	m_doneCv.wait(lk, [&] {
		if (m_busy && m_busySeq <= seq)
			return false;
		for (const SubmitOp& op : m_ops)
			if (op.seq <= seq)
				return false;
		return true;
	});
}

bool VkCtx::waitExecuted(QueueKind kind, uint64_t value) const
{
	if (!m_threaded || !value)
		return value != 0;
	return syncExecuted(queueIndex(kind), value, UINT64_MAX) == Sync::Executed;
}

// vkDeviceWaitIdle under the queue lock (no other submit-thread op may be running: callers drain or stopped it first, or ARE the
// thread - the refusal drain), RETRIED until it proves idle: VK_SUCCESS, or VK_ERROR_DEVICE_LOST (completion for the call that
// returns it). Vulkan also allows OUT_OF_HOST/DEVICE_MEMORY; freeing what the GPU may still read is worse than waiting, so such an
// answer is retried - for at most 2 s of WALL time from the first one (a driver that keeps answering it
// must not hang Reset or teardown). Then the device is terminal and its idle cannot be proven: the process ENDS (integrator
// ruling 13/10, PREREG_T A21 am. 2 (2)) - keeping every object instead would need a guard at each of ~60 destroy/free sites, which
// cannot be shown complete. COST: std::_Exit skips the game's own shutdown and atexit handlers, so a replay being recorded or a
// log still queued in the stderr pump's ring may be cut short; the FATAL line itself goes straight to fd 2 (writeFatal: unbuffered,
// best effort, never held for more than 1 s). A driver call that never RETURNS is not bounded here (no Vulkan call can be).
// Acceptable only because this follows a terminal device failure MoltenVK is not known to produce: MVKDevice::waitIdle returns
// the device's configuration result after waitUntilCompleted (MVKDevice.mm:3850-3858, MVKQueue.mm:152-163).
// `inject` (TEST): answer with it instead of calling the driver.
void VkCtx::idleDevice(VkResult inject)
{
	std::chrono::steady_clock::time_point firstFail;
	for (uint32_t answers = 1;; ++answers) {
		VkResult r = inject;
		if (r == VK_SUCCESS) {
			std::lock_guard<std::mutex> g(m_queueMutex);   // host access to every queue (vkDeviceWaitIdle)
			m_blockedWaits.fetch_add(1);
			r = vkDeviceWaitIdle(m_device);
			m_blockedWaits.fetch_sub(1);
		}
		if (r == VK_SUCCESS)
			return;   // no log here either: the refusal drain publishes its latch right after this
		if (r == VK_ERROR_DEVICE_LOST) {
			// latch the driver loss (completion for the call that returns it) WITHOUT a log line: the synchronous path's device idles
			// were raw, unlogged vkDeviceWaitIdle calls (VkCtx.cpp:800, GxNativeDevice.cpp:119/:1806 ... @33d36f1cc - A21 am. 2 (6))
			m_deviceLost.store(true);
			return;
		}
		// a failing answer: the clock starts NOW and nothing in this loop logs - a stuck stderr must not hold it before or after the
		// deadline; the FATAL line reports the answers
		const auto now = std::chrono::steady_clock::now();
		if (answers == 1) {
			firstFail = now;
			if (inject != VK_SUCCESS) {   // TEST: the child arms' proof that the failing loop was entered (rw3V-code-r5 P2-4)
				// t= is this process's steady_clock at the first failing answer: the arm measures the bound from it, not from when it
				// happens to read the line
				char mark[160];
				const int m = std::snprintf(mark, sizeof(mark), "[GXVK] TEST idle: first injected failing answer - the 2 s bound starts t=%lld\n",
					static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
				if (m > 0)
					writeFatal(mark, static_cast<size_t>(std::min(m, static_cast<int>(sizeof(mark)) - 1)));
			}
		} else if (now - firstFail >= std::chrono::seconds(2) && !g_testMutants.idleUnbounded) {
			char line[320];
			const int n = std::snprintf(line, sizeof(line), "[GXVK] FATAL vkDeviceWaitIdle never proved the device idle: %u answers over "
				"2 s, the last VkResult %d - ending the process rather than free what the GPU may still read or wait forever (t=%lld)\n",
				answers, static_cast<int>(r),
				static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count()));
			if (n > 0)
				writeFatal(line, static_cast<size_t>(std::min(n, static_cast<int>(sizeof(line)) - 1)));
			if (g_testMutants.idleSlowExit)   // TEST plant (rw3V-code-r9 P2-3): the handoff to _Exit overruns its allowance
				std::this_thread::sleep_for(std::chrono::seconds(4));
			if (inject != VK_SUCCESS) {   // TEST: the child arms time the handoff on THIS clock, at the _Exit itself (rw3V-code-r10 P2-1)
				char exitMark[96];
				const int e = std::snprintf(exitMark, sizeof(exitMark), "[GXVK] TEST idle: _Exit t=%lld\n",
					static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now().time_since_epoch()).count()));
				if (e > 0)
					writeFatal(exitMark, static_cast<size_t>(std::min(e, static_cast<int>(sizeof(exitMark)) - 1)));
			}
			std::_Exit(3);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void VkCtx::waitIdle()
{
	if (m_device == VK_NULL_HANDLE)
		return;
	if (m_threaded && !g_testMutants.waitIdleNoDrain) {   // holding no lock the thread needs
		std::unique_lock<std::mutex> lk(m_opMutex);
		++m_drainWaiters;
		m_doneCv.wait(lk, [this] { return m_ops.empty() && !m_busy; });
		--m_drainWaiters;
	}
	idleDevice(g_testMutants.idleResult);
	// idle is proven here: each owner settles the retirements a latch or a loss left, while the pools they free into still exist
	// (rw3V-design-r1 P1-2)
	if (!g_testMutants.latchNoRetire) {
		bool latched;
		{
			std::lock_guard<std::mutex> g(m_opMutex);
			latched = m_latched;
		}
		if (m_deviceLost.load() || latched)
			settleRetirements();   // a driver loss or a latch: nothing still runs, and a refused value never completes
		else
			collect();
	}
}

SubmitRefusal VkCtx::refusal() const
{
	std::lock_guard<std::mutex> g(m_opMutex);
	return m_refusal;
}

SubmitThreadStats VkCtx::threadStats() const
{
	std::lock_guard<std::mutex> g(m_opMutex);
	SubmitThreadStats s = m_threadStats;
	s.waitFastPath = m_waitFastPath.load(std::memory_order_relaxed);
	return s;
}

void VkCtx::testHoldAt(uint64_t value)
{
	std::lock_guard<std::mutex> g(m_opMutex);
	m_holdAt = value;
}

int VkCtx::testDrainWaiters() const
{
	std::lock_guard<std::mutex> g(m_opMutex);
	return m_drainWaiters;
}

int VkCtx::testQuiesceWaiters() const
{
	std::lock_guard<std::mutex> g(m_opMutex);
	return m_quiesceWaiters;
}

void VkCtx::testSet(const std::function<void(TestMutants&)>& set)
{
	std::lock_guard<std::mutex> g(m_opMutex);
	set(g_testMutants);
}

void VkCtx::testRelease()
{
	{
		std::lock_guard<std::mutex> g(m_opMutex);
		m_holdAt = 0;
	}
	m_opCv.notify_all();
}

}} // namespace GXRender::Vk
