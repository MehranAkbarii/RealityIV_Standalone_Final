#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <unordered_map>
#include <mutex>
#include <string>
#include <cstring>
#include <fstream>


// ============================================================
// ReShade external runtime API
// ============================================================

enum class reshade_device_api : unsigned int
{
	vulkan = 0x20000
};

struct reshade_effect_runtime;

using PFN_ReShadeCreateEffectRuntime =
bool (__cdecl*)(
	reshade_device_api,
	void*,
	void*,
	void*,
	const char*,
	reshade_effect_runtime**
	);

using PFN_ReShadeDestroyEffectRuntime =
void (__cdecl*)(
	reshade_effect_runtime*
	);

using PFN_ReShadeUpdateAndPresentEffectRuntime =
void (__cdecl*)(
	reshade_effect_runtime*
	);


// ============================================================
// Globals
// ============================================================

static HMODULE g_module = nullptr;

static HMODULE g_reshade = nullptr;
static HMODULE g_addon = nullptr;

static PFN_ReShadeCreateEffectRuntime
g_create = nullptr;

static PFN_ReShadeDestroyEffectRuntime
g_destroy = nullptr;

static PFN_ReShadeUpdateAndPresentEffectRuntime
g_update = nullptr;


static std::mutex g_mutex;


// ============================================================
// Vulkan state
// ============================================================

struct InstanceState
{
	PFN_vkGetInstanceProcAddr next_gipa{};
};


struct DeviceState
{
	VkInstance instance{};

	PFN_vkGetDeviceProcAddr next_gdpa{};

	PFN_vkDestroyDevice next_destroyDevice{};

	PFN_vkGetDeviceQueue next_getDeviceQueue{};
	PFN_vkGetDeviceQueue2 next_getDeviceQueue2{};

	PFN_vkCreateSwapchainKHR next_createSwapchain{};
	PFN_vkDestroySwapchainKHR next_destroySwapchain{};

	PFN_vkGetSwapchainImagesKHR next_getSwapchainImages{};

	PFN_vkAcquireNextImageKHR next_acquireNextImage{};

	PFN_vkQueuePresentKHR next_queuePresent{};
};


struct RuntimeState
{
	VkDevice device{};
	VkQueue queue{};
	VkSwapchainKHR swapchain{};

	reshade_effect_runtime* runtime{};
};


// ============================================================
// Maps
// ============================================================

static std::unordered_map<
	VkInstance,
	InstanceState
> g_instances;


static std::unordered_map<
	VkPhysicalDevice,
	VkInstance
> g_phys_instances;


static std::unordered_map<
	VkDevice,
	DeviceState
> g_devices;


static std::unordered_map<
	VkQueue,
	VkDevice
> g_queues;


static std::unordered_map<
	VkSwapchainKHR,
	VkDevice
> g_swapchains;


static RuntimeState g_runtime{};


// ============================================================
// Forward declarations
// ============================================================

extern "C"
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr (
	VkInstance instance,
	const char* name
);


extern "C"
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr (
	VkDevice device,
	const char* name
);


extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance (
	const VkInstanceCreateInfo* ci,
	const VkAllocationCallbacks* ac,
	VkInstance* out
);


extern "C"
VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance (
	VkInstance instance,
	const VkAllocationCallbacks* ac
);


extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices (
	VkInstance instance,
	uint32_t* count,
	VkPhysicalDevice* devices
);


extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice (
	VkPhysicalDevice physical,
	const VkDeviceCreateInfo* ci,
	const VkAllocationCallbacks* ac,
	VkDevice* out
);


extern "C"
VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice (
	VkDevice device,
	const VkAllocationCallbacks* ac
);


extern "C"
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue (
	VkDevice device,
	uint32_t family,
	uint32_t index,
	VkQueue* out
);


extern "C"
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue2 (
	VkDevice device,
	const VkDeviceQueueInfo2* info,
	VkQueue* out
);


extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR (
	VkDevice device,
	const VkSwapchainCreateInfoKHR* ci,
	const VkAllocationCallbacks* ac,
	VkSwapchainKHR* out
);


extern "C"
VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR (
	VkDevice device,
	VkSwapchainKHR swapchain,
	const VkAllocationCallbacks* ac
);


extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR (
	VkDevice device,
	VkSwapchainKHR swapchain,
	uint32_t* count,
	VkImage* images
);


extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR (
	VkDevice device,
	VkSwapchainKHR swapchain,
	uint64_t timeout,
	VkSemaphore semaphore,
	VkFence fence,
	uint32_t* imageIndex
);


extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR (
	VkQueue queue,
	const VkPresentInfoKHR* info
);


extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion (
	VkNegotiateLayerInterface* v
);


// ============================================================
// Helpers
// ============================================================

static std::string module_dir ()
{
	char path[MAX_PATH]{};

	GetModuleFileNameA (
		g_module,
		path,
		MAX_PATH
	);

	std::string s (path);

	const auto p =
		s.find_last_of ("\\/");

	if (p == std::string::npos)
		return ".";

	return s.substr (0, p);
}


// ============================================================
// Debug log
// ============================================================

static void debug_log (const char* msg)
{
	OutputDebugStringA ("[RealityIV] ");
	OutputDebugStringA (msg);
	OutputDebugStringA ("\n");
}


// ============================================================
// Vulkan instance function log
// ============================================================

static void log_instance_proc (const char* name)
{
	std::ofstream log (
		"RealityIV_VkInstanceProc.log",
		std::ios::app
	);

	log << name << "\n";
}


// ============================================================
// Vulkan device function log
// ============================================================

static void log_device_proc (const char* name)
{
	std::ofstream log (
		"RealityIV_VkProc.log",
		std::ios::app
	);

	log << name << "\n";
}


// ============================================================
// ReShade log
// ============================================================

static void log_reshade (const char* msg)
{
	std::ofstream log (
		"RealityIV_ReShade.log",
		std::ios::app
	);

	log << msg << "\n";
}


// ============================================================
// ReShade runtime loading
// ============================================================

static bool load_runtime ()
{
	std::ofstream log (
		"RealityIV_ReShade.log",
		std::ios::app
	);

	log << "================================\n";
	log << "load_runtime() called\n";


	if (g_create &&
		g_destroy &&
		g_update &&
		g_addon)
	{
		log << "Runtime already loaded\n";
		return true;
	}


	const std::string dir =
		module_dir ();

	const std::string reshade_path =
		dir + "\\ReShade.dll";


	log << "Layer directory: "
		<< dir
		<< "\n";

	log << "ReShade path: "
		<< reshade_path
		<< "\n";


	// --------------------------------------------------------
	// ReShade.dll
	// --------------------------------------------------------
	SetEnvironmentVariableA (
		"RESHADE_DISABLE_LOADING_CHECK",
		"1"
	);
	if (!g_reshade)
	{
		log << "Loading ReShade.dll...\n";

		g_reshade =
			LoadLibraryA (
				reshade_path.c_str ()
			);
	}


	if (!g_reshade)
	{
		const DWORD error =
			GetLastError ();

		log << "ReShade.dll FAILED\n";
		log << "GetLastError: "
			<< error
			<< "\n";

		return false;
	}


	log << "ReShade.dll LOADED\n";


	// --------------------------------------------------------
	// External runtime exports
	// --------------------------------------------------------

	g_create =
		reinterpret_cast<
		PFN_ReShadeCreateEffectRuntime
		>(
			GetProcAddress (
				g_reshade,
				"ReShadeCreateEffectRuntime"
			)
			);


	g_destroy =
		reinterpret_cast<
		PFN_ReShadeDestroyEffectRuntime
		>(
			GetProcAddress (
				g_reshade,
				"ReShadeDestroyEffectRuntime"
			)
			);


	g_update =
		reinterpret_cast<
		PFN_ReShadeUpdateAndPresentEffectRuntime
		>(
			GetProcAddress (
				g_reshade,
				"ReShadeUpdateAndPresentEffectRuntime"
			)
			);


	log << "ReShadeCreateEffectRuntime: "
		<< (g_create ? "FOUND" : "MISSING")
		<< "\n";


	log << "ReShadeDestroyEffectRuntime: "
		<< (g_destroy ? "FOUND" : "MISSING")
		<< "\n";


	log << "ReShadeUpdateAndPresentEffectRuntime: "
		<< (g_update ? "FOUND" : "MISSING")
		<< "\n";


	if (!g_create ||
		!g_destroy ||
		!g_update)
	{
		log << "ReShade exports FAILED\n";

		FreeLibrary (g_reshade);
		g_reshade = nullptr;

		g_create = nullptr;
		g_destroy = nullptr;
		g_update = nullptr;

		return false;
	}


	log << "ReShade exports OK\n";


	// --------------------------------------------------------
	// RealityFX addon
	// --------------------------------------------------------

	const std::string addon_path =
		dir + "\\RealityFX.addon32";


	log << "RealityFX addon path: "
		<< addon_path
		<< "\n";


	if (!g_addon)
	{
		log << "Loading RealityFX.addon32...\n";

		g_addon =
			LoadLibraryA (
				addon_path.c_str ()
			);
	}


	if (!g_addon)
	{
		const DWORD error =
			GetLastError ();

		log << "RealityFX.addon32 FAILED\n";
		log << "GetLastError: "
			<< error
			<< "\n";

		FreeLibrary (g_reshade);
		g_reshade = nullptr;

		g_create = nullptr;
		g_destroy = nullptr;
		g_update = nullptr;

		return false;
	}


	log << "RealityFX.addon32 LOADED\n";

	log << "load_runtime() SUCCESS\n";

	return true;
}


// ============================================================
// Destroy ReShade runtime
//
// IMPORTANT:
// This function expects g_mutex to already be locked.
// ============================================================

static void destroy_runtime_locked ()
{
	if (g_runtime.runtime &&
		g_destroy)
	{
		log_reshade (
			"Destroying ReShade runtime..."
		);

		g_destroy (
			g_runtime.runtime
		);
	}


	g_runtime = {};


	if (g_addon)
	{
		FreeLibrary (g_addon);
		g_addon = nullptr;
	}


	if (g_reshade)
	{
		FreeLibrary (g_reshade);
		g_reshade = nullptr;
	}


	g_create = nullptr;
	g_destroy = nullptr;
	g_update = nullptr;
}


// ============================================================
// Create ReShade runtime
// ============================================================

static void try_create_runtime (
	VkDevice device,
	VkQueue queue,
	VkSwapchainKHR swapchain)
{
	if (!device ||
		!queue ||
		!swapchain)
	{
		log_reshade (
			"try_create_runtime: invalid Vulkan objects."
		);

		return;
	}


	// --------------------------------------------------------
	// Check whether runtime already exists
	// --------------------------------------------------------

	{
		std::lock_guard<std::mutex> lock (g_mutex);

		if (g_runtime.runtime)
		{
			log_reshade (
				"Runtime already exists."
			);

			return;
		}
	}


	// --------------------------------------------------------
	// Load ReShade WITHOUT holding g_mutex
	// --------------------------------------------------------

	if (!load_runtime ())
		return;

	
	const std::string config =
		module_dir () + "\\ReShade.ini";


	reshade_effect_runtime* runtime =
		nullptr;


	log_reshade (
		"Calling ReShadeCreateEffectRuntime..."
	);


	log_reshade (
		device
		? "VkDevice: VALID"
		: "VkDevice: NULL"
	);

	log_reshade (
		queue
		? "VkQueue: VALID"
		: "VkQueue: NULL"
	);

	log_reshade (
		swapchain
		? "VkSwapchainKHR: VALID"
		: "VkSwapchainKHR: NULL"
	);

	log_reshade (("Config: " + config).c_str ());

	log_reshade ("Calling ReShadeCreateEffectRuntime...");

	const bool result =
		g_create (
			reshade_device_api::vulkan,

			reinterpret_cast<void*>(device),

			reinterpret_cast<void*>(queue),

			reinterpret_cast<void*>(swapchain),

			config.c_str (),

			&runtime
		);


	log_reshade (
		result
		? "ReShadeCreateEffectRuntime returned TRUE"
		: "ReShadeCreateEffectRuntime returned FALSE"
	);


	if (result && runtime)
	{
		std::lock_guard<std::mutex> lock (g_mutex);

		if (!g_runtime.runtime)
		{
			g_runtime.device = device;
			g_runtime.queue = queue;
			g_runtime.swapchain = swapchain;
			g_runtime.runtime = runtime;

			log_reshade (
				"SUCCESS: External Vulkan runtime created."
			);
		} else
		{
			// Another thread created one meanwhile.
			if (g_destroy)
				g_destroy (runtime);
		}
	} else
	{
		log_reshade (
			"FAILED: ReShadeCreateEffectRuntime."
		);
	}
}


// ============================================================
// Vulkan Loader negotiation
// ============================================================

extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion (
	VkNegotiateLayerInterface* v)
{



	if (!v ||
		v->sType !=
		LAYER_NEGOTIATE_INTERFACE_STRUCT)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}


	if (v->loaderLayerInterfaceVersion > 2)
		v->loaderLayerInterfaceVersion = 2;


	v->pfnGetInstanceProcAddr =
		vkGetInstanceProcAddr;


	v->pfnGetDeviceProcAddr =
		vkGetDeviceProcAddr;


	v->pfnGetPhysicalDeviceProcAddr =
		nullptr;


	return VK_SUCCESS;
}


// ============================================================
// vkCreateInstance
// ============================================================

extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance (
	const VkInstanceCreateInfo* ci,
	const VkAllocationCallbacks* ac,
	VkInstance* out)
{


	if (!ci || !out)
		return VK_ERROR_INITIALIZATION_FAILED;


	auto* link =
		reinterpret_cast<VkLayerInstanceCreateInfo*>(
			const_cast<void*>(ci->pNext)
			);


	PFN_vkGetInstanceProcAddr next_gipa =
		nullptr;


	while (link)
	{
		if (link->sType ==
			VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
			link->function ==
			VK_LAYER_LINK_INFO)
		{
			next_gipa =
				link->u.pLayerInfo
				->pfnNextGetInstanceProcAddr;


			link->u.pLayerInfo =
				link->u.pLayerInfo->pNext;


			break;
		}


		link =
			reinterpret_cast<VkLayerInstanceCreateInfo*>(
				const_cast<void*>(link->pNext)
				);
	}


	if (!next_gipa)
		return VK_ERROR_INITIALIZATION_FAILED;


	auto real_create =
		reinterpret_cast<PFN_vkCreateInstance>(
			next_gipa (
				VK_NULL_HANDLE,
				"vkCreateInstance"
			)
			);


	if (!real_create)
		return VK_ERROR_INITIALIZATION_FAILED;


	const VkResult result =
		real_create (
			ci,
			ac,
			out
		);


	if (result != VK_SUCCESS)
		return result;


	{
		std::lock_guard<std::mutex> lock (g_mutex);

		g_instances[*out] = {
			next_gipa
		};
	}


	return VK_SUCCESS;
}


// ============================================================
// vkDestroyInstance
// ============================================================

extern "C"
VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance (
	VkInstance instance,
	const VkAllocationCallbacks* ac)
{
	InstanceState state{};


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_instances.find (instance);


		if (it == g_instances.end ())
			return;


		state = it->second;


		g_instances.erase (it);


		for (auto it2 = g_phys_instances.begin ();
			it2 != g_phys_instances.end ();)
		{
			if (it2->second == instance)
				it2 = g_phys_instances.erase (it2);
			else
				++it2;
		}
	}


	auto fn =
		reinterpret_cast<PFN_vkDestroyInstance>(
			state.next_gipa (
				instance,
				"vkDestroyInstance"
			)
			);


	if (fn)
	{
		fn (
			instance,
			ac
		);
	}
}


// ============================================================
// vkEnumeratePhysicalDevices
// ============================================================

extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkEnumeratePhysicalDevices (
	VkInstance instance,
	uint32_t* count,
	VkPhysicalDevice* devices)
{
	PFN_vkEnumeratePhysicalDevices fn =
		nullptr;


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_instances.find (instance);


		if (it == g_instances.end ())
			return VK_ERROR_INITIALIZATION_FAILED;


		fn =
			reinterpret_cast<
			PFN_vkEnumeratePhysicalDevices
			>(
				it->second.next_gipa (
					instance,
					"vkEnumeratePhysicalDevices"
				)
				);
	}


	if (!fn)
		return VK_ERROR_INITIALIZATION_FAILED;


	const VkResult result =
		fn (
			instance,
			count,
			devices
		);


	if ((result == VK_SUCCESS ||
		result == VK_INCOMPLETE) &&
		devices &&
		count)
	{
		std::lock_guard<std::mutex> lock (g_mutex);


		for (uint32_t i = 0;
			i < *count;
			++i)
		{
			g_phys_instances[
				devices[i]
			] = instance;
		}
	}


	return result;
}


// ============================================================
// vkCreateDevice
// ============================================================

extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice (
	VkPhysicalDevice physical,
	const VkDeviceCreateInfo* ci,
	const VkAllocationCallbacks* ac,
	VkDevice* out)
{



	if (!ci || !out)
		return VK_ERROR_INITIALIZATION_FAILED;


	VkInstance instance =
		VK_NULL_HANDLE;


	PFN_vkGetInstanceProcAddr next_gipa =
		nullptr;


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto p =
			g_phys_instances.find (
				physical
			);


		if (p == g_phys_instances.end ())
			return VK_ERROR_INITIALIZATION_FAILED;


		instance =
			p->second;


		auto i =
			g_instances.find (
				instance
			);


		if (i == g_instances.end ())
			return VK_ERROR_INITIALIZATION_FAILED;


		next_gipa =
			i->second.next_gipa;
	}


	// --------------------------------------------------------
	// Get next device proc address
	// --------------------------------------------------------

	auto* link =
		reinterpret_cast<
		VkLayerDeviceCreateInfo*
		>(
			const_cast<void*>(ci->pNext)
			);


	PFN_vkGetDeviceProcAddr next_gdpa =
		nullptr;


	while (link)
	{
		if (link->sType ==
			VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
			link->function ==
			VK_LAYER_LINK_INFO)
		{
			next_gdpa =
				link->u.pLayerInfo
				->pfnNextGetDeviceProcAddr;


			link->u.pLayerInfo =
				link->u.pLayerInfo->pNext;


			break;
		}


		link =
			reinterpret_cast<
			VkLayerDeviceCreateInfo*
			>(
				const_cast<void*>(
					link->pNext
					)
				);
	}


	if (!next_gdpa)
		return VK_ERROR_INITIALIZATION_FAILED;


	// --------------------------------------------------------
	// Real vkCreateDevice
	// --------------------------------------------------------

	auto real_create =
		reinterpret_cast<
		PFN_vkCreateDevice
		>(
			next_gipa (
				instance,
				"vkCreateDevice"
			)
			);


	if (!real_create)
		return VK_ERROR_INITIALIZATION_FAILED;


	const VkResult result =
		real_create (
			physical,
			ci,
			ac,
			out
		);


	if (result != VK_SUCCESS)
		return result;


	// --------------------------------------------------------
	// Build DeviceState
	// --------------------------------------------------------

	DeviceState state{};


	state.instance =
		instance;


	state.next_gdpa =
		next_gdpa;


	state.next_destroyDevice =
		reinterpret_cast<
		PFN_vkDestroyDevice
		>(
			next_gdpa (
				*out,
				"vkDestroyDevice"
			)
			);


	state.next_getDeviceQueue =
		reinterpret_cast<
		PFN_vkGetDeviceQueue
		>(
			next_gdpa (
				*out,
				"vkGetDeviceQueue"
			)
			);


	state.next_getDeviceQueue2 =
		reinterpret_cast<
		PFN_vkGetDeviceQueue2
		>(
			next_gdpa (
				*out,
				"vkGetDeviceQueue2"
			)
			);


	state.next_createSwapchain =
		reinterpret_cast<
		PFN_vkCreateSwapchainKHR
		>(
			next_gdpa (
				*out,
				"vkCreateSwapchainKHR"
			)
			);


	state.next_destroySwapchain =
		reinterpret_cast<
		PFN_vkDestroySwapchainKHR
		>(
			next_gdpa (
				*out,
				"vkDestroySwapchainKHR"
			)
			);


	state.next_getSwapchainImages =
		reinterpret_cast<
		PFN_vkGetSwapchainImagesKHR
		>(
			next_gdpa (
				*out,
				"vkGetSwapchainImagesKHR"
			)
			);


	state.next_acquireNextImage =
		reinterpret_cast<
		PFN_vkAcquireNextImageKHR
		>(
			next_gdpa (
				*out,
				"vkAcquireNextImageKHR"
			)
			);


	state.next_queuePresent =
		reinterpret_cast<
		PFN_vkQueuePresentKHR
		>(
			next_gdpa (
				*out,
				"vkQueuePresentKHR"
			)
			);


	// --------------------------------------------------------
	// Register device
	// --------------------------------------------------------

	{
		std::lock_guard<std::mutex> lock (g_mutex);

		g_devices[*out] = state;
	}


	return VK_SUCCESS;
}


// ============================================================
// vkGetInstanceProcAddr
// ============================================================

extern "C"
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr (
	VkInstance instance,
	const char* name)
{
	if (!name)
		return nullptr;


	log_instance_proc (name);


	// --------------------------------------------------------
	// Global functions
	// --------------------------------------------------------

	if (!instance)
	{
		if (!std::strcmp (
			name,
			"vkGetInstanceProcAddr"))
		{
			return reinterpret_cast<
				PFN_vkVoidFunction
			>(
				vkGetInstanceProcAddr
				);
		}


		if (!std::strcmp (
			name,
			"vkGetDeviceProcAddr"))
		{
			return reinterpret_cast<
				PFN_vkVoidFunction
			>(
				vkGetDeviceProcAddr
				);
		}


		if (!std::strcmp (
			name,
			"vkNegotiateLoaderLayerInterfaceVersion"))
		{
			return reinterpret_cast<
				PFN_vkVoidFunction
			>(
				vkNegotiateLoaderLayerInterfaceVersion
				);
		}


		if (!std::strcmp (
			name,
			"vkCreateInstance"))
		{
			return reinterpret_cast<
				PFN_vkVoidFunction
			>(
				vkCreateInstance
				);
		}


		return nullptr;
	}


	// --------------------------------------------------------
	// vkGetDeviceProcAddr must always be available
	// --------------------------------------------------------

	if (!std::strcmp (
		name,
		"vkGetDeviceProcAddr"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkGetDeviceProcAddr
			);
	}


	// --------------------------------------------------------
	// Instance functions owned by our layer
	// --------------------------------------------------------

	if (!std::strcmp (
		name,
		"vkDestroyInstance"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkDestroyInstance
			);
	}


	if (!std::strcmp (
		name,
		"vkEnumeratePhysicalDevices"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkEnumeratePhysicalDevices
			);
	}


	if (!std::strcmp (
		name,
		"vkCreateDevice"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkCreateDevice
			);
	}


	// --------------------------------------------------------
	// Pass everything else down
	// --------------------------------------------------------

	PFN_vkGetInstanceProcAddr next_gipa =
		nullptr;


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_instances.find (instance);


		if (it == g_instances.end ())
			return nullptr;


		next_gipa =
			it->second.next_gipa;
	}


	return next_gipa (
		instance,
		name
	);
}


// ============================================================
// vkGetDeviceProcAddr
//
// IMPORTANT:
// Hooked functions are returned BEFORE g_devices lookup.
// ============================================================

extern "C"
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr (
	VkDevice device,
	const char* name)
{
	if (!name)
		return nullptr;


	log_device_proc (name);


	// --------------------------------------------------------
	// Device functions intercepted by RealityIV
	// --------------------------------------------------------

	if (!std::strcmp (
		name,
		"vkGetDeviceProcAddr"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkGetDeviceProcAddr
			);
	}


	if (!std::strcmp (
		name,
		"vkGetDeviceQueue"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkGetDeviceQueue
			);
	}


	if (!std::strcmp (
		name,
		"vkGetDeviceQueue2"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkGetDeviceQueue2
			);
	}


	if (!std::strcmp (
		name,
		"vkCreateSwapchainKHR"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkCreateSwapchainKHR
			);
	}


	if (!std::strcmp (
		name,
		"vkDestroySwapchainKHR"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkDestroySwapchainKHR
			);
	}


	if (!std::strcmp (
		name,
		"vkGetSwapchainImagesKHR"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkGetSwapchainImagesKHR
			);
	}


	if (!std::strcmp (
		name,
		"vkAcquireNextImageKHR"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkAcquireNextImageKHR
			);
	}


	if (!std::strcmp (
		name,
		"vkQueuePresentKHR"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkQueuePresentKHR
			);
	}


	if (!std::strcmp (
		name,
		"vkDestroyDevice"))
	{
		return reinterpret_cast<
			PFN_vkVoidFunction
		>(
			vkDestroyDevice
			);
	}


	// --------------------------------------------------------
	// Everything else goes to the next layer/driver
	// --------------------------------------------------------

	PFN_vkGetDeviceProcAddr next_gdpa =
		nullptr;


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_devices.find (device);


		if (it == g_devices.end ())
			return nullptr;


		next_gdpa =
			it->second.next_gdpa;
	}


	return next_gdpa (
		device,
		name
	);
}


// ============================================================
// vkGetDeviceQueue
// ============================================================

extern "C"
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue (
	VkDevice device,
	uint32_t family,
	uint32_t index,
	VkQueue* out)
{
	DeviceState state{};


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_devices.find (device);


		if (it == g_devices.end ())
			return;


		state =
			it->second;
	}


	if (!state.next_getDeviceQueue)
		return;


	state.next_getDeviceQueue (
		device,
		family,
		index,
		out
	);


	if (out && *out)
	{
		std::lock_guard<std::mutex> lock (g_mutex);

		g_queues[*out] =
			device;
	}
}


// ============================================================
// vkGetDeviceQueue2
// ============================================================

extern "C"
VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue2 (
	VkDevice device,
	const VkDeviceQueueInfo2* info,
	VkQueue* out)
{
	DeviceState state{};


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_devices.find (device);


		if (it == g_devices.end ())
			return;


		state =
			it->second;
	}


	if (!state.next_getDeviceQueue2)
		return;


	state.next_getDeviceQueue2 (
		device,
		info,
		out
	);


	if (out && *out)
	{
		std::lock_guard<std::mutex> lock (g_mutex);

		g_queues[*out] =
			device;
	}
}


// ============================================================
// vkCreateSwapchainKHR
// ============================================================

extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkCreateSwapchainKHR (
	VkDevice device,
	const VkSwapchainCreateInfoKHR* ci,
	const VkAllocationCallbacks* ac,
	VkSwapchainKHR* out)
{
	DeviceState state{};


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_devices.find (device);


		if (it == g_devices.end ())
			return VK_ERROR_INITIALIZATION_FAILED;


		state =
			it->second;
	}


	if (!state.next_createSwapchain)
		return VK_ERROR_EXTENSION_NOT_PRESENT;


	const VkResult result =
		state.next_createSwapchain (
			device,
			ci,
			ac,
			out
		);


	if (result == VK_SUCCESS &&
		out &&
		*out)
	{
		std::lock_guard<std::mutex> lock (g_mutex);

		g_swapchains[*out] =
			device;
	}


	return result;
}


// ============================================================
// vkDestroySwapchainKHR
// ============================================================

extern "C"
VKAPI_ATTR void VKAPI_CALL
vkDestroySwapchainKHR (
	VkDevice device,
	VkSwapchainKHR swapchain,
	const VkAllocationCallbacks* ac)
{
	DeviceState state{};


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_devices.find (device);


		if (it == g_devices.end ())
			return;


		state =
			it->second;


		g_swapchains.erase (
			swapchain
		);
	}


	// Runtime must not be destroyed while holding g_mutex.

	bool destroy_runtime =
		false;


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		if (g_runtime.device == device &&
			g_runtime.swapchain == swapchain)
		{
			destroy_runtime = true;
		}
	}


	if (destroy_runtime)
	{
		std::lock_guard<std::mutex> lock (g_mutex);

		destroy_runtime_locked ();
	}


	if (state.next_destroySwapchain)
	{
		state.next_destroySwapchain (
			device,
			swapchain,
			ac
		);
	}
}


// ============================================================
// vkGetSwapchainImagesKHR
// ============================================================

extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkGetSwapchainImagesKHR (
	VkDevice device,
	VkSwapchainKHR swapchain,
	uint32_t* count,
	VkImage* images)
{
	DeviceState state{};


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_devices.find (device);


		if (it == g_devices.end ())
			return VK_ERROR_INITIALIZATION_FAILED;


		state =
			it->second;
	}


	if (!state.next_getSwapchainImages)
		return VK_ERROR_EXTENSION_NOT_PRESENT;


	return state.next_getSwapchainImages (
		device,
		swapchain,
		count,
		images
	);
}


// ============================================================
// vkAcquireNextImageKHR
// ============================================================

extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkAcquireNextImageKHR (
	VkDevice device,
	VkSwapchainKHR swapchain,
	uint64_t timeout,
	VkSemaphore semaphore,
	VkFence fence,
	uint32_t* imageIndex)
{
	DeviceState state{};


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_devices.find (device);


		if (it == g_devices.end ())
			return VK_ERROR_INITIALIZATION_FAILED;


		state =
			it->second;
	}


	if (!state.next_acquireNextImage)
		return VK_ERROR_EXTENSION_NOT_PRESENT;


	return state.next_acquireNextImage (
		device,
		swapchain,
		timeout,
		semaphore,
		fence,
		imageIndex
	);
}


// ============================================================
// vkQueuePresentKHR
// ============================================================

extern "C"
VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR (
	VkQueue queue,
	const VkPresentInfoKHR* info)
{
	static bool first_present =
		false;


	if (!first_present)
	{
		first_present = true;

		log_reshade (
			"================================"
		);

		log_reshade (
			"RealityIV vkQueuePresentKHR reached."
		);


	}


	if (!info ||
		!info->swapchainCount ||
		!info->pSwapchains)
	{
		return VK_ERROR_INITIALIZATION_FAILED;
	}


	const VkSwapchainKHR swapchain =
		info->pSwapchains[0];


	VkDevice device =
		VK_NULL_HANDLE;


	PFN_vkQueuePresentKHR present =
		nullptr;


	// --------------------------------------------------------
	// Resolve queue -> device
	// --------------------------------------------------------

	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto q =
			g_queues.find (queue);


		if (q != g_queues.end ())
		{
			device =
				q->second;
		}


		if (!device)
		{
			auto s =
				g_swapchains.find (
					swapchain
				);


			if (s != g_swapchains.end ())
			{
				device =
					s->second;
			}
		}


		if (device)
		{
			auto d =
				g_devices.find (device);


			if (d != g_devices.end ())
			{
				present =
					d->second.next_queuePresent;
			}
		}
	}


	if (!device ||
		!present)
	{
		log_reshade (
			"FAILED: Could not resolve device/present."
		);

		return VK_ERROR_DEVICE_LOST;
	}


	// --------------------------------------------------------
	// Create ReShade runtime
	// --------------------------------------------------------

	bool runtime_exists =
		false;


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		runtime_exists =
			g_runtime.runtime != nullptr;
	}


	if (!runtime_exists)
	{



		try_create_runtime (
			device,
			queue,
			swapchain
		);
	}


	// --------------------------------------------------------
	// Get runtime pointer
	// --------------------------------------------------------

	reshade_effect_runtime* runtime =
		nullptr;


	PFN_ReShadeUpdateAndPresentEffectRuntime update =
		nullptr;


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		if (g_runtime.device == device &&
			g_runtime.queue == queue &&
			g_runtime.swapchain == swapchain)
		{
			runtime =
				g_runtime.runtime;


			update =
				g_update;
		}
	}


	// --------------------------------------------------------
	// Let ReShade process this frame
	// --------------------------------------------------------

	if (runtime &&
		update)
	{
		log_reshade (
			"Calling ReShadeUpdateAndPresentEffectRuntime..."
		);


		update (runtime);


		log_reshade (
			"ReShade update completed."
		);
	}


	// --------------------------------------------------------
	// Finally call real Vulkan Present
	// --------------------------------------------------------

	return present (
		queue,
		info
	);
}


// ============================================================
// vkDestroyDevice
// ============================================================

extern "C"
VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice (
	VkDevice device,
	const VkAllocationCallbacks* ac)
{
	DeviceState state{};


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		auto it =
			g_devices.find (device);


		if (it == g_devices.end ())
			return;


		state =
			it->second;
	}


	// --------------------------------------------------------
	// Destroy ReShade runtime
	// --------------------------------------------------------

	bool has_runtime =
		false;


	{
		std::lock_guard<std::mutex> lock (g_mutex);


		has_runtime =
			(g_runtime.device == device &&
				g_runtime.runtime != nullptr);
	}


	if (has_runtime)
	{
		std::lock_guard<std::mutex> lock (g_mutex);

		destroy_runtime_locked ();
	}


	// --------------------------------------------------------
	// Destroy real device
	// --------------------------------------------------------

	if (state.next_destroyDevice)
	{
		state.next_destroyDevice (
			device,
			ac
		);
	}


	// --------------------------------------------------------
	// Cleanup
	// --------------------------------------------------------

	{
		std::lock_guard<std::mutex> lock (g_mutex);


		g_devices.erase (device);


		for (auto it = g_queues.begin ();
			it != g_queues.end ();)
		{
			if (it->second == device)
				it = g_queues.erase (it);
			else
				++it;
		}


		for (auto it = g_swapchains.begin ();
			it != g_swapchains.end ();)
		{
			if (it->second == device)
				it = g_swapchains.erase (it);
			else
				++it;
		}
	}
}


// ============================================================
// DLL Entry
// ============================================================

BOOL APIENTRY
DllMain (
	HMODULE h,
	DWORD reason,
	LPVOID)
{
	if (reason ==
		DLL_PROCESS_ATTACH)
	{
		g_module =
			h;


		DisableThreadLibraryCalls (
			h
		);


	}


	return TRUE;
}