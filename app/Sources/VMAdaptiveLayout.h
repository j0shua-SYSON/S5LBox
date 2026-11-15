// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <UIKit/UIKit.h>
#include <math.h>

// A readable column in a vertically scrolling form, including narrow windows.
static inline void VMConstrainReadableColumn(UIStackView *column, UIScrollView *scroll, CGFloat maximumWidth) {
    NSLayoutConstraint *fill = [column.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-40];
    fill.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.contentLayoutGuide.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
        [column.centerXAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.centerXAnchor],
        [column.widthAnchor constraintLessThanOrEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-40],
        [column.widthAnchor constraintLessThanOrEqualToConstant:maximumWidth], fill
    ]];
}

typedef struct { CGRect screen, dock, console; BOOL verticalDock; } VMEmulatorLayout;
static inline VMEmulatorLayout VMLayoutEmulator(CGRect bounds, UIEdgeInsets safe, BOOL console) {
    CGRect area = UIEdgeInsetsInsetRect(bounds, safe);
    area = CGRectInset(area, MIN(12, MAX(0, area.size.width / 2)), MIN(8, MAX(0, area.size.height / 2)));
    area.size.width = MAX(0, area.size.width); area.size.height = MAX(0, area.size.height);
    VMEmulatorLayout layout = {0};
    // A short floating window cannot hold three vertically stacked controls.
    layout.verticalDock = area.size.width > area.size.height && area.size.height >= 236;
    if (layout.verticalDock) {
        layout.dock = CGRectMake(CGRectGetMaxX(area) - 76, CGRectGetMidY(area) - 118, 76, 236);
        area.size.width = MAX(0, area.size.width - 92);
    } else {
        CGFloat width = MIN(300, area.size.width);
        layout.dock = CGRectMake(CGRectGetMidX(area) - width / 2, CGRectGetMaxY(area) - 76, width, 76);
        area.size.height = MAX(0, area.size.height - 88);
    }
    CGFloat band = console ? floor(area.size.height * 0.62) : area.size.height;
    CGFloat scale = MAX(0, MIN(area.size.width / 320, band / 480));
    layout.screen = CGRectMake(CGRectGetMidX(area) - 160 * scale,
        area.origin.y + (band - 480 * scale) / 2, 320 * scale, 480 * scale);
    layout.console = console ? CGRectMake(area.origin.x, area.origin.y + band + 6,
        area.size.width, MAX(0, area.size.height - band - 6)) : CGRectZero;
    return layout;
}
