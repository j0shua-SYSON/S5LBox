//
//  S5LBox — the emulated machine's physical buttons. See VMButtonBar.h.
//
//  WHY THE KEYS ARE DRAWN AT ALL IF THEY CANNOT BE PRESSED
//
//  Because the alternative is worse. Leaving them out entirely says nothing;
//  including them live would be a lie the first time someone pressed Home and
//  the guest did not go anywhere. A visible, disabled key with the reason
//  written under it says exactly what is true: this machine has these five
//  inputs, this emulator does not carry them yet, here is why.
//
//  That is the same discipline as bootkernel's run header, which prints
//  "requested but NOT APPLIED" rather than dropping the row.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMButtonBar.h"

#import <math.h>

static const CGFloat kVMKeyHeight     = 30.0;
static const CGFloat kVMCaptionHeight = 15.0;
static const CGFloat kVMKeyGap        =  6.0;
static const CGFloat kVMCaptionGap    =  3.0;

// Declared up front so every call below is checked against a prototype.
@interface VMButtonBar ()
- (void)keyDown:(UIButton *)sender;
- (void)keyUp:(UIButton *)sender;
- (void)applyAvailability;
@end

@implementation VMButtonBar {
    NSArray<UIButton *> *_keys;
    UILabel             *_caption;
}

@synthesize unavailableReason = _unavailableReason;

+ (CGFloat)preferredHeight {
    return kVMKeyHeight + kVMCaptionGap + kVMCaptionHeight;
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (!self) return nil;

    self.backgroundColor = [UIColor clearColor];

    NSMutableArray<UIButton *> *keys =
        [NSMutableArray arrayWithCapacity:VMButtonCount];
    for (NSUInteger i = 0; i < VMButtonCount; i++) {
        UIButton *key = [UIButton buttonWithType:UIButtonTypeSystem];
        if (!key) continue;
        key.tag = (NSInteger)i;
        [key setTitle:[VMEngine nameForButton:(VMButton)i]
             forState:UIControlStateNormal];
        key.titleLabel.font = [UIFont fontWithName:@"Menlo" size:11]
                              ?: [UIFont systemFontOfSize:11];
        [key setTitleColor:[UIColor colorWithWhite:0.85 alpha:1.0]
                  forState:UIControlStateNormal];
        [key setTitleColor:[UIColor colorWithWhite:0.34 alpha:1.0]
                  forState:UIControlStateDisabled];
        key.layer.borderWidth  = 1.0;
        key.layer.cornerRadius = 5.0;

        /* Down and up separately, because a physical button is a state and not
         * an event: a guest that could read these would want to know how long
         * Home was held, which is how a real device tells a press from a
         * force-restart. Touch-up-outside and cancel release it too, so
         * sliding off a key cannot leave it stuck down. */
        [key addTarget:self action:@selector(keyDown:)
      forControlEvents:UIControlEventTouchDown];
        [key addTarget:self action:@selector(keyUp:)
      forControlEvents:UIControlEventTouchUpInside |
                       UIControlEventTouchUpOutside |
                       UIControlEventTouchCancel];
        [self addSubview:key];
        [keys addObject:key];
    }
    _keys = [keys copy];

    _caption = [[UILabel alloc] initWithFrame:CGRectZero];
    _caption.backgroundColor = [UIColor clearColor];
    _caption.textAlignment = NSTextAlignmentCenter;
    _caption.font = [UIFont fontWithName:@"Menlo" size:9]
                    ?: [UIFont systemFontOfSize:9];
    _caption.adjustsFontSizeToFitWidth = YES;
    _caption.minimumScaleFactor = 0.75;
    [self addSubview:_caption];

    [self applyAvailability];
    return self;
}

- (void)setUnavailableReason:(NSString *)unavailableReason {
    _unavailableReason = [unavailableReason copy];
    [self applyAvailability];
}

- (void)applyAvailability {
    const BOOL usable = (_unavailableReason.length == 0);

    for (UIButton *key in _keys) {
        key.enabled = usable;
        key.layer.borderColor = usable
            ? [UIColor colorWithWhite:0.45 alpha:1.0].CGColor
            : [UIColor colorWithWhite:0.22 alpha:1.0].CGColor;
    }

    if (usable) {
        _caption.text = @"physical buttons";
        _caption.textColor = [UIColor colorWithWhite:0.5 alpha:1.0];
    } else {
        _caption.text = [NSString stringWithFormat:@"buttons disabled: %@",
                         _unavailableReason];
        /* Orange rather than red: this is a stated limitation, not an error
         * that has just happened. */
        _caption.textColor = [UIColor colorWithRed:0.95 green:0.65
                                              blue:0.25 alpha:1.0];
    }
}

- (void)layoutSubviews {
    [super layoutSubviews];

    const CGRect bounds = self.bounds;
    const NSUInteger count = _keys.count;
    if (count == 0 || bounds.size.width <= 0.0) return;

    CGFloat width = floor((bounds.size.width - kVMKeyGap * (CGFloat)(count + 1))
                          / (CGFloat)count);
    if (width < 1.0) width = 1.0;

    CGFloat x = kVMKeyGap;
    for (UIButton *key in _keys) {
        key.frame = CGRectMake(x, 0.0, width, kVMKeyHeight);
        x += width + kVMKeyGap;
    }

    _caption.frame = CGRectMake(kVMKeyGap,
                                kVMKeyHeight + kVMCaptionGap,
                                bounds.size.width - 2.0 * kVMKeyGap,
                                kVMCaptionHeight);
}

- (void)keyDown:(UIButton *)sender {
    [self.delegate buttonBar:self
             didChangeButton:(VMButton)sender.tag
                     pressed:YES];
}

- (void)keyUp:(UIButton *)sender {
    [self.delegate buttonBar:self
             didChangeButton:(VMButton)sender.tag
                     pressed:NO];
}

@end
