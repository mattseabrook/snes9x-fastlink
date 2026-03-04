#!/usr/bin/env bash
set -euo pipefail

#===============================================================================
# snes9x-fastlink - Linux -> Windows cross build (clang-cl + lld-link)
# - Fully automated: builds missing dependencies (zlib, libpng, glslang, spirv-cross)
# - Dynamically discovers sources (VCXPROJ-free)
# - Builds .res from src/platform/win32/rsrc/snes9x.rc
# - Links static zlib/libpng (+ optional glslang + SPIRV-Cross)
#
# Usage: ./build.sh [build|clean|deps]
#   build (default) - build dependencies if needed, then build snes9x.exe
#   clean           - remove build artifacts
#   deps            - only build dependencies
#===============================================================================

# ----------------------------- Pretty output ---------------------------------
if [[ -t 1 ]] && command -v tput >/dev/null 2>&1 && [[ $(tput colors 2>/dev/null || echo 0) -ge 8 ]]; then
  C0=$'\033[0m'; CB=$'\033[1m'; CR=$'\033[31m'; CG=$'\033[32m'; CY=$'\033[33m'; CC=$'\033[36m'
else
  C0=""; CB=""; CR=""; CG=""; CY=""; CC=""
fi

banner(){ local w=78; local line; line="$(printf '%*s' "$w" | tr ' ' '=')"
  printf "\n${CB}${CC}%s\n  %s\n%s${C0}\n\n" "$line" "$1" "$line"
}
info(){ printf "${CY}→ %s${C0}\n" "$*"; }
ok(){ printf "${CG}✓ %s${C0}\n" "$*"; }
die(){ printf "${CR}✗ %s${C0}\n" "$*" >&2; exit 1; }

# ------------------------------ Config ---------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -d "$SCRIPT_DIR/win32" && -f "$SCRIPT_DIR/.gitmodules" ]]; then
  ROOT_DIR="$SCRIPT_DIR"
elif [[ -d "$SCRIPT_DIR/../win32" && -f "$SCRIPT_DIR/../.gitmodules" ]]; then
  ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
else
  ROOT_DIR="$SCRIPT_DIR"
fi
cd "$ROOT_DIR"

TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-pc-windows-msvc}"
CONFIG="${CONFIG:-Release}"
BUILD_DIR="${BUILD_DIR:-build}"
WINSDK_BASE="${WINSDK_BASE:-/opt/winsdk}"
DEPS_BUILD_DIR="${DEPS_BUILD_DIR:-/tmp/snes9x-deps-build}"
MNT_COPY_DIR="${MNT_COPY_DIR:-/mnt/PhantomIDE}"

# Prebuilt static libs location
LIBS_PREFIX="${LIBS_PREFIX:-/opt/windows-libs}"
ZLIB_DIR="${ZLIB_DIR:-$LIBS_PREFIX/zlib}"
LIBPNG_DIR="${LIBPNG_DIR:-$LIBS_PREFIX/libpng}"
GLSLANG_DIR="${GLSLANG_DIR:-$LIBS_PREFIX/glslang}"
SPIRVCROSS_DIR="${SPIRVCROSS_DIR:-$LIBS_PREFIX/spirv-cross}"

# Feature toggles
WITH_SLANG="${WITH_SLANG:-1}"  # 1 = link slang deps; 0 = compile without USE_SLANG

# Tools
CL="${CL:-clang-cl}"
LINKER="${LINKER:-lld-link}"
RC_TOOL="${RC_TOOL:-llvm-rc}"

JOBS="${JOBS:-$(nproc)}"

# ------------------------ Windows SDK detection ------------------------------
detect_winsdk() {
  [[ -d "$WINSDK_BASE" ]] || die "WINSDK_BASE not found: $WINSDK_BASE (run 'xwin splat' first)"

  local incA="" libA=""

  if [[ -d "$WINSDK_BASE/sdk/include" ]]; then
    incA="$WINSDK_BASE/sdk/include"
  elif [[ -d "$WINSDK_BASE/Include" ]]; then
    incA="$WINSDK_BASE/Include"
  fi

  if [[ -d "$WINSDK_BASE/sdk/lib" ]]; then
    libA="$WINSDK_BASE/sdk/lib"
  elif [[ -d "$WINSDK_BASE/Lib" ]]; then
    libA="$WINSDK_BASE/Lib"
  fi

  [[ -n "$incA" && -n "$libA" ]] || die "Could not detect SDK include/lib roots under $WINSDK_BASE"

  local ver=""
  if compgen -G "$incA/*" >/dev/null; then
    ver="$(ls -1 "$incA" 2>/dev/null | grep -E '^10\.' | head -n 1 || true)"
  fi

  export SDK_INC_ROOT="$incA"
  export SDK_LIB_ROOT="$libA"
  export SDK_VER="$ver"
}

sdk_flags() {
  local inc_um inc_shared inc_ucrt lib_um lib_ucrt crt_inc crt_lib

  crt_inc="$WINSDK_BASE/crt/include"
  crt_lib="$WINSDK_BASE/crt/lib"

  if [[ -n "$SDK_VER" && -d "$SDK_INC_ROOT/$SDK_VER/um" ]]; then
    inc_um="$SDK_INC_ROOT/$SDK_VER/um"
    inc_shared="$SDK_INC_ROOT/$SDK_VER/shared"
    inc_ucrt="$SDK_INC_ROOT/$SDK_VER/ucrt"
    if [[ -d "$SDK_LIB_ROOT/um/x86_64" ]]; then
      lib_um="$SDK_LIB_ROOT/um/x86_64"
      lib_ucrt="$SDK_LIB_ROOT/ucrt/x86_64"
    else
      lib_um="$SDK_LIB_ROOT/$SDK_VER/um/x64"
      lib_ucrt="$SDK_LIB_ROOT/$SDK_VER/ucrt/x64"
    fi
  else
    inc_um="$SDK_INC_ROOT/um"
    inc_shared="$SDK_INC_ROOT/shared"
    inc_ucrt="$SDK_INC_ROOT/ucrt"
    lib_um="$SDK_LIB_ROOT/um/x86_64"
    lib_ucrt="$SDK_LIB_ROOT/ucrt/x86_64"
  fi

  [[ -d "$inc_um" && -d "$inc_shared" && -d "$inc_ucrt" ]] || die "Missing SDK include dirs"
  [[ -d "$lib_um" && -d "$lib_ucrt" ]] || die "Missing SDK lib dirs"

  export CRT_INC="$crt_inc"
  export CRT_LIB="$crt_lib"
  export INC_UM="$inc_um"
  export INC_SHARED="$inc_shared"
  export INC_UCRT="$inc_ucrt"
  export LIB_UM="$lib_um"
  export LIB_UCRT="$lib_ucrt"
}

# ----------------------- CMake Toolchain for deps ----------------------------
create_toolchain_file() {
  local outdir="$1"
  local cmake_inc_flags=""
  [[ -d "$CRT_INC" ]] && cmake_inc_flags+="-imsvc $CRT_INC "
  [[ -d "$INC_UCRT" ]] && cmake_inc_flags+="-imsvc $INC_UCRT "
  [[ -d "$INC_UM" ]] && cmake_inc_flags+="-imsvc $INC_UM "
  [[ -d "$INC_SHARED" ]] && cmake_inc_flags+="-imsvc $INC_SHARED "

  local cmake_lib_flags=""
  [[ -d "$CRT_LIB/x86_64" ]] && cmake_lib_flags+="-libpath:$CRT_LIB/x86_64 "
  [[ -d "$LIB_UM" ]] && cmake_lib_flags+="-libpath:$LIB_UM "
  [[ -d "$LIB_UCRT" ]] && cmake_lib_flags+="-libpath:$LIB_UCRT "

  cat > "$outdir/windows-cross.cmake" << EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)
set(CMAKE_AR llvm-lib)
set(CMAKE_C_ARCHIVE_CREATE "<CMAKE_AR> /OUT:<TARGET> <OBJECTS>")
set(CMAKE_C_ARCHIVE_FINISH "")
set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> /OUT:<TARGET> <OBJECTS>")
set(CMAKE_CXX_ARCHIVE_FINISH "")
set(CMAKE_C_FLAGS_INIT "-fuse-ld=lld-link $cmake_inc_flags /MT -D_WIN32 -D_WIN64 -fms-compatibility")
set(CMAKE_CXX_FLAGS_INIT "-fuse-ld=lld-link $cmake_inc_flags /MT -D_WIN32 -D_WIN64 -fms-compatibility")
set(CMAKE_C_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O2 -DNDEBUG")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_EXE_LINKER_FLAGS_INIT "$cmake_lib_flags -DEFAULTLIB:libcmt.lib -NODEFAULTLIB:msvcrt.lib")
set(CMAKE_FIND_ROOT_PATH "$WINSDK_BASE" "$LIBS_PREFIX")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
}

# ----------------------- Dependency builders ---------------------------------
build_zlib() {
  banner "Building zlib"
  mkdir -p "$DEPS_BUILD_DIR" && cd "$DEPS_BUILD_DIR"

  if [[ ! -d "zlib-1.3.1" ]]; then
    info "Downloading zlib 1.3.1..."
    curl -sL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz | tar xz
  fi

  cd zlib-1.3.1
  rm -rf build_win && mkdir build_win && cd build_win
  create_toolchain_file "$(pwd)"

  cmake .. -DCMAKE_TOOLCHAIN_FILE=windows-cross.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$ZLIB_DIR" -DBUILD_SHARED_LIBS=OFF >/dev/null

  make zlibstatic -j"$JOBS" >/dev/null

  mkdir -p "$ZLIB_DIR/lib" "$ZLIB_DIR/include"
  cp zlibstatic.lib "$ZLIB_DIR/lib/zlib.lib"
  cp ../zlib.h zconf.h "$ZLIB_DIR/include/"
  ok "zlib installed to $ZLIB_DIR"
}

build_libpng() {
  banner "Building libpng"
  mkdir -p "$DEPS_BUILD_DIR" && cd "$DEPS_BUILD_DIR"

  if [[ ! -d "libpng-1.6.50" ]]; then
    info "Downloading libpng 1.6.50..."
    curl -sL https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.50.tar.gz | tar xz
  fi

  cd libpng-1.6.50

  # Use prebuilt pnglibconf.h
  if [[ -f "projects/vstudio/pnglibconf.h" ]]; then
    cp projects/vstudio/pnglibconf.h .
  else
    cp scripts/pnglibconf.h.prebuilt pnglibconf.h
  fi

  rm -rf build_win && mkdir build_win && cd build_win

  # Manual compile (CMake has issues with libpng cross)
  local cflags=(
    "--target=$TARGET_TRIPLE" "/nologo" "/c" "/O2" "/MT" "/DNDEBUG"
    "/DPNG_STATIC" "/DZLIB_STATIC" "/DPNG_NO_MMX_CODE"
    "-I.." "-I$ZLIB_DIR/include"
    "/I$CRT_INC" "/I$INC_UCRT" "/I$INC_UM" "/I$INC_SHARED"
  )

  local srcs=(png pngerror pngget pngmem pngpread pngread pngrio pngrtran
              pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil)
  local objs=()

  for s in "${srcs[@]}"; do
    "$CL" "${cflags[@]}" "../$s.c" "/Fo$s.obj" 2>/dev/null && objs+=("$s.obj")
  done

  llvm-lib "/OUT:libpng.lib" "${objs[@]}" >/dev/null

  mkdir -p "$LIBPNG_DIR/lib" "$LIBPNG_DIR/include"
  cp libpng.lib "$LIBPNG_DIR/lib/libpng.lib"
  cp ../png.h ../pngconf.h ../pnglibconf.h "$LIBPNG_DIR/include/"
  ok "libpng installed to $LIBPNG_DIR"
}

build_glslang() {
  banner "Building glslang"
  mkdir -p "$DEPS_BUILD_DIR" && cd "$DEPS_BUILD_DIR"

  if [[ ! -d "glslang" ]]; then
    info "Cloning glslang..."
    git clone --depth 1 https://github.com/KhronosGroup/glslang.git 2>/dev/null
  fi

  cd glslang
  git submodule update --init --recursive 2>/dev/null || true

  rm -rf build_win && mkdir build_win && cd build_win
  create_toolchain_file "$(pwd)"

  cmake .. -DCMAKE_TOOLCHAIN_FILE=windows-cross.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$GLSLANG_DIR" -DBUILD_SHARED_LIBS=OFF \
    -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DENABLE_SPVREMAPPER=ON \
    -DSKIP_SPIRV_TOOLS_INSTALL=ON -DENABLE_OPT=OFF >/dev/null

  cmake --build . --config Release -j"$JOBS" 2>/dev/null

  mkdir -p "$GLSLANG_DIR/lib" "$GLSLANG_DIR/include/glslang" "$GLSLANG_DIR/include/SPIRV"
  find . -name "*.lib" -exec cp {} "$GLSLANG_DIR/lib/" \; 2>/dev/null
  cp -r ../glslang/* "$GLSLANG_DIR/include/glslang/" 2>/dev/null || true
  cp -r ../SPIRV/* "$GLSLANG_DIR/include/SPIRV/" 2>/dev/null || true
  ok "glslang installed to $GLSLANG_DIR"
}

build_spirv_cross() {
  banner "Building SPIRV-Cross"
  mkdir -p "$DEPS_BUILD_DIR" && cd "$DEPS_BUILD_DIR"

  if [[ ! -d "SPIRV-Cross" ]]; then
    info "Cloning SPIRV-Cross..."
    git clone --depth 1 https://github.com/KhronosGroup/SPIRV-Cross.git 2>/dev/null
  fi

  cd SPIRV-Cross
  rm -rf build_win && mkdir build_win && cd build_win
  create_toolchain_file "$(pwd)"

  cmake .. -DCMAKE_TOOLCHAIN_FILE=windows-cross.cmake -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SPIRVCROSS_DIR" -DBUILD_SHARED_LIBS=OFF \
    -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON -DSPIRV_CROSS_CLI=OFF \
    -DSPIRV_CROSS_ENABLE_TESTS=OFF >/dev/null

  cmake --build . --config Release -j"$JOBS" 2>/dev/null

  mkdir -p "$SPIRVCROSS_DIR/lib" "$SPIRVCROSS_DIR/include"
  find . -name "*.lib" -exec cp {} "$SPIRVCROSS_DIR/lib/" \; 2>/dev/null
  cp -r ../include/* "$SPIRVCROSS_DIR/include/" 2>/dev/null || true
  ok "SPIRV-Cross installed to $SPIRVCROSS_DIR"
}

# ----------------------- Dependency checker ----------------------------------
# Note: glslang and SPIRV-Cross sources are bundled in external/
# We only need zlib and libpng as external static libraries
check_and_build_deps() {
  banner "Checking dependencies"

  local need_zlib=0 need_png=0

  if [[ ! -f "$ZLIB_DIR/lib/zlib.lib" ]]; then
    info "zlib: NOT FOUND"; need_zlib=1
  else
    ok "zlib: $ZLIB_DIR"
  fi

  if [[ ! -f "$LIBPNG_DIR/lib/libpng.lib" ]]; then
    info "libpng: NOT FOUND"; need_png=1
  else
    ok "libpng: $LIBPNG_DIR"
  fi

  # glslang/SPIRV-Cross are bundled in external/ - just check they exist
  if [[ "$WITH_SLANG" == "1" ]]; then
    if [[ -d "external/glslang" ]]; then
      ok "glslang: bundled in external/glslang"
    else
      die "external/glslang not found - clone with submodules"
    fi
    if [[ -d "external/SPIRV-Cross" ]]; then
      ok "SPIRV-Cross: bundled in external/SPIRV-Cross"
    else
      die "external/SPIRV-Cross not found - clone with submodules"
    fi
  fi

  # Build missing dependencies
  [[ $need_zlib -eq 1 ]] && build_zlib
  [[ $need_png -eq 1 ]] && build_libpng

  ok "All dependencies ready"
}

# ------------------------- Dynamic source discovery --------------------------
discover_sources() {
  local -a common_roots=(
    "src/filter"
    "src/jma"
    "src/core"
    "src/fastlink"
    "src/platform/win32"
    "src/unzip"
    "src/vulkan"
    "external/imgui"
    "external/stb"
  )

  local -a common_sources=()
  local -a apu_sources=()
  local -a slang_sources=()
  local -a all_sources=()

  local -a apu_units=(
    "src/core/apu/apu.cpp"
    "src/core/apu/bapu/dsp/sdsp.cpp"
    "src/core/apu/bapu/smp/smp.cpp"
    "src/core/apu/bapu/smp/smp_state.cpp"
  )
  local unit
  for unit in "${apu_units[@]}"; do
    [[ -f "$unit" ]] && apu_sources+=("$unit")
  done

  if [[ ${#common_roots[@]} -gt 0 ]]; then
    mapfile -d '' -t common_sources < <(
      find "${common_roots[@]}" -type f \
        \( -name "*.c" -o -name "*.cpp" \) \
        ! -path "src/core/apu/*" \
        ! -path "src/platform/win32/rsrc/*" \
        ! -path "src/platform/win32/zlib/*" \
        ! -path "src/platform/win32/CD3DCG.cpp" \
        ! -path "src/platform/win32/cgFunctions.cpp" \
        ! -path "src/platform/win32/image_functions.cpp" \
        ! -path "src/platform/win32/winmain_patch.cpp" \
        ! -path "src/core/cartridge/srtcemu.cpp" \
        ! -path "src/core/cartridge/spc7110emu.cpp" \
        ! -path "external/SPIRV-Cross/main.cpp" \
        ! -path "external/imgui/imgui_impl_dx9.cpp" \
        ! -path "src/unzip/minizip.c" \
        ! -path "src/unzip/miniunz.c" \
        ! -path "src/vulkan/slang_preset_test.cpp" \
        ! -path "src/vulkan/std_chrono_throttle.cpp" \
        | LC_ALL=C sort -z
    )
  fi

  if [[ "$WITH_SLANG" == "1" ]]; then
    local -a glslang_roots=(
      "external/glslang/glslang/GenericCodeGen"
      "external/glslang/glslang/MachineIndependent"
      "external/glslang/glslang/MachineIndependent/preprocessor"
      "external/glslang/glslang/OSDependent/Windows"
      "external/glslang/glslang/ResourceLimits"
      "external/glslang/OGLCompilersDLL"
      "external/glslang/SPIRV"
      "external/SPIRV-Cross"
    )

    local -a include_predicates=(
      -name "*.cpp"
    )

    mapfile -d '' -t slang_sources < <(
      find "${glslang_roots[@]}" -type f \
        \( "${include_predicates[@]}" \) \
        ! -path "external/SPIRV-Cross/*/*" \
        ! -path "external/SPIRV-Cross/main.cpp" \
        ! -path "external/glslang/glslang/OSDependent/Windows/main.cpp" \
        ! -path "*/test/*" \
        ! -path "*/tests/*" \
        | LC_ALL=C sort -z
    )
  fi

  all_sources=("${apu_sources[@]}" "${common_sources[@]}" "${slang_sources[@]}")
  [[ ${#all_sources[@]} -gt 0 ]] || die "No sources discovered"

  printf '%s\n' "${all_sources[@]}"
}

# --------------------------- Build steps --------------------------------------
compile_res() {
  banner "Compiling resources (.rc → .res)"

  local rc="src/platform/win32/rsrc/snes9x.rc"
  [[ -f "$rc" ]] || die "Missing resource file: $rc"

  mkdir -p "$BUILD_DIR/obj"

  # llvm-rc struggles with MSVC-style RC files; preprocess with clang first
  local rc_inc=("-I$CRT_INC" "-I$INC_UCRT" "-I$INC_UM" "-I$INC_SHARED" "-Isrc/platform/win32/rsrc")
  local preprocessed="$BUILD_DIR/obj/snes9x_pp.rc"

  # Preprocess the RC file (fallback path)
  "$CL" --target="$TARGET_TRIPLE" /nologo /E /D_WIN32 /DWIN32 \
    "${rc_inc[@]}" "$rc" 2>/dev/null > "$preprocessed" || true

  # Prefer compiling the original RC so resource syntax stays intact
  if command -v "$RC_TOOL" >/dev/null 2>&1; then
    "$RC_TOOL" /nologo /DWIN32 /D_WIN32 /Isrc/platform/win32/rsrc \
      /I"$INC_UM" /I"$INC_SHARED" /I"$INC_UCRT" /I"$CRT_INC" \
      /fo "$BUILD_DIR/obj/snes9x.res" "$rc" 2>/dev/null || {
      # Fallback: try preprocessed file if direct compile fails
      if [[ -s "$preprocessed" ]]; then
        "$RC_TOOL" /nologo /Isrc/platform/win32/rsrc /fo "$BUILD_DIR/obj/snes9x.res" "$preprocessed" 2>/dev/null || {
          info "Resource compilation failed - creating stub .res"
          echo "" | "$RC_TOOL" /nologo /fo "$BUILD_DIR/obj/snes9x.res" /dev/stdin 2>/dev/null || \
            touch "$BUILD_DIR/obj/snes9x.res"
        }
      else
        info "Resource compilation failed - creating stub .res"
        echo "" | "$RC_TOOL" /nologo /fo "$BUILD_DIR/obj/snes9x.res" /dev/stdin 2>/dev/null || \
          touch "$BUILD_DIR/obj/snes9x.res"
      fi
    }
  elif command -v windres >/dev/null 2>&1; then
    windres -O coff -i "$rc" -o "$BUILD_DIR/obj/snes9x.res"
  else
    info "No resource compiler available - creating empty .res"
    touch "$BUILD_DIR/obj/snes9x.res"
  fi
  ok "Resources compiled"
}

compile_objects() {
  banner "Compiling sources ($CONFIG)"

  mapfile -t SOURCES < <(discover_sources)
  [[ ${#SOURCES[@]} -gt 0 ]] || die "No sources discovered"

  mkdir -p "$BUILD_DIR/obj"
  rm -f "$BUILD_DIR/obj"/*.obj "$BUILD_DIR/obj"/*.log 2>/dev/null || true
  rm -f "$BUILD_DIR/compile_manifest.txt"

  # Defines from vcxproj Release|x64 configuration
  local defs=(
    "-DWIN32" "-D_WIN32" "-DWIN64" "-D_WIN64"
    "-DUNICODE" "-D_UNICODE"
    "-D_CRT_SECURE_NO_WARNINGS"
    "-DZLIB" "-DZLIB_STATIC" "-DPNG_STATIC"
    "-D__WIN32__"
    "-DHAVE_LIBPNG"
    "-DJMA_SUPPORT"
    "-DUNZIP_SUPPORT"
    "-DNETPLAY_SUPPORT"
    "-DALLOW_CPU_OVERCLOCK"
    "-DVK_USE_PLATFORM_WIN32_KHR"
    "-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1"
    "-DVMA_DYNAMIC_VULKAN_FUNCTIONS=1"
    "-DVMA_STATIC_VULKAN_FUNCTIONS=0"
    "-DVMA_USE_STL_SHARED_MUTEX=0"
    "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES"
    "-DIMGUI_DISABLE_SSE"
    "-DSTBI_NO_SIMD"
  )

  if [[ "$WITH_SLANG" == "0" ]]; then
    defs+=("-DUSE_SLANG=0")
  else
    defs+=("-DUSE_SLANG=1")
  fi

  local opt=()
  if [[ "$CONFIG" == "Debug" ]]; then
    opt+=("-Od" "-Z7" "-DDEBUG" "-D_DEBUG")
  else
    opt+=("-O2" "-DNDEBUG")
  fi

  # Include paths - core sources are now under src/, plus bundled external and platform dirs.
  local inc=(
    "-I." "-Isrc" "-Isrc/platform/win32" "-Isrc/platform/win32/rsrc"
    "-Isrc/core/cpu" "-Isrc/core/ppu" "-Isrc/core/memory" "-Isrc/core/cartridge"
    "-Isrc/core/state" "-Isrc/core/cheats" "-Isrc/core/netplay" "-Isrc/core/video-common"
    "-Isrc/core/util"
    "-Isrc/fastlink" "-Isrc/fastlink/memserve" "-Isrc/fastlink/memshare"
    "-Isrc/unzip" "-Isrc/jma" "-Isrc/filter" "-Isrc/common" "-Isrc/vulkan" "-Isrc/core/apu"
    "-Iexternal" "-Iexternal/glad/include"
    "-Isrc/platform/win32/glslang" "-Isrc/platform/win32/glslang/glslang" "-Isrc/platform/win32/glslang/SPIRV"
    "-Iexternal/glslang"
    "-Iexternal/SPIRV-Cross"
    "-Iexternal/stb"
    "-Iexternal/vulkan-headers/include"
    "-Iexternal/VulkanMemoryAllocator-Hpp/include"
    "-Iexternal/fmt/include"
    "-Iexternal/imgui"
    "-I$ZLIB_DIR/include"
    "-I$LIBPNG_DIR/include"
  )

  local msvc_inc=(
    "/I$CRT_INC" "/I$INC_UCRT" "/I$INC_UM" "/I$INC_SHARED"
  )

  local cflags=(
    "--target=$TARGET_TRIPLE"
    "/nologo"
    "/std:c++latest"
    "-Wno-enum-enum-conversion"
    "/EHsc"
    "/MT"
    "/Oi"
    "/arch:SSE2"
    "-msse2"
    "-fuse-ld=$LINKER"
    "${opt[@]}"
    "${defs[@]}"
    "${inc[@]}"
    "${msvc_inc[@]}"
  )

  info "Discovered ${#SOURCES[@]} source files"

  local src obj count=0
  for src in "${SOURCES[@]}"; do
    [[ -f "$src" ]] || continue
    obj="$BUILD_DIR/obj/$(echo "$src" | sed 's#/#_#g' | sed 's/\.[cC][pP]*[pP]*$/.obj/')"
    printf "%s %s\n" "$src" "$obj" >> "$BUILD_DIR/compile_manifest.txt"
    ((count++)) || true
  done

  info "Compiling $count files with $JOBS parallel jobs..."

  # Export for xargs subshell
  export CL_CMD="$CL"
  export CFLAGS_STR="${cflags[*]}"

  # Compile each source once in parallel. Keep per-file logs for failures and
  # print diagnostics immediately so broken objects are easy to identify.
  if ! cat "$BUILD_DIR/compile_manifest.txt" \
    | xargs -P "$JOBS" -n 2 sh -c '
        log="$1.log"
        if ! $CL_CMD $CFLAGS_STR /c "$0" /Fo"$1" >"$log" 2>&1; then
          echo "FAILED: $0" >&2
          cat "$log" >&2
          exit 1
        fi
        rm -f "$log"
      '; then
    die "Compilation failed"
  fi

  # Check we got some obj files
  local obj_count
  obj_count=$(find "$BUILD_DIR/obj" -name "*.obj" 2>/dev/null | wc -l)
  [[ $obj_count -gt 0 ]] || die "No object files produced"

  ok "Compile complete ($obj_count object files)"
}

link_exe() {
  banner "Linking snes9x.exe"

  local objs=("$BUILD_DIR/obj/"*.obj)
  [[ -f "${objs[0]}" ]] || die "No object files found in $BUILD_DIR/obj"

  # Windows SDK and system libs + our static deps (zlib, libpng)
  # glslang/SPIRV-Cross are compiled from source in external/
  local libs=(
    "/libpath:$CRT_LIB/x86_64"
    "/libpath:$LIB_UCRT"
    "/libpath:$LIB_UM"
    "kernel32.lib" "user32.lib" "gdi32.lib" "comdlg32.lib" "advapi32.lib"
    "shell32.lib" "ole32.lib" "oleaut32.lib" "winmm.lib" "shlwapi.lib"
    "d3d11.lib" "dxgi.lib" "d3dcompiler.lib" "dxguid.lib" "dsound.lib" "comctl32.lib" "opengl32.lib" "hid.lib"
    "ws2_32.lib" "wsock32.lib" "vfw32.lib" "msxml2.lib" "delayimp.lib"
    "$ZLIB_DIR/lib/zlib.lib"
    "$LIBPNG_DIR/lib/libpng.lib"
  )

  local out="$BUILD_DIR/snes9x.exe"
  mkdir -p "$BUILD_DIR"

  "$CL" --target="$TARGET_TRIPLE" /nologo /MT -fuse-ld="$LINKER" \
    "${objs[@]}" \
    "$BUILD_DIR/obj/snes9x.res" \
    /link \
    /nologo \
    /subsystem:windows \
    /incremental:no \
    /out:"$out" \
    "${libs[@]}" \
    /NODEFAULTLIB:msvcrt.lib

  ok "Built: $out ($(du -h "$out" | cut -f1))"

  if [[ -n "$MNT_COPY_DIR" ]]; then
    mkdir -p "$MNT_COPY_DIR" || die "Failed to create MNT_COPY_DIR: $MNT_COPY_DIR"
    cp -f "$out" "$MNT_COPY_DIR/snes9x.exe" || die "Failed to copy executable to $MNT_COPY_DIR"
    ok "Copied executable to $MNT_COPY_DIR/snes9x.exe"
  fi
}

clean() {
  banner "Cleaning"
  rm -rf "$BUILD_DIR" out
  ok "Clean complete"
}

usage() {
  cat <<EOF
Usage: $0 [build|clean|deps]

Commands:
  build (default)  Build snes9x.exe (auto-builds missing deps)
  clean            Remove build artifacts
  deps             Only build/check dependencies

Environment variables:
  CONFIG=Release|Debug       Build configuration (default: Release)
  WITH_SLANG=1|0            Enable slang shader support (default: 1)
  WINSDK_BASE=/opt/winsdk   Windows SDK location
  LIBS_PREFIX=/opt/windows-libs  Where to install/find deps
  MNT_COPY_DIR=/mnt/PhantomIDE    Post-build mirror destination (set empty to disable)
EOF
}

main() {
  local cmd="${1:-build}"

  case "$cmd" in
    build)
      banner "snes9x-fastlink Cross Build"
      detect_winsdk
      sdk_flags
      check_and_build_deps
      compile_res
      compile_objects
      link_exe
      ;;
    deps)
      banner "Building Dependencies Only"
      detect_winsdk
      sdk_flags
      check_and_build_deps
      ;;
    clean) clean ;;
    -h|--help|help) usage ;;
    *) usage; exit 1 ;;
  esac
}

main "$@"
