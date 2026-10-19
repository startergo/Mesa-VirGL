#!/bin/bash
#
# build-10.6.sh — cross-compile Mesa-VirGL for x86_64-apple-macos10.6
#
# Companion to cross-compat/build_10.6.md, which explains WHY each step
# exists. This script is the executable form; the doc stays authoritative
# for the reasoning and for the patch table.
#
# Run from the repo root:  ./cross-compat/build-10.6.sh [--clean] [--jobs N]
#
set -euo pipefail

BUILD_DIR="build-106"
JOBS=4
CLEAN=0
FORCE_LINK=1

SDK="/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk"
LLVM_AR="/opt/homebrew/opt/llvm/bin/llvm-ar"
CROSS_FILE="cross-compat/mesa-cross-10.6.txt"

while [ $# -gt 0 ]; do
    case "$1" in
        --clean)      CLEAN=1 ;;
        --jobs)       JOBS="$2"; shift ;;
        --no-force-link) FORCE_LINK=0 ;;
        --build-dir)  BUILD_DIR="$2"; shift ;;
        -h|--help)
            sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

fail() { echo "ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------- checks
# Every one of these has cost a debugging cycle when missing. Check up
# front rather than failing 400 targets into a build.

[ -f "meson.build" ] || fail "run from the Mesa-VirGL repo root (no meson.build here)"
[ -d "$SDK" ]        || fail "10.6 SDK not found at $SDK"
[ -x "$LLVM_AR" ]    || fail "llvm-ar not found at $LLVM_AR — GNU ar and Apple's ar both produce archives ld64 rejects"
[ -d "cross-compat/libcxx" ] || fail "cross-compat/libcxx missing (libcxx 5.0.1 headers + dylibs)"
[ -f "$CROSS_FILE" ] || fail "cross file missing: $CROSS_FILE"

BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")
# A checkout that contains cross-10.6 (an agent branch made from it) carries the patches; a caller
# that verified the commit itself (VMQemuVGA scripts/build-mesa.sh: the pinned submodule commit)
# sets CROSS106_CHECKED=1. Either way: no prompt.
if [ "$BRANCH" != "cross-10.6" ]; then
    if [ "${CROSS106_CHECKED:-0}" = 1 ]; then
        BRANCH="cross-10.6"
        echo "NOTE: branch check bypassed — caller set CROSS106_CHECKED=1"
    else
        for ref in cross-10.6 origin/cross-10.6; do
            if git rev-parse -q --verify "$ref" >/dev/null && git merge-base --is-ancestor "$ref" HEAD 2>/dev/null; then
                BRANCH="cross-10.6"; break
            fi
        done
    fi
fi
if [ "$BRANCH" != "cross-10.6" ] && [ ! -t 0 ]; then
    fail "on '$BRANCH', which does not contain cross-10.6 (non-interactive: no prompt). If you verified this checkout carries the 10.6 patches (an agent branch made from cross-10.6, or the pinned submodule commit), rerun with CROSS106_CHECKED=1."
fi
if [ "$BRANCH" != "cross-10.6" ]; then
    echo "WARNING: on branch '$BRANCH', not 'cross-10.6'."
    echo "         The 10.6 portability patches live on cross-10.6; this build"
    echo "         will fail or silently miss them elsewhere."
    read -r -p "         Continue anyway? [y/N] " reply
    [ "$reply" = "y" ] || exit 1
fi

for tool in meson ninja bison flex; do
    command -v "$tool" >/dev/null || fail "$tool not on PATH"
done
python3 -c 'import mako' 2>/dev/null || fail "python3 mako module not installed"

# -------------------------------------------------- cross-file interpolation
# The tracked cross file is a TEMPLATE — it carries @SOURCE_ROOT@ placeholders
# that must be substituted with the actual checkout path before meson sees it.
# Substituting in place dirties the tracked file (git status shows machine-
# specific paths forever after, and someone eventually commits them with an
# unrelated change). Write the interpolated copy into the build directory
# instead; the tracked template stays pristine.
CROSS_FILE_TEMPLATE="$CROSS_FILE"
CROSS_FILE="$BUILD_DIR/mesa-cross-10.6.interpolated.txt"
mkdir -p "$BUILD_DIR"
echo "==> interpolating @SOURCE_ROOT@ -> $(pwd)"
sed "s|@SOURCE_ROOT@|$(pwd)|g" "$CROSS_FILE_TEMPLATE" > "$CROSS_FILE"

if ! grep -q "$(pwd)" "$CROSS_FILE"; then
    echo "ERROR: interpolation did not substitute $(pwd) into $CROSS_FILE"
    echo "       template was $CROSS_FILE_TEMPLATE"
    exit 1
fi

# ------------------------------------------------------------- configure
# Use build.ninja (created by meson setup) as the "already configured" marker,
# not [ -d "$BUILD_DIR" ] — we just created BUILD_DIR to hold the interpolated
# cross file.
if [ "$CLEAN" = "1" ] && [ -f "$BUILD_DIR/build.ninja" ]; then
    echo "==> removing $BUILD_DIR (clean reconfigure)"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"
    sed "s|@SOURCE_ROOT@|$(pwd)|g" "$CROSS_FILE_TEMPLATE" > "$CROSS_FILE"
fi

if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "==> meson setup $BUILD_DIR"
    meson setup "$BUILD_DIR" \
        --cross-file "$CROSS_FILE" \
        -Dgallium-drivers=virgl,softpipe \
        -Dglx=disabled \
        -Degl=disabled \
        -Dgbm=disabled \
        -Dllvm=disabled \
        -Dshared-glapi=enabled \
        -Dplatforms=[] \
        -Dwerror=false \
        -Dshader-cache=disabled \
        -Dosmesa=true \
        -Dxmlconfig=enabled \
        -Dvideo-codecs=[]
else
    echo "==> reusing existing $BUILD_DIR (pass --clean to reconfigure)"
fi

# ----------------------------------------------------------------- build
# The OSMesa dylib link step is EXPECTED to fail — meson doesn't include
# virgl archives in the OSMesa target by design. All prior targets
# (compilation + static archives) must succeed; only the final dylib link
# fails with undefined virgl symbols. The force-link step below re-links
# with all four virgl archives and produces the final dylib.
# -k 0: keep building past the expected OSMesa link failure. Without it,
# ninja aborts the whole run at that failure and whether static archives
# (libosmesa_st.a, libmesa.a) got re-archived first is scheduler luck —
# measured 2026-10-09: a patched osmesa.c compiled but the dylib linked
# the stale August archive, silently reverting the change in the artifact.
echo "==> ninja -C $BUILD_DIR -j$JOBS -k 0"
set +e
ninja -C "$BUILD_DIR" -j"$JOBS" -k 0
NINJA_RC=$?
set -e

OSMESA="$BUILD_DIR/src/gallium/targets/osmesa/libOSMesa.8.dylib"

if [ "$NINJA_RC" -ne 0 ]; then
    # Verify the failure is only the OSMesa link, not a compilation error.
    # All four virgl archives must exist (built before the link step).
    for a in libvirgl.a libvirgliokit.a libvirglvtest.a libvirglcommon.a; do
        found=$(find "$BUILD_DIR/src/gallium" -name "$a" 2>/dev/null | head -1)
        [ -n "$found" ] || fail "ninja failed and $a is missing — real compilation error, not the expected OSMesa link failure"
    done
    echo "==> ninja failed at OSMesa link as expected — force-link will resolve"
fi

# ------------------------------------------------- force-link virgl into OSMesa
# meson links only software rasterizers into the OSMesa target by design,
# so virgl must be force-loaded. Re-run the recorded link command with the
# four virgl archives inserted. Without this, GALLIUM_DRIVER=virgl finds
# no driver and silently falls back.
#
# The four archives:
#   libvirgl.a        — the driver (provides virgl_create_screen)
#   libvirglvtest.a   — vtest winsys (the "virpipe" dispatch path)
#   libvirglcommon.a  — shared virgl winsys code
#   libvirgliokit.a   — the IOKit winsys (the "virgl" dispatch path)
#
# osmesa_target.c.o has an undefined reference to virgl_iokit_winsys_wrap
# (gated on GALLIUM_VIRGL && __APPLE__ in inline_sw_helper.h). Without
# libvirgliokit.a force-loaded, that reference is unresolved and the virgl
# driver loads but cannot reach the kext.

if [ "$FORCE_LINK" = "1" ]; then
    echo "==> force-linking virgl into OSMesa"
    LINK_CMD=$(ninja -C "$BUILD_DIR" -t commands \
        src/gallium/targets/osmesa/libOSMesa.8.dylib | tail -1)

    case "$LINK_CMD" in
        *libsoftpipe.a*) ;;
        *) fail "OSMesa link command does not mention libsoftpipe.a — the sed anchor is stale, check ninja -t commands output" ;;
    esac

    PATCHED=$(printf '%s' "$LINK_CMD" | sed 's|src/gallium/drivers/softpipe/libsoftpipe.a|& -Wl,-force_load,src/gallium/drivers/virgl/libvirgl.a -Wl,-force_load,src/gallium/winsys/virgl/vtest/libvirglvtest.a -Wl,-force_load,src/gallium/winsys/virgl/common/libvirglcommon.a -Wl,-force_load,src/gallium/winsys/virgl/iokit/libvirgliokit.a|')

    # libvirgliokit.a calls IOConnectCallMethod, IOServiceOpen, etc. The iokit
    # winsys meson.build doesn't declare an IOKit framework dependency, and
    # since the OSMesa target gets virgl via force-load (not via meson's
    # dependency chain), meson never adds -framework IOKit. Append it here.
    PATCHED="$PATCHED -framework IOKit"

    # Statically link libc++ instead of re-exporting the dynamic version.
    # The cross file's link args include -Wl,-reexport_library for libc++,
    # which produces LC_REEXPORT_DYLIB load commands. When libOSMesa is
    # re-exported from the substitute OpenGL.framework, dyld processes
    # the libc++ re-export during framework initialization — before the
    # host process is fully set up. libc++'s ios_base::Init static
    # initializer runs too early, calls through a NULL function pointer,
    # and crashes. Statically linking libc++ into libOSMesa eliminates
    # the dynamic dependency entirely — no init-order conflict, no
    # duplicate C++ runtime when loaded into a host app that bundles its
    # own libc++. Mesa's C++ (GLSL compiler) is entirely internal; the
    # GL entry points are C. No C++ symbols cross the ABI boundary.
    LIBCXX_DIR="$(cd "$(dirname "$0")/libcxx/lib" && pwd)"
    PATCHED=$(printf '%s' "$PATCHED" \
        | sed "s|-Wl,-reexport_library,[^ ]*libc++.1.dylib|$LIBCXX_DIR/libc++.a|g" \
        | sed "s|-Wl,-reexport_library,[^ ]*libc++abi.dylib|$LIBCXX_DIR/libc++abi.a|g")
    PATCHED="$PATCHED -nostdlib++"

    ( cd "$BUILD_DIR" && eval "$PATCHED" )
fi

[ -f "$OSMESA" ] || fail "force-link finished but $OSMESA does not exist"

# ---------------------------------------------------------------- verify
# Compile-clean has meant "doesn't work" more than once on this project.
# These are cheap post-conditions, not a substitute for the guest render test.

echo "==> verifying $OSMESA"
file "$OSMESA" | grep -q 'x86_64' || fail "not an x86_64 Mach-O"

UNDEF=$(nm -u "$OSMESA" 2>/dev/null | grep -v '^ *$' | wc -l | tr -d ' ')
echo "    undefined symbols: $UNDEF (all should resolve against libSystem/libcxx)"

if [ "$FORCE_LINK" = "1" ]; then
    # NOTE: grep -q is FORBIDDEN here. Under `set -o pipefail`, grep -q's
    # early exit SIGPIPEs nm (exit 141) and fails the pipeline on a GOOD
    # artifact — a flaky false "virgl symbols absent" (observed 2026-08-18:
    # build succeeded, dylib contained 120 _virgl* T-symbols, verify failed).
    # Plain grep reads all input; no early exit, no SIGPIPE.
    if nm "$OSMESA" 2>/dev/null | grep 'virgl_drm_winsys\|virgl_vtest_winsys\|virgl_create_screen' > /dev/null; then
        echo "    virgl: present"
    else
        fail "virgl symbols absent after force-link — GALLIUM_DRIVER=virgl would silently fall back"
    fi
    if ! nm "$OSMESA" 2>/dev/null | grep 'virgl_iokit' > /dev/null; then
        fail "virgl_iokit_winsys symbols absent — GALLIUM_DRIVER=virgl would find the driver but have no iokit transport, falling back through vtest (fails at runtime, no socket)"
    fi
    echo "    virgl_iokit_winsys: present"
fi

SIZE=$(du -h "$OSMESA" | cut -f1)
echo
echo "Built: $OSMESA ($SIZE)"
echo
echo "This proves Mesa compiles and links for 10.6 x86_64. It does NOT prove"
echo "the binary loads, runs, or renders on 10.6.8 — run the OSMesa+softpipe"
echo "render test in the guest for that:"
echo
echo "    DYLD_LIBRARY_PATH=/tmp GALLIUM_DRIVER=softpipe ./osmesa_softpipe_test"
echo "    DYLD_LIBRARY_PATH=/tmp GALLIUM_DRIVER=virgl    ./virgl_clear_test"
