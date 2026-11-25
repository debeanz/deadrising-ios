// ios_overlay.mm - everything that is not the game on iPhone / iPad (ios_overlay.h): the on-screen controller, the
// Settings panel, the live log and the frame counter, in UIKit and Core Animation above the game. The controller and
// the panel follow the user's Tunic port (TunicControls.m: the mobile kit's layout, look and rows, rebuilt natively);
// presses go into the virtual pad the game reads (hid/input.h, Input::SetTouchPad). Main thread only.
//
// Settings: Dead Rising's own (user/config.h, saved to recomp_config.toml) and the touch ones (NSUserDefaults, the
// same keys as the Tunic port and the start screen: PortTouchControls, PortTouchOpacity 20..100, PortTouchSize 70..150,
// PortFloatingStick, PortHaptics).

#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>

#include <string>
#include <string_view>
#include <vector>
#include <cmath>
#include <SDL.h>
#include <SDL_syswm.h>
#include "ios_overlay.h"
#include <gpu/video.h>
#include <hid/input.h>
#include <os/logger.h>
#include <os/memory_info.h>
#include <user/config.h>
#include <fmt/core.h>

static void DRLog(NSString* text)
{
    os::logger::Log(fmt::format("[overlay] {}", text.UTF8String));
}

/* ================================================================ settings */
static NSString* const DRChanged = @"DRTouchSettingsChanged";
static NSUserDefaults* DRDefaults() { return NSUserDefaults.standardUserDefaults; }
static void DRRegisterDefaults()
{
    [DRDefaults() registerDefaults:@{ @"PortTouchControls": @YES, @"PortTouchOpacity": @65, @"PortTouchSize": @100,
                                      @"PortFloatingStick": @NO, @"PortHaptics": @YES }];
}
static NSInteger DRInt(NSString* key) { return [DRDefaults() integerForKey:key]; }
static BOOL DRBool(NSString* key) { return [DRDefaults() boolForKey:key]; }

/* ================================================================ the virtual pad (hid/input.h) */
namespace DRBit
{
enum : uint16_t
{
    DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008, START = 0x0010, BACK = 0x0020,
    LEFT_THUMB = 0x0040, RIGHT_THUMB = 0x0080, LEFT_SHOULDER = 0x0100, RIGHT_SHOULDER = 0x0200, A = 0x1000, B = 0x2000,
    X = 0x4000, Y = 0x8000,
};
}
static Input::Pad g_pad;

/* button ids as the kit numbers them: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LT, 7 RT, 8 L3, 9 R3, 10 Back, 11 Start */
static void DRPadButton(int button, BOOL down)
{
    using namespace DRBit;
    static const uint16_t bits[12] = { A, B, X, Y, LEFT_SHOULDER, RIGHT_SHOULDER, 0, 0, LEFT_THUMB, RIGHT_THUMB, BACK, START };
    if (button == 6)
        g_pad.leftTrigger = down ? 255 : 0;
    else if (button == 7)
        g_pad.rightTrigger = down ? 255 : 0;
    else if (button >= 0 && button < 12)
        g_pad.buttons = down ? (g_pad.buttons | bits[button]) : (g_pad.buttons & ~bits[button]);
    Input::SetTouchPad(g_pad);
}
static void DRPadDpad(int x, int y)
{
    using namespace DRBit;
    g_pad.buttons &= ~(DPAD_UP | DPAD_DOWN | DPAD_LEFT | DPAD_RIGHT);
    if (y > 0) g_pad.buttons |= DPAD_UP;
    if (y < 0) g_pad.buttons |= DPAD_DOWN;
    if (x < 0) g_pad.buttons |= DPAD_LEFT;
    if (x > 0) g_pad.buttons |= DPAD_RIGHT;
    Input::SetTouchPad(g_pad);
}
static void DRPadStick(BOOL right, float x, float y)
{
    const int16_t sx = int16_t(std::fmax(-1.0f, std::fmin(1.0f, x)) * 32767.0f), sy = int16_t(std::fmax(-1.0f, std::fmin(1.0f, y)) * 32767.0f);
    if (right) { g_pad.thumbRX = sx; g_pad.thumbRY = sy; } else { g_pad.thumbLX = sx; g_pad.thumbLY = sy; }
    Input::SetTouchPad(g_pad);
}
static void DRPadRelease()
{
    g_pad = Input::Pad{};
    Input::SetTouchPad(g_pad);
}
static BOOL DRPadWanted() { return !Input::ControllerConnected(); }
static BOOL DRGameDrawing() { return Video::GameFrames() > 0; }
static GCExtendedGamepad* DRRealGamepad()
{
    for (GCController* c in GCController.controllers)
        if (c.extendedGamepad) return c.extendedGamepad;
    return nil;
}

/* ================================================================ look (the kit's TouchStyle) */
static UIColor* DRBody(CGFloat a) { return [UIColor colorWithRed:0.075 green:0.08 blue:0.10 alpha:a]; } /* smoked glass */
static UIColor* DRNeutral() { return [UIColor colorWithRed:0.92 green:0.94 blue:1.0 alpha:1]; }
static UIColor* DRGreen() { return [UIColor colorWithRed:0.36 green:0.80 blue:0.20 alpha:1]; }
static UIColor* DRRed() { return [UIColor colorWithRed:0.93 green:0.24 blue:0.21 alpha:1]; }
static UIColor* DRBlue() { return [UIColor colorWithRed:0.20 green:0.52 blue:0.96 alpha:1]; }
static UIColor* DRYellow() { return [UIColor colorWithRed:1.00 green:0.78 blue:0.15 alpha:1]; }
static UIColor* DRBlood() { return [UIColor colorWithRed:0.80 green:0.10 blue:0.08 alpha:1]; }

static UIBezierPath* DRRoundedPolygon(const CGPoint* p, int n, CGFloat r)
{
    UIBezierPath* path = [UIBezierPath bezierPath];
    for (int i = 0; i < n; i++)
    {
        CGPoint a = p[(i + n - 1) % n], b = p[i], c = p[(i + 1) % n];
        CGFloat la = hypot(a.x - b.x, a.y - b.y), lc = hypot(c.x - b.x, c.y - b.y), rr = MIN(r, MIN(la, lc) / 2);
        CGPoint s = CGPointMake(b.x + (a.x - b.x) / la * rr, b.y + (a.y - b.y) / la * rr);
        CGPoint e = CGPointMake(b.x + (c.x - b.x) / lc * rr, b.y + (c.y - b.y) / lc * rr);
        if (i == 0) [path moveToPoint:s]; else [path addLineToPoint:s];
        [path addQuadCurveToPoint:e controlPoint:b];
    }
    [path closePath];
    return path;
}

/* SF Symbol drawn white into an image for a layer */
static CGImageRef DRSymbol(NSString* name, CGFloat pt)
{
    UIImageSymbolConfiguration* cfg = [UIImageSymbolConfiguration configurationWithPointSize:pt weight:UIImageSymbolWeightSemibold];
    UIImage* img = [[UIImage systemImageNamed:name withConfiguration:cfg] imageWithTintColor:UIColor.whiteColor renderingMode:UIImageRenderingModeAlwaysOriginal];
    if (!img) return NULL;
    UIGraphicsImageRendererFormat* f = [UIGraphicsImageRendererFormat preferredFormat];
    f.opaque = NO;
    UIImage* r = [[[UIGraphicsImageRenderer alloc] initWithSize:img.size format:f] imageWithActions:^(UIGraphicsImageRendererContext* c) { [img drawAtPoint:CGPointZero]; }];
    return CGImageRetain(r.CGImage); /* caller releases */
}

/* ================================================================ controls */
typedef NS_ENUM(int, DRKind) { DRRound, DRPill, DRTrigger, DRDpad, DRStick, DRGear, DRToggle };
@interface DRControl : NSObject
@property DRKind kind;
@property int button;
@property CGPoint anchor, offset; /* kit units: anchor (0,0) = bottom-left; offset/size on a 1080-high canvas, y up */
@property CGSize size;
@property BOOL right, mirrored, colored;
@property(copy) NSString *label, *icon;
@property(strong) UIColor* accent;
@property(strong) CALayer* layer;
@property(strong) CAShapeLayer *body, *rim, *knob, *knobRim;
@property(strong) NSArray<CAShapeLayer*>* arms; /* D-pad highlights: up, right, down, left */
@property CGPoint center, home;
@property CGFloat radius;
@property(weak) UITouch* touch;
@property int dx, dy;
@end
@implementation DRControl
@end

@class DRPanel;
@interface DRPadView : UIView
- (void)drPanelClosed;
- (void)setPanel:(DRPanel*)p;
- (void)drReleaseAll;
@end
@interface DRPanel : UIView
@property(weak) DRPadView* pad;
- (void)drShow;
- (void)drHide;
- (BOOL)drOpen;
- (void)drPadButton:(int)b down:(BOOL)d;
- (void)drPadDir:(int)x y:(int)y;
@end

@implementation DRPadView {
    NSMutableArray<DRControl*>* _ctls;
    CALayer* _root;
    CADisplayLink* _link;
    BOOL _shown;
    CGSize _builtFor;
    UIEdgeInsets _builtInsets;
    NSInteger _builtSize;
    BOOL _builtFloating;
    UIImpactFeedbackGenerator* _hap;
    DRPanel* _panel;
}
- (instancetype)initWithFrame:(CGRect)f
{
    self = [super initWithFrame:f];
    if (!self) return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    self.hidden = YES;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    _ctls = [NSMutableArray array];
    _root = [CALayer layer];
    [self.layer addSublayer:_root];
    _hap = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(drTick:)];
    _link.preferredFramesPerSecond = 30;
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(drSettingsChanged) name:DRChanged object:nil];
    return self;
}
- (void)setPanel:(DRPanel*)p { _panel = p; }
- (void)drSettingsChanged
{
    _builtFor = CGSizeZero;
    [self setNeedsLayout];
    [self drTick:nil];
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    [self drBuildIfNeeded];
}

/* ---- the kit's layout (TouchOverlay.Build) */
- (DRControl*)drAdd:(DRKind)k button:(int)b anchor:(CGPoint)a offset:(CGPoint)o size:(CGSize)s label:(NSString*)l icon:(NSString*)ic accent:(UIColor*)acc colored:(BOOL)col
{
    DRControl* c = [DRControl new];
    c.kind = k; c.button = b; c.anchor = a; c.offset = o; c.size = s; c.label = l; c.icon = ic;
    c.accent = acc ?: UIColor.whiteColor;
    c.colored = col;
    [_ctls addObject:c];
    return c;
}
- (void)drBuildIfNeeded
{
    CGSize S = self.bounds.size;
    NSInteger size = DRInt(@"PortTouchSize");
    BOOL fl = DRBool(@"PortFloatingStick");
    if (S.width <= 0 || (CGSizeEqualToSize(S, _builtFor) && UIEdgeInsetsEqualToEdgeInsets(self.safeAreaInsets, _builtInsets) && size == _builtSize && fl == _builtFloating))
        return;
    for (DRControl* c in _ctls)
        if (c.touch) [self drRelease:c];
    _builtFor = S; _builtInsets = self.safeAreaInsets; _builtSize = size; _builtFloating = fl;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    for (CALayer* l in [_root.sublayers copy]) [l removeFromSuperlayer];
    [_ctls removeAllObjects];
    CGPoint BL = { 0, 0 }, BR = { 1, 0 }, TL = { 0, 1 }, TR = { 1, 1 }, TC = { 0.5, 1 };
    [self drAdd:DRStick button:-1 anchor:BL offset:CGPointMake(290, 260) size:CGSizeMake(270, 270) label:nil icon:nil accent:nil colored:NO];
    [self drAdd:DRDpad button:-1 anchor:BL offset:CGPointMake(255, 620) size:CGSizeMake(230, 230) label:nil icon:nil accent:nil colored:NO];
    [self drAdd:DRStick button:-1 anchor:BR offset:CGPointMake(-575, 175) size:CGSizeMake(184, 184) label:nil icon:nil accent:nil colored:NO].right = YES;
    CGFloat fb = 124, sp = 108;
    CGPoint c0 = { -270, 290 };
    [self drAdd:DRRound button:0 anchor:BR offset:CGPointMake(c0.x, c0.y - sp) size:CGSizeMake(fb, fb) label:@"A" icon:nil accent:DRGreen() colored:YES];
    [self drAdd:DRRound button:1 anchor:BR offset:CGPointMake(c0.x + sp, c0.y) size:CGSizeMake(fb, fb) label:@"B" icon:nil accent:DRRed() colored:YES];
    [self drAdd:DRRound button:2 anchor:BR offset:CGPointMake(c0.x - sp, c0.y) size:CGSizeMake(fb, fb) label:@"X" icon:nil accent:DRBlue() colored:YES];
    [self drAdd:DRRound button:3 anchor:BR offset:CGPointMake(c0.x, c0.y + sp) size:CGSizeMake(fb, fb) label:@"Y" icon:nil accent:DRYellow() colored:YES];
    [self drAdd:DRPill button:4 anchor:TL offset:CGPointMake(165, -95) size:CGSizeMake(210, 84) label:@"LB" icon:nil accent:nil colored:NO];
    [self drAdd:DRTrigger button:6 anchor:TL offset:CGPointMake(395, -100) size:CGSizeMake(150, 112) label:@"LT" icon:nil accent:nil colored:NO];
    [self drAdd:DRPill button:5 anchor:TR offset:CGPointMake(-165, -95) size:CGSizeMake(210, 84) label:@"RB" icon:nil accent:nil colored:NO];
    [self drAdd:DRTrigger button:7 anchor:TR offset:CGPointMake(-395, -100) size:CGSizeMake(150, 112) label:@"RT" icon:nil accent:nil colored:NO].mirrored = YES;
    [self drAdd:DRRound button:10 anchor:TC offset:CGPointMake(-85, -72) size:CGSizeMake(80, 80) label:nil icon:@"rectangle.on.rectangle" accent:nil colored:NO];
    [self drAdd:DRRound button:11 anchor:TC offset:CGPointMake(85, -72) size:CGSizeMake(80, 80) label:nil icon:@"line.3.horizontal" accent:nil colored:NO];
    [self drAdd:DRRound button:8 anchor:BL offset:CGPointMake(510, 120) size:CGSizeMake(76, 76) label:@"L3" icon:nil accent:nil colored:NO];
    [self drAdd:DRRound button:9 anchor:BR offset:CGPointMake(-575, 355) size:CGSizeMake(76, 76) label:@"R3" icon:nil accent:nil colored:NO];
    [self drAdd:DRGear button:-1 anchor:TC offset:CGPointMake(255, -72) size:CGSizeMake(68, 68) label:nil icon:@"gearshape.fill" accent:nil colored:NO];
    [self drAdd:DRToggle button:-1 anchor:TC offset:CGPointMake(-255, -72) size:CGSizeMake(68, 68) label:nil icon:@"gamecontroller" accent:nil colored:NO];
    CGRect sa = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    CGFloat u = sa.size.height / 1080.0 * size / 100.0; /* kit: 1080-high canvas in the safe area, x Touch Size */
    for (DRControl* c in _ctls)
    {
        CGPoint p = CGPointMake(sa.origin.x + c.anchor.x * sa.size.width + c.offset.x * u, CGRectGetMaxY(sa) - c.anchor.y * sa.size.height - c.offset.y * u);
        CGSize s = CGSizeMake(c.size.width * u, c.size.height * u);
        c.center = c.home = p;
        c.radius = MAX(s.width, s.height) / 2;
        [self drDraw:c size:s];
    }
    [CATransaction commit];
    [self drApplyVisibility];
}
- (void)drDraw:(DRControl*)c size:(CGSize)s
{
    CALayer* L = [CALayer layer];
    L.bounds = CGRectMake(0, 0, s.width, s.height);
    L.position = c.center;
    c.layer = L;
    [_root addSublayer:L];
    CGRect b = L.bounds;
    CGFloat lw = MAX(1, MIN(s.width, s.height) * 0.022);
    UIBezierPath* shape;
    switch (c.kind)
    {
    case DRPill: shape = [UIBezierPath bezierPathWithRoundedRect:CGRectInset(b, lw, lw) cornerRadius:s.height * 0.38]; break;
    case DRTrigger:
    {
        CGFloat w = s.width, h = s.height, ins = lw;
        CGPoint p[4] = { { ins, ins }, { w - ins, h * 0.24 }, { w - ins, h - ins }, { ins, h - ins } };
        if (c.mirrored)
        {
            for (int i = 0; i < 4; i++) p[i].x = w - p[i].x;
            CGPoint t = p[0]; p[0] = p[1]; p[1] = t; t = p[2]; p[2] = p[3]; p[3] = t;
        }
        shape = DRRoundedPolygon(p, 4, h * 0.22);
        break;
    }
    case DRDpad:
    {
        CGFloat w = s.width, a = w * 0.34, lo = (w - a) / 2, hi = (w + a) / 2, ins = lw;
        CGPoint p[12] = { { lo, ins }, { hi, ins }, { hi, lo }, { w - ins, lo }, { w - ins, hi }, { hi, hi },
                          { hi, w - ins }, { lo, w - ins }, { lo, hi }, { ins, hi }, { ins, lo }, { lo, lo } };
        shape = DRRoundedPolygon(p, 12, a * 0.22);
        break;
    }
    default: shape = [UIBezierPath bezierPathWithOvalInRect:CGRectInset(b, lw, lw)]; break;
    }
    BOOL stick = c.kind == DRStick;
    CAShapeLayer* body = [CAShapeLayer layer];
    body.path = shape.CGPath;
    body.fillColor = DRBody(stick ? 0.42 : 0.66).CGColor;
    [L addSublayer:body];
    c.body = body;
    CAGradientLayer* shine = [CAGradientLayer layer]; /* light falls from above */
    shine.frame = b;
    shine.colors = @[ (id)[UIColor colorWithWhite:1 alpha:0.16].CGColor, (id)[UIColor colorWithWhite:1 alpha:0].CGColor ];
    shine.endPoint = CGPointMake(0.5, 0.55);
    CAShapeLayer* m = [CAShapeLayer layer];
    m.path = shape.CGPath;
    shine.mask = m;
    [L addSublayer:shine];
    CAShapeLayer* rim = [CAShapeLayer layer];
    rim.path = shape.CGPath;
    rim.fillColor = nil;
    rim.lineWidth = lw;
    rim.strokeColor = [UIColor colorWithWhite:1 alpha:0.34].CGColor;
    [L addSublayer:rim];
    c.rim = rim;
    if (c.kind == DRRound || stick)
    {
        CGFloat k = stick ? 0.30 : 0.09;
        CAShapeLayer* inner = [CAShapeLayer layer];
        inner.path = [UIBezierPath bezierPathWithOvalInRect:CGRectInset(b, s.width * k, s.height * k)].CGPath;
        inner.fillColor = nil;
        inner.lineWidth = MAX(0.75, lw * 0.6);
        inner.strokeColor = [UIColor colorWithWhite:1 alpha:stick ? 0.10 : 0.08].CGColor;
        [L addSublayer:inner];
    }
    if (c.kind == DRDpad)
    {
        CGFloat w = s.width, a = w * 0.34, lo = (w - a) / 2, hi = (w + a) / 2, t = a * 0.22;
        NSMutableArray* arms = [NSMutableArray array];
        CGRect armR[4] = { CGRectMake(lo, lw, a, lo - lw), CGRectMake(hi, lo, w - hi - lw, a), CGRectMake(lo, hi, a, w - hi - lw), CGRectMake(lw, lo, lo - lw, a) };
        CGPoint tip[4] = { { w / 2, lo * 0.38 }, { w - lo * 0.38, w / 2 }, { w / 2, w - lo * 0.38 }, { lo * 0.38, w / 2 } };
        for (int i = 0; i < 4; i++)
        {
            CAShapeLayer* h = [CAShapeLayer layer];
            h.path = [UIBezierPath bezierPathWithRoundedRect:armR[i] cornerRadius:a * 0.2].CGPath;
            h.fillColor = [UIColor colorWithWhite:1 alpha:0.24].CGColor;
            h.hidden = YES;
            [L addSublayer:h];
            [arms addObject:h];
            CGPoint q = tip[i];
            CGFloat dx = (i == 1) - (i == 3), dy = (i == 2) - (i == 0);
            UIBezierPath* tri = [UIBezierPath bezierPath];
            [tri moveToPoint:CGPointMake(q.x + dx * t, q.y + dy * t)];
            [tri addLineToPoint:CGPointMake(q.x - dx * t * 0.6 - dy * t, q.y - dy * t * 0.6 + dx * t)];
            [tri addLineToPoint:CGPointMake(q.x - dx * t * 0.6 + dy * t, q.y - dy * t * 0.6 - dx * t)];
            [tri closePath];
            CAShapeLayer* ar = [CAShapeLayer layer];
            ar.path = tri.CGPath;
            ar.fillColor = [UIColor colorWithWhite:1 alpha:0.78].CGColor;
            [L addSublayer:ar];
        }
        c.arms = arms;
    }
    if (stick)
    {
        CGFloat kd = s.width / 2 * 1.05;
        CGRect kr = CGRectMake((s.width - kd) / 2, (s.height - kd) / 2, kd, kd);
        CAShapeLayer* k = [CAShapeLayer layer];
        k.frame = b;
        k.path = [UIBezierPath bezierPathWithOvalInRect:kr].CGPath;
        k.fillColor = DRBody(0.86).CGColor;
        CAShapeLayer* kr2 = [CAShapeLayer layer];
        kr2.frame = b;
        kr2.fillColor = nil;
        kr2.lineWidth = MAX(1, kd * 0.03);
        UIBezierPath* rings = [UIBezierPath bezierPathWithOvalInRect:kr];
        [rings appendPath:[UIBezierPath bezierPathWithOvalInRect:CGRectInset(kr, kd * 0.17, kd * 0.17)]];
        kr2.path = rings.CGPath;
        kr2.strokeColor = [UIColor colorWithWhite:1 alpha:0.40].CGColor;
        [L addSublayer:k];
        [L addSublayer:kr2];
        c.knob = k;
        c.knobRim = kr2;
    }
    if (c.label)
    {
        CGFloat fs = MIN(s.width, s.height) * (c.kind == DRRound && c.label.length == 1 ? 0.44 : 0.34);
        CATextLayer* t = [CATextLayer layer];
        t.contentsScale = UIScreen.mainScreen.scale;
        t.alignmentMode = kCAAlignmentCenter;
        UIFont* f = [UIFont systemFontOfSize:fs weight:UIFontWeightHeavy];
        t.font = (__bridge CFTypeRef)f;
        t.fontSize = fs;
        t.string = c.label;
        t.foregroundColor = (c.colored ? c.accent : DRNeutral()).CGColor;
        CGFloat dy = c.kind == DRTrigger ? s.height * 0.06 : 0;
        t.frame = CGRectMake(0, (s.height - f.lineHeight) / 2 + dy, s.width, f.lineHeight);
        t.shadowOpacity = 0.6;
        t.shadowRadius = 0;
        t.shadowOffset = CGSizeMake(0, fs * 0.04);
        t.shadowColor = UIColor.blackColor.CGColor;
        [L addSublayer:t];
    }
    if (c.icon)
    {
        CGFloat pt = s.width * 0.34;
        CGImageRef img = DRSymbol(c.icon, pt);
        if (img)
        {
            CALayer* i = [CALayer layer];
            CGFloat iw = CGImageGetWidth(img) / UIScreen.mainScreen.scale, ih = CGImageGetHeight(img) / UIScreen.mainScreen.scale;
            i.contents = (__bridge id)img;
            i.contentsScale = UIScreen.mainScreen.scale;
            i.frame = CGRectMake((s.width - iw) / 2, (s.height - ih) / 2, iw, ih);
            i.opacity = 0.92;
            [L addSublayer:i];
            CGImageRelease(img);
        }
    }
    L.shadowColor = c.accent.CGColor;
    L.shadowOpacity = 0;
    L.shadowRadius = MIN(s.width, s.height) * 0.16;
    L.shadowOffset = CGSizeZero;
    L.shadowPath = shape.CGPath;
}

/* ---- visibility: once the game draws; without a real controller (then only the gear and the toggle stay) */
- (BOOL)drControlsOn { return DRBool(@"PortTouchControls"); }
- (BOOL)drIsUtility:(DRControl*)c { return c.kind == DRGear || c.kind == DRToggle; }
- (void)drApplyVisibility
{
    BOOL padWanted = DRPadWanted(), on = [self drControlsOn];
    [CATransaction begin];
    [CATransaction setAnimationDuration:0.2];
    _root.opacity = (float)(DRInt(@"PortTouchOpacity") / 100.0);
    for (DRControl* c in _ctls)
    {
        BOOL vis = c.kind == DRGear ? YES : c.kind == DRToggle ? padWanted : padWanted && on;
        c.layer.hidden = !vis;
        c.layer.opacity = [self drIsUtility:c] && (!on || !padWanted) ? 0.55 : 1;
    }
    [CATransaction commit];
}
- (void)drTick:(CADisplayLink*)l
{
    BOOL want = DRGameDrawing();
    if (want != _shown)
    {
        _shown = want;
        self.hidden = !want;
        if (want)
        {
            DRLog([NSString stringWithFormat:@"touch controls shown (size %ld%%, opacity %ld%%)", (long)DRInt(@"PortTouchSize"), (long)DRInt(@"PortTouchOpacity")]);
            [_hap prepare];
        }
    }
    if (!want) return;
    [self drBuildIfNeeded];
    static int lastPad = -1, lastOn = -1;
    static NSInteger lastOp = -1;
    BOOL pad = DRPadWanted(), on = [self drControlsOn];
    NSInteger op = DRInt(@"PortTouchOpacity");
    if (pad != lastPad || on != lastOn || op != lastOp)
    {
        lastPad = pad; lastOn = on; lastOp = op;
        if (!pad || !on)
            for (DRControl* c in _ctls)
                if (c.touch && ![self drIsUtility:c]) [self drRelease:c];
        [self drApplyVisibility];
    }
}

/* ---- touches */
- (BOOL)drActive:(DRControl*)c { return !c.layer.hidden; }
- (DRControl*)drControlAt:(CGPoint)p
{
    DRControl* best = nil;
    CGFloat bestD = CGFLOAT_MAX;
    for (DRControl* c in _ctls)
    {
        if (![self drActive:c]) continue;
        CGFloat r = c.radius * 1.18, d = hypot(p.x - c.home.x, p.y - c.home.y); /* kit HitScale 1.18 */
        if (d <= r && d < bestD) { best = c; bestD = d; }
    }
    return best;
}
- (DRControl*)drLeftStick
{
    for (DRControl* c in _ctls)
        if (c.kind == DRStick && !c.right) return c;
    return nil;
}
- (BOOL)drInFloatingZone:(CGPoint)p
{
    if (!DRBool(@"PortFloatingStick")) return NO;
    DRControl* ls = [self drLeftStick];
    if (!ls || ![self drActive:ls]) return NO;
    CGRect sa = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets); /* kit: 0-40 % wide, bottom 58 % */
    return p.x >= sa.origin.x && p.x <= sa.origin.x + sa.size.width * 0.40 && p.y >= CGRectGetMaxY(sa) - sa.size.height * 0.58;
}
- (UIView*)hitTest:(CGPoint)p withEvent:(UIEvent*)e
{
    if (self.hidden) return nil;
    if ([self drControlAt:p] || [self drInFloatingZone:p]) return self;
    return nil; /* the rest of the screen: the panel or the game */
}
- (void)drHaptic
{
    if (DRBool(@"PortHaptics")) { [_hap impactOccurredWithIntensity:0.75]; [_hap prepare]; }
}
- (void)drPressed:(DRControl*)c on:(BOOL)on
{
    [CATransaction begin];
    [CATransaction setAnimationDuration:on ? 0.06 : 0.14];
    c.layer.transform = on ? CATransform3DMakeScale(0.92, 0.92, 1) : CATransform3DIdentity;
    c.layer.shadowOpacity = on && c.colored ? 0.85 : 0;
    UIColor* pressFill = c.colored ? [c.accent colorWithAlphaComponent:0.55] : [UIColor colorWithWhite:0.32 alpha:0.75];
    if (c.kind != DRStick && c.kind != DRDpad) c.body.fillColor = (on ? pressFill : DRBody(0.66)).CGColor;
    [CATransaction commit];
}
- (void)drButton:(DRControl*)c down:(BOOL)d
{
    if (_panel.drOpen) { [_panel drPadButton:c.button down:d]; return; } /* panel open: the pad navigates it */
    DRPadButton(c.button, d);
}
- (void)drStick:(DRControl*)c to:(CGPoint)p
{
    CGFloat R = c.radius, dx = p.x - c.center.x, dy = p.y - c.center.y, len = hypot(dx, dy), k = len > R ? R / len : 1;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    c.knob.position = CGPointMake(c.layer.bounds.size.width / 2 + dx * k, c.layer.bounds.size.height / 2 + dy * k);
    c.knobRim.position = c.knob.position;
    [CATransaction commit];
    float x = (float)(dx * k / R), y = (float)(-dy * k / R);
    if (_panel.drOpen) { if (!c.right) [_panel drPadDir:(x > 0.5) - (x < -0.5) y:(y > 0.5) - (y < -0.5)]; return; }
    DRPadStick(c.right, x, y);
}
- (void)drDpad:(DRControl*)c at:(CGPoint)p
{
    CGFloat dx = p.x - c.center.x, dy = p.y - c.center.y, R = c.radius, dead = R * 0.22;
    int x = 0, y = 0;
    if (hypot(dx, dy) > dead) /* 8-way: diagonals within 22.5 degrees of 45 */
    {
        double a = atan2(-dy, dx);
        int oct = (int)lround(a / (M_PI / 4));
        oct = (oct + 8) % 8;
        static const int ox[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }, oy[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
        x = ox[oct]; y = oy[oct];
    }
    if (x == c.dx && y == c.dy) return;
    if (x || y) [self drHaptic];
    c.dx = x; c.dy = y;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    c.arms[0].hidden = y <= 0; c.arms[1].hidden = x <= 0; c.arms[2].hidden = y >= 0; c.arms[3].hidden = x >= 0;
    [CATransaction commit];
    if (_panel.drOpen) { [_panel drPadDir:x y:y]; return; }
    DRPadDpad(x, y);
}
- (void)touchesBegan:(NSSet<UITouch*>*)ts withEvent:(UIEvent*)e
{
    for (UITouch* t in ts)
    {
        CGPoint p = [t locationInView:self];
        DRControl* c = [self drControlAt:p];
        if (!c && [self drInFloatingZone:p]) /* floating left stick: it comes to the thumb */
        {
            c = [self drLeftStick];
            if (c && !c.touch)
            {
                [CATransaction begin];
                [CATransaction setDisableActions:YES];
                c.center = p;
                c.layer.position = p;
                [CATransaction commit];
            }
        }
        if (!c || c.touch) continue;
        c.touch = t;
        switch (c.kind)
        {
        case DRGear:
        case DRToggle: [self drHaptic]; [self drPressed:c on:YES]; break;
        case DRDpad: [self drDpad:c at:p]; break;
        case DRStick: [self drStick:c to:p]; break;
        default: [self drHaptic]; [self drPressed:c on:YES]; [self drButton:c down:YES]; break;
        }
    }
}
- (void)touchesMoved:(NSSet<UITouch*>*)ts withEvent:(UIEvent*)e
{
    for (UITouch* t in ts)
    {
        CGPoint p = [t locationInView:self];
        for (DRControl* c in _ctls)
        {
            if (c.touch != t) continue;
            if (c.kind == DRStick) [self drStick:c to:p];
            else if (c.kind == DRDpad) [self drDpad:c at:p];
            else if (hypot(p.x - c.home.x, p.y - c.home.y) > c.radius * 1.7) [self drRelease:c]; /* slid off: let go */
        }
    }
}
- (void)drRelease:(DRControl*)c
{
    DRKind k = c.kind;
    c.touch = nil;
    if (k == DRStick)
    {
        [self drStick:c to:c.center];
        if (!c.right && DRBool(@"PortFloatingStick"))
        {
            [CATransaction begin];
            [CATransaction setDisableActions:YES];
            c.center = c.home;
            c.layer.position = c.home;
            [CATransaction commit];
        }
    }
    else if (k == DRDpad) [self drDpad:c at:c.center];
    else
    {
        [self drPressed:c on:NO];
        if (k != DRGear && k != DRToggle) [self drButton:c down:NO];
    }
}
- (void)touchesEnded:(NSSet<UITouch*>*)ts withEvent:(UIEvent*)e
{
    for (UITouch* t in ts)
        for (DRControl* c in [_ctls copy])
        {
            if (c.touch != t) continue;
            BOOL tap = hypot([t locationInView:self].x - c.home.x, [t locationInView:self].y - c.home.y) <= c.radius * 1.7;
            DRKind k = c.kind;
            [self drRelease:c];
            if (tap && k == DRGear) { if (_panel.drOpen) [_panel drHide]; else [_panel drShow]; }
            if (tap && k == DRToggle) /* the show / hide button: the Touch Controls setting */
            {
                [DRDefaults() setBool:![self drControlsOn] forKey:@"PortTouchControls"];
                DRLog([NSString stringWithFormat:@"touch controls %@ (pad button)", [self drControlsOn] ? @"on" : @"off"]);
                [NSNotificationCenter.defaultCenter postNotificationName:DRChanged object:nil];
            }
        }
}
- (void)touchesCancelled:(NSSet<UITouch*>*)ts withEvent:(UIEvent*)e
{
    for (UITouch* t in ts)
        for (DRControl* c in [_ctls copy])
            if (c.touch == t) [self drRelease:c];
}
- (void)drPanelClosed
{
    for (DRControl* c in _ctls)
        if (c.touch && ![self drIsUtility:c]) [self drRelease:c];
}
- (void)drReleaseAll
{
    for (DRControl* c in [_ctls copy])
        if (c.touch) [self drRelease:c];
    DRPadRelease();
}
@end

/* ================================================================ the Settings panel (the kit's PortSettingsPanel) */
@interface DRRow : NSObject
@property(copy) NSString* title;
@property(copy) NSArray<NSString*>* values;
@property(copy) NSInteger (^current)(void);
@property(copy) void (^choose)(NSInteger);
@property(strong) UIView* view;
@property(strong) UILabel *label, *value;
@property(strong) CAShapeLayer* bg;
@end
@implementation DRRow
@end

static NSArray<NSString*>* DRPercent(int from, int to, int step)
{
    NSMutableArray* a = [NSMutableArray array];
    for (int v = from; v <= to; v += step) [a addObject:[NSString stringWithFormat:@"%d%%", v]];
    return a;
}
static NSInteger DRResolutionIndex()
{
    const float s = Config::ResolutionScale;
    return s >= 1.75f ? 2 : s >= 1.25f ? 1 : 0;
}
static void DRSetResolution(NSInteger i) /* 720p, 1080p, 1440p (no 2160p on iPhone / iPad: Config::kMaxResolutionScale) */
{
    static const float scales[] = { 1.0f, 1.5f, 2.0f };
    Config::ResolutionScale = scales[MAX(0, MIN(2, i))];
}

@implementation DRPanel {
    UIView* _card;
    UIScrollView* _scroll;
    NSArray<DRRow*>* _rows;
    NSInteger _sel;
    BOOL _open;
    CADisplayLink* _link;
    int _heldDir;
    double _nextRepeat;
    BOOL _hadA, _hadB, _hadMenu;
    BOOL _waitRelease; /* closed: the game gets the controller back once everything on it is let go */
    UISelectionFeedbackGenerator* _tick;
}
- (instancetype)initWithFrame:(CGRect)f
{
    self = [super initWithFrame:f];
    if (!self) return nil;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
    self.hidden = YES;
    self.alpha = 0;
    _tick = [UISelectionFeedbackGenerator new];
    _card = [UIView new];
    _card.backgroundColor = DRBody(0.94);
    _card.layer.cornerRadius = 26;
    _card.layer.cornerCurve = kCACornerCurveContinuous;
    _card.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.35].CGColor;
    _card.layer.borderWidth = 1;
    [self addSubview:_card];
    UIView* accent = [UIView new];
    accent.backgroundColor = DRBlood();
    accent.tag = 3;
    [_card addSubview:accent];
    UILabel* title = [UILabel new];
    title.text = @"Settings";
    title.textColor = DRNeutral();
    title.textAlignment = NSTextAlignmentCenter;
    title.font = [UIFont systemFontOfSize:21 weight:UIFontWeightHeavy];
    title.tag = 1;
    [_card addSubview:title];
    UILabel* hint = [UILabel new];
    hint.text = @"Tap a setting's left / right half to change it    D-pad ▲▼ choose  ◀▶ change    B or three fingers: close";
    hint.textColor = [UIColor colorWithWhite:1 alpha:0.6];
    hint.font = [UIFont systemFontOfSize:12 weight:UIFontWeightMedium];
    hint.textAlignment = NSTextAlignmentCenter;
    hint.adjustsFontSizeToFitWidth = YES;
    hint.minimumScaleFactor = 0.7;
    hint.tag = 2;
    [_card addSubview:hint];
    _scroll = [UIScrollView new];
    _scroll.showsVerticalScrollIndicator = NO;
    _scroll.alwaysBounceVertical = YES;
    [_card addSubview:_scroll];
    [self drBuildRows];
    UITapGestureRecognizer* tap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(drTap:)];
    tap.cancelsTouchesInView = NO;
    [self addGestureRecognizer:tap];
    return self;
}
- (BOOL)drOpen { return _open; }
- (DRRow*)row:(NSString*)t values:(NSArray*)v get:(NSInteger (^)(void))g set:(void (^)(NSInteger))s
{
    DRRow* r = [DRRow new];
    r.title = t; r.values = v; r.current = g; r.choose = s;
    return r;
}
- (void)drBuildRows
{
    void (^touch)(void) = ^{ [NSNotificationCenter.defaultCenter postNotificationName:DRChanged object:nil]; };
    NSArray* offOn = @[ @"Off", @"On" ];
    _rows = @[
        [self row:@"Frame Rate" values:@[ @"30 FPS (console)", @"60 FPS" ] get:^{ return (NSInteger)(Config::FrameRate == 60); } set:^(NSInteger i) { Config::FrameRate = i == 1 ? 60 : 30; }],
        [self row:@"Resolution" values:@[ @"720p (console)", @"1080p", @"1440p" ] get:^{ return DRResolutionIndex(); } set:^(NSInteger i) { DRSetResolution(i); }],
        [self row:@"Screen Scaling" values:@[ @"Fit", @"Fill", @"Fill 1.943:1" ] get:^{ return (NSInteger)Config::ScreenScaling.load(); } set:^(NSInteger i) { Config::ScreenScaling = int(MAX(0, MIN(2, i))); }],
        [self row:@"Anti-Aliasing" values:@[ @"Off", @"4X MSAA" ] get:^{ return (NSInteger)Config::MSAA.load(); } set:^(NSInteger i) { Config::MSAA = i == 1; }],
        [self row:@"FXAA" values:offOn get:^{ return (NSInteger)Config::FXAA.load(); } set:^(NSInteger i) { Config::FXAA = i == 1; }],
        [self row:@"Motion Blur" values:offOn get:^{ return (NSInteger)Config::MotionBlur.load(); } set:^(NSInteger i) { Config::MotionBlur = i == 1; }],
        [self row:@"V-Sync" values:offOn get:^{ return (NSInteger)Config::VSync.load(); } set:^(NSInteger i) { Config::VSync = i == 1; }],
        [self row:@"Show FPS" values:offOn get:^{ return (NSInteger)Config::ShowFps.load(); } set:^(NSInteger i) { Config::ShowFps = i == 1; }],
        [self row:@"Touch Controls" values:offOn get:^{ return (NSInteger)DRBool(@"PortTouchControls"); } set:^(NSInteger i) { [DRDefaults() setBool:i == 1 forKey:@"PortTouchControls"]; touch(); }],
        [self row:@"Touch Opacity" values:DRPercent(20, 100, 5) get:^{ return (DRInt(@"PortTouchOpacity") - 20) / 5; } set:^(NSInteger i) { [DRDefaults() setInteger:20 + i * 5 forKey:@"PortTouchOpacity"]; touch(); }],
        [self row:@"Touch Size" values:DRPercent(70, 150, 10) get:^{ return (DRInt(@"PortTouchSize") - 70) / 10; } set:^(NSInteger i) { [DRDefaults() setInteger:70 + i * 10 forKey:@"PortTouchSize"]; touch(); }],
        [self row:@"Left Stick" values:@[ @"Fixed", @"Floating" ] get:^{ return (NSInteger)DRBool(@"PortFloatingStick"); } set:^(NSInteger i) { [DRDefaults() setBool:i == 1 forKey:@"PortFloatingStick"]; touch(); }],
        [self row:@"Vibration" values:offOn get:^{ return (NSInteger)DRBool(@"PortHaptics"); } set:^(NSInteger i) { [DRDefaults() setBool:i == 1 forKey:@"PortHaptics"]; touch(); }],
    ];
    for (DRRow* r in _rows)
    {
        r.view = [UIView new];
        [_scroll addSubview:r.view];
        r.bg = [CAShapeLayer layer];
        r.bg.lineWidth = 1;
        [r.view.layer addSublayer:r.bg];
        r.label = [UILabel new];
        r.label.text = r.title;
        r.label.font = [UIFont systemFontOfSize:16 weight:UIFontWeightMedium];
        [r.view addSubview:r.label];
        r.value = [UILabel new];
        r.value.textAlignment = NSTextAlignmentCenter;
        r.value.font = [UIFont monospacedDigitSystemFontOfSize:16 weight:UIFontWeightSemibold];
        r.value.textColor = UIColor.whiteColor;
        r.value.adjustsFontSizeToFitWidth = YES;
        r.value.minimumScaleFactor = 0.7;
        [r.view addSubview:r.value];
    }
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    CGRect sa = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    CGFloat rowH = 44, gap = 8, cw = MIN(580, sa.size.width - 40), head = 54, foot = 36;
    CGFloat ch = MIN(sa.size.height - 24, head + foot + _rows.count * (rowH + gap));
    _card.frame = CGRectMake(CGRectGetMidX(sa) - cw / 2, CGRectGetMidY(sa) - ch / 2, cw, ch);
    [_card viewWithTag:3].frame = CGRectMake(cw * 0.35, 0, cw * 0.30, 4);
    [_card viewWithTag:1].frame = CGRectMake(0, 12, cw, 30);
    [_card viewWithTag:2].frame = CGRectMake(12, ch - foot + 4, cw - 24, 24);
    _scroll.frame = CGRectMake(16, head, cw - 32, ch - head - foot);
    _scroll.contentSize = CGSizeMake(cw - 32, _rows.count * (rowH + gap) - gap);
    for (NSUInteger i = 0; i < _rows.count; i++)
    {
        DRRow* r = _rows[i];
        CGFloat w = cw - 32;
        r.view.frame = CGRectMake(0, i * (rowH + gap), w, rowH);
        r.bg.path = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(0.5, 0.5, w - 1, rowH - 1) cornerRadius:rowH * 0.38].CGPath;
        r.label.frame = CGRectMake(16, 0, w * 0.42, rowH);
        r.value.frame = CGRectMake(w * 0.42, 0, w * 0.58 - 12, rowH);
    }
    [self drPaint];
}
- (void)drPaint
{
    for (NSUInteger i = 0; i < _rows.count; i++)
    {
        DRRow* r = _rows[i];
        BOOL sel = (NSInteger)i == _sel;
        NSInteger v = MAX(0, MIN((NSInteger)r.values.count - 1, r.current()));
        r.value.text = [NSString stringWithFormat:@"◀   %@   ▶", r.values[v]];
        r.bg.fillColor = (sel ? [UIColor colorWithRed:0.36 green:0.14 blue:0.13 alpha:0.95] : [UIColor colorWithWhite:1 alpha:0.06]).CGColor;
        r.bg.strokeColor = (sel ? [DRBlood() colorWithAlphaComponent:0.95] : [UIColor colorWithWhite:1 alpha:0.10]).CGColor;
        r.label.textColor = sel ? UIColor.whiteColor : [UIColor colorWithWhite:1 alpha:0.82];
    }
}
- (void)drSelect:(NSInteger)i
{
    i = MAX(0, MIN((NSInteger)_rows.count - 1, i));
    if (i == _sel) return;
    _sel = i;
    [_tick selectionChanged];
    [self drPaint];
    CGRect f = _rows[i].view.frame;
    [_scroll scrollRectToVisible:CGRectInset(f, 0, -10) animated:YES];
}
- (void)drChange:(NSInteger)d
{
    DRRow* r = _rows[_sel];
    NSInteger n = r.values.count, v = MAX(0, MIN(n - 1, r.current()));
    NSInteger nv = ((v + d) % n + n) % n;
    r.choose(nv);
    [_tick selectionChanged];
    [self drPaint];
    DRLog([NSString stringWithFormat:@"Settings: %@ -> %@", r.title, r.values[nv]]);
}
- (void)drShow
{
    if (_open) return;
    _open = YES;
    _waitRelease = NO;
    [self drPaint];
    self.hidden = NO;
    [self.superview bringSubviewToFront:self];
    [self.superview bringSubviewToFront:(UIView*)self.pad]; /* the pad stays on top: its D-pad and buttons drive the panel */
    Input::SetBlocked(true);
    _card.transform = CGAffineTransformMakeScale(0.96, 0.96);
    [UIView animateWithDuration:0.18 delay:0 options:UIViewAnimationOptionCurveEaseOut animations:^{
        self.alpha = 1;
        self->_card.transform = CGAffineTransformIdentity;
    } completion:nil];
    GCExtendedGamepad* g = DRRealGamepad();
    _hadA = g.buttonA.isPressed; _hadB = g.buttonB.isPressed; _hadMenu = g.buttonMenu.isPressed;
    _heldDir = 0;
    if (!_link)
    {
        _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(drPoll:)];
        [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    _link.paused = NO;
    DRLog(@"Settings opened");
}
- (void)drHide
{
    if (!_open) return;
    _open = NO;
    _waitRelease = YES; /* B, Start or A still held on a controller must not reach the game */
    [UIView animateWithDuration:0.16 delay:0 options:UIViewAnimationOptionCurveEaseIn animations:^{
        self.alpha = 0;
        self->_card.transform = CGAffineTransformMakeScale(0.97, 0.97);
    } completion:^(BOOL done) {
        if (!self->_open) self.hidden = YES;
    }];
    [self.pad drPanelClosed];
    Config::Save();
    DRLog(@"Settings closed, saved");
}
/* touches: a row = select it; its value's left / right half = previous / next; outside the card = close */
- (void)drTap:(UITapGestureRecognizer*)g
{
    if (!_open) return;
    CGPoint p = [g locationInView:_card];
    if (![_card pointInside:p withEvent:nil]) { [self drHide]; return; }
    CGPoint q = [g locationInView:_scroll];
    for (NSUInteger i = 0; i < _rows.count; i++)
    {
        DRRow* r = _rows[i];
        if (!CGRectContainsPoint(r.view.frame, q)) continue;
        [self drSelect:i];
        CGPoint v = [g locationInView:r.value];
        if ([r.value pointInside:v withEvent:nil]) [self drChange:v.x < r.value.bounds.size.width / 2 ? -1 : 1];
        return;
    }
}
/* the on-screen pad while the panel is open */
- (void)drPadButton:(int)b down:(BOOL)d
{
    if (!d) return;
    if (b == 1 || b == 11) [self drHide];
    else if (b == 0) [self drChange:1];
}
- (void)drPadDir:(int)x y:(int)y { [self drDir:(y > 0 ? 1 : y < 0 ? 2 : x < 0 ? 3 : x > 0 ? 4 : 0) now:CACurrentMediaTime()]; }
- (void)drDir:(int)dir now:(double)now /* 1 up 2 down 3 left 4 right; repeat while held */
{
    if (dir != _heldDir)
    {
        _heldDir = dir;
        _nextRepeat = now + 0.4;
        if (dir) [self drStep:dir];
        return;
    }
    if (dir && now >= _nextRepeat) { _nextRepeat = now + 0.11; [self drStep:dir]; }
}
- (void)drStep:(int)dir
{
    if (dir == 1) [self drSelect:_sel - 1];
    else if (dir == 2) [self drSelect:_sel + 1];
    else if (dir == 3) [self drChange:-1];
    else if (dir == 4) [self drChange:1];
}
/* a real controller, polled at display rate (the game does not get it while the panel is open) */
- (void)drPoll:(CADisplayLink*)l
{
    if (_waitRelease)
    {
        const Input::Pad raw = Input::GetRawPad();
        if (raw.buttons == 0 && raw.leftTrigger < 30 && raw.rightTrigger < 30 && std::abs(raw.thumbLX) < 8000 && std::abs(raw.thumbLY) < 8000)
        {
            _waitRelease = NO;
            _link.paused = YES;
            Input::SetBlocked(false);
        }
        return;
    }
    GCExtendedGamepad* g = DRRealGamepad();
    if (!g) return;
    double now = CACurrentMediaTime();
    float x = g.dpad.xAxis.value + g.leftThumbstick.xAxis.value, y = g.dpad.yAxis.value + g.leftThumbstick.yAxis.value;
    int dir = y > 0.5 ? 1 : y < -0.5 ? 2 : x < -0.5 ? 3 : x > 0.5 ? 4 : 0;
    [self drDir:dir now:now];
    BOOL a = g.buttonA.isPressed, b = g.buttonB.isPressed, m = g.buttonMenu.isPressed;
    if (a && !_hadA) [self drChange:1];
    if ((b && !_hadB) || (m && !_hadMenu)) [self drHide];
    _hadA = a; _hadB = b; _hadMenu = m;
}
@end

/* ================================================================ the live log and the frame counter */
@interface DRLogView : UIView
- (instancetype)initWithFrame:(CGRect)f fps:(UILabel*)fps;
- (void)drToggle;
@end
@implementation DRLogView {
    UILabel* _title;
    UITextView* _text;
    CADisplayLink* _link;
    BOOL _userOpen, _autoOpen;
    UILabel* _fps;
}
- (instancetype)initWithFrame:(CGRect)f fps:(UILabel*)fps
{
    self = [super initWithFrame:f];
    if (!self) return nil;
    self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    self.backgroundColor = [UIColor colorWithWhite:0.02 alpha:0.82];
    self.hidden = YES;
    self.userInteractionEnabled = YES;
    _fps = fps;
    _title = [UILabel new];
    _title.textColor = [UIColor colorWithRed:1 green:0.72 blue:0.35 alpha:1];
    _title.font = [UIFont systemFontOfSize:14 weight:UIFontWeightBold];
    _title.adjustsFontSizeToFitWidth = YES;
    _title.minimumScaleFactor = 0.7;
    [self addSubview:_title];
    _text = [UITextView new];
    _text.editable = NO;
    _text.selectable = YES;
    _text.backgroundColor = UIColor.clearColor;
    _text.textColor = [UIColor colorWithWhite:0.86 alpha:1];
    _text.font = [UIFont monospacedSystemFontOfSize:11 weight:UIFontWeightRegular];
    [self addSubview:_text];
    _link = [CADisplayLink displayLinkWithTarget:self selector:@selector(drTick:)];
    _link.preferredFramesPerSecond = 4;
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    return self;
}
- (void)layoutSubviews
{
    [super layoutSubviews];
    CGRect sa = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    _title.frame = CGRectMake(sa.origin.x + 16, sa.origin.y + 8, sa.size.width - 32, 22);
    _text.frame = CGRectMake(sa.origin.x + 10, sa.origin.y + 34, sa.size.width - 20, sa.size.height - 40);
}
- (void)drToggle
{
    _userOpen = !_userOpen;
    _autoOpen = NO;
    if (_userOpen) [self.superview bringSubviewToFront:self]; /* above the controls; Settings still opens above it */
    DRLog(_userOpen ? @"live log shown" : @"live log hidden");
    [self drRefresh:YES];
}
- (void)drRefresh:(BOOL)scroll
{
    const BOOL visible = _userOpen || _autoOpen;
    self.hidden = !visible;
    if (!visible) return;
    const double waited = Video::SecondsSinceGameFrame();
    if (_userOpen)
        _title.text = @"Live log. Tap with four fingers to close it. Full log: Files > Dead Rising > DRRecomp.log";
    else
        _title.text = [NSString stringWithFormat:Video::GameFrames() == 0 ? @"The game has not shown a picture yet: %.0f s. The log so far:" : @"No new picture from the game for %.0f s. The log so far:", waited];
    std::string joined;
    for (const std::string& line : os::logger::RecentLines(200))
        joined += line + "\n";
    NSString* text = [NSString stringWithUTF8String:joined.c_str()] ?: @"";
    const BOOL atBottom = _text.contentOffset.y + _text.bounds.size.height >= _text.contentSize.height - 40;
    if (![_text.text isEqualToString:text])
    {
        _text.text = text;
        if (scroll || atBottom)
            [_text scrollRangeToVisible:NSMakeRange(text.length > 0 ? text.length - 1 : 0, 0)];
    }
}
- (void)drCheckMemory
{
    // Below 600 MB left, every 100 MB less is logged (the last line before the system ends the app says how close it was).
    static int64_t lastStep = INT64_MAX;
    const int64_t left = MemoryInfo::AvailableMB();
    if (left < 0) return;
    const int64_t step = left / 100;
    if (left < 600 && step < lastStep)
    {
        lastStep = step;
        LOGFN_WARNING("[memory] low: {}", MemoryInfo::Describe());
    }
    else if (lastStep != INT64_MAX && step > lastStep + 1)
        lastStep = step; // memory came back: report the next drop again
}
- (void)drTick:(CADisplayLink*)l
{
    // Only on a four-finger tap (the user asked, 2026-11-25: it no longer opens by itself while the game starts).
    [self drCheckMemory];
    [self drRefresh:NO];
    // the frame counter (Show FPS)
    _fps.hidden = !Config::ShowFps || (_userOpen || _autoOpen);
    if (!_fps.hidden)
    {
        _fps.text = [NSString stringWithFormat:@"  %.0f FPS  ", Video::Fps()];
        [_fps sizeToFit];
        CGRect sa = UIEdgeInsetsInsetRect(self.superview.bounds, self.superview.safeAreaInsets);
        _fps.frame = CGRectMake(sa.origin.x + 10, sa.origin.y + 8, _fps.bounds.size.width, 26);
    }
}
@end

/* ================================================================ the overlay root */
@interface DROverlay : NSObject
@property(strong) DRPadView* pad;
@property(strong) DRPanel* panel;
@property(strong) DRLogView* log;
@end
@implementation DROverlay
- (void)drThreeFingers:(UITapGestureRecognizer*)g
{
    if (g.state != UIGestureRecognizerStateRecognized) return;
    [self.pad drReleaseAll];
    if (self.panel.drOpen) [self.panel drHide]; else [self.panel drShow];
}
- (void)drFourFingers:(UITapGestureRecognizer*)g
{
    if (g.state != UIGestureRecognizerStateRecognized) return;
    [self.pad drReleaseAll];
    [self.log drToggle];
}
@end

static DROverlay* g_overlay = nil;

void Overlay::Create(SDL_Window* window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_UIKIT)
    {
        LOGN_ERROR("[overlay] no UIKit window: no on-screen controls");
        return;
    }
    UIWindow* uiWindow = info.info.uikit.window;
    UIView* host = uiWindow.rootViewController.view ?: uiWindow;
    DRRegisterDefaults();

    g_overlay = [DROverlay new];
    UILabel* fps = [UILabel new];
    fps.textColor = [UIColor colorWithRed:1 green:0.92 blue:0.31 alpha:1];
    fps.backgroundColor = [UIColor colorWithWhite:0 alpha:0.55];
    fps.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightBold];
    fps.layer.cornerRadius = 6;
    fps.clipsToBounds = YES;
    fps.hidden = YES;
    fps.userInteractionEnabled = NO;

    g_overlay.panel = [[DRPanel alloc] initWithFrame:host.bounds];
    g_overlay.pad = [[DRPadView alloc] initWithFrame:host.bounds];
    g_overlay.log = [[DRLogView alloc] initWithFrame:host.bounds fps:fps];
    [host addSubview:g_overlay.panel];
    [host addSubview:g_overlay.pad]; /* above the panel: its D-pad and buttons drive it */
    [host addSubview:g_overlay.log];
    [host addSubview:fps];
    g_overlay.panel.pad = g_overlay.pad;
    [g_overlay.pad setPanel:g_overlay.panel];

    // Three fingers: Settings; four: the live log. On the window, so they work over the game and the controls.
    UITapGestureRecognizer* three = [[UITapGestureRecognizer alloc] initWithTarget:g_overlay action:@selector(drThreeFingers:)];
    three.numberOfTouchesRequired = 3;
    three.cancelsTouchesInView = NO;
    UITapGestureRecognizer* four = [[UITapGestureRecognizer alloc] initWithTarget:g_overlay action:@selector(drFourFingers:)];
    four.numberOfTouchesRequired = 4;
    four.cancelsTouchesInView = NO;
    [three requireGestureRecognizerToFail:four];
    [uiWindow addGestureRecognizer:three];
    [uiWindow addGestureRecognizer:four];

    // Leaving the screen (home, app switcher, Control Center, a call): no GPU work from the background, the game
    // waits (gpu/video.cpp); nothing stays pressed.
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationWillResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
        [g_overlay.pad drReleaseAll];
        Video::SetAppActive(false);
    }];
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
        Video::SetAppActive(true);
    }];
    // iOS ends an app over its memory limit without a message: its warnings, and how close the app is, go to the log.
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidReceiveMemoryWarningNotification object:nil queue:nil usingBlock:^(NSNotification* n) {
        LOGFN_WARNING("[memory] iOS memory warning: {}", MemoryInfo::Describe());
    }];
    LOGN("[overlay] native controls, Settings (gear / three fingers) and live log (four fingers) ready");
}

void Overlay::ToggleSettings()
{
    if (g_overlay == nil)
        return;
    [g_overlay.pad drReleaseAll];
    if (g_overlay.panel.drOpen) [g_overlay.panel drHide]; else [g_overlay.panel drShow];
}

bool IosGpu::SupportsBCTextures()
{
    @autoreleasepool
    {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil)
            return false;
        if (@available(iOS 16.4, *))
            return [device supportsBCTextureCompression];
        return false;
    }
}
