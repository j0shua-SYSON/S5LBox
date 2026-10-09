// Native repository metadata and conservative Debian dependency planning.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import <Foundation/Foundation.h>

NSError *VMPackageError(NSString *message);
NSComparisonResult VMPackageVersionCompare(NSString *a, NSString *b);
BOOL VMPackageIdentifierValid(NSString *name);

@interface VMPackageCatalog : NSObject
@property (nonatomic, copy) NSArray<NSDictionary *> *packages;
@property (nonatomic, copy) NSArray<NSDictionary *> *installed;
+ (NSArray<NSDictionary *> *)parse:(NSData *)data error:(NSError **)error;
+ (BOOL)protectedPackage:(NSDictionary *)package;
+ (BOOL)requiresRespring:(NSArray<NSDictionary *> *)changes;
// Dependency-first order. No removals, downgrades, essential changes, or forced
// dependency overrides. Unsupported relationships/cycles fail before download.
- (NSArray<NSDictionary *> *)planInstall:(NSDictionary *)package error:(NSError **)error;
- (BOOL)canRemove:(NSDictionary *)package error:(NSError **)error;
@end
