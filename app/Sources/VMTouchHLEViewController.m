// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
// Kept out of the normal app until the integrated feature passes device tests.
#if S5LBOX_TOUCHHLE
#import "VMTouchHLEViewController.h"
#import "VMTouchHLESurface.h"
#import "VMTouchHLEFiles.h"
#import "VMGuestApplications.h"
#import "VMPackageBridge.h"
#import "VMUSBService.h"
#import <CoreMotion/CoreMotion.h>
#include <math.h>
#include <errno.h>

static NSString *HLEChoiceKey(NSString *instance) { return [@"TouchHLEApps-" stringByAppendingString:instance]; }
static NSArray<NSString *> *HLEChoices(NSString *instance) {
    NSArray *saved=[NSUserDefaults.standardUserDefaults arrayForKey:HLEChoiceKey(instance)];
    NSMutableSet *valid=[NSMutableSet set];
    NSCharacterSet *bad=[[NSCharacterSet characterSetWithCharactersInString:
        @"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-"] invertedSet];
    for (id name in saved) if ([name isKindOfClass:NSString.class] && [name length] &&
        [name length]<=255 && [name rangeOfCharacterFromSet:bad].location==NSNotFound && valid.count<128) [valid addObject:name];
    return [valid.allObjects sortedArrayUsingSelector:@selector(compare:)];
}
static NSData *HLERouteRequest(uint8_t op,NSData *session,uint64_t ticket,NSArray<NSString *> *apps) {
    uint8_t header[27]={0}; header[0]=op; memcpy(header+1,session.bytes,16);
    for (unsigned i=0;i<8;i++) header[17+i]=(uint8_t)(ticket>>(56-i*8));
    header[25]=(uint8_t)(apps.count>>8); header[26]=(uint8_t)apps.count;
    NSMutableData *request=[NSMutableData dataWithBytes:header length:sizeof header];
    for (NSString *app in apps) {
        NSData *name=[app dataUsingEncoding:NSUTF8StringEncoding];
        uint8_t length[2]={(uint8_t)(name.length>>8),(uint8_t)name.length};
        [request appendBytes:length length:2]; [request appendData:name];
    }
    return request;
}
static NSError *HLEError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.HLE" code:1 userInfo:@{NSLocalizedDescriptionKey:message}];
}
@interface VMTouchHLECoordinator ()
@property(atomic) BOOL invalidated;
@property(atomic,strong) VMPackageBridge *activeBridge;
+ (instancetype)forInstance:(NSString *)identifier;
- (void)openIdentifier:(NSString *)identifier name:(NSString *)name;
- (void)nativeIdentifier:(NSString *)identifier;
- (void)setIdentifier:(NSString *)identifier usesHLE:(BOOL)enabled completion:(void (^)(NSError *))completion;
@end

@interface HLEDisplayView : UIView <UIKeyInput>
@property(nonatomic,weak) VMTouchHLESurface *surface;
@property(nonatomic) BOOL inputEnabled;
- (void)showFrame:(NSData *)data;
@end
static void HLEReleaseFrame(void *context,const void *bytes,size_t count) {
    (void)bytes; (void)count; CFRelease(context);
}
@implementation HLEDisplayView {
    NSMutableDictionary<NSNumber *,NSValue *> *_pressed;
}
@synthesize inputEnabled=_inputEnabled;
- (void)setInputEnabled:(BOOL)enabled {
    if (_inputEnabled==enabled) return;
    if (!enabled) {
        // Finish fingers already delivered to the guest. New taps during a
        // pause must not become a delayed burst of input after Resume.
        for (NSNumber *finger in _pressed) {
            CGPoint point=_pressed[finger].CGPointValue;
            s5lbox_hle_input_event event={0}; event.kind=9;
            event.finger=finger.longLongValue; event.x=point.x; event.y=point.y;
            [self.surface enqueueInput:event];
        }
        [_pressed removeAllObjects];
    }
    _inputEnabled=enabled;
    self.userInteractionEnabled=enabled;
}
- (void)showFrame:(NSData *)data {
    uint32_t width=self.surface.width,height=self.surface.height;
    if (!data || !width || !height || data.length!=(NSUInteger)width*height*4) return;
    void *retained=(__bridge_retained void *)data;
    CGDataProviderRef provider=CGDataProviderCreateWithData(retained,data.bytes,data.length,HLEReleaseFrame);
    if (!provider) { CFRelease(retained); return; }
    CGColorSpaceRef color=CGColorSpaceCreateDeviceRGB();
    CGImageRef frame=CGImageCreate(width,height,8,32,width*4,color,
        kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,provider,NULL,false,kCGRenderingIntentDefault);
    self.layer.contents=(__bridge id)frame;
    if (frame) CGImageRelease(frame);
    CGColorSpaceRelease(color); CGDataProviderRelease(provider);
}
- (void)touches:(NSSet<UITouch *> *)touches kind:(uint32_t)kind {
    if (!_inputEnabled || self.bounds.size.width<=0 || self.bounds.size.height<=0) return;
    for (UITouch *touch in touches) {
        CGPoint point=[touch locationInView:self];
        s5lbox_hle_input_event event={0}; event.kind=kind;
        event.finger=(int64_t)(uintptr_t)(__bridge void *)touch;
        event.x=fmaxf(0,fminf(1,(float)(point.x/self.bounds.size.width)));
        event.y=fmaxf(0,fminf(1,(float)(point.y/self.bounds.size.height)));
        if ([self.surface enqueueInput:event]) {
            if (!_pressed) _pressed=[NSMutableDictionary dictionary];
            NSNumber *finger=@(event.finger);
            if (kind==3 || kind==9) [_pressed removeObjectForKey:finger];
            else _pressed[finger]=[NSValue valueWithCGPoint:CGPointMake(event.x,event.y)];
        }
    }
}
- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self touches:touches kind:1]; }
- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self touches:touches kind:2]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self touches:touches kind:3]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self touches:touches kind:9]; }
- (BOOL)canBecomeFirstResponder { return _inputEnabled; }
- (BOOL)hasText { return YES; }
- (void)deleteBackward { if (!_inputEnabled) return; s5lbox_hle_input_event event={0}; event.kind=5; [self.surface enqueueInput:event]; }
- (void)insertText:(NSString *)text {
    if (!_inputEnabled) return;
    if ([text isEqual:@"\n"]) { s5lbox_hle_input_event event={0}; event.kind=6; [self.surface enqueueInput:event]; return; }
    NSData *bytes=[text dataUsingEncoding:NSUTF8StringEncoding]; const uint8_t *p=bytes.bytes; NSUInteger left=bytes.length;
    while (left) {
        NSUInteger count=MIN(left,31u);
        // Split only at UTF-8 character boundaries, preserving pasted text.
        if (count<left) while (count && (p[count]&0xc0)==0x80) count--;
        if (!count) return;
        s5lbox_hle_input_event event={0}; event.kind=4; memcpy(event.text,p,count);
        if (![self.surface enqueueInput:event]) return;
        p+=count; left-=count;
    }
}
@end

@interface HLERunViewController : UIViewController
@property(nonatomic,strong) VMUSBTransport *transport;
@property(nonatomic,copy) NSString *instanceID;
@property(nonatomic,copy) NSString *appIdentifier,*appName;
@property(nonatomic,strong) VMTouchHLECoordinator *coordinator;
@end
@implementation HLERunViewController {
    HLEDisplayView *_display;
    UILabel *_status;
    UIButton *_home,*_controls,*_tryNative;
    UIActivityIndicatorView *_spinner;
    CADisplayLink *_link;
    VMTouchHLESurface *_surface;
    VMPackageBridge *_bridge;
    VMGuestApplications *_discovery;
    CMMotionManager *_motion;
    const s5lbox_hle_control *_control;
    BOOL _paused,_closing,_forceAvailable,_finished,_inactive,_wantsTextInput;
    BOOL _wantsIdleTimerDisabled,_holdsIdleTimer,_previousIdleTimerDisabled;
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor=UIColor.systemBackgroundColor;
    self.title=self.appName; self.modalInPresentation=YES;
    self.navigationController.modalInPresentation=YES;
    _display=[HLEDisplayView new]; _display.backgroundColor=UIColor.blackColor;
    _display.multipleTouchEnabled=YES; _display.layer.magnificationFilter=kCAFilterNearest;
    _display.accessibilityLabel=self.appName;
    _status=[UILabel new]; _status.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _status.adjustsFontForContentSizeCategory=YES; _status.numberOfLines=0;
    _status.textAlignment=NSTextAlignmentCenter; _status.textColor=UIColor.whiteColor; _status.text=@"Opening app…";
    _spinner=[[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _spinner.color=UIColor.whiteColor;
    [_spinner startAnimating];
    _home=[UIButton buttonWithType:UIButtonTypeSystem]; [_home setTitle:@"Home" forState:UIControlStateNormal];
    [_home addTarget:self action:@selector(home) forControlEvents:UIControlEventTouchUpInside];
    _controls=[UIButton buttonWithType:UIButtonTypeSystem]; [_controls setImage:[UIImage systemImageNamed:@"slider.horizontal.3"] forState:UIControlStateNormal];
    _controls.accessibilityLabel=@"App controls";
    [_controls addTarget:self action:@selector(controls) forControlEvents:UIControlEventTouchUpInside];
    _tryNative=[UIButton buttonWithType:UIButtonTypeSystem];
    [_tryNative setTitle:@"Try iPhone OS" forState:UIControlStateNormal];
    _tryNative.titleLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _tryNative.titleLabel.adjustsFontForContentSizeCategory=YES; _tryNative.hidden=YES;
    [_tryNative addTarget:self action:@selector(tryNative) forControlEvents:UIControlEventTouchUpInside];
    for (UIView *view in @[_display,_status,_spinner,_home,_controls,_tryNative]) [self.view addSubview:view];
    _surface=[[VMTouchHLESurface alloc] initWithWidth:320 height:480]; _display.surface=_surface;
    __weak HLERunViewController *weakSelf=self;
    _surface.textInputChanged=^(BOOL enabled) {
        HLERunViewController *owner=weakSelf;
        if (!owner) return;
        owner->_wantsTextInput=enabled;
        [owner updateInputState];
    };
    _surface.idleTimerChanged=^(BOOL enabled) {
        HLERunViewController *owner=weakSelf;
        if (!owner) return;
        owner->_wantsIdleTimerDisabled=!enabled;
        [owner updateIdleTimerState];
    };
    _surface.accelerationRequested=^{
        HLERunViewController *owner=weakSelf;
        if (!owner || owner->_finished || owner->_closing) return;
        if (!owner->_motion) {
            owner->_motion=[CMMotionManager new]; owner->_motion.accelerometerUpdateInterval=1.0/60.0;
        }
        [owner updateMotionState];
    };
    _link=[CADisplayLink displayLinkWithTarget:self selector:@selector(frame)];
    [_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(inactive) name:UIApplicationWillResignActiveNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(active) name:UIApplicationDidBecomeActiveNotification object:nil];
    _inactive=UIApplication.sharedApplication.applicationState!=UIApplicationStateActive;
    [_surface setGraphicsSuspended:_inactive];
    [self updateInputState];
    [self start];
}
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews]; CGRect safe=self.view.safeAreaLayoutGuide.layoutFrame;
    CGFloat height=MAX(0,MIN(safe.size.height-84,(safe.size.width-32)*1.5)),width=height/1.5;
    _display.frame=CGRectMake(CGRectGetMidX(safe)-width/2,safe.origin.y+(safe.size.height-68-height)/2,width,height);
    _status.frame=CGRectInset(_display.frame,16,16); _spinner.center=CGPointMake(CGRectGetMidX(safe),CGRectGetMinY(_display.frame)+32);
    _home.frame=CGRectMake(CGRectGetMidX(safe)-54,CGRectGetMaxY(safe)-60,108,48);
    _controls.frame=CGRectMake(CGRectGetMaxX(safe)-72,CGRectGetMaxY(safe)-60,48,48);
    _tryNative.frame=CGRectMake(safe.origin.x+20,CGRectGetMaxY(_display.frame)-56,MAX(0,safe.size.width-40),48);
    if (_finished) _status.frame=CGRectMake(CGRectGetMinX(_display.frame)+16,CGRectGetMinY(_display.frame)+16,
        MAX(0,_display.frame.size.width-32),MAX(0,_display.frame.size.height-88));
}
- (void)start {
    _control=s5lbox_hle_control_create();
    s5lbox_hle_control_pause(_control,_paused||_inactive);
    _bridge=[[VMPackageBridge alloc] initWithTransport:self.transport instanceID:self.instanceID];
    _discovery=[[VMGuestApplications alloc] initWithTransport:self.transport instanceID:self.instanceID];
    VMPackageBridge *bridge=_bridge; VMTouchHLESurface *surface=_surface;
    VMGuestApplications *discovery=_discovery;
    const s5lbox_hle_control *control=_control;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        @autoreleasepool {
            NSError *error=nil; VMTouchHLEFiles *files=nil;
            VMGuestApplication *app=nil;
            for (VMGuestApplication *candidate in [discovery installedApplications:&error])
                if ([candidate.identifier isEqual:self.appIdentifier]) { app=candidate; break; }
            if (!app && !error) error=HLEError(@"This app is no longer installed in the guest.");
            if (app) dispatch_async(dispatch_get_main_queue(), ^{
                self.title=app.name; self->_display.accessibilityLabel=app.name;
            });
            VMUSBService *service=app?[bridge openLiveFilesForContainer:app.containerID error:&error]:nil;
            s5lbox_hle_outcome outcome={0}; int result=S5LBOX_HLE_ERROR;
            if (service) {
                files=[[VMTouchHLEFiles alloc] initWithService:service homeDirectory:app.homeDirectory bundleDirectory:app.bundleDirectory];
                NSURL *support=[NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory inDomains:NSUserDomainMask].firstObject;
                NSURL *data=[[support URLByAppendingPathComponent:@"touchHLE"] URLByAppendingPathComponent:self.instanceID];
                if ([NSFileManager.defaultManager createDirectoryAtURL:data withIntermediateDirectories:YES attributes:nil error:&error]) {
                    NSString *resources=NSBundle.mainBundle.resourcePath;
                    s5lbox_hle_config config={0}; config.abi_version=1;
                    config.resource_directory=resources.fileSystemRepresentation; config.data_directory=data.fileSystemRepresentation;
                    s5lbox_hle_live_config live=files.configuration;
                    s5lbox_hle_presentation presentation=surface.configuration;
                    result=s5lbox_hle_run_presented(&config,&live,&presentation,control,&outcome);
                    if (files.error) error=files.error;
                }
            }
            [surface finish]; [files close]; [service close];
            NSString *message=error.localizedDescription ?: [NSString stringWithUTF8String:outcome.message] ?: @"The app could not run.";
            dispatch_async(dispatch_get_main_queue(), ^{
                s5lbox_hle_control_release(control); self->_control=NULL; self->_finished=YES;
                [self->_link invalidate]; self->_link=nil; [self->_spinner stopAnimating];
                [self updateInputState];
                [self updateMotionState];
                if (self->_closing || (result==S5LBOX_HLE_OK && !error)) [self dismissViewControllerAnimated:YES completion:nil];
                else {
                    self->_status.text=[@"Could not open with touchHLE.\n\n" stringByAppendingString:
                        message.length>350?[message substringToIndex:350]:message];
                    self->_status.hidden=NO; self->_status.textColor=UIColor.labelColor; self->_display.hidden=YES;
                    self->_tryNative.hidden=NO; self->_controls.hidden=YES;
                    [self.view setNeedsLayout];
                }
            });
        }
    });
}
- (void)frame {
    CMAccelerometerData *sample=_motion.accelerometerData;
    if (sample && !_paused && !_inactive && !_closing && !_finished)
        [_surface setAccelerationX:(float)sample.acceleration.x y:(float)sample.acceleration.y z:(float)sample.acceleration.z];
    NSData *frame=[_surface takeFrame]; if (!frame) return;
    [_display showFrame:frame]; if (!_closing && !_paused) _status.hidden=YES; [_spinner stopAnimating];
}
- (void)updateInputState {
    _display.inputEnabled=!(_paused||_inactive||_closing||_finished);
    BOOL keyboard=_display.inputEnabled && _wantsTextInput;
    // The software keyboard covers the bottom bar. Keep the same actions
    // reachable above it without permanently duplicating the runtime chrome.
    if (keyboard && !self.navigationItem.leftBarButtonItem) {
        self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Home"
            style:UIBarButtonItemStylePlain target:self action:@selector(home)];
        UIBarButtonItem *controls=[[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"slider.horizontal.3"]
            style:UIBarButtonItemStylePlain target:self action:@selector(controls)];
        controls.accessibilityLabel=@"App controls"; self.navigationItem.rightBarButtonItem=controls;
    } else if (!keyboard) {
        self.navigationItem.leftBarButtonItem=nil; self.navigationItem.rightBarButtonItem=nil;
    }
    if (keyboard) [_display becomeFirstResponder];
    else [_display resignFirstResponder];
    _link.paused=_paused||_inactive;
    [self updateMotionState];
    [self updateIdleTimerState];
    if (!_closing && !_finished) {
        _status.text=_paused?@"Paused":@"Opening app…";
        _status.hidden=!_paused && _surface.frameCount>0;
    }
}
- (void)updateIdleTimerState {
    BOOL hold=_wantsIdleTimerDisabled && !(_paused||_inactive||_closing||_finished);
    UIApplication *application=UIApplication.sharedApplication;
    if (hold && !_holdsIdleTimer) {
        _previousIdleTimerDisabled=application.idleTimerDisabled;
        application.idleTimerDisabled=YES;
        _holdsIdleTimer=YES;
    } else if (!hold && _holdsIdleTimer) {
        application.idleTimerDisabled=_previousIdleTimerDisabled;
        _holdsIdleTimer=NO;
    }
}
- (void)updateMotionState {
    if (!_motion) return;
    BOOL enabled=!(_paused||_inactive||_closing||_finished);
    if (enabled && _motion.accelerometerAvailable && !_motion.accelerometerActive) [_motion startAccelerometerUpdates];
    else if (!enabled && _motion.accelerometerActive) [_motion stopAccelerometerUpdates];
}
- (void)inactive {
    _inactive=YES; [self updateInputState];
    if (_control) s5lbox_hle_control_pause(_control,true);
    [_surface setGraphicsSuspended:YES];
}
- (void)active {
    [_surface setGraphicsSuspended:_paused];
    _inactive=NO; [self updateInputState];
    if (_control) s5lbox_hle_control_pause(_control,_paused);
}
- (void)home {
    if (_finished) { [self dismissViewControllerAnimated:YES completion:nil]; return; }
    if (_forceAvailable) { if (_control) s5lbox_hle_control_stop(_control); _bridge.canceled=YES; _discovery.canceled=YES; return; }
    if (_closing) return;
    _closing=YES; _paused=NO; [_surface setGraphicsSuspended:_inactive];
    if (_control) s5lbox_hle_control_pause(_control,_inactive);
    [self updateInputState];
    s5lbox_hle_input_event event={0}; event.kind=8; [_surface enqueueInput:event];
    _status.text=@"Closing app…"; _status.textColor=UIColor.whiteColor; _status.hidden=NO;
    [_display resignFirstResponder];
    __weak HLERunViewController *weakSelf=self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),dispatch_get_main_queue(), ^{
        HLERunViewController *owner=weakSelf; if (!owner || owner->_finished) return;
        owner->_forceAvailable=YES; [owner->_home setTitle:@"Force Close" forState:UIControlStateNormal];
        owner->_status.text=@"The app has not closed. Force Close may lose unsaved changes.";
    });
}
- (void)tryNative {
    if (!_finished) return;
    VMTouchHLECoordinator *coordinator=self.coordinator; NSString *identifier=self.appIdentifier;
    [self dismissViewControllerAnimated:YES completion:^{ [coordinator nativeIdentifier:identifier]; }];
}
- (void)controls {
    if (_closing || _finished) return;
    UIAlertController *sheet=[UIAlertController alertControllerWithTitle:nil message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:_paused?@"Resume":@"Pause" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        self->_paused=!self->_paused; [self updateInputState];
        if (self->_control) s5lbox_hle_control_pause(self->_control,self->_paused||self->_inactive);
        [self->_surface setGraphicsSuspended:self->_paused||self->_inactive];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView=_controls; sheet.popoverPresentationController.sourceRect=_controls.bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}
- (void)dealloc {
    if (_holdsIdleTimer) UIApplication.sharedApplication.idleTimerDisabled=_previousIdleTimerDisabled;
    [_motion stopAccelerometerUpdates]; [NSNotificationCenter.defaultCenter removeObserver:self]; [_link invalidate];
}
@end

@implementation VMTouchHLECoordinator {
    VMUSBTransport *_transport;
    NSString *_instanceID;
    __weak UIViewController *_presenter;
    NSData *_session;
    dispatch_queue_t _queue;
    BOOL _busy,_armed,_canRoute;
    NSTimeInterval _next;
}
static NSMapTable<NSString *,VMTouchHLECoordinator *> *HLECoordinators;
+ (instancetype)forInstance:(NSString *)identifier { return [HLECoordinators objectForKey:identifier]; }
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier presenter:(UIViewController *)presenter {
    self=[super init]; if (self) {
        _transport=transport; _instanceID=[identifier copy]; _presenter=presenter;
        uuid_t bytes; [NSUUID.UUID getUUIDBytes:bytes]; _session=[NSData dataWithBytes:bytes length:16];
        _queue=dispatch_queue_create("com.j0shua.s5lbox.app-routing",DISPATCH_QUEUE_SERIAL);
        if (!HLECoordinators) HLECoordinators=[NSMapTable strongToWeakObjectsMapTable];
        [HLECoordinators setObject:self forKey:identifier];
    } return self;
}
- (NSData *)exchange:(NSData *)request error:(NSError **)error {
    if (self.invalidated) { if (error) *error=HLEError(@"The machine session has ended."); return nil; }
    VMPackageBridge *bridge=[[VMPackageBridge alloc] initWithTransport:_transport instanceID:_instanceID];
    self.activeBridge=bridge;
    if (self.invalidated) bridge.canceled=YES;
    NSData *reply=[bridge exchangeAppRoute:request error:error]; self.activeBridge=nil;
    if (!reply && error && !*error) *error=HLEError(@"The guest app service did not respond.");
    if (reply) {
        const uint8_t *p=reply.bytes;
        unsigned length=((unsigned)p[16]<<8)|p[17];
        if (p[4]||p[5]||p[6]||p[7]||reply.length!=18+length) {
            if (error) *error=HLEError(!p[4]&&!p[5]&&!p[6]&&p[7]==EACCES ?
                @"Unlock iPhone OS, then open the app from its icon." :
                @"The guest could not complete the app request. Return to iPhone OS and try again.");
            return nil;
        }
    }
    return reply;
}
- (void)tick:(BOOL)canRoute guestAwake:(BOOL)guestAwake {
    _canRoute=canRoute && guestAwake;
    // Sleeping guests cannot tap icons. Do not wake one just to renew or
    // disarm a routing lease; it expires on its own, and refreshes on wake.
    if (!guestAwake) return;
    NSTimeInterval now=NSProcessInfo.processInfo.systemUptime;
    if (self.invalidated || _busy || now<_next) return;
    NSArray *choices=canRoute?HLEChoices(_instanceID):@[];
    if (!choices.count && !_armed) return;
    _busy=YES; _next=now+1;
    NSData *request=HLERouteRequest(1,_session,0,choices);
    dispatch_async(_queue, ^{
        NSError *error=nil; NSData *reply=[self exchange:request error:&error];
        uint64_t ticket=0; NSString *identifier=nil;
        if (reply) {
            const uint8_t *p=reply.bytes;
            for (unsigned i=0;i<8;i++) ticket=(ticket<<8)|p[8+i];
            if (ticket) identifier=[[NSString alloc] initWithBytes:p+18 length:reply.length-18 encoding:NSUTF8StringEncoding];
        }
        if (ticket) {
            NSData *ack=[self exchange:HLERouteRequest(2,self->_session,ticket,@[]) error:&error];
            if (!ack) identifier=nil; // Unknown acknowledgement is not permission to run twice.
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_busy=NO; self->_armed=reply && choices.count>0;
            if (!reply) self->_next=NSProcessInfo.processInfo.systemUptime+3;
            if (identifier.length && self->_canRoute && !self.invalidated)
                [self openIdentifier:identifier name:identifier];
        });
    });
}
- (void)openIdentifier:(NSString *)identifier name:(NSString *)name {
    UIViewController *presenter=_presenter;
    if (self.invalidated || !presenter.viewIfLoaded.window || presenter.presentedViewController) return;
    HLERunViewController *run=[HLERunViewController new];
    run.appIdentifier=identifier; run.appName=name; run.transport=_transport; run.instanceID=_instanceID; run.coordinator=self;
    UINavigationController *nav=[[UINavigationController alloc] initWithRootViewController:run];
    nav.modalPresentationStyle=UIModalPresentationFullScreen;
    [presenter presentViewController:nav animated:YES completion:nil];
}
- (void)showError:(NSError *)error {
    UIViewController *presenter=_presenter; if (!presenter.viewIfLoaded.window || presenter.presentedViewController || self.invalidated) return;
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Could not open app"
        message:error.localizedDescription preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
    [presenter presentViewController:alert animated:YES completion:nil];
}
- (void)nativeIdentifier:(NSString *)identifier {
    _busy=YES; NSArray *choices=HLEChoices(_instanceID);
    dispatch_async(_queue, ^{
        NSError *error=nil;
        if ([self exchange:HLERouteRequest(1,self->_session,0,choices) error:&error])
            [self exchange:HLERouteRequest(3,self->_session,0,@[identifier]) error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{ self->_busy=NO; if (error) [self showError:error]; });
    });
}
- (void)setIdentifier:(NSString *)identifier usesHLE:(BOOL)enabled completion:(void (^)(NSError *))completion {
    NSMutableArray *choices=[HLEChoices(_instanceID) mutableCopy]; [choices removeObject:identifier];
    if (enabled) [choices addObject:identifier];
    if (choices.count>128) { completion(HLEError(@"Up to 128 apps can use touchHLE in one machine.")); return; }
    if (!enabled) {
        // Recovery must remain available even if the guest helper is offline.
        [NSUserDefaults.standardUserDefaults setObject:choices forKey:HLEChoiceKey(_instanceID)];
        _armed=YES; _next=0; completion(nil); return;
    }
    _busy=YES;
    dispatch_async(_queue, ^{
        NSError *error=nil;
        [self exchange:HLERouteRequest(1,self->_session,0,choices) error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_busy=NO;
            if (!error && !self.invalidated) {
                [NSUserDefaults.standardUserDefaults setObject:choices forKey:HLEChoiceKey(self->_instanceID)];
                self->_armed=choices.count>0;
            }
            completion(error);
        });
    });
}
- (void)invalidate { self.invalidated=YES; self.activeBridge.canceled=YES; }
@end

@implementation VMTouchHLEAppsViewController {
    VMUSBTransport *_transport;
    NSString *_instanceID;
    VMGuestApplications *_discovery;
    VMPackageBridge *_setup;
    NSArray<VMGuestApplication *> *_apps;
    NSString *_pendingSelection;
    BOOL _busy,_connecting;
    UILabel *_message;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self=[super initWithStyle:UITableViewStyleInsetGrouped];
    if (self) { _transport=transport; _instanceID=[identifier copy]; }
    return self;
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=@"touchHLE";
    self.navigationItem.largeTitleDisplayMode=UINavigationItemLargeTitleDisplayModeNever;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Set Up" style:UIBarButtonItemStylePlain target:self action:@selector(setup)];
    self.refreshControl=[UIRefreshControl new]; [self.refreshControl addTarget:self action:@selector(refresh) forControlEvents:UIControlEventValueChanged];
    self.tableView.rowHeight=UITableViewAutomaticDimension; self.tableView.estimatedRowHeight=60;
    _message=[UILabel new]; _message.textAlignment=NSTextAlignmentCenter; _message.numberOfLines=0;
    _message.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody]; _message.adjustsFontForContentSizeCategory=YES;
    [self refresh];
}
- (void)message:(NSString *)text {
    _message.text=text; self.tableView.backgroundView=text.length?_message:nil;
}
- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    if (self.isMovingFromParentViewController || self.navigationController.isBeingDismissed)
        _discovery.canceled=YES;
}
- (void)refresh {
    if (_busy) return; _busy=YES; _connecting=YES;
    [self message:_apps.count?nil:@"Connecting to iPhone OS…"];
    _discovery=[[VMGuestApplications alloc] initWithTransport:_transport instanceID:_instanceID];
    VMGuestApplications *discovery=_discovery;
    __weak VMTouchHLEAppsViewController *weakSelf=self;
    __weak VMGuestApplications *weakDiscovery=discovery;
    discovery.progress=^(VMGuestApplicationsStage stage) {
        dispatch_async(dispatch_get_main_queue(), ^{
            VMTouchHLEAppsViewController *owner=weakSelf;
            if (!owner || owner->_discovery!=weakDiscovery || weakDiscovery.canceled || !owner->_busy) return;
            owner->_connecting=stage==VMGuestApplicationsConnecting;
            [owner message:owner->_apps.count?nil:owner->_connecting?
                @"Connecting to iPhone OS…":@"Loading installed apps…"];
        });
    };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,8*NSEC_PER_SEC),dispatch_get_main_queue(), ^{
        VMTouchHLEAppsViewController *owner=weakSelf;
        if (owner && owner->_discovery==discovery && !discovery.canceled &&
            owner->_busy && owner->_connecting && !owner->_apps.count)
            [owner message:@"Waiting for iPhone OS…\nLet the guest finish booting and unlock it."];
    });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        NSError *error=nil; NSArray *apps=[discovery installedApplications:&error];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_busy=NO; [self.refreshControl endRefreshing];
            self->_apps=apps?:@[];
            [self.tableView reloadData];
            [self message:error.localizedDescription ?: (apps.count?nil:@"Install an app in this guest to open it here.")];
        });
    });
}
- (NSInteger)tableView:(UITableView *)table numberOfRowsInSection:(NSInteger)section { (void)table; (void)section; return _apps.count; }
- (UITableViewCell *)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell=[table dequeueReusableCellWithIdentifier:@"app"];
    if (!cell) cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"app"];
    VMGuestApplication *app=_apps[path.row]; cell.textLabel.text=app.name;
    cell.detailTextLabel.text=[HLEChoices(_instanceID) containsObject:app.identifier]?@"Opens with touchHLE":@"Opens in iPhone OS";
    cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody]; cell.textLabel.adjustsFontForContentSizeCategory=YES;
    cell.detailTextLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1]; cell.detailTextLabel.adjustsFontForContentSizeCategory=YES;
    cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator; return cell;
}
- (void)tableView:(UITableView *)table didSelectRowAtIndexPath:(NSIndexPath *)path {
    [table deselectRowAtIndexPath:path animated:YES]; if (_busy) return;
    VMGuestApplication *app=_apps[path.row];
    VMTouchHLECoordinator *coordinator=[VMTouchHLECoordinator forInstance:_instanceID];
    if (!coordinator) return;
    BOOL usesHLE=[HLEChoices(_instanceID) containsObject:app.identifier];
    UIAlertController *sheet=[UIAlertController alertControllerWithTitle:app.name
        message:@"Choose how this app opens from its iPhone OS icon." preferredStyle:UIAlertControllerStyleActionSheet];
    [sheet addAction:[UIAlertAction actionWithTitle:usesHLE?@"Use iPhone OS":@"Use touchHLE"
        style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        self->_busy=YES;
        [coordinator setIdentifier:app.identifier usesHLE:!usesHLE completion:^(NSError *error) {
            self->_busy=NO; [self.tableView reloadData];
            if (error) {
                UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Guest support needed"
                    message:@"Install guest support and respring iPhone OS. Your app choice will be applied automatically."
                    preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
                [alert addAction:[UIAlertAction actionWithTitle:@"Set Up" style:UIAlertActionStyleDefault
                    handler:^(__unused UIAlertAction *a) { self->_pendingSelection=app.identifier; [self installSupport]; }]];
                [self presentViewController:alert animated:YES completion:nil];
            }
        }];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Open with touchHLE" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) {
        UIViewController *presenter=self.navigationController.presentingViewController;
        [presenter dismissViewControllerAnimated:YES completion:^{ [coordinator openIdentifier:app.identifier name:app.name]; }];
    }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView=[table cellForRowAtIndexPath:path];
    sheet.popoverPresentationController.sourceRect=[table cellForRowAtIndexPath:path].bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}
- (void)setup {
    if (_busy) return;
    _pendingSelection=nil;
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Set up touchHLE?"
        message:@"Install guest support and respring iPhone OS. Close guest apps first."
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Set Up" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *action) { [self installSupport]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)installSupport {
    if (_busy) return;
    _apps=@[]; [self.tableView reloadData];
    _busy=YES; self.navigationItem.hidesBackButton=YES; self.navigationController.modalInPresentation=YES;
    self.navigationItem.rightBarButtonItem.enabled=NO; [self message:@"Setting up touchHLE…"];
    _setup=[[VMPackageBridge alloc] initWithTransport:_transport instanceID:_instanceID];
    VMPackageBridge *setup=_setup;
    NSString *selection=_pendingSelection; _pendingSelection=nil;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        NSError *error=nil; NSData *status=[setup status:&error];
        NSURL *package=[NSBundle.mainBundle URLForResource:@"s5lbox-hle-launch" withExtension:@"deb"];
        if (status && package) status=[setup install:@[package] status:status error:&error];
        else if (status) error=[NSError errorWithDomain:@"S5LBox.HLE" code:1 userInfo:@{NSLocalizedDescriptionKey:@"The guest launch package is missing from this build."}];
        BOOL okay=status && !error;
        if (okay && setup.finishAction) okay=[setup performFinish:status error:&error];
        if (okay) {
            dispatch_async(dispatch_get_main_queue(), ^{ [self message:@"Waiting for iPhone OS…"]; });
            // A successful package install is not a ready SpringBoard hook.
            // Probe its authenticated handshake through the respring, with a
            // fresh empty session so an old ticket cannot be replayed.
            uuid_t bytes; [NSUUID.UUID getUUIDBytes:bytes];
            NSData *session=[NSData dataWithBytes:bytes length:16];
            NSData *probe=HLERouteRequest(1,session,0,@[]);
            NSTimeInterval deadline=NSProcessInfo.processInfo.systemUptime+45;
            okay=NO;
            while (!setup.canceled && NSProcessInfo.processInfo.systemUptime<deadline) {
                error=nil; NSData *reply=[setup exchangeAppRoute:probe error:&error];
                const uint8_t *p=reply.bytes;
                if (reply.length==18 && !p[4]&&!p[5]&&!p[6]&&!p[7]) { okay=YES; error=nil; break; }
                [NSThread sleepForTimeInterval:1];
            }
            if (!okay) error=HLEError(@"Guest support is installed, but iPhone OS has not reconnected. Let it finish the respring, then try again.");
        }
        dispatch_async(dispatch_get_main_queue(), ^{
            if (okay && selection.length) {
                [self message:@"Updating app preference…"];
                VMTouchHLECoordinator *coordinator=[VMTouchHLECoordinator forInstance:self->_instanceID];
                if (coordinator) {
                    [coordinator setIdentifier:selection usesHLE:YES completion:^(NSError *choiceError) {
                        [self finishSetup:choiceError];
                    }];
                } else [self finishSetup:HLEError(@"The machine session has ended.")];
            } else [self finishSetup:okay?nil:error];
        });
    });
}
- (void)finishSetup:(NSError *)error {
    _busy=NO; self.navigationItem.hidesBackButton=NO; self.navigationController.modalInPresentation=NO;
    self.navigationItem.rightBarButtonItem.enabled=YES;
    if (error) [self message:error.localizedDescription];
    else [self refresh];
}
- (void)dealloc { _discovery.canceled=YES; _setup.canceled=YES; }
@end
#endif
