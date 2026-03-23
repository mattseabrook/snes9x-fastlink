`snes9x-fastlink` is a Windows-focused, low-latency fork of Snes9x with custom memory streaming features and a Linux-hosted cross-compilation toolchain.

**Table-of-Contents**
- [Introduction](#introduction)
  - [What this fork is optimizing for](#what-this-fork-is-optimizing-for)
- [Snes9x 1.63 vs snes9x-fastlink 1.666](#snes9x-163-vs-snes9x-fastlink-1666)
- [Technical Change Inventory](#technical-change-inventory)
  - [1) FastLink memory integration](#1-fastlink-memory-integration)
  - [2) Win32 frame pipeline and pacing work](#2-win32-frame-pipeline-and-pacing-work)
  - [3) Direct3D11 path latency tuning](#3-direct3d11-path-latency-tuning)
  - [4) Vulkan path queue-depth behavior](#4-vulkan-path-queue-depth-behavior)
  - [5) Audio backend architecture (Win32)](#5-audio-backend-architecture-win32)
  - [6) Win32 input latency path (Phase D)](#6-win32-input-latency-path-phase-d)
  - [7) Toolchain/build modernization in this fork](#7-toolchainbuild-modernization-in-this-fork)
- [Per-file change matrix](#per-file-change-matrix)
  - [Build prerequisites (Linux host)](#build-prerequisites-linux-host)
    - [Ubuntu / Debian one-liner (default)](#ubuntu--debian-one-liner-default)
    - [Arch Linux one-liner](#arch-linux-one-liner)
  - [One-time setup: Windows SDK via xwin](#one-time-setup-windows-sdk-via-xwin)
  - [Build walkthrough (Linux -\> Windows)](#build-walkthrough-linux---windows)
    - [1) Clone with submodules](#1-clone-with-submodules)
    - [2) Build dependency libraries (if missing)](#2-build-dependency-libraries-if-missing)
    - [3) Build emulator executable](#3-build-emulator-executable)
    - [4) Clean artifacts](#4-clean-artifacts)
  - [`build.sh` deep dive (custom build pipeline)](#buildsh-deep-dive-custom-build-pipeline)
    - [Commands](#commands)
    - [High-level pipeline](#high-level-pipeline)
    - [Why this build system is custom](#why-this-build-system-is-custom)
    - [Environment variables supported](#environment-variables-supported)
    - [Dependency model in `build.sh`](#dependency-model-in-buildsh)
  - [`build_windows_libs.sh` deep dive](#build_windows_libssh-deep-dive)
    - [What it does](#what-it-does)
    - [Command interface](#command-interface)
    - [Relationship to `build.sh`](#relationship-to-buildsh)
  - [Troubleshooting](#troubleshooting)
    - [`WINSDK_BASE not found`](#winsdk_base-not-found)
    - [Missing zlib/libpng `.lib`](#missing-zliblibpng-lib)
    - [Submodule-related compile failures](#submodule-related-compile-failures)
    - [Tool missing (`clang-cl`, `llvm-rc`, etc.)](#tool-missing-clang-cl-llvm-rc-etc)
  - [Output artifacts](#output-artifacts)
- [Changelog](#changelog)

# Introduction

The objective is not emulation speed hacks or speculative simulation; the objective is to minimize avoidable host-side latency while preserving canonical SNES timing semantics.

We treat the runtime as a queueing-and-control system, where total user-perceived latency is the sum of queue depths and service times across the pipeline:

$$
L_{\text{total}} = L_{\text{input}} + L_{\text{emu}} + L_{\text{handoff}} + L_{\text{render-queue}} + L_{\text{scanout}} + L_{\text{audio-path}}
$$

The fork strategy is to reduce every controllable queue term without altering emulated clock semantics:

$$
\Delta L_{\text{total}} = \sum_i \Delta L_i, \quad \text{with} \quad \Delta L_i \le 0 \text{ from host-side engineering only}
$$

For dynamic audio stability, we model buffer occupancy around a target midpoint and apply bounded control on resampling ratio. Let $B$ be total bytes and $F$ be free bytes. The occupancy error term is:

$$
e = B - 2F
$$

Baseline dynamic-rate control sets multiplier as:

$$
m_{\text{target}} = 1 + \frac{\lambda e}{1000B}
$$

with $\lambda = \text{DynamicRateLimit}$. In this fork, multiplier transitions are smoothed and per-update clamped to suppress abrupt audible changes:

$$
m_t = m_{t-1} + \operatorname{clamp}\left(\alpha (m_{\text{target}} - m_{t-1}), -s, s\right),
\quad \alpha = 0.16, \ s = 0.0035
$$

This yields a stable low-pass control response with bounded slew: short jitter is attenuated and long-horizon drift is still corrected.

For frame pacing, render queue depth is constrained at the graphics API level (D3D11 swapchain + minimal buffering + explicit present synchronization), reducing queue-induced phase lag between emulation completion and presentation submit.

## What this fork is optimizing for

1. **Low and stable end-to-end latency** without changing SNES emulation pacing logic.
2. **Windows runtime reliability** for streaming/live usage.
3. **Deterministic Linux-hosted cross-builds** that emit a Windows executable.
4. **FastLink memory tooling** (`memserve`, `memshare`) for external integrations.

# Snes9x 1.63 vs snes9x-fastlink 1.666

The table below is limited to values directly observable in source (no synthetic benchmark numbers).

| Metric                                  | Snes9x 1.63                                             | snes9x-fastlink 1.666                                                               |
| --------------------------------------- | ------------------------------------------------------- | ----------------------------------------------------------------------------------- |
| Win32 audio backend selection           | XAudio2 + WaveOut selectable in `win32/win32_sound.cpp` | WASAPI-only runtime path (`S9xSoundOutput = &S9xWasapi`) in `win32/win32_sound.cpp` |
| Sound callback topology                 | `S9xSoundCallback` directly calls `ProcessSound()`      | Dedicated worker thread, event-driven wake, backend process event integration       |
| Sound init buffer argument              | `S9xInitSound(25)`                                      | Profile-driven (`S9xInitSound(32)` default; safe/Bluetooth path uses 64ms)         |
| D3D backend API                         | Direct3D9-era fixed-function pipeline                    | Direct3D11 shader-based fullscreen pipeline                                          |
| D3D present/buffering policy            | Device-era backbuffer toggles                            | Minimal swapchain buffering with explicit D3D11 present control                      |
| D3D device creation path                | Classic `Direct3DCreate9` and `CreateDevice`            | `D3D11CreateDeviceAndSwapChain` (hardware, then WARP fallback)                      |
| Dynamic-rate smoothing coefficient      | No EMA smoothing constant in Win32 path                 | EMA + bounded step (`alpha = 0.16`, max step `0.0035`) in `src/core/apu/apu.cpp`   |
| WASAPI shared-mode minimum buffer floor | Not applicable (no WASAPI backend in 1.63 Win32 path)   | `minSharedBuffer = 160000` (16ms)                                                   |
| Audio process-event interface           | No `GetProcessEvent()` contract                         | `IS9xSoundOutput::GetProcessEvent()` added and wired by WASAPI backend              |
| Input latency fast path                 | Legacy polling/inline-only host path                    | Phase D fast HID ingest worker path (always enabled in this fork)                   |
| Input-to-photon absolute ms             | Not defined in source alone                             | Not defined in source alone (requires external measurement harness)                 |

# Technical Change Inventory

This section documents the notable fork-specific behavior and infrastructure compared to a stock upstream checkout.

## 1) FastLink memory integration

- Added runtime memory serving and memory sharing side channels.
- Introduced settings toggles and config keys for memory serving.
- Added UI hooks/menu wiring in Win32 frontend for memory service controls.

Primary files:

- `memserve.cpp`, `memserve.h`
- `memshare.cpp`, `memshare.h`
- `snes9x.h`
- `win32/wconfig.cpp`
- `win32/wsnes9x.cpp`
- `win32/rsrc/resource.h`, `win32/rsrc/snes9x.rc`

## 2) Win32 frame pipeline and pacing work

- Moved to an emulation-thread + render-thread handoff model in Win32 frontend flow.
- Added frame handoff buffering with event-driven wakeup for render consumption.
- Added latest-frame handoff mode in active low-latency pipeline.
- Added dialog-focus safety path to avoid buffer contention artifacts while Win32 config dialogs are active.
- Reduced handoff copy cost by storing packed frame rows (`width * 2`) instead of source pitch when possible.
- Added optional latency tracing points from input sample -> emu -> publish -> present submit.

Primary files:

- `win32/wsnes9x.cpp`
- `win32/win32_display.cpp`, `win32/win32_display.h`

## 3) Direct3D11 path latency tuning

- Replaced Direct3D9/Cg path with a Direct3D11 renderer while keeping the same Win32 output slot (`DIRECT3D`).
- Uses dynamic texture upload + minimal fullscreen shader pipeline with runtime-filtered sampling.
- Preserves low-latency present behavior through explicit D3D11 swapchain/device management.

Primary files:

- `win32/CDirect3D.cpp`
- `win32/CDirect3D.h`

## 4) Vulkan path queue-depth behavior

- Vulkan swapchain is requested with minimal image count for lower frame queueing pressure.

Primary files:

- `src/vulkan/vulkan_context.cpp`
- `win32/CVulkan.cpp`

## 5) Audio backend architecture (Win32)

- Win32 path is centered around WASAPI output (`CWasapi`).
- Worker thread is event-driven with backend process-event integration.
- Uses MMCSS `Pro Audio` class when available.
- Shared-mode output is default and non-invasive to other applications; optional exclusive test mode remains available via environment variable.
- Dynamic rate control remains available and uses bounded smoothing to reduce abrupt ratio shifts.
- `ProcessSound` writes only available real samples (no synthetic pad content in tail regions).
- Sound init headroom is profile-driven (`32ms` default, `64ms` safe/Bluetooth profile).

Primary files:

- `win32/CWasapi.cpp`, `win32/CWasapi.h`
- `win32/win32_sound.cpp`
- `win32/IS9xSoundOutput.h`
- `src/core/apu/apu.cpp`

## 6) Win32 input latency path (Phase D)

- Added a fast HID ingest worker thread for reduced input scheduling jitter.
- Fast input path is always enabled in this fork's runtime policy.
- If explicit VID:PID targets are omitted, all Raw HID gamepads are auto-routed through the fast path.
- Includes fail-safe fallback to inline decode if the worker is unavailable.

Primary files:

- `win32/win32.cpp`
- `win32/wconfig.cpp`
- `win32/wsnes9x.h`

## 7) Toolchain/build modernization in this fork

- Added Linux-hosted Windows cross-build entrypoint (`build.sh`).
- Added dependency bootstrap builder (`build_windows_libs.sh`) for Windows static libs.
- Build now extracts compile units from Visual Studio project files at build time (no hand-maintained source list).
- Integrates bundled shader tool sources in-tree for compile/link.
- Compiler language mode now uses `clang-cl /std:c++latest` (C++23-class feature set) for Win32 builds.
- MinGW-gated Win32 frontend units now target modern Windows API baseline (`_WIN32_WINNT=0x0A00`, `_WIN32_IE=0x0A00`).
- Legacy Win32 config migration now promotes `Display\\Win::Direct3D:D3DShader` to `Display\\Win::Direct3D11:Shader` during preload.
- Obsolete DirectDraw-era `LocalVidMem` setting/state/resource artifacts were removed from active Win32 configuration surface.
- Source tree is normalized under `src/` for `common`, `filter`, `jma`, `unzip`, and `vulkan` modules.
- Legacy helper `scripts/` folder was removed; dependency helper script now lives at repository root (`build_windows_libs.sh`).

Primary files:

- `build.sh`
- `build_windows_libs.sh`
- `.gitmodules`
- `external/*`

---

# Per-file change matrix

This matrix is intended as a maintenance-grade inventory: what changed, what was removed, and why that improves efficiency.

| File                               | Change type                     | Added / Changed behavior                                                                                                        | Removed behavior / path                                                                                                    | Efficiency impact                                                 |
| ---------------------------------- | ------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------- |
| `win32/win32_sound.cpp`            | Modified                        | WASAPI-only output path; worker thread with event-driven wake and MMCSS `Pro Audio`; profile-driven `S9xInitSound(32/64)`       | Runtime backend switch logic (WaveOut/XAudio2 dispatch) removed from active path; direct callback-only mixing path removed | Lower callback jitter, fewer missed deadlines, cleaner scheduling |
| `win32/CWasapi.cpp`                | Added/Modified backend          | Shared-mode WASAPI default with event callback; 16ms shared floor; optional exclusive test mode with fallback to shared          | Tail gap-fill/repeat concealment path removed; forced-exclusive startup path removed                                         | Stable low-latency audio without disrupting other apps            |
| `win32/CWasapi.h`                  | Modified                        | Process-event exposure and lean state                                                                                           | Last-sample concealment state fields removed                                                                               | Less mutable state, fewer branch paths                            |
| `win32/IS9xSoundOutput.h`          | Modified interface              | Added optional `GetProcessEvent()` backend contract                                                                             | N/A                                                                                                                        | Enables true event-driven audio scheduling                        |
| `src/core/apu/apu.cpp`             | Modified                        | EMA-smoothed host output dynamic-rate multiplier (`alpha = 0.16`) with bounded per-update step (`max_step = 0.0035`)            | Immediate step assignment of host dynamic multiplier removed                                                               | Faster convergence with click/pop suppression                     |
| `src/platform/win32/CDirect3D.cpp` | Modified                        | Full Direct3D11 rendering path (device/swapchain, shaders, texture upload, present, sync query)                               | Legacy Direct3D9Ex/classic D3D9 rendering code removed                                                                    | Keeps fallback renderer current and maintainable                  |
| `src/platform/win32/CDirect3D.h`   | Modified                        | Interface/state updated for D3D11 objects                                                                                       | D3D9-specific device/surface/shader state removed                                                                         | Aligns fallback renderer with modern Windows graphics stack       |
| `src/vulkan/vulkan_context.cpp`    | Modified                        | Swapchain desired latency request reduced to 1 image (`create(1, ...)`)                                                         | Prior higher requested image count path removed from this fork’s context implementation                                    | Lower queued-frame depth in Vulkan path                           |
| `win32/win32_display.cpp`          | Modified                        | Frame handoff with packed-copy rows (`width*2`), latest-frame mode, event-driven render wake, optional latency trace points      | Full-pitch always-copy handoff path removed in active flow                                                                 | Lower copy bandwidth + reduced handoff latency                    |
| `win32/wsnes9x.cpp`                | Modified                        | Emu thread + render consumption loop with blocking frame-ready wait; key low-latency toggles are forced on at runtime           | Legacy single-loop immediate render behavior removed from active flow                                                      | Better phase decoupling and lower jitter                          |
| `win32/win32.cpp`                  | Modified                        | Raw HID fast ingest worker path (Phase D), auto-target-all mode when target list is empty, and inline safety fallback            | Inline-only HID processing as sole path                                                                                    | Lower input jitter with robust fallback semantics                 |
| `win32/wconfig.cpp`                | Modified                        | Updated config semantics for dynamic-rate controls and Win32 sound/input behavior                                                 | Outdated descriptions aligned to old backend assumptions removed                                                           | Reduces operator misconfiguration risk                            |
| `build.sh`                         | Added/maintained custom system  | Automated Linux->Windows cross build, dynamic source discovery with guarded excludes, dependency bootstrap, robust RC fallback  | Hand-maintained source manifest model removed                                                                              | Reproducible builds, lower maintenance overhead                   |
| `build_windows_libs.sh`            | Added/maintained helper system  | SDK layout auto-detection, dependency library bootstrap, per-lib build targets                                                  | Manual repetitive per-library setup workflow removed                                                                       | Faster environment provisioning and recovery                      |
| `win32/rsrc/*` + menu wiring files | Modified (FastLink feature era) | Memory service controls and integration hooks                                                                                   | N/A                                                                                                                        | Operational observability + tooling support                       |

### Emulation-integrity note (APU)

The `src/core/apu/apu.cpp` EMA change is in host playback control (`S9xUpdateDynamicRate` -> `UpdatePlaybackRate` -> `resampler.time_ratio(...)`).

It does **not** alter SPC700/DSP emulation behavior in `src/core/apu/bapu/*` (instruction execution, DSP opcode semantics, BRR decode pipeline, APU RAM/register modeling).

So yes: this is a legal/accurate change under the “do not alter true SNES pacing/emulation” rule; it only smooths host-side rate corrections used to feed the PC audio output device.

## Build prerequisites (Linux host)

The build host is Linux. Output target is Windows (`x86_64-pc-windows-msvc`).

### Ubuntu / Debian one-liner (default)

```bash
sudo apt update && sudo apt install -y \
  build-essential clang lld llvm cmake make ninja-build \
  python3 git curl pkg-config unzip zip tar xz-utils ca-certificates \
  rustup
```

Then enable Rust toolchain in your shell and install `xwin`:

```bash
source "$HOME/.cargo/env" && cargo install xwin
```

### Arch Linux one-liner

```bash
sudo pacman -Syu --needed \
  base-devel clang lld llvm cmake make ninja python git curl pkgconf \
  unzip zip tar xz ca-certificates rustup
```

Then:

```bash
rustup default stable && cargo install xwin
```

---

## One-time setup: Windows SDK via xwin

`build.sh` expects Windows SDK/CRT material under `/opt/winsdk` by default.

```bash
sudo mkdir -p /opt/winsdk && sudo chown "$USER":"$USER" /opt/winsdk
xwin --accept-license splat --output /opt/winsdk
```

Notes:

- The scripts support both modern xwin layouts (`/opt/winsdk/sdk/...` + `/opt/winsdk/crt/...`) and classic SDK layout variants (`/opt/winsdk/Include`, `/opt/winsdk/Lib`).
- If you want a custom path, set `WINSDK_BASE`.

---

## Build walkthrough (Linux -> Windows)

### 1) Clone with submodules

```bash
git clone --recurse-submodules https://github.com/<your-org>/snes9x-fastlink.git
cd snes9x-fastlink
```

If already cloned:

```bash
git submodule update --init --recursive
```

### 2) Build dependency libraries (if missing)

Preferred:

```bash
./build.sh deps
```

Alternative helper script:

```bash
./build_windows_libs.sh all
```

### 3) Build emulator executable

```bash
./build.sh
```

Result:

- `build/snes9x.exe`

### 4) Clean artifacts

```bash
./build.sh clean
```

---

## `build.sh` deep dive (custom build pipeline)

`build.sh` is the canonical build entrypoint for this repository.

### Commands

- `./build.sh` or `./build.sh build` -> full build
- `./build.sh deps` -> only dependency check/build
- `./build.sh clean` -> remove build artifacts

### High-level pipeline

1. Detect Windows SDK roots (`detect_winsdk`)
2. Resolve include/lib flags for SDK + CRT (`sdk_flags`)
3. Check/build static deps (zlib/libpng) (`check_and_build_deps`)
4. Compile resources from `win32/rsrc/snes9x.rc` (`compile_res`)
5. Discover source files dynamically from project trees with explicit excludes (`discover_sources`, `compile_objects`)
6. Compile all sources in parallel via `clang-cl`
7. Link final executable with `lld-link` + system/static libs (`link_exe`)

### Why this build system is custom

- **No hand-maintained source list**: C/C++ units are discovered dynamically from source trees at build time.
- **Cross-targeting MSVC ABI from Linux**: uses `clang-cl` + `lld-link` with Windows SDK/UCRT includes and libs.
- **Graceful resource compile fallback**: attempts direct `llvm-rc`, then preprocessed RC fallback, then stub `.res` as last resort.
- **Optional Slang shader stack toggle**: `WITH_SLANG=1|0` gates shader-source compilation path.

### Environment variables supported

Build pipeline:

- `CONFIG=Release|Debug`
- `WITH_SLANG=1|0`
- `WINSDK_BASE=/opt/winsdk`
- `LIBS_PREFIX=/opt/windows-libs`
- `TARGET_TRIPLE` (default `x86_64-pc-windows-msvc`)
- `BUILD_DIR`, `DEPS_BUILD_DIR`, `MNT_COPY_DIR`
- `CL`, `LINKER`, `RC_TOOL`
- `JOBS`

Runtime latency/audio toggles:

- `SNES9X_AUDIO_EXCLUSIVE=1` (optional exclusive-mode test path; falls back to shared mode on failure)
- `SNES9X_AUDIO_SAFE_PROFILE=1` (forces safer 64ms audio init headroom)
- `SNES9X_LATENCY_LOG=1` (enables latency trace logging)

### Dependency model in `build.sh`

- Required as external static libs:
  - zlib (`$LIBS_PREFIX/zlib/lib/zlib.lib`)
  - libpng (`$LIBS_PREFIX/libpng/lib/libpng.lib`)
- Bundled in repository and consumed from source:
  - glslang
  - SPIRV-Cross

---

## `build_windows_libs.sh` deep dive

`build_windows_libs.sh` is the extended dependency/bootstrap script.

### What it does

- Bootstraps Windows SDK via `xwin` (`setup` path)
- Detects SDK/CRT structure variants (xwin-style and classic-style)
- Builds static Windows-target libraries (zlib, libpng, glslang, SPIRV-Cross, libADLMIDI)
- Installs outputs under `/opt/windows-libs` by default

### Command interface

```bash
./build_windows_libs.sh setup
./build_windows_libs.sh test
./build_windows_libs.sh zlib
./build_windows_libs.sh libpng
./build_windows_libs.sh spirv-cross
./build_windows_libs.sh glslang
./build_windows_libs.sh adlmidi
./build_windows_libs.sh all
```

### Relationship to `build.sh`

- Use `build.sh` for normal day-to-day builds.
- Use `build_windows_libs.sh` when you need to (re)bootstrap or inspect the Windows dependency toolchain in detail.

---

## Troubleshooting

### `WINSDK_BASE not found`

Install SDK files with:

```bash
xwin --accept-license splat --output /opt/winsdk
```

Or set a custom base:

```bash
WINSDK_BASE=/custom/path ./build.sh
```

### Missing zlib/libpng `.lib`

Run:

```bash
./build.sh deps
```

### Submodule-related compile failures

Run:

```bash
git submodule update --init --recursive
```

### Tool missing (`clang-cl`, `llvm-rc`, etc.)

Ensure LLVM toolchain packages are installed and binaries are visible in `PATH`.

---

## Output artifacts

- Final executable: `build/snes9x.exe`
- Build manifest example: `build/compile_manifest.txt`
- Intermediate objects: `build*/obj/`

---

# Changelog

## 2026-03-23

- added Phase D fast HID ingest path in Win32 input runtime, with always-on policy in this fork
- added auto-target-all behavior when no VID:PID list is provided and kept explicit target support for diagnostics
- fixed fast-path regression that could drop controller input if worker state was unavailable
- updated frame handoff to avoid write/render contention and added dialog-focus safety behavior for Win32 config windows
- finalized always-on low-latency runtime policy for key toggles (frame handoff, emu worker boost, fast input)
- updated audio runtime defaults and fallback docs (shared default, optional exclusive test, profile-driven headroom)

## 2026-03-03 - 4:17 PM EST

- moved loose root source modules into `src/` (`common`, `filter`, `jma`, `unzip`, `vulkan`) and updated Win32/build include paths accordingly
- removed `scripts/` directory and promoted `build_windows_libs.sh` to repository root
- moved `tools/winmain_patch.cpp` to `src/platform/win32/winmain_patch.cpp` (adjacent to Win32 frontend sources) and excluded it from normal build discovery
- cleared obsolete files from `docs/` while preserving the directory itself
- updated `appveyor.yml` packaging to stop referencing removed `docs/changes.txt`
- validated full `./build.sh` succeeds and `/mnt/PhantomIDE/snes9x.exe` mirror copy succeeds after reorganization

## 2026-03-03 - 3:49 PM EST

- removed dead Win32 `LocalVidMem` artifacts from GUI state/config registration/resource IDs
- changed persisted D3D shader key from `Display\\Win::Direct3D:D3DShader` to `Display\\Win::Direct3D11:Shader`
- added config preload migration to carry forward legacy D3D shader values and delete the old key
- updated display dialog wording to `Direct3D11 Shader File`
- validated full build and `/mnt` mirror copy after Phase 2 cleanup

## 2026-03-03 - 3:32 PM EST

- updated Win32 build scripts to use `/std:c++latest` for C++23-era language support on current `clang-cl`
- raised MinGW Win32 API targeting from XP-era (`0x0501`) to modern baseline (`0x0A00`) in frontend sources
- updated Win32 UI/config strings from legacy terminology to modern renderer naming (`Direct3D11`)
- marked obsolete DirectDraw-era config key semantics as legacy/no-op for current render paths
- validated full build and `/mnt` mirror copy after modernization tranche

## 2026-03-03 - 3:05 PM EST

- removed legacy Direct3D9/Cg source files from `src/platform/win32`
- removed unused Dear ImGui DX9 backend files from `external/imgui`
- updated `CCGShader.h` to remove stale Cg runtime include dependency
- updated README to document Direct3D11 fallback renderer instead of Direct3D9Ex
- added and back-populated this changelog section

## 2026-03-03 - 2:39 PM EST

- replaced `CDirect3D` implementation with a Direct3D11 renderer
- replaced DX9 visualization renderer with a GDI implementation to eliminate D3D9 dependency
- switched link libraries from `d3d9.lib` to `d3d11.lib`, `dxgi.lib`, and `d3dcompiler.lib`
- excluded obsolete D3D9/Cg compile units from dynamic source discovery
- validated full build output and `/mnt` mirror copy after migration

## 2026-03-03 - 1:58 PM EST

- changed default executable output from `out/snes9x.exe` to `build/snes9x.exe`
- added guaranteed post-link copy to `/mnt/PhantomIDE/snes9x.exe` via `MNT_COPY_DIR`
- updated clean behavior to remove legacy `out` artifacts
- updated README output-artifact documentation to match new pipeline

## 2026-03-03 - 1:22 PM EST

- moved APU implementation from `apu/` to `src/core/apu/`
- updated include paths and build references to the new core APU location
- adjusted dynamic source discovery to include explicit canonical APU translation units
- validated successful full build after APU relocation

## 2026-03-03 - 12:40 PM EST

- removed static `build_sources.txt` dependency from the build system
- implemented dynamic source discovery in `build.sh`
- added exclusion guards for non-build/test/tool translation units
- stabilized dynamic-discovery build to successfully compile and link end-to-end