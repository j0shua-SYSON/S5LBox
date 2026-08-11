//
//  S5LBox -- download and authenticate the pinned guest package set.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^VMGuestPackageDownloadProgress)(uint64_t completed,
                                                uint64_t total,
                                                NSString *stage);
typedef void (^VMGuestPackageDownloadCompletion)(
    NSURL * _Nullable packageDirectory, NSError * _Nullable error);

@interface VMGuestPackageDownloader : NSObject

@property (nonatomic, readonly, getter=isRunning) BOOL running;

/* Completion and progress are always delivered on the main queue. */
- (void)startWithProgress:(VMGuestPackageDownloadProgress)progress
               completion:(VMGuestPackageDownloadCompletion)completion;

/* Cancellation never removes already authenticated cache entries. */
- (void)cancel;

@end

NS_ASSUME_NONNULL_END
