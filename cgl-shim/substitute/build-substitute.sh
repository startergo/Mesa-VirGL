#!/bin/bash
#
# build-substitute.sh — build the substitute OpenGL.framework
#
# The substitute is a thin x86_64 dylib that:
#   - Has install_name matching the real OpenGL.framework
#   - Re-exports libOSMesa for gl* (so dlsym finds Mesa)
#   - Re-exports libOpenGL_real (renamed copy of real OpenGL) for CGL
#   - Defines ~12 CGL symbols wrapping OSMesa (overriding the re-exports)
#   - Includes the NSOpenGLContext swizzle from cgl_shim.mm
#
# Deploy via DYLD_FRAMEWORK_PATH. Gate test (2026-08-12) confirmed
# DYLD_FRAMEWORK_PATH overrides absolute install_names on 10.6's dyld.
#
set -euo pipefail

SHIM_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MESA_ROOT="$(cd "$SHIM_DIR/.." && pwd)"
SUBST_DIR="$SHIM_DIR/substitute"
SDK="/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk"
OSMESA="$MESA_ROOT/build-106/src/gallium/targets/osmesa/libOSMesa.8.dylib"
GLAPI="$MESA_ROOT/build-106/src/mapi/shared-glapi/libglapi.0.dylib"
LIBCXX="$MESA_ROOT/cross-compat/libcxx/lib"
REAL_GL="$SUBST_DIR/libOpenGL_real_x86_64"

OUTPUT="$SUBST_DIR/OpenGL.framework/Versions/A/OpenGL"

# ---------------------------------------------------------------- checks
[ -f "$OSMESA" ] || { echo "ERROR: libOSMesa.8.dylib not found at $OSMESA"; exit 1; }
[ -f "$GLAPI" ] || { echo "ERROR: libglapi.0.dylib not found at $GLAPI"; exit 1; }
[ -f "$REAL_GL" ] || { echo "ERROR: libOpenGL_real not found. Run prepare_real_opengl.sh first."; exit 1; }
[ -f "$SHIM_DIR/cgl_shim.mm" ] || { echo "ERROR: cgl_shim.mm not found"; exit 1; }

echo "==> building substitute OpenGL.framework"

# ---------------------------------------------------------------- compile
COMMON_CFLAGS=(
    -target x86_64-apple-macos10.6
    -isysroot "$SDK"
    -fallow-unsupported
    -Wno-deprecated
    -include "$MESA_ROOT/include/GL/gl.h"
    -I"$SHIM_DIR"
    -I"$MESA_ROOT/include"
)

echo "  [1/3] clang    cgl_shim.m (ObjC — no libc++ dependency)"
clang "${COMMON_CFLAGS[@]}" \
    -c "$SHIM_DIR/cgl_shim.mm" -o "$SUBST_DIR/cgl_shim.o"

echo "  [2/3] clang    cgl_interpose.c (no interpose section)"
clang "${COMMON_CFLAGS[@]}" -DSUBSTITUTE_FRAMEWORK \
    -c "$SHIM_DIR/cgl_interpose.c" -o "$SUBST_DIR/cgl_interpose.o"

echo "  [3/4] clang    substitute_cgl.c"
clang "${COMMON_CFLAGS[@]}" \
    -c "$SUBST_DIR/substitute_cgl.c" -o "$SUBST_DIR/substitute_cgl.o"

echo "  [4/4] clang    glu_shim.c (GLU subset — shadows the Apple re-export)"
clang "${COMMON_CFLAGS[@]}" \
    -c "$SUBST_DIR/glu_shim.c" -o "$SUBST_DIR/glu_shim.o"

# ---------------------------------------------------------------- link
echo "  [4/4] clang    link → OpenGL.framework (no libc++)"
mkdir -p "$(dirname "$OUTPUT")"
clang "${COMMON_CFLAGS[@]}" \
    -dynamiclib \
    -install_name /System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL \
    -compatibility_version 1.0.0 \
    -current_version 1.0.0 \
    -o "$OUTPUT" \
    "$SUBST_DIR/cgl_shim.o" \
    "$SUBST_DIR/cgl_interpose.o" \
    "$SUBST_DIR/substitute_cgl.o" \
    "$SUBST_DIR/glu_shim.o" \
    -L"$(dirname "$OSMESA")" \
    -L"$(dirname "$GLAPI")" \
    -L"$LIBCXX" \
    -framework Cocoa \
    -framework IOKit \
    -framework IOSurface \
    -framework QuartzCore \
    -lglapi.0 \
    -Wl,-exported_symbol,_gluBuild2DMipmaps \
    -Wl,-exported_symbol,_gluScaleImage \
    -Wl,-exported_symbol,_gluErrorString \
    -Wl,-exported_symbol,_gluPerspective \
    -Wl,-exported_symbol,_gluLookAt \
    -Wl,-exported_symbol,_gluOrtho2D \
    -Wl,-exported_symbol,_gluPickMatrix \
    -Wl,-exported_symbol,_glUniform4fv \
    -Wl,-exported_symbol,_glUniformMatrix4fv \
    -Wl,-exported_symbol,_glUniform2f \
    -Wl,-exported_symbol,_glDrawArrays \
    -Wl,-exported_symbol,_glBindTexture \
    -Wl,-reexport_library,"$SUBST_DIR/libOSMesa.8.dylib" \
    -Wl,-reexport_library,"$REAL_GL" \
    -Wl,-rpath,@loader_path \
    -Wl,-rpath,@loader_path/../../.. \
    -Wl,-not_for_dyld_shared_cache

# Patch __objc_imageinfo flags for GC compatibility. The cross-compiler
# on Apple Silicon can't emit -fobjc-gc (deprecated post-10.8). The
# section attribute in substitute_cgl.c creates the section but zeroes
# the flags. Post-build patch sets flags to OBJC_IMAGE_SUPPORTS_GC (2)
# so GC-required apps (System Preferences, others) accept the image.
OBJC_INFO_OFF=$(otool -l "$OUTPUT" 2>/dev/null | awk '/sectname __objc_imageinfo/{found=1} found && /^ *offset/{print $2; exit}')
if [ -n "$OBJC_INFO_OFF" ]; then
    printf '\x02\x00\x00\x00' | dd of="$OUTPUT" bs=1 seek=$((OBJC_INFO_OFF + 4)) conv=notrunc 2>/dev/null
    echo "    GC flag patched at offset $((OBJC_INFO_OFF + 4))"
else
    echo "WARNING: __objc_imageinfo section not found — GC apps will reject this image"
fi

# ---------------------------------------------------------------- verify
echo "==> verifying $OUTPUT"
file "$OUTPUT" | grep -q 'x86_64' || { echo "ERROR: not x86_64"; exit 1; }

otool -D "$OUTPUT" | grep -q "OpenGL.framework" || { echo "ERROR: wrong install_name"; exit 1; }

echo "    install_name: $(otool -D "$OUTPUT" | tail -1)"
echo "    CGL exports: $(nm -gU "$OUTPUT" 2>/dev/null | grep -c ' T _CGL')"
echo "    gl* exports: $(nm -gU "$OUTPUT" 2>/dev/null | grep -cE ' T _gl[A-Z]')"
echo "    re-exports: $(otool -l "$OUTPUT" 2>/dev/null | grep -c LC_REEXPORT_DYLIB)"

SIZE=$(du -h "$OUTPUT" | cut -f1)
echo
echo "Built: $OUTPUT ($SIZE)"
echo
echo "Deploy alongside libOpenGL_real_x86_64 + libOSMesa.8.dylib + libglapi.0.dylib"
echo "Load with: DYLD_FRAMEWORK_PATH=/path/to/OpenGL.framework/..  (parent dir)"
