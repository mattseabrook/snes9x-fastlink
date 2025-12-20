#!/usr/bin/env bash
set -euo pipefail

#===============================================================================
# snes9x-fastlink - Linux -> Windows cross build (clang-cl + lld-link)
# - Parses win32/snes9xw.vcxproj for sources (no hand-maintained file lists)
# - Builds .res from win32/rsrc/snes9x.rc
# - Links static zlib/libpng (+ optional glslang + SPIRV-Cross)
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
die(){ printf "${CR}%s${C0}\n" "$*" >&2; exit 1; }

# ------------------------------ Config ---------------------------------------
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

TARGET_TRIPLE="${TARGET_TRIPLE:-x86_64-pc-windows-msvc}"
CONFIG="${CONFIG:-Release}"          # Release | Debug
BUILD_DIR="${BUILD_DIR:-build-win64}"
OUT_DIR="${OUT_DIR:-out}"
WINSDK_BASE="${WINSDK_BASE:-/opt/winsdk}"

# Prebuilt static libs (your convention)
ZLIB_DIR="${ZLIB_DIR:-/opt/windows-libs/zlib}"
LIBPNG_DIR="${LIBPNG_DIR:-/opt/windows-libs/libpng}"
GLSLANG_DIR="${GLSLANG_DIR:-/opt/windows-libs/glslang}"          # optional
SPIRVCROSS_DIR="${SPIRVCROSS_DIR:-/opt/windows-libs/spirv-cross}"# optional

# Feature toggles
WITH_SLANG="${WITH_SLANG:-1}"  # 1 = link slang deps; 0 = compile without USE_SLANG

# Tools
CL="${CL:-clang-cl}"
LINKER="${LINKER:-lld-link}"
RC_TOOL="${RC_TOOL:-llvm-rc}"  # fallback to windres if needed

JOBS="${JOBS:-$(nproc)}"

# ------------------------ Windows SDK detection ------------------------------
# This matches the "xwin splat" layout patterns your system uses.
detect_winsdk() {
  [[ -d "$WINSDK_BASE" ]] || die "WINSDK_BASE not found: $WINSDK_BASE (run xwin splat first)"

  local incA libA
  incA=""
  libA=""

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

  # Detect versioned include dir (traditional SDK) if present; else treat as flat (xwin)
  local ver=""
  if compgen -G "$incA/*" >/dev/null; then
    # pick first "10.*" directory if exists
    ver="$(ls -1 "$incA" 2>/dev/null | grep -E '^10\.' | head -n 1 || true)"
  fi

  export SDK_INC_ROOT="$incA"
  export SDK_LIB_ROOT="$libA"
  export SDK_VER="$ver"
}

# Build MSVC-style include/lib flags for clang-cl/lld-link
sdk_flags() {
  local inc_um inc_shared inc_ucrt
  local lib_um lib_ucrt
  local crt_inc crt_lib

  # xwin usually places CRT under WINSDK_BASE/crt/*
  crt_inc="$WINSDK_BASE/crt/include"
  crt_lib="$WINSDK_BASE/crt/lib"

  if [[ -n "$SDK_VER" && -d "$SDK_INC_ROOT/$SDK_VER/um" ]]; then
    inc_um="$SDK_INC_ROOT/$SDK_VER/um"
    inc_shared="$SDK_INC_ROOT/$SDK_VER/shared"
    inc_ucrt="$SDK_INC_ROOT/$SDK_VER/ucrt"
    # lib layout differs by SDK flavor; prefer xwin-style first
    if [[ -d "$SDK_LIB_ROOT/um/x86_64" ]]; then
      lib_um="$SDK_LIB_ROOT/um/x86_64"
      lib_ucrt="$SDK_LIB_ROOT/ucrt/x86_64"
    else
      lib_um="$SDK_LIB_ROOT/$SDK_VER/um/x64"
      lib_ucrt="$SDK_LIB_ROOT/$SDK_VER/ucrt/x64"
    fi
  else
    # flat xwin-style
    inc_um="$SDK_INC_ROOT/um"
    inc_shared="$SDK_INC_ROOT/shared"
    inc_ucrt="$SDK_INC_ROOT/ucrt"
    lib_um="$SDK_LIB_ROOT/um/x86_64"
    lib_ucrt="$SDK_LIB_ROOT/ucrt/x86_64"
  fi

  [[ -d "$inc_um" && -d "$inc_shared" && -d "$inc_ucrt" ]] || die "Missing SDK include dirs"
  [[ -d "$lib_um" && -d "$lib_ucrt" ]] || die "Missing SDK lib dirs"

  # Export for callers
  export CRT_INC="$crt_inc"
  export CRT_LIB="$crt_lib"
  export INC_UM="$inc_um"
  export INC_SHARED="$inc_shared"
  export INC_UCRT="$inc_ucrt"
  export LIB_UM="$lib_um"
  export LIB_UCRT="$lib_ucrt"
}

# ------------------------- Project parsing -----------------------------------
# Extract <ClCompile Include="..."> items from the vcxproj
extract_sources() {
  python3 - "$1" <<'PY'
import sys, xml.etree.ElementTree as ET
path=sys.argv[1]
ns={'msb':'http://schemas.microsoft.com/developer/msbuild/2003'}
t=ET.parse(path); r=t.getroot()
out=[]
for item in r.findall('.//msb:ClCompile', ns):
    inc=item.attrib.get('Include')
    if not inc: continue
    if inc.lower().endswith(('.cpp','.c')):
        out.append(inc.replace('\\','/'))
for s in out:
    print(s)
PY
}

# --------------------------- Build steps --------------------------------------
ensure_deps() {
  [[ -d "$ZLIB_DIR/include" && -f "$ZLIB_DIR/lib/zlib.lib" ]] || die "Missing zlib at $ZLIB_DIR (need include/ and lib/zlib.lib)"
  [[ -d "$LIBPNG_DIR/include" && -f "$LIBPNG_DIR/lib/libpng.lib" ]] || die "Missing libpng at $LIBPNG_DIR (need include/ and lib/libpng.lib)"

  if [[ "$WITH_SLANG" == "1" ]]; then
    [[ -d "$GLSLANG_DIR/include" && -d "$GLSLANG_DIR/lib" ]] || die "WITH_SLANG=1 but missing glslang at $GLSLANG_DIR"
    [[ -d "$SPIRVCROSS_DIR/include" && -d "$SPIRVCROSS_DIR/lib" ]] || die "WITH_SLANG=1 but missing SPIRV-Cross at $SPIRVCROSS_DIR"
  fi
}

compile_res() {
  banner "Compiling resources (.rc -> .res)"

  local rc="win32/rsrc/snes9x.rc"
  [[ -f "$rc" ]] || die "Missing resource file: $rc"

  mkdir -p "$BUILD_DIR/obj" "$OUT_DIR"

  if command -v "$RC_TOOL" >/dev/null 2>&1; then
    "$RC_TOOL" /nologo /fo "$BUILD_DIR/obj/snes9x.res" "$rc"
  elif command -v windres >/dev/null 2>&1; then
    windres -O coff -i "$rc" -o "$BUILD_DIR/obj/snes9x.res"
  else
    die "No resource compiler found (llvm-rc or windres)"
  fi
}

compile_objects() {
  banner "Compiling sources ($CONFIG)"

  local proj="win32/snes9xw.vcxproj"
  [[ -f "$proj" ]] || die "Missing project: $proj"

  mapfile -t SOURCES < <(extract_sources "$proj")
  [[ ${#SOURCES[@]} -gt 0 ]] || die "No sources extracted from $proj"

  mkdir -p "$BUILD_DIR/obj" "$OUT_DIR"

  # common defines (keep these minimal; rely on existing code’s #ifdefs)
  local defs=(
    "-DWIN32" "-D_WIN32" "-DWIN64" "-D_WIN64"
    "-DUNICODE" "-D_UNICODE"
    "-D_CRT_SECURE_NO_WARNINGS"
    "-DZLIB_STATIC" "-DPNG_STATIC"
  )

  # If you want to hard-disable slang support at compile time:
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

  # include paths
  local inc=(
    "-I." "-Iwin32" "-Iwin32/rsrc"
    "-I$ZLIB_DIR/include"
    "-I$LIBPNG_DIR/include"
  )

  if [[ "$WITH_SLANG" == "1" ]]; then
    inc+=("-I$GLSLANG_DIR/include" "-I$SPIRVCROSS_DIR/include")
  fi

  # SDK includes (MSVC mode)
  local msvc_inc=(
    "/I$CRT_INC" "/I$INC_UCRT" "/I$INC_UM" "/I$INC_SHARED"
  )

  # Compile
  local cflags=(
    "--target=$TARGET_TRIPLE"
    "/nologo"
    "/std:c++20"
    "/EHsc"
    "/MT"
    "-fuse-ld=$LINKER"
    "${opt[@]}"
    "${defs[@]}"
    "${inc[@]}"
    "${msvc_inc[@]}"
  )

  printf "${CY}Extracted %d sources from %s${C0}\n" "${#SOURCES[@]}" "$proj"

  # Compile each source to obj, preserving directories inside obj/ to avoid name collisions
  # (e.g., foo/bar.cpp -> obj/foo_bar.obj)
  local src obj
  for src in "${SOURCES[@]}"; do
    [[ -f "$src" ]] || continue
    obj="$BUILD_DIR/obj/$(echo "$src" | sed 's#/#_#g' | sed 's/\.[cC][pP][pP]$/.obj/' | sed 's/\.c$/.obj/')"
    printf "%s %s\n" "$src" "$obj" >> "$BUILD_DIR/compile_manifest.txt"
  done

  # parallel compile (xargs)
  cat "$BUILD_DIR/compile_manifest.txt" \
    | xargs -P "$JOBS" -n 2 bash -lc '
        src="$0"; obj="$1";
        mkdir -p "$(dirname "$obj")";
        clang-cl '"${cflags[*]}"' /c "$src" /Fo"$obj" >/dev/null
      '

  echo "${CG}✅ Compile complete${C0}"
}

link_exe() {
  banner "Linking snes9x.exe"

  local objs=("$BUILD_DIR/obj/"*.obj)
  [[ -f "${objs[0]}" ]] || die "No object files found in $BUILD_DIR/obj"

  local libs=(
    # SDK libs
    "/libpath:$CRT_LIB/x86_64"
    "/libpath:$LIB_UCRT"
    "/libpath:$LIB_UM"
    "kernel32.lib" "user32.lib" "gdi32.lib" "comdlg32.lib" "advapi32.lib"
    "shell32.lib" "ole32.lib" "oleaut32.lib" "winmm.lib" "shlwapi.lib"
    "d3d9.lib" "dxguid.lib"
    # your static deps
    "$ZLIB_DIR/lib/zlib.lib"
    "$LIBPNG_DIR/lib/libpng.lib"
  )

  if [[ "$WITH_SLANG" == "1" ]]; then
    # Your built libs may have slightly different names; adjust if needed.
    libs+=(
      "/libpath:$GLSLANG_DIR/lib"
      "/libpath:$SPIRVCROSS_DIR/lib"
      "glslang.lib" "SPIRV.lib" "SPVRemapper.lib"
      "spirv-cross-core.lib" "spirv-cross-glsl.lib" "spirv-cross-util.lib"
    )
  fi

  local out="$OUT_DIR/snes9x.exe"
  mkdir -p "$OUT_DIR"

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

  echo "${CG}✅ Built: $out${C0}"
}

clean() {
  banner "Cleaning"
  rm -rf "$BUILD_DIR" "$OUT_DIR"
  echo "${CG}✅ Clean${C0}"
}

usage() {
  cat <<EOF
Usage: $0 {build|clean}

Env overrides:
  CONFIG=Release|Debug
  WITH_SLANG=1|0
  WINSDK_BASE=/opt/winsdk
  ZLIB_DIR=/opt/windows-libs/zlib
  LIBPNG_DIR=/opt/windows-libs/libpng
  GLSLANG_DIR=/opt/windows-libs/glslang
  SPIRVCROSS_DIR=/opt/windows-libs/spirv-cross
EOF
}

main() {
  local cmd="${1:-build}"

  case "$cmd" in
    build)
      banner "Detecting Windows SDK"
      detect_winsdk
      sdk_flags
      ensure_deps
      compile_res
      compile_objects
      link_exe
      ;;
    clean) clean ;;
    *) usage; exit 1 ;;
  esac
}

main "$@"
