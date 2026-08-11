//
//  S5LBox -- stopped-machine guest installation progress screen.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface VMGuestInstallViewController : UIViewController

- (instancetype)initWithInstanceID:(NSString *)instanceID
                        machineName:(NSString *)machineName
    NS_DESIGNATED_INITIALIZER;
- (instancetype)initWithNibName:(NSString * _Nullable)nibNameOrNil
                          bundle:(NSBundle * _Nullable)nibBundleOrNil
    NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder *)coder NS_UNAVAILABLE;

/* Called after a durable image publication, on the main queue. */
@property (nonatomic, copy, nullable) void (^readyHandler)(void);

@end

NS_ASSUME_NONNULL_END
