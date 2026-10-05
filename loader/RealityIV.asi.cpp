
#include <Windows.h>
#include <string>

static HMODULE g_module = nullptr;

static std::string module_dir()
{
    char path[MAX_PATH]{};

    GetModuleFileNameA(
        g_module,
        path,
        MAX_PATH
    );

    std::string s(path);

    const auto p = s.find_last_of("\\/");

    return p == std::string::npos
        ? "."
        : s.substr(0, p);
}

BOOL APIENTRY DllMain(
    HMODULE hModule,
    DWORD reason,
    LPVOID
)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = hModule;

        DisableThreadLibraryCalls(hModule);

        const std::string dir = module_dir();

        // Tell Vulkan Loader where RealityIV's layer manifest is.
        SetEnvironmentVariableA(
            "VK_LAYER_PATH",
            dir.c_str()
        );

        // Enable RealityIV Vulkan layer.
        SetEnvironmentVariableA(
            "VK_INSTANCE_LAYERS",
            "VK_LAYER_RealityIV"
        );




        HMODULE vulkanlib = GetModuleHandleA("vulkan-1.dll");

   


        HMODULE vulkan = LoadLibraryA("vulkan-1.dll");



        // Load the RealityIV Vulkan layer.
        HMODULE layer = LoadLibraryA(
            "RealityIV_VkLayer32.dll"
        );

   
    }

    return TRUE;
}