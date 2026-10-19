# Building Mesa-VirGL for macOS 10.6 (Snow Leopard)

Cross-compile from Apple Silicon to x86_64-apple-macos10.6 using the
10.6 SDK, libcxx 5.0.1 from leopard-webkit-build, and llvm-ar.

## Prerequisites

- **Xcode** with the 10.6 SDK installed at
  `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk`
- **libcxx 5.0.1** vendored in `cross-compat/libcxx/` (headers + dylibs,
  originally from the leopard-webkit-build project)
- **Homebrew LLVM** for `llvm-ar` / `llvm-ranlib` at
  `/opt/homebrew/opt/llvm/bin/`
- **meson >= 1.0**, **ninja**, **Python 3 with mako**, **bison**, **flex**

## Patches applied to upstream Mesa

All patches live in the `cross-10.6` branch. They are mechanical
portability fixes, not behavioural changes (except the TLS gate — see
below).

### Source patches

| File | Change | Reason |
|------|--------|--------|
| `meson.build` | Skip `find_library('rt')` on darwin, keep `cc.has_function('clock_gettime')` as primary | macOS has no librt; `clock_gettime` is either native (10.12+) or provided by the cross-compat shim. Detection via `has_function` remains the authoritative check; darwin special-case only avoids the librt probe that would fail. |
| `src/c11/impl/time.c` | Forward-declare `clock_gettime`, gated on `__has_include(<_time.h>)` | 10.6 SDK has no `<_time.h>` and doesn't declare `clock_gettime`; modern macOS SDK (CI) has both and declares it with `clockid_t`. The `__has_include` gate tracks the actual root cause without being corrupted by deployment-target env quirks. |
| `src/util/compress.h` | `#include <stddef.h>` | 10.6 SDK doesn't transitively expose `size_t` through `inttypes.h` |
| `src/util/fossilize_db.c` | Forward-declare `strndup` | macOS 10.7+ only |
| `src/util/memstream.c` | Forward-declare `open_memstream` | macOS 10.13+ only |
| `src/util/ralloc.c` | Forward-declare `strnlen` | macOS 10.7+ only |
| `src/util/u_call_once.c` | Gate `thread_local` fallback on macOS < 10.7 | TLS requires macOS 10.7+ dyld. Plain `static` is used only when `__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070`; `thread_local` is kept everywhere else (Linux/glibc, modern macOS). **Correctness compromise on 10.6 only: single-threaded call_once.** |
| `src/util/u_qsort.cpp` | Gate `thread_local` fallback on macOS < 10.7 | Same TLS issue. Plain `static` on 10.6 only; `thread_local` elsewhere. macOS has native BSD `qsort_r` since 10.5 so the fallback path should never execute at runtime anyway. |
| `src/util/u_thread.h` | Gate `__THREAD_INITIAL_EXEC` on macOS >= 10.7 | The GL dispatch path (`src/mapi/`) uses TLS for per-thread current-context. On 10.6, `__THREAD_INITIAL_EXEC` is defined empty so the dispatch state variables declared with it (`_glapi_tls_Dispatch`, `_glapi_tls_Context`, `__glX_tls_Context`, `_egl_TLS`) become plain globals. **Correctness compromise on 10.6: single-threaded GL only.** |

### TLS gate — behavioural risk

The `u_thread.h` patch is the one change with runtime behavioural
risk, not just compile-time portability. Falling back to plain globals
for `_glapi_tls_Dispatch` / `_glapi_tls_Context` is correct only if
nothing calls into the GL dispatch from multiple threads
concurrently. Mesa's GL dispatch is single-context-per-thread by
design, so it is likely fine for a first port — but it should be
marked as such so nobody later assumes it's safe under threading.

### Compat files (in `cross-compat/`)

| File | Purpose |
|------|---------|
| `mesa-cross-10.6.txt` | Meson cross file: SDK, libcxx, llvm-ar, all `-D` defines |
| `clock_gettime_compat.c` | Shims: `clock_gettime` (gettimeofday), `strndup`, `strnlen`, `open_memstream` (built on `funopen()`, not tmpfile — see the file header: "GNU/BSD open_memstream built on funopen()") |
| `math_compat.h` | 36 missing C99 math declarations (llrint, llround, trunc, etc.) — force-included via `cpp_args` |
| `compat-headers/string.h` | Wrapper adding `strndup` / `strnlen` declarations via `#include_next` |
| `compat-headers/xf86drm.h` | DRM stubs (all `abort()`). Source: fork CI workflow, commits `1577651647d` and `8681e0ec7d9` |
| `compat-headers/libsync.h` | Sync fence stubs. Source: same fork CI workflow |
| `vl_video_stubs.c` | 5 `vl_video_buffer_*` stubs (all `abort()`). Reason: `video-codecs=[]` disables vl/ subsystem |
| `dri_stubs.c` | 3 `dri{ParseConfigFiles,QueryOption{b,i}}` stubs (return defaults). Reason: meson feature resolution for expat fails in cross mode |

## Build steps

### 1. Compile compat objects

```sh
SDK=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX10.6.sdk

for f in clock_gettime_compat vl_video_stubs dri_stubs; do
  clang -target x86_64-apple-macos10.6 -arch x86_64 \
    -isysroot "$SDK" -fallow-unsupported -w \
    -c cross-compat/${f}.c -o cross-compat/${f}.o
done
```

All three objects are linked into every target via the cross file's
`c_link_args` / `cpp_link_args`. `clock_gettime_compat.o` provides the
POSIX clock shim; `dri_stubs.o` and `vl_video_stubs.o` resolve the
driver-init references that virgl pulls in (`driParseConfigFiles`,
`driQueryOptionb/i`, `vl_video_buffer_*`) — xmlconfig.c and the vl
subsystem are not compiled in this cross build.

### 2. Configure

Before running `meson setup`, substitute the `@SOURCE_ROOT@` placeholder
in the cross file with the absolute path to your repo root:

```sh
sed -i.bak "s|@SOURCE_ROOT@|$(pwd)|" cross-compat/mesa-cross-10.6.txt
```

The cross file also references `/Applications/Xcode.app/.../MacOSX10.6.sdk`
for the 10.6 SDK and `/opt/homebrew/opt/llvm/bin/llvm-ar` for llvm-ar;
adjust those if your toolchain is in a different location.

Then:

```sh
meson setup build-106 \
  --cross-file cross-compat/mesa-cross-10.6.txt \
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
```

### 3. Build

```sh
ninja -C build-106 -j2
```

Produces 913 targets. The OSMesa target links softpipe but not virgl
(meson only links software rasterizers into OSMesa).

### 4. Force-link virgl into OSMesa (mandatory)

Meson only links software rasterizers into the OSMesa target by design.
Since `inline_sw_helper.h` references `virgl_iokit_winsys_wrap` (gated on
`GALLIUM_VIRGL && __APPLE__`), the compiled `osmesa_target.c.o` has an
undefined reference to it. The meson-generated link command contains zero
virgl archives, so the ninja link step **fails by design** — it cannot
resolve the three virgl symbols:

```
Undefined symbols for architecture x86_64:
  "_virgl_create_screen", referenced from: _osmesa_create_screen
  "_virgl_iokit_winsys_wrap", referenced from: _osmesa_create_screen
  "_virgl_vtest_winsys_wrap", referenced from: _osmesa_create_screen
```

The force-link step re-runs the recorded link command with all four virgl
archives force-loaded, plus `-framework IOKit` (libvirgliokit.a calls
`IOConnectCallMethod` / `IOServiceOpen`; the iokit meson.build declares
the framework dependency, but since virgl enters via `-force_load` rather
than through meson's dependency graph, meson never contributes the flag).

The automated script `cross-compat/build-10.6.sh` handles this end-to-end:
it tolerates the expected ninja link failure, runs the force-link, and
verifies the result. The equivalent manual commands:

```sh
cd build-106
eval "$(ninja -t commands src/gallium/targets/osmesa/libOSMesa.8.dylib | tail -1 | \
  sed 's|src/gallium/drivers/softpipe/libsoftpipe.a|& \
    -Wl,-force_load,src/gallium/drivers/virgl/libvirgl.a \
    -Wl,-force_load,src/gallium/winsys/virgl/vtest/libvirglvtest.a \
    -Wl,-force_load,src/gallium/winsys/virgl/common/libvirglcommon.a \
    -Wl,-force_load,src/gallium/winsys/virgl/iokit/libvirgliokit.a|')" \
  -framework IOKit
```

Result: `build-106/src/gallium/targets/osmesa/libOSMesa.8.dylib`
(~19 MB, Mach-O 64-bit x86_64).

Two more substitutions the script applies before `eval` — easy to miss
manually, and without them the manual link binds the *shipped* libc++
dylibs instead of the static archives, so the result differs from the
scripted one:

```sh
# (a) replace both libc++ reexports with the static archives in
#     cross-compat/libcxx/lib (see the libc++ section above), then
# (b) append -nostdlib++ so libc++ never enters the link
PATCHED=$(printf '%s' "$LINK_CMD" \
  | sed 's|-Wl,-reexport_library,[^ ]*libc++.1.dylib|<libcxx>/libc++.a|g' \
  | sed 's|-Wl,-reexport_library,[^ ]*libc++abi.dylib|<libcxx>/libc++abi.a|g')
PATCHED="$PATCHED -nostdlib++"
```

Verify with (note the path — after `cd build-106` the dylib is three
directories down, exactly as in the "Result" line above; `nm libOSMesa.8.dylib`
from there finds nothing):

```sh
nm src/gallium/targets/osmesa/libOSMesa.8.dylib | grep virgl_iokit >/dev/null \
  && echo "iokit: present" || echo "iokit: ABSENT"
```

**The `--no-force-link` flag in build-10.6.sh is a legacy option that predates
the iokit dispatch path.** With `inline_sw_helper.h` referencing
`virgl_iokit_winsys_wrap`, the meson link cannot succeed without the
force-load. `--no-force-link` produces no dylib at all — the ninja link
fails and there is no fallback.

## Cross file reference

The cross file (`cross-compat/mesa-cross-10.6.txt`) sets:

- **Target:** `x86_64-apple-macos10.6`
- **SDK:** 10.6 SDK via `-isysroot`
- **libcxx:** 5.0.1 headers via `-nostdinc++ -isystem`, libraries via `-Wl,-reexport_library`
- **llvm-ar:** `/opt/homebrew/opt/llvm/bin/llvm-ar` (GNU ar produces misaligned archives; Apple's ar produces corrupted member names; libtool has flag mismatches with meson)
- **Warnings:** `-w` (Mesa's per-warning `-Werror=foo` flags survive `-Dwerror=false` and `-Wno-error`; `-w` is the only way to suppress all of them for a cross-build)
- **Defines:** `HAVE_STRUCT_TIMESPEC`, `HAVE_PTHREAD`, `CLOCK_REALTIME=0`, `CLOCK_MONOTONIC=1`, `O_CLOEXEC=0x01000000`, `static_assert=_Static_assert`
- **C++ math:** `-include cross-compat/math_compat.h` (36 missing C99 math declarations)
- **Compat objects:** linked via `c_link_args` / `cpp_link_args`

## What doesn't work yet

- **TLS:** macOS 10.6 has no thread-local storage runtime. All
  `thread_local` / `__thread` usages patched to plain `static` **only on
  macOS < 10.7** (`#if !defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__)
  || __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 1070`): 10.7+ has real
  TLS for these usages, and emulated TLS (`-femulated-tls`) is only needed
  for the 10.6 target. The GL
  dispatch path falls back to global variables — single-threaded only.
- **libGL.dylib:** not produced. GLX/EGL/GBM all disabled; no CGL
  binding layer exists yet. The CGL shim is the next project.
- **OSMesa with virgl:** meson doesn't link virgl into the OSMesa
  target, so the ninja link step fails by design (three undefined virgl
  symbols from `osmesa_target.c.o`). The force-link step is mandatory,
  not optional — it resolves those symbols by force-loading the four
  virgl archives + IOKit framework. Automated by
  `cross-compat/build-10.6.sh`.

## Scoping

This build proves Mesa's source compiles and links for 10.6 x86_64.
It does not prove the resulting binary loads, runs, or renders
correctly on 10.6.8 — that requires the OSMesa+softpipe render test
in the guest.
