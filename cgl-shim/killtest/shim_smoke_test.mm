// shim_smoke_test.mm — minimal Cocoa app that uses NSOpenGLContext.
//
// Run WITH the CGL shim to test the swizzle + presentation path:
//
//   DYLD_INSERT_LIBRARIES=/tmp/cgl_shim.dylib \
//   DYLD_LIBRARY_PATH=/tmp \
//   GALLIUM_DRIVER=virgl \
//   /tmp/shim_smoke_test
//
// Run WITHOUT the shim as a baseline (uses Apple's software renderer):
//
//   /tmp/shim_smoke_test
//
// Expected: a window filled with solid red (0.8, 0.2, 0.2).
// If the window is black or unchanged with no crash, the swizzles
// installed but presentation never reached the view — check whether
// -flushBuffer is being called at all.

#define _SECURITY_SECKEYCHAIN_H_ 1

#import <Cocoa/Cocoa.h>
#import <OpenGL/gl.h>
#import <OpenGL/OpenGL.h>

@interface ShimTestView : NSView {
    NSOpenGLContext *mCtx;
    NSOpenGLPixelFormat *mFmt;
    NSTimer *mTimer;
    int mFrameCount;
}
- (void)renderFrame:(NSTimer *)t;
@end

@implementation ShimTestView

- (BOOL)isFlipped { return YES; }

- (id)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;

    // Minimal pixel format — 32-bit colour, double-buffered.
    NSOpenGLPixelFormatAttribute attrs[] = {
        NSOpenGLPFAWindow,
        NSOpenGLPFADoubleBuffer,
        NSOpenGLPFAColorSize, 32,
        NSOpenGLPFADepthSize, 16,
        0
    };
    mFmt = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
    if (!mFmt) {
        fprintf(stderr, "shim_smoke_test: failed to create pixel format\n");
        [super dealloc];
        return nil;
    }

    // Create context — the shim intercepts this and creates an OSMesa context.
    mCtx = [[NSOpenGLContext alloc] initWithFormat:mFmt shareContext:nil];
    if (!mCtx) {
        fprintf(stderr, "shim_smoke_test: failed to create NSOpenGLContext\n");
        [mFmt release];
        [super dealloc];
        return nil;
    }

    // Attach to this view and make current.
    [mCtx setView:self];
    [mCtx makeCurrentContext];

    // GL setup.
    glClearColor(0.8f, 0.2f, 0.2f, 1.0f);
    glDisable(GL_DEPTH_TEST);

    mFrameCount = 0;
    fprintf(stderr, "shim_smoke_test: context created, view attached, starting timer\n");

    mTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/30.0
                                              target:self
                                            selector:@selector(renderFrame:)
                                            userInfo:nil
                                             repeats:YES];
    return self;
}

- (void)renderFrame:(NSTimer *)t {
    (void)t;
    mFrameCount++;

    // Clear to the known colour, then flush.
    // The shim's -flushBuffer swizzle handles presentation.
    glClear(GL_COLOR_BUFFER_BIT);
    [mCtx flushBuffer];

    if (mFrameCount == 1) {
        fprintf(stderr, "shim_smoke_test: first frame rendered + flushBuffer called\n");
    }
    if (mFrameCount % 60 == 0) {
        fprintf(stderr, "shim_smoke_test: %d frames rendered\n", mFrameCount);
    }
}

- (void)dealloc {
    [mTimer invalidate];
    if (mCtx) { [mCtx clearDrawable]; [mCtx release]; mCtx = nil; }
    if (mFmt) { [mFmt release]; mFmt = nil; }
    [super dealloc];
}

@end

@interface ShimTestAppDelegate : NSObject <NSApplicationDelegate> {
    NSWindow *mWin;
    ShimTestView *mView;
}
@end

@implementation ShimTestAppDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;

    NSRect frame = NSMakeRect(100, 100, 400, 300);
    mWin = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSTitledWindowMask | NSClosableWindowMask |
                            NSMiniaturizableWindowMask | NSResizableWindowMask
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [mWin setTitle:@"CGL Shim Smoke Test"];
    [mWin setReleasedWhenClosed:NO];

    mView = [[ShimTestView alloc] initWithFrame:[[mWin contentView] bounds]];
    [mView setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [mWin setContentView:mView];
    [mWin makeKeyAndOrderFront:nil];

    fprintf(stderr, "shim_smoke_test: window created, view installed\n");
}

- (void)dealloc {
    [mWin release];
    [super dealloc];
}

@end

int main(int argc, const char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSApplication *app = [NSApplication sharedApplication];
    ShimTestAppDelegate *delegate = [[ShimTestAppDelegate alloc] init];
    [app setDelegate:delegate];
    [app run];
    [delegate release];
    [pool release];
    return 0;
}
