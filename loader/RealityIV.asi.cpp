// RealityIV.asi
//
// ReShade has no public "create runtime" export. On Vulkan it only works as a
// real Vulkan layer that the Vulkan loader itself loads, so that ReShade sees
// vkCreateInstance / vkCreateDevice / vkCreateSwapchainKHR / vkQueuePresentKHR.
//
// This ASI therefore does only one thing, early at game start (before DXVK
// creates its VkInstance): it makes ReShade's own layer visible to the Vulkan
// loader for THIS PROCESS ONLY, without running ReShade's installer.
//
//   mode "registry" (default): generates a layer manifest (.json) that points
//       to ReShade.dll, adds it under HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers
//       and removes it again when the game exits.
//   mode "env": generates the same manifest and sets VK_ADD_IMPLICIT_LAYER_PATH
//       (needs Vulkan loader 1.3.234 or newer). Nothing is written to the registry.
//
// RealityFX.addon32 is NOT loaded here. ReShade loads add-ons by itself from
// its own folder (it must be an add-on-enabled ReShade build).
//
// Settings: RealityIV_Loader.ini next to this .asi (optional).
// Log:      RealityIV_Loader.log next to this .asi.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>

namespace
{
	const wchar_t kLayerName[]   = L"VK_LAYER_reshade";
	const wchar_t kRegistryKey[] = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";

	HMODULE      g_module       = nullptr;
	HANDLE       g_log          = INVALID_HANDLE_VALUE;
	bool         g_registry_set = false;
	std::wstring g_registry_value;

	// --------------------------------------------------------
	// Small helpers (Win32 only, safe to use from DllMain)
	// --------------------------------------------------------

	std::string to_utf8 (const std::wstring& w)
	{
		if (w.empty ())
			return std::string ();

		const int n = WideCharToMultiByte (
			CP_UTF8, 0, w.c_str (), static_cast<int>(w.size ()),
			nullptr, 0, nullptr, nullptr);

		if (n <= 0)
			return std::string ();

		std::string s (static_cast<size_t>(n), '\0');

		WideCharToMultiByte (
			CP_UTF8, 0, w.c_str (), static_cast<int>(w.size ()),
			&s[0], n, nullptr, nullptr);

		return s;
	}

	void log_line (const std::wstring& text)
	{
		if (g_log == INVALID_HANDLE_VALUE)
			return;

		std::string line = to_utf8 (text);
		line += "\r\n";

		DWORD written = 0;
		WriteFile (g_log, line.data (), static_cast<DWORD>(line.size ()), &written, nullptr);
	}

	void log_error (const wchar_t* what, DWORD code)
	{
		log_line (std::wstring (what) + L" (error " + std::to_wstring (code) + L")");
	}

	std::wstring module_path (HMODULE m)
	{
		wchar_t buf[2048]{};
		const DWORD n = GetModuleFileNameW (m, buf, 2048);
		return std::wstring (buf, n);
	}

	std::wstring dir_of (const std::wstring& path)
	{
		const auto p = path.find_last_of (L"\\/");
		return p == std::wstring::npos ? std::wstring (L".") : path.substr (0, p);
	}

	bool file_exists (const std::wstring& path)
	{
		const DWORD a = GetFileAttributesW (path.c_str ());
		return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	}

	void ensure_dir (const std::wstring& path)
	{
		CreateDirectoryW (path.c_str (), nullptr); // ERROR_ALREADY_EXISTS is fine
	}

	bool write_file (const std::wstring& path, const std::string& data)
	{
		const HANDLE f = CreateFileW (
			path.c_str (), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
			CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

		if (f == INVALID_HANDLE_VALUE)
		{
			log_error ((L"Cannot write " + path).c_str (), GetLastError ());
			return false;
		}

		DWORD written = 0;
		const BOOL ok = WriteFile (f, data.data (), static_cast<DWORD>(data.size ()), &written, nullptr);
		CloseHandle (f);

		return ok && written == data.size ();
	}

	std::string json_escape (const std::string& s)
	{
		std::string out;
		out.reserve (s.size () + 8);

		for (const char c : s)
		{
			if (c == '\\' || c == '"')
				out += '\\';
			out += c;
		}

		return out;
	}

	// --------------------------------------------------------
	// Settings
	// --------------------------------------------------------

	struct Config
	{
		bool registry_mode         = true;  // false = "env"
		bool disable_graphics_hook = false; // RESHADE_DISABLE_GRAPHICS_HOOK
		bool disable_loading_check = true;  // RESHADE_DISABLE_LOADING_CHECK
	};

	Config read_config (const std::wstring& ini)
	{
		Config cfg;

		wchar_t mode[32]{};
		GetPrivateProfileStringW (L"RealityIV", L"LayerMode", L"registry", mode, 32, ini.c_str ());

		cfg.registry_mode = lstrcmpiW (mode, L"env") != 0;

		cfg.disable_graphics_hook =
			GetPrivateProfileIntW (L"RealityIV", L"DisableGraphicsHook", 0, ini.c_str ()) != 0;

		cfg.disable_loading_check =
			GetPrivateProfileIntW (L"RealityIV", L"DisableLoadingCheck", 1, ini.c_str ()) != 0;

		return cfg;
	}

	// --------------------------------------------------------
	// Locate ReShade.dll (next to the .asi first, then next to the .exe)
	// --------------------------------------------------------

	std::wstring find_reshade (const std::wstring& asi_dir, const std::wstring& exe_dir)
	{
		const std::wstring dirs[] = { asi_dir, exe_dir };
		const wchar_t* names[]    = { L"ReShade.dll", L"ReShade32.dll" };

		for (const auto& d : dirs)
			for (const wchar_t* n : names)
			{
				const std::wstring candidate = d + L"\\" + n;

				if (file_exists (candidate))
					return candidate;
			}

		return std::wstring ();
	}

	// --------------------------------------------------------
	// Layer manifest (same layout as ReShade32.json from the official setup)
	// --------------------------------------------------------

	bool write_manifest (const std::wstring& dir, const std::wstring& reshade_dll, std::wstring& json_path)
	{
		const std::string dll = json_escape (to_utf8 (reshade_dll));

		std::string json;
		json += "{\n";
		json += "\t\"file_format_version\": \"1.1.2\",\n";
		json += "\t\"layer\": {\n";
		json += "\t\t\"name\": \"" + to_utf8 (kLayerName) + "\",\n";
		json += "\t\t\"type\": \"GLOBAL\",\n";
		json += "\t\t\"library_path\": \"" + dll + "\",\n";
		json += "\t\t\"api_version\": \"1.3.0\",\n";
		json += "\t\t\"implementation_version\": \"1\",\n";
		json += "\t\t\"description\": \"ReShade (enabled by RealityIV.asi)\",\n";
		json += "\t\t\"disable_environment\": {\n";
		json += "\t\t\t\"DISABLE_VK_LAYER_reshade_1\": \"1\"\n";
		json += "\t\t}\n";
		json += "\t}\n";
		json += "}\n";

		const std::wstring path = dir + L"\\ReShade32_Layer.json";

		if (!write_file (path, json))
			return false;

		json_path = path;
		return true;
	}

	// --------------------------------------------------------
	// Registry mode
	// --------------------------------------------------------

	bool registry_add (const std::wstring& json_path)
	{
		HKEY key = nullptr;

		LONG r = RegCreateKeyExW (
			HKEY_CURRENT_USER, kRegistryKey, 0, nullptr, 0,
			KEY_SET_VALUE, nullptr, &key, nullptr);

		if (r != ERROR_SUCCESS)
		{
			log_error (L"RegCreateKeyExW failed", static_cast<DWORD>(r));
			return false;
		}

		const DWORD zero = 0; // 0 = layer enabled

		r = RegSetValueExW (
			key, json_path.c_str (), 0, REG_DWORD,
			reinterpret_cast<const BYTE*>(&zero), sizeof (zero));

		RegCloseKey (key);

		if (r != ERROR_SUCCESS)
		{
			log_error (L"RegSetValueExW failed", static_cast<DWORD>(r));
			return false;
		}

		g_registry_value = json_path;
		g_registry_set   = true;
		return true;
	}

	void registry_remove ()
	{
		if (!g_registry_set)
			return;

		HKEY key = nullptr;

		if (RegOpenKeyExW (HKEY_CURRENT_USER, kRegistryKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
		{
			RegDeleteValueW (key, g_registry_value.c_str ());
			RegCloseKey (key);
		}

		g_registry_set = false;
	}

	// --------------------------------------------------------
	// Environment mode
	// --------------------------------------------------------

	bool env_add_layer_path (const std::wstring& dir)
	{
		std::wstring value = dir;

		wchar_t existing[4096]{};
		const DWORD n = GetEnvironmentVariableW (L"VK_ADD_IMPLICIT_LAYER_PATH", existing, 4096);

		if (n > 0 && n < 4096)
			value = std::wstring (existing, n) + L";" + dir;

		if (!SetEnvironmentVariableW (L"VK_ADD_IMPLICIT_LAYER_PATH", value.c_str ()))
		{
			log_error (L"SetEnvironmentVariableW(VK_ADD_IMPLICIT_LAYER_PATH) failed", GetLastError ());
			return false;
		}

		return true;
	}

	// --------------------------------------------------------
	// Main setup
	// --------------------------------------------------------

	void setup ()
	{
		const std::wstring asi_path = module_path (g_module);
		const std::wstring asi_dir  = dir_of (asi_path);
		const std::wstring exe_dir  = dir_of (module_path (nullptr));

		g_log = CreateFileW (
			(asi_dir + L"\\RealityIV_Loader.log").c_str (), GENERIC_WRITE, FILE_SHARE_READ,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

		log_line (L"RealityIV.asi loaded from: " + asi_path);

		const Config cfg = read_config (asi_dir + L"\\RealityIV_Loader.ini");

		log_line (std::wstring (L"LayerMode: ") + (cfg.registry_mode ? L"registry" : L"env"));

		// ReShade environment switches (inherited by ReShade when the Vulkan loader loads it).
		if (cfg.disable_loading_check)
			SetEnvironmentVariableW (L"RESHADE_DISABLE_LOADING_CHECK", L"1");

		if (cfg.disable_graphics_hook)
			SetEnvironmentVariableW (L"RESHADE_DISABLE_GRAPHICS_HOOK", L"1");

		const std::wstring reshade_dll = find_reshade (asi_dir, exe_dir);

		if (reshade_dll.empty ())
		{
			log_line (L"ERROR: ReShade.dll (or ReShade32.dll) not found next to the .asi or the .exe. Nothing registered.");
			return;
		}

		log_line (L"ReShade DLL: " + reshade_dll);

		// Manifest folder: <asi dir>\RealityIV_Layer, else %LOCALAPPDATA%\RealityIV\Layer.
		std::wstring manifest_dir  = asi_dir + L"\\RealityIV_Layer";
		std::wstring manifest_path;

		ensure_dir (manifest_dir);

		if (!write_manifest (manifest_dir, reshade_dll, manifest_path))
		{
			wchar_t local[MAX_PATH]{};
			const DWORD n = GetEnvironmentVariableW (L"LOCALAPPDATA", local, MAX_PATH);

			if (n == 0 || n >= MAX_PATH)
			{
				log_line (L"ERROR: cannot write the layer manifest and %LOCALAPPDATA% is unavailable.");
				return;
			}

			const std::wstring base = std::wstring (local, n) + L"\\RealityIV";
			ensure_dir (base);

			manifest_dir = base + L"\\Layer";
			ensure_dir (manifest_dir);

			if (!write_manifest (manifest_dir, reshade_dll, manifest_path))
			{
				log_line (L"ERROR: cannot write the layer manifest anywhere.");
				return;
			}
		}

		log_line (L"Layer manifest: " + manifest_path);

		if (cfg.registry_mode)
		{
			if (registry_add (manifest_path))
				log_line (L"Registered layer in HKCU (removed again on exit).");
			else
				log_line (L"ERROR: registry registration failed. Try LayerMode=env in RealityIV_Loader.ini.");
		}
		else
		{
			if (env_add_layer_path (manifest_dir))
				log_line (L"Set VK_ADD_IMPLICIT_LAYER_PATH (needs Vulkan loader 1.3.234+).");
			else
				log_line (L"ERROR: could not set VK_ADD_IMPLICIT_LAYER_PATH.");
		}

		log_line (L"Done. ReShade is now loaded by the Vulkan loader when DXVK creates its instance.");
	}

	void cleanup ()
	{
		registry_remove ();

		if (g_log != INVALID_HANDLE_VALUE)
		{
			CloseHandle (g_log);
			g_log = INVALID_HANDLE_VALUE;
		}
	}
}

BOOL APIENTRY DllMain (HMODULE hModule, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = hModule;

		DisableThreadLibraryCalls (hModule);

		setup ();
	}
	else if (reason == DLL_PROCESS_DETACH)
	{
		cleanup ();
	}

	return TRUE;
}
