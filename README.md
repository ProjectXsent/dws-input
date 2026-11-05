# DWS Input

Three small proxy DLLs that present an **SDL3 gamepad** to old games through the
APIs they already use, all configured by **one file**, `dws_input.ini`.

| DLL | Legacy API it covers |
|---|---|
| `winmm.dll` | winmm joystick (`joyGetPosEx`, `joyGetDevCaps`, ...). Everything else in winmm is forwarded to the real DLL. |
| `dinput.dll` | DirectInput 1-7 (`DirectInputCreateA/W/Ex`). Keyboard/mouse go to the real DLL. |
| `dinput8.dll` | DirectInput 8 (`DirectInput8Create`, A and W interfaces). Keyboard/mouse go to the real DLL. |

Use whichever one the game loads (usually just one). Copy that DLL, `SDL3.dll` and
`dws_input.ini` into the game folder.

## Layout

```
CMakeLists.txt        builds all three DLLs
CMakePresets.json     x64 + x86 presets
dws_input.ini         the single config (replaces winmm_sdl.ini, dinput_sdl.ini, dinput8_sdl.ini)
src/core/dws_core.h   shared: INI reader (per-DLL overrides), module dir, log naming
src/core/dws_src.h    shared: control-source names/parser used by winmm + dinput
src/winmm/            winmm.cpp + winmm.def
src/dinput/           dinput.cpp + dinput.def
src/dinput8/          dinput8.cpp + dinput8.def
```

## Build

Requires Visual Studio 2022 (MSVC) and CMake 3.21+. SDL3 is used if CMake can find it
(for example via vcpkg, or `-DSDL3_DIR=...`); otherwise it is downloaded and built
automatically (`-DDWS_FETCH_SDL3=OFF` to disable, `-DDWS_SDL3_TAG=...` to pick a version).

Build the architecture that matches the **game** (most games in this era are 32-bit):

```
cmake --preset vs2022-x86
cmake --build --preset x86-release
cmake --install build/x86 --config Release      # -> dist/x86/

cmake --preset vs2022-x64
cmake --build --preset x64-release
cmake --install build/x64 --config Release      # -> dist/x64/
```

Each `dist/<arch>/` folder contains `winmm.dll`, `dinput.dll`, `dinput8.dll`,
`SDL3.dll` and `dws_input.ini`. The MSVC runtime is linked statically (no
redistributable needed); use `-DDWS_STATIC_CRT=OFF` to turn that off.

## Configuration

Every key is optional. Lookup order for any key is: the DLL's own override section
first, then the shared section, then the built-in default.

| Section | Used by |
|---|---|
| `[General]` `[Input]` `[Buttons]` `[Rumble]` | all DLLs (`[Rumble]` has no effect on winmm) |
| `[Axes.WinMM]` | winmm.dll, axes X Y Z R U V |
| `[Axes.DInput]` | dinput.dll **and** dinput8.dll, axes X Y Z Rx Ry Rz |
| `[WinMM]` `[DInput]` `[DInput8]` | overrides for that one DLL; any key from any section can go here |

Example: `dws_input.ini` ships with `DPadAsStick=None` in `[Input]` and
`DPadAsStick=Left` in `[WinMM]`, which reproduces the old defaults of each DLL.

Logs (when `Log=1`) are one per DLL, next to the DLL: `dws_input_winmm.log`,
`dws_input_dinput.log`, `dws_input_dinput8.log`. The effective settings are echoed
at the top of each.

## Credits

Built on [SDL3](https://libsdl.org/) (zlib license).