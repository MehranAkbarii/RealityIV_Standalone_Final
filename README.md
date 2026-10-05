# RealityIV Standalone

Architecture:
GTA IV -> DXVK -> Vulkan loader -> VK_LAYER_RealityIV -> local ReShade external runtime -> RealityFX.addon32

Outputs:
- `bin\\Win32\\Release\\RealityIV.asi`
- `bin\\Win32\\Release\\RealityIV_VkLayer32.dll`

## Runtime folder
Put these beside `GTAIV.exe`:
- `RealityIV.asi`
- `RealityIV_VkLayer32.dll`
- `RealityIV_VkLayer.json`
- `ReShade.dll`
- `RealityFX.addon32`
- the original RealityIV `ReShade.ini`, `RealityIV_Preset.ini`, `RealityFX.ini`, `reshade-shaders` and `RealityFX` asset folders.

