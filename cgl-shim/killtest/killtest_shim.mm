// killtest_shim.mm — killtest converted from direct OSMesa to NSOpenGLContext.
//
// Same rotating triangle, same FPS measurement, but through the CGL shim:
//   [NSOpenGLContext] → [shim swizzle] → OSMesa → Mesa/virgl → host GPU
//   [ctx flushBuffer] → [shim] → swap + rebind + setNeedsDisplay → drawRect
//
// Run WITH the shim:
//   DYLD_INSERT_LIBRARIES=/tmp/cgl_shim.dylib \
//   DYLD_LIBRARY_PATH=/tmp \
//   GALLIUM_DRIVER=virgl \
//   /tmp/killtest_shim
//
// Run WITHOUT the shim (Apple software renderer, baseline):
//   /tmp/killtest_shim
//
// The difference in frame rate isolates the shim + virgl overhead from
// Apple's own renderer. Under the shim, -flushBuffer fires glFinish +
// glReadPixels (for virgl) + buffer swap + OSMesaMakeCurrent rebind +
// drawRect on every frame — the full compositor-thread path.

#define _SECURITY_SECKEYCHAIN_H_ 1

#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/OpenGL.h>

@interface KillTestShimView : NSView {
@private
    NSOpenGLContext *mCtx;
    NSOpenGLPixelFormat *mFmt;

    uint64_t mStartTime;
    uint64_t mFrameCount;
    uint64_t mLastReportTime;

    NSTimer *mTimer;
}
- (void)renderFrame:(NSTimer *)t;
- (void)teardownGL;
- (void)stopAnimation;
@end

@implementation KillTestShimView

- (BOOL)isFlipped { return YES; }

- (id)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (!self) return nil;

    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAWindow,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAColorSize, 32,
        NSOpenGLPFADepthSize, 16,
        0
    };
    mFmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!mFmt) {
        fprintf(stderr, "failed to create pixel format\n");
        [super dealloc];
        return nil;
    }

    // NSOpenGLContext creation — the shim intercepts this and creates
    // an OSMesa context internally. Without the shim, Apple's software
    // renderer is used.
    mCtx = [[NSOpenGLContext alloc] initWithFormat:mFmt shareContext:nil];
    if (!mCtx) {
        fprintf(stderr, "failed to create NSOpenGLContext\n");
        [mFmt release];
        [super dealloc];
        return nil;
    }

    [mCtx setView:self];
    [mCtx makeCurrentContext];

    // GL setup — same as original killtest.
    int viewportW = (int)[self bounds].size.width;
    int viewportH = (int)[self bounds].size.height;
    if (viewportW < 1) viewportW = 800;
    if (viewportH < 1) viewportH = 600;

    glViewport(0, 0, viewportW, viewportH);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
    glDisable(GL_DEPTH_TEST);

    mStartTime = mach_absolute_time();
    mFrameCount = 0;
    mLastReportTime = mStartTime;

    fprintf(stderr, "killtest_shim: context=%p viewport=%dx%d driver=%s\n",
            (void *)mCtx, viewportW, viewportH,
            getenv("GALLIUM_DRIVER") ?: "(Apple)");

    /* YSMPI (PR review): interval 0.0 — the run loop fires the timer on
     * every pass, so the frame rate is the throughput of the shim, not a
     * 30 Hz ceiling. The old 1/30.0 cap made every avg-fps number ≥30
     * impossible to read as a comparison between the two paths. */
    mTimer = [NSTimer scheduledTimerWithTimeInterval:0.0
                                              target:self
                                            selector:@selector(renderFrame:)
                                            userInfo:nil
                                             repeats:YES];
    return self;
}

- (void)renderFrame:(NSTimer *)t {
    (void)t;

    // T0 at the top of the render loop — pairs with the shim's T1 (just
    // before glFinish inside flushBuffer) to close the unmeasured span.
    // Same process as the shim (loaded via DYLD_INSERT_LIBRARIES), so
    // mach_absolute_time() is directly comparable. Pair the Nth KT_T0
    // with the Nth shim frame[] log in post-processing.
    uint64_t t0 = mach_absolute_time();

    // Time-based rotation: constant angular velocity regardless of frame rate.
    // Makes the killtest an honest visual indicator — smooth at high fps,
    // juddery at low fps, but always the same wall-clock time per revolution.
    // A frame-rate improvement (e.g. poll-loop fix) visibly smooths without
    // changing speed. Same GL commands per frame regardless of the angle.
    static mach_timebase_info_data_t spin_tbi;
    if (spin_tbi.denom == 0) mach_timebase_info(&spin_tbi);
    uint64_t spin_now = mach_absolute_time();
    double elapsed_s = (double)(spin_now - mStartTime) *
                       (double)spin_tbi.numer / (double)spin_tbi.denom / 1e9;
    float angle_deg = (float)(elapsed_s * 60.0); // 60 deg/s → one rev per 6s

    // Log T0 (gated to first 10 frames — same pattern as shim's diagnostics).
    // Absolute mach_absolute_time so it pairs directly with the shim's t1_abs.
    {
        static int s_t0_log = 0;
        if (s_t0_log < 10) {
            fprintf(stderr, "KT_T0[%d] abs=%llu\n",
                    s_t0_log, (unsigned long long)t0);
            s_t0_log++;
        }
    }

    /* YSMPC (PR review): the viewport was set once at init from the initial
     * frame, so resizing the window kept the initial-size viewport while
     * the surface grew — clipped content. Follow the view's live bounds;
     * glViewport only when they actually change. (The projection below is a
     * square 2×2 glOrtho, so on a non-square window the triangle is still
     * stretched by the aspect — unchanged, and now only because of the
     * projection, not the viewport.) */
    {
        NSRect b = [self bounds];
        static CGFloat s_lastW = -1.0, s_lastH = -1.0;
        if (b.size.width != s_lastW || b.size.height != s_lastH) {
            s_lastW = b.size.width;
            s_lastH = b.size.height;
            glViewport(0, 0, (GLsizei)b.size.width, (GLsizei)b.size.height);
        }
    }

    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();
    glRotatef(angle_deg, 0.0f, 0.0f, 1.0f);
    glBegin(GL_TRIANGLES);
        glColor3f(0.9f, 0.2f, 0.2f); glVertex2f( 0.0f,  0.6f);
        glColor3f(0.2f, 0.9f, 0.2f); glVertex2f(-0.6f, -0.4f);
        glColor3f(0.2f, 0.2f, 0.9f); glVertex2f( 0.6f, -0.4f);
    glEnd();

    // flushBuffer — the shim intercepts this and does:
    //   glFinish → glReadPixels → swap + OSMesaMakeCurrent rebind →
    //   performSelectorOnMainThread setNeedsDisplay → drawRect
    // Without the shim, Apple's flushBuffer swaps the CGL surface.
    [mCtx flushBuffer];

    mFrameCount++;
    uint64_t now = mach_absolute_time();
    uint64_t elapsedSinceReport = now - mLastReportTime;

    static mach_timebase_info_data_t sTBI;
    if (sTBI.denom == 0) mach_timebase_info(&sTBI);
    double elapsed_ns = (double)elapsedSinceReport *
                        (double)sTBI.numer / (double)sTBI.denom;
    if (elapsed_ns > 2.0e9) {
        double total_ns = (double)(now - mStartTime) *
                          (double)sTBI.numer / (double)sTBI.denom;
        double avg_fps = (double)mFrameCount / (total_ns / 1e9);
        fprintf(stderr, "killtest_shim: frame %6llu  avg=%5.1f fps  driver=%s\n",
                (unsigned long long)mFrameCount, avg_fps,
                getenv("GALLIUM_DRIVER") ?: "(Apple)");
        fflush(stderr);
        mLastReportTime = now;
    }
}

- (void)teardownGL {
    if (mCtx) { [mCtx clearDrawable]; [mCtx release]; mCtx = nil; }
    if (mFmt) { [mFmt release]; mFmt = nil; }
}

- (void)stopAnimation {
    /* YSMO9 (PR review): the repeating timer retains this view, so
     * dealloc (the only place that invalidated it) could not run while the
     * timer was live — the view and its GL context leaked after the window
     * closed. The delegate calls this from windowWillClose.
     * t8dme (PR review): clear mTimer as well — after invalidate the run
     * loop drops it, so a later -dealloc must not message the stale
     * pointer. */
    [mTimer invalidate];
    mTimer = nil;
}

- (void)dealloc {
    [mTimer invalidate];
    [self teardownGL];
    [super dealloc];
}

@end

@interface KillTestShimAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate> {
    NSWindow *mWin;
    KillTestShimView *mView;
}
- (void)windowWillClose:(NSNotification *)n;
@end

@implementation KillTestShimAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;

    NSRect frame = NSMakeRect(100, 100, 800, 600);
    mWin = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSTitledWindowMask | NSClosableWindowMask |
                            NSMiniaturizableWindowMask | NSResizableWindowMask
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [mWin setTitle:@"Killtest via NSOpenGLContext"];
    [mWin setReleasedWhenClosed:NO];
    /* YSMO9 (PR review): the view's repeating timer retains the view; break
     * the retain cycle from the window's close instead of waiting for a
     * dealloc that cannot run. */
    [mWin setDelegate:self];

    mView = [[KillTestShimView alloc] initWithFrame:[[mWin contentView] bounds]];
    [mView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [mWin setContentView:mView];
    [mWin makeKeyAndOrderFront:nil];

    fprintf(stderr, "killtest_shim: running — rotating triangle via NSOpenGLContext\n");
}

- (void)windowWillClose:(NSNotification *)n {
    (void)n;
    [mView stopAnimation];
}

- (void)dealloc {
    [mWin release];
    [mView release];
    [super dealloc];
}

@end

int main(int argc, const char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSApplication *app = [NSApplication sharedApplication];
    KillTestShimAppDelegate *delegate = [[KillTestShimAppDelegate alloc] init];
    [app setDelegate:delegate];
    [app run];
    [delegate release];
    [pool release];
    return 0;
}
