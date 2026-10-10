// A momentary guest button: holds, cancelled touches and VoiceOver share one path.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <UIKit/UIKit.h>

@interface VMHardwareButton : UIButton
@property (nonatomic, copy) void (^pressChanged)(BOOL pressed);
- (void)releasePress;
@end
