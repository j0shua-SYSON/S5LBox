#import "VMPackageRepository.h"
#import "VMPackageCatalog.h"
#import <CommonCrypto/CommonDigest.h>
#include <bzlib.h>
#include <zlib.h>

@implementation VMPackageRepository {
    NSMutableData *_received;
    NSUInteger _limit;
    NSError *_failure;
    dispatch_semaphore_t _finished;
    BOOL _allowHTTP;
}
+ (NSURL *)directory {
    NSURL *base = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *url = [base URLByAppendingPathComponent:@"PackageManager" isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:NULL];
    return url;
}
+ (NSString *)sha256:(NSData *)data {
    unsigned char hash[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(data.bytes,(CC_LONG)data.length,hash);
    NSMutableString *result = [NSMutableString string];
    for (NSUInteger i = 0; i < sizeof hash; i++) [result appendFormat:@"%02x",hash[i]];
    return result;
}
+ (NSString *)checksumField:(NSDictionary *)p {
    for (NSString *field in @[@"SHA256",@"SHA1",@"MD5sum"]) if (p[field]) return field;
    return nil;
}
+ (BOOL)verify:(NSData *)data package:(NSDictionary *)p {
    long long expected=[p[@"Size"] longLongValue];
    if (!data || expected < 1 || expected > 64ll*1024ll*1024ll || data.length != (NSUInteger)expected) return NO;
    NSString *field=[self checksumField:p];
    if ([field isEqual:@"SHA256"]) return [[self sha256:data] isEqual:[p[field] lowercaseString]];
    // Legacy digests detect corruption only; HTTPS is mandatory without SHA256.
    if (![[NSURL URLWithString:p[@"_base"]].scheme.lowercaseString isEqual:@"https"]) return NO;
    unsigned char digest[CC_SHA1_DIGEST_LENGTH]; NSUInteger length=0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if ([field isEqual:@"SHA1"]) { CC_SHA1(data.bytes,(CC_LONG)data.length,digest); length=CC_SHA1_DIGEST_LENGTH; }
    else if ([field isEqual:@"MD5sum"]) { CC_MD5(data.bytes,(CC_LONG)data.length,digest); length=CC_MD5_DIGEST_LENGTH; }
#pragma clang diagnostic pop
    if (!length) return NO;
    NSMutableString *hex=[NSMutableString string]; for (NSUInteger i=0;i<length;i++) [hex appendFormat:@"%02x",digest[i]];
    return [hex isEqual:[p[field] lowercaseString]];
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task
    didReceiveResponse:(NSURLResponse *)response completionHandler:(void (^)(NSURLSessionResponseDisposition))completion {
    (void)session; (void)task;
    NSInteger status = [(NSHTTPURLResponse *)response statusCode];
    if ((!_allowHTTP && ![response.URL.scheme.lowercaseString isEqual:@"https"]) || status != 200 || response.expectedContentLength > (int64_t)_limit) {
        _failure = VMPackageError([NSString stringWithFormat:@"Repository HTTP %ld or response exceeds the size limit.",(long)status]);
        completion(NSURLSessionResponseCancel);
    } else completion(NSURLSessionResponseAllow);
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request
    completionHandler:(void (^)(NSURLRequest *))completion {
    (void)session; (void)task; (void)response;
    if (![request.URL.scheme.lowercaseString isEqual:@"https"] && !(_allowHTTP && [request.URL.scheme.lowercaseString isEqual:@"http"])) {
        _failure = VMPackageError(@"Refused an insecure repository redirect."); completion(nil);
    } else completion(request);
}
- (void)URLSession:(NSURLSession *)session dataTask:(NSURLSessionDataTask *)task didReceiveData:(NSData *)data {
    (void)session;
    if (self.canceled || data.length > _limit - _received.length) {
        _failure = VMPackageError(self.canceled ? @"Canceled." : @"Repository response is too large."); [task cancel];
    } else [_received appendData:data];
}
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error {
    (void)session; (void)task; if (!_failure) _failure = error; dispatch_semaphore_signal(_finished);
}
- (NSData *)fetch:(NSURL *)url limit:(NSUInteger)limit error:(NSError **)error {
    if (NSThread.isMainThread || (![url.scheme.lowercaseString isEqual:@"https"] && !(_allowHTTP && [url.scheme.lowercaseString isEqual:@"http"])) || !url.host.length || url.user || url.password || url.fragment) {
        if (error) *error = VMPackageError(@"Use an HTTPS repository URL without credentials or fragments."); return nil;
    }
    if (self.canceled) { if (error) *error = VMPackageError(@"Canceled."); return nil; }
    _received = [NSMutableData data]; _limit = limit; _failure = nil; _finished = dispatch_semaphore_create(0);
    NSURLSessionConfiguration *config = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    config.URLCache = nil; config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    config.timeoutIntervalForRequest = 30; config.timeoutIntervalForResource = 180;
    NSOperationQueue *queue = [NSOperationQueue new]; queue.maxConcurrentOperationCount = 1;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:config delegate:self delegateQueue:queue];
    NSURLSessionDataTask *task = [session dataTaskWithURL:url]; [task resume];
    NSTimeInterval deadline = NSProcessInfo.processInfo.systemUptime + 185;
    while (dispatch_semaphore_wait(_finished, dispatch_time(DISPATCH_TIME_NOW, 100*NSEC_PER_MSEC))) {
        if (self.canceled || NSProcessInfo.processInfo.systemUptime > deadline) [task cancel];
    }
    [session finishTasksAndInvalidate];
    NSData *result = _failure ? nil : [_received copy]; _received = nil;
    if (!result && error) *error = _failure; return result;
}
- (NSData *)expand:(NSData *)data suffix:(NSString *)suffix error:(NSError **)error {
    if (!suffix.length) return data;
    NSMutableData *out = [NSMutableData dataWithLength:32u*1024u*1024u]; BOOL ok = NO;
    if ([suffix isEqual:@".bz2"]) {
        unsigned int size = (unsigned int)out.length;
        ok = BZ2_bzBuffToBuffDecompress(out.mutableBytes,&size,(char *)data.bytes,(unsigned int)data.length,0,0) == BZ_OK;
        if (ok) out.length = size;
    } else {
        z_stream s = {0}; s.next_in = (Bytef *)data.bytes; s.avail_in = (uInt)data.length;
        s.next_out = out.mutableBytes; s.avail_out = (uInt)out.length;
        if (inflateInit2(&s,15+32) == Z_OK) { ok = inflate(&s,Z_FINISH) == Z_STREAM_END && !s.avail_in; out.length = s.total_out; inflateEnd(&s); }
    }
    if (!ok && error) *error = VMPackageError(@"Invalid compressed index or expanded index exceeds 32 MB.");
    return ok ? out : nil;
}
- (NSArray<NSDictionary *> *)refresh:(NSArray<NSDictionary *> *)sources error:(NSError **)error {
    NSMutableArray *all = [NSMutableArray array];
    for (NSDictionary *source in sources) {
        _allowHTTP=[source[@"allowHTTP"] boolValue];
        if (self.progress) self.progress([NSString stringWithFormat:@"Loading %@",source[@"name"]]);
        NSData *data = nil; NSError *failure = nil;
        for (NSString *suffix in @[@".bz2",@".gz",@""]) {
            NSURL *url = [NSURL URLWithString:[source[@"index"] stringByAppendingFormat:@"Packages%@",suffix]];
            NSData *raw = [self fetch:url limit:32u*1024u*1024u error:&failure];
            if (raw) data = [self expand:raw suffix:suffix error:&failure];
            if (data || self.canceled) break;
        }
        if (!data) { if (error) *error = failure; return nil; }
        NSArray *records = [VMPackageCatalog parse:data error:error]; if (!records) return nil;
        for (NSDictionary *record in records) {
            if (![@[@"iphoneos-arm",@"all"] containsObject:record[@"Architecture"] ?: @""]) continue;
            NSMutableDictionary *p = [record mutableCopy]; p[@"_source"] = source[@"name"]; p[@"_base"] = source[@"base"]; p[@"_allowHTTP"]=@(_allowHTTP);
            [all addObject:p];
        }
    }
    // Atomic replacement: a failed source never erases the last usable index.
    NSData *cache = [NSPropertyListSerialization dataWithPropertyList:all format:NSPropertyListBinaryFormat_v1_0 options:0 error:error];
    if (!cache || ![cache writeToURL:[[self.class directory] URLByAppendingPathComponent:@"catalog.plist"] options:NSDataWritingAtomic error:error]) return nil;
    return all;
}
- (NSURL *)download:(NSDictionary *)p error:(NSError **)error {
    _allowHTTP=[p[@"_allowHTTP"] boolValue];
    NSString *field=[self.class checksumField:p]; NSString *hash = [p[field ?: @""] lowercaseString];
    NSUInteger expected=[field isEqual:@"SHA256"] ? 64 : [field isEqual:@"SHA1"] ? 40 : 32;
    long long size = [p[@"Size"] longLongValue];
    if (hash.length != expected || [hash rangeOfCharacterFromSet:[[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"] invertedSet]].location != NSNotFound || size < 1 || size > 64ll*1024*1024 ||
        (![field isEqual:@"SHA256"] && ![[NSURL URLWithString:p[@"_base"]].scheme.lowercaseString isEqual:@"https"])) {
        if (error) *error = VMPackageError(@"Package requires a valid checksum and a size no greater than 64 MB. Legacy SHA-1/MD5 packages require an HTTPS source."); return nil;
    }
    NSString *key=[self.class sha256:[[NSString stringWithFormat:@"%@:%@:%@",p[@"_base"],field,hash] dataUsingEncoding:NSUTF8StringEncoding]];
    NSURL *cache = [[self.class directory] URLByAppendingPathComponent:[key stringByAppendingString:@".deb"]];
    NSData *bytes = [NSData dataWithContentsOfURL:cache options:NSDataReadingMappedIfSafe error:NULL];
    if ([self.class verify:bytes package:p]) return cache;
    NSString *name = p[@"Filename"];
    NSURL *base = [NSURL URLWithString:p[@"_base"]];
    NSURL *url = name.length ? [NSURL URLWithString:[name stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet] relativeToURL:base].absoluteURL : nil;
    // Repositories cannot turn a Filename into an arbitrary network request.
    if (!url || ![url.host isEqual:base.host] || (![url.scheme isEqual:@"https"] && !(_allowHTTP && [url.scheme isEqual:@"http"])) ||
        [name hasPrefix:@"/"] || [name containsString:@":"] || [[name componentsSeparatedByString:@"/"] containsObject:@".."] || [name containsString:@"%"] || [name containsString:@"\\"]) {
        if (error) *error = VMPackageError(@"Unsafe package download path."); return nil;
    }
    if (self.progress) self.progress([NSString stringWithFormat:@"Downloading %@",p[@"Name"] ?: p[@"Package"]]);
    bytes = [self fetch:url limit:(NSUInteger)size error:error];
    if (!bytes) return nil;
    if (![self.class verify:bytes package:p]) {
        if (error) *error = VMPackageError(@"Package size or checksum did not match the repository. Nothing was installed."); return nil;
    }
    return [bytes writeToURL:cache options:NSDataWritingAtomic error:error] ? cache : nil;
}
@end
