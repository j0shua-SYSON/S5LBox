// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMUSBPairing.h"
#import <TargetConditionals.h>

/* Small DER encoder for generated RSA X.509 certificates, not a general
 * ASN.1 parser. Crypto operations remain in Apple's Security framework. */
static NSData *Bytes(const void *p, NSUInteger n) { return [NSData dataWithBytes:p length:n]; }
static NSData *Join(NSArray<NSData *> *parts) {
    NSMutableData *data = [NSMutableData data];
    for (NSData *part in parts) [data appendData:part];
    return data;
}
static NSData *DER(uint8_t tag, NSData *body) {
    uint8_t prefix[6] = {tag}; NSUInteger count = 2, n = body.length;
    if (n < 128) prefix[1] = (uint8_t)n;
    else {
        unsigned bytes = n > 0xffffff ? 4 : n > 0xffff ? 3 : n > 0xff ? 2 : 1;
        prefix[1] = 0x80 | bytes;
        for (unsigned i = 0; i < bytes; i++) prefix[2 + i] = (uint8_t)(n >> (8 * (bytes - i - 1)));
        count += bytes;
    }
    return Join(@[Bytes(prefix, count), body]);
}
static NSData *Seq(NSArray<NSData *> *parts) { return DER(0x30, Join(parts)); }
static NSData *Number(uint8_t value) { return DER(2, Bytes(&value, 1)); }
static NSData *Algorithm(BOOL signature) {
    uint8_t oid[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,1,1,signature ? 5 : 1};
    return Seq(@[DER(6, Bytes(oid, sizeof oid)), DER(5, [NSData data])]);
}
static NSData *Name(NSString *name) {
    uint8_t cn[] = {0x55,4,3};
    return Seq(@[DER(0x31, Seq(@[DER(6, Bytes(cn, sizeof cn)),
                                DER(12, [name dataUsingEncoding:NSUTF8StringEncoding])]))]);
}
static NSData *PEM(NSData *der) {
    NSString *base64 = [der base64EncodedStringWithOptions:NSDataBase64Encoding64CharacterLineLength |
                                                        NSDataBase64EncodingEndLineWithLineFeed];
    return [[NSString stringWithFormat:@"-----BEGIN CERTIFICATE-----\n%@\n-----END CERTIFICATE-----\n", base64]
            dataUsingEncoding:NSASCIIStringEncoding];
}
static SecKeyRef RSAKey(NSData *data, BOOL privateKey, CFErrorRef *error) {
    NSDictionary *attributes = @{(__bridge id)kSecAttrKeyType:(__bridge id)kSecAttrKeyTypeRSA,
        (__bridge id)kSecAttrKeyClass:(__bridge id)(privateKey ? kSecAttrKeyClassPrivate : kSecAttrKeyClassPublic)};
    return SecKeyCreateWithData((__bridge CFDataRef)data, (__bridge CFDictionaryRef)attributes, error);
}
static NSData *Certificate(SecKeyRef subjectKey, SecKeyRef rootKey, NSString *subject,
                            uint8_t serial, BOOL root, NSError **error) {
    CFErrorRef failure = NULL;
    NSData *publicKey = CFBridgingRelease(SecKeyCopyExternalRepresentation(subjectKey, &failure));
    if (!publicKey) { if (error) *error = CFBridgingRelease(failure); else if (failure) CFRelease(failure); return nil; }
    uint8_t zero = 0, yes = 0xff;
    NSData *spki = Seq(@[Algorithm(NO), DER(3, Join(@[Bytes(&zero, 1), publicKey]))]);
    uint8_t basicOID[] = {0x55,0x1d,0x13}, usageOID[] = {0x55,0x1d,0x0f};
    uint8_t usage[] = {root ? 1 : 5, root ? 0x06 : 0xa0};
    NSData *extensions = DER(0xa3, Seq(@[
        Seq(@[DER(6, Bytes(basicOID, 3)), DER(1, Bytes(&yes, 1)),
              DER(4, Seq(root ? @[DER(1, Bytes(&yes, 1))] : @[]))]),
        Seq(@[DER(6, Bytes(usageOID, 3)), DER(1, Bytes(&yes, 1)), DER(4, DER(3, Bytes(usage, 2)))])
    ]));
    NSData *tbs = Seq(@[DER(0xa0, Number(2)), Number(serial), Algorithm(YES), Name(@"S5LBox Pairing Root"),
        Seq(@[DER(23, [@"000101000000Z" dataUsingEncoding:NSASCIIStringEncoding]),
              DER(23, [@"491231235959Z" dataUsingEncoding:NSASCIIStringEncoding])]),
        Name(subject), spki, extensions]);
    /* SHA-1 here is a legacy guest certificate format, never internet trust.
     * TLS pins the specific guest RSA key instead of accepting arbitrary roots. */
    NSData *signature = CFBridgingRelease(SecKeyCreateSignature(rootKey,
        kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA1, (__bridge CFDataRef)tbs, &failure));
    if (!signature) { if (error) *error = CFBridgingRelease(failure); else if (failure) CFRelease(failure); return nil; }
    return Seq(@[tbs, Algorithm(YES), DER(3, Join(@[Bytes(&zero, 1), signature]))]);
}
static NSError *PairingError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.USBPairing" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}
@implementation VMUSBPairing {
    SecIdentityRef _identity;
    SecKeyRef _deviceKey;
    NSDictionary *_record;
}
- (SecIdentityRef)identity { return _identity; }
- (SecKeyRef)deviceKey { return _deviceKey; }
- (NSDictionary *)record { return _record; }
- (void)dealloc {
    if (_identity) CFRelease(_identity);
    if (_deviceKey) CFRelease(_deviceKey);
}
- (instancetype)initWithInstanceID:(NSString *)identifier devicePublicKey:(NSData *)key error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    NSCharacterSet *allowed = [NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdefABCDEF-"];
    if (!identifier.length || identifier.length > 64 ||
        [identifier rangeOfCharacterFromSet:allowed.invertedSet].location != NSNotFound ||
        !key.length || key.length > 16384) {
        if (error) *error = PairingError(@"Invalid guest pairing identity."); return nil;
    }
    NSString *text = [[NSString alloc] initWithData:key encoding:NSASCIIStringEncoding];
    NSData *der = key;
    if ([text containsString:@"-----BEGIN RSA PUBLIC KEY-----"]) {
        NSMutableString *base64 = [NSMutableString string];
        for (NSString *line in [text componentsSeparatedByCharactersInSet:NSCharacterSet.newlineCharacterSet])
            if (![line hasPrefix:@"-----"]) [base64 appendString:line];
        der = [[NSData alloc] initWithBase64EncodedString:base64 options:0];
    }
    _deviceKey = der ? RSAKey(der, NO, NULL) : NULL;
    if (!_deviceKey) { if (error) *error = PairingError(@"The guest returned an unsupported RSA public key."); return nil; }
    NSData *canonicalKey = CFBridgingRelease(SecKeyCopyExternalRepresentation(_deviceKey, NULL));
    if (!canonicalKey) { if (error) *error = PairingError(@"Could not read the guest public key."); return nil; }
    NSFileManager *fm = NSFileManager.defaultManager;
    NSURL *base = [fm URLForDirectory:NSApplicationSupportDirectory inDomain:NSUserDomainMask
                   appropriateForURL:nil create:YES error:error];
    if (!base) return nil;
    NSURL *folder = [base URLByAppendingPathComponent:@"USBPairing" isDirectory:YES];
    if (![fm createDirectoryAtURL:folder withIntermediateDirectories:YES
                       attributes:@{NSFilePosixPermissions:@0700} error:error]) return nil;
    [folder setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    NSURL *url = [folder URLByAppendingPathComponent:[identifier stringByAppendingPathExtension:@"plist"]];
    NSNumber *size = nil;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    NSData *stored = size && size.unsignedLongLongValue <= 65536 ?
        [NSData dataWithContentsOfURL:url options:0 error:nil] : nil;
    NSDictionary *saved = stored.length <= 65536 ? [NSPropertyListSerialization propertyListWithData:stored ?: NSData.data
        options:NSPropertyListImmutable format:nil error:nil] : nil;
    SecKeyRef hostKey = NULL;
    SecCertificateRef hostCertificate = NULL;
    NSDictionary *record = nil;
    if ([saved isKindOfClass:NSDictionary.class] && [saved[@"DevicePublicKey"] isEqual:canonicalKey] &&
        [saved[@"HostPrivateKey"] isKindOfClass:NSData.class] &&
        [saved[@"HostCertificateDER"] isKindOfClass:NSData.class] &&
        [saved[@"Record"] isKindOfClass:NSDictionary.class] &&
        [saved[@"Record"][@"HostID"] isKindOfClass:NSString.class] &&
        [saved[@"Record"][@"SystemBUID"] isKindOfClass:NSString.class] &&
        [saved[@"Record"][@"RootCertificate"] isKindOfClass:NSData.class] &&
        [saved[@"Record"][@"HostCertificate"] isKindOfClass:NSData.class] &&
        [saved[@"Record"][@"DeviceCertificate"] isKindOfClass:NSData.class]) {
        hostKey = RSAKey(saved[@"HostPrivateKey"], YES, NULL);
        hostCertificate = SecCertificateCreateWithData(NULL, (__bridge CFDataRef)saved[@"HostCertificateDER"]);
        record = saved[@"Record"];
    }
    if (!hostKey || !hostCertificate) {
        if (hostKey) CFRelease(hostKey);
        if (hostCertificate) CFRelease(hostCertificate);
        NSDictionary *attributes = @{(__bridge id)kSecAttrKeyType:(__bridge id)kSecAttrKeyTypeRSA,
                                      (__bridge id)kSecAttrKeySizeInBits:@2048};
        SecKeyRef rootKey = SecKeyCreateRandomKey((__bridge CFDictionaryRef)attributes, NULL);
        hostKey = SecKeyCreateRandomKey((__bridge CFDictionaryRef)attributes, NULL);
        if (!rootKey || !hostKey) {
            if (rootKey) CFRelease(rootKey); if (hostKey) CFRelease(hostKey);
            if (error) *error = PairingError(@"Could not generate a guest pairing key."); return nil;
        }
        SecKeyRef rootPublic = SecKeyCopyPublicKey(rootKey), hostPublic = SecKeyCopyPublicKey(hostKey);
        NSData *rootCert = rootPublic ? Certificate(rootPublic, rootKey, @"S5LBox Pairing Root", 1, YES, error) : nil;
        NSData *hostCert = hostPublic ? Certificate(hostPublic, rootKey, @"S5LBox Pairing Host", 2, NO, error) : nil;
        NSData *deviceCert = Certificate(_deviceKey, rootKey, @"S5LBox Guest", 3, NO, error);
        if (rootPublic) CFRelease(rootPublic); if (hostPublic) CFRelease(hostPublic);
        CFRelease(rootKey);
        NSData *privateKey = CFBridgingRelease(SecKeyCopyExternalRepresentation(hostKey, NULL));
        if (!rootCert || !hostCert || !deviceCert || !privateKey) { CFRelease(hostKey); return nil; }
        record = @{ @"HostID":NSUUID.UUID.UUIDString, @"SystemBUID":NSUUID.UUID.UUIDString,
            @"RootCertificate":PEM(rootCert), @"HostCertificate":PEM(hostCert), @"DeviceCertificate":PEM(deviceCert) };
        NSDictionary *persist = @{ @"DevicePublicKey":canonicalKey, @"HostPrivateKey":privateKey,
            @"HostCertificateDER":hostCert, @"Record":record };
        NSData *encoded = [NSPropertyListSerialization dataWithPropertyList:persist
                              format:NSPropertyListBinaryFormat_v1_0 options:0 error:error];
        NSDataWritingOptions options = NSDataWritingAtomic;
#if TARGET_OS_IPHONE
        options |= NSDataWritingFileProtectionCompleteUntilFirstUserAuthentication;
#endif
        if (!encoded || ![encoded writeToURL:url options:options error:error]) { CFRelease(hostKey); return nil; }
        [fm setAttributes:@{NSFilePosixPermissions:@0600} ofItemAtPath:url.path error:nil];
        hostCertificate = SecCertificateCreateWithData(NULL, (__bridge CFDataRef)hostCert);
    }
    _identity = hostCertificate ? SecIdentityCreate(NULL, hostCertificate, hostKey) : NULL;
    if (hostCertificate) CFRelease(hostCertificate);
    if (hostKey) CFRelease(hostKey);
    if (!_identity) { if (error) *error = PairingError(@"Could not construct the guest TLS identity."); return nil; }
    _record = [record copy];
    return self;
}
@end
