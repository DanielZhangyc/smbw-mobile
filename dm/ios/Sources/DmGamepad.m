// 虚拟手柄（常见串流手柄的布局，任天堂键位）+ 实体手柄（GameController）。两路合并后经 dm_input_set 交给宿主（hle_hid.c，键位见 dm_input.h）。
//   上角：ZL 在上、L 在下（同一列），ZR / R 同，水滴形，尖朝屏角；下角：− / +，紧贴其上是十字键（左，四个分开的箭头键）与 A（右）B（下）X（上）Y（左）菱形，手指在键间滑动会换键；
//   紧挨在它们内侧、贴底：左、右摇杆（环固定，推量按环心算，带小死区）。控件都往边角靠，给画面多留地方；按键叠在画面上，每个控件一个 CAShapeLayer：按下只换填色，摇杆只挪手柄头，不整屏重画。
//   接上实体手柄时虚拟手柄隐藏，拔掉后重现。实体手柄按位置对应：下 = B、右 = A、左 = Y、上 = X（与 Switch 手柄的键位位置一致）。
// Virtual gamepad (the usual streaming-pad layout, Nintendo button positions) + physical controllers (GameController). Both are merged and handed to the host through dm_input_set (hle_hid.c, bit layout in dm_input.h).
//   Top corners: ZL above L (one column), ZR above R likewise, teardrops with the tip toward the screen corner; bottom corners: − / +, with the D-pad (left, four separate arrow pads) and the A (right) B (bottom) X (top) Y (left) diamond right above them, sliding a finger between keys switches keys;
//   just inside those, along the bottom edge: left and right sticks (fixed rings, tilt measured from the ring centre, small dead zone). Everything hugs the edges and corners to leave the picture room; controls sit over the picture, one CAShapeLayer each: a press only swaps the fill, a stick only moves its knob, nothing redraws the whole screen.
//   The virtual gamepad hides while a physical controller is connected and comes back when it is removed. Physical controllers map by position: bottom = B, right = A, left = Y, top = X (same positions as on a Switch controller).
#import <UIKit/UIKit.h>
#import <GameController/GameController.h>
#import <CoreText/CoreText.h>
#include <pthread.h>
#include "dm_input.h"

enum { B_A = 0, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN, B_N };
enum { C_NONE = 0, C_STICK, C_DPAD, C_FACE, C_BTN };   // 触点占用的控件 | which control a touch holds

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_touch_btn, g_pad_btn;
static int32_t g_touch_lx, g_touch_ly, g_touch_rx, g_touch_ry, g_pad_lx, g_pad_ly, g_pad_rx, g_pad_ry;
static int tilted(int32_t x, int32_t y) { return (int64_t)x * x + (int64_t)y * y > (int64_t)4000 * 4000; }
static void publish(void) {
    pthread_mutex_lock(&g_mu);
    int32_t lx = g_touch_lx, ly = g_touch_ly, rx = g_touch_rx, ry = g_touch_ry;
    if (tilted(g_pad_lx, g_pad_ly)) { lx = g_pad_lx; ly = g_pad_ly; }   // 实体摇杆推着时用实体的 | a tilted physical stick wins
    if (tilted(g_pad_rx, g_pad_ry)) { rx = g_pad_rx; ry = g_pad_ry; }
    dm_input_set(g_touch_btn | g_pad_btn, lx, ly, rx, ry);
    pthread_mutex_unlock(&g_mu);
}

// ---- 布局常数（单位 u；按左半边给，x 量自外侧边，右半边镜像） | Layout constants (in u; given for the left half, x measured from the outer edge, the right half mirrors) ----
static const CGFloat
    K_ZL_X = 44, K_ZL_Y = 40, K_ZL_R = 26,          // ZL / ZR：中心离外侧、离顶，半径 | ZL / ZR: centre from the outer edge and the top, radius
    K_L_X = 44, K_L_Y = 108, K_L_R = 24,            // L / R：与 ZL / ZR 同一列 | L / R: same column as ZL / ZR
    K_DROP_TIP = 1.45,                              // 水滴尖离圆心（半径的倍数） | teardrop tip distance from the centre (times the radius)
    K_DPAD_X = 64, K_FACE_X = 81, K_PAD_Y = 108,    // 十字键、菱形中心离外侧，二者离底 | D-pad and diamond centre from the outer edge, both from the bottom
    K_DPAD_R = 54, K_ARM_W = 18, K_ARM_IN = 12,     // 十字键：箭头外沿离中心、半宽、尖离中心 | D-pad: arm outer edge from the centre, half-width, tip from the centre
    K_FACE_GAP = 50, K_FACE_R = 27,                 // 菱形：键离中心、键半径 | diamond: key distance from the centre, key radius
    K_PILL_X = 32, K_PILL_Y = 16, K_PILL_W = 50, K_PILL_H = 28,   // − / +：中心离外侧、离底，宽高 | − / +: centre from the outer edge and the bottom, size
    K_STICK_LX = 176, K_STICK_RX = 214, K_STICK_Y = 64, K_STICK_R = 50, K_KNOB_R = 22,   // 摇杆：左 / 右环心离外侧（菱形比十字键宽）、离底，环半径、手柄头半径 | sticks: left / right ring centre from the outer edge (the diamond is wider than the D-pad), from the bottom, ring radius, knob radius
    K_DEAD = 0.12;                                  // 摇杆死区（环半径的倍数） | stick dead zone (times the ring radius)
static const int k_btns[] = {B_ZL, B_L, B_ZR, B_R, B_MINUS, B_PLUS};

@interface DmGamepad : UIView
@end
static __weak DmGamepad *g_view;

@implementation DmGamepad {
    NSMapTable<UITouch *, NSNumber *> *_owner;     // 触点 → 控件（C_BTN 时附带键号、C_STICK 时附带摇杆号，<< 8） | touch → control (with the key for C_BTN or the stick for C_STICK, << 8)
    UITouch *_stick[2]; CGPoint _stickNow[2];      // 0 左 1 右 | 0 left, 1 right
    uint64_t _faceBits, _dpadBits, _btnBits;
    NSMapTable<UITouch *, NSNumber *> *_faceOf;    // 每个落在菱形区的触点各自按着哪些键 | which keys each touch on the diamond currently holds
    CAShapeLayer *_key[B_N]; CATextLayer *_label[B_N];   // 每个键一层（摇杆按键位空着） | one layer per key (the stick-click bits stay empty)
    CAShapeLayer *_ring[2], *_knob[2];
    BOOL _sticksShown;                             // 上次刷新时有摇杆按着 | a stick was held at the last refresh
}

static UIColor *k_idle, *k_on, *k_line, *k_ringFill, *k_text, *k_textOn;
static CAShapeLayer *shape(CALayer *parent) {
    CAShapeLayer *l = [CAShapeLayer layer];
    l.fillColor = k_idle.CGColor; l.strokeColor = k_line.CGColor; l.lineWidth = 1.5;
    l.shadowColor = UIColor.blackColor.CGColor; l.shadowOpacity = 0.2; l.shadowRadius = 3; l.shadowOffset = CGSizeMake(0, 1);
    [parent addSublayer:l]; return l;
}
static CATextLayer *label(CALayer *parent, NSString *s) {
    CATextLayer *t = [CATextLayer layer];
    t.string = s; t.alignmentMode = kCAAlignmentCenter; t.foregroundColor = k_text.CGColor; t.contentsScale = UIScreen.mainScreen.scale;
    [parent addSublayer:t]; return t;
}

- (instancetype)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        self.multipleTouchEnabled = YES; self.opaque = NO; self.backgroundColor = UIColor.clearColor;
        _owner = [NSMapTable strongToStrongObjectsMapTable]; _faceOf = [NSMapTable strongToStrongObjectsMapTable];
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            // 各色各自压低不透明度，不调整个视图的 alpha（那样全屏层要多一遍离屏合成） | each colour carries its own low opacity instead of lowering the whole view's alpha (that would add an off-screen pass for the full-screen layer)
            k_idle = [UIColor colorWithWhite:1 alpha:0.14]; k_on = [UIColor colorWithWhite:1 alpha:0.45]; k_line = [UIColor colorWithWhite:1 alpha:0.38];
            k_ringFill = [UIColor colorWithWhite:1 alpha:0.06]; k_text = [UIColor colorWithWhite:1 alpha:0.6]; k_textOn = [UIColor colorWithWhite:0 alpha:0.7];
        });
        for (int i = 0; i < 2; i++) {
            _ring[i] = shape(self.layer); _ring[i].fillColor = k_ringFill.CGColor;
            _knob[i] = shape(self.layer);
            CAShapeLayer *in = [CAShapeLayer layer]; in.fillColor = nil; in.strokeColor = k_line.CGColor; in.lineWidth = 1.5;   // 手柄头内圈 | inner ring of the knob
            [_knob[i] addSublayer:in];
        }
        static NSString *const names[B_N] = {@"A", @"B", @"X", @"Y", nil, nil, @"L", @"R", @"ZL", @"ZR", @"+", @"−"};
        for (int b = 0; b < B_N; b++) {
            if (b == B_LS || b == B_RS) continue;
            _key[b] = shape(self.layer);
            if (names[b]) _label[b] = label(_key[b], names[b]);
        }
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserver:self selector:@selector(padsChanged) name:GCControllerDidConnectNotification object:nil];
        [nc addObserver:self selector:@selector(padsChanged) name:GCControllerDidDisconnectNotification object:nil];
        [self padsChanged];
    }
    return self;
}

// ---- 布局（点；按安全区内的矩形算） | Layout (points; computed inside the safe area) ----
- (CGRect)safe { return UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets); }
- (CGFloat)u { return MIN(self.bounds.size.height / 430.0, 1.3); }   // 以 430 点高（iPhone Pro Max 横屏）为 1 | 1 = 430 points tall (iPhone Pro Max landscape)
static CGPoint side(CGRect s, int right, CGFloat dx, CGFloat y) { return CGPointMake(right ? CGRectGetMaxX(s) - dx : CGRectGetMinX(s) + dx, y); }
- (CGPoint)faceCenter { CGRect s = self.safe; CGFloat u = self.u; return side(s, 1, K_FACE_X * u, CGRectGetMaxY(s) - K_PAD_Y * u); }
- (CGFloat)faceGap { return K_FACE_GAP * self.u; }
- (CGFloat)faceR { return K_FACE_R * self.u; }
- (CGPoint)facePos:(int)b { CGPoint c = self.faceCenter; CGFloat g = self.faceGap;
    switch (b) { case B_A: return CGPointMake(c.x + g, c.y); case B_B: return CGPointMake(c.x, c.y + g); case B_X: return CGPointMake(c.x, c.y - g); default: return CGPointMake(c.x - g, c.y); } }
- (CGPoint)dpadCenter { CGRect s = self.safe; CGFloat u = self.u; return side(s, 0, K_DPAD_X * u, CGRectGetMaxY(s) - K_PAD_Y * u); }
- (CGFloat)dpadR { return K_DPAD_R * self.u; }
- (CGPoint)stickHome:(int)i { CGRect s = self.safe; CGFloat u = self.u; return side(s, i, (i ? K_STICK_RX : K_STICK_LX) * u, CGRectGetMaxY(s) - K_STICK_Y * u); }
- (CGFloat)stickR { return K_STICK_R * self.u; }
static BOOL isDrop(int b) { return b == B_ZL || b == B_L || b == B_ZR || b == B_R; }
- (CGPoint)dropCenter:(int)b { CGRect s = self.safe; CGFloat u = self.u; BOOL z = b == B_ZL || b == B_ZR;
    return side(s, b == B_ZR || b == B_R, (z ? K_ZL_X : K_L_X) * u, CGRectGetMinY(s) + (z ? K_ZL_Y : K_L_Y) * u); }
- (CGFloat)dropR:(int)b { return (b == B_ZL || b == B_ZR ? K_ZL_R : K_L_R) * self.u; }
- (CGRect)pillRect:(int)b { CGRect s = self.safe; CGFloat u = self.u, w = K_PILL_W * u, h = K_PILL_H * u;
    CGPoint c = side(s, b == B_PLUS, K_PILL_X * u, CGRectGetMaxY(s) - K_PILL_Y * u);
    return CGRectMake(c.x - w / 2, c.y - h / 2, w, h); }

static CGFloat dist(CGPoint a, CGPoint b) { return hypot(a.x - b.x, a.y - b.y); }
- (int)btnAt:(CGPoint)p {   // 肩键与 − / +（只在按下时判，之后锁定） | shoulders and − / + (tested on touch-down only, then locked)
    CGFloat u = self.u;
    for (int i = 0; i < 6; i++) { int b = k_btns[i];
        if (isDrop(b) ? dist(p, [self dropCenter:b]) <= [self dropR:b] * 1.35 : CGRectContainsPoint(CGRectInset([self pillRect:b], -8 * u, -8 * u), p)) return b; }
    return -1;
}
- (uint64_t)faceAt:(CGPoint)p {   // 菱形区里手指下的键（两键之间可同时按） | keys under the finger in the diamond (between two keys both count)
    uint64_t m = 0; CGFloat r = self.faceR * 1.25;
    for (int b = B_A; b <= B_Y; b++) if (dist(p, [self facePos:b]) <= r) m |= 1ull << b;
    return m;
}
- (BOOL)inFace:(CGPoint)p { return dist(p, self.faceCenter) <= self.faceGap + self.faceR * 1.6; }
- (uint64_t)dpadAt:(CGPoint)p {
    CGPoint c = self.dpadCenter; CGFloat dx = p.x - c.x, dy = p.y - c.y;
    if (hypot(dx, dy) < self.dpadR * 0.25) return 0;
    double a = atan2(-dy, dx) * 180 / M_PI; uint64_t m = 0;   // 8 向：每向 45°，斜向按两键 | 8 directions: 45° each, diagonals press two keys
    if (a > -67.5 && a < 67.5) m |= 1ull << B_RIGHT;
    if (a > 22.5 && a < 157.5) m |= 1ull << B_UP;
    if (a > 112.5 || a < -112.5) m |= 1ull << B_LEFT;
    if (a < -22.5 && a > -157.5) m |= 1ull << B_DOWN;
    return m;
}
- (BOOL)inDpad:(CGPoint)p { return dist(p, self.dpadCenter) <= self.dpadR * 1.3; }
- (BOOL)inStick:(int)i at:(CGPoint)p { return dist(p, [self stickHome:i]) <= self.stickR * 1.5; }
- (CGPoint)knobOffset:(int)i {   // 手柄头偏离环心（限在环内） | knob offset from the ring centre (clamped to the ring)
    if (!_stick[i]) return CGPointZero;
    CGPoint h = [self stickHome:i]; CGFloat dx = _stickNow[i].x - h.x, dy = _stickNow[i].y - h.y, d = hypot(dx, dy), R = self.stickR;
    if (d > R) { dx *= R / d; dy *= R / d; }
    return CGPointMake(dx, dy);
}

// ---- 触摸 | Touch ----
- (void)recompute {
    uint64_t face = 0; for (UITouch *t in _faceOf) face |= [_faceOf objectForKey:t].unsignedLongLongValue;
    uint64_t btn = 0, dpad = 0;
    for (UITouch *t in _owner) { uint64_t v = [_owner objectForKey:t].unsignedLongLongValue; int c = (int)(v & 0xff);
        if (c == C_BTN) btn |= 1ull << (v >> 8);
        else if (c == C_DPAD) dpad |= [self dpadAt:[t locationInView:self]]; }
    int32_t ax[4] = {0};
    for (int i = 0; i < 2; i++) {
        CGPoint o = [self knobOffset:i]; CGFloat R = self.stickR, d = hypot(o.x, o.y), dz = R * K_DEAD;
        if (d <= dz) continue;
        CGFloat m = (d - dz) / (R - dz) / d;   // 死区外从 0 起算到环边为满 | outside the dead zone: 0 rising to full at the ring edge
        ax[i * 2] = (int32_t)(o.x * m * 32767); ax[i * 2 + 1] = (int32_t)(-o.y * m * 32767);   // 宿主：上为正 | host: up is positive
    }
    BOOL changed = face != _faceBits || dpad != _dpadBits || btn != _btnBits;
    _faceBits = face; _dpadBits = dpad; _btnBits = btn;
    pthread_mutex_lock(&g_mu);
    g_touch_btn = face | dpad | btn; g_touch_lx = ax[0]; g_touch_ly = ax[1]; g_touch_rx = ax[2]; g_touch_ry = ax[3];
    pthread_mutex_unlock(&g_mu);
    publish();
    BOOL sticks = _stick[0] || _stick[1];
    if (changed || sticks || _sticksShown) [self refresh];   // 松开摇杆那次也要刷，手柄头才回中 | also refresh on the stick release so the knob returns to the centre
    _sticksShown = sticks;
}
- (void)touchesBegan:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) {
        CGPoint p = [t locationInView:self]; int hit = [self btnAt:p];
        if (hit >= 0) [_owner setObject:@(C_BTN | (uint64_t)hit << 8) forKey:t];
        else if ([self inFace:p]) { [_owner setObject:@(C_FACE) forKey:t]; [_faceOf setObject:@([self faceAt:p]) forKey:t]; }
        else if ([self inDpad:p]) [_owner setObject:@(C_DPAD) forKey:t];
        else for (int i = 0; i < 2; i++) if (!_stick[i] && [self inStick:i at:p]) {
            [_owner setObject:@(C_STICK | (uint64_t)i << 8) forKey:t]; _stick[i] = t; _stickNow[i] = p; break; }
    }
    [self recompute];
}
- (void)touchesMoved:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e {
    for (UITouch *t in ts) {
        uint64_t v = [_owner objectForKey:t].unsignedLongLongValue; int c = (int)(v & 0xff); CGPoint p = [t locationInView:self];
        if (c == C_FACE) [_faceOf setObject:@([self faceAt:p]) forKey:t];
        else if (c == C_STICK) _stickNow[v >> 8] = p;
    }
    [self recompute];
}
- (void)endTouches:(NSSet<UITouch *> *)ts {
    for (UITouch *t in ts) { [_owner removeObjectForKey:t]; [_faceOf removeObjectForKey:t]; for (int i = 0; i < 2; i++) if (t == _stick[i]) _stick[i] = nil; }
    [self recompute];
}
- (void)touchesEnded:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self endTouches:ts]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)ts withEvent:(UIEvent *)e { [self endTouches:ts]; }
- (void)reset { [_owner removeAllObjects]; [_faceOf removeAllObjects]; _stick[0] = _stick[1] = nil; [self recompute]; }

// ---- 形状 | Shapes ----
static CGPathRef circlePath(CGPoint c, CGFloat r) { return CGPathCreateWithEllipseInRect(CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r), NULL); }
static CGPathRef dropPath(CGPoint c, CGFloat r, CGFloat tipAngle, CGFloat round) {   // 水滴：圆 + 两条切线交于尖，尖略圆 | teardrop: circle + two tangents meeting at the tip, tip slightly rounded
    CGFloat d = r * K_DROP_TIP, th = acos(r / d);
    CGPoint tip = CGPointMake(c.x + d * cos(tipAngle), c.y + d * sin(tipAngle));
    CGPoint p1 = CGPointMake(c.x + r * cos(tipAngle + th), c.y + r * sin(tipAngle + th)), p2 = CGPointMake(c.x + r * cos(tipAngle - th), c.y + r * sin(tipAngle - th));
    CGMutablePathRef p = CGPathCreateMutable();
    CGPathMoveToPoint(p, NULL, p2.x, p2.y);
    CGPathAddArcToPoint(p, NULL, tip.x, tip.y, p1.x, p1.y, round);
    CGPathAddArc(p, NULL, c.x, c.y, r, tipAngle + th, tipAngle - th + 2 * M_PI, false);
    CGPathCloseSubpath(p); return p;
}
static CGPathRef armPath(CGPoint c, CGFloat R, CGFloat w, CGFloat in, CGFloat angle, CGFloat round) {   // 十字键一臂：朝上的五边形（尖朝中心）绕中心转 angle | one D-pad arm: an upward pentagon (tip toward the centre) rotated by angle around the centre
    CGPoint v[5] = {{-w, -R}, {w, -R}, {w, -(in + w)}, {0, -in}, {-w, -(in + w)}};
    CGFloat cs = cos(angle), sn = sin(angle);
    for (int i = 0; i < 5; i++) v[i] = CGPointMake(c.x + v[i].x * cs - v[i].y * sn, c.y + v[i].x * sn + v[i].y * cs);
    CGMutablePathRef p = CGPathCreateMutable();
    CGPathMoveToPoint(p, NULL, (v[4].x + v[0].x) / 2, (v[4].y + v[0].y) / 2);
    for (int i = 0; i < 5; i++) CGPathAddArcToPoint(p, NULL, v[i].x, v[i].y, v[(i + 1) % 5].x, v[(i + 1) % 5].y, round);
    CGPathCloseSubpath(p); return p;
}
static void setPath(CAShapeLayer *l, CGPathRef p) { l.path = p; l.shadowPath = p; CGPathRelease(p); }
static void placeLabel(CATextLayer *t, CGPoint c, CGFloat fs) {
    CTFontRef f = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, fs, NULL);
    t.font = f; t.fontSize = fs;
    CGFloat h = CTFontGetAscent(f) + CTFontGetDescent(f) + CTFontGetLeading(f), w = fs * 3;
    t.frame = CGRectMake(c.x - w / 2, c.y - h / 2, w, h);
    CFRelease(f);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGFloat u = self.u;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int b = B_A; b <= B_Y; b++) { CGPoint c = [self facePos:b]; setPath(_key[b], circlePath(c, self.faceR)); placeLabel(_label[b], c, 20 * u); }
    static const struct { int b; CGFloat angle; } arms[] = {{B_UP, 0}, {B_RIGHT, M_PI_2}, {B_DOWN, M_PI}, {B_LEFT, -M_PI_2}};   // 同一臂转 90° 一步 | the same arm in 90° steps
    for (int i = 0; i < 4; i++) setPath(_key[arms[i].b], armPath(self.dpadCenter, self.dpadR, K_ARM_W * u, K_ARM_IN * u, arms[i].angle, 6 * u));
    for (int i = 0; i < 6; i++) { int b = k_btns[i];
        if (isDrop(b)) {
            BOOL right = b == B_ZR || b == B_R; CGPoint c = [self dropCenter:b];
            setPath(_key[b], dropPath(c, [self dropR:b], right ? -M_PI_4 : -3 * M_PI_4, 4 * u)); placeLabel(_label[b], c, 16 * u);   // 尖朝同侧上角 | tip toward the near top corner
        } else {
            CGRect rc = [self pillRect:b];
            setPath(_key[b], CGPathCreateWithRoundedRect(rc, rc.size.height / 2, rc.size.height / 2, NULL)); placeLabel(_label[b], CGPointMake(CGRectGetMidX(rc), CGRectGetMidY(rc)), 18 * u);
        }
    }
    for (int i = 0; i < 2; i++) {
        setPath(_ring[i], circlePath([self stickHome:i], self.stickR));
        CGFloat k = K_KNOB_R * u; _knob[i].bounds = CGRectMake(0, 0, 2 * k, 2 * k);
        setPath(_knob[i], circlePath(CGPointMake(k, k), k));
        CAShapeLayer *in = (CAShapeLayer *)_knob[i].sublayers.firstObject; CGPathRef ip = circlePath(CGPointMake(k, k), k * 0.62);
        in.frame = _knob[i].bounds; in.path = ip; CGPathRelease(ip);
    }
    [CATransaction commit];
    [self refresh];
}
- (void)safeAreaInsetsDidChange { [super safeAreaInsetsDidChange]; [self setNeedsLayout]; }
- (void)refresh {   // 按下状态 → 填色，摇杆 → 手柄头位置 | pressed state → fill, sticks → knob position
    uint64_t all = _faceBits | _dpadBits | _btnBits;
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    for (int b = 0; b < B_N; b++) if (_key[b]) { BOOL on = (all >> b) & 1;
        _key[b].fillColor = (on ? k_on : k_idle).CGColor; _label[b].foregroundColor = (on ? k_textOn : k_text).CGColor; }
    for (int i = 0; i < 2; i++) {
        CGPoint h = [self stickHome:i], o = [self knobOffset:i];
        _knob[i].position = CGPointMake(h.x + o.x, h.y + o.y); _knob[i].fillColor = (_stick[i] ? k_on : k_idle).CGColor;
    }
    [CATransaction commit];
}

// ---- 实体手柄 | Physical controllers ----
static int32_t axis(float v) { return (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * 32767); }
static void read_pad(GCExtendedGamepad *g) {
    uint64_t m = 0;
    #define K(btn, bit) if (btn.isPressed) m |= 1ull << bit
    K(g.buttonB, B_A); K(g.buttonA, B_B); K(g.buttonY, B_X); K(g.buttonX, B_Y);   // 按位置：右 A、下 B、上 X、左 Y | by position: right A, bottom B, top X, left Y
    K(g.leftShoulder, B_L); K(g.rightShoulder, B_R); K(g.leftTrigger, B_ZL); K(g.rightTrigger, B_ZR);
    K(g.buttonMenu, B_PLUS); if (g.buttonOptions) K(g.buttonOptions, B_MINUS);
    if (g.leftThumbstickButton) K(g.leftThumbstickButton, B_LS); if (g.rightThumbstickButton) K(g.rightThumbstickButton, B_RS);
    K(g.dpad.left, B_LEFT); K(g.dpad.up, B_UP); K(g.dpad.right, B_RIGHT); K(g.dpad.down, B_DOWN);
    #undef K
    pthread_mutex_lock(&g_mu);
    g_pad_btn = m; g_pad_lx = axis(g.leftThumbstick.xAxis.value); g_pad_ly = axis(g.leftThumbstick.yAxis.value);
    g_pad_rx = axis(g.rightThumbstick.xAxis.value); g_pad_ry = axis(g.rightThumbstick.yAxis.value);
    pthread_mutex_unlock(&g_mu);
    publish();
}
- (void)padsChanged {
    BOOL any = NO;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad) {
        any = YES; c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; read_pad(g); };
        read_pad(c.extendedGamepad);
    }
    if (!any) { pthread_mutex_lock(&g_mu); g_pad_btn = 0; g_pad_lx = g_pad_ly = g_pad_rx = g_pad_ry = 0; pthread_mutex_unlock(&g_mu); publish(); }
    dispatch_async(dispatch_get_main_queue(), ^{ self.hidden = any; if (any) [self reset]; });
}
@end

UIView *dm_gamepad_view(CGRect frame) { DmGamepad *v = [[DmGamepad alloc] initWithFrame:frame]; g_view = v; return v; }
void dm_gamepad_reset(void) {   // 失活时：松开所有键（按着键切走、在后台松手，回来不会补「松开」） | On resign active: release everything (a key held while switching away and released in background gets no "release")
    dispatch_async(dispatch_get_main_queue(), ^{ [g_view reset]; });
    pthread_mutex_lock(&g_mu); g_pad_btn = 0; g_pad_lx = g_pad_ly = g_pad_rx = g_pad_ry = 0; pthread_mutex_unlock(&g_mu); publish();
}
