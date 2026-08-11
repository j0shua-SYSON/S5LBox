//
//  S5LBox -- VMGuestPackageDownloader. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMGuestPackageDownloader.h"

#import "VMGuestPackageFile.h"
#import "VMGuestPackageManifest.h"

static NSString *const VMGuestPackageDownloadErrorDomain =
    @"com.j0shua.S5LBox.GuestPackageDownload";

typedef NS_ENUM(NSInteger, VMGuestPackageDownloadError) {
    VMGuestPackageDownloadInvalidManifest = 1,
    VMGuestPackageDownloadCacheUnavailable,
    VMGuestPackageDownloadInvalidURL,
    VMGuestPackageDownloadTransport,
    VMGuestPackageDownloadHTTP,
    VMGuestPackageDownloadFile,
    VMGuestPackageDownloadIdentity,
    VMGuestPackageDownloadCancelled
};

@interface VMGuestPackageDownloader ()
    <NSURLSessionDownloadDelegate, NSURLSessionTaskDelegate>
@end

@implementation VMGuestPackageDownloader {
    BOOL _running;
    BOOL _cancelled;
    NSUInteger _index;
    uint64_t _verifiedBytes;
    uint64_t _totalBytes;
    uint64_t _currentBytes;
    NSURL *_directory;
    NSURL *_partialURL;
    NSURLSession *_session;
    NSURLSessionDownloadTask *_task;
    NSOperationQueue *_delegateQueue;
    NSError *_transferError;
    BOOL _downloadPrepared;
    VMGuestPackageDownloadProgress _progress;
    VMGuestPackageDownloadCompletion _completion;
}

- (BOOL)isRunning { return _running; }

static NSError *VMGuestDownloadError(VMGuestPackageDownloadError code,
                                      NSString *description) {
    return [NSError errorWithDomain:VMGuestPackageDownloadErrorDomain
                               code:code
                           userInfo:@{
        NSLocalizedDescriptionKey: description.length ? description
                                                       : @"Package download failed."
    }];
}

static NSString *VMGuestPackageName(const vm_guest_package_t *package) {
    if (!package || !package->filename) return @"package";
    return [NSString stringWithUTF8String:package->filename] ?: @"package";
}

- (void)reportCompleted:(uint64_t)completed stage:(NSString *)stage {
    VMGuestPackageDownloadProgress callback = _progress;
    uint64_t total = _totalBytes;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (callback) callback(completed, total, stage ?: @"Downloading packages");
    });
}

- (void)finishWithDirectory:(NSURL *)directory error:(NSError *)error {
    if (!_running) return;
    _running = NO;
    _task = nil;
    [_session finishTasksAndInvalidate];
    _session = nil;
    VMGuestPackageDownloadCompletion callback = _completion;
    _completion = nil;
    _progress = nil;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (callback) callback(error ? nil : directory, error);
    });
}

- (NSURL *)prepareCacheDirectory:(NSError **)error {
    NSFileManager *files = [NSFileManager defaultManager];
    NSURL *cache = [files URLForDirectory:NSCachesDirectory
                                 inDomain:NSUserDomainMask
                        appropriateForURL:nil
                                   create:YES
                                    error:error];
    if (!cache) return nil;

    uint8_t digest[VM_GUEST_PACKAGE_SHA256_SIZE];
    if (!vm_guest_package_manifest_sha256(digest)) {
        if (error) *error = VMGuestDownloadError(
            VMGuestPackageDownloadInvalidManifest,
            @"The built-in package manifest has no stable identity.");
        return nil;
    }
    NSMutableString *identity = [NSMutableString stringWithCapacity:16u];
    for (NSUInteger i = 0u; i < 8u; i++)
        [identity appendFormat:@"%02x", digest[i]];
    NSURL *directory = [cache URLByAppendingPathComponent:
        [@"GuestPackages-" stringByAppendingString:identity]
                                             isDirectory:YES];
    if (![files createDirectoryAtURL:directory
          withIntermediateDirectories:YES attributes:nil error:error])
        return nil;
    return directory;
}

- (BOOL)verifyPackage:(const vm_guest_package_t *)package
                 atURL:(NSURL *)url
                detail:(NSString **)detail {
    char why[256];
    uint64_t size = 0u;
    vm_guest_package_file_status_t status = vm_guest_package_file_verify(
        package, url.fileSystemRepresentation, &size, why, sizeof why);
    if (status == VM_GUEST_PACKAGE_FILE_OK) return YES;
    if (detail) {
        NSString *reason = why[0] ? [NSString stringWithUTF8String:why] : nil;
        *detail = reason ?: [NSString stringWithUTF8String:
            vm_guest_package_file_status_text(status)];
    }
    return NO;
}

- (void)advance {
    NSAssert(![NSThread isMainThread], @"package I/O belongs off the main thread");
    if (_cancelled) {
        [self finishWithDirectory:nil error:VMGuestDownloadError(
            VMGuestPackageDownloadCancelled, @"The package download was cancelled.")];
        return;
    }

    NSFileManager *files = [NSFileManager defaultManager];
    while (_index < vm_guest_package_count()) {
        const vm_guest_package_t *package = vm_guest_package_at(_index);
        NSString *filename = VMGuestPackageName(package);
        NSURL *final = [_directory URLByAppendingPathComponent:filename
                                                   isDirectory:NO];
        if ([files fileExistsAtPath:final.path]) {
            NSString *why = nil;
            if ([self verifyPackage:package atURL:final detail:&why]) {
                _verifiedBytes += package->size;
                _index++;
                [self reportCompleted:_verifiedBytes
                                stage:[@"Verified " stringByAppendingString:filename]];
                continue;
            }
            NSError *removeError = nil;
            if (![files removeItemAtURL:final error:&removeError]) {
                [self finishWithDirectory:nil error:VMGuestDownloadError(
                    VMGuestPackageDownloadFile,
                    [NSString stringWithFormat:
                        @"The invalid cached %@ could not be replaced: %@",
                        filename, removeError.localizedDescription])];
                return;
            }
        }

        NSString *source = package && package->source_url
            ? [NSString stringWithUTF8String:package->source_url] : nil;
        NSURL *url = source ? [NSURL URLWithString:source] : nil;
        if (!url || ![url.scheme.lowercaseString isEqualToString:@"https"]) {
            [self finishWithDirectory:nil error:VMGuestDownloadError(
                VMGuestPackageDownloadInvalidURL,
                [NSString stringWithFormat:@"%@ has no valid HTTPS source.", filename])];
            return;
        }

        NSURL *partial = [_directory URLByAppendingPathComponent:
            [filename stringByAppendingString:@".partial"] isDirectory:NO];
        if ([files fileExistsAtPath:partial.path] &&
            ![files removeItemAtURL:partial error:NULL]) {
            [self finishWithDirectory:nil error:VMGuestDownloadError(
                VMGuestPackageDownloadFile,
                [NSString stringWithFormat:
                    @"The incomplete %@ download could not be cleared.", filename])];
            return;
        }

        NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url];
        request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
        request.timeoutInterval = 90.0;
        [request setValue:@"S5LBox/1" forHTTPHeaderField:@"User-Agent"];
        _partialURL = partial;
        _currentBytes = 0u;
        _transferError = nil;
        _downloadPrepared = NO;
        _task = [_session downloadTaskWithRequest:request];
        [self reportCompleted:_verifiedBytes
                        stage:[@"Downloading " stringByAppendingString:filename]];
        [_task resume];
        return;
    }

    [self reportCompleted:_totalBytes stage:@"Packages verified"];
    [self finishWithDirectory:_directory error:nil];
}

- (void)startWithProgress:(VMGuestPackageDownloadProgress)progress
               completion:(VMGuestPackageDownloadCompletion)completion {
    NSParameterAssert(completion != nil);
    if (_running) {
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(nil, VMGuestDownloadError(
                VMGuestPackageDownloadTransport,
                @"A package download is already running."));
        });
        return;
    }

    char why[256];
    if (!vm_guest_package_manifest_validate(why, sizeof why)) {
        NSString *reason = why[0] ? [NSString stringWithUTF8String:why] : nil;
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(nil, VMGuestDownloadError(
                VMGuestPackageDownloadInvalidManifest,
                reason ?: @"The built-in package manifest is invalid."));
        });
        return;
    }

    _running = YES;
    _cancelled = NO;
    _index = 0u;
    _verifiedBytes = 0u;
    _currentBytes = 0u;
    _totalBytes = vm_guest_package_total_download_bytes();
    _progress = [progress copy];
    _completion = [completion copy];

    _delegateQueue = [[NSOperationQueue alloc] init];
    _delegateQueue.name = @"com.j0shua.S5LBox.GuestPackageDownload";
    _delegateQueue.maxConcurrentOperationCount = 1;
    NSURLSessionConfiguration *configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.HTTPMaximumConnectionsPerHost = 1;
    configuration.timeoutIntervalForRequest = 90.0;
    configuration.timeoutIntervalForResource = 600.0;
    configuration.waitsForConnectivity = YES;
    _session = [NSURLSession sessionWithConfiguration:configuration
                                             delegate:self
                                        delegateQueue:_delegateQueue];

    __weak VMGuestPackageDownloader *weakSelf = self;
    [_delegateQueue addOperationWithBlock:^{
        VMGuestPackageDownloader *self_ = weakSelf;
        if (!self_) return;
        NSError *error = nil;
        self_->_directory = [self_ prepareCacheDirectory:&error];
        if (!self_->_directory) {
            [self_ finishWithDirectory:nil error:VMGuestDownloadError(
                VMGuestPackageDownloadCacheUnavailable,
                error.localizedDescription ?: @"The package cache is unavailable.")];
            return;
        }
        [self_ advance];
    }];
}

- (void)cancel {
    if (!_running) return;
    __weak VMGuestPackageDownloader *weakSelf = self;
    [_delegateQueue addOperationWithBlock:^{
        VMGuestPackageDownloader *self_ = weakSelf;
        if (!self_ || !self_->_running) return;
        self_->_cancelled = YES;
        if (self_->_task)
            [self_->_task cancel];
        else
            [self_ advance];
    }];
}

#pragma mark - NSURLSession

- (void)URLSession:(NSURLSession *)session
        task:(NSURLSessionTask *)task
 willPerformHTTPRedirection:(NSHTTPURLResponse *)response
         newRequest:(NSURLRequest *)request
  completionHandler:(void (^)(NSURLRequest * _Nullable))completionHandler {
    (void)session; (void)task; (void)response;
    if ([request.URL.scheme.lowercaseString isEqualToString:@"https"])
        completionHandler(request);
    else
        completionHandler(nil);
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
      didWriteData:(int64_t)bytesWritten
 totalBytesWritten:(int64_t)totalBytesWritten
totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
    (void)session; (void)downloadTask; (void)bytesWritten;
    (void)totalBytesExpectedToWrite;
    const vm_guest_package_t *package = vm_guest_package_at(_index);
    uint64_t amount = totalBytesWritten > 0 ? (uint64_t)totalBytesWritten : 0u;
    if (package && amount > package->size) amount = package->size;
    _currentBytes = amount;
    [self reportCompleted:_verifiedBytes + amount
                    stage:[@"Downloading " stringByAppendingString:
                           VMGuestPackageName(package)]];
}

- (void)URLSession:(NSURLSession *)session
      downloadTask:(NSURLSessionDownloadTask *)downloadTask
didFinishDownloadingToURL:(NSURL *)location {
    (void)session;
    const vm_guest_package_t *package = vm_guest_package_at(_index);
    NSString *filename = VMGuestPackageName(package);
    NSHTTPURLResponse *response = (NSHTTPURLResponse *)downloadTask.response;
    if (![response isKindOfClass:[NSHTTPURLResponse class]] ||
        response.statusCode != 200) {
        _transferError = VMGuestDownloadError(
            VMGuestPackageDownloadHTTP,
            [NSString stringWithFormat:@"%@ returned HTTP %ld.", filename,
             (long)(response ? response.statusCode : 0)]);
        return;
    }
    if (![response.URL.scheme.lowercaseString isEqualToString:@"https"]) {
        _transferError = VMGuestDownloadError(
            VMGuestPackageDownloadHTTP,
            [NSString stringWithFormat:@"%@ left HTTPS during download.", filename]);
        return;
    }

    NSFileManager *files = [NSFileManager defaultManager];
    NSError *moveError = nil;
    if (![files moveItemAtURL:location toURL:_partialURL error:&moveError]) {
        _transferError = VMGuestDownloadError(
            VMGuestPackageDownloadFile,
            [NSString stringWithFormat:@"%@ could not be staged: %@", filename,
             moveError.localizedDescription]);
        return;
    }

    NSString *identityDetail = nil;
    if (![self verifyPackage:package atURL:_partialURL detail:&identityDetail]) {
        (void)[files removeItemAtURL:_partialURL error:NULL];
        _transferError = VMGuestDownloadError(
            VMGuestPackageDownloadIdentity,
            [NSString stringWithFormat:@"%@ was rejected: %@", filename,
             identityDetail ?: @"identity mismatch"]);
        return;
    }

    NSURL *final = [_directory URLByAppendingPathComponent:filename
                                                isDirectory:NO];
    if (![files moveItemAtURL:_partialURL toURL:final error:&moveError]) {
        _transferError = VMGuestDownloadError(
            VMGuestPackageDownloadFile,
            [NSString stringWithFormat:@"%@ could not be published: %@", filename,
             moveError.localizedDescription]);
        return;
    }
    _downloadPrepared = YES;
}

- (void)URLSession:(NSURLSession *)session
              task:(NSURLSessionTask *)task
didCompleteWithError:(NSError *)error {
    (void)session; (void)task;
    _task = nil;
    if (_cancelled) {
        [self finishWithDirectory:nil error:VMGuestDownloadError(
            VMGuestPackageDownloadCancelled, @"The package download was cancelled.")];
        return;
    }
    NSError *failure = _transferError ?: error;
    if (!failure && !_downloadPrepared)
        failure = VMGuestDownloadError(VMGuestPackageDownloadTransport,
                                       @"The package download did not produce a file.");
    if (failure) {
        if (_partialURL) (void)[[NSFileManager defaultManager]
            removeItemAtURL:_partialURL error:NULL];
        [self finishWithDirectory:nil error:failure];
        return;
    }

    const vm_guest_package_t *package = vm_guest_package_at(_index);
    if (package) _verifiedBytes += package->size;
    _index++;
    _currentBytes = 0u;
    _partialURL = nil;
    _transferError = nil;
    _downloadPrepared = NO;
    [self advance];
}

@end
