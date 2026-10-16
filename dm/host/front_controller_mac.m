// Mac 原生扩展手柄：全部状态由主队列上的连接通知和输入事件驱动，不按渲染帧轮询。
// Native Mac extended controllers: main-queue connection notifications and input events drive all state, without per-render-frame polling.
// 支持系统识别的 Xbox / PlayStation / Nintendo 扩展布局，单玩家；可选摇杆按键 / Options 缺失时不合成按键。
// Supports system-recognized Xbox / PlayStation / Nintendo extended layouts for one player; absent optional stick clicks / Options are not synthesized.
// 触觉按真实设备能力异步接入；不提供六轴、Home / Capture、单只 Joy-Con 或配对界面；无线设备须先由 macOS 配对。
// Haptics use actual device capabilities asynchronously; no motion, Home / Capture, single Joy-Con or pairing UI; wireless devices must first be paired through macOS.
#import <GameController/GameController.h>
#include <math.h>
#include "front.h"
#include "dm.h"
#include "dm_input.h"

static NSMutableArray<GCController *> *g_controllers;
static NSMutableArray<id> *g_controller_observers;
static GCController *g_controller;
static BOOL g_controller_active;

static void controller_release(void) { dm_pad_reset(); }
static int32_t controller_axis(float v) {
    return isfinite(v) ? (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * DM_STICK_MAX) : 0;
}

static void controller_publish(GCExtendedGamepad *g) {
    if (!g_controller_active || !g || g.controller != g_controller) return;
    uint64_t buttons = 0;
#define BUTTON(INPUT, BIT) do { if ((INPUT).isPressed) buttons |= (BIT); } while (0)
    // Apple 扩展布局固定为下 A、右 B、左 X、上 Y；Switch 固定为右 A、下 B、上 X、左 Y。
    // Apple's extended layout is bottom A, right B, left X, top Y; Switch uses right A, bottom B, top X, left Y.
    // 按位置而非外壳字母映射；保留系统的用户重映射，不按 Xbox / PS / Nintendo 名称猜测。
    // Map positions rather than printed letters; honor system user remapping without guessing from Xbox / PS / Nintendo names.
    BUTTON(g.buttonB, DM_BTN_A); BUTTON(g.buttonA, DM_BTN_B);
    BUTTON(g.buttonY, DM_BTN_X); BUTTON(g.buttonX, DM_BTN_Y);
    BUTTON(g.leftShoulder, DM_BTN_L); BUTTON(g.rightShoulder, DM_BTN_R);
    BUTTON(g.leftTrigger, DM_BTN_ZL); BUTTON(g.rightTrigger, DM_BTN_ZR);
    BUTTON(g.leftThumbstickButton, DM_BTN_LS); BUTTON(g.rightThumbstickButton, DM_BTN_RS);
    BUTTON(g.buttonMenu, DM_BTN_PLUS); BUTTON(g.buttonOptions, DM_BTN_MINUS);
    BUTTON(g.dpad.left, DM_BTN_LEFT); BUTTON(g.dpad.up, DM_BTN_UP);
    BUTTON(g.dpad.right, DM_BTN_RIGHT); BUTTON(g.dpad.down, DM_BTN_DOWN);
#undef BUTTON
    dm_pad_set(buttons, controller_axis(g.leftThumbstick.xAxis.value), controller_axis(g.leftThumbstick.yAxis.value),
               controller_axis(g.rightThumbstick.xAxis.value), controller_axis(g.rightThumbstick.yAxis.value));
}

static void controller_select(GCController *controller) {
    if (controller == g_controller) return;
    controller_release();
    g_controller = controller;
    dm_haptic_bind((__bridge void *)controller, g_controller_active);
    if (controller) LOG("Mac 手柄：当前 %s；按位置映射：右=A 下=B 上=X 左=Y", controller.vendorName.UTF8String ?: "未命名");
}

static void controller_attach(GCController *controller) {
    if (!g_controllers || !controller || [g_controllers containsObject:controller]) return;
    if (!controller.extendedGamepad) {
        LOG("Mac 手柄：忽略不具备 extendedGamepad 的设备 %s", controller.vendorName.UTF8String ?: "未命名");
        return;
    }
    [g_controllers addObject:controller];
    controller.handlerQueue = dispatch_get_main_queue();
    controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
        (void)element;
        GCController *source = gamepad.controller;
        if (!source || ![g_controllers containsObject:source]) return;
        // current 由系统根据真实操作选择；同时在事件内核对，以免通知与首个按键的顺序漏掉输入。
        // The system selects current from actual use; also check it inside the event so notification ordering cannot lose the first press.
        if (source == GCController.current) controller_select(source);
        controller_publish(gamepad);
    };
    LOG("Mac 手柄：连接 %s (%s)", controller.vendorName.UTF8String ?: "未命名", controller.productCategory.UTF8String);
    if (!g_controller || controller == GCController.current) controller_select(controller);
}

void front_controller_start(void) {
    NSCAssert(NSThread.isMainThread, @"Controller lifecycle must run on the main thread");
    if (g_controllers) return;
    g_controllers = [NSMutableArray new]; g_controller_observers = [NSMutableArray new];
    GCController.shouldMonitorBackgroundEvents = NO;
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    NSOperationQueue *queue = NSOperationQueue.mainQueue;
    [g_controller_observers addObject:[nc addObserverForName:GCControllerDidConnectNotification object:nil queue:queue usingBlock:^(NSNotification *note) {
        controller_attach(note.object);
    }]];
    [g_controller_observers addObject:[nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:queue usingBlock:^(NSNotification *note) {
        GCController *controller = note.object;
        if (!controller || ![g_controllers containsObject:controller]) return;
        controller.extendedGamepad.valueChangedHandler = nil;
        [g_controllers removeObject:controller];
        if (g_controller == controller) {
            controller_select(nil);
            GCController *next = GCController.current;
            controller_select(next && [g_controllers containsObject:next] ? next : g_controllers.firstObject);
            // 断线切换只释放，下一次真实输入才发布新手柄，避免恢复另一手柄的旧按下状态。
            // Disconnect switching only releases; the next real input publishes the new controller, avoiding an old held state.
        }
        LOG("Mac 手柄：断开 %s", controller.vendorName.UTF8String ?: "未命名");
    }]];
    [g_controller_observers addObject:[nc addObserverForName:GCControllerDidBecomeCurrentNotification object:nil queue:queue usingBlock:^(NSNotification *note) {
        GCController *controller = note.object;
        if (!controller || ![g_controllers containsObject:controller]) return;
        controller_select(controller); controller_publish(controller.extendedGamepad);
    }]];
    for (GCController *controller in GCController.controllers) controller_attach(controller);
    if (!g_controllers.count) LOG("Mac 手柄：未检测到扩展手柄，实体设备输入尚未实机验证");
}

void front_controller_set_active(int active) {
    NSCAssert(NSThread.isMainThread, @"Controller lifecycle must run on the main thread");
    BOOL wanted = active != 0;
    if (g_controller_active == wanted) return;
    g_controller_active = wanted;
    dm_haptic_bind((__bridge void *)g_controller, wanted);
    // 失焦立即释放；恢复后等待真实输入，不重发后台可能未收到松键的旧值。
    // Release immediately on focus loss; after returning, await real input rather than replaying a possibly stale background hold.
    controller_release();
}

void front_controller_stop(void) {
    NSCAssert(NSThread.isMainThread, @"Controller lifecycle must run on the main thread");
    g_controller_active = NO; controller_release();
    (void)dm_haptic_shutdown("controller-stop", 600);
    for (id observer in g_controller_observers) [NSNotificationCenter.defaultCenter removeObserver:observer];
    for (GCController *controller in g_controllers) controller.extendedGamepad.valueChangedHandler = nil;
    g_controller = nil; g_controllers = nil; g_controller_observers = nil;
}
