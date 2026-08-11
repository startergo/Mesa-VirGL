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

    mTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/30.0
                                              target:self
                                            selector:@selector(renderFrame:)
                                            userInfo:nil
                                             repeats:YES];
    return self;
}

- (void)renderFrame:(NSTimer *)t {
    (void)t;

    // Same animated triangle as the original killtest.
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

- (void)dealloc {
    [mTimer invalidate];
    [self teardownGL];
    [super dealloc];
}

@end

@interface KillTestShimAppDelegate : NSObject <NSApplicationDelegate> {
    NSWindow *mWin;
    KillTestShimView *mView;
}
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

    mView = [[KillTestShimView alloc] initWithFrame:[[mWin contentView] bounds]];
    [mView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [mWin setContentView:mView];
    [mWin makeKeyAndOrderFront:nil];

    fprintf(stderr, "killtest_shim: running — rotating triangle via NSOpenGLContext\n");
}

- (void)dealloc {
    [mWin release];
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
