*Snes9x - Portable Super Nintendo Entertainment System (TM) emulator*

This is a fork of the official source code repository for the Snes9x project, cleaned up for 2024 and building as `C++ 20` in `VS 2022`. Importantly, a new feature has been implemented that adds a socket server on a dedicated thread that surfaces the entire state RAM of the emulated SNES. This behaves like a standard WWW Rest API on the `localhost:9000/`

**Table-of-Contents**

- [Developers](#developers)
  - [SNES9X Proprietary SNES Memory Architecture](#snes9x-proprietary-snes-memory-architecture)
    - [CMemory struct](#cmemory-struct)
- [TODO](#todo)
- [CHANGELOG](#changelog)
  - [v0.2 -](#v02--)
  - [v0.1 - 2024-02-09](#v01---2024-02-09)
    - [Lifted the project from VS 2017 to VS 2022](#lifted-the-project-from-vs-2017-to-vs-2022)
    - [Switching from DirectX SDK to Windows 10 SDK Dependency](#switching-from-directx-sdk-to-windows-10-sdk-dependency)
    - [XAudio2](#xaudio2)
    - [DirectDraw Deprecation](#directdraw-deprecation)
    - [fmtlib - External Dependency Warnings](#fmtlib---external-dependency-warnings)
    - [Const Correctness](#const-correctness)
    - [Warning C4244](#warning-c4244)
- [Modified Files](#modified-files)
  - [memmap.h](#memmaph)
  - [memserve.cpp](#memservecpp)
  - [memserve.h](#memserveh)
  - [prototype.html](#prototypehtml)
  - [snes9x.h](#snes9xh)
  - [win32/render.cpp](#win32rendercpp)
  - [win32/rsrc/resource.h](#win32rsrcresourceh)
  - [win32/rsrc/snes9x.rc](#win32rsrcsnes9xrc)
  - [win32/snes9xw.vcxproj](#win32snes9xwvcxproj)
  - [win32/snes9xw.vcxproj.filters](#win32snes9xwvcxprojfilters)
  - [win32/wconfig.cpp](#win32wconfigcpp)
  - [win32/win32\_display.cpp](#win32win32_displaycpp)
  - [win32/wsnes9x.cpp](#win32wsnes9xcpp)
  - [win32/wsnes9x.h](#win32wsnes9xh)
  - [win32/wlanguage.h](#win32wlanguageh)

# Developers

## SNES9X Proprietary SNES Memory Architecture

### CMemory struct

**Files**: `memmap.h` , `memmap.c`

| Name                  | Type                 | Description                                                                              |
| --------------------- | -------------------- | ---------------------------------------------------------------------------------------- |
| RAM                   | uint8                | The working / state memory of the Super Nintendo itself                                  |
| ROMStorage            | std::vector<uint8_t> | This is the Byte Array that contains the ROM file of the Super Nintendo cartridge (game) |
| ROM                   | uint8 *              | This is the pointer to the above, as the size will be dynamic                            |
| SRAMStorage           | std::vector<uint8_t> | This is the Byte Array that contains the Save State / Battery backed-up storage          |
| SRAM                  | uint8 *              | This is t he pointer to the above, as the size will be dynamic                           |
| SRAM_SIZE             | const size_t         | Default: `0x80000` but set to the exact size of the SRAM per game                        |
| VRAM                  | uint8                | Default: `0x10000`. This is the Video / PPU RAM of the Super Nintendo                    |
| ROMFilename           | std::string          | The name of the Super Nintendo ROM file (game name) as it is on the user's hard drive    |
| ROMName               | char                 | Length set to a define `ROM_NAME_LEN` - game name as it is in the ROM file               |
| ROMId                 | char                 | Length set to `5`                                                                        |
| CompanyId             | int32                |                                                                                          |
| ROMRegion             | uint8                |                                                                                          |
| ROMSpeed              | uint8                |                                                                                          |
| ROMType               | uint8                |                                                                                          |
| ROMSize               | uint8                |                                                                                          |
| ROMChecksum           | uint32               |                                                                                          |
| ROMComplementChecksum | uint32               |                                                                                          |
| ROMCRC32              | uint32               |                                                                                          |
| ROMSHA256             | unsigned char        | 32                                                                                       |
| ROMFramesPerSecond    | int32                |                                                                                          |
| HiROM                 | bool8                |                                                                                          |
| LoROM                 | bool8                |                                                                                          |
| SRAMSize              | uint8                |                                                                                          |
| SRAMMask              | uint32               |                                                                                          |
| CalculatedSize        | uint32               |                                                                                          |
| CalculatedChecksum    | uint32               |                                                                                          |

# TODO

- Add-in the Nathaniel Lomann JSON header-only implementation
- Entrypoint for Socket Server is in `wsnes9x.cpp` on Line `3917` (maybe)

# CHANGELOG

## v0.2 -

- Added `bool8 MemoryServe;` and `int MemServePort;` to the `SSettings` struct definition in `snes9x.h`
- Added the following code to `wconfig.cpp` to add support for our new feature into `snes9x.conf`:
```cpp
#define	CATEGORY "MemoryServe"
	AddBool("Enabled", Settings.MemoryServe, false);
	AddInt("Port", Settings.MemServePort, 9000);
#undef CATEGORY
```
- Emulation menu (`wsnes9x.cpp`):
```cpp
mii.fState = Settings.MemoryServe ? MFS_CHECKED : MFS_UNCHECKED;
SetMenuItemInfo(GUI.hMenu, ID_EMULATION_MEMSERVE, FALSE, &mii);

WM_COMMAND:
		case ID_EMULATION_MEMSERVE:
			MessageBox(hWnd, TEXT("All your base are belong to us."), TEXT("Notice"), MB_OK);
			break;
```


## v0.1 - 2024-02-09

Efforts in `v0.1` were related to getting the project to compile in `VS 2022`, targeting `C++ 20` and the `Windows 10 SDK`, as well as removing deprecated features, and updating libraries.

### Lifted the project from VS 2017 to VS 2022

- Windows SDK Version changed from `8.1` to `10.0 Latest`
- Platform Toolset changed from `VS 2017 v141_xp` to `VS 2022 v143`
- C++ Language Standard changed from `C++ 17` to `C++ 20`

### Switching from DirectX SDK to Windows 10 SDK Dependency

Review my updated compiling instructions, no DirectX Run-times/SDK downloads are required any longer. Here's what's actually changed:

- Deleted `dxerr.h` and `dxerr.cpp`
- Removed all instances of `#include "dxerr.h` through the entire code-base
- Updated Direct3D initialization and rendering logic to be compatible with the Windows 10 SDK
- To avoid overhauling from D3D9 to a newer version, we are now just linking against `d3d9.lib`

### XAudio2

x

### DirectDraw Deprecation

Completely removed `DirectDraw` from the code base. Here's exactly what changed:

- Deleted `ddraw` folder.
- Deleted `CDirectDraw.h` and `CDirectDraw.cpp`
- Removed `TEXT("DirectDraw")` from the `const TCHAR* driverNames[]` in `win32_display.cpp`
- Removed the pragma command to link with `ddraw*.lib`
- Removed `DIRECTDRAW = 0,` from the `OutputMethod` enum in `wsnes9x.h` and set `DIRECT3D` to `0`
- Removed checks like this: `GUI.outputMethod!=DIRECTDRAW`
  
### fmtlib - External Dependency Warnings

```
warning C4996: 'stdext::checked_array_iterator<T *>::value_type': warning STL4043: stdext::checked_array_iterator, stdext::unchecked_array_iterator, and related factory functions are non-Standard extensions and will be removed in the future. std::span (since C++20) and gsl::span can be used instead. You can define _SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING or _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS to suppress this warning.
```

- These warnings were solved by overwriting the `src` and `include` folders in this repository with the `src` and `include` folders from the latest version of `fmt`
  
### Const Correctness

| Files Modified |
| -------------- |
| wsnes9x.cpp    |

Several instances across the project of errors `C2440` and `C2664`, including variable declarations and function arguments, were updated to conform to the C++ 20 requirement `/Zc:strictStrings`. Compilation errors arose when non-const pointers were initialized with string literals. This was primarily observed in `wsnes9x.cpp` and other files where `TCHAR*` variables were assigned string literals directly. This practice is deemed unsafe in C++ 20 as it allows modification of string literals, which are stored in read-only memory sections of an application.

### Warning C4244

x

| Files Modified          |
| ----------------------- |
| snes9x_imgui.cpp        |
| COpenGL.cpp             |
| vulkan_shader_chain.cpp |

# Modified Files

## memmap.h
Minor formatting tweak to the `CMemory` struct definition.

## memserve.cpp
Implements an HTTP server that streams SNES RAM as binary data.

## memserve.h
Header for the MemServe feature.

## prototype.html
Web page that fetches RAM from `localhost:9000` and draws it on a canvas.

## snes9x.h
Added `MemoryServe` and `MemServePort` to the `SSettings` struct.

## win32/render.cpp
Removed a `DIRECTDRAW` check to always clear the change log.

## win32/rsrc/resource.h
Introduces `ID_EMULATION_MEMSERVE` for the new menu item.

## win32/rsrc/snes9x.rc
Adds the "SNES Memory REST API" entry under the Emulation menu.

## win32/snes9xw.vcxproj
Links against `ws2_32.lib` and includes the MemServe source files.

## win32/snes9xw.vcxproj.filters
Places MemServe files under a "FastLink" filter for organization.

## win32/wconfig.cpp
Registers MemServe configuration options in `snes9x.conf`.

## win32/win32_display.cpp
Removed the DirectDraw driver name from the output method list.

## win32/wsnes9x.cpp
Spawns and joins the MemServe thread and handles the menu toggle.

## win32/wsnes9x.h
Adjusted `OutputMethod` enum to drop `DIRECTDRAW`.

## win32/wlanguage.h
Updated the window title and disclaimer text for "Snes9x-FastLink".
