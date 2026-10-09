// S5LBox -- Files-visible IPA storage. MIT licensed.
#import <Foundation/Foundation.h>

@interface VMIPALibrary : NSObject
+ (NSURL *)directoryWithError:(NSError **)error;
+ (NSArray<NSURL *> *)filesWithError:(NSError **)error;
/* Coordinated, bounded copy into IPAs. Never overwrites a user's file. */
+ (NSURL *)importURL:(NSURL *)url error:(NSError **)error;
@end
