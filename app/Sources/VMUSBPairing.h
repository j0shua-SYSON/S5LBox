// Per-machine, app-private pairing identity. MIT licensed.
#import <Foundation/Foundation.h>
#import <Security/Security.h>
@interface VMUSBPairing : NSObject
- (instancetype)initWithInstanceID:(NSString *)identifier devicePublicKey:(NSData *)key error:(NSError **)error;
/* Explicit storage root for isolated tests; production uses app-private Library. */
- (instancetype)initWithInstanceID:(NSString *)identifier devicePublicKey:(NSData *)key
                         directory:(NSURL *)directory error:(NSError **)error;
@property (nonatomic, readonly) NSDictionary *record; // public certificates + HostID only
@property (nonatomic, readonly) SecIdentityRef identity;
@property (nonatomic, readonly) SecKeyRef deviceKey;
@end
