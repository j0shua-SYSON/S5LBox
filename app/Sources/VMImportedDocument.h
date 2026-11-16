// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN
// Own only a temporary copy returned by an import-mode document picker.
// Retain through asynchronous consumption (and firmware retries). Releasing
// the last owner removes that copy, never an external or library original.
@interface VMImportedDocument : NSObject
@property (nonatomic, strong, readonly) NSURL *url;
- (instancetype)initWithPickerURL:(NSURL *)url;
@end
NS_ASSUME_NONNULL_END
