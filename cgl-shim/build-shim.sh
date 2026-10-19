#!/bin/bash
#
# build-shim.sh — build cgl_shim.dylib for x86_64-apple-macos10.6
#
# The shim intercepts NSOpenGLContext (ObjC swizzle) and _CGL* functions
# (__DATA,__interpose) to route GL through OSMesa → virgl → host GPU.
# Loaded via DYLD_INSERT_LIBRARIES; links the Mesa OSMesa dylib built by
# cross-compat/build-10.6.sh.
#
# Run from the Mesa-VirGL repo root:  ./cgl-shim/build-shim.sh
#
set -euo pipefail

SHIM_DIR="cgl-shim"
BUILD_DIR="build-106"
SDK="/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk"
MESA_INCLUDE="include"
LIBCXX_DIR="cross-compat/libcxx/lib"
OSMESA="$BUILD_DIR/src/gallium/targets/osmesa/libOSMesa.8.dylib"
GLAPI="$BUILD_DIR/src/mapi/shared-glapi/libglapi.0.dylib"
OUTPUT="$SHIM_DIR/cgl_shim.dylib"

JOBS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --clean) rm -f "$SHIM_DIR/cgl_shim.o" "$SHIM_DIR/cgl_interpose.o" "$OUTPUT" ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

fail() { echo "ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------- checks
[ -f "meson.build" ] || fail "run from the Mesa-VirGL repo root (no meson.build here)"
[ -d "$SDK" ]        || fail "10.6 SDK not found at $SDK"
[ -f "$MESA_INCLUDE/GL/osmesa.h" ] || fail "Mesa osmesa.h not found at $MESA_INCLUDE/GL/"
[ -f "$OSMESA" ]     || fail "libOSMesa.8.dylib not found at $OSMESA — run cross-compat/build-10.6.sh first"
[ -f "$GLAPI" ]      || fail "libglapi.0.dylib not found at $GLAPI — run cross-compat/build-10.6.sh first"
[ -f "$SHIM_DIR/cgl_shim.mm" ] || fail "cgl_shim.mm not found in $SHIM_DIR/"
[ -f "$SHIM_DIR/cgl_interpose.c" ] || fail "cgl_interpose.c not found in $SHIM_DIR/"

BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
if [ "$BRANCH" != "cross-10.6" ]; then
    echo "WARNING: on branch '$BRANCH', not 'cross-10.6'."
    read -r -p "         Continue anyway? [y/N] " reply
    [ "$reply" = "y" ] || exit 1
fi

# ---------------------------------------------------------------- build
# Two source files compiled in one clang++ invocation:
#   cgl_shim.mm     — ObjC++ (swizzles, timing, OSMesa calls)
#   cgl_interpose.c — C (_CGL* function interposes)
#
# Frameworks: Cocoa (AppKit/Foundation), OpenGL (CGL types),
# IOKit (referenced by cgl_interpose's CGL token plumbing),
# QuartzCore (CALayer import in cgl_shim.mm).
#
# Link paths use -L pointing at the build-106 output so the linker finds
# the dylibs at build time. At runtime, @rpath resolves via
# DYLD_LIBRARY_PATH on the guest (Mesa dylibs deployed to /tmp).

echo "==> building $OUTPUT"

# Mesa's headers pair with Mesa's library — use include/GL/gl.h and
# include/GL/osmesa.h from this repo, not the 10.6 SDK's XQuartz copy.
# XQuartz's GL/gl.h has a hardcoded #include of an absolute system path
# that doesn't exist under -isysroot, and even if it did, mixing
# XQuartz's GL declarations against Mesa's OSMesa ABI is wrong.
# -I"$MESA_INCLUDE" must come before any system GL include path.
#
# -DGLAPI=extern: the shim #imports <OpenGL/OpenGL.h> (for CGL types),
# which sets __gl_h_ and blocks Mesa's <GL/gl.h> from processing. Without
# this, GLAPI / APIENTRY / GL types are all undefined when osmesa.h is
# later included.
#
# -include $MESA_INCLUDE/GL/gl.h: force Mesa's gl.h to be processed FIRST,
# before #import <OpenGL/OpenGL.h> can set __gl_h_. Mesa's gl.h defines
# GLAPI, APIENTRY, GLAPIENTRY and all GL types. When the system OpenGL
# headers later try to include their own gl.h, __gl_h_ is already set and
# the system copy is skipped — but Mesa's definitions are already in
# scope. This is the correct pairing: Mesa headers with Mesa library.
#
# SILENT-FAILURE RISK: this mechanism is order-dependent. If the -include
# flag is ever dropped, or if a future compiler/SDK change reorders the
# processing, the system's GL declarations win the guard race and the
# shim compiles against Apple's GL types while linking against Mesa's
# OSMesa ABI — no diagnostic, just wrong types at the boundary. The
# OSMesa symbol cross-check below catches link-time mismatches but not
# type-ABI mismatches. A compile-time #error guard in the shim source
# (checking for a Mesa-specific marker if __gl_h_ is set) would close
# that gap; deferred until it bites.
#
# Two compile steps: cgl_shim.mm as ObjC++, cgl_interpose.c as C (the
# interpose file uses dlsym→void* assigned to function pointers, which
# is valid C but not valid C++).
COMMON_CFLAGS=(
    -target x86_64-apple-macos10.6
    -isysroot "$SDK"
    -fallow-unsupported
    -Wno-deprecated
    -include "$MESA_INCLUDE/GL/gl.h"
    -I"$SHIM_DIR"
    -I"$MESA_INCLUDE"
)

echo "  [1/3] clang++  cgl_shim.mm"
clang++ "${COMMON_CFLAGS[@]}" -std=c++11 \
    -c "$SHIM_DIR/cgl_shim.mm" -o "$SHIM_DIR/cgl_shim.o"

echo "  [2/3] clang    cgl_interpose.c"
clang "${COMMON_CFLAGS[@]}" \
    -c "$SHIM_DIR/cgl_interpose.c" -o "$SHIM_DIR/cgl_interpose.o"

echo "  [3/3] clang++  link → cgl_shim.dylib"
clang++ "${COMMON_CFLAGS[@]}" \
    -dynamiclib \
    -install_name @rpath/cgl_shim.dylib \
    -o "$OUTPUT" \
    "$SHIM_DIR/cgl_shim.o" \
    "$SHIM_DIR/cgl_interpose.o" \
    "$SHIM_DIR/substitute_stub.o" \
    -L"$(dirname "$OSMESA")" \
    -L"$(dirname "$GLAPI")" \
    -L"$LIBCXX_DIR" \
    -framework Cocoa \
    -framework OpenGL \
    -framework IOSurface \
    -framework IOKit \
    -framework QuartzCore \
    -lOSMesa.8 \
    -lglapi.0 \
    -Wl,-rpath,@loader_path \
    -Wl,-rpath,@loader_path/../../"$BUILD_DIR"/src/gallium/targets/osmesa \
    -Wl,-rpath,@loader_path/../../"$BUILD_DIR"/src/mapi/shared-glapi

# ---------------------------------------------------------------- verify
echo "==> verifying $OUTPUT"
file "$OUTPUT" | grep -q 'x86_64' || fail "not an x86_64 Mach-O"

# The shim exports three C-linkage registry functions and installs an
# ObjC +load that swizzles NSOpenGLContext. If shim_register is missing,
# the +load path didn't compile or link — DYLD_INSERT_LIBRARIES would
# load the dylib but no swizzles would fire (silent no-op).
if ! nm -g "$OUTPUT" 2>/dev/null | grep -q 'T _shim_register'; then
    fail "shim_register not exported — the swizzle +load path is broken"
fi
if ! nm -g "$OUTPUT" 2>/dev/null | grep -q 'T _shim_lookup'; then
    fail "shim_lookup not exported — the registry is broken"
fi

# GL interpose binding check: glClear must bind to OpenGL, not libOSMesa.
# If it binds to libOSMesa, the DYLD_INTERPOSE tuple targets Mesa's glClear
# instead of Apple's — the interpose would replace Mesa's implementation
# while the app's calls stay on Apple's GL. Present as "interpose installed,
# nothing changed." Link order must put -framework OpenGL before -lOSMesa.
# NOTE: no `grep -q` here — under `set -o pipefail` grep -q closes the pipe
# early, nm dies on SIGPIPE and the whole `if` fails spuriously on a GOOD
# artifact (observed 3 times out of 4 builds; same rule documented in
# cross-compat/build-10.6.sh's verify section). grep without -q reads to EOF.
if /usr/bin/nm -m "$OUTPUT" 2>/dev/null | grep '_glClear.*from OpenGL' >/dev/null; then
    echo "    glClear binding: OpenGL (correct)"
elif /usr/bin/nm -m "$OUTPUT" 2>/dev/null | grep '_glClear.*from libOSMesa' >/dev/null; then
    fail "glClear binds to libOSMesa, not OpenGL — link order is wrong. The interpose would target Mesa's glClear instead of Apple's. Put -framework OpenGL before -lOSMesa in the link command."
else
    fail "glClear binding not found in nm -m output — unexpected link state"
fi

UNDEF=$(nm -u "$OUTPUT" 2>/dev/null | grep -v '^ *$' | wc -l | tr -d ' ')
echo "    undefined symbols: $UNDEF (should resolve against libSystem/libcxx/Mesa at runtime)"
echo "    shim_register: present"
echo "    shim_lookup: present"

# Cross-check: every OSMesa* symbol the shim needs must be exported by
# libOSMesa.8.dylib. "Links clean" at build time doesn't guarantee
# "loads at runtime" — if the shim references a symbol the library
# doesn't export, dlopen fails in the guest several minutes and a deploy
# later, naming the symbol but not the cause. Same failure class as the
# virgl force-link check in build-10.6.sh.
WANT=$(mktemp)
HAVE=$(mktemp)
nm -u "$OUTPUT" 2>/dev/null | grep '_OSMesa' | sed 's/^ *U *_//' | sort -u > "$WANT"
nm -g "$OSMESA" 2>/dev/null | awk '$2=="T"{print $3}' | sed 's/^_//' | sort -u > "$HAVE"
MISSING=$(comm -23 "$WANT" "$HAVE")
WANT_COUNT=$(wc -l < "$WANT" | tr -d ' ')
rm -f "$WANT" "$HAVE"
if [ -n "$MISSING" ]; then
    fail "shim references OSMesa symbols not exported by $OSMESA:\n$MISSING"
fi
echo "    OSMesa symbol cross-check: $WANT_COUNT referenced symbols resolved"

SIZE=$(du -h "$OUTPUT" | cut -f1)
echo
echo "Built: $OUTPUT ($SIZE)"

# ---------------------------------------------------------------- killtest
# killtest_shim is the NSOpenGLContext test app that exercises the shim's
# full flushBuffer → glFinish → glReadPixels → swap → drawRect path.
# Links only Cocoa + OpenGL (no Mesa) — the shim is injected at runtime
# via DYLD_INSERT_LIBRARIES. Time-based rotation: constant angular velocity
# regardless of frame rate, so frame-rate improvements are visible as
# smoothness changes, not speed changes.
KILLTEST="$SHIM_DIR/killtest/killtest_shim"
KILLTEST_SRC="$SHIM_DIR/killtest/killtest_shim.mm"

if [ -f "$KILLTEST_SRC" ]; then
    echo
    echo "==> building $KILLTEST"
    clang++ \
        -target x86_64-apple-macos10.6 \
        -isysroot "$SDK" \
        -fallow-unsupported \
        -std=c++11 \
        -Wno-deprecated \
        -o "$KILLTEST" \
        "$KILLTEST_SRC" \
        -framework Cocoa \
        -framework OpenGL

    file "$KILLTEST" | grep -q 'x86_64' || fail "killtest_shim not x86_64"
    KT_SIZE=$(du -h "$KILLTEST" | cut -f1)
    echo "    killtest_shim: x86_64 ($KT_SIZE)"
else
    echo "==> killtest_shim.mm not found, skipping killtest build"
fi

echo
echo "Deploy to guest alongside libOSMesa.8.dylib and libglapi.0.dylib."
echo "Load with: DYLD_INSERT_LIBRARIES=/path/to/cgl_shim.dylib ..."
