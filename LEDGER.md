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

Last updated: 2026-08-16 (TRIGGER DUMP: duplication IS in Gecko's composite — two identical strips at y=0 and proper place; blit exonerated; dump-07 overreach corrected; present-failure fallback hypothesis leads)

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
