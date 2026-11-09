// GeneralsX @feature B · dlss-fg-1 14/10/2026 census_selftest: proves VK_LAYER_GX_queue_census sees what it claims, in both
// directions (PREREG A39a). One device, one queue, empty submits; no window, no swapchain.
//   control  the main thread submits alone           -> the queue shows ONE thread, overlaps 0
//   overlap  a 2nd thread submits while the main one is held INSIDE the layer (gxCensusTestHoldNext; the held call has not
//            reached the driver yet, so there is no real driver race)   -> two threads on the queue, overlaps >= 1
//   plant    GX_VK_CENSUS_PLANT=3                    -> one "census-plant" operation on the queue
// Exit 0 = the census reported exactly what the mode does. census_verdict.py on the same summary must PASS control and FAIL the
// other two (C3, C1).
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>
#endif
#include <vulkan/vulkan.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string exeDir()
{
#if defined(_WIN32)
	char p[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, p, MAX_PATH);
#else
	char p[4096] = {};
	uint32_t n = sizeof(p);
	_NSGetExecutablePath(p, &n);
#endif
	std::string s(p);
	const size_t slash = s.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : s.substr(0, slash);
}

void setEnv(const char* k, const char* v)
{
#if defined(_WIN32)
	_putenv_s(k, v ? v : "");
	SetEnvironmentVariableA(k, v);	// the Vulkan loader reads the PROCESS environment (GetEnvironmentVariable), not the CRT's copy
#else
	if (v)
		setenv(k, v, 1);
	else
		unsetenv(k);
#endif
}

void* openLib(const char* const* names)
{
	for (const char* const* n = names; *n; ++n) {
#if defined(_WIN32)
		if (HMODULE m = LoadLibraryA(*n))
			return m;
#else
		if (void* m = dlopen(*n, RTLD_NOW | RTLD_LOCAL))
			return m;
#endif
	}
	return nullptr;
}

void* sym(void* lib, const char* name)
{
#if defined(_WIN32)
	return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
	return dlsym(lib, name);
#endif
}

void* layerModule()
{
#if defined(_WIN32)
	return GetModuleHandleA("VkLayer_gx_queue_census.dll");
#else
	const std::string p = exeDir() + "/libVkLayer_gx_queue_census.dylib";
	return dlopen(p.c_str(), RTLD_NOW | RTLD_NOLOAD);
#endif
}

int fail(const char* what)
{
	std::fprintf(stderr, "SELFTEST ERROR %s\n", what);
	return 2;
}

} // namespace

int main(int argc, char** argv)
{
	const std::string mode = argc > 1 ? argv[1] : "control";
	if (mode != "control" && mode != "overlap" && mode != "plant")
		return fail("usage: census_selftest control|overlap|plant");
	const std::string dir = exeDir();
	const std::string out = dir + "/census-" + mode + ".txt";
	std::remove(out.c_str());
	setEnv("VK_ADD_LAYER_PATH", dir.c_str());
	setEnv("VK_INSTANCE_LAYERS", "VK_LAYER_GX_queue_census");
	setEnv("GX_VK_CENSUS", out.c_str());
	setEnv("GX_VK_CENSUS_PLANT", mode == "plant" ? "3" : nullptr);

	const char* loaders[] = {
#if defined(_WIN32)
		"vulkan-1.dll",
#else
		"libvulkan.1.dylib", "/opt/homebrew/lib/libvulkan.1.dylib", "/usr/local/lib/libvulkan.1.dylib",
#endif
		nullptr };
	void* loader = openLib(loaders);
	if (!loader)
		return fail("no Vulkan loader");
	const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(sym(loader, "vkGetInstanceProcAddr"));
	const auto enumInstExt = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
	const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));

	uint32_t n = 0;
	enumInstExt(nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> iext(n);
	enumInstExt(nullptr, &n, iext.data());
	std::vector<const char*> instExts;
	VkInstanceCreateFlags instFlags = 0;
	for (const VkExtensionProperties& e : iext)
		if (std::strcmp(e.extensionName, "VK_KHR_portability_enumeration") == 0) {	// MoltenVK
			instExts.push_back("VK_KHR_portability_enumeration");
			instFlags |= 0x00000001;	// VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
		}
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.apiVersion = VK_API_VERSION_1_2;
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.flags = instFlags;
	ici.pApplicationInfo = &app;
	ici.enabledExtensionCount = uint32_t(instExts.size());
	ici.ppEnabledExtensionNames = instExts.data();
	VkInstance inst = VK_NULL_HANDLE;
	if (createInstance(&ici, nullptr, &inst) != VK_SUCCESS)
		return fail("vkCreateInstance");
	const auto enumPhys = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
	const auto qfp = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties"));
	const auto enumDevExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(inst, "vkEnumerateDeviceExtensionProperties"));
	const auto createDevice = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst, "vkCreateDevice"));
	const auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr"));
	const auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
	n = 0;
	enumPhys(inst, &n, nullptr);
	if (!n)
		return fail("no physical device");
	std::vector<VkPhysicalDevice> phys(n);
	enumPhys(inst, &n, phys.data());
	const VkPhysicalDevice pd = phys[0];
	qfp(pd, &n, nullptr);
	std::vector<VkQueueFamilyProperties> fams(n);
	qfp(pd, &n, fams.data());
	uint32_t family = UINT32_MAX;
	for (uint32_t i = 0; i < n && family == UINT32_MAX; ++i)
		if (fams[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
			family = i;
	if (family == UINT32_MAX)
		return fail("no graphics/compute family");
	enumDevExt(pd, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> dext(n);
	enumDevExt(pd, nullptr, &n, dext.data());
	std::vector<const char*> devExts;
	for (const VkExtensionProperties& e : dext)
		if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0)
			devExts.push_back("VK_KHR_portability_subset");
	const float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
	qci.queueFamilyIndex = family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = uint32_t(devExts.size());
	dci.ppEnabledExtensionNames = devExts.data();
	VkDevice dev = VK_NULL_HANDLE;
	if (createDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS)
		return fail("vkCreateDevice");
	const auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(dev, "vkGetDeviceQueue"));
	const auto submit = reinterpret_cast<PFN_vkQueueSubmit>(gdpa(dev, "vkQueueSubmit"));
	const auto waitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(gdpa(dev, "vkDeviceWaitIdle"));
	const auto destroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(gdpa(dev, "vkDestroyDevice"));
	VkQueue q = VK_NULL_HANDLE;
	getQueue(dev, family, 0, &q);

	void* layer = layerModule();
	if (!layer)
		return fail("the census layer is not loaded (manifest next to the exe? VK_ADD_LAYER_PATH honoured?)");
	const auto hold = reinterpret_cast<void (*)(unsigned)>(sym(layer, "gxCensusTestHoldNext"));
	const auto flush = reinterpret_cast<void (*)()>(sym(layer, "gxCensusFlush"));
	if (!hold || !flush)
		return fail("census exports missing");

	for (int i = 0; i < 6; ++i)
		submit(q, 0, nullptr, VK_NULL_HANDLE);
	if (mode == "overlap") {
		hold(400);	// the next queue operation (ours, just below) waits inside the layer
		std::thread other([&] {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			submit(q, 0, nullptr, VK_NULL_HANDLE);
		});
		submit(q, 0, nullptr, VK_NULL_HANDLE);
		other.join();
	}
	if (mode == "plant")
		std::this_thread::sleep_for(std::chrono::milliseconds(500));	// the plant thread is detached
	waitIdle(dev);
	flush();

	// Read the summary back: the queue's threads and modules, and the overlap count.
	std::ifstream f(out);
	if (!f)
		return fail("no summary written");
	std::set<std::string> tids, modules;
	unsigned long long overlaps = ~0ull;
	std::string line;
	while (std::getline(f, line)) {
		std::printf("  %s\n", line.c_str());
		if (line.rfind("census v1", 0) == 0) {
			const size_t p = line.find("overlaps=");
			overlaps = p == std::string::npos ? ~0ull : std::strtoull(line.c_str() + p + 9, nullptr, 10);
		}
		if (line.rfind("op q=", 0) == 0) {
			std::istringstream ss(line);
			std::string tok;
			while (ss >> tok) {
				if (tok.rfind("tid=", 0) == 0)
					tids.insert(tok.substr(4));
				if (tok.rfind("module=", 0) == 0)
					modules.insert(tok.substr(7));
			}
		}
	}
	bool ok = false;
	if (mode == "control")
		ok = tids.size() == 1 && overlaps == 0 && !modules.count("census-plant");
	else if (mode == "overlap")
		ok = tids.size() == 2 && overlaps >= 1 && overlaps != ~0ull;
	else
		ok = modules.count("census-plant") == 1;
	std::printf("SELFTEST %s %s threads=%zu overlaps=%llu plant=%d\n", mode.c_str(), ok ? "PASS" : "FAIL", tids.size(), overlaps,
		int(modules.count("census-plant")));
	destroyDevice(dev, nullptr);
	destroyInstance(inst, nullptr);
	return ok ? 0 : 1;
}
