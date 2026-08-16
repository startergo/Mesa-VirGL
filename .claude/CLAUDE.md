# Mesa-VirGL — repo-local rules

Branch: `cross-10.6`. This repo is Mesa cross-built for macOS 10.6 x86_64,
plus the `cgl-shim/` (per-process GL interpose) and
`cgl-shim/substitute/` (drop-in `OpenGL.framework`) that route real apps
onto it. The kext lives in the sibling repo `~/VMQemuVGA`; the
cross-project ground rules (negative controls, one variable per run,
commit before running, never state an inference as a fact) live in
`~/VMQemuVGA/.claude/CLAUDE.md` and apply here unchanged.

Each rule below cost a session to learn. Sources cited are in this repo.

## Build

**Build only with `cross-compat/build-10.6.sh` (Mesa) and
`cgl-shim/build-shim.sh` / `cgl-shim/substitute/build-substitute.sh`
(the dylibs). Do not reason about build settings** — same rule as the
kext repo.

**The force-link step is MANDATORY** (`cross-compat/build_10.6.md:137-193`,
commit `7687f4c`). Meson's OSMesa target links softpipe but NOT virgl —
the four virgl objects must be appended by re-running the recorded link
command. `build-10.6.sh` does this automatically and tolerates the expected
ninja link failure. `--no-force-link` is a legacy flag that produces NO
dylib at all — the ninja link failure is expected, not the result.

**`-include` ordering for Mesa's gl.h is load-bearing**
(`cgl-shim/build-shim.sh:69-106`). Mesa's headers must pair with Mesa's
library: `-I"$MESA_INCLUDE"` before any system GL path, and
`-include "$MESA_INCLUDE/GL/gl.h"` forces Mesa's gl.h to be processed
FIRST, before `<OpenGL/OpenGL.h>` sets `__gl_h_`. If anything includes its
own gl.h first, `__gl_h_` is already set and Mesa's declarations are
silently skipped — compiles fine, breaks at runtime. This mechanism is
order-dependent and fails silently; if GL symbols misbehave, check this
before anything else.

## Runtime

**`GALLIUM_DRIVER` must be set before the first context creation**
(`cgl-shim/cgl_shim.mm:400-411`, commit `85da3f4`). Mesa selects the
driver in a `call_once` when the first context is created; the shim sets
`GALLIUM_DRIVER=virgl` in a constructor (respecting a pre-set value).
Never patch the driver selection to be unconditional — that made softpipe
unreachable at runtime once and cost the bisect tool.

**The softpipe bisect is the first instrument for any wrong-pixels bug.**
The same binary with `GALLIUM_DRIVER=softpipe` renders on the CPU through
Mesa only — no virgl, no winsys, no kext, no host. Identical output under
softpipe and virgl ⇒ the fault is downstream of Mesa (winsys / kext /
host); differs ⇒ the fault is in Mesa's state tracker or the virgl
encoder. Every pixel-correctness investigation starts here.

**Pixel-store state is context-global** (`substitute_cgl.c:466-488`).
`GL_UNPACK_ALIGNMENT` / `GL_UNPACK_ROW_LENGTH` set for an internal upload
leak into the app's next texture upload unless saved/restored. The units
bug here (ROW_LENGTH reset to hardcoded 0) shifted sub-4-byte texture
rows horizontally — glyph atlases at progressive offsets.

## Structure

- `cgl-shim/` — per-process interpose route (flat-namespace test binaries).
- `cgl-shim/substitute/` — substitute `OpenGL.framework` for two-level
  namespace real apps (the route Gecko uses). `prepare_real_opengl.sh`
  pulls the real framework for re-export.
- `cross-compat/` — cross file, compat shims (`math_compat.h`,
  `clock_gettime_compat.c`, `dri_stubs.c`), and the build procedure doc.
- `cgl-shim/killtest/` — test programs: `osmesa_softpipe_test`,
  `triangle_test`, `textured_triangle_test` (direct-OSMesa, flat
  namespace), `killtest_shim`, `shim_smoke_test`, `stress_test` (shim
  route), `virgl_clear_test`, `blit_test`.
- The single-threaded-only `u_thread.h` TLS gate is a correctness
  compromise, not a portability shim — do not remove it casually.

**This repo now has `LEDGER.md`** — update it at the end of every session,
same rules as the kext repo. Before 2026-08-16 its state lived only in
commit messages.
