// killtest/main.mm
//
// Kill-test for the CGL-shim architecture. Isolates the unavoidable cost
// of the presentation path: OSMesa (softpipe) -> CPU readback -> CPU blit
// into NSView backing -> CoreGraphics composite. No virgl, no swizzle.
//
// Run on the 10.6 guest. Measures achieved FPS rendering a 1920x1080
// animated triangle into a smaller window, every frame, on the main thread.

#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Modern clang rejects the 10.6 SDK's SecKeychain.h AUTH_TYPE_FIX_ macro
// (signed left shift discards bits). Guard out the SecKeychain API surface
// we don't use, then pull Cocoa.
#define _SECURITY_SECKEYCHAIN_H_ 1

#import <Cocoa/Cocoa.h>

#include <GL/gl.h>
#include <GL/osmesa.h>

@interface KillTestView : NSView {
@private
    OSMesaContext mCtx;
    void         *mFramebuffer;
    GLsizei       mFbWidth;
    GLsizei       mFbHeight;
    GLint         mRowbytes;

    uint64_t      mStartTime;
    uint64_t      mFrameCount;
    uint64_t      mLastReportTime;

    NSTimer      *mTimer;
}
- (void)renderFrame:(NSTimer *)t;
- (void)teardownGL;
@end

@implementation KillTestView

- (BOOL)isFlipped { return YES; }

- (id)initWithFrame:(NSRect)frameRect fbWidth:(GLsizei)w fbHeight:(GLsizei)h {
    self = [super initWithFrame:frameRect];
    if (!self) return nil;

    mFbWidth = w;
    mFbHeight = h;
    mRowbytes = w * 4;
    mFramebuffer = malloc((size_t)w * (size_t)h * 4);
    if (!mFramebuffer) {
        fprintf(stderr, "failed to allocate %dx%d framebuffer\n", w, h);
        [super dealloc];
        return nil;
    }

    // softpipe via GALLIUM_DRIVER env. Force it here so we don't depend
    // on the caller setting it.
    setenv("GALLIUM_DRIVER", "softpipe", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);

    mCtx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
    if (!mCtx) {
        fprintf(stderr, "OSMesaCreateContextExt failed\n");
        free(mFramebuffer);
        mFramebuffer = NULL;
        [super dealloc];
        return nil;
    }

    if (!OSMesaMakeCurrent(mCtx, mFramebuffer, GL_UNSIGNED_BYTE, w, h)) {
        fprintf(stderr, "OSMesaMakeCurrent failed\n");
        OSMesaDestroyContext(mCtx);
        mCtx = NULL;
        free(mFramebuffer);
        mFramebuffer = NULL;
        [super dealloc];
        return nil;
    }

    // Mesa setup: viewport + ortho projection for a 2D-ish triangle.
    glViewport(0, 0, w, h);
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

    return self;
}

- (void)startAnimation {
    if (mTimer) return;
    // ~60 Hz target; actual rate measured below.
    mTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
                                             target:self
                                           selector:@selector(renderFrame:)
                                           userInfo:nil
                                            repeats:YES];
}

- (void)stopAnimation {
    [mTimer invalidate];
    mTimer = nil;
}

- (void)renderFrame:(NSTimer *)t {
    (void)t;

    // 1. Render into OSMesa framebuffer via softpipe.
    static float phase = 0.0f;
    phase += 0.02f;

    glClear(GL_COLOR_BUFFER_BIT);
    glLoadIdentity();
    glRotatef(phase * 57.2957795f, 0.0f, 0.0f, 1.0f);
    glBegin(GL_TRIANGLES);
        glColor3f(0.9f, 0.2f, 0.2f); glVertex2f( 0.0f,  0.6f);
        glColor3f(0.2f, 0.9f, 0.2f); glVertex2f(-0.6f, -0.4f);
        glColor3f(0.2f, 0.2f, 0.9f); glVertex2f( 0.6f, -0.4f);
    glEnd();
    glFinish();  // force softpipe to complete

    /* nothing else happens between
     * the render and the blit — this test measures the frame cost of
     * render + present, and a per-frame BGRA→RGBA conversion loop
     * (added earlier to fix the channel order for the rep) was CPU
     * work the real presentation path never does (the stub presents
     * the BGRA buffer as-is). The context is now created OSMESA_RGBA
     * (line 66) so the rep can consume the buffer directly. */

    // 2. CPU blit: copy the OSMesa RGBA buffer into the view's backing
    //    store via NSBitmapImageRep + composite. This is the exact path
    //    the swizzle would take for CGLFlushDrawable.
    NSRect bounds = [self bounds];
    int viewW = (int)bounds.size.width;
    int viewH = (int)bounds.size.height;
    if (viewW < 1 || viewH < 1) {
        mFrameCount++;
        return;
    }

    unsigned char *planes[1] = { (unsigned char *)mFramebuffer };
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:planes
                      pixelsWide:mFbWidth
                      pixelsHigh:mFbHeight
                   bitsPerSample:8
                 samplesPerPixel:4
                        hasAlpha:YES
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:mRowbytes
                    bitsPerPixel:32];

    [NSGraphicsContext saveGraphicsState];
    [NSGraphicsContext setCurrentContext:[NSGraphicsContext currentContext]];
    [rep drawInRect:bounds
           fromRect:NSZeroRect
          operation:NSCompositeCopy
           fraction:1.0f
     respectFlipped:NO
              hints:nil];
    [NSGraphicsContext restoreGraphicsState];
    [rep release];

    [self setNeedsDisplay:YES];

    // 3. Measure.
    mFrameCount++;
    uint64_t now = mach_absolute_time();
    uint64_t elapsedSinceReport = now - mLastReportTime;

    static mach_timebase_info_data_t sTBI;
    if (sTBI.denom == 0) mach_timebase_info(&sTBI);
    double elapsed_ns = (double)elapsedSinceReport *
                        (double)sTBI.numer / (double)sTBI.denom;
    if (elapsed_ns > 2.0e9 /* ~2s */) {
        double total_ns = (double)(now - mStartTime) *
                          (double)sTBI.numer / (double)sTBI.denom;
        double avg_fps = (double)mFrameCount / (total_ns / 1e9);
        double recent_fps = (double)(mFrameCount) /
                            (elapsed_ns / 1e9);  // approximate (since last report)
        fprintf(stdout,
                "frame %6llu  avg=%5.1f fps  recent=%5.1f fps  fb=%dx%d  view=%dx%d\n",
                (unsigned long long)mFrameCount, avg_fps, recent_fps,
                mFbWidth, mFbHeight, viewW, viewH);
        fflush(stdout);
        mLastReportTime = now;
    }
}

- (void)teardownGL {
    if (mCtx) {
        OSMesaDestroyContext(mCtx);
        mCtx = NULL;
    }
    if (mFramebuffer) {
        free(mFramebuffer);
        mFramebuffer = NULL;
    }
}

- (void)dealloc {
    [self stopAnimation];
    [self teardownGL];
    [super dealloc];
}

@end

@interface KillTestAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation KillTestAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    GLsizei fbW = 1920;
    GLsizei fbH = 1080;
    NSRect winRect = NSMakeRect(100, 100, 800, 600);

    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:winRect
                  styleMask:NSTitledWindowMask | NSClosableWindowMask |
                            NSMiniaturizableWindowMask | NSResizableWindowMask
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"OSMesa kill-test (softpipe, 1920x1080 -> blit)"];
    [win setReleasedWhenClosed:NO];

    KillTestView *view = [[KillTestView alloc]
        initWithFrame:[[win contentView] bounds] fbWidth:fbW fbHeight:fbH];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [win setContentView:view];
    [win makeKeyAndOrderFront:nil];
    [win center];

    [view startAnimation];
    fprintf(stdout,
            "kill-test running: OSMesa %dx%d -> 800x600 view, "
            "softpipe backend, NSTimer ~60Hz target\n",
            fbW, fbH);
    fflush(stdout);
}
@end

int main(int argc, const char *argv[]) {
    (void)argc; (void)argv;
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSApplication *app = [NSApplication sharedApplication];
    KillTestAppDelegate *delegate = [[KillTestAppDelegate alloc] init];
    [app setDelegate:delegate];
    [app run];
    [delegate release];
    [pool release];
    return 0;
}
