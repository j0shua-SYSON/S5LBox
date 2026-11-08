/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
// Minimal public ABI declarations, like the other community-SDK fixtures.
typedef signed char BOOL;
#define NO 0
#define nil ((id)0)
typedef uint32_t UInt32;
typedef uint64_t UInt64;
typedef int32_t OSStatus;
typedef const void *CFURLRef;
typedef void *AudioFileID;
typedef void *AudioQueueRef;
typedef void *ExtAudioFileRef;
typedef struct { double mSampleRate; UInt32 mFormatID,mFormatFlags,mBytesPerPacket,mFramesPerPacket,mBytesPerFrame,mChannelsPerFrame,mBitsPerChannel,mReserved; } AudioStreamBasicDescription;
typedef struct { int64_t mStartOffset; UInt32 mVariableFramesInPacket,mDataByteSize; } AudioStreamPacketDescription;
typedef struct { UInt32 mAudioDataBytesCapacity; void *mAudioData; UInt32 mAudioDataByteSize; void *mUserData; UInt32 mPacketDescriptionCapacity; AudioStreamPacketDescription *mPacketDescriptions; UInt32 mPacketDescriptionCount; } AudioQueueBuffer;
typedef AudioQueueBuffer *AudioQueueBufferRef;
typedef struct { UInt32 mNumberChannels,mDataByteSize; void *data; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
enum { kAudioFileReadPermission=1, kAudioFormatMPEGLayer3=0x2e6d7033, kAudioFormatLinearPCM=0x6c70636d,
    kAudioFormatFlagIsSignedInteger=4, kAudioFormatFlagIsPacked=8, kAudioFilePropertyDataFormat=0x64666d74,
    kAudioFilePropertyAudioDataPacketCount=0x70636e74, kAudioFilePropertyPacketSizeUpperBound=0x706b7562,
    kExtAudioFileProperty_ClientDataFormat=0x63666d74, eofErr=-39 };
extern void *CFRunLoopGetCurrent(void);
extern id kCFRunLoopCommonModes, NSDefaultRunLoopMode;
extern OSStatus AudioFileOpenURL(CFURLRef,signed char,UInt32,AudioFileID *);
extern OSStatus AudioFileGetProperty(AudioFileID,UInt32,UInt32 *,void *);
extern OSStatus AudioFileReadPackets(AudioFileID,bool,UInt32 *,AudioStreamPacketDescription *,int64_t,UInt32 *,void *);
extern OSStatus AudioFileReadPacketData(AudioFileID,bool,UInt32 *,AudioStreamPacketDescription *,int64_t,UInt32 *,void *);
extern OSStatus AudioFileClose(AudioFileID);
extern OSStatus AudioQueueNewOutput(const AudioStreamBasicDescription *,void (*)(void *,AudioQueueRef,AudioQueueBufferRef),void *,void *,id,UInt32,AudioQueueRef *);
extern OSStatus AudioQueueAllocateBufferWithPacketDescriptions(AudioQueueRef,UInt32,UInt32,AudioQueueBufferRef *);
extern OSStatus AudioQueueEnqueueBuffer(AudioQueueRef,AudioQueueBufferRef,UInt32,const AudioStreamPacketDescription *);
extern OSStatus AudioQueueStart(AudioQueueRef,const void *);
extern OSStatus AudioQueuePause(AudioQueueRef);
extern OSStatus AudioQueueReset(AudioQueueRef);
extern OSStatus AudioQueueStop(AudioQueueRef,bool);
extern OSStatus AudioQueueDispose(AudioQueueRef,bool);
extern OSStatus ExtAudioFileOpenURL(CFURLRef,ExtAudioFileRef *);
extern OSStatus ExtAudioFileSetProperty(ExtAudioFileRef,UInt32,UInt32,const void *);
extern OSStatus ExtAudioFileRead(ExtAudioFileRef,UInt32 *,AudioBufferList *);
extern OSStatus ExtAudioFileDispose(ExtAudioFileRef);
extern OSStatus AudioServicesCreateSystemSoundID(CFURLRef,UInt32 *);
extern OSStatus AudioServicesAddSystemSoundCompletion(UInt32,void *,id,void (*)(UInt32,void *),void *);
extern void AudioServicesRemoveSystemSoundCompletion(UInt32);
extern void AudioServicesPlaySystemSound(UInt32);
extern OSStatus AudioServicesDisposeSystemSoundID(UInt32);
__attribute__((objc_root_class)) @interface NSObject { Class isa; }
+ (id)new; + (id)alloc; - (void)release;
@end
@interface NSAutoreleasePool : NSObject - (void)drain; @end
@interface NSString : NSObject - (id)stringByAppendingPathComponent:(id)component; @end
extern NSString *NSTemporaryDirectory(void);
@interface NSMutableData : NSObject
+ (id)data; - (void)appendBytes:(const void *)bytes length:(unsigned)length;
- (BOOL)writeToFile:(id)path atomically:(BOOL)atomic;
@end
@interface NSURL : NSObject + (id)fileURLWithPath:(id)path; @end
@interface NSDate : NSObject + (id)dateWithTimeIntervalSinceNow:(double)interval; - (double)timeIntervalSinceNow; @end
@interface NSRunLoop : NSObject + (id)currentRunLoop; - (void)runUntilDate:(id)date; @end
@interface NSFileManager : NSObject + (id)defaultManager; - (BOOL)removeItemAtPath:(id)path error:(id *)error; @end
@interface AVAudioPlayer : NSObject
- (id)initWithContentsOfURL:(id)url error:(id *)error;
- (id)initWithData:(id)data error:(id *)error;
- (BOOL)prepareToPlay; - (BOOL)play; - (void)stop;
@end

static int failures, completions;
static int sound_completions;
static bool dispose_on_callback;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
static void completed(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    (void)context; (void)queue; (void)buffer; completions++;
    if (dispose_on_callback) CHECK(AudioQueueDispose(queue, false) == 0);
}
static void sound_completed(UInt32 sound, void *context) {
    CHECK(context == &sound_completions);
    sound_completions++;
    AudioServicesRemoveSystemSoundCompletion(sound);
}
static void pump(double seconds) {
    NSDate *end = [NSDate dateWithTimeIntervalSinceNow:seconds];
    [[NSRunLoop currentRunLoop] runUntilDate:end];
}
int main(void) {
    setbuf(stdout, NULL);
    puts("AUDIO fixture entered");
    NSAutoreleasePool *pool = [NSAutoreleasePool new];
    // Self-generated silent MPEG-1 Layer III frames: zero-length granules and
    // zero side information, alternating 128/160 kbps. No third-party media.
    NSMutableData *data = [NSMutableData data];
    unsigned char frame[522] = {0xff, 0xfb, 0x90, 0};
    for (int i = 0; i < 12; i++) {
        frame[2] = (i & 1) ? 0xa0 : 0x90;
        [data appendBytes:frame length:(i & 1) ? 522 : 417];
    }
    NSString *path = [NSTemporaryDirectory() stringByAppendingPathComponent:@"packet-fixture.mp3"];
    CHECK([data writeToFile:path atomically:NO]);
    NSURL *url = [NSURL fileURLWithPath:path];
    AudioFileID file = NULL;
    CHECK(AudioFileOpenURL((CFURLRef)url, kAudioFileReadPermission, 0, &file) == 0);
    puts("AUDIO file opened");
    if (!file) return 1;
    AudioStreamBasicDescription format = {0}; UInt32 size = sizeof(format);
    CHECK(AudioFileGetProperty(file, kAudioFilePropertyDataFormat, &size, &format) == 0);
    CHECK(format.mFormatID == kAudioFormatMPEGLayer3 && format.mBytesPerPacket == 0);
    CHECK(format.mFramesPerPacket == 1152 && format.mSampleRate == 44100 && format.mChannelsPerFrame == 2);
    UInt64 total = 0; size = sizeof(total);
    CHECK(AudioFileGetProperty(file, kAudioFilePropertyAudioDataPacketCount, &size, &total) == 0 && total == 12);
    UInt32 maximum = 0; size = sizeof(maximum);
    CHECK(AudioFileGetProperty(file, kAudioFilePropertyPacketSizeUpperBound, &size, &maximum) == 0 && maximum == 522);
    unsigned char bytes[6000]; memset(bytes, 0xa5, sizeof(bytes));
    AudioStreamPacketDescription descriptions[12];
    UInt32 count = 12, length = 500;
    CHECK(AudioFileReadPacketData(file, false, &length, descriptions, 0, &count, bytes) == 0);
    CHECK(count == 1 && length == 417 && bytes[417] == 0xa5);
    CHECK(descriptions[0].mStartOffset == 0 && descriptions[0].mDataByteSize == 417 && descriptions[0].mVariableFramesInPacket == 0);
    count = 12; length = 0;
    CHECK(AudioFileReadPackets(file, false, &length, descriptions, 0, &count, bytes) == 0);
    CHECK(count == 12 && length == 5634 && descriptions[1].mStartOffset == 417 && descriptions[1].mDataByteSize == 522);
    count = 2; length = sizeof(bytes);
    CHECK(AudioFileReadPacketData(file, false, &length, descriptions, 11, &count, bytes) == eofErr);
    CHECK(count == 1 && length == 522);
    count = 1; length = sizeof(bytes);
    CHECK(AudioFileReadPacketData(file, false, &length, descriptions, 12, &count, bytes) == eofErr && count == 0 && length == 0);

    AudioQueueRef queue = NULL;
    puts("AUDIO packet reads complete");
    CHECK(AudioQueueNewOutput(&format, completed, NULL, CFRunLoopGetCurrent(), kCFRunLoopCommonModes, 0, &queue) == 0);
    AudioQueueBufferRef buffers[2];
    for (int i = 0; i < 2; i++) {
        CHECK(AudioQueueAllocateBufferWithPacketDescriptions(queue, 3000, 6, &buffers[i]) == 0);
        CHECK(buffers[i]->mPacketDescriptionCapacity == 6 && buffers[i]->mPacketDescriptions != NULL);
        count = 6; length = 3000;
        CHECK(AudioFileReadPacketData(file, false, &length, buffers[i]->mPacketDescriptions, i * 6, &count, buffers[i]->mAudioData) == 0);
        buffers[i]->mAudioDataByteSize = length;
        buffers[i]->mPacketDescriptionCount = count;
        CHECK(AudioQueueEnqueueBuffer(queue, buffers[i], 0, NULL) == 0);
        CHECK(AudioQueueEnqueueBuffer(queue, buffers[i], 0, NULL) != 0);
    }
    CHECK(AudioQueueStart(queue, NULL) == 0);
    CHECK(AudioQueuePause(queue) == 0);
    pump(0.1); CHECK(completions == 0);
    CHECK(AudioQueueStart(queue, NULL) == 0);
    pump(0.7); CHECK(completions == 2);
    CHECK(AudioQueueReset(queue) == 0);
    CHECK(AudioQueueEnqueueBuffer(queue, buffers[0], 0, NULL) == 0);
    CHECK(AudioQueueStart(queue, NULL) == 0);
    pump(0.4); CHECK(completions == 3);
    CHECK(AudioQueueStop(queue, true) == 0);
    CHECK(AudioQueueDispose(queue, true) == 0);
    puts("AUDIO queue playback complete");

    // Disposal can be requested during playback or from an output callback.
    // The caller may free its context immediately: no later callback is legal.
    for (int scenario=0; scenario<3; scenario++) {
        CHECK(AudioQueueNewOutput(&format,completed,NULL,CFRunLoopGetCurrent(),kCFRunLoopCommonModes,0,&queue)==0);
        for (int i=0; i<2; i++) {
            CHECK(AudioQueueAllocateBufferWithPacketDescriptions(queue,3000,6,&buffers[i])==0);
            count=6; length=3000;
            CHECK(AudioFileReadPacketData(file,false,&length,buffers[i]->mPacketDescriptions,i*6,&count,buffers[i]->mAudioData)==0);
            buffers[i]->mAudioDataByteSize=length;
            buffers[i]->mPacketDescriptionCount=count;
            CHECK(AudioQueueEnqueueBuffer(queue,buffers[i],0,NULL)==0);
        }
        int before=completions;
        dispose_on_callback=scenario==1;
        if (scenario!=2) CHECK(AudioQueueStart(queue,NULL)==0);
        if (!dispose_on_callback) CHECK(AudioQueueDispose(queue,false)==0);
        pump(0.7);
        CHECK(completions==before+(dispose_on_callback ? 1 : 0));
        dispose_on_callback=false;
        pump(0.1);
        CHECK(completions==before+(scenario==1 ? 1 : 0));
    }
    puts("AUDIO asynchronous disposal complete");

    ExtAudioFileRef ext = NULL;
    CHECK(ExtAudioFileOpenURL((CFURLRef)url, &ext) == 0);
    AudioStreamBasicDescription pcm = {44100, kAudioFormatLinearPCM, kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, 4, 1, 4, 2, 16, 0};
    CHECK(ExtAudioFileSetProperty(ext, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm) == 0);
    unsigned char samples[60000];
    AudioBufferList output = {1, {{2, sizeof(samples), samples}}};
    UInt32 frames = 15000;
    CHECK(ExtAudioFileRead(ext, &frames, &output) == 0 && frames == 12 * 1152);
    CHECK(output.mBuffers[0].mDataByteSize == frames * 4);
    frames = 10; output.mBuffers[0].mDataByteSize = sizeof(samples);
    CHECK(ExtAudioFileRead(ext, &frames, &output) == 0 && frames == 0);
    CHECK(ExtAudioFileDispose(ext) == 0);
    puts("AUDIO extended decode complete");
    UInt32 sound = 0;
    CHECK(AudioServicesCreateSystemSoundID((CFURLRef)url, &sound) == 0);
    CHECK(AudioServicesAddSystemSoundCompletion(sound, NULL, nil, sound_completed, &sound_completions) == 0);
    AudioServicesPlaySystemSound(sound);
    CHECK(sound_completions == 0);
    pump(0.7); CHECK(sound_completions == 1);
    AudioServicesPlaySystemSound(sound);
    pump(0.7); CHECK(sound_completions == 1);
    CHECK(AudioServicesDisposeSystemSoundID(sound) == 0);
    AVAudioPlayer *player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:NULL];
    CHECK(player != nil && [player prepareToPlay] && [player play]);
    pump(0.7); [player stop]; [player release];
    CHECK(AudioFileClose(file) == 0);
    [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
    CHECK([[AVAudioPlayer alloc] initWithContentsOfURL:url error:NULL] == nil);
    CHECK([[AVAudioPlayer alloc] initWithData:[NSMutableData data] error:NULL] == nil);
    // MPEG-2 uses 576-frame packets, and this stream also exercises ID3 removal
    // and mono decoding at the lower sample rate used by older game music.
    data = [NSMutableData data];
    unsigned char tag[10] = {'I','D','3',4,0,0,0,0,0,0};
    [data appendBytes:tag length:sizeof(tag)];
    unsigned char low[208] = {0xff,0xf3,0x80,0xc0};
    for (int i=0; i<12; i++) [data appendBytes:low length:sizeof(low)];
    CHECK([data writeToFile:path atomically:NO]);
    CHECK(AudioFileOpenURL((CFURLRef)url,kAudioFileReadPermission,0,&file)==0);
    size=sizeof(format);
    CHECK(AudioFileGetProperty(file,kAudioFilePropertyDataFormat,&size,&format)==0);
    CHECK(format.mFormatID==kAudioFormatMPEGLayer3 && format.mFramesPerPacket==576 && format.mSampleRate==22050 && format.mChannelsPerFrame==1);
    CHECK(AudioFileClose(file)==0);
    CHECK(ExtAudioFileOpenURL((CFURLRef)url,&ext)==0);
    pcm.mSampleRate=22050; pcm.mChannelsPerFrame=1; pcm.mBytesPerPacket=2; pcm.mBytesPerFrame=2;
    CHECK(ExtAudioFileSetProperty(ext,kExtAudioFileProperty_ClientDataFormat,sizeof(pcm),&pcm)==0);
    frames=15000; output.mBuffers[0].mDataByteSize=sizeof(samples);
    CHECK(ExtAudioFileRead(ext,&frames,&output)==0 && frames==12*576);
    CHECK(output.mBuffers[0].mDataByteSize==frames*2);
    CHECK(ExtAudioFileDispose(ext)==0);
    [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
    printf("S5LBOX-HLE-AUDIO %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    [pool drain]; return failures ? 1 : 0;
}
