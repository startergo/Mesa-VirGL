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

**GLU ROUTING FIX (user's layering rule, landed):** GLU is a
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

**Also this session (user, recorded): FMTX scope correction** —
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
damage-scissored composite (source read).** CORRECTION (user,
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
dumps) — the user's blind URL-bar clicks painted NOTHING over the
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

**User refinement at close — the artifact needs TWO separable
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

**FIFTH MEASUREMENT CONFOUND (user, recorded in .claude/CLAUDE.md
too):** an artifact that comes and goes cannot be established fixed
by any single screenshot — only repetition can. "Same page, clean
now, duplicated then" = INTERMITTENT, not fixed. The CGL-forward
closure was exactly this error (user's own acknowledgment — the
inference-as-fact rule in its newest costume: the timeline and
error count were checkable and were not checked). Every A/B on the
duplication class carries a run count from here on.

**The persistence hypothesis (user) — now explaining EVERY
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

**Milestone (user screenshot): google.com renders** in the Snow
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
content. Two readings (user): (a) region cleared then redrawn
at the offset — cleared area exposed — ties to the SAME offset
bug; (b) first instance drawn with an unpopulated source — a
DISTINCT fault.

**PRE-REGISTERED discriminator (user): SHIM_PATTERN_FILL on
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

**TLS trap (user, design recorded BEFORE code):** the shim's
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
- **Dump 07 (startup frame) verdict RETRACTED as overreach** (user
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
  next 6 blits): user clicked the URL bar, confirmed the artifact
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
strip)**. Screen observed via user's screenshot (all four
programmatic capture routes denied: ssh lacks window-server
context on 10.6; host screencapture lacks grant; `.command` via
`open` and `launchctl bsexec` produced nothing — user's eyes
remain the visual instrument).

**Reading (user's, primary — and it CORRECTS the automated image
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

**Artifact B's shape (user analysis):** content repeated at a
vertical offset, clipped at constant left x, clipped text
perfectly rendered — a COPY WITH A WRONG SOURCE RECTANGLE, not a
rendering fault. Gecko scrolls and moves layers by copying
regions within its framebuffer; a wrong source or destination
origin in such a copy produces exactly this signature.

**Next discriminator — PRE-REGISTERED (user), one build, no
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
