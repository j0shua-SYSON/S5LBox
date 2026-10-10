// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMHardwareButton.h"

@implementation VMHardwareButton {
    BOOL _pressed;
    NSUInteger _pressGeneration;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    [self addTarget:self action:@selector(beginPress) forControlEvents:UIControlEventTouchDown];
    [self addTarget:self action:@selector(releasePress) forControlEvents:
        UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(appWillResignActive:)
        name:UIApplicationWillResignActiveNotification object:nil];
    return self;
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)appWillResignActive:(NSNotification *)notification { (void)notification; [self releasePress]; }
- (void)beginPress {
    if (_pressed || !self.enabled) return;
    _pressed = YES;
    ++_pressGeneration;
    self.highlighted = YES;
    if (self.pressChanged) self.pressChanged(YES);
}
- (void)releasePress {
    if (!_pressed) return;
    _pressed = NO;
    ++_pressGeneration;
    self.highlighted = NO;
    if (self.pressChanged) self.pressChanged(NO);
}
- (void)setEnabled:(BOOL)enabled {
    if (!enabled) [self releasePress];
    [super setEnabled:enabled];
    self.alpha = !enabled ? 0.4 : self.highlighted ? 0.55 : 1;
}
- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    self.alpha = !self.enabled ? 0.4 : highlighted ? 0.55 : 1;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (!self.window) [self releasePress];
}
- (BOOL)accessibilityActivate {
    if (!self.enabled) return NO;
    [self beginPress];
    NSUInteger generation = _pressGeneration;
    __weak VMHardwareButton *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        VMHardwareButton *button = weakSelf;
        if (button && button->_pressGeneration == generation) [button releasePress];
    });
    return YES;
}
@end
