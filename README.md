# RealityIV Standalone Asi (Launcher)

Architecture:

    GTA IV -> DXVK -> Vulkan loader -> ReShade (its own Vulkan layer) -> RealityFX.addon32

`RealityIV.asi` does one job: before DXVK creates its `VkInstance`, it makes ReShade's own
Vulkan layer visible to the Vulkan loader for this process only (no ReShade installer).

## Why the old design failed

The old `VK_LAYER_RealityIV` called `ReShadeCreateEffectRuntime` in `ReShade.dll`. Official ReShade
has no such export (its exports are add-on API only: `ReShadeRegisterAddon`, `ReShadeRegisterEvent`, ...).
On Vulkan, ReShade only creates its runtime when the Vulkan loader loads it as a layer and its hooks
see `vkCreateInstance`, `vkCreateDevice`, `vkCreateSwapchainKHR` and `vkQueuePresentKHR`.
The custom layer (`RealityIV_VkLayer32.dll`, `RealityIV_VkLayer.json`) is therefore removed.

## Build

Open `RealityIV_Standalone.sln`, configuration `Release | x86`. Output:

- `bin\Win32\Release\RealityIV.asi`

(Static CRT, no Vulkan SDK needed.)

## Runtime folder

Put these beside `GTAIV.exe`:

- `RealityIV.asi`
- `ReShade32.dll`
- `ReShade32.json`
- `RealityIV_Loader.ini`
- the original RealityIV `ReShade.ini`, `RealityIV_Preset.ini`, `RealityFX.ini`, `reshade-shaders`, `RealityFX.addon32` and `RealityFX` asset folders.

On start the ASI writes `RealityIV_Layer\ReShade32_Layer.json` (absolute path to your ReShade.dll)
and registers it. ReShade loads `RealityFX.addon32` on its own from its folder.

## Check that it works

1. `RealityIV_Loader.log` should end with "Done. ReShade is now loaded by the Vulkan loader ...".
2. `ReShade.log` appears and shows the Vulkan device/swapchain being initialized, and the add-on being loaded.
3. The ReShade overlay opens in game (Home key).

## Troubleshooting
- Do not run the game as administrator; the Vulkan loader may ignore per-user layer settings then.
  start and close the game once normally.
