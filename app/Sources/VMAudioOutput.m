// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMAudioOutput.h"
#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

/* Render/tap blocks retain storage, not the engine or controller. A callback
 * finishing during graph teardown therefore cannot access freed memory. */
@interface VMAudioStorage : NSObject {
@public vm_audio_buffer_t pcm;
}
@end
@implementation VMAudioStorage
- (instancetype)init {
    if ((self = [super init]) && !vm_audio_buffer_init(&pcm)) return nil;
    return self;
}
@end

@implementation VMAudioOutput {
    VMAudioStorage *_storage;
    AVAudioEngine *_engine;
    NSMutableArray<id> *_observers;
    BOOL _running, _foreground, _interrupted, _microphoneEnabled, _tapInstalled;
    NSUInteger _permissionRequest;
    NSString *_status;
}
- (instancetype)init {
    if (!(self = [super init])) return nil;
    _storage = [VMAudioStorage new];
    if (!_storage) return nil;
    _foreground = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    _status = @"Audio is stopped.";
    _observers = [NSMutableArray array];
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    __weak VMAudioOutput *weakSelf = self;
    for (NSString *name in @[AVAudioSessionRouteChangeNotification,
                             AVAudioSessionMediaServicesWereResetNotification,
                             AVAudioEngineConfigurationChangeNotification,
                             AVAudioSessionInterruptionNotification,
                             UIApplicationWillResignActiveNotification,
                             UIApplicationDidBecomeActiveNotification]) {
        id token = [nc addObserverForName:name object:nil queue:NSOperationQueue.mainQueue
                             usingBlock:^(NSNotification *note) {
            VMAudioOutput *self = weakSelf;
            if (!self) return;
            if ([note.name isEqualToString:AVAudioEngineConfigurationChangeNotification] &&
                (note.object != self->_engine || self->_engine.isRunning)) return;
            if ([note.name isEqualToString:AVAudioSessionRouteChangeNotification] &&
                [note.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue] ==
                AVAudioSessionRouteChangeReasonCategoryChange) return;
            if ([note.name isEqualToString:UIApplicationWillResignActiveNotification]) self->_foreground = NO;
            if ([note.name isEqualToString:UIApplicationDidBecomeActiveNotification]) self->_foreground = YES;
            if ([note.name isEqualToString:AVAudioSessionInterruptionNotification]) {
                BOOL began = [note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] ==
                             AVAudioSessionInterruptionTypeBegan;
                BOOL resume = ([note.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue] &
                               AVAudioSessionInterruptionOptionShouldResume) != 0;
                self->_interrupted = began || !resume;
            }
            [self rebuild];
        }];
        [_observers addObject:token];
    }
    return self;
}
- (vm_audio_buffer_t *)buffer { return _storage ? &_storage->pcm : NULL; }
- (BOOL)microphoneEnabled { return _microphoneEnabled; }
- (NSString *)status { return _status; }
- (void)stopGraph {
    if (!_storage) return;
    atomic_store_explicit(&_storage->pcm.host_running, false, memory_order_release);
    atomic_store_explicit(&_storage->pcm.microphone_enabled, false, memory_order_release);
    atomic_fetch_add_explicit(&_storage->pcm.epoch, 1, memory_order_release);
    AVAudioEngine *old = _engine;
    _engine = nil;
    [old stop];
    if (_tapInstalled) [old.inputNode removeTapOnBus:0];
    _tapInstalled = NO;
}
- (void)rebuild {
    NSAssert(NSThread.isMainThread, @"audio session control must be on main thread");
    [self stopGraph];
    if (!_running || !_foreground || _interrupted) {
        _status = @"Audio is paused.";
        [AVAudioSession.sharedInstance setActive:NO
            withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:nil];
        return;
    }
    AVAudioSession *session = AVAudioSession.sharedInstance;
    NSError *error = nil;
    AVAudioSessionCategoryOptions options = AVAudioSessionCategoryOptionMixWithOthers;
    if (_microphoneEnabled)
        options |= AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetooth;
    BOOL ready = [session setCategory:_microphoneEnabled ? AVAudioSessionCategoryPlayAndRecord : AVAudioSessionCategoryPlayback
                                mode:AVAudioSessionModeDefault options:options error:&error];
    if (ready) {
        [session setPreferredSampleRate:VM_AUDIO_RATE error:nil];
        [session setPreferredIOBufferDuration:0.01 error:nil];
        ready = [session setActive:YES error:&error];
    }
    if (!ready) { _status = error.localizedDescription ?: @"Audio session unavailable."; return; }
    AVAudioEngine *engine = [AVAudioEngine new];
    VMAudioStorage *storage = _storage;
    AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:VM_AUDIO_RATE channels:2];
    AVAudioSourceNode *source = [[AVAudioSourceNode alloc] initWithFormat:format
        renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *time,
                              AVAudioFrameCount count, AudioBufferList *output) {
            (void)time;
            if (output->mNumberBuffers == 2 && output->mBuffers[0].mData && output->mBuffers[1].mData) {
                vm_audio_render(&storage->pcm, output->mBuffers[0].mData,
                                 output->mBuffers[1].mData, count);
                *silence = NO;
            } else {
                for (UInt32 b = 0; b < output->mNumberBuffers; b++)
                    if (output->mBuffers[b].mData)
                        memset(output->mBuffers[b].mData, 0, output->mBuffers[b].mDataByteSize);
                *silence = YES;
            }
            return noErr;
        }];
    [engine attachNode:source];
    [engine connect:source to:engine.mainMixerNode format:format];
    if (_microphoneEnabled) {
        AVAudioInputNode *input = engine.inputNode;
        AVAudioFormat *inputFormat = [input outputFormatForBus:0];
        if (inputFormat.sampleRate > 0 && inputFormat.channelCount > 0) {
            [input installTapOnBus:0 bufferSize:512 format:inputFormat
                            block:^(AVAudioPCMBuffer *buffer, AVAudioTime *when) {
                (void)when;
                float *const *data = buffer.floatChannelData;
                if (!data) return;
                uint32_t rate = (uint32_t)buffer.format.sampleRate;
                if (buffer.format.isInterleaved) {
                    unsigned channels = buffer.format.channelCount;
                    for (unsigned i = 0; i < buffer.frameLength; i++)
                        vm_audio_microphone(&storage->pcm, &data[0][i * channels],
                            channels > 1 ? &data[0][i * channels + 1] : NULL, 1, rate);
                } else vm_audio_microphone(&storage->pcm, data[0],
                    buffer.format.channelCount > 1 ? data[1] : NULL, buffer.frameLength, rate);
            }];
            _tapInstalled = YES;
        }
    }
    _engine = engine;
    [engine prepare];
    if (![engine startAndReturnError:&error]) {
        [self stopGraph];
        _status = error.localizedDescription ?: @"Audio output could not start.";
        return;
    }
    atomic_store_explicit(&_storage->pcm.microphone_enabled, _tapInstalled, memory_order_release);
    atomic_store_explicit(&_storage->pcm.host_running, true, memory_order_release);
    _status = _tapInstalled ? @"Playback and microphone are available." :
        (_microphoneEnabled ? @"Playback is available. No microphone input route." : @"Playback is available. Microphone is off.");
}
- (void)setRunning:(BOOL)running {
    if (!NSThread.isMainThread) {
        dispatch_async(dispatch_get_main_queue(), ^{ [self setRunning:running]; }); return;
    }
    if (_running == running) return;
    _running = running;
    if (running) _interrupted = NO;
    [self rebuild];
}
- (void)setMicrophoneEnabled:(BOOL)enabled completion:(void (^)(BOOL, NSString *))completion {
    NSAssert(NSThread.isMainThread, @"microphone permission UI must be on main thread");
    NSUInteger request = ++_permissionRequest;
    if (!enabled) {
        _microphoneEnabled = NO; [self rebuild];
        if (completion) completion(YES, @""); return;
    }
    __weak VMAudioOutput *weakSelf = self;
    [AVAudioSession.sharedInstance requestRecordPermission:^(BOOL granted) {
        dispatch_async(dispatch_get_main_queue(), ^{
            VMAudioOutput *self = weakSelf;
            if (!self || request != self->_permissionRequest) return;
            self->_microphoneEnabled = granted;
            [self rebuild];
            if (completion) completion(granted, granted ? @"" :
                @"Microphone access is off. You can allow it in iOS Settings > S5LBox > Microphone.");
        });
    }];
}
- (void)dealloc {
    for (id observer in _observers) [NSNotificationCenter.defaultCenter removeObserver:observer];
    [self stopGraph];
}
@end
