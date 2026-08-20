# Mesa-VirGL — Ledger

Current state of the 3D stack. **Volatile — update at the end of every
session.** Sibling repo: `~/VMQemuVGA` (kext; its ledger covers the transport
and the 2D accelerator surface path — a separate stack with no GL in it).
Durable rules for THIS repo live in `.claude/CLAUDE.md`; the cross-project
ground rules (negative controls, one variable per run, commit before
running) live in `~/VMQemuVGA/.claude/CLAUDE.md` and apply here too.

Rules for maintaining this file: same as the VMQemuVGA ledger — record what
the evidence supports; "verified" requires a negative control or a visual
check; never quietly drop an unexplained residual; mark inferences.

**This ledger was created 2026-08-16.** State before that date lives in
commit messages; the summary below is attributed to them and was not
re-verified at creation time.

Last updated: 2026-08-19 night (FENCE ERA LANDED AND VERIFIED (kext bf8d36a + winsys c462bdbb): 25× batch throughput, zero drain-fallbacks/timeouts, DRM VIRTGPU_WAIT contract end-to-end. White→BLACK transition = the compositor now ASSEMBLES and PRESENTS (blit runs) but its program fails: **glUniform3("uLayerRects" vec4-not-vec3) / glUniform1("uLayerTransform" is-matrix) — the compositor's OWN uniforms mismatching = object-table crossing between Gecko's two GL threads. CARRIER HYPOTHESIS: the u_thread.h TLS gate (single-threaded-GL plain globals) — CONFIRMED REQUIRED (Apple clang REFUSES __thread for macos10.6 targets even with -femulated-tls: "thread-local storage is not supported for the current target"; modern Mesa DELETED the pthread-TSD fallback, so per-thread contexts = restoring it ourselves in u_current). OMTC-off pref test incomplete (launch died); pref `layers.offmainthreadcomposition.enabled=false` is STILL SET in both user.js — remove or complete before the next browser test. Next: restore u_current pthread-TSD storage; entry 20 tail)


**HANDOFF SERIALIZATION LANDED + VALIDATED-2/2 + THE INPUT/CONNECTING STALL (09:20–09:50):**
- Implemented (53b2604050f + hotfix): one global handoff mutex around the
  whole context switch via a single choke point (shim_osmesa_make_current,
  all 12+ switch/rebind sites routed); owner tracking logs
  HANDOFF-VIOLATION on concurrent same-context use; queue lock re-landed
  (301d44d60b0). LOCK ORDER INVARIANT: sc->lock → handoff → queue.
- **First build self-deadlocked** (bulk-regex routed the helper's OWN
  OSMesaMakeCurrent through itself — two stacked helper frames in the
  sample, dock-stuck). Fixed; substitute 320ad16c.
- **Validation: 2/2 clean starts** — chrome RENDERED both runs (fields
  and buttons visible), errs=0 (losing runs showed 11–12 program-not-
  linked), viol=0 (switch-only serialization sufficient — zero
  concurrent same-context use observed). 4 more runs queued for the
  6-count when the guest is next used.
- **New stall face (both runs): "connecting" on a file:// URL + input
  fields dead.** Not network (DNS+TCP verified working from the guest:
  example.com 200). Sample: main thread 83% inside
  PaintWindow→Refresh (the flush-wait chain), compositor actively
  inside RecvFlushRendering→CompositeToTarget — composites RUN (chrome
  stays rendered) but the synchronous flush cycle owns the main
  thread: event loop starves (typing dead), page-load completion
  callbacks never run ("connecting" forever on a local file — the
  docloader park of the prior session is this same stall).
  Samples: /tmp/v4s.txt, /tmp/ho.txt.
- **Frontier (unchanged target, new precision): per-flush cost.** The
  recorded design queue applies: fewer full-frame readbacks per
  composite (present path), async flush semantics — measure per-flush
  wall first (shim timing stats) on the next session.
**SCREENSHOT DATUM + THE PER-FLUSH NUMBER (09:50–09:57):**
- **A one-glClear page (no shaders at all) reports cleared + getError
  0x0 and the canvas is STILL WHITE** — the page-shader family is
  REMOVED as a necessary ingredient of the empty-canvas problem. The
  frontier reframes to: the delivery path between a correct GL result
  and the canvas. Title-bar cautions (non-findings): webgl2:FAILED is
  expected (GL 2.1/GLSL 1.20 cannot back WebGL2's ES-3.0-class
  context); renderer:"Mozilla" is Gecko's sanitised string — says
  nothing about virgl vs software.
- **Per-flush wall measured (v4 run, rolling stats): mean 130–138 ms,
  min 18, max 342 — split submit 44–68 ms + transfer 61–76 ms; lock
  0.1 ms.** Against the ~16 ms the event loop needs: even the best
  flush exceeds it; the synchronous wait per display cycle is UI death
  (input dead, "connecting" forever), not slowness. The 5.5 MB/frame ×
  22 fps ≈ 120 MB/s guest-side copy sits IN FRONT of the event loop.
  **Host-side blit is thereby promoted from performance idea to THE
  fix for the stall**; the submit half (44–68 ms vs the fence era's
  0–3 ms batches) is per-flush fence round trips under TCG — the
  second fix target.
**HOST-SIDE BLIT IMPLEMENTED (late session; network died before the
deploy-and-boot — artifacts built, undeployed):**
- Kext `9c907d8` (binary 31a042b2): 0x600C selector → `hostBlit3D` —
  kext-owned display-blit ctx (lazy; BOTH resources CTX_ATTACHed),
  VIRGL_CCMD_BLIT (cmd 16, len 21; header 0x00150010) with **src-Y
  INVERTED** for the GL→scanout flip, dst format B8G8R8A8_UNORM, src
  fmt/dims from the geometry table (new userResourceFmt accessor),
  RESOURCE_FLUSH after. **Overwrite rule:** transferToHost2D calls
  hostBlitReissueIfIntersecting — any guest transfer intersecting the
  stored window rect re-issues the blit (guest memory never holds the
  window's pixels; ~17 ms refresh cadence would erase it otherwise).
- Mesa `959f1d86038` + `9265b1acd54`: winsys flush_frontbuffer
  IMPLEMENTED (was an empty stub — the vtable hole): fence-waits the
  resource (contract: prior 3D complete host-side before the blit
  reads) then calls 0x600C with the window rect. OSMesa's front
  snapshot AND the shim's forced glReadPixels both gated off under
  SHIM_HOST_PRESENT (the 61-76 ms transfer half of the 130 ms
  per-flush wall). Rect state lives in the WINSYS (one-way dep — the
  substitute dlsym-resolves the entry points, not link-time); the
  shim's drawRect (AppKit-safe main thread) computes view→desktop
  coords (top-left, screen height − bottom − height) and calls the
  setter; in host-present mode drawRect skips the CG blit.
- Artifacts BUILT: kext 31a042b2 (in build/Release), dylib ff1c5258,
  substitute (96K, dlsym build). **UNDEPLOYED — the guest went
  unreachable mid-scp (mDNS dead AND sl-ip timeout; the recorded
  reachability≠state class; NOT a guest-crash conclusion until
  checked at the display).**
- Pre-registered for the boot: (a) window content appears IN PLACE on
  the desktop (desktop intact around it) → delivery path confirmed as
  the empty-canvas carrier AND the transfer split collapses (~0);
  (b) nothing appears → the host resource's content is wrong → the
  carrier is upstream of delivery (what the compositor renders into
  the front resource); (c) fullscreen-browser artifacts → the
  rect/coords math is wrong (check HOST-BLIT rect log lines).
  Watch: hostBlit attach/submit FAILs, and the re-blit's cost at tick
  rate (heartbeat logs).
**HOST-BLIT DEBUG ARC (Aug 20, 13:57–17:20) — three silent dead links
found and fixed; blit now FIRES end-to-end; wall = host-side verdict:**
1. **dlsym visibility** (dylib ff1c5258 era): the winsys entry points
   compiled under Mesa's -fvisibility=hidden — nm shows them as T but
   they never reach the dylib's export trie, so the substitute's
   dlsym returned NULL and host_present_mode() silently read 0.
   Diagnosed by splitting the gates: mode → local getenv in the shim;
   setter failure → loud log. Fix: visibility("default") on the three
   (dylib eedc500f). Guest nm failing on the substitute (1-line
   output) is a 10.6 nm artifact, NOT a stale file (digests matched).
2. **The chain instrumented link by link** (dylib c68503b0): OSMesa's
   HOST-PRESENT branch fires (pipe+screen valid) → winsys
   flush_frontbuffer HOOK fires (res=287, lvl=0) → rect stash fires
   (1280x821 at (4,44) — plausible desktop coords). All logged.
3. **The kext never received 0x600C** because the first handler
   returned kIOReturnNotReady SILENTLY: m_display_resource_id was 0 —
   setupDisplayResource NEVER RAN (0 log lines) and neither did
   setscanout (0 lines) — **this boot's desktop runs in VGA
   compatibility mode** (BAR-0 auto-display; the recorded two-mode
   note). The live desktop surface = the refresh timer's transfer
   target = the framebuffer's resource 1 (convention). Fix (kext
   e44d8598): m_hblit_dst_res — setscanout() records scanout-0
   bindings; fallback 1; dst attach-cache separate from the 3D res.
4. **BLIT FIRES**: "hostBlit ctx=3 created (dst res 1)", both
   attachments OK, BLIT+RESOURCE_FLUSH submitted, zero failure
   responses, hook fires per flush. **Nothing visible on screen.**
   Per the pre-registered outcomes this is (b): the carrier is either
   the host resource's content or a host-side rejection invisible
   from the guest BY CONSTRUCTION.
- **WAITING ON: UTM Debug Log toggle** (GUI action, VM settings) —
  no debug log exists on disk (searched twice). virglrenderer's
  verdict on a BLIT into a RESOURCE_CREATE_2D resource (the prime
  suspect: vrend may reject/no-op blits on 2D-created resources) is
  only visible there. Path gets recorded in the ledger once found.
- Sub-suspects if the log clears the blit: dims/rect mismatch (1280
  window on the desktop), src resource 287's content being empty
  (which would FOLD this into the empty-canvas frontier — the blit
  faithfully delivering nothing).
---

## 2026-08-19 (entry 21) — fence era landed; the white→black transition names the carrier class

**Fence era (deployed, boot-verified):** kext `bf8d36a` (252700bd) +
winsys `c462bdbb` — FCE1-framed handle lists per submit, per-resource
last_seq, 0x600B WAIT block/NOWAIT. **70,922 batches in ~3 min (~400/s,
25× the pre-fence rate), 0–3 ms each, zero drain-fallback calls, zero
fence timeouts**, wall 43→18 ms. Both of the day's transport bugs are
structurally impossible in this design.

**The face changed: WHITE → BLACK.** Per the recorded guard mechanics:
white = blit SKIPPED (transparent frame, Gecko's white software paint
stands); black = blit RUNS — the compositor now assembles an opaque
framebuffer, clears it, and PRESENTS it. The fence era moved the
compositor from "renders nothing" to "renders an FBO without working
draws".

**The error census names why — and it is the COMPOSITOR'S OWN program:**
```
35 × glUniform3("uLayerRects"@2 has 4 components, not 3)
35 × glUniform1(uniform "uLayerTransform"@5 is matrix)
13 × glBindTexture(target mismatch)
```
uLayerRects/uLayerTransform are CompositorOGL's layer-quad shader
uniforms. The program LINKS but its uniform declarations don't match
Gecko's SET calls — object tables from two contexts crossing. Same
family as `glFramebufferTexture2D(non-existent texture 2)` (an id-2
collision) and the target mismatches: ALL object-table errors.

**Carrier hypothesis (fits everything incl. the fish
run's luck): the u_thread.h TLS gate.** 10.6 build defines
`__THREAD_INITIAL_EXEC` EMPTY → `_glapi_tls_Dispatch/_glapi_tls_Context`
are PLAIN GLOBALS → Gecko's two GL threads (main chrome ctx + compositor
thread) stomp each other's current-context/dispatch on every MakeCurrent.
The 2026-08-18 fish/aquarium run was one-context-lucky timing.

**Gate-removal attempt — the gate's REAL reason found (corrects the
ledger's own note):** with `-femulated-tls` present in the compile line,
Apple clang still refuses: `glapi.h:84: error: thread-local storage is
not supported for the current target` (x86_64-apple-macos10.6). The
gate is a HARD COMPILER REFUSAL, not a dyld assumption. AND modern Mesa
deleted the pthread-TSD fallback from u_current (TLS-only upstream) —
so per-thread contexts on 10.6 = RESTORING the old design ourselves.

**TSD RESTORATION — LANDED AND VERIFIED (17:30–17:56, commit
`503d14cadde`, dylib `19d3f756479252d233b6319a27ad1888`):**
- The historical pre-2015 u_current design recovered via
  `git show 18db13f5865^:src/mapi/u_current.c` and ported under
  `MAPI_PTHREAD_TSD` (u_thread.h's 10.6 branch): plain-global fast
  path; ThreadSafe flips 0→1 on a second thread's MakeCurrent (checked
  from both set entry points, one mutex); pthread_once keys;
  GET_DISPATCH/GET_CURRENT_CONTEXT via the accessors (Windows shape);
  dead `_glapi_tls_*` kept for ABI. No pthread_getspecific on the hot
  path while single-threaded — the historical property, preserved.
- **VERIFIED on the fence-era boot (no kext change, subst swap only):
  the object-table error family went to ZERO** — uLayerRects (was
  ×35), uLayerTransform (was ×35), target mismatches (was ×13) all 0
  in the first 90 s; stayed 0 across a second run. The two-thread
  stomp was the carrier of the uniform mismatches. TSD dylib also
  carries the full fence winsys (0x600b ×14).
- **Frames STILL uniformly zero** (8 skip-dumps, nonzero=16 = header
  only) and the window still white — but the remaining census is now
  PURELY the recorded page-context family (entry 18's open item):
  glBindTexture target mismatch ×11, webgl_ uniform errors, non-existent
  texture 2 ×8, glReadPixels incomplete fb — the PAGE's WebGL context,
  not the compositor's. Transport, sync, per-thread contexts, and the
  compositor's own program are all fixed and verified; the empty layer
  tree's last standing carrier is the client-side content path.
- Cleanup done this run: the OMTC pref removed from both user.js
  (test was never completed — launch died; recorded as inconclusive).
- Two browsers died cleanly mid-run without crash reports (16:11,
  ~17:52) — exit cause unknown; not chased (no crash signature; the
  17:54 relaunch survived).

**SOFTPIPE BISECT ATTEMPTED — BLOCKED BY A NEW DEATH CLASS (18:00–18:15):**

**PRE-TSD COMPARISON (18:16–18:24) — TSD LINE VINDICATED; my crash
attribution flipped twice in one hour (recorded in order):**
- Pre-TSD fence dylib (b52686ad, mapi files at 503d14cadde^) +
  softpipe, foreground: **immediate SEGFAULT (exit 139), zero frames**
  — and its crash report (181841) carries the SAME garbage-PC
  compositor signature (0x13f13103a) as the TSD-era crashes, in a
  binary with NO TSD code. My earlier attribution of ALL garbage-PC
  crashes to the TSD NULL bug was WRONG; the class has ≥2 carriers.
- **The ladder, re-read:**
  | dylib | softpipe compositor fate | cause |
  |---|---|---|
  | 3565cf02 (async era, 14:40) | 3 min, frame 11, teardown crash | stomp timing luck |
  | b52686ad (pre-TSD, fence) | immediate garbage-PC segfault | the ORIGINAL two-thread stomp (plain-global dispatch) |
  | 19d3f756 (TSD, no noop) | garbage-PC ~45 s | MY NULL-table porting bug |
  | a85609b3/cce52813 (TSD+noop) | **42 s of real layer drawing**, symbolized mid-draw crash | the only config softpipe ever composited |
- **Conclusions:** the noop fix was load-bearing; the TSD restoration
  is confirmed directional (pre-TSD is WORSE — the stomp crash
  returns); the empty-layer-tree verdict from the 18:09:18 crash
  (softpipe HAS ImageLayers and draws them; virgl composes uniformly
  zero) STANDS — the carrier is virgl-path-dependent, not Gecko-glue.
- Artifacts: /tmp/libOSMesa_preTSD.dylib (b52686ad) preserved; guest
  now runs cce52813 (TSD+noop+fences; 0x600b ×14, pthread_getspecific
  present). Softpipe's remaining DrawGeometry SIGSEGV (real stack,
  layers present) is a SEPARATE queued defect — not read against the
  bisect verdict.

**SESSION CLOSE (18:24) — the exit-0 death class gets a prime suspect
and the instrument notes:**
- **LaunchServices `GetOurLSSessionIDInit` SIGABRT ×2** (18:10:50,
  18:19:11) — 10.6's LS daemon wedging under RAPID RELAUNCH (8+
  launches/20 min today) is now the leading suspect for the clean
  exit-0 deaths that ended otherwise-healthy runs (the layers run:
  **353 frames composited** — the most ever before a clean death —
  wall mean 61 ms, then gone, no crash report). Next session: no more
  rapid relaunch cycles; pause between launches; if deaths persist on
  a quiet cadence, lsregister/launchservicesd reset is the queued
  remedy.
- **MOZ_LOG=Layers:5 produced ZERO output** on the virgl run — module
  name or env form wrong for this Gecko 52 build. Next: try
  NSPR_LOG_MODULES=Layers:5, then MOZ_LOG=getflattenedtree:5 /
  LayerManager variants; check the build's log-module list before
  burning another run.
- Guest state: cce52813 subst + fence kext installed; browsers dead;
  artifacts preserved (preTSD dylib at /tmp/libOSMesa_preTSD.dylib,
  fish kext at /tmp/VMQemuVGA_fish.kext); crash series 18:04–18:19 all
  banked above.

**CLOSE-OUT CORRECTIONS:**
1. **The softpipe-vs-virgl A/B is NOT yet controlled.** Softpipe dies
   almost immediately — "softpipe fills layers" describes its FIRST
   frames only; a full virgl session is being weighed against a
   handful of softpipe frames. The frontier statement is downgraded
   to: "softpipe's EARLY frames carry ImageLayers; virgl's steady-
   state frames compose zero." Whether softpipe fills the SAME content
   virgl leaves empty — or just fills early — is the open comparison;
   it needs softpipe surviving longer (fresh profile may enable that).
2. **The LaunchServices SIGABRT is a SYMPTOM of the storm, not an
   independent fault** — consistent with the recorded 10.6 rule
   (launching a GUI app without an Aqua session aborts in
   GetOurLSSessionIDInit; a crash-loop relaunch racing session state
   hits the same path). Evidence: 3 SIGABRTs in all history (08-13
   01:29 loop era; today 18:10+18:19) — all in storm contexts, none
   isolated. Do not chase it as a fault; avoid it by not
   crash-looping.
3. **Profile contamination = the SIXTH confound and the gating one**
   (the five before were about timing; this one invalidates any run).
   FRESH-PROFILE BASELINE is the first action of the next session —
   everything else is uninterpretable until it exists.

**FRESH-PROFILE BASELINE RUNS (18:30–18:48) — CONTENT VISIBLE, FIRST
TIME; the profile-prefs confound in full:**
- **baseline1 (fresh profile, NO prefs)**: white↔BLACK ALTERNATION
  within one session (transparent frames = blit
  skips, opaque = blit runs — the compositor alternates; flat grey
  traffic lights = Gecko CoreGraphics showing on skip), DOUBLE corner
  stamps (drawInTitlebar default returned — the pref, not code, closed
  that class), error storm: glUniform(program not linked) ×17, target
  mismatch ×15, texSubImage ×7. **An unconfigured stack is NOT the
  debugged configuration — every fresh-profile observation without
  user.js is against the wrong stack.** The entire entry-18 error
  family is PREF-DEPENDENT (msaa-samples=0 + force-layers-readback
  suppress it).
- **baseline2 (fresh profile + user.js copied in BEFORE first launch —
  drawInTitlebar=false, msaa=0, webgl2 off, force-layers-readback)**:
  at 4 min — ALIVE, calm (12.8% CPU vs baseline1's 70%), census nearly
  clean (3× GL_MAJOR_VERSION benign + 1 glUseProgram), and **CHROME
  RENDERS — the first visible browser UI of the entire 2026-08-19
  effort.** **The page (webgltest.html) does
  not load** — UI composites, page content absent. That is the new
  frontier, cleanly split.
- **STAMP probe confirmed on-screen** (blue corner
  squares; code read: 8×8 0xCAFE0000|frame blocks at all 4 corners of
  render_buf before each readback — cgl_shim.mm:808+). Squares visible
  = stamps survive readback → present path works end-to-end AND the
  host transfer overwrites ~nothing per frame (empty composition
  delivered faithfully). NOTE: with content now rendering, the
  on-screen stamps are an instrument artifact to remember when reading
  screenshots.
- **NSPR_LOG_MODULES=Layers:5 + NSPR_LOG_FILE=/tmp/layers.log**: file
  created, ZERO bytes — logging inert even in the old form. The
  "Layers" module is nearly silent in release builds (few call sites);
  do not spend more runs on it without first grepping what
  Layers.cpp's sLog actually logs.
- **Anomaly**: /tmp/pf_baseline.log was readable at 18:37:00 and
  unlinked by 18:37:32 while its writer lived — something cleans
  /tmp/pf_*.log on the guest (possibly the parallel session). Flagged.

**FRAME-RATE STARVATION TEST (19:16) — INCONCLUSIVE, but surfaced the
day's sharpest crash (full symbols, our code):**
- Run died at 31 s (crash `powerfox_2026-08-19-191644`):
```
Compositor SIGSEGV:
0  libOSMesa virgl_transfer_queue_clear + 242
1  libOSMesa virgl_flush_eq + 105
2  libOSMesa st_glFlush + 46
3  libOSMesa _mesa_make_current + 252
```
- Code read: the transfer queue is PER-CONTEXT (`ctx->queue`,
  virgl_context.c:1084) with **NO LOCKING anywhere in its API**
  (virgl_transfer_queue.c — clear/unmap/add_internal all bare). Under
  the pre-TSD globals-stomp, cross-thread touches of one context's
  queue were accidentally serialized; TSD removed the accident and the
  race now crashes during a cross-thread MakeCurrent's implicit flush
  (st_glFlush of the thread's previous context). **TSD-EXPOSED hole
  in the virgl driver — not a TSD-port defect.**
- **NEXT (before re-running the starvation test):** enumerate which
  transfer-queue operations can execute off the owning thread
  (make_current flush path is proven one); add locking with that map —
  not a bolt-on mutex. THEN re-run layout.frame_rate=5 on baseline2
  (the test itself never reached measurement).
- **Free checks first (banked):** sharelist conclusively dormant —
  n=6 context creations in the TSD-era run, ALL share=0x0 (Gecko never
  passes a share context on this build; observation over source read).
- **The bisect instrument itself dies:** post-TSD dylib (19d3f756) +
  GALLIUM_DRIVER=softpipe = clean startup death, EXIT=0, ~seconds of
  life (pf_soft3/4/5/6), zero completed frames; the SAME dylib under
  virgl runs minutes. Pre-TSD softpipe (14:40) survived to frame 11.
  - Ruled out: stale profile locks (cleared both .parentlock, death
    recurred); pure remoting handshake (-no-remote extended life past
    25 s and produced ONE dump — shim_skip_00.bmp nonzero=14, a
    DEGENERATE frame (truncated header — zero/tiny dims?) — then died
    before any completed frame).
  - **Correlation, attribution open:** TSD-era dylib × softpipe path =
    lethal; virgl unaffected. Candidates: a defect in MY TSD port that
    softpipe's init path exercises (review the flip sequence vs
    historical first), or a Gecko capability-quit (exit 0 = clean) on
    what softpipe reports post-TSD. NOT diagnosed; pre-registered next
    step below.
- **The day's mystery "clean deaths" partially reattributed:** at least
  the softpipe ones are NOT locks/handshake alone; -no-remote changed
  the survival time. The virgl mid-run deaths (16:11, 17:52) remain
  unexplained (no crash reports, exit unknown — capture next time with
  a foreground run).

**NEXT (revised order):**
1. Diagnose the softpipe death: run with the PRE-TSD fence dylib
   (rebuild from 503d14cadde^ of the mapi files only, or keep a copy)
   to confirm the correlation is TSD-caused; if confirmed, audit the
   TSD flip sequence against the historical original (setters write
   TSD unconditionally; flip nulls globals) — the divergence is the
   bug. MOZ_LOG=Layers:5 (release-build route — MOZ_DUMP_PAINTING is
   compile-gated, inert here) runs on VIRGL regardless of softpipe.
2. VIRGL + MOZ_LOG=Layers:5 launch: does the client fill the layer
   tree at all? (The direct instrument for the empty-tree question.)
3. Text in any softpipe result = uninterpretable (glyph-atlas fault,
   pre-registered).

**PRE-TSD A/B + CRASH-CLASS REFRAME (18:15–18:25, after the parallel
session's softpipe-death series):**
- **TSD EXONERATED for the softpipe death**: pre-TSD fence dylib
  (`cce52813`, 0x600b ×14, no TSD symbols) on softpipe died at
  SECONDS — last log line `gl_interpose_init`, EARLIER than the TSD
  runs reached. Meanwhile VIRGL on the same dylib survived 60 s+ with
  the parallel session's STAMP instrument live (`STAMP[29..30] ...
  survived=0/4` — no stamped pixels survive to present; its own
  empty-frames datum). The driver-dependence is real; the TSD
  correlation was coincidence. Softpipe's instant death remains
  UNATTRIBUTED (candidates: profile crash-storm state, the substitute's
  new STAMP probe, something in the current subst mix — note the 14:40
  pre-fence softpipe run DID survive to frame 11).
- **Garbage-PC compositor crash occurs on PRE-TSD TOO** (18:18:41,
  crash `powerfox_2026-08-19-181841`, Compositor at 0x13f13103a on
  cce52813): plain-globals storage yields the same class — a stale
  other-thread dispatch table jumped mid-swap. **The class IS the
  two-thread problem; my TSD NULL (fixed in 551def4f885) was one
  instance, not the cause.** Per-thread storage remains necessary —
  when TSD returns, the noop-table default must return with it.
- **LaunchServices SIGABRT family recurring** (18:10:50, 18:19:14):
  main-thread abort in GetOurLSSessionIDInit — the crash-relaunch/
  profile-storm era. A black safe-mode dialog appeared in this
  window (chrome-only UI compositing empty).
- Visible state at close: repeated crashes, nothing usable —
  browsers killed; guest left clean.

**NEXT (revised, in order):**
1. **Fresh profile first** (the crash-storm state is now a contaminat-
   ing variable for EVERY launch): `-CreateProfile test` / new profile,
   then one virgl launch on cce52813 — establishes a clean baseline
   before any further bisect.
2. Re-land TSD WITH the noop fix (503d14cadde + 551def4f885 — both
   commits exist; the working tree currently holds the pre-TSD revert)
   once the baseline is clean: the garbage-PC class needs per-thread
   storage in BOTH modes' evidence.
3. MOZ_LOG=Layers:5 virgl run (release-build route) for the layer-tree
   fill question; the STAMP probe's survived=0/4 needs the parallel
   session's instrument read for its own conclusion.

---

## 2026-08-19 (entry 20) — async era deployed; the latency face measured by sample

**Deployment (with kext 45d538e, one pair, one boot — see kext ledger
for the boot verdict):** subst at `/Users/sl/subst` = libOSMesa.8.dylib
md5 `3565cf0254c588cdbf0aef0bb1b313c2` (built 13:17 from faea295 source;
`$0x600a` immediate ×10 confirmed in disassembly), libglapi.0.dylib
`a12d03e8423694d72687b047b7cdbc6a`, substitute OpenGL binary
`725804ea25ead810b39fe957940e799a` — IDENTICAL to the previous deploy,
expected: faea295 touched only winsys code inside libOSMesa; the thin
CGL wrapper is unchanged. `otool -tvV` on the dylib was the deployment
check (nm -m is unavailable on this host's nm).

**The run:** browser (launch_pf.sh, GALLIUM_DRIVER=virgl,
DYLD_FRAMEWORK_PATH=/Users/sl/subst) launched 13:56:15 guest with
webgltest.html. First 3D submit 13:57:47 (kext worker lazy-started).
20 visible submits, 92 host OK responses, transfers both directions,
**zero submit timeouts, zero drain timeouts, zero failures of any class**
— the dropped-compile class that produced the old white-page face is
GONE at the kernel.

**But the window stayed WHITE ≥20 guest-min with no chrome** — and the
mechanism is now sampled, not guessed. `sample` of pid 397 at
14:01:54 / 14:08:43 / 14:13:55 (guest):

- Main thread: `nsViewManager::Refresh →
  PCompositorBridgeChild::SendFlushRendering → PR_WaitCondVar` — parked.
- Compositor thread: `RecvFlushRendering → CompositeToTarget →
  CompositorOGL::EndFrame → GLContextCGL::SwapBuffers →
  shim_flushBuffer → {_mesa_ReadPixels (+3505) | st_glFinish →
  st_manager_flush_frontbuffer → osmesa_st_framebuffer_flush_front →
  osmesa_read_buffer (+2910)} → virgl_resource_transfer_map →
  {virgl_iokit_resource_wait (0x600A drain) ~50% of samples |
  virgl_iokit_transfer_get (0x3009) ~45%} → IOConnectCallMethod →
  mach_msg_trap`.

The stack MOVES between captures (ReadPixels phase → flush_front phase
→ back) — not one stuck call: the compositor progresses through
readbacks at a glacial rate. Model: **each readback = flush_eq submit +
drain (which waits out the worker's submit latency for that batch) +
full-frame 0x3009 round-trip**; flushBuffer performs SEVERAL readbacks
per present (ReadPixels AND the flush_front/read_buffer path);
host starvation (mediaanalysisd 146–166%, load 7.7–9.5, guest clock 7 min
behind host) multiplies every leg. One composite ran >12 guest-min
without returning.

**Attribution discipline:** this is NOT the old white-page face (that
one had 58.9k compositING frames with dead programs; this one has the
compositor inside ONE EndFrame that never returns). NOT the 2026-08-13
AppKit deadlock face (cgl_shim.mm:672-687 — same two-thread posture,
different block point: kernel round-trips, not [view bounds]). Same
family as the "transient startup white (~40 s)" but 30× longer.

**Host hygiene learned:** mediaanalysisd respawns through killall/kill
-9; `launchctl bootout gui/$(id -u)/com.apple.mediaanalysisd` (no sudo)
stops it. spotlightknowledged/mdworker resisted bootout; `sudo mdutil
-a -i off` needs the host password (not attempted).

**NEXT (pre-registered):**
1. Quiet-host relaunch (load <2): prediction — composites complete,
   window paints, wall ≈ the 133–300 ms era. If white persists on a
   quiet host, the readback-carries-submit-cost model is falsified;
   then capture the shim's existing T1–T6 timing splits (they measure
   exactly these phases, cgl_shim.mm:166-174) live in the window.
2. Count the readbacks per present: the sample shows ReadPixels AND
   flush_front→read_buffer in one flushBuffer — audit whether the
   present path can do ONE readback (or none — the kext-side blit
   presents without readback on the 2D path).
3. The blit-skip dump-on-skip instrument (entry 19's queue) gains a new
   motive: on the white face it would show whether ANY presented buffer
   was non-empty.

**AFTERNOON ARC (same day, 14:22–14:44) — the arc that rewrites entry 20's
white-window reading:**
- **The "stall" is a GRIND, measured:** instrumented relaunch
  (SHIM_TIMING=1 VIRGL_IOKIT_DEBUG=1, zero code change, pid 1099) —
  `frame[6604] wall=64–83 ms submit=5.6–16.8 transfer=18.6–49.5
  lock=0.1 ms size=5536096B`, ~22 fps, 13,214 submits + 13,214
  transfer_gets (1531×904 full-frame) + **34 transfer_puts (ALL 18×18
  cursor sprites)** in 5 min. Every "stuck in resource_wait" sample was
  the dominant phase of a loop, not a block. The main thread spends ~65%
  of samples in SendFlushRendering (sync wait per composite).
- **SPARSE-FRAME CONFIRMED, ASYNC ERA, direct evidence:** `blit-skip[1..5]
  empty frame p0=0x00000000` — frames compose fully transparent, nothing
  presents, Gecko's white software paint stands. **Falsifies the
  dropped-compile attribution for emptiness**: zero drops (kext ledger),
  zero Mesa errors on stderr (virgl run), uploads ride INLINE_WRITE in
  the 0x6008 batches (put=34 is not by itself diagnostic). 9 min, 8,154
  frames, still white — "slow startup" is nearly dead as an explanation.
- **Fresh-connection control (user visually confirmed colors cycling):**
  test_virgl_clear on its own user client ran frame 1598+ DURING the
  browser's white grind — device, kext global state, worker, host all
  healthy. Not a shim-lock convoy either (no mutex-parked threads).
- **Softpipe bisect (attempted, aborted at 3 min):**
  frame[11] only (CPU raster under TCG), also blit-skip empty at first
  frames, one `Mesa: error: GL_INVALID_VALUE in glUseProgram`. **PowerFox is known-good
  on real GPUs — Gecko-side hunting is OFF; the fish run (2026-08-18,
  this browser, this path, chrome+content+fish visible) is the
  known-good reference.**
- **Corrections banked (mine):** (1) "8 min quiet-host white = structural
  falsifier" was PREMATURE — killed the run before startup could
  finish; the instrumented run then proved frames flow. (2) The
  "no 3D activity for 10 min" reading was the capped-log artifact
  (kext ledger). (3) Sample snapshots CANNOT distinguish grind from
  wedge when one phase dominates — only counters (frame[i], put/get
  counts) can.

**Teardown crash datum (14:41:15, softpipe run pid 1292, ~20 s after
SIGTERM):** `powerfox_2026-08-19-144119_slqemu.crash` —
EXC_BAD_ACCESS KERN_INVALID_ADDRESS 0x0, Crashed Thread 20 Compositor
with PC at unmapped `0x000000012c33103a` (jump through garbage, zero
frames). Main thread was in MessageChannel::Send/PR_WaitCondVar at
death. Kill-triggered teardown crash — same family as the recorded
"intermittent teardown crash under RecvFlushRendering" (entry 18),
new stack face. NOT chased; not the empty-frame question.

**THE OPEN ITEM (sharp form):** frames compose EMPTY with a clean
transport in BOTH eras (morning sync-6000 white face AND async era) —
so the async pair did NOT introduce it; it REMOVED the drop class and
exposed that emptiness has another carrier. The fish run rendered
content; today's three first-launch-after-boot runs all whited.
**NEXT (pre-registered, one variable): REVERT BOOT to the fish-run-era
pair** (sync-submit kext + 0afb1aa-era winsys — the aquarium-run
configuration) and launch first-after-boot: renders → the async pair is
the regression (then bisect {drain-before-put, is_busy probe, worker});
whites → the regression predates async and the fish run's differing
condition (warm browser? session state?) is what to identify. Second
rung: dump-on-skip instrument (see whether frames are uniformly
transparent or have content regions beyond pixel[0]).

**UPLOAD CENSUS + FRAME-RATE PROBE (same day, 14:45–14:58) — famine
confirmed, starvation FALSIFIED as its carrier:**
- Census instruments found: CGLTexImageIOSurface2D probe = the MTX
  counter (substitute_cgl.c:966+, ≤24 logs); winsys put/get via
  VIRGL_IOKIT_DEBUG; the kext's STREAMXFER window is UNUSABLE for a
  census (≥500k-px filter only + the known-wrong header layout from
  the ca69ac4 correction — both limits recorded already).
- Main-thread samples during the grind (pid 1402): 758/1100 in the
  SendFlushRendering cond-wait; **ZERO frames in TexImage/TexSubImage/
  TextureClient/PaintLayer/BuildLayer/DrawTarget anywhere** — Gecko
  never reaches an upload call. Not asking, not calls-not-landing.
- **layout.frame_rate=5 probe (pref in both profiles' user.js, since
  REMOVED):** flush tax gone — main thread now parks in _DPSNextEvent
  (event-idle, no dirty views), compositor parks on its 5 Hz timer,
  0.0% CPU, 988 kernel 3D lines in the window (5 Hz composites flow),
  **window still white**. With CPU available Gecko paints nothing and
  goes quiescent: the display cycles COMPLETE with nothing dirty —
  Gecko believes there is nothing to paint. Starvation falsified;
  five restarts, identical white — the face is era-wide, not
  per-process state.
- Startup log of the throttled run: __NSAutoreleaseNoPool spam (early
  startup), `Mesa: error: GL_INVALID_ENUM in glGetIntegerv(pname=
  GL_MAJOR_VERSION)` (the recorded compat-3.0 version-query face),
  missing EGL symbols (expected). stderr silent after ~2 min.
- **Rule banked:** PowerFox known-good on real GPUs — no
  Gecko-side hunting; the fish run (08-18, this browser, chrome+
  content+fish visible) is the reference configuration.

**REGRESSION PINNED AND FIXED — is_busy, one line, boot-free A/B
(15:05–15:18, commit `a88c72eb4b4`):**
- **Correction: the "possibly nothing regressed" hedge was wrong**: last render (aquarium, 08-19 ~10–11)
  was on `5e5029e` PRE-async; the only deltas to the white face were
  the async pair; no render ever occurred on it.
- **Mechanism (code-level, explains every datum):** faea295's
  `resource_is_busy` = 1 ms drain probe → BUSY under any in-flight
  batch → with the async compositor batching continuously, always
  busy → `virgl_resource.c:296` returns VIRGL_TRANSFER_MAP_ERROR for
  every PIPE_MAP_DONTBLOCK map → Gecko's content uploads (DONTBLOCK)
  all error silently at the map level → famine (34 cursor puts vs 13k
  readbacks), empty layer tree, white, zero transport errors.
- **Boot-free A/B (subst swap only, same async kext):** is_busy →
  `return false` (drains intact; dylib `fd9a0dd86f99f187a30b8ce58fd5aefb`,
  `$0x600a` count 10→9 = exactly the probe's call removed). Result:
  **puts 34→122 in 120 s with content-sized boxes (809×264 chrome
  strips, 1024×1024 canvas), attachBacking+ctxAttachResource bursts,
  composite loop fell from ~22 fps free-spin to ~2/s properly
  interleaved.** FAMINE OVER. Fix committed as `a88c72eb4b4`.
- **Remaining face (throughput, not correctness):** after the fix, the
  browser grinds in BURSTS separated by minutes (kernel log: burst of
  OK uploads at 15:18:03 after a 3.5-min silence; 522 host ops in 8 min;
  VM itself mostly idle — QEMU ~76% of one core, host load 5.5, no
  mediaanalysisd). Model: the worker chews compile-carrying batches
  for minutes each (cold shader-compile chains serialized), every
  drain eventually SUCCEEDS (hence zero timeout logs — the gaps end
  when the worker finishes, drain returns success, queued work bursts).
  Main thread sits 100% in SendFlushRendering during gaps → **window
  white + mouse dead = the flush wait**, not a wedge. The fish/aquarium
  era rendered with WARM shaders; today's cold starts pay the full
  compile bill through the serialized worker.
- **Build trap recorded (cost ~30 min):** build-10.6.sh's designed
  ninja link failure RACES the archive jobs under -j4 — deleting a
  stale object/archive and rebuilding once re-runs the link against
  the STALE archive (force-link loads whatever exists when ninja
  stops). Symptom: identical dylib md5 after a source change. Fix:
  run the script TWICE after touching sources (second run force-links
  against the fresh archive). VERIFY the binary delta (md5 + symbol/
  immediate count) after every Mesa build — never trust "rc=0".
- **Artifacts on hand:** fish kext `dd9b0d713c9bedad8bfb38bce7c40c56`
  at /tmp/VMQemuVGA_fish.kext (byte-identical to the ledger's named
  fish-run build — provenance exact; worktree route failed on
  MacKernelSDK, in-tree `git checkout cb291c9 -- FB/` + build +
  restore worked); fish dylib attempt was the stale-archive trap
  above, not rebuilt since — the is_busy fix superseded the need.
  The revert boot stays QUEUED as the confirmation rung if the
  is_busy fix's renders regress again.

**HEARTBEAT BOOT + THE REAL ERROR HUNT (16:05–16:15
pivot: "there is no storm — look for errors, look for the debug log"):**
- **Heartbeat kext** `b492386f7711310ecc419b5b8cb3151e` (per-batch
  uncapped "v3d batch done ctx size ms q ret"): boot clean, and the
  verdict is decisive — **1,354 batches at 0–15 ms each, q=0 always,
  ret=0x0 always, on a quiet host (load 1.8)**. NO STORM. The
  compile-storm and starvation theories are both dead; the worker is
  not the bottleneck. The 3.5-min-burst-gap pattern from the earlier
  run was a different regime (load 5.5 era) and is NOT reproducible
  as batch cost.
- **The errors were in the browser stderr all along** — my earlier
  `uniq -c` filter buried them under timing-stat lines (reading the
  artifact with a bad filter = not reading it; recorded as an
  instrument lesson). The family in pf_hb.log:
  `glFramebufferTexture2D(non-existent texture 2)` ×2,
  `glReadPixels(incomplete framebuffer)` ×3, `glUseProgram
  GL_INVALID_VALUE` ×8, plus the RECORDED page-shader family
  (webgl_ uniform errors, glBindTexture target mismatch — the
  2026-08-18 open item). **The new lead: a framebuffer binding to a
  non-existent texture (#2 = one of a context's first objects) +
  incomplete-framebuffer readbacks — reads like the COMPOSITOR's own
  FBO failing to assemble → frames compose transparent → blit-skip →
  white. Attribution pending** (compositor vs page context); the
  about:blank A/B was launched but the browser was closed before the
  census; its one datum: frames were FAST (wall mean 31.9 ms, ~30 fps
  rolling stats) right up to exit — the compositor runs at speed and
  still composes nothing.
- **Linux reference comparison (virtgpu_ioctl.c @
  3a0dd7ba):** (1) VIRTGPU_WAIT is PER-RESOURCE (gem handle →
  obj->resv → dma_resv_wait_timeout, 15 s, NOWAIT flag =
  dma_resv_test_signaled) — our global drain/probe is the divergence
  that produced BOTH today's bugs; my is_busy=false interim
  approximates "no fences on this resource" without implementing it.
  (2) TRANSFER_TO/FROM_HOST ioctls are ASYNC-WITH-FENCE: the command
  CARRIES a fresh fence, ioctl returns immediately, sync happens at
  WAIT — our kext's drain-everything-then-poll-inside-the-ioctl is
  the anti-model and the present path's serializer. (3) Every
  submit/transfer carries a fence in Linux. **The pre-registered
  per-resource-fence design is the reference contract, not an
  optimization.**
- **Instruments ready:** dump-on-skip built in cgl_shim.mm
  (SHIM_DUMP_SKIP=n — BMP of skipped "empty" frames; uniformly-zero =
  famine-consistent (pre-registered), content beyond p0 = the guard
  is a false-positive filter eating real frames) — needs subst swap,
  no boot. UTM debug log: STILL not located on disk (find empty);
  likely needs Debug Log enabled in the VM's UTM settings — the
  ledger's "path unrecorded" note stands; confirm before relying on it.

**NEXT (pre-registered):**
1. Deploy dump-on-skip subst; ONE launch (about:blank first — no
   WebGL context exists there, so any GL errors + empty frames are
   the COMPOSITOR's alone; then the page). Read the BMPs + the
   stderr errors side by side.
2. Attribute `non-existent texture 2` + `incomplete framebuffer` to
   a context (the dump + about:blank A/B do this); if compositor-side,
   the chase is the compositor's FBO assembly in OUR stack (format
   bindings? the storm-anatomy depth-as-color/RGBA-as-ZS family?).
3. Durable architecture per the Linux reference: per-resource fences
   (kext: fence ctx + seqno per submit/transfer; winsys: WAIT ioctl
   semantics per handle) — replaces the global drain AND the
   is_busy=false interim in one design.

---

## 2026-08-18 (entry 19) — canvas presentation: mechanism found by source read; readback-pref run pre-registered

**The standing open item (canvas presentation never worked) now has a
mechanism, established entirely by reading source — no boot yet.** Every
link read directly, not inferred:

1. **Factory choice** — `GLScreenBuffer::CreateFactory`
   (powerfox platform/gfx/gl/GLScreenBuffer.cpp:85-95): with LAYERS_OPENGL
   (our CompositorOGL) + XP_MACOSX + 10.6 SDK, the WebGL canvas front
   buffer factory is **SurfaceFactory_IOSurface**. The canvas's shared
   surface is an IOSurface; texture handles are never passed between
   contexts on this path.
2. **Producer bind** — SharedSurfaceIO.cpp:128-152 (`BackTextureWithIOSurf`)
   + :169: `mProdTex` is bound on GL_TEXTURE_RECTANGLE_ARB and backed via
   `CGLTexImageIOSurface2D(webgl CGL ctx, surf)` — on real CGL the IOSurface
   becomes the texture's *storage*, so GL renders land in it.
3. **Frame delivery** — GLScreenBuffer.cpp:440-476 (`AssureBlitted`): at
   publish the frame is BlitFramebuffer'ed into the ReadBuffer whose
   attachment is `mProdTex`. `ProducerReleaseImpl` = `mGL->fFlush()`
   (SharedSurfaceIO.cpp:38-43) — glFlush is the frame-done signal.
4. **Consumer bind** — SharedSurfaceIO.cpp:180-188 hands
   SurfaceDescriptorMacIOSurface (by IOSurface ID) to the compositor;
   MacIOSurfaceTextureHostOGL.cpp:44 and :164 (`BindTexture`) re-bind via
   `CGLTexImageIOSurface2D(compositor CGL ctx)` on the compositor's own
   context, per bind.
5. **Our substitute** — substitute_cgl.c:932+ implements
   CGLTexImageIOSurface2D as a ONE-WAY CPU upload (surface → texture).
   So: the producer-side call uploads the freshly-created (empty) surface
   into a plain Mesa texture; WebGL renders into that Mesa texture (why
   every in-page GL call passes); **the IOSurface itself never receives a
   pixel**; the compositor's per-frame upload reads zeros. Zeros + hasAlpha
   → grey (page background through a=0); zeros + opaque → black. Both
   observed faces, one cause. Also predicts: webgltest2's FBO ladder
   passes in-page while the canvas shows the mask — observed.

**Sharing EXONERATED for this item (corrects the Explore agent's summary):**
GLContextProviderCGL.mm:245-246 (offscreen, `CreateWithFormat`) and :323
(window) both create contexts with `shareContext:nullptr/nil` — Gecko never
asks for a CGL share group on cocoa; the IOSurface *is* the cross-context
vehicle. The substitute's dropped `share` param (substitute_cgl.c:606 passes
NULL to OSMesaCreateContextExt) is real but cannot be the canvas failure.
(Mesa-side plumbing exists if ever wanted: osmesa.c:575-681 passes share →
st_shared; virgl handles are global, virgl_context.c:58-62.)

**RUN PRE-REGISTERED (one variable: the pref):** deploy the handoff
substitute as-is (tree binary md5 725804ea... == ledger handoff build;
binary mtimes postdate all sources), add
`user_pref("webgl.force-layers-readback", true);` to BOTH profiles' user.js,
kext unchanged (e39300ae), 4 vCPUs, launch with the standard incantation,
open file:///Users/sl/webgltest.html (clears to RGB 0.2,0.6,1.0 — light
blue; bg #333; title channel prefix "WGL:").
Mechanism of the pref: gfxPrefs.h:596 → GLScreenBuffer.cpp:85 keeps the
factory null → SurfaceFactory_Basic → SharedSurface_Basic →
CanvasClientSharedSurface::UpdateRenderer sees mType==Basic → CPU readback
(TexClientFromReadback) → normal texture upload — the path chrome content
already exercises successfully.
**ROOT CAUSE FOUND (2026-08-18 late) — sub-box 3D transfers misplace
data: stride/layer_stride/offset never reach the host.**

The winsys discards them: `virgl_iokit_transfer_put/get`
(virgl_iokit_winsys.c:239-281) take stride/layer_stride/buf_offset as
parameters and pass a 9-scalar box {res,level,x,y,z,w,h,d,ctx} to the
kext; the kext's 0x3008/0x3009 constructors put stride=0 layer_stride=0
offset=0 on the wire (kernel log: "0x3009 wire: res=263 ...
stride=0 layer_stride=0 offset=0" on every transfer). vrend's sub-box
placement is y*stride + x*bpp — with stride=0 every row collapses
toward offset 0. FULL-surface-at-origin transfers take a sequential
path that is correct. Hence the exact cleavage observed all session:

- WORKS: 2D desktop refresh (full window at 0,0), chrome presents
  (full-window readbacks at origin), glGetTexImage full read
  (120000/120000 magenta), DFB 1x1 read at (0,0), full 400x300
  readback (all magenta), Gecko's initial full-surface uploads.
- BROKEN: any read or write with x≠0 or y≠0 — offscreen_min geometry
  map: probe(0,0)=magenta; (1,0),(0,1),(10,0),(0,10),(399,299),
  (200,150) ALL zeros. Browser canvas = the same bug: Gecko's
  shared-surface init (FULL upload) lands its 0xE5 content; every
  PARTIAL update misplaces host-side → surface shows init content
  forever (grey/black canvas). THE CANVAS-PRESENTATION ITEM AND THIS
  ARE ONE BUG.

Correction chain this session (each recorded because believed earlier):
1. "clears don't land in FBO textures" — WRONG: the CLEAR was always
   encoded and submitted (submit 1 dword 26: CLEAR buffers=4 color
   1.0,0.0,1.0,1.0 after SET_FRAMEBUFFER_STATE surface 3 = the
   texture). The kext's 20-dword hex dump truncated exactly before it.
2. "reads serve stale guest storage / clean_mask" — WRONG for reads:
   the 0x3009 readback fires every time; the BYTES come back misplaced.
3. "the quad landed, the clear didn't" — artifact of (1)+(2).
4. KEXT STREAM IDENTIFIER (ca69ac4) PARSED WITH THE WRONG HEADER
   LAYOUT — len<<20|cmd assumed; actual is cmd|(obj<<8)|(len<<16)
   (virgl_protocol.h:152, verified by decoding live streams). All its
   zero-hit oversize-scan results are MEANINGLESS. Fix when touched.
5. The "3s sleep still zeros" async falsification was run against a
   binary whose clear WAS submitted — so it remains valid evidence
   AGAINST pure host-async; the fence question (below) stays open but
   is NOT the current failure.

Decisive instruments (all in cgl-shim/killtest/, deploy to /Users/sl/):
- offscreen_test.mm — Gecko-style offscreen ctx + FBO/upload/quad/
  getTexImage/read discriminators (found the cleavage).
- offscreen_min.mm — minimal clear+read with geometry probes.
- VIRGL_IOKIT_DUMP=1 (virgl_iokit_winsys.c submit_cmd, committed with
  the build) — full command-stream dump, 8 dwords/line; this replaced
  the kext's truncated dump as the stream source of truth.
Guest-side offset math verified upstream-correct
(virgl_resource.c:932-933, 551) — the fix is transport-side.

**FIX (pre-registered, spans both repos — NOT yet built):**
1. Mesa: virgl_iokit_transfer_put/get pass stride, layer_stride,
   buf_offset (trans->base.stride, trans->l_stride, trans->offset) —
   extend the scalar array 9→12.
2. Kext: 0x3008/0x3009 selectors accept 12 scalars and put the real
   stride/layer_stride/offset on the wire (fields already exist in the
   virtio-gpu transfer commands — the kext just writes zeros today).
3. Deploy both together (one boot); rerun offscreen_min — prediction:
   ALL probes magenta, read(200,150) magenta. Then the browser
   webgltest.html — prediction: canvas finally shows light blue.
   Negative control: revert one side → zeros return.
Related follow-up (queued, not blocking): the Linux reference
(virtgpu_ioctl.c review) shows the real stack's sync is
fence-based (dma_fence per submit/transfer + VIRTGPU_WAIT on the
resource resv); our winsys stubs resource_wait/is_busy on the
"sync-submit" assumption. If post-fix anomalies remain (reads racing
host execution), the fence mechanism is the design to adopt.

**THE FIX LANDED AND VERIFIED (2026-08-18 night — Mesa 0afb1aa +
kext a378b9b, deployed together, one boot):**
- Stage 1 (transport): offscreen_min — **every probe magenta**,
  read(200,150)=255,0,255,255; wire now real: `box=(399,299)
  stride=1600 offset=479996` (= 299*1600+399*4 exactly); canvas-sized
  readbacks `400x300 stride=1600`; no XFER-CLAMP fires.
- Stage 2 (screen, visual check): the webgltest canvas presents
  its clear colour (light blue). THE FIRST CANVAS CONTENT EVER
  PRESENTED through this stack; the canvas-presentation item, open
  since canvases were first tried, is CLOSED.
- **UNPLANNED BONUS (visual check): the double-tab duplication
  artifact is GONE** — the ghost/double-tab class (the sole remaining
  open duplication item, carried as the autocomplete-dropdown class)
  vanished with the same fix. ATTRIBUTION REWRITE: its carrier was
  misplaced sub-box updates leaving stale texture content — the whole
  "persistence family" (vacated-rect clearing, drop-blit probes,
  entry-9-style framings) was this transport bug wearing UI masks.
  The drawInTitlebar pref closed the corners/black-stripe subset
  (genuinely Gecko geometry); the REMAINING class was ours.
- Browser fully interactive under WebGL load (URL-bar
  typing; no wedge).
- Transient startup white (~40 s before first content composite)
  observed once — recorded, unexplained, went away; watch for
  recurrence.
- Guest state: kext 8f011dca, libOSMesa 8e5b016b (md5-verified),
  substitute framework 725804ea, 4 vCPUs, /tmp/subst survives until
  next reboot; probe tools in /Users/sl/.
- Standing next steps: the aquarium-class WebGL page under the fix
  (does actual scene content render now?); MSAA capacity accounting
  (virgl_transfer_map_size nr_samples) still dormant-at-0; fence
  design per virtgpu_ioctl.c review still queued for sync hardening;
  webgltest2/3 ladder re-run as regression instruments.

**FLIP VERDICT + PRESENT-PATH SESSION (2026-08-19, kext budget arc):**
- The mirrored strip (upside-down URL bar + mirrored tab at the
  window's bottom edge, glyphs inverted, word order intact = pure
  y→H−y of the top chrome, band exactly toolbar height) VANISHED in
  the SHIM_DROP_BLIT run (page completely white — no GL content, no
  strip): the strip's pixels reach the screen ONLY through our
  present blit. Carrier = present path. Remaining split: composed
  INTO present_buf by Gecko's GL (its flip-Y math against our buffer
  convention) vs produced by the blit's CTM — the SHIM_DUMP_BUFFER
  instrument exists for exactly this, but the blit-skip
  (first-pixel-transparent, cgl_shim.mm:1144-1155) returns BEFORE the
  dump block, and in the degraded state every frame skips (empty) —
  the dump never fires. INSTRUMENT FIX QUEUED: dump-on-skip too.
- The degraded state itself (sparse frame: black window, white
  cleared-but-unfilled rectangles, one chrome corner fragment; 58.9k
  composited frames): compositor running with DEAD programs — a cold
  TGSI→GLSL→ANGLE→Metal compile exceeded even the kext's 10 s poll
  budget under host load and its batch was dropped. The budget arc
  (150→600→6000) proves NO fixed poll survives host-load variance.
- **DURABLE DESIGN, pre-registered (both repos, next change): async
  submit + fence** — the Linux virtio-gpu model: submit queues and
  returns; completion signals via fence; the winsys waits on
  resource-scoped fences before reuse/readback. Kills the poll
  class entirely and unblocks long compiles.
- Full storm anatomy and budget measurements: VMQemuVGA ledger
  (2026-08-19 entries); killer-batch dumps persist at
  /Users/sl/pf_dump.log.

**RUN 1 OUTCOME — pref ENGAGED, verdict stolen by a host-class panic
(2026-08-18 evening):** launch happened; kernel log carried
`0x3009 wire: res=283 box=(0,0,0, 400x300x1)` (+ a 2x2 probe at y=298)
— 400x300 is EXACTLY the webgltest canvas size ⇒ the readback factory
ran and Mesa pulled the canvas frame host→guest through the kext. Colour
verdict never reached the screen: the guest PANICKED —
`panic(cpu 3): "Spinlock acquisition timed out: lock owner active on
CPU 0x0"` in `_kqueue_scan→_kevent` (fontd); the owner thread
0xffffff800bbb4260 appears as R9 in CPU 0's dump, which sits in
`_lapic_interrupt→AppleACPIPlatform` — one CPU stalled in an emulated
ACPI/lapic interrupt while another spun on its lock. NO VMQemuVGA frames
in either backtrace; the recorded SMP APIC-timing artifact family, first
occurrence since the 4-vCPU re-enable. Captured from the UTM serial
console (panics after the buffer never reach kernel.log on
disk). AMENDMENT (revised — the 1-vCPU proposal was
overtaken): **rerun on 4 vCPU as configured.** Grounds: the panic is not
caused by the test's variable or our code (no kext frames; pre-existing
SMP timing artifact), and the canvas verdict is a correctness question
the CPU count does not touch — the 400x300 readback transfers already
flowed under 4 vCPU. The artifact is load-correlated in firing
probability only. 1 vCPU remains the documented fallback if the artifact
steals another run. P1/P2 unchanged.
- **P1 (mechanism confirmed):** the canvas region shows LIGHT BLUE on
  screen — first canvas content ever presented. Title channel unchanged
  (ctx OK, getError clean). Explicit visual check required (the correction-
  x2 lesson: read "what colour is the canvas on screen?" literally; an
  ambiguous observation is not a finding).
- **P2 (still grey/black):** write-direction story insufficient or wrong →
  next instrument: log both CGLTexImageIOSurface2D call sites (producer at
  canvas creation on the webgl ctx; consumer per frame on the compositor
  ctx) and verify the Basic factory actually ran (no IOSurface-alias call
  at canvas creation).
- **Fix direction (pre-registered, NOT to be built before P1 lands):**
  write direction in the substitute — on CGLTexImageIOSurface2D record a
  per-shim-ctx texture↔IOSurface alias (surface ref, dims, stride); on
  glFlush from an owning ctx, glGetTexImage(GL_TEXTURE_RECTANGLE) →
  IOSurfaceLock(write) → row-by-row memcpy honouring surface stride →
  IOSurfaceUnlock. Producer-side only, same thread, same current context
  (TLS-gate safe); consumer path unchanged. Negative control by env flag:
  write-back off → grey must return. Cost: one CPU readback per canvas per
  frame — same family as the chrome IOSurface upload already paid
  ("correct first, optimize later").

---

## 2026-08-17 (entry 18) — the compositor gate: found, measured, fixed; the failing era decoded; typing-cliff at the new frontier

**The reboot test (session start):** first launch after reboot COMPOSITED
(run 1, binary d965f3dc, same profile/env as the failing era) — the run
verdict "GL-composited chrome with triple tab." Ten-click test then ran:
**BOUNDED — unbounded-feedback falsified** (10 clicks did not march to 10
strips; arrangement reshuffled: 2 tabs stacked + tab strip over URL box;
compositor active throughout at ~3 Hz). The artifact = the WALK, fixed
set of stray draws at offset destinations.

**Then every subsequent launch failed identically.** Same signature as
the failing era: `GraphicsCriticalError … FEATURE_FAILURE_OPENGL_CREATE_
CONTEXT` at t≈95-125 s, with ZERO CGL traffic through the substitute (no
CGL_FWD, no CGLCreateContext). Visual: white first paint, then black.

**Falsification chain (each a separate launch, one variable):**
- env (SHIM_SCISSOR_LOG/MOZ_LOG) — exonerated (run 3, no extra env)
- profile — exonerated (run 4, fresh profile `-P freshsci`)
- our stack in-process — exonerated (run 6, NO substitute: plain Apple
  GL fails identically)
- the scissor delta — exonerated (run 5, committed rebuild; the
  attribution "the last change" falsified by direct A/B)
- "first launch of boot composites" law — FALSIFIED (run 8: clean boot,
  first launch, failed)
- clean quit — does not clear it (run 7 after AppleScript quit)
- host disk pressure (entry 17's cause) — RULED OUT this session
  (debug.log 3.4K, 100+ Gi free at every failure)
- guest disk — ruled out (9.3 Gi free)
- kernel surface path — zero NotReady/MISMATCH/CannotLock; accelerator
  alive in ioreg the whole time
- **RESIDUAL, unexplained: run 1's success.** One composite in ~15
  launches, same binary class, same env, same profile as failures.
  Consistent with a rare race (entry-16 precedent class) — the renderer
  state at 12:54 on that boot may have briefly answered differently.
  Not chased further: the fix (below) makes the question moot.

**Root cause — source read + direct probe (probe/probe_pix.c, new):**
- `gfxPlatformMac::RequiresAcceleratedGLContextForCompositorOGL()`
  returns TRUE unconditionally (gfxPlatformMac.h:89 — "in a VM,
  unaccelerated CompositorOGL shows black flashes").
- So CompositorOGL always demands
  `{NSOpenGLPFAAccelerated, AllowOfflineRenderers, DoubleBuffer}` via
  NSOpenGLPixelFormat (GLContextProviderCGL.mm:194, CreateWithFormat).
- The shim's +load swizzle list NEVER covered NSOpenGLPixelFormat —
  the request went to REAL AppKit matching.
- probe_pix (both plain and substitute, clean boot AND poisoned):
  `CGLQueryRendererInfo(0xFFFFFFFF)` → **nrend=1, id=0x01020400,
  accelerated=0** — software-only, boot-constant. The `{Accelerated}`
  format therefore returns nil ALWAYS in this VM. GA CFPlugIn absent
  (the standing architecture gap) is why no accelerated renderer ever
  exists for real GL to find.
- Black-over-white mechanism: compositor fails → BasicCompositor paints
  software (white) → the swizzled view-path presents its EMPTY OPAQUE-
  BLACK buffer over it (drawptr 0xff000000) — white-then-black exactly.

**THE FIX (cgl_shim.mm, committed):** swizzle
`NSOpenGLPixelFormat initWithAttributes:` at +load; when
NSOpenGLPFAAccelerated (73, boolean) is present, strip it and pass the
rest to the original (acceleration is not a property AppKit can measure
against our virgl-on-host-GPU GL). Negative control built in:
`SHIM_KEEP_ACCEL_ATTR=1` disables the strip.
**Landing verified:** `PIXFMT_STRIP[1] dropped NSOpenGLPFAAccelerated
from 3-attr list` (Gecko's exact list), zero FEATURE_FAILURE, ~3900
frames, renderer=virgl throughout, **visual verdict "GL composited
chrome."** Negative control EXERCISED same evening (4-vCPU boot, same
profile, one-variable relaunch): `SHIM_KEEP_ACCEL_ATTR=1` → zero
PIXFMT_STRIP lines, `FEATURE_FAILURE_OPENGL_CREATE_CONTEXT` at t=12.1,
CGL_FWD=0, view-path-only frames, **visual: black** — the
pre-fix symptom, reproduced on demand. Recovery relaunch without the
env: PIXFMT_STRIP fired, zero FEATURE_FAILURE, ~1470 frames composited.
**The strip is load-bearing; the only variable between black and
GL-composited is that env var.**

**New frontier — typing-burst congestion collapse (unattributed):**
after ~3500 healthy frames (~300 ms wall each), a typing burst hit and
wall mean went 300→755→2347 ms (max 18.7 s) and never recovered. Cliff
anatomy: at the transition block wall(755) ≫ submit(89)+transfer(67)+
blit(14) — ~580 ms unaccounted = thread starvation FIRST; submit/
transfer inflated only behind the backlog. The kext's own 2D refresh
workavg rose 3.4→15 ms in the same window (kernel-side, browser-
independent) = host service degraded too; host was at load 14-20
(AddressBookS 90%, contactsd 81% — host's own daemons, not ours).
Browser ended in uninterruptible wait (U), then the process was gone
with NO crash report. **Hypothesis (unverified): 1-vCPU TCG congestion
collapse triggered by input burst on a busy host. Pre-registered next:
quiet host; relaunch; idle stays healthy?; small typing burst
reproduces the cliff? Compare wall-vs-parts gap at the transition.**

**Frontier RESOLVED same evening (pre-registration outcomes):**
- A relaunch under host load ~20 degraded from its FIRST frames with
  zero typing (wall 1.3-2.9 s throughout) → the degraded regime is
  host-load-driven; the typing correlation in the first run was
  concurrent, not causal.
- Typing on the degraded baseline produced a TRANSIENT U-state wedge
  (~1-2 min, self-recovering, no crash) — not a terminal deadlock.
- **VM reconfigured to 4 vCPUs (SMP axis opened): compositing at wall
  159-165 ms mean, max 214-225 ms, WITH host at load ~20 — and typing
  during the window produced NO wedge. The congestion/wedge class is
  1-vCPU starvation, closed by SMP.** Best quiet-host figure observed
  earlier in the evening: 133-238 ms mean (min 64 ms).
- Related host-starvation face: UTM's NAT forward path wedged
  (guest→internet 100% loss while host clean); reset by VM restart.
- Kext-side IOLog gate landed same evening (VMQemuVGA 36f8f97) —
  kernel.log under compositing 1.3 MB/min → 0.95 KB/min (~1400×),
  so the shim stats blocks are now the dominant per-frame logging and
  the msgbuf no longer wraps.

**THE WALK INSTRUMENT — built, run, and the draw-stream space CLOSED
(2026-08-17, late evening).** Source read of Gecko 52 established the
carriers (SetLayerRects/SetTextureRects → glUniform4fv 4×vec4;
uMatrixProj → glUniformMatrix4fv; RenderTargetOffset → glUniform2f;
draws via glDrawArrays; textures via glBindTexture). Five wrappers
defined in substitute_cgl.c and force-exported; env master arm
SHIM_WALK_LOG=1 plus a RUNTIME WINDOW (/tmp/walk_on marker — the first
build logged unconditionally and burned the 100k cap during startup).
Run: capture window open through the github.com navigation (20,032
draws; 10,879 with layer+texture rect pairs; layer uniform always
loc=1). DECODE:
- Same texture-source drawn at >1 destination: **ZERO** — the
  destination-walk and transform hypotheses are falsified in the
  quad data (outcomes 1/2 dead).
- Grow box (15×15): 111 draws, ONE destination ever (1476,895) —
  yet the screen shows it duplicated up-and-left. The ghost is NOT
  DRAWN in the current stream.
- Titlebar: drawn EVERY frame at (0,0 1491×79) — full top coverage,
  yet a black band + ghost strip sit inside that region.
- Conclusion: the duplication lives BELOW the draw stream — stale
  content inside the snapshot TEXTURE (one draw per frame of a
  texture whose content carries [black band][ghost strip][current
  strip]) or old pixels persisting below the draw (entry-9 contract:
  DoubleBuffer-without-BackingStore vs our ever-persistent buffer).
  Outcome 3 (double-draw) also dead — no within-frame repeats.
- **Next discriminator (pre-registered): the pattern-fill run on the
  upload path** — ghost band shows checkerboard → duplication is IN
  the texture content (upload-side, RectTextureImage snapshot stale);
  stays black under pattern fill → below the texture path (buffer
  persistence). Same run answers the black band (drawn-and-covered vs
  never-uploaded).
- Third symptom recorded: black band above the ghost strip (absence,
  not displacement) — the vacated-region reading is consistent
  with the persistence family; band-vs-ghost growth correlation is
  directly checkable in the next run.

**SEPARATE ITEM (kept apart from the walk — evidence does not connect
them): missing glyphs on github.com.** Page loads (HTTPS, lock,
scrollbar, shapes/gradients/layout correct) but text is nearly absent:
nav dropdown arrows bare, hero headline a single "H", empty search
field/buttons. Chrome + system-font pages (start page, google.com)
rendered text fine → suspicion: web-font path. STRIKING resemblance to
the FIXED 2026-08-13 pixel-store bug ("glyph atlases at progressive
offsets" — ROW_LENGTH reset bug; single-"H" = first-glyph-survives
signature). Discriminators queued: (a) no-web-font site; (b)
layers.acceleration.disabled=true — glyphs appear on software
compositor → GL path; still missing → font loading/rasterisation.

**SOFTPIPE BISECT RUN (2026-08-17 late night) — BOTH verdicts in:**
- Run: GALLIUM_DRIVER=softpipe, same page, renderer="softpipe",
  zero virgl_iokit engagement. Verdict on the differences vs
  virgl: (1) **double tab GONE under softpipe**; (2) URL-box text
  visible (but under virgl the ghost strip covered the URL box —
  entangled with the duplication, no independent text signal);
  (3) **page web-font text STILL BARE under softpipe.**
- **Duplication verdict: virgl/host-side.** Clean under softpipe with
  identical Gecko + shim buffers + texture-upload path → the ghost
  carrier is the persistent HOST-SIDE virgl render target (entry-9
  family: DoubleBuffer-without-BackingStore vs our ever-persistent
  surface). Texture-snapshot-staleness also dead (textures traverse
  the same Mesa path in both drivers). Fix direction: surgical
  vacated-rect clearing (walk wrappers already hold per-texture
  previous rects; clearing only moved-from regions avoids the entry-9
  full-clear breakage). The in-shim diff instrument (buffer vs union
  of frame draw rects) remains the mechanical validation.
- **Web-font text verdict: OUR-MESA side.** Bare under BOTH drivers;
  real hardware runs the same PowerFox+github fully — but on Apple GL,
  not our Mesa, so hardware success never exonerated our Mesa. The
  fault is in our Mesa's handling of web-font glyph atlas uploads (or
  Gecko's path through them). Leading candidate by precedent: a
  format/size/alignment fault in the glTexSubImage2D family — the
  2026-08-13 ROW_LENGTH bug was exactly this family. NEXT INSTRUMENT
  (pre-registered): glTexImage2D/glTexSubImage2D wrappers logging
  internalformat/format/type/size/alignment — diff a WORKING atlas
  upload (system font) against a FAILING one (web font) on the same
  page.
- Session history note: a mid-run host-side DEVICE WEDGE killed the
  first pattern-fill attempt (recorded in the kext ledger); the
  restart cleared it (56.3 Hz refresh after — best yet). The checker
  run post-restart showed ZERO checkerboard on any visible content →
  CGLTexImageIOSurface2D is not on Gecko's visible-content path in
  this configuration (only 3 one-time widget uploads ever seen);
  pattern-fill must move to the glTexImage2D family to see anything.

**DIFF INSTRUMENT LANDED AND MEASURED (commit 54eb37a, same night).**
Present-time scan: presented buffer vs the union of the frame's draw
rects (walk wrappers feed it; LayerRects = second-to-last 4×vec4
glUniform4fv per draw, y-flipped). Windowed (SHIM_DIFF_LOG=1 +
/tmp/walk_on); trigger without mouse: AppleScript `set bounds of
front window` (the pointer couldn't capture — the resize was
scripted over ssh instead). First run: 312 frames through two
resizes. **Steady state, identical across ~310 frames: coverage
91.4% (37 draws/frame), STALE 116,604 px == the uncovered 8.6% — a
constant full-width ~96-px band at the window bottom; BLANK 0 (no
never-drawn region in this window state).** The persistence mechanism
is MEASURED: ~9% of every presented frame is content drawn by no
frame of that pass, riding the buffer. The user-visible ghosts are
instances of this class. **FIX TARGET (pre-registered): at present,
clear exactly the uncovered region — surgical, avoids the entry-9
full-clear breakage.** Cosmetic known bugs: frame-0 coverage/count
warm-up artifact (reset only inside window); tile threshold 32 px/32²
hides ghosts smaller than a tile inside covered tiles.

**THE ARTIFACT CHAIN CLOSES GECKO-SIDE (2026-08-18 morning).** The
arc, in order:
1. **Vacate fix BUILT (ef5d916) and FALSIFIED by visual verdict**:
   SHIM_VACATE_CLEAR=1 (clear prev-covered-now-uncovered pixels at
   present) left double tab, black stripe, and hidden-URL-bar
   unchanged. The fix's target (our buffer's stale pixels) was the
   wrong layer.
2. **Drop-blit discriminator (SHIM_DROP_BLIT=1)**: with our ENTIRE
   present path off the screen, the page goes white (the blit was
   the only page-content source — nothing else paints it) but the
   **duplicated corners survive** → the ghosts live in Gecko's own
   CoreGraphics paint. (Reading: the CG-drawn parts duplicate,
   the GL part is absent — one mechanism explains both.)
3. **drawRect-entry instrument (64e5719, SHIM_DRAWRECT_LOG=1)**:
   code review first — lazy install IS guarded one-shot; both swizzle
   targets call the original exactly once. Empirical: T2
   (PixelHostingView) fires ~1:1 with frames, CTM uniform on every
   entry; T1's offset-CTM entries are ALL Gecko's own
   **NonDraggableView** (titlebar corner/region views) painting
   micro-rects at a spread of y=831..871, x clusters 4/85/1380/
   1412/1448 — the ghost-strip pattern IS Gecko's view population at
   stale layout positions. T1 inert on them (no associated
   present_buf).
**VERDICT: the duplication artifact is Gecko-side** — its titlebar
view machinery holding/painting views at old positions under this
configuration. The shim is exonerated by measurement at every
instrumented layer: GL uniforms (walk), draws (walk), present buffer
(diff), blit (drop), drawRect/CTM (DR). Likely interaction: Gecko's
titlebar/snapshot invalidation keys off window-surface state that the
emulated context presents differently than real hardware (real-Mac
PowerFox renders github's chrome correctly). **NEXT (if pursued):
Gecko-side — enumerate NonDraggableView frames over time (their
positions ARE the ghost positions), or source-read nsChildView's
titlebar snapshot invalidation (~/powerfox-browser platform/widget/
cocoa).** The earlier softpipe "no double tab" observation is
re-attributed to early-state timing, not driver.

**Kext-side same morning: backing table full (VMQemuVGA 36f8f97-era
limit)** — fresh-boot browser start filled the 64-entry
m_user_backings table at res id 0x1bc → attachBackingUser FAIL
0xe00002be → Mesa GL_OUT_OF_MEMORY → compositor SIGSEGV at 0xb5
(crash report, thread 20). Fixed: MAX_USER_BACKINGS 64 → 512
(installed 322cf426, table-full zero in the new boot). Note: the
table was ALSO filling silently on Aug 17 23:30 — the limit had been
biting since the SMP era, unnoticed.

**STARTUP-FIRST-PAINT DATUM — the dynamic framings die (2026-08-18,
observation): the duplicated corners are visible at STARTUP, on
the white screen, BEFORE any blit or GL present.** No movement
history exists at first paint — "stale positions," "damage-region
repaint," and "ghost accumulation" are all falsified as descriptions.
The startup T1 census confirms: the NonDraggableView y-spread
(831..871) is present in the FIRST logged paint. **The duplication is
in Gecko's INITIAL view-tree geometry: the titlebar layout creates
the NonDraggableView population at two overlapping geometry sets from
the first pass** (two layout assumptions — e.g. unified vs plain
toolbar measurement — under this window configuration). Remaining
question, answerable by SOURCE READ (no build/boot): nsChildView/
nsCocoaWindow NonDraggableView creation in ~/powerfox-browser —
which two geometry computations produce the doubled set.
Related same-day datum (change-triggered DR log, 0d5bed3b): clicking
the search box paints a popup view stack (NSView→ChildView→
ViewRegionContainerView→PixelHostingView) at 115x18 → 288x146 →
288x326 — the AUTOCOMPLETE DROPDOWN opening and growing; the "extra
tab covering the search box" is the dropdown's own rendering (same
class our T2 swizzle covers; T2 is passthrough there — SC_KEY-gated
blocks unarmed, blit inert without associated buffer — asserted from
code, not yet logged).

**PREF TEST VERDICT — the artifact family SPLITS (2026-08-18,
browser.tabs.drawInTitlebar=false via user.js):**
- **Corners: GONE. Black top stripe: GONE.** The draw-in-titlebar
  machinery (native snapshot titlebar vs XUL titlebar layout — two
  geometries disagreeing from first paint) is CONFIRMED as their
  mechanism, exactly as pre-registered. The pref is a usable fix NOW:
  one geometry, two of three symptoms eliminated, price = a
  conventional titlebar.
- **Double tab over the URL bar / search-box ghost: STILL PRESENT.**
  Separate class: the AUTOCOMPLETE DROPDOWN's rendering (the popup
  view stack captured at 115x18 → 288x146 → 288x326). This is the
  ONE remaining open graphics item — Gecko's popup content rendering
  under this configuration. Next instruments if pursued: dump the
  popup's surface after first paint; or source-read the autocomplete
  popup's content path (Gecko-side, the popup is a ChildView window
  whose PixelHostingView paints tab-strip-like content).

**GLYPH ITEM CLOSED (2026-08-17, the one-variable local page).** The
softpipe github reading was DOUBLE-confounded: the page never finished
(script hang — the "Unresponsive script" dialog was github's), AND
softpipe has its own atlas fault. The one-variable page settled it:
`/Users/sl/fonttest.html` — two lines, identical text; one via
@font-face to a LOCAL TTF (no network, no script, instant load),
one system monospace; `monospace` fallback on both so a rejected
font would RENDER, not vanish. Results:
- softpipe: system ✓, **webfont INVISIBLE** (⇒ font loaded, glyphs
  blanked — a SOFTPIPE-SPECIFIC atlas fault; instrument-mode bug,
  NOT the product path; record so it never re-confounds a bisect)
- virgl: **system ✓, webfont ✓ — the production stack renders
  downloaded fonts correctly, end to end.**
Verdicts: github's bare text = LOAD-STALL confound (text never
arrived), not font rendering; WOFF2 decode = fine (CPU-side, same
binary as the real machine that renders github fully); the glyph
item LEAVES the graphics queue. The only open graphics artifact is
the duplication (virgl/host-side persistence; next instrument =
in-shim buffer-vs-draw-rects diff).

## OPEN ITEMS AT SESSION END (2026-08-18, the canvas-presentation item and the queue)

**1. CANVAS PRESENTATION UNDER THE SUBSTITUTE — the standing open
item, from scratch.** WebGL/2D canvas GL content has NEVER reached
the screen through the substitute (no verified working moment; see
correction x2 below). Known faces: transparent mask (grey-over-
background) and opaque-empty-buffer mask (black) — the blit presents
either nothing (early-return) or an empty buffer. Known GOOD: the GL
itself works — webgltest2's FBO pipeline passes every call with zero
GL errors; the title channel reads verdicts without page text.
Instruments in tree: webgltest.html (title-channel), webgltest2.html
(FBO ladder), webgltest3.html (no-FBO control). FIRST QUESTIONS:
(1) does the canvas present path ever CALL our shim's present
(blit-skip lines vs blit lines in a fresh log)? (2) where does
Gecko's canvas compositing route under the substitute — the
CanvasClient/ImageBridge path may bypass our flushBuffer present
entirely (the same class as the drawRect-as-flush-point gap that
needed its own hook). Suspect: the canvas layer presents through a
path the shim does not wrap — the entry-16 lesson (AppKit flushes
through private __NS_CGL* wrappers our swizzles cannot see) applies
to canvases too.

**2. Wedge residual (the 0x6008-stream oversized transfer):** kext
paths exonerated by capture; MSAA capacity accounting named as the
remaining candidate; msaa-samples=0 pref closes the crash class in
practice. Fix locus = Mesa virgl transfer sizing under nr_samples
(virgl_resource.c:342-343 has no sample multiplication).

**3. WebGL2 gate:** blocked by Gecko's 3.2 version check under our
compat-3.0 OSMesa context. Fix = core/compat context design (real
surface area). enable-webgl2=false makes pages fall back to WebGL1.

**4. Intermittent teardown crash:** TiledContentHost/DrawGeometry
under RecvFlushRendering when closing a live webgl canvas. Not
deterministic; stack archived in ledger.

**5. Standing hygiene:** debug log growth (cap it); scissor stash
(decide); GL_MAX_SAMPLES guest report; the dropdown-class artifact
(Gecko popup, sole remaining duplication item).

Handoff config: 4 vCPUs; kext 37268cb3 (512-table, attach+transfer
clamps, stream identifier); substitute 725804ea (strip+walk+diff+
vacate+UAF+DR-instrument); prefs drawInTitlebar=false,
msaa-samples=0, enable-webgl2=false (in both profiles' user.js);
probe tools in VMQemuVGA/probe/ and killtest/; /tmp/subst needs
redeploy after every guest reboot.

---

**WEBGL CODE READ + PREF TEST (2026-08-18, cross-repo session tail):**
- virgl_resource.c:342-343 — virgl_transfer_map_size computes
  stride/layer_stride from format x box ONLY; nr_samples appears
  nowhere in guest transfer sizing. Dormant at msaa-samples=0.
- virgl_screen.c:801-817 — sample gating against caps exists
  (v1.max_samples + v2 multisample format bits); whether OUR caps
  blob overclaims (host max 4) is unverified — GL_MAX_SAMPLES query
  queued.
- Host "Skipping N samples" lines are boot-time format-table
  enumeration, NOT per-resource evidence (dead lead, recorded).
- Pref test (webgl.msaa-samples=0): the fatal capacity error FROZE —
  crash class closed. Remaining black = rendering correctness: 306x
  glBindTexture(target mismatch), invalid uniform locations,
  glTexSubImage2D(level 0) — the WebGL page's ANGLE-translated
  program failing Mesa-side semantics. NEXT (Mesa-side hunt): start
  at the texture-target mismatch (RECTANGLE vs 2D binding in the
  page's program) — one root mismatch likely explains the storm.

**WEBGL MINIMAL-PAGE VERDICT — the class splits clean (2026-08-18,
the one-variable page + a title channel read via AppleScript, since
page text did not render while the canvas did):**
- **WebGL1: WORKS** — context OK, clear to blue, getError 0x0 clean.
  Base WebGL is fully functional in this stack.
- **WebGL2: BLOCKED at Gecko's version gate** — GLContextFeatures:
  clear_buffers needs GL 3.0, get_integer64_indexed needs GL 3.2;
  pure version checks, no extension alternates. Our OSMesa COMPAT
  context reports 3.0 (Mesa virgl compat ceiling); virgl CORE reaches
  3.3+. The earlier "black webgl" page demanded WebGL2 specifically
  ("WebGL 2 requires... Exhausted GL driver options") — no rendering
  fault; a profile ceiling. The 306-error storm was downstream noise
  of a refused/never-properly-created context.
- **FIX DIRECTION (design decision, next session's opener):** dual
  contexts in the shim (compat for Gecko's compositor path, core for
  GL/WebGL consumers) or attrib-based creation serving 3.3+ where
  Gecko tolerates core. Real surface area — needs a considered design
  pass, not a tail-of-session patch.
- Reusable asset: cgl-shim/killtest/webgltest.html (clear-to-colour +
  verdict in document.title — readable over ssh when page text
  doesn't render).

**UAF FIX VALIDATED + the rendering-correctness repro (2026-08-18,
end of the marathon):** the aquarium-class WebGL page (webgl2 off →
WebGL1 fallback, msaa 0) LOADS AND RUNS at ~1 FPS with NO crash —
the free_buffers UAF guard (e43ca76) held through the churn that
crashed twice before. The canvas shows BLACK (no fish) while the
page stays alive: the rendering-correctness item now has a live,
non-crashing repro AND a working baseline (webgltest.html clears
blue, getError clean) — the delta between them (offscreen FBO
rendering, scene textures, translated shaders under GL 3.0 compat)
is the next session's bounded hunt. Guest config at handoff:
4 vCPUs, kext 37268cb3 (clamps+identifier), substitute 725804ea
(strip+UAF), prefs drawInTitlebar=false + msaa-samples=0 +
enable-webgl2=false.

**GREY-CANVAS SESSION STATE + COMPOSITOR CRASH (2026-08-18, the
final arc):**
- The FBO ladder page (webgltest2) passed EVERYTHING in-title (ctx OK,
  default RED clear, FBO COMPLETE, FBO BLUE clear, program link OK,
  quad drawn, zero GL errors, final GREEN clear) yet the canvas showed
  GREY = the #333 page background through a transparent canvas: NO GL
  content reached the presented buffer for the canvas region.
- **Decisive: webgltest.html (page 1) — BLUE earlier in a fresh
  session — showed GREY when reopened in the SAME session with the
  aquarium tab still live (its 1 FPS loop running).** The page's own
  GL is exonerated; the flip is SESSION STATE — a live WebGL canvas
  (the aquarium) holds/wedges the shared canvas-compositing path and
  every other canvas composites transparent. The aquarium's own black
  canvas rereads as: it TOOK the path and renders nothing visible
  through it.
- **NEW CRASH CLASS (stack archived):** while testing
  aquarium-close + page-1 reload, the COMPOSITOR thread crashed in
  CompositorOGL::DrawGeometry rendering a TiledContentHost tile,
  under CompositorBridgeParent::RecvFlushRendering — the canvas-flush
  after page teardown drew a tile whose texture source had died.
  Texture-lifetime class on Gecko's compositor side; no shim frames
  in the stack.
- NEXT SESSION OPENERS (in order): (1) the coexistence test that was
  interrupted — close aquarium, reload page 1, blue returns?;
  (2) the teardown crash — reproducible by closing a live webgl
  canvas? (3) the ladder pages (webgltest2/3) as instruments.
- Handoff config: 4 vCPUs, kext 37268cb3, substitute 725804ea
  (+e43ca76 UAF), prefs drawInTitlebar=false + msaa-samples=0 +
  enable-webgl2=false.

**COEXISTENCE TEST COMPLETE (2026-08-18, the marathon's last act):**
fresh single instance (substitute restored post-/tmp-wipe; earlier
"gate failures" were an env-less dock launch + the wipe — no crash-
protection needed; force-enabled pref added but untested as cause):
- websamples.org webgl page restored by session (black canvas, 2
  white UI parts) + webgltest.html in a second tab: page 1's canvas
  = BLACK (opaque-empty-buffer mask this session; grey=transparent
  mask last session — same wedge, different alpha face).
- **Closed the websamples tab: browser SURVIVED (teardown crash is
  INTERMITTENT), page 1 reloaded — canvas STILL BLACK.**
- **VERDICT: the canvas-compositing wedge is established by the
  heavy webgl page, PERSISTS after the trigger dies, resets only on
  relaunch.** Backing-table-clog theory WEAKENED: unref (2131) >
  attach-ok (1144), slots flow back; one table-full event all boot.
  The wedge is Gecko canvas-manager state (TextureClient/ImageBridge
  family) — the same bad state that intermittently crashes
  TiledContentHost on teardown.
- NEXT SESSION: Gecko-side canvas-manager hunt; first instrument =
  the ladder pages + a relaunch A/B (blue on fresh, black after
  heavy page — the repro is now 100% schedulable).

**CORRECTION x2 — CANVAS NEVER WORKED; NO BLUE EVER OBSERVED
(2026-08-18, final):** the "coexistence wedge" framing is RETRACTED,
and so is the "blue residual": **there was never a blue canvas at
any point.** The earlier session message "canvas is blue" was not an
observation of the screen — it was recorded by me as a verified
visual verdict without checking, and every downstream conclusion
(wedge, regression-from-blue, UAF-suspect, residual #2) inherited
that invention. PROCESS LESSON (ledger-worthy): a visual verdict
earns recording only from an explicit answer to an explicit question
("what color is the canvas on screen?") — paraphrase-able or
ambiguous replies get confirmed first. FACTS THAT STAND: WebGL
canvas presentation has NEVER worked under the substitute (grey =
transparent mask, black = opaque-empty-buffer mask — the standing
failure faces); the FBO ladder page passes all its GL calls cleanly
(the GL works; the presentation doesn't); the teardown crash is real
and separate. THE ITEM: canvas presentation under the substitute,
from scratch, with no proven-working baseline.

**Corrections to earlier entries:**
- Entry 17's "black browser = host disk pressure" — PARTIAL: disk
  pressure caused the 2.5-24 s frames; but the black CHROME also/instead
  required the compositor gate (this entry). Two black classes
  coexisted; the pre-reboot era's failures were the GATE
  (binary-independent, launch-independent).
- The glClear no-context probe (substitute_cgl.c, "gated to first 20")
  was NEVER force-exported → per the GLU-shim lesson it almost
  certainly never fired; "zero foreign forwards" readings built on it
  are unproven. Same class as installation ≠ firing.
- Entry 13's "per-quad clip scissors" premise — FALSIFIED: Gecko's
  compositor makes ZERO glScissor calls (scissor wrapper deployed,
  proven live by probe, zero hits across ~3900 frames). The artifact
  offset lives in vertex/uniform data. The wrapper delta is in
  `git stash` ("scissor-wrapper-delta") — keep stashed or drop.

**Tools added (VMQemuVGA/probe/):** probe_pix.c (renderer list +
Gecko's exact pixel-format call — needs NSApplication bootstrap or it
fails bare; the AutoreleaseNoPool spam was the tell), probe_sci.c
(glScissor export/fire prover). Deploy to /Users/sl/ — /tmp is wiped
every reboot and ate the tools twice.

---

## 2026-08-17 (entry 17) — sixth confound: host-disk-pressure (the black browser)

**Symptom:** PowerFox "black just like Flurry." Log: frames taking
2.5-24 s (wall mean 7.5 s; transfer mean 2.5 s — previously
sub-100 ms); STAMP corners returning 0xff000000. NOT a shim
regression — nothing in the delta runs on the Gecko path with the
gate off.
**Cause:** host Data volume 97% full (26 Gi free), with the UTM
per-VM debug.log at 35.3 GB writing thousands of DEBUG lines per
minute (cs_gl_draw et al. per presented frame). Host I/O
starvation → guest frame stalls. Also plausibly explains Flurry's
exit-after-one-frame (entry 16) — same era, same pressure.
**Fix applied:** truncated the log in place (inode kept; UTM
appends on) — 26→58 Gi free; lock stats recovered max 2554 ms →
61 ms within seconds. **The Debug Log must be DISABLED in UTM
settings or the 35 GB regrows and this recurs.**
**Rule added (.claude/CLAUDE.md, commit 2de24b4):** the 1x1
lesson (flushBuffer owns the resize; alternate present paths
inherit it) + silent-declining-guards rule (src=0 class;
installation-≠-firing class).
**Pending:** ten-click test (the walk's unbounded-vs-bounded
prediction) once the host is healthy and the browser paints.

---
Last updated: 2026-08-17 (entry 16: drawRect-as-flush-point LANDED FOR THE CLASS — full-frame present proven (1680x1050, real content); the 1x1 lesson; Flurry exits after one frame — animateOneFrame lead; roadmap: walk measurement → drawRect fix (DONE) → GA CFPlugIn)

---

## 2026-08-17 (entry 16) — drawRect-as-flush-point: landed for the class, proven with a full frame

**Diagnosis chain (NSOpenGLView theory, confirmed):** zero
CGLGetCurrentContext + zero flushes = the app doesn't present;
AppKit does, via private __NS_CGL* wrappers interposition can't
reach. AppleFlurryOpenGLView is the idiom's instance here. The
swizzle was already installed on that class; a firing probe
proved it FIRES (one line added; installation ≠ firing had been
invisible — the blit early-returns silently without present_buf).

**The fix (env-gated SHIM_DRAWRECT_PRESENT, armed at setView by
associating the shim_ctx on the view):** resize to view bounds
BEFORE the app's drawRect (main thread — AppKit safe), call the
original (app draws GL), then flushBuffer-equivalent (readback +
swap + rebind) and the existing blit. Covers the whole
NSOpenGLView idiom — a large class of era apps, not Flurry
specifically (universal-driver work, for the class).

**THE 1x1 LESSON (first run after landing):** presented frame was
`drawRect[0] 1x1` — flushBuffer owns the viewport-driven resize;
apps that never flushBuffer stay at CGLCreateContext's initial
1x1 allocation and everything clips to one pixel (presented pixel
was REAL — 0xff000000 — proving content in a one-pixel frame).
The pre-draw resize fixed it: second run shows `resized to
1680x1050` then `drawRect[0] 1680x1050 pixel RGBA(0,0,0,255)` —
**a full-frame present of real content. The idiom works end to
end.**

**Flurry's remaining quirk (named, not chased):** the engine
exits after that one frame. ScreenSaver apps animate via
-animateOneFrame (a timer calling the SAVER, drawing outside
drawRect into the current context) — those draws land in our
render_buf with no present until a drawRect that never comes, and
the engine terminates (~90s, cause unattributed). NEXT
PRESENTATION ITEM: present on animate too (hook -animateOneFrame
or a frame-timer), same machinery. ALSO noted: one non-
deterministic early AppKit exception (`setObject:forKey: nil
key`, Terminating) on an identical relaunch — environmental
variance, not the build (the re-run of the same binary worked);
unexplained, watch for recurrence.

**Roadmap:** (1) finish the
WALK measurement (render-offset/scissor log — last shim-side
question on the chrome artifact); (2) land drawRect-as-flush —
DONE this entry, for the class; (3) the GA CFPlugIn — connects
the two halves: apps attach to the accelerator, the shim presents
into a real CGS surface WindowServer composites instead of
readback+blit; builds on the working surface path; the
architectural answer to presentation. GLD stays optional
(integration, not capability — Mesa already gives GL 2.1).
Presentation is the unfinished service of the three (winsys done,
loader done) — every remaining problem is presentation.

---
Last updated: 2026-08-17 (entry 15: Flurry CGL census — CGLFlushDrawable FALSIFIED (zero calls); present path is neither -flushBuffer nor CGLFlushDrawable; single-buffered/-flush or CVDisplayLink queued; direct-CGL present hook landed as infrastructure)

---

## 2026-08-17 (entry 15) — Flurry present-path census: both standard paths falsified

**Infrastructure landed (4a4f06a6):** CGLFlushDrawable on a shim
context now runs the SHIM'S OWN presentation (readback + swap +
rebind + view store + drawRect — cgl_shim_present_from_cgl in the
.mm, extern "C" for ObjC++ linkage), third instance of the
implement-not-forward rule. CGLGetCurrentContext logs its return
classification (FOREIGN/shim, capped). This lights the census AND
the fix if the prediction held.

**Census verdict (flurry8, softpipe): the prediction is
FALSIFIED — ZERO CGLFlushDrawable calls, ZERO GetCurrentContext
calls.** Flurry presents through NEITHER -flushBuffer NOR
CGLFlushDrawable. Remaining candidates: (a) the single-buffered
idiom — NSOpenGLContext -flush (glFlush + front update), which
the swizzle set does NOT cover; (b) CVDisplayLink-driven drawing;
(c) an as-yet-unnamed private path. Also: flurry8's process EXITED
(<85 s, softpipe) where flurry7 (softpipe, previous build) was
still alive at 3+ min — labeled UNEXPLAINED VARIANCE (possibly
screensaver idle-dismissal timing; nothing in the delta calls
itself: FlushDrawable/GetCurrentContext logged zero hits).

**Next (queued):** swizzle -flush (and log CVDisplayLink creation
via dlsym survey); then Flurry screen verdict. The hook landed
today is the fix IF the present is CGLFlushDrawable-shaped in
disguise (e.g. -flush → CGLFlushDrawable internally — worth
checking whether NSOpenGLContext -flush CALLS CGLFlushDrawable;
if yes, the zero census means -flush is never called either, and
the hunt moves to CVDisplayLink/other).

---
Last updated: 2026-08-17 (entry 14: GLU FIX LANDED — crash gone; Flurry now WHITE-under-both (bisect: Mesa-side/upstream); Flurry's presentation BYPASSES the shim entirely; full-screen surface binding the lead)

---

## 2026-08-17 (entry 14) — GLU fix landed; Flurry: crash → white; bisect verdict; presentation-path bypass found

**GLU ROUTING FIX (layering rule, landed):** GLU is a
CLIENT of GL. Apple's umbrella re-exports libGLU (absolute-path
LC_REEXPORT inside libOpenGL_real), so Apple's GLU ran with the
shim token where it dereferences _CGLContextObject internals →
halveImage_ubyte OOB (entry 13). Fix: glu_shim.c DEFINES the
era-common glu* inside the substitute (Build2DMipmaps, ScaleImage
[box filter, not bit-exact], ErrorString, Perspective, LookAt,
Ortho2D, PickMatrix — over p_*/dlsym'd Mesa pointers; quadrics/
NURBS/tess/project deliberately absent = loud bind failure).
Linker suppressed the definitions from the export trie (the
re-export chain also supplies the names — nm showed T, dyld
never saw them); -Wl,-exported_symbol ×7 forces them (build
86c19d00). ALSO FOUND: overwriting the framework binary while a
process holds it silently fails — kill ALL holders (leftover
ScreenSaverEngine pids) before deploy; verify md5 both ends.

**RESULT: the crash is GONE.** Flurry now runs — white screen.
- Under virgl: white, runs a few seconds, exits clean (no crash
  report).
- Under softpipe: white, RUNS INDEFINITELY, 10.9s+ CPU and
  climbing.

**SOFTPIPE BISECT VERDICT: white under BOTH ⇒ the fault is
Mesa-side or in Flurry's GL expectations — NOT the virgl/winsys/
kext/host chain.** Host debug log (found INSIDE the bundle —
/Users/macbookpro/Documents/Snow Leopard.utm/Data/debug.log,
34.8 GB, tail-only; no UTM shutdown needed): clean 25 Hz
cs_gl_draw present loop, ZERO virgl decode errors during Flurry.

**The sharper datum: Flurry's presentation BYPASSES the shim
entirely.** Both drivers: the setView lazy swizzle fired on
AppleFlurryOpenGLView, but shim_flushBuffer NEVER RAN (zero
pre-resize/swap_mc/drawptr lines — those log their first 5
unconditionally). Flurry creates/presents through a path none of
the swizzles wrap. LEAD (inference): full-screen surface
binding — CGLSetFullScreen / CGLSetSurface / PBuffer route,
where the CGL_FORWARD token-failure model applies: forwarded
with the shim token → real CGL fails → Flurry has no drawable →
draws go nowhere; capability-check differences between drivers
could explain virgl's early exit vs softpipe's endless loop.
NOTE: no CGL_FWD lines in flurry logs — whatever it calls is
either implemented-and-quiet or another private entry point.

**NEXT (queued):** log every CGL entry Flurry exercises (the
CGL_FWD survey exists; extend the logging to implemented entries
too), identify its surface/present calls, then wrap or honestly
refuse them. Flurry remains the regression test: crash fixed,
white next.

---
Last updated: 2026-08-17 (entry 13: FLURRY TEST — crashed in Apple libGLU gluBuild2DMipmaps/halveImage_ubyte through the substitute; the blast-radius question has a crashing answer; GLU coverage gap logged)

---

## 2026-08-17 (entry 13) — Flurry through the substitute: CRASH in Apple's GLU

**Run:** ScreenSaverEngine -module Flurry with the standard
substitute env. The full chain engaged cleanly first: swizzles,
GALLIUM_DRIVER=virgl, libOpenGL_real, **virgl capset via the kext
(get_caps capset_id=2 ver=2 size=1408 — VIRGL2)**, contexts
created. Then SIGSEGV, crash report at
~/Library/Logs/CrashReporter/ScreenSaverEngine_2026-08-17-*.crash:

```
0  libGLU.dylib  halveImage_ubyte + 278          ← OOB (KERN_INVALID_ADDRESS)
1  libGLU.dylib  gluBuild2DMipmapsCTXCore        (takes _CGLContextObject*)
2  libGLU.dylib  gluBuild2DMipmapsCTX / gluBuild2DMipmaps
3  com.apple.Flurry (texture build at startup)
```

**Mechanism (inference, high confidence):** libGLU on 10.6 links
OpenGL.framework — under DYLD_FRAMEWORK_PATH substitution its gl*
resolve against the SUBSTITUTE. GLU's mipmap builder receives the
shim token as its CGL context and sizes its work from GL state
queries; a query returning garbage/0 through a misrouted wrapper
breaks its row/stride arithmetic → OOB write in halveImage_ubyte.
This is the substitute's "blast radius" question (the no-context
glClear probe was built for QuartzCore) now answered by a
measured, crashing consumer.

**Next step (queued):** identify which gl* entry libGLU resolves
against the substitute misroutes — nm the substitute's export
table for the GLU-consumed set (glGetIntegerv/pixel-store/
glTexImage2D family), then either export proper Mesa-forwarding
wrappers for them or treat libGLU as a consumer needing the
interpose layer too. Flurry is the regression test for it.

**Also this session: FMTX scope correction** —
the GL_MODELVIEW_MATRIX identity verdict covered the
FIXED-FUNCTION stack only; CompositorOGL passes transforms as
shader uniforms the fixed-function matrix cannot see, so FMTX
never constrained Gecko's destination. The walk instrument
upgrades to glScissor logging (per-quad clip scissors walk with
the damage region; readable from the substitute with no Gecko
internals). Plus a free no-code prediction queued: the feedback
loop, if unbounded, gives TEN strips for ten clicks, marching
down until off-window; if it stops, the loop story needs a bound.

---
Last updated: 2026-08-16 (entry 12: SOURCE SESSION — the strip IDENTIFIED as mTitlebarImage (tabs-in-titlebar snapshot); full path traced with line refs; the walk narrowed to two inputs; self-damaging-feedback hypothesis)

---

## 2026-08-16 (entry 12) — source session: the walk hunt; strip identified; path traced

**The strip is nsChildView's mTitlebarImage.** Chain (all
powerfox-browser/platform/widget/cocoa/): Gecko covers the
titlebar with the GL context and re-draws the real titlebar
controls into GL by snapshotting them (nsChildView.mm:3076-3085
comment); with tabs-in-titlebar the snapshot contains the TAB
STRIP, not just buttons — matching every screenshot. Evidence
closure: exactly ONE IOSurface upload ever logged (1453x27 =
RectTextureImage.mm:307-330 BindIOSurfaceToTexture, gated
`!mTexture` — first bind only); a Basic+GLPresenter window image
would be 1453x910 and logged uncapped (w>=1000) — never seen ⇒
the OGL compositor path is active, the titlebar image is the
only IOSurface in play.

**Path (line refs):** UpdateTitlebarCGContext (nsChildView.mm
:2915) snapshots controls into a POW2 bitmap (texSize =
RoundUpPow2, :2927-2931 → 2048x32) → MaybeDrawTitlebar (:3087)
→ RectTextureImage::UpdateFromCGContext (raw size 1453x27
surface, bitmap read at pow2 size, drawn OP_SOURCE clipped to
the dirty region) → RectTextureImage::Draw (:247): quad = full
surface, destination = aTransform.PostTranslate(aLocation) with
aLocation = mTitlebarRect.TopLeft() (:3110), renderOffset (0,0),
projection = the compositor's.

**The walk — narrowed to two inputs, not yet pinned:**
1. mTitlebarRect.TopLeft() (set :2637 =
   RectContainingTitlebarControls()) — should be static; would
   need the controls' frames to shift by exactly h per click.
2. **The compositor frame's render offset** — DrawWindowOverlay
   draws inside the frame CompositorOGL set up; if the frame's
   render origin follows the DAMAGE region's bounds and the
   damage walks, every overlay draw walks with it.
**Working hypothesis (unproven): a self-damaging feedback loop**
— an overlay draw at y covers real content there → native dirty
rect appears there (:2923 getAndResetNativeDirtyRect) → next
damage origin walks → the next overlay draw walks +h. Fits
"one new copy per click" and "full repaint clears" (resets the
damage region).

**Also recorded, a smell not the walk:** pow2 bitmap (2048x32)
feeding the raw-size surface (1453x27) — UpdateFromCGContext
reads the bitmap at its OWN size into the smaller target,
clipped; copies land at (0,0) stably — mismatch noted, not
implicated.

**Next read (one session, queued):** CompositorOGL::BeginFrame
render-offset computation + who chooses the damage region
origin; then RectContainingTitlebarControls. The fix, once the
walking input is named, is upstream-in-Gecko or
compensate-in-shim (log the render offset per frame in the
substitute to CONFIRM the walk's source before touching Gecko).

---
Last updated: 2026-08-16 (entry 11: CORNER-STAMP RUN — readback is FULL-FRAME host transfer, 0/4 survival every frame; magenta was Gecko's behavior; persistence ingredient relocated HOST-side — the virgl framebuffer never discards)

---

## 2026-08-16 (entry 11) — corner-stamp run: readback = full-frame; persistence is HOST-side

**Instrument (fc27bee1, normal run, no clear):** varying magic
stamped into 4 corner 8×8 blocks of guest render_buf immediately
before p_glReadPixels; same corners read immediately after.
**Result: survived=0/4 in ALL 12 startup frames** — post values
are host content (0xffffffff page white top corners,
0x00000000 transparent bottom), never the stamp.

**Verdict per entry 10's pre-registration: OVERWRITTEN ⇒ the
readback is a FULL-FRAME host→guest transfer per call.** No
guest-passthrough, no dirty-region retention in the readback
path. Therefore:

1. The SHIM_CLEAR_FRAME magenta experiment WAS measuring Gecko's
   behavior (not a readback artifact) — **Gecko does not
   full-redraw the window on damage; it relies on prior frame
   content surviving** (de-facto persistence dependence, tension
   with the no-BackingStore pixel format now RESOLVED in
   Gecko's favor-in-practice).
2. **The persistence ingredient of the artifact lives HOST-side:
   the virgl framebuffer is a single ever-accumulating target
   with no swap-discard.** Guest buffers were never the keepers —
   readbacks faithfully return the accumulated host framebuffer.
   (How the guest-side magenta reached the host in the clear run
   — bind-time guest→host upload of the backing is the leading
   candidate — remains unverified; noted, not load-bearing for
   the verdict.)

**Consequences:** shim-side discard emulation is UNSAFE (Gecko
depends on persistence — magenta proved it breaks). The fix must
target the stray draws themselves: Gecko-side, both ingredients
in Gecko's court after all — the WALK (unexplained) and the
stray one-frame draws (harmless under real 10.6 CGL de-facto
persistence? — on real 10.6 hardware the artifact evidently does
not occur, so whatever produces the strays here differs from
real-hardware conditions; the compositor source read remains the
avenue).

**Standing questions: the walk; full-window vs
damage-scissored composite (source read).** CORRECTION (
session close): the earlier phrasing "why real hardware shows no
strays" framed the wrong comparison. PowerFox works PERFECTLY on
a real-hardware Mac mini — running Apple's REAL OpenGL/CGL on a
real GPU; the substitute/Mesa/virgl stack is not in that path at
all. It exists BECAUSE the UTM/VM guest has no GPU. So the
question is not "why does Gecko misbehave here and not there" —
Gecko's code is constant. It is: **which of OUR emulated-CGL
behaviors diverges from real CGL to create the conditions (the
walk, the strays) that Gecko's identical code produces under
them.** The comparison class is our-semantics vs real-CGL-
semantics, end to end.

---
Last updated: 2026-08-16 (entry 10: Cmd+L/open test RUN — post-navigation frames pure magenta, but CONFOUNDED by uncharacterized virgl readback semantics; corner-stamp instrument pre-registered; compositor question returns to the source read)

---

## 2026-08-16 (entry 10) — the Cmd+L test run: INCONCLUSIVE, new confound named; readback semantics is the next instrument

**Run:** `open -a PowerFox http://example.com/` used as the
damage trigger (stronger than Cmd+L; ssh cannot inject keys).
First attempt VOIDED: the running magenta instance was
Apple-Event-unresponsive, so `open` RELAUNCHED the app without
the shim env (operational datum: check pid before trusting a
forwarded event). Redone cleanly: fresh shim instance
(SHIM_CLEAR_FRAME=1, pid 5181, pf14.log), settled, URL
forwarded — same process, frames flowed (6 armed dumps).

**Result: post-navigation frames are PURE MAGENTA (910/910
rows).** But NOT interpretable as "Gecko does not repaint":
- Startup diagnostics under the SAME clear show readbacks
  DELIVERING content (preswap render[0]=0xffffffff opaque white,
  pf14) — the readback path works with the clear active, early.
- Late frames pure magenta ⇒ somewhere between, readbacks
  stopped delivering host content into the guest buffer — OR
  Gecko genuinely stopped compositing. Distinguisher missing:
  **the virgl OSMesa readback semantics are UNCHARACTERIZED**
  (full-frame TRANSFER_FROM_HOST per call vs dirty-region
  transfers + guest-buffer passthrough — the latter feeds my own
  clear back for undamaged regions by construction).

**NEXT INSTRUMENT (pre-registered, one build): corner-stamp the
guest render_buf with a known pattern immediately before
p_glReadPixels; after the readback, check whether the stamp
survives in undamaged corners.** Survives ⇒ readback is
dirty-region/guest-passthrough (and the magenta experiment
measured the readback, not Gecko — persistence conclusion needs
redo); Overwritten ⇒ full-frame transfer (and the magenta WAS
Gecko's behavior: no repaint on damage ⇒ de-facto persistence
dependence ⇒ shim-side discard emulation unsafe).

**The compositor question (full-window vs damage-scissored
redraw) returns to the powerfox-browser source read — unaffected
by this confound. The walk remains the standing target.**

---
Last updated: 2026-08-16 (entry 9: CLEAR_FRAME RUN — magenta everywhere, clicks painted nothing; THE CONTRACT FOUND: Gecko requests DoubleBuffer WITHOUT BackingStore — back buffer undefined-after-swap by Apple semantics; our ever-persistent buffer converts stray draws into permanent artifacts; fix defined by contract, one keyboard test decides safety)

---

## 2026-08-16 (entry 9) — clear-frame run: magenta everywhere; the CGL contract found in Gecko source; session close

**SHIM_CLEAR_FRAME run (63d1ccd6):** banner active; startup diag
showed real content flowing early (readbacks returning page gray
0xffe8e8e8); then the screen AND the buffers went 100% magenta
(mean B=255,G=0,R=255; zero non-magenta rows across 11 armed
dumps) — the blind URL-bar clicks painted NOTHING over the
clear (they could not see the window; position-guessing). Two
readings recorded: (a) Gecko composited nothing further because
no damage event landed (idle/no valid input) — consistent with
damage-driven compositing; (b) compositing stopped when the
cleared buffer broke an assumption. INCONCLUSIVE on the
per-click-draw detail. Conclusive part: screen content is NOT
rebuilt from scratch per frame in this configuration.

**THE CONTRACT (read from powerfox-browser source, not inferred):
GLContextProviderCGL.mm:194-209 — the double-buffered context
requests NSOpenGLPFADoubleBuffer and NOT NSOpenGLPFABackingStore.**
Apple semantics: without the backing-store attribute the back
buffer is UNDEFINED after swap. Gecko works on real hardware
under those rules.

**The artifact's mechanism, now defined by contract:** our shim
provides an EVER-PERSISTENT back buffer — stricter than the
contract requires — which converts one-frame stray draws
(invisible on real hardware: wiped or arbitrary at next swap)
into PERMANENT visible artifacts. The accumulating strips are
that class: drawn once per click at walked positions, never
discarded. No accumulating draw LIST exists in Gecko (consistent
with FMTX identity, entry 7); the accumulation lived in OUR
buffer semantics all along.

**Fix direction (defined by the contract, safety TBD by one
measurement):** emulate the requested semantics — present and
leave the back buffer undefined (cleared). The magenta experiment
WAS exactly that semantic, and Gecko's response suggests
10.6-era Apple hardware may have RETAINED back-buffer contents
in practice (undefined ≠ discarded — Gecko may effectively rely
on persistence that real 10.6 CGL happened to provide). THE
DECIDING TEST (one keypress, no build): with a clear-frame run
up, press Cmd+L and TYPE — keyboard input reaches the focused
browser regardless of screen state, guaranteeing damage.
Content repaints over magenta ⇒ full-redraw-on-damage ⇒
swap-discard emulation is SAFE and is the fix (clear to
background, not magenta). Nothing repaints ⇒ Gecko depends on
de-facto persistence ⇒ fix must instead target the stray draws
themselves (find them in Gecko's toolbar repaint path with the
one-draw-per-click fact).

**Refinement at close — the artifact needs TWO separable
ingredients, only one accounted for:**
1. **The WALK: destination moves by h per repaint — UNEXPLAINED.**
   Pure persistence of a stationary stray gives ONE extra copy in
   one place, however many frames pass; observed is one NEW copy
   per click at 0, h, 2h.
2. **The PERSISTENCE: buffer never discards — accounted for**
   (entry 9's contract finding).
Fix only 2 → single misplaced strip remains. Fix only 1 → correct
strip redrawn in place, invisible. BOTH needed. The walk needs
finding either way.

**Tension with the magenta result, to resolve before banking the
contract story:** if Gecko truly treated the back buffer as
undefined-after-swap it would redraw the full window each frame —
and the magenta would have been overwritten. It wasn't → Gecko
relies on persistence for undamaged regions (de-facto, whatever
the pixel format says), OR the strip comes from a path outside
the GL composite. FIRST QUESTION of the source session: does
Gecko's Mac compositor redraw the full window per composite, or
scissor to damage and rely on the rest surviving? (First grep:
CompositorOGL uses per-QUAD clip scissors (:608,:1044), not a
frame-level damage scissor — the Render-path traversal read
decides.) The answer chooses the fix side: shim-side (emulate
discard) vs Gecko-side (find the walk). The walk is the target
either way.

**Session state at close:** Mesa-VirGL at 756325d0; guest running
the clear-frame build (magenta, pid 3101); every instrument
landed and recorded (pattern-fill both modes, drop-upload,
drop-blit, blit log, buffer dumps + flag arming, FMTX, matrix
probes, clear-frame, numeric BMP reader at /tmp/bmpread.py);
shim exonerated on every axis except buffer semantics — which
turns out to be the artifact.

---

## 2026-08-16 (entry 8) — the fifth confound; the persistence hypothesis; SHIM_CLEAR_FRAME pre-registered and built

**FIFTH MEASUREMENT CONFOUND (recorded in .claude/CLAUDE.md
too):** an artifact that comes and goes cannot be established fixed
by any single screenshot — only repetition can. "Same page, clean
now, duplicated then" = INTERMITTENT, not fixed. The CGL-forward
closure was exactly this error (acknowledged — the
inference-as-fact rule in its newest costume: the timeline and
error count were checkable and were not checked). Every A/B on the
duplication class carries a run count from here on.

**The persistence hypothesis — now explaining EVERY
observation including entry 7's matrix identity:** nothing has
established that render_buf is ever cleared between frames. If it
isn't, "two bands in one dumped buffer" is equally consistent with
ONE draw per click plus PIXEL PERSISTENCE (band N left from frame
N−1) as with two draws in one frame. Real double-buffered GL
DISCARDS the back buffer on swap; Gecko may lawfully rely on
swap-to-clear; our persistent render_buf never clears. Under this
model: accumulation = stale pixels (no GL state accumulates →
matrix identity, entry 7, consistent); full repaint clears it
(overwrites everything); intermittency on page content = partial
redraws leaving exposed stale regions (black rectangles = cleared
regions never repainted); one upload reused (texture drawn once per
event at a walking destination).

**SHIM_CLEAR_FRAME (built, 63d1ccd6, PRE-REGISTERED):** after each
swap, magenta-clear the fresh render_buf (old present; present_buf
still holds frame N for drawRect). Run: launch with
SHIM_CLEAR_FRAME=1, click the URL bar 3×:
- **Bands VANISH** → persistence + walking destination; the fix is
  swap-to-clear semantics in the shim — possibly THE artifact, a
  one-line fix class.
- **Bands REMAIN** → both draws genuinely occur per frame; the
  one-upload-two-draws reading stands; proceed to the Gecko source
  session with that question.
Side datum either way: any magenta visible on screen = regions
Gecko does NOT repaint per frame (its assumed-clear map).

**Next after the discriminator: powerfox-browser source session**
(hunt the accumulating/offset draw — or, if persistence confirmed,
never needed).

---

## 2026-08-16 (entry 7) — FMTX verdict: Gecko's draw loop; shim exonerated on every instrumentable axis

---

## 2026-08-16 (entry 7) — FMTX verdict: Gecko's draw loop; shim exonerated on every instrumentable axis

**The matrix discriminator (change-gated probe at flushBuffer, frame
end): after three URL-bar clicks with the chrome strip duplicated on
screen, the log holds exactly ONE line — identity, every frame,
artifact frames included. Zero foreign/kCGLBadContext activity.**
Per the pre-registration: the accumulation is INSIDE GECKO'S OWN
DRAW LOOP. The shim's GL state is exonerated — a persistent
accumulating matrix would survive to frame end by construction
(persistence was the mechanism); identity-at-frame-end kills it.
Caveat: a translation applied and popped WITHIN a frame would not
show at frame end — but that cannot produce cross-FRAME
accumulation, which is the observed signature (N clicks → N+1
copies persisting until full repaint).

**Page-content artifact — TIMELINE CORRECTION (the hopeful
attribution does not hold):** the "fixed by the forward" reading
fails both confirmations:
- Build timeline: session-restore duplication = pf7 (28de34cf,
  PRE-forward); google.com search-box duplication + black rects =
  pf8 (7a53ce73 = 8e13519, the forward ALREADY IN); current clean
  page = pf12 (d9e0e568, logging-only delta from pf8). Page
  duplication existed BOTH pre- and post-forward, and pf8→pf12
  contains no functional change.
- kCGLBadContext: zero now — but also zero in pf8 WHILE google.com
  duplicated (entry 5's branch 3: the rejection path was never
  exercised; it caused nothing observed).
**The page-content artifact is INTERMITTENT/state-dependent, NOT
closed.** What IS established: a proper before/after on identical
content (same session-restore page, same trigger clicks) drawing
clean — intermittency is real, not screenshot noise.

**Chrome strip: persists on EVERY build (pf7→pf12); now a
Gecko-side question.** The shim/transport is exonerated on every
axis we can instrument: upload path (pattern-fill: clean, singular),
readback (pattern + dumps), blit (drop-blit sole-drawer + CTM log:
single view, single CTM, no double-fire), GL state (this entry),
CGL policy (forward correct, unexercised), geometry coherence
(bounds = buffer = viewport).

**NEXT UNIT — powerfox-browser source (read-only reference at
~/powerfox-browser): the strip's draw path.** Signature to hunt:
on toolbar invalidation, a quad/layer/effect ADDED per damage
event and not retired — accumulating draw list (0, h, 2h stacking,
cleared by full repaint). Candidates: the toolbar's surface layer
compositing (surface → texture → compositor quad bookkeeping),
damage-region → destination-offset arithmetic, layer retention
across invalidations. The one-upload-two-draws fact (entries 4/6)
constrains it: same texture, successive draws at h-multiples.

---

## 2026-08-16 (entry 6) — google.com through the whole stack; artifact ACCUMULATES; black rectangles; pattern-fill discriminator pre-registered

**Milestone (screenshot): google.com renders** in the Snow
Leopard guest through Mesa → virgl → kext (2D transport) →
virglrenderer → Metal.

**Colour question CLOSED:** the Google logo shows blue, red,
yellow, green in the right places — no R/B swap anywhere in the
chain. The last open format question from the substitute era is
answered.

**Artifact class extended, with ACCUMULATION:** the search box
duplicates exactly like the tab strip — two instances, vertical
offset ≈ the element's own height; same signature as the
27-row strip at rows 0-30/30-60, now on PAGE CONTENT. And the
mechanism datum: **a second URL-bar click makes the strip a
TRIPLICATE — each damage event ADDS a copy; copies persist and
accumulate.** Not a moved redraw — an accumulating draw/layer.
(Numeric buffer read of the triplicate dump pending — both
vision analyzers reject the CDN-transformed PNGs; a
pure-Python BMP decode answers band counts/geometry more
rigorously anyway.)

**Black rectangles — NEW, not pure displacement:** they sit
where the first instance's background should be; previous
artifacts were correct-content-wrong-place, never absent
content. Two readings: (a) region cleared then redrawn
at the offset — cleared area exposed — ties to the SAME offset
bug; (b) first instance drawn with an unpopulated source — a
DISTINCT fault.

**PRE-REGISTERED discriminator: SHIM_PATTERN_FILL on
google.com.** Black regions come back CHECKERBOARDED → they are
IOSurface-borne; the offset story covers them (reading a).
Black regions STAY BLACK → separate fault, own thread
(reading b). Read NUMERICALLY from a buffer dump (period-8
alternation detector in the black regions), not by eye.

**One-bug-or-two result (entry 5's test):** branch 3 — ZERO
foreign CGLSetCurrentContext forwards in 5 min of pf8 (all
uploads clean, no error lines). The forward fix (8e13519)
stands as correct policy, unexercised: QuartzCore does not call
CGLSetCurrentContext through the substitute (private-symbol
path, inferred). The rejection was NOT the artifact trigger.

---

## 2026-08-16 (entry 5) — the :271/:617 policy fix, PRE-REGISTERED (one bug or two?)

**Finding:** both kCGLBadContext sites use the registry detection
correctly and the POLICY wrongly — "not one of ours" was meant to
mean "forward to the real framework"; at both it currently means
"reject." `:617` mostly moot (a forward exists at :612 when real
GL loaded). **`:271` (CGLSetCurrentContext) is the consequential
one:** if QuartzCore creates a real CGL context via private entry
points and makes it current, the substitute refuses — that
context can NEVER become current and everything QuartzCore does
after goes somewhere undefined. The IOSurface-present failure is
the visible symptom; SetCurrentContext is upstream of all of it.
(Fixed-forward diagnostic build skipped: log-instrument-then-fix
collapsed into fix-with-logging — the forward itself logs its
first firings and any residual errors.)

**TLS trap (design recorded BEFORE code):** the shim's
CGLGetCurrentContext derives from OSMesaGetCurrentContext (Mesa's
TLS) + registry reverse-lookup. Once real contexts can be current
on the same thread, that derivation answers stale. Fix: one
pthread TSD key records the LAST CGLContextObj set by
CGLSetCurrentContext (token or real); CGLGetCurrentContext
returns a REAL occupant from TSD directly, else falls to the
existing OSMesa-derivation; every OSMesaMakeCurrent site
(cgl_shim.mm :421/:456/:464/:477/:534/:685, substitute :211/:266/
:273) updates the TSD so the swizzle path cannot leave a stale
real occupant. Last-set-wins across both kinds.

**ONE-BUG-OR-TWO TEST (pre-registered before the build):**
after the forward lands, click the URL bar (the trigger) —
- **Duplication GONE and the foreign-forward log shows the path
  firing without error** → ONE BUG: the token rejection produced
  Gecko's fallback redraw; the duplicated strip was a symptom of
  the substitute's policy, not an independent Gecko defect.
- **Duplication PERSISTS with foreign forwards firing clean** →
  TWO BUGS: the artifact investigation continues into Gecko's
  compositor with one fewer confound.
- No foreign forwards fire at all → the CARenderer hypothesis is
  wrong about the entry point; back to instrumenting which CGL
  call returns the error (the survey showed only 2 CGL_FWD
  calls, both CGLSetOption — the failure lives in an IMPLEMENTED
  entry's reject path or a private symbol).

**Mechanism prediction:** foreign CGLSetCurrentContext forwards
fire at composite/present time (QuartzCore render thread),
≥1 per presented frame.

---

## 2026-08-16 (entry 4) — trigger-correlated dump: the duplication is IN THE BUFFER; Gecko composited the strip twice

**Instrument sequence that got here (each step pre-registered):**
- Buffer dump built (pre-blit site in shim_blit_present_buf; BMP
  bottom-up = screen-upright for free). First run captured only
  474×129 frames — the small-surface capture error the codebase
  already recorded once for the upload log; re-gated (w≥1000 or
  first 2).
- **Dump 07 (startup frame) verdict RETRACTED as overreach** (
  caught it): it showed a correct new-tab composite (strip at
  proper place, white page) — a NON-artifact frame. The artifact
  is transient (URL-bar click); a startup dump arbitrates nothing.
- **SHIM_DROP_BLIT run: screen completely white, no chrome** →
  the blit is the SOLE content source; "correct chrome is Gecko's
  software paint" inference FALSIFIED. Also falsified: any
  double-drawer model.
- **SHIM_LOG_BLITS: 66+ blits, ALL view=PixelHostingView** (not
  ChildView — the setView one-shot swizzles the PixelHostingView
  class), bounds 1391×886, CTM IDENTICAL every call
  [1 0 0 -1 0 886]. No double-blit, no CTM variance. (The
  NSView-level swizzle's "never fires for ChildView" comment
  predates the ChildView/PixelHostingView swizzle — both exist;
  only one fires.)
- **Trigger-correlated dump** (`touch /tmp/shim_dump_now` arms the
  next 6 blits): the URL bar was clicked, confirmed the artifact
  on screen, dump armed at that moment.

**VERDICT — dump 05 of the armed window (1453×910, artifact on
screen at capture): the buffer contains the duplication.** Two
identical tab-strip bands — rows 0-30 (y=0, the duplicate) and
rows 30-60 (the proper place) — session-restore page
("…is embarrassing.", the clipped-text class) rendered once
below. **Gecko's GL composite draws the same chrome content
twice, once at a dropped/zero destination offset. The blit, the
readback, and the kext are exonerated — each by its own gate.**

**Where the double-draw likely originates (INFERENCE, next
lead):** in-source comment in shim_childViewDrawRect — "its
IOSurface present fails with kCGLBadContext" — Gecko's
PixelHostingView present path errors EVERY FRAME under the
substitute. A Gecko error-path fallback/retry that composites the
chrome layer a second time (with the offset not applied) fits all
observations: duplication appears on a CHROME repaint trigger,
lands at y=0 (offset never applied on the retry), content
otherwise correct. What would settle it: find which CGL present
call returns kCGLBadContext (log in the substitute), make it
succeed or return success, and see whether the duplicate
vanishes. Alternative: the double-draw is pure Gecko (two quads);
then the fix belongs in the powerfox-browser source.

**SHIM_DROP_UPLOAD prediction (test built, not yet run):** both
strips go empty/black — both copies are GL quads sampling the
uploaded texture; the page stays (software-rendered into the
context? no — page is also GL; predict the whole composite goes
mostly empty). Either outcome further constrains the Gecko path.

**Session residuals:** pkill silently fails on this guest (two
lock-dialog incidents) — kill by explicit PID; the profile lock
is `.parentlock` INSIDE `Profiles/<name>/`, not at the app-support
top level.

---

## 2026-08-16 (entry 3) — drop gates + blit logging built (SHIM_DROP_UPLOAD / SHIM_DROP_BLIT / SHIM_LOG_BLITS)

Commits cf81469 + the trigger-arm build. Gates as pre-registered
in entry 2; SHIM_LOG_BLITS logs per-blit view class, bounds, and
entry CTM (double-blit detector); flag-file /tmp/shim_dump_now
arms the next 6 dumps for transient-artifact capture.

---

## 2026-08-16 (entry 2) — pattern-fill RUN 1: two artifacts, one eliminated

**Run:** PowerFox on the guest, fresh substitute at `/tmp/subst`
(md5 verified), `SHIM_PATTERN_FILL=1` + established env. Banner
fired; uploads: 15×15, 4×4, **#1 = 1391×27 bpr=5568 (chrome
strip)**. Screen observed via screenshot (all four
programmatic capture routes denied: ssh lacks window-server
context on 10.6; host screencapture lacks grant; `.command` via
`open` and `launchctl bsexec` produced nothing — eyes-on-screen
remain the visual instrument).

**Reading (primary — and it CORRECTS the automated image
analysis, which reported "no duplication" while the duplication
was present in the same image; recorded as an instrument-trust
lesson):** TWO artifacts, previously treated as one:

- **Artifact A — the checkerboarded top strip (1391×27):** clean,
  axis-aligned, singular, no shear. Everything through
  `CGLTexImageIOSurface2D` and nothing else — the pattern marks
  exactly that path, and the strip is all it marked.
- **Artifact B — the page-content duplication, UNMARKED by the
  pattern:** "is embarrassing." appears twice (y≈320 and y≈920),
  body text clipped on the left at a consistent x ("ible
  recovering your windows…", "e tabs that you think…", "w
  browsing session"), clipped text still perfectly rendered.

**Elimination (the run's result): the IOSurface upload path is
excluded as the source of the page-content artifact.** It
produces the strip and nothing else. The prior hypothesis "the
IOSurface upload carries the artifact content" was right about
the strip, wrong about the artifact that matters. One run
eliminated the entire hypothesis space the instrument was built
to explore, and split the problem in two.

**Artifact B's shape (analysis):** content repeated at a
vertical offset, clipped at constant left x, clipped text
perfectly rendered — a COPY WITH A WRONG SOURCE RECTANGLE, not a
rendering fault. Gecko scrolls and moves layers by copying
regions within its framebuffer; a wrong source or destination
origin in such a copy produces exactly this signature.

**Next discriminator — PRE-REGISTERED, one build, no
boot, no kext:** dump the OSMesa buffer immediately before the
drawRect: blit.
- **Duplication already in the buffer** → Gecko's compositing;
  investigation moves into Gecko's source (the wrong-source-rect
  copy is upstream of everything of ours).
- **Buffer clean, duplication on screen** → the blit introduces
  it; the fault is in the shim's presentation path.
Env-gated, BMP written to /tmp (uncompressed, hand-rolled
header — no image libs on the guest), retrieved via scp.

---

## 2026-08-16 (entry 1) — pattern-fill diagnostic: PRE-REGISTERED, built, awaiting its run

**The artifact class under investigation (frontier):** PowerFox chrome
artifacts — same content drawn twice at different destinations. Two
hypotheses already eliminated:

1. **Guest-side buffer staleness — FALSIFIED** (commit `685ba319`,
   `SHIM_SINGLE_BUF`): render_buf and present_buf aliased so partial damage
   lands on the last presented frame by construction; artifacts persisted.
2. **`GL_UNPACK_ROW_LENGTH` units bug — FIXED** (commit `26c82b3`):
   ROW_LENGTH was reset to a hardcoded 0 instead of the app's value, and
   ALIGNMENT leaked after IOSurface uploads; both now saved/restored around
   every upload. (This class is a reminder: pixel-store state is
   context-global.)

**The instrument (designed earlier, pre-registered here, never yet run):**
env-gated `SHIM_PATTERN_FILL` — overwrite every IOSurface upload's source
IN PLACE with a test pattern before the teximage, written with the
surface's OWN stride (`bpr`) and bpp. Pattern: 8-colour checkerboard with
8-pixel blocks, `palette[((x>>3)+(y>>3))&7]`. Pure horizontal bars would be
shear-INVISIBLE (a constant-along-x stripe is shift-invariant); the
checkerboard makes a stride fault read as a staircase and a duplicate
destination as offset ghosting/moiré.

**Pre-registered outcomes (decided before the run):**
- **Bars/checkerboard appear SHEARED (staircase rows)** → stride fault
  DOWNSTREAM of the upload: something between the teximage and the host
  (Mesa's virgl unpack, the transfer, or the winsys scatter list) is
  reading a different row pitch than the surface's `bpr`. Next step:
  compare the unpack stride at each hop (glTexImage2D row-length state,
  virgl `TRANSFER_TO_HOST_3D` box, kext scatter entries).
- **Checkerboard CLEAN but DUPLICATED at different destinations** → the
  upload/display path is stride-correct; the artifact is composite
  destinations — Gecko (or the PixelHostingView swizzle path) drawing the
  same content twice at different positions. Next step: Gecko-side
  (which layer/view draws where; the empty-frame-blit skip `8775b09`
  adjacent).
- Checkerboard clean and singular (artifact gone with pattern) → the fault
  is in Gecko's CONTENT, not the path — re-examine with real content,
  pattern as overlay instead of replacement.

Cost: one build, no boot, no kext. Run: guest, PowerFox with
`SHIM_PATTERN_FILL=1` in the environment, observe chrome vs content areas.

**State at ledger creation (from commit log, tip `685ba319`, branch
`cross-10.6`; not re-verified here):** both GL dispatch routes landed
(flat-namespace `__DATA,__interpose` `fd7b7cf`; two-level substitute
`4b7c463`); killtest / stress (resize + multi-context + CGLEnable) / smoke
PASS; **Gecko UI renders via the substitute** (`9fb95e8` — PixelHostingView
swizzle, IOSurface upload, blit flip); fixes since: empty-frame blit skip
(`8775b09`), pixel-store save/restore (`26c82b3`), uncapped large-surface
upload logging (d5644dc). Presentation-era history: drawRect swizzle RED
WINDOW (`ef2f574`).

**Open questions carried:**
- The chrome-artifact class (this entry).
- Sustained frame rate / real workload performance — unmeasured.
- The substitute's full CPU copy per texture per frame (noted at
  `substitute_cgl.c:436` — correct first, optimize later).

---

## Superseded

(none yet)
