//
//  S5LBox — root view controller.
//
//  The screen is now the point. The top of the view is the guest's 320x480
//  framebuffer; underneath it is the guest's UART, which is where an operating
//  system announces itself. Between them is a status line showing that the
//  emulator is doing work rather than being a picture of an emulator.
//
//  The self-tests that used to be this whole screen still run, once, at launch,
//  and print into the console: they are the proof that the ARM core, the MMU,
//  the bus, the UART, the VIC and the timer all work on *this* device, and that
//  is worth keeping even now that there is something to look at.
//
//  WHAT THE CONTROLS ON THIS SCREEN DO, AND DO NOT DO
//
//  Play/pause and reset drive the emulator and are read back from it, so the
//  toolbar shows what the machine is doing rather than what the last tap
//  intended. Everything to do with INPUT does not work, and is drawn as not
//  working: the row of physical keys under the screen is disabled with the
//  reason printed beneath it, and a finger on the guest's panel has its
//  coordinate mapped, shown on the status line, and thrown away.
//
//  That last part is deliberate rather than lazy. The mapping is real, tested
//  arithmetic (VMTouchMap.c, app/Tests/test_vmtouchmap.c), and showing it live
//  is how it can be seen to be right before there is a digitizer to send it to.
//  What must never happen is a control that looks like it works.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "EmulatorViewController.h"
#import "VMButtonBar.h"
#import "VMEngine.h"
#import "VMFramebufferView.h"
#import "VMGuest.h"
#import "VMSettings.h"
#import "VMSettingsViewController.h"
#import "VMTouchMap.h"

#import <QuartzCore/QuartzCore.h>
#import <math.h>
#import <stdlib.h>
#import <string.h>
#import <sys/mman.h>
#import <sys/utsname.h>
#import <unistd.h>

// csops() reports our own code-signing flags. A jailbreak that enables "JIT in
// apps" sets CS_DEBUGGED, which is what permits unsigned executable memory.
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED   0x10000000

// Scrollback kept in the console. The guest prints one short line per frame
// forever, so this cannot be unbounded.
static const NSUInteger kConsoleScrollback = 12000;

@class EmulatorViewController;

/* CADisplayLink retains its target. Keeping only a weak edge back to the view
 * controller lets normal controller teardown reach -dealloc and stop the VM. */
@interface VMDisplayLinkProxy : NSObject
- (instancetype)initWithTarget:(EmulatorViewController *)target;
- (void)tick:(CADisplayLink *)sender;
@end

// Declared up front so every call below is checked against a prototype.
@interface EmulatorViewController () <VMButtonBarDelegate,
                                      VMFramebufferViewTouchDelegate>
- (void)startEmulator;
- (void)launchEngine;
- (void)tick:(CADisplayLink *)sender;
- (void)append:(NSString *)line;
- (void)appendConsole:(NSString *)text;
- (void)flushConsole;
- (void)reportEnvironment;
- (void)runUartDemo;
- (void)runInterruptDemo;
- (void)runMmuDemo;
- (void)appWillResignActive:(NSNotification *)notification;
- (void)appDidBecomeActive:(NSNotification *)notification;
- (void)settingsDidChange:(NSNotification *)notification;
- (void)applyPauseState;
- (void)applySettingsToEngine;
- (void)refreshRunControls;
- (void)refreshStatusLine;
- (void)playPauseTapped:(id)sender;
- (void)resetTapped:(id)sender;
- (void)settingsTapped:(id)sender;
@end

@implementation VMDisplayLinkProxy {
    __weak EmulatorViewController *_target;
}

- (instancetype)initWithTarget:(EmulatorViewController *)target {
    self = [super init];
    if (self) _target = target;
    return self;
}

- (void)tick:(CADisplayLink *)sender {
    EmulatorViewController *target = _target;
    if (target) [target tick:sender];
    else [sender invalidate];
}

@end

@implementation EmulatorViewController {
    VMFramebufferView *_screen;
    VMButtonBar       *_keys;
    UILabel           *_stats;
    UITextView        *_console;
    UIToolbar         *_toolbar;
    NSMutableString   *_consoleText;
    BOOL               _consoleDirty;

    VMEngine          *_engine;
    CADisplayLink     *_link;
    uint8_t           *_frame;        // main thread's copy of the guest's pixels
    NSUInteger         _ticks;

    /* Pause has two independent causes and the engine has one flag, so the
     * causes are tracked here and combined. Without this, coming back to the
     * foreground would silently un-pause a machine the user had paused on
     * purpose. Neither of these is a copy of engine state: they are the two
     * reasons for it, and -applyPauseState is the only thing that writes it. */
    BOOL               _userPaused;
    BOOL               _inBackground;

    /* What the toolbar is currently showing, so it is only rebuilt when the
     * engine's answer changes rather than four times a second. */
    BOOL               _toolbarShowsPlay;
    BOOL               _toolbarBuilt;

    /* The last touch the mapping produced. Displayed, never delivered. */
    BOOL               _haveTouch;
    int                _touchX;
    int                _touchY;
}

#pragma mark - Lifecycle

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor blackColor];

    _consoleText = [NSMutableString string];

    _screen = [[VMFramebufferView alloc] initWithFrame:CGRectZero];
    _screen.layer.borderWidth = 1.0;
    _screen.layer.borderColor = [UIColor colorWithWhite:0.25 alpha:1.0].CGColor;
    /* Setting the delegate is what turns the picture into a touch surface. The
     * coordinates it produces are shown on the status line and discarded; see
     * -framebufferView:touchAtGuestX:guestY:phase:. */
    _screen.touchDelegate = self;
    [self.view addSubview:_screen];

    _keys = [[VMButtonBar alloc] initWithFrame:CGRectZero];
    _keys.delegate = self;
    /* The bar asks the engine whether input goes anywhere, rather than being
     * told here that it does not. One nil from the engine lights it up. */
    _keys.unavailableReason = [VMEngine buttonUnavailableReason];
    [self.view addSubview:_keys];

    _stats = [[UILabel alloc] initWithFrame:CGRectZero];
    _stats.backgroundColor = [UIColor clearColor];
    _stats.textColor = [UIColor colorWithWhite:0.62 alpha:1.0];
    _stats.font = [UIFont fontWithName:@"Menlo" size:10]
                  ?: [UIFont systemFontOfSize:10];
    _stats.numberOfLines = 2;
    _stats.text = @"starting…";
    [self.view addSubview:_stats];

    _console = [[UITextView alloc] initWithFrame:CGRectZero];
    _console.backgroundColor = [UIColor blackColor];
    _console.textColor = [UIColor colorWithRed:0.4 green:1.0 blue:0.5 alpha:1.0];
    _console.font = [UIFont fontWithName:@"Menlo" size:10]
                    ?: [UIFont systemFontOfSize:10];
    _console.editable = NO;
    /* Stated rather than inherited. A non-editable text view is selectable by
     * default, but "you can select and copy the guest's output" is a property
     * worth writing down, and -flushConsole depends on it being true. */
    _console.selectable = YES;
    _console.dataDetectorTypes = UIDataDetectorTypeNone;
    _console.textContainerInset = UIEdgeInsetsMake(6, 12, 12, 12);
    [self.view addSubview:_console];

    /* A real UIToolbar rather than a row of buttons: the system draws the
     * standard play/pause/refresh glyphs, and they are the ones anybody with an
     * iPhone already knows. */
    _toolbar = [[UIToolbar alloc] initWithFrame:CGRectZero];
    _toolbar.barStyle = UIBarStyleBlack;
    _toolbar.translucent = NO;
    [self.view addSubview:_toolbar];
    [self refreshRunControls];

    [self append:@"S5LBox  ·  on-device self-test"];
    [self append:@"================================\n"];
    [self reportEnvironment];
    [self append:@"\n-- emulated S5L8900 --"];
    [self runUartDemo];
    [self runInterruptDemo];
    [self runMmuDemo];
    [self append:@"\n-- guest framebuffer --"];

    [self startEmulator];
}

/* Everything that happens once: the pixel buffer, the display link, and the
 * notifications. -launchEngine is separate because Reset does it again. */
- (void)startEmulator {
    _frame = calloc(1, VM_FB_BYTES);
    if (!_frame) { [self append:@"[vm] out of memory for the frame buffer"]; return; }

    // 30 Hz is plenty: the guest cannot repaint 320x480 anywhere near that
    // fast, so a higher rate would only re-upload identical pixels.
    //
    VMDisplayLinkProxy *proxy = [[VMDisplayLinkProxy alloc] initWithTarget:self];
    _link = [CADisplayLink displayLinkWithTarget:proxy selector:@selector(tick:)];
    _link.preferredFramesPerSecond = 30;
    [_link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];

    // Interpreting flat out in the background is a good way to be terminated,
    // and nobody is looking at the screen anyway.
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    [nc addObserver:self selector:@selector(appWillResignActive:)
               name:UIApplicationWillResignActiveNotification object:nil];
    [nc addObserver:self selector:@selector(appDidBecomeActive:)
               name:UIApplicationDidBecomeActiveNotification object:nil];
    [nc addObserver:self selector:@selector(settingsDidChange:)
               name:VMSettingsDidChangeNotification object:nil];

    [self launchEngine];

    /* Reconcile with the current state, so a resign-active notification that
     * arrived before the observers above existed cannot leave the interpreter
     * burning CPU in the background on a memory-constrained phone.
     *
     * Tested against Background specifically. -viewDidLoad runs inside
     * -application:didFinishLaunchingWithOptions:, where the state is always
     * Inactive rather than Active, so "not Active" would take this branch on
     * every single cold launch and then immediately undo it. */
    if ([UIApplication sharedApplication].applicationState == UIApplicationStateBackground)
        [self appWillResignActive:nil];
}

- (void)launchEngine {
    if (!_frame) return;

    _engine = [[VMEngine alloc] init];
    if (![_engine start]) {
        [self append:@"[vm] emulator failed to start"];
        [self appendConsole:[_engine takePendingConsoleText]];
        _engine = nil;
        [self refreshRunControls];
        return;
    }

    [self applySettingsToEngine];
    [self applyPauseState];
    [self refreshRunControls];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
    [_link invalidate];
    [_engine stop];
    free(_frame);
}

- (void)appWillResignActive:(NSNotification *)notification {
    (void)notification;
    _inBackground = YES;
    [self applyPauseState];
}

- (void)appDidBecomeActive:(NSNotification *)notification {
    (void)notification;
    _inBackground = NO;
    [self applyPauseState];
}

- (void)settingsDidChange:(NSNotification *)notification {
    (void)notification;
    /* Only two settings are applied by this app, and both are cheap to
     * re-apply, so re-apply both rather than working out which moved. */
    [self applySettingsToEngine];
    [self applyPauseState];
    [self refreshStatusLine];
}

#pragma mark - Run state

/*
 * The single writer of the engine's pause flag. Pause has two causes — the
 * user asked, or the app is not frontmost and the setting says to stop — and
 * folding them here is what stops a return to the foreground from resuming a
 * machine the user deliberately paused.
 *
 * Backgrounding is honoured through a setting because a user who has turned
 * "pause when not frontmost" off is asking for the interpreter to keep going,
 * and the settings screen states the consequence.
 */
- (void)applyPauseState {
    const BOOL backgroundPause =
        _inBackground && [[VMSettings sharedSettings] pausesInBackground];
    const BOOL paused = _userPaused || backgroundPause;

    [_engine setPaused:paused];

    /*
     * The link stops only when the app is hidden — NOT when the machine is
     * paused. It is the only thing that drains the engine's UART buffer, and
     * the emulator thread is mid-chunk when a pause request lands, so it still
     * publishes one last time afterwards: stopping the link on pause would
     * strand exactly the output that says why the machine was paused. A tick
     * against a paused engine costs a -copyFrameInto: that returns NO and a
     * drain that returns nil.
     */
    _link.paused = _inBackground;

    [self refreshRunControls];
}

- (void)applySettingsToEngine {
    [_engine setInstructionCap:[[VMSettings sharedSettings] instructionCap]];
}

/* The toolbar is built from what the engine says, not from what was last
 * tapped, so a machine that stopped on its own — a halt, or a reached
 * instruction cap — is shown as stopped without anything having to notice. */
- (void)refreshRunControls {
    const BOOL showPlay = (_engine == nil) || [_engine isPaused] ||
                          ![_engine isRunning];
    if (_toolbarBuilt && showPlay == _toolbarShowsPlay) return;
    _toolbarShowsPlay = showPlay;
    _toolbarBuilt = YES;

    UIBarButtonItem *playPause = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:(showPlay ? UIBarButtonSystemItemPlay
                                              : UIBarButtonSystemItemPause)
                             target:self
                             action:@selector(playPauseTapped:)];
    UIBarButtonItem *reset = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh
                             target:self
                             action:@selector(resetTapped:)];
    UIBarButtonItem *space = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemFlexibleSpace
                             target:nil
                             action:nil];
    UIBarButtonItem *settings = [[UIBarButtonItem alloc]
        initWithTitle:@"Settings"
                style:UIBarButtonItemStylePlain
               target:self
               action:@selector(settingsTapped:)];

    [_toolbar setItems:@[playPause, reset, space, settings] animated:NO];
}

- (void)playPauseTapped:(id)sender {
    (void)sender;

    /* A machine that has stopped — halted, or reached its cap — cannot be
     * resumed, because its s5l8900_t is gone. Play then means start a new one,
     * which is what Reset does, and saying so is better than a dead button. */
    if (!_engine || ![_engine isRunning]) {
        [self resetTapped:nil];
        return;
    }

    _userPaused = !_userPaused;
    [self applyPauseState];
    [self refreshStatusLine];
}

- (void)resetTapped:(id)sender {
    (void)sender;
    if (!_frame) return;

    /* Take what the outgoing machine has already said before letting go of it.
     * Its final publication — the last of the guest's output, and the engine's
     * own "stopped" line — happens on a thread whose only reader is about to
     * be dropped, so anything not collected here is lost. */
    [self appendConsole:[_engine takePendingConsoleText]];
    [self append:@"\n[vm] reset: dropping this machine and building a new one"];

    /* -stop is a request the emulator thread notices between chunks, so the
     * old machine is not gone yet. Nothing waits for it: the thread holds the
     * last reference to its own engine and frees the machine before letting
     * go, so a fresh engine can be built immediately alongside it. The two
     * overlap for a few tens of milliseconds and 128 MB of guest DRAM is
     * address space rather than footprint until it is touched (see VMEngine.m),
     * so the overlap costs a fraction of a megabyte. */
    [_engine stop];
    _engine = nil;

    _userPaused = NO;
    _haveTouch = NO;
    [self launchEngine];
    [self refreshStatusLine];
}

- (void)settingsTapped:(id)sender {
    (void)sender;

    VMSettingsViewController *settings = [[VMSettingsViewController alloc] init];
    UINavigationController *nav = [[UINavigationController alloc]
        initWithRootViewController:settings];
    // The emulator screen is black; a white sheet over it would be a jolt.
    nav.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    /* Nothing needs intercepting on dismissal: every control writes through to
     * NSUserDefaults immediately and posts VMSettingsDidChangeNotification, so
     * a swipe-to-dismiss and the Done button are the same thing. */
    [self presentViewController:nav animated:YES completion:nil];
}

#pragma mark - Layout

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    CGRect b = self.view.bounds;
    UIEdgeInsets safe = self.view.safeAreaInsets;

    // The run controls sit on the bottom edge, above the home indicator.
    const CGFloat toolbarH = 44.0;
    CGFloat toolbarY = b.size.height - safe.bottom - toolbarH;
    if (toolbarY < 0.0) toolbarY = 0.0;
    _toolbar.frame = CGRectMake(0.0, toolbarY, b.size.width, toolbarH);

    const CGFloat top     = safe.top + 8.0;
    const CGFloat keysH   = [VMButtonBar preferredHeight];
    const CGFloat statsH  = 28.0;
    const CGFloat chrome  = 6.0 + keysH + 6.0 + statsH + 4.0;

    /* Split what is left between the guest's screen and the console. The fixed
     * chrome comes off the top of the calculation so that a cramped screen —
     * a small phone, or this one in landscape — shrinks the picture rather than
     * pushing the console out through the bottom of the view. The band is
     * never allowed to exceed the space that exists, so no two views here can
     * overlap however little room there is. */
    CGFloat freeSpace = toolbarY - top - chrome;
    if (freeSpace < 0.0) freeSpace = 0.0;

    // Fit 320x480 inside the band without distortion. The layer's
    // contentsGravity would do this anyway; doing it here too means the view's
    // own aspect is already correct, so there is no interpretation of
    // contentsScale that can stretch the image — and vm_touch_map() is then
    // working against the same rectangle the picture is drawn in.
    CGFloat band = floor(freeSpace * 0.62);
    if (band < 60.0) band = fmin(60.0, freeSpace);
    CGFloat scale = fmin(b.size.width / (CGFloat)VM_FB_WIDTH,
                         band / (CGFloat)VM_FB_HEIGHT);
    CGFloat w = floor((CGFloat)VM_FB_WIDTH  * scale);
    CGFloat h = floor((CGFloat)VM_FB_HEIGHT * scale);
    _screen.frame = CGRectMake(floor((b.size.width - w) * 0.5),
                               top + floor((band - h) * 0.5), w, h);

    CGFloat y = top + band + 6.0;
    _keys.frame = CGRectMake(0.0, y, b.size.width, keysH);
    y += keysH + 6.0;

    _stats.frame = CGRectMake(14.0, y, b.size.width - 28.0, statsH);
    y += statsH + 4.0;

    CGFloat consoleH = toolbarY - y;
    if (consoleH < 0.0) consoleH = 0.0;
    _console.frame = CGRectMake(0.0, y, b.size.width, consoleH);
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    // The whole screen is black; the default dark clock would be invisible.
    return UIStatusBarStyleLightContent;
}

#pragma mark - Presentation

- (void)tick:(CADisplayLink *)sender {
    (void)sender;

    /* Geometry comes back with the pixels rather than being assumed here. The
     * emulator learns it from whichever CLCD window the guest enabled, and the
     * guest is under no obligation to pick 320x480 — VMFramebufferView also
     * measures touches against it, so an assumption here would be an
     * assumption about where a finger landed. */
    BOOL argb = NO;
    uint32_t fbW = 0, fbH = 0, fbStride = 0;
    if (_frame && [_engine copyFrameInto:_frame
                                capacity:VM_FB_BYTES
                                   width:&fbW
                                  height:&fbH
                                  stride:&fbStride
                                    argb:&argb])
        [_screen presentPixels:_frame
                         width:fbW
                        height:fbH
                        stride:fbStride
                          argb:argb];

    [self appendConsole:[_engine takePendingConsoleText]];
    [self flushConsole];

    // The status line reads as noise if it changes 30 times a second.
    if ((++_ticks % 8) == 0) {
        [self refreshStatusLine];
        // A machine can stop on its own, so the toolbar has to keep asking.
        [self refreshRunControls];
    }
}

/*
 * Two lines. The first is the machine: whether it is paused, and then whatever
 * VMEngine reports about the guest, its rate and this process's footprint. The
 * second is input, which is permanently a statement that it does not work —
 * once there is a touch to report, the coordinate that WOULD have been sent
 * sits next to the reason it was not.
 */
- (void)refreshStatusLine {
    /* One source for the machine's state: the engine. It reports "paused"
     * itself now, so prefixing "paused" here as well would produce
     * "paused · running", which is a contradiction rather than a status. */
    NSString *machine = _engine ? ([_engine statusLine] ?: @"?") : @"no machine";

    /* The touch path's own account of itself. "Delivered" here means the
     * emulated controller ACCEPTED the report — not that the guest acted on
     * it, which this app is in no position to know and will not claim. */
    NSString *reason = _engine ? [_engine touchUnavailableReason]
                               : @"no machine";
    NSString *input;
    if (reason.length == 0) {
        uint64_t delivered = 0;
        [_engine touchCountersQueued:NULL delivered:&delivered
                           coalesced:NULL dropped:NULL];
        input = _haveTouch
            ? [NSString stringWithFormat:
               @"touch %d,%d  ·  %llu accepted by the controller",
               _touchX, _touchY, (unsigned long long)delivered]
            : [NSString stringWithFormat:
               @"touch  ·  %llu accepted by the controller",
               (unsigned long long)delivered];
    } else if (_haveTouch) {
        input = [NSString stringWithFormat:
                 @"touch %d,%d  ·  NOT delivered: %@", _touchX, _touchY, reason];
    } else {
        input = [NSString stringWithFormat:@"input  ·  NOT delivered: %@",
                 reason];
    }

    _stats.text = [NSString stringWithFormat:@"%@\n%@", machine, input];
}

- (void)append:(NSString *)line {
    [self appendConsole:[line stringByAppendingString:@"\n"]];
    [self flushConsole];
}

/* Accumulate only. Pushing the string into the text view is -flushConsole's
 * job, because doing it here would mean doing it from inside the self-tests
 * before the view has been laid out, and thirty times a second afterwards. */
- (void)appendConsole:(NSString *)text {
    if (!text.length) return;

    [_consoleText appendString:text];

    if (_consoleText.length > kConsoleScrollback) {
        NSUInteger excess = _consoleText.length - kConsoleScrollback;
        /* Cut at a line boundary rather than mid-word: the top of the console
         * is then a whole line of the guest's output instead of a fragment
         * that reads as corruption. */
        NSRange newline = [_consoleText rangeOfString:@"\n"
                                              options:0
                                                range:NSMakeRange(excess,
                                                    _consoleText.length - excess)];
        /* Only when something survives it. If the only newline left is the
         * very last character, taking this boundary would delete the entire
         * scrollback and blank the console — a worse outcome than the ragged
         * first line the fallback leaves. */
        if (newline.location != NSNotFound &&
            newline.location + newline.length < _consoleText.length)
            excess = newline.location + newline.length;
        [_consoleText deleteCharactersInRange:NSMakeRange(0, excess)];
    }
    _consoleDirty = YES;
}

/*
 * Replacing a UITextView's text destroys any selection in it, and the guest
 * prints a line per frame forever, so a plain "assign every tick" console
 * cannot be copied from: the selection is gone before a finger reaches Copy.
 * So while something is selected, new output is held in _consoleText and shown
 * when the selection is dropped. Scrollback is still bounded either way.
 */
- (void)flushConsole {
    if (!_consoleDirty) return;
    if (_console.selectedRange.length > 0) return;

    // Only follow the tail if the user has not scrolled up to read something.
    CGFloat slack = _console.contentSize.height - _console.contentOffset.y
                  - _console.bounds.size.height;
    BOOL followTail = (slack < 40.0);
    CGPoint wasAt = _console.contentOffset;

    _console.text = _consoleText;
    _consoleDirty = NO;

    if (followTail && _consoleText.length) {
        [_console scrollRangeToVisible:NSMakeRange(_consoleText.length - 1, 1)];
        return;
    }

    /* Assigning .text scrolls a UITextView back to the top, so without this
     * the "let the user read" branch threw them to the start of a 12000
     * character buffer on the next line of guest output — worse than not
     * having the feature. Clamped because the scrollback may have been
     * trimmed and the content is now shorter.
     *
     * This holds the OFFSET, not the text: a trim removes characters from the
     * front, so a reader parked in a full buffer still sees the text drift up
     * past them. Fixing that properly means measuring the removed text's
     * height, which is not worth the machinery — a terminal scrolling its
     * oldest lines away is behaviour people expect. */
    CGFloat limit = _console.contentSize.height - _console.bounds.size.height;
    if (limit < 0.0) limit = 0.0;
    if (wasAt.y > limit) wasAt.y = limit;
    _console.contentOffset = wasAt;
}

#pragma mark - Input (recorded and shown; never delivered)

- (void)framebufferView:(VMFramebufferView *)view
          touchAtGuestX:(int)x
                 guestY:(int)y
                  phase:(vm_touch_phase_t)phase {
    (void)view;

    _haveTouch = YES;
    _touchX = x;
    _touchY = y;

    // Returns NO today, and the status line says so. Routed through the engine
    // anyway so there is exactly one place that will start returning YES.
    [_engine sendTouchAtGuestX:x y:y phase:phase];
    [self refreshStatusLine];
}

- (void)buttonBar:(VMButtonBar *)bar
  didChangeButton:(VMButton)button
          pressed:(BOOL)pressed {
    (void)bar;

    /* Unreachable while +[VMEngine buttonUnavailableReason] is non-nil, because
     * the bar disables every key. Wired regardless: the day that returns nil,
     * the keys light up and this is already the path they take. */
    [_engine setButton:button pressed:pressed];
    [self refreshStatusLine];
}

#pragma mark - Environment

- (void)reportEnvironment {
    struct utsname u; uname(&u);
    [self append:[NSString stringWithFormat:@"device : %s", u.machine]];
    [self append:[NSString stringWithFormat:@"os     : iOS %@",
                  [[UIDevice currentDevice] systemVersion]]];

    int flags = 0;
    BOOL debugged = (csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) == 0)
                    && (flags & CS_DEBUGGED);
    [self append:[NSString stringWithFormat:@"CS_DEBUGGED : %@", debugged ? @"YES" : @"no"]];

    /* Requesting an RWX page is a useful capability hint on A9, but it is not
     * proof that branching to unsigned memory is safe. Never execute probe
     * code automatically during viewDidLoad: a jailbreak policy mismatch
     * would turn every app launch into the same crash loop. The eventual JIT
     * diagnostics screen can run an explicit, recoverable execution test. */
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    BOOL rwx = (page != MAP_FAILED);
    [self append:[NSString stringWithFormat:@"RWX mmap    : %@",
                  rwx ? @"YES" : @"no"]];
    [self append:[NSString stringWithFormat:
        @"JIT execute : not run at startup (%@)",
        (rwx && debugged) ? @"manual diagnostic required"
                          : @"capability preflight failed"]];
    if (rwx) munmap(page, 4096);

    [self append:[NSString stringWithFormat:@"footprint   : %.1f MB at launch",
                  [VMEngine physFootprintBytes] / 1048576.0]];
}

#pragma mark - Emulator self-tests

// A bare-metal payload: load the UART base from a literal and print "HI\n".
- (void)runUartDemo {
    s5l8900_t m;
    if (!s5l8900_init(&m, 0, 1u << 20)) { [self append:@"[soc] init failed"]; return; }

    const uint32_t payload[] = {
        0xe59f0018u,          // LDR r0,[pc,#24]
        0xe3a01048u,          // MOV r1,#'H'
        0xe5801020u,          // STR r1,[r0,#0x20]   UTXH
        0xe3a01049u,          // MOV r1,#'I'
        0xe5801020u,          // STR r1,[r0,#0x20]
        0xe3a0100au,          // MOV r1,#'\n'
        0xe5801020u,          // STR r1,[r0,#0x20]
        0xeafffffeu,          // B .
        S5L8900_UART0_BASE    // literal
    };
    s5l8900_load(&m, 0, payload, sizeof payload);
    m.cpu.r[15] = 0;

    arm_status_t st = ARM_OK;
    unsigned n = s5l8900_run(&m, 32, &st);
    m.uart0.tx[m.uart0.tx_len] = '\0';

    [self append:[NSString stringWithFormat:@"[uart] guest said: %s", m.uart0.tx]];
    [self append:[NSString stringWithFormat:@"[uart] %u instructions, status %d", n, (int)st]];
    s5l8900_free(&m);
}

// Timer -> VIC -> CPU IRQ -> guest handler -> SUBS pc,lr,#4 back to the loop.
- (void)runInterruptDemo {
    s5l8900_t m;
    if (!s5l8900_init(&m, 0, 1u << 20)) { [self append:@"[soc] init failed"]; return; }

    const uint32_t branch = 0xea000000u | (((0x40u - 0x18u - 8u) / 4u) & 0x00ffffffu);
    s5l8900_load(&m, 0x18, &branch, 4);          // IRQ vector -> 0x40

    const uint32_t handler[] = {
        0xe3a01054u,   // MOV r1,#'T'
        0xe5801020u,   // STR r1,[r0,#0x20]
        0xe3a01000u,   // MOV r1,#0
        0xe58210a4u,   // STR r1,[r2,#0xa4]   stop timer 4
        0xe3a01803u,   // MOV r1,#0x00030000
        0xe58210f4u,   // STR r1,[r2,#0xf4]   acknowledge, as the kernel does
        0xe25ef004u    // SUBS pc,lr,#4
    };
    s5l8900_load(&m, 0x40, handler, sizeof handler);

    const uint32_t spin = 0xeafffffeu;
    s5l8900_load(&m, 0x100, &spin, 4);

    // This self-test exercises device -> controller -> CPU, not the clock
    // ratio, so run the timebase at one tick per instruction to keep it quick.
    m.cpu_hz = m.tb_hz = 1;

    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << S5L8900_IRQ_TIMER);
    m.bus.write32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER4_COUNTBUF, 4);
    m.bus.write32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER4_STATE,
                  TIMER4_STATE_START | TIMER4_STATE_UPDATE);

    m.cpu.r[15] = 0x100;
    m.cpu.r[0]  = S5L8900_UART0_BASE;
    m.cpu.r[2]  = S5L8900_TIMER_BASE;
    m.cpu.cpsr  = ARM_MODE_SYS;                  // IRQs unmasked

    arm_status_t st = ARM_OK;
    s5l8900_run(&m, 200, &st);
    m.uart0.tx[m.uart0.tx_len] = '\0';

    BOOL ok = (strcmp(m.uart0.tx, "T") == 0)
              && ((m.cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS)
              && (m.cpu.r[15] == 0x100);
    [self append:[NSString stringWithFormat:@"[irq]  handler printed \"%s\", resumed pc=%08x  %@",
                  m.uart0.tx, m.cpu.r[15], ok ? @"OK" : @"FAIL"]];
    s5l8900_free(&m);
}

// Prove the MMU translates on-device: map one 1 MB section and walk it.
- (void)runMmuDemo {
    s5l8900_t m;
    if (!s5l8900_init(&m, 0, 1u << 20)) { [self append:@"[soc] init failed"]; return; }

    const uint32_t l1 = 0x4000;
    uint32_t entry = (0x00200000u & 0xfff00000u) | (3u << 10) | 2u;   // section, AP=11
    s5l8900_load(&m, l1 + ((0x80000000u >> 20) << 2), &entry, 4);
    m.cpu.cp15.ttbr0 = l1;
    m.cpu.cp15.dacr  = 1u;
    m.cpu.cp15.sctlr |= ARM_SCTLR_M;

    uint32_t pa = 0;
    uint32_t fsr = arm_mmu_translate(&m.cpu, 0x80001234u, ARM_ACCESS_READ, true, &pa);
    [self append:[NSString stringWithFormat:@"[mmu]  0x80001234 -> 0x%08x (fsr %u)  %@",
                  pa, fsr, (fsr == 0 && pa == 0x00201234u) ? @"OK" : @"FAIL"]];
    s5l8900_free(&m);
}

@end
