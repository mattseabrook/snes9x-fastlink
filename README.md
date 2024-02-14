*Snes9x - Portable Super Nintendo Entertainment System (TM) emulator*

This is a fork of the official source code repository for the Snes9x project, cleaned up for 2024 and building as `C++ 20` in `VS 2022`. Importantly, a new feature has been implemented that adds a socket server on a dedicated thread that surfaces the entire state RAM of the emulated SNES. This behaves like a standard WWW Rest API on the `localhost:9000/`

**Table-of-Contents**

- [SNES9X Proprietary SNES Memory Architecture](#snes9x-proprietary-snes-memory-architecture)
  - [CMemory struct](#cmemory-struct)
- [TODO](#todo)
- [CHANGELOG](#changelog)
  - [v0.1 - 2024-02-09](#v01---2024-02-09)
    - [Lifted the project from VS 2017 to VS 2022](#lifted-the-project-from-vs-2017-to-vs-2022)
    - [Switching from DirectX SDK to Windows 10 SDK Dependency](#switching-from-directx-sdk-to-windows-10-sdk-dependency)
    - [XAudio2](#xaudio2)
    - [DirectDraw Deprecation](#directdraw-deprecation)
    - [fmtlib - External Dependency Warnings](#fmtlib---external-dependency-warnings)
    - [Const Correctness](#const-correctness)
    - [Warning C4244](#warning-c4244)

# SNES9X Proprietary SNES Memory Architecture

## CMemory struct

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

- Document the 
# CHANGELOG

## v0.1 - 2024-02-09

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
```
delete mode 100644 win32/CDirectDraw.cpp
 delete mode 100644 win32/CDirectDraw.h
 delete mode 100644 win32/ddraw/ddraw_x64.lib
 delete mode 100644 win32/ddraw/ddraw_x86.lib
 ```
  
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