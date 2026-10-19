// blit_test.mm
//
// Kill-test v2: isolates the downstream presentation cost.
//
// The user-defined test was "OSMesa + softpipe + triangle + blit into NSView".
// The local inline_sw_helper.h was modified to force virgl as the only sw
// path, which makes softpipe-only OSMesa impossible to build without source
// changes. Rather than revert that local patch, this variant replaces the
// rendering backend with a hand-rolled CPU triangle rasterizer that writes
// directly into the BGRA buffer.
//
// What this measures: malloc'd 1920x1080 BGRA buffer -> NSBitmapImageRep
// -> composite into NSView -> setNeedsDisplay. Every frame. On the SL
// guest under TCG. That's the unavoidable downstream cost the swizzle
// would also pay -- the renderer above it (softpipe, virgl, whatever)
// only adds to this number, never subtracts.
//
// If THIS can't reach a usable rate, the architecture is dead regardless
// of which renderer sits on top. If it can, softpipe/virgl viability is a
// separate, smaller question.

#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define _SECURITY_SECKEYCHAIN_H_ 1   // bypass modern clang's SecKeychain rejection
#import <Cocoa/Cocoa.h>

// Minimal software triangle rasterizer. Covers the same pixel work softpipe
// would do for a single triangle, no GL machinery.
static inline void raster_triangle(uint32_t *fb, int w, int h,
                                    float ax, float ay, uint32_t ca,
                                    float bx, float by, uint32_t cb,
                                    float cx, float cy, uint32_t cc,
                                    float phase) {
    // rotate the triangle
    float s = sinf(phase), cs = cosf(phase);
    float rax = ax * cs - ay * s, ray = ax * s + ay * cs;
    float rbx = bx * cs - by * s, rby = bx * s + by * cs;
    float rcx = cx * cs - cy * s, rcy = cx * s + cy * cs;
    float minx = fminf(rax, fminf(rbx, rcx));
    float maxx = fmaxf(rax, fmaxf(rbx, rcx));
    float miny = fminf(ray, fminf(rby, rcy));
    float maxy = fmaxf(ray, fmaxf(rby, rcy));
    int x0 = (int)floorf((minx + 1.0f) * 0.5f * w);
    int x1 = (int)ceilf((maxx + 1.0f) * 0.5f * w);
    int y0 = (int)floorf((miny + 1.0f) * 0.5f * h);
    int y1 = (int)ceilf((maxy + 1.0f) * 0.5f * h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    float area = (rbx - rax) * (rcy - ray) - (rcx - rax) * (rby - ray);
    if (fabsf(area) < 1e-6f) return;
    float inv_area = 1.0f / area;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            float px = (float)x / (float)w * 2.0f - 1.0f;
            float py = (float)y / (float)h * 2.0f - 1.0f;
            float w0 = (rbx - px) * (rcy - py) - (rcx - px) * (rby - py);
            float w1 = (rcx - px) * (ray - py) - (rax - px) * (rcy - py);
            float w2 = (rax - px) * (rby - py) - (rbx - px) * (ray - py);
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                float b0 = w0 * inv_area;
                float b1 = w1 * inv_area;
                float b2 = w2 * inv_area;
                uint32_t a = (uint32_t)((ca >> 24) & 0xff);
                uint32_t r = (uint32_t)((ca >> 16) & 0xff) * b0 + (uint32_t)((cb >> 16) & 0xff) * b1 + (uint32_t)((cc >> 16) & 0xff) * b2;
                uint32_t g = (uint32_t)((ca >>  8) & 0xff) * b0 + (uint32_t)((cb >>  8) & 0xff) * b1 + (uint32_t)((cc >>  8) & 0xff) * b2;
                uint32_t b = (uint32_t)((ca >>  0) & 0xff) * b0 + (uint32_t)((cb >>  0) & 0xff) * b1 + (uint32_t)((cc >>  0) & 0xff) * b2;
                fb[y * w + x] = (a << 24) | ((r & 0xff) << 16) | ((g & 0xff) << 8) | (b & 0xff);
            }
        }
    }
}

@interface KillTestView : NSView {
@private
    uint32_t     *mFramebuffer;
    GLsizei       mFbWidth;
    GLsizei       mFbHeight;

    uint64_t      mStartTime;
    uint64_t      mFrameCount;
    uint64_t      mLastReportTime;
    uint64_t      mLastReportFrame;
    NSTimer      *mTimer;
}
- (void)renderFrame:(NSTimer *)t;
@end

@implementation KillTestView

- (BOOL)isFlipped { return YES; }

- (id)initWithFrame:(NSRect)frameRect fbWidth:(GLsizei)w fbHeight:(GLsizei)h {
    self = [super initWithFrame:frameRect];
    if (!self) return nil;

    mFbWidth = w;
    mFbHeight = h;
    mFramebuffer = (uint32_t *)malloc((size_t)w * (size_t)h * sizeof(uint32_t));
    if (!mFramebuffer) {
        fprintf(stderr, "failed to allocate %dx%d framebuffer\n", w, h);
        [super dealloc];
        return nil;
    }
    // initial clear
    memset(mFramebuffer, 0x18, (size_t)w * (size_t)h * sizeof(uint32_t));

    mStartTime = mach_absolute_time();
    mFrameCount = 0;
    mLastReportTime = mStartTime;
    mLastReportFrame = 0;
    return self;
}

- (void)startAnimation {
    if (mTimer) return;
    mTimer = [NSTimer scheduledTimerWithTimeInterval:1.0/60.0
                                             target:self
                                           selector:@selector(renderFrame:)
                                           userInfo:nil
                                            repeats:YES];
}

- (void)stopAnimation { [mTimer invalidate]; mTimer = nil; }

- (void)renderFrame:(NSTimer *)t {
    (void)t;
    static float phase = 0.0f;
    phase += 0.02f;

    // 1. CPU render: clear + single rotating triangle. Approximates the
    //    minimum per-frame CPU raster cost; softpipe would do more.
    for (int i = 0; i < mFbWidth * mFbHeight; i++) {
        mFramebuffer[i] = 0xff18181a;  // BGRA clear
    }
    raster_triangle(mFramebuffer, mFbWidth, mFbHeight,
                    0.0f, 0.6f, 0xff3320e6,    // red (BGRA)
                    -0.6f, -0.4f, 0xff20e633,  // green
                     0.6f, -0.4f, 0xffe62033,  // blue
                    phase);

    // 2. CPU blit into NSView via NSBitmapImageRep composite.
    NSRect bounds = [self bounds];
    int viewW = (int)bounds.size.width;
    int viewH = (int)bounds.size.height;
    if (viewW < 1 || viewH < 1) { mFrameCount++; return; }

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
                     bytesPerRow:mFbWidth * 4
                    bitsPerPixel:32];

    [NSGraphicsContext saveGraphicsState];
    /* renderFrame: runs from the
     * animation timer, not from drawRect: — there is no current
     * NSGraphicsContext, so drawInRect: had nowhere to draw and the
     * frame was never presented (the fps numbers measured the timer
     * cadence, not the blit). lockFocus establishes a context targeting
     * this view; unlockFocus flushes it. */
    [self lockFocus];
    [rep drawInRect:bounds
           fromRect:NSZeroRect
          operation:NSCompositeCopy
           fraction:1.0f
     respectFlipped:YES
              hints:nil];
    [self unlockFocus];
    [NSGraphicsContext restoreGraphicsState];
    [rep release];

    [self setNeedsDisplay:YES];

    // 3. Measure
    mFrameCount++;
    uint64_t now = mach_absolute_time();
    uint64_t elapsedSinceReport = now - mLastReportTime;
    static mach_timebase_info_data_t sTBI;
    if (sTBI.denom == 0) mach_timebase_info(&sTBI);
    double elapsed_ns = (double)elapsedSinceReport *
                        (double)sTBI.numer / (double)sTBI.denom;
    if (elapsed_ns > 5.0e9) {
        double total_ns = (double)(now - mStartTime) *
                          (double)sTBI.numer / (double)sTBI.denom;
        double avg_fps = (double)mFrameCount / (total_ns / 1e9);
        double recent_fps = (double)(mFrameCount - mLastReportFrame) /
                            (elapsed_ns / 1e9);
        fprintf(stdout,
                "frame %6llu  avg=%5.1f fps  window=%5.1f fps  fb=%dx%d  view=%dx%d\n",
                (unsigned long long)mFrameCount, avg_fps, recent_fps,
                mFbWidth, mFbHeight, viewW, viewH);
        fflush(stdout);
        mLastReportTime = now;
        mLastReportFrame = mFrameCount;
    }
}

- (void)dealloc {
    [self stopAnimation];
    if (mFramebuffer) { free(mFramebuffer); mFramebuffer = NULL; }
    [super dealloc];
}

@end

@interface KillTestAppDelegate : NSObject <NSApplicationDelegate> @end
@implementation KillTestAppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n {
    (void)n;
    GLsizei fbW = 1920, fbH = 1080;
    NSRect winRect = NSMakeRect(100, 100, 800, 600);
    NSWindow *win = [[NSWindow alloc]
        initWithContentRect:winRect
                  styleMask:NSTitledWindowMask | NSClosableWindowMask |
                            NSMiniaturizableWindowMask | NSResizableWindowMask
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"OSMesa kill-test (CPU raster, 1920x1080 -> 800x600 blit)"];
    [win setReleasedWhenClosed:NO];
    KillTestView *view = [[KillTestView alloc]
        initWithFrame:[[win contentView] bounds] fbWidth:fbW fbHeight:fbH];
    [view setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [win setContentView:view];
    [win makeKeyAndOrderFront:nil];
    [win center];
    [view startAnimation];
    fprintf(stdout,
            "kill-test running: CPU rasterizer %dx%d -> 800x600 view, "
            "NSTimer ~60Hz target. (No OSMesa -- local inline_sw_helper.h "
            "patch forces virgl; testing downstream blit cost only.)\n",
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
