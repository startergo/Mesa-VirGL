// stress_test.mm — exercises resize-under-load, multi-context, and CGLEnable.
//
// Three things no test has covered yet:
// 1. Resize: window grows and shrinks every 2s while animating.
//    Shim's bounds-check in flushBuffer reallocates the buffer.
// 2. Multi-context: two NSOpenGLContexts live simultaneously, each
//    rendering to its own view. Tests the registry and TLS.
// 3. CGLEnable: calls [ctx CGLContextObj] → CGLEnable through the
//    interpose layer. Shim should no-op for shim tokens.
//
// Run with shim:
//   DYLD_INSERT_LIBRARIES=/tmp/cgl_shim.dylib \
//   DYLD_LIBRARY_PATH=/tmp GALLIUM_DRIVER=virgl \
//   /tmp/stress_test

#define _SECURITY_SECKEYCHAIN_H_ 1

#include <mach/mach_time.h>
#include <stdio.h>
#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/OpenGL.h>
#import <QuartzCore/QuartzCore.h>

@interface StressView : NSView {
@private
    NSOpenGLContext *mCtx;
    NSOpenGLPixelFormat *mFmt;
    float mPhase;
    float mR, mG, mB;
    int mFrameCount;
    int mCglEnableErr;   /* Yovb4: CGLEnable result, reported at exit */
}
- (id)initWithFrame:(NSRect)frame colorR:(float)r colorG:(float)g colorB:(float)b;
- (int)frameCount;
- (int)cglEnableError;
- (void)renderFrame;
- (void)teardownGL;
@end

@implementation StressView

- (BOOL)isFlipped { return YES; }

- (id)initWithFrame:(NSRect)frame colorR:(float)r colorG:(float)g colorB:(float)b {
    self = [super initWithFrame:frame];
    if (!self) return nil;

    mR = r; mG = g; mB = b;
    mPhase = 0.0f;
    mFrameCount = 0;
    mCglEnableErr = 0;

    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAWindow, NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAColorSize, 32, NSOpenGLPFADepthSize, 16, 0
    };
    mFmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    mCtx = [[NSOpenGLContext alloc] initWithFormat:mFmt shareContext:nil];
    if (!mCtx) { [mFmt release]; [super dealloc]; return nil; }

    [mCtx setView:self];
    [mCtx makeCurrentContext];

    glDisable(GL_DEPTH_TEST);
    glClearColor(mR * 0.2f, mG * 0.2f, mB * 0.2f, 1.0f);

    // CGLEnable test — call through the interpose layer.
    // The shim should no-op for shim tokens (CGLEnable returns kCGLNoError).
    // Yovb4 (PR review): the result was printed and forgotten, so a shim that
    // BROKE CGLEnable still produced a green test. Keep it and report it.
    CGLContextObj cgl = (CGLContextObj)(uintptr_t)[mCtx CGLContextObj];
    CGLError err = CGLEnable(cgl, kCGLCEMPEngine);
    mCglEnableErr = (int)err;
    fprintf(stderr, "stress: CGLEnable(ctx=%p) → %d (expected 0)%s\n", cgl,
            (int)err, err == kCGLNoError ? "" : "  ← FAIL");

    return self;
}

- (int)cglEnableError { return mCglEnableErr; }

- (void)renderFrame {
    [mCtx makeCurrentContext];

    int w = (int)[self bounds].size.width;
    int h = (int)[self bounds].size.height;
    if (w < 1 || h < 1) return;

    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);

    mPhase += 0.03f;
    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();
    glRotatef(mPhase * 57.2957795f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_TRIANGLES);
        glColor3f(mR, mG * 0.3f, mB * 0.3f); glVertex2f( 0.0f,  0.7f);
        glColor3f(mR * 0.3f, mG, mB * 0.3f); glVertex2f(-0.7f, -0.4f);
        glColor3f(mR * 0.3f, mG * 0.3f, mB); glVertex2f( 0.7f, -0.4f);
    glEnd();

    [mCtx flushBuffer];
    mFrameCount++;
}

- (int)frameCount { return mFrameCount; }

- (void)teardownGL {
    if (mCtx) { [mCtx clearDrawable]; [mCtx release]; mCtx = nil; }
    if (mFmt) { [mFmt release]; mFmt = nil; }
}

- (void)dealloc { [self teardownGL]; [super dealloc]; }

@end

@interface StressAppDelegate : NSObject <NSApplicationDelegate> {
    NSWindow *mWin;
    StressView *mView1;  // primary context
    StressView *mView2;  // second context (multi-context test)
    NSTimer *mRenderTimer;
    NSTimer *mResizeTimer;
    int mResizePhase;
    int mTotalFrames;
    uint64_t mStartTime;
}
@end

@implementation StressAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;

    NSRect frame = NSMakeRect(100, 100, 600, 400);
    mWin = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSTitledWindowMask | NSClosableWindowMask |
                            NSMiniaturizableWindowMask | NSResizableWindowMask
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [mWin setTitle:@"Stress: resize + multi-context + CGLEnable"];
    [mWin setReleasedWhenClosed:NO];

    // Two views side by side — each gets its own NSOpenGLContext.
    // No NSSplitView (causes layout issues with this setup on 10.6).
    // Just two plain subviews in the content view.
    NSView *contentView = [mWin contentView];
    NSRect cb = [contentView bounds];
    NSRect leftRect = NSMakeRect(0, 0, cb.size.width / 2, cb.size.height);
    NSRect rightRect = NSMakeRect(cb.size.width / 2, 0, cb.size.width / 2, cb.size.height);

    mView1 = [[StressView alloc] initWithFrame:leftRect colorR:0.9 colorG:0.2 colorB:0.2];
    /* Yovb8 (PR review): autoresizing distributes the superview's delta only
     * over the flexible parts, proportionally to their ORIGINAL size. With
     * only WidthSizable set on BOTH halves, each view absorbed the whole
     * width delta and the two views overlapped after any horizontal resize.
     * Give the left view a flexible right margin and the right view a
     * flexible left margin (400/400 each initially), so each half takes
     * exactly half of the delta and they keep tiling. */
    [mView1 setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable | NSViewMaxXMargin];
    mView2 = [[StressView alloc] initWithFrame:rightRect colorR:0.2 colorG:0.9 colorB:0.2];
    [mView2 setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable | NSViewMinXMargin];

    [contentView addSubview:mView1];
    [contentView addSubview:mView2];

    [mWin makeKeyAndOrderFront:nil];

    mResizePhase = 0;
    mTotalFrames = 0;
    mStartTime = mach_absolute_time();

    // Render timer: ~30 Hz
    mRenderTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/30.0
                                                   target:self
                                                 selector:@selector(render)
                                               userInfo:nil repeats:YES];

    // Resize timer: every 2s, toggle window size between small and large
    mResizeTimer = [NSTimer scheduledTimerWithTimeInterval:2.0
                                                    target:self
                                                  selector:@selector(toggleResize)
                                                userInfo:nil repeats:YES];

    fprintf(stderr, "stress: running — two contexts, resize every 2s, CGLEnable tested at init\n");
}

- (void)render {
    [mView1 renderFrame];
    [mView2 renderFrame];
    mTotalFrames++;

    if (mTotalFrames % 30 == 0) {
        static mach_timebase_info_data_t tbi;
        if (tbi.denom == 0) mach_timebase_info(&tbi);
        uint64_t now = mach_absolute_time();
        double elapsed = (double)(now - mStartTime) * tbi.numer / tbi.denom / 1e9;
        fprintf(stderr, "stress: %d frames in %.1fs (%.1f fps), ctx1=%d ctx2=%d frames, win=%dx%d, cglEnable=%d/%d\n",
                mTotalFrames, elapsed, mTotalFrames / elapsed,
                [mView1 frameCount], [mView2 frameCount],
                (int)[[mWin contentView] bounds].size.width,
                (int)[[mWin contentView] bounds].size.height,
                [mView1 cglEnableError], [mView2 cglEnableError]);
        /* Yovb4: a nonzero CGLEnable result is a FAILED check, say so loudly. */
        if ([mView1 cglEnableError] != (int)kCGLNoError ||
            [mView2 cglEnableError] != (int)kCGLNoError) {
            fprintf(stderr, "stress: FAIL — CGLEnable did not return kCGLNoError "
                    "(ctx1=%d ctx2=%d)\n",
                    [mView1 cglEnableError], [mView2 cglEnableError]);
        }
    }
}

- (void)toggleResize {
    NSRect frame = [mWin frame];
    if (mResizePhase % 2 == 0) {
        // Grow
        [mWin setFrame:NSMakeRect(frame.origin.x, frame.origin.y, 900, 600)
               display:YES animate:NO];
        fprintf(stderr, "stress: resize → 900×600\n");
    } else {
        // Shrink
        [mWin setFrame:NSMakeRect(frame.origin.x, frame.origin.y, 400, 300)
               display:YES animate:NO];
        fprintf(stderr, "stress: resize → 400×300\n");
    }
    mResizePhase++;
}

- (void)dealloc {
    [mRenderTimer invalidate];
    [mResizeTimer invalidate];
    [mView1 release];
    [mView2 release];
    [mWin release];
    [super dealloc];
}

@end

int main(int argc, const char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSApplication *app = [NSApplication sharedApplication];
    StressAppDelegate *delegate = [[StressAppDelegate alloc] init];
    [app setDelegate:delegate];
    [app run];
    [delegate release];
    [pool release];
    return 0;
}
