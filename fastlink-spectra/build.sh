#!/usr/bin/env bash
set -euo pipefail

# FastLink Spectra build script (Linux -> Windows cross build)
# Inspired by research/Engine/build.sh but focused on this visualizer project.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-pc-windows-msvc}"
WINSDK_BASE="${WINSDK_BASE:-/opt/winsdk}"
OUT_DIR="${OUT_DIR:-build}"
EXE_NAME="FastLinkSpectra.exe"

CL="${CL:-clang-cl}"

mkdir -p "$OUT_DIR"

if [[ "${1:-build}" == "clean" ]]; then
  rm -rf "$OUT_DIR"
  echo "Cleaned $OUT_DIR"
  exit 0
fi

if [[ ! -d "$WINSDK_BASE" ]]; then
  echo "WINSDK_BASE not found: $WINSDK_BASE"
  exit 1
fi

SDK_INC_ROOT=""
SDK_LIB_ROOT=""
if [[ -d "$WINSDK_BASE/sdk/include" ]]; then
  SDK_INC_ROOT="$WINSDK_BASE/sdk/include"
  SDK_LIB_ROOT="$WINSDK_BASE/sdk/lib"
elif [[ -d "$WINSDK_BASE/Include" ]]; then
  SDK_INC_ROOT="$WINSDK_BASE/Include"
  SDK_LIB_ROOT="$WINSDK_BASE/Lib"
fi

if [[ -z "$SDK_INC_ROOT" || -z "$SDK_LIB_ROOT" ]]; then
  echo "Could not locate Windows SDK include/lib roots under $WINSDK_BASE"
  exit 1
fi

SDK_VER="$(ls -1 "$SDK_INC_ROOT" 2>/dev/null | grep -E '^10\.' | head -n 1 || true)"
if [[ -n "$SDK_VER" && -d "$SDK_INC_ROOT/$SDK_VER/um" ]]; then
  INC_UM="$SDK_INC_ROOT/$SDK_VER/um"
  INC_SHARED="$SDK_INC_ROOT/$SDK_VER/shared"
  INC_UCRT="$SDK_INC_ROOT/$SDK_VER/ucrt"
  if [[ -d "$SDK_LIB_ROOT/um/x86_64" ]]; then
    LIB_UM="$SDK_LIB_ROOT/um/x86_64"
    LIB_UCRT="$SDK_LIB_ROOT/ucrt/x86_64"
  else
    LIB_UM="$SDK_LIB_ROOT/$SDK_VER/um/x64"
    LIB_UCRT="$SDK_LIB_ROOT/$SDK_VER/ucrt/x64"
  fi
else
  INC_UM="$SDK_INC_ROOT/um"
  INC_SHARED="$SDK_INC_ROOT/shared"
  INC_UCRT="$SDK_INC_ROOT/ucrt"
  LIB_UM="$SDK_LIB_ROOT/um/x86_64"
  LIB_UCRT="$SDK_LIB_ROOT/ucrt/x86_64"
fi

CRT_INC="$WINSDK_BASE/crt/include"
CRT_LIB="$WINSDK_BASE/crt/lib/x86_64"

CXXFLAGS=(
  "-fuse-ld=lld"
  "--target=$TARGET_TRIPLE"
  "/nologo"
  "/std:c++latest"
  "/EHsc"
  "/O2"
  "/arch:AVX2"
  "-mavx2"
  "/MT"
  "/DNDEBUG"
  "/DUNICODE"
  "/D_UNICODE"
  "/DNOMINMAX"
  "/DWIN32_LEAN_AND_MEAN"
  "/Iinclude"
  "/I$CRT_INC"
  "/I$INC_UCRT"
  "/I$INC_UM"
  "/I$INC_SHARED"
)

LINKFLAGS=(
  "/link"
  "/nologo"
  "/subsystem:windows"
  "/entry:WinMainCRTStartup"
  "/libpath:$CRT_LIB"
  "/libpath:$LIB_UCRT"
  "/libpath:$LIB_UM"
  "/defaultlib:libcmt.lib"
  "/nodefaultlib:msvcrt.lib"
  "user32.lib"
  "gdi32.lib"
  "kernel32.lib"
  "winhttp.lib"
  "d3d11.lib"
  "dxgi.lib"
  "d3dcompiler.lib"
  "/out:$OUT_DIR/$EXE_NAME"
)

SOURCES=(
  src/main.cpp
  src/app.cpp
  src/d3d11_renderer.cpp
  src/ipc.cpp
  src/rest_client.cpp
  src/visualizer.cpp
)

"$CL" "${CXXFLAGS[@]}" "${SOURCES[@]}" "${LINKFLAGS[@]}"

echo "Built: $OUT_DIR/$EXE_NAME"
