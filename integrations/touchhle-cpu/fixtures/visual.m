/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * Tiny ARMv6 UIKit app. Public API declarations are kept here so the community
 * SDK can build the test without distributing Apple headers or firmware. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef signed char BOOL;
typedef id (*IMP)(id,SEL,...);
extern SEL sel_getUid(const char *name);
extern SEL sel_registerName(const char *name);
typedef struct { float x,y; } CGPoint;
typedef struct { float width,height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
__attribute__((objc_root_class)) @interface NSObject { Class isa; }
+ (id)alloc;
+ (Class)class;
+ (IMP)methodForSelector:(SEL)selector;
+ (IMP)instanceMethodForSelector:(SEL)selector;
- (id)init;
- (id)autorelease;
- (void)release;
- (IMP)methodForSelector:(SEL)selector;
@end
@interface NSAutoreleasePool : NSObject @end
@interface NSNumber : NSObject
+ (id)numberWithUnsignedLong:(unsigned long)value;
- (id)initWithUnsignedLong:(unsigned long)value;
- (unsigned long)unsignedLongValue;
- (unsigned long long)unsignedLongLongValue;
- (long)longValue;
@end
@interface NSString : NSObject
+ (id)stringWithUTF8String:(const char *)text;
- (const char *)UTF8String;
- (BOOL)isEqualToString:(id)other;
- (id)stringByAppendingPathComponent:(id)component;
@end
@interface NSBundle : NSObject
+ (id)mainBundle;
+ (id)bundleWithPath:(id)path;
+ (id)bundleForClass:(Class)class;
- (id)bundlePath;
- (id)bundleIdentifier;
- (id)executablePath;
- (id)pathForResource:(id)name ofType:(id)type;
- (id)localizedStringForKey:(id)key value:(id)value table:(id)table;
@end
@interface NSIndexPath : NSObject
+ (id)indexPathForRow:(int)row inSection:(int)section;
- (unsigned)length;
- (unsigned)indexAtPosition:(unsigned)position;
- (int)row;
- (int)section;
- (id)indexPathByAddingIndex:(unsigned)index;
- (id)indexPathByRemovingLastIndex;
- (BOOL)isEqual:(id)other;
- (int)compare:(id)other;
@end
@interface NSUserDefaults : NSObject
+ (id)standardUserDefaults;
- (int)integerForKey:(id)key;
- (void)setInteger:(int)value forKey:(id)key;
@end
@interface UIColor : NSObject
+ (id)colorWithRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha;
+ (id)whiteColor;
@end
@interface UIView : NSObject
- (id)initWithFrame:(CGRect)rect;
- (void)setFrame:(CGRect)rect;
- (void)setBackgroundColor:(id)color;
- (void)addSubview:(id)view;
@end
@interface UIWindow : UIView
- (void)makeKeyAndVisible;
@end
@interface UILabel : UIView
- (id)text;
- (void)setText:(id)text;
- (void)setTextColor:(id)color;
- (void)setTextAlignment:(int)alignment;
@end
@interface NSArray : NSObject
- (unsigned)count;
@end
@interface UITableViewCell : UIView
- (id)initWithStyle:(int)style reuseIdentifier:(id)identifier;
- (id)textLabel;
- (BOOL)isSelected;
@end
@interface UITableView : UIView
- (id)initWithFrame:(CGRect)frame style:(int)style;
- (void)setDataSource:(id)source;
- (void)setDelegate:(id)delegate;
- (void)reloadData;
- (int)numberOfSections;
- (int)numberOfRowsInSection:(int)section;
- (id)dequeueReusableCellWithIdentifier:(id)identifier;
- (id)cellForRowAtIndexPath:(id)path;
- (id)indexPathForSelectedRow;
- (id)visibleCells;
- (CGSize)contentSize;
- (void)setContentOffset:(CGPoint)offset;
- (void)selectRowAtIndexPath:(id)path animated:(BOOL)animated scrollPosition:(int)position;
- (void)deselectRowAtIndexPath:(id)path animated:(BOOL)animated;
- (void)touchesBegan:(id)touches withEvent:(id)event;
- (void)touchesMoved:(id)touches withEvent:(id)event;
- (void)touchesEnded:(id)touches withEvent:(id)event;
- (void)touchesCancelled:(id)touches withEvent:(id)event;
@end
@interface TableSource : NSObject { @public unsigned created,selected; }
@end
@implementation TableSource
- (int)numberOfSectionsInTableView:(id)table { (void)table; return 2; }
- (int)tableView:(id)table numberOfRowsInSection:(int)section { (void)table; (void)section; return 1000; }
- (float)tableView:(id)table heightForRowAtIndexPath:(NSIndexPath *)path {
    (void)table; return [path section] ? 44.0f : 22.0f;
}
- (id)tableView:(UITableView *)table cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell=[table dequeueReusableCellWithIdentifier:@"fixture"];
    if (!cell) { cell=[[[UITableViewCell alloc] initWithStyle:0 reuseIdentifier:@"fixture"] autorelease]; created++; }
    char label[48]; snprintf(label,sizeof(label),"row %d/%d",[path section],[path row]);
    [[cell textLabel] setText:[NSString stringWithUTF8String:label]];
    return cell;
}
- (void)tableView:(id)table didSelectRowAtIndexPath:(id)path { (void)table; (void)path; selected++; }
@end
@interface TableTouch : NSObject { @public CGPoint point,previous; }
@end
@implementation TableTouch
- (CGPoint)locationInView:(id)view { (void)view; return point; }
- (CGPoint)previousLocationInView:(id)view { (void)view; return previous; }
@end
@interface UIControl : UIView
- (void)addTarget:(id)target action:(SEL)action forControlEvents:(unsigned)events;
- (void)touchesBegan:(id)touches withEvent:(id)event;
- (void)touchesCancelled:(id)touches withEvent:(id)event;
- (void)cancelTrackingWithEvent:(id)event;
- (BOOL)isTracking;
- (BOOL)isHighlighted;
@end
@interface UIButton : UIControl
+ (id)buttonWithType:(int)type;
- (void)setTitle:(id)title forState:(unsigned)state;
@end
typedef struct { unsigned location,length; } NSRange;
@interface UITextField : UIControl
- (void)setDelegate:(id)delegate;
- (void)setText:(id)text;
- (id)text;
- (BOOL)resignFirstResponder;
@end
@interface NSSet : NSObject
+ (id)setWithObject:(id)object;
@end
@interface UITouch : NSObject @end
@interface NSTimer : NSObject
+ (id)scheduledTimerWithTimeInterval:(double)interval target:(id)target selector:(SEL)selector userInfo:(id)info repeats:(BOOL)repeats;
- (void)invalidate;
@end
@interface UIAcceleration : NSObject
- (double)x;
- (double)y;
- (double)z;
@end
@interface UIAccelerometer : NSObject
+ (id)sharedAccelerometer;
- (void)setDelegate:(id)delegate;
- (void)setUpdateInterval:(double)interval;
@end
extern void *alcOpenDevice(const char *);
extern void *alcCreateContext(void *,const int *);
extern BOOL alcMakeContextCurrent(void *);
extern void alcDestroyContext(void *);
extern BOOL alcCloseDevice(void *);
extern void alGenBuffers(int,unsigned *);
extern void alDeleteBuffers(int,const unsigned *);
extern void alBufferData(unsigned,int,const void *,int,int);
extern void alGenSources(int,unsigned *);
extern void alDeleteSources(int,const unsigned *);
extern void alSourcei(unsigned,int,int);
extern void alSourcef(unsigned,int,float);
extern void alSourcePlay(unsigned);
extern void alSourceStop(unsigned);
extern void alGetSourcei(unsigned,int,int *);
extern int alGetError(void);
@interface CancelTarget : NSObject { @public unsigned ended,cancelled; }
- (void)ended:(id)sender;
- (void)cancelled:(id)sender;
@end
@implementation CancelTarget
- (void)ended:(id)sender { (void)sender; ended++; }
- (void)cancelled:(id)sender { (void)sender; cancelled++; }
@end
@interface UIApplication : NSObject
+ (id)sharedApplication;
- (void)setStatusBarHidden:(BOOL)hidden;
- (BOOL)isIdleTimerDisabled;
- (void)setIdleTimerDisabled:(BOOL)disabled;
@end
extern int UIApplicationMain(int argc,char **argv,id principal,id delegate);
@interface HLEVisualDelegate : NSObject {
    UIWindow *_window;
    UILabel *_label;
    unsigned _taps;
    UIButton *_audioButton;
    NSTimer *_audioTimer;
    void *_audioDevice,*_audioContext;
    unsigned _audioBuffer,_audioSource;
    BOOL _audioPlaying;
    UIButton *_motionButton;
    unsigned _motionSamples;
    UITextField *_textField;
    unsigned _edits;
    UIButton *_idleButton;
}
@end
@implementation HLEVisualDelegate
- (void)applicationDidFinishLaunching:(UIApplication *)application {
    [application setStatusBarHidden:1];
    char path[1024]; snprintf(path,sizeof path,"%s/Documents/visual-touch.txt",getenv("HOME"));
    FILE *saved=fopen(path,"r");
    if (saved) { if (fscanf(saved,"%u",&_taps)!=1) _taps=0; fclose(saved); }
    _window=[[UIWindow alloc] initWithFrame:(CGRect){{0,0},{320,480}}];
    [_window setBackgroundColor:[UIColor colorWithRed:0.06f green:0.12f blue:0.25f alpha:1]];
    _label=[[UILabel alloc] initWithFrame:(CGRect){{12,130},{296,60}}];
    [_label setBackgroundColor:[UIColor colorWithRed:0 green:0 blue:0 alpha:0]];
    char title[96]; snprintf(title,sizeof title,"Saved touches: %u",_taps);
    [_label setText:[NSString stringWithUTF8String:title]];
    [_label setTextColor:[UIColor whiteColor]];
    [_label setTextAlignment:1];
    [_window addSubview:_label];
    _textField=[[UITextField alloc] initWithFrame:(CGRect){{30,60},{260,44}}];
    [_textField setDelegate:self];
    char textPath[1024],savedText[1024]={0};
    snprintf(textPath,sizeof textPath,"%s/Documents/visual-keyboard.txt",getenv("HOME"));
    FILE *textFile=fopen(textPath,"r");
    if (textFile) { fread(savedText,1,sizeof savedText-1,textFile); fclose(textFile); }
    [_textField setText:[NSString stringWithUTF8String:savedText]];
    [_textField addTarget:self action:@selector(textChanged:) forControlEvents:(1u<<17)];
    [_window addSubview:_textField];
    _idleButton=[UIButton buttonWithType:1];
    [_idleButton setFrame:(CGRect){{50,195},{220,35}}];
    [_idleButton setTitle:@"Keep awake: Off" forState:0];
    [_idleButton addTarget:self action:@selector(idleTapped:) forControlEvents:64];
    [_window addSubview:_idleButton];
    UIButton *button=[UIButton buttonWithType:1];
    [button setFrame:(CGRect){{50,245},{220,64}}];
    [button setTitle:@"Test guest touch" forState:0];
    [button addTarget:self action:@selector(tapped:) forControlEvents:64];
    [_window addSubview:button];
    _audioButton=[UIButton buttonWithType:1];
    [_audioButton setFrame:(CGRect){{50,335},{220,64}}];
    [_audioButton setTitle:@"Play audio" forState:0];
    [_audioButton addTarget:self action:@selector(audioTapped:) forControlEvents:64];
    [_window addSubview:_audioButton];
    _motionButton=[UIButton buttonWithType:1];
    [_motionButton setFrame:(CGRect){{30,415},{260,44}}];
    [_motionButton setTitle:@"Read motion" forState:0];
    [_motionButton addTarget:self action:@selector(motionTapped:) forControlEvents:64];
    [_window addSubview:_motionButton];
    [_window makeKeyAndVisible];
    puts("S5LBOX-HLE-VISUAL READY"); fflush(stdout);
    printf("S5LBOX-HLE-VISUAL RESTORED touches=%u exits=%d\n",_taps,
        [[NSUserDefaults standardUserDefaults] integerForKey:@"CompletedExits"]); fflush(stdout);
}
- (void)saveText {
    char path[1024]; snprintf(path,sizeof path,"%s/Documents/visual-keyboard.txt",getenv("HOME"));
    FILE *file=fopen(path,"w");
    if (file) { fputs([[_textField text] UTF8String],file); fclose(file); }
}
- (void)textChanged:(id)sender {
    (void)sender;
    char title[96]; snprintf(title,sizeof title,"Keyboard edits: %u",++_edits);
    [_label setText:[NSString stringWithUTF8String:title]];
}
- (BOOL)textField:(UITextField *)field shouldChangeCharactersInRange:(NSRange)range replacementString:(NSString *)text {
    (void)field; (void)range;
    // A deliberate delegate rejection for the keyboard regression test.
    return ![text isEqualToString:@"!"];
}
- (BOOL)textFieldShouldReturn:(UITextField *)field {
    [self saveText];
    return [field resignFirstResponder];
}
- (void)motionTapped:(id)sender {
    (void)sender;
    UIAccelerometer *motion=[UIAccelerometer sharedAccelerometer];
    [motion setUpdateInterval:0.25]; [motion setDelegate:self];
}
- (void)idleTapped:(id)sender {
    (void)sender;
    UIApplication *application=[UIApplication sharedApplication];
    BOOL disabled=![application isIdleTimerDisabled];
    [application setIdleTimerDisabled:disabled];
    [_idleButton setTitle:disabled?@"Keep awake: On":@"Keep awake: Off" forState:0];
}
- (void)accelerometer:(UIAccelerometer *)sensor didAccelerate:(UIAcceleration *)sample {
    (void)sensor;
    char title[96]; snprintf(title,sizeof title,"Tilt %.2f  %.2f  %.2f",[sample x],[sample y],[sample z]);
    [_motionButton setTitle:[NSString stringWithUTF8String:title] forState:0];
    if (++_motionSamples==4) { puts("S5LBOX-HLE-VISUAL MOTION samples=4"); fflush(stdout); }
}
- (void)tapped:(id)sender {
    (void)sender;
    char text[96]; snprintf(text,sizeof text,"Guest touches: %u",++_taps);
    [_label setText:[NSString stringWithUTF8String:text]];
    [_window setBackgroundColor:[UIColor colorWithRed:0.05f green:0.32f blue:0.22f alpha:1]];
    char path[1024]; snprintf(path,sizeof path,"%s/Documents/visual-touch.txt",getenv("HOME"));
    FILE *file=fopen(path,"w");
    if (file) { fprintf(file,"%u\n",_taps); fclose(file); }
    printf("S5LBOX-HLE-VISUAL TOUCH count=%u saved=%d\n",_taps,file!=NULL); fflush(stdout);
}
- (void)audioTick:(id)timer {
    (void)timer;
    if (!_audioPlaying) return;
    alcMakeContextCurrent(_audioContext);
    int position=0; alGetSourcei(_audioSource,0x1025,&position); // AL_SAMPLE_OFFSET
    char title[64]; snprintf(title,sizeof title,"Audio: %d",position);
    [_audioButton setTitle:[NSString stringWithUTF8String:title] forState:0];
}
- (void)audioTapped:(id)sender {
    (void)sender;
    if (!_audioContext) {
        if (!_audioDevice) _audioDevice=alcOpenDevice(NULL);
        if (_audioDevice) _audioContext=alcCreateContext(_audioDevice,NULL);
        if (!_audioContext || !alcMakeContextCurrent(_audioContext)) {
            if (_audioContext) { alcDestroyContext(_audioContext); _audioContext=NULL; }
            [_audioButton setTitle:@"Audio unavailable" forState:0]; return;
        }
        short samples[44100];
        for (unsigned i=0;i<44100;i++) samples[i]=(short)(((int)(i%100)-50)*200);
        alGenBuffers(1,&_audioBuffer);
        alBufferData(_audioBuffer,0x1101,samples,sizeof samples,44100); // MONO16
        alGenSources(1,&_audioSource);
        alSourcei(_audioSource,0x1009,(int)_audioBuffer); // AL_BUFFER
        alSourcei(_audioSource,0x1007,1); // AL_LOOPING
        alSourcef(_audioSource,0x100a,0.1f); // Quiet fixture; never changes host volume.
        _audioTimer=[NSTimer scheduledTimerWithTimeInterval:0.25 target:self
            selector:@selector(audioTick:) userInfo:0 repeats:1];
    }
    alcMakeContextCurrent(_audioContext);
    _audioPlaying=!_audioPlaying;
    if (_audioPlaying) alSourcePlay(_audioSource);
    else { alSourceStop(_audioSource); [_audioButton setTitle:@"Play audio" forState:0]; }
    int error=alGetError();
    if (error) { _audioPlaying=0; [_audioButton setTitle:@"Audio error" forState:0]; }
    printf("S5LBOX-HLE-VISUAL AUDIO playing=%d error=%d\n",_audioPlaying,error); fflush(stdout);
}
- (void)applicationWillTerminate:(UIApplication *)application {
    (void)application;
    [self saveText];
    NSUserDefaults *defaults=[NSUserDefaults standardUserDefaults];
    int exits=[defaults integerForKey:@"CompletedExits"]+1;
    // Intentionally do not call synchronize: the runtime must flush changes
    // made by termination callbacks, not only changes made before Home.
    [defaults setInteger:exits forKey:@"CompletedExits"];
    [_audioTimer invalidate];
    [[UIAccelerometer sharedAccelerometer] setDelegate:0];
    if (_audioContext) {
        alcMakeContextCurrent(_audioContext);
        if (_audioSource) alDeleteSources(1,&_audioSource);
        if (_audioBuffer) alDeleteBuffers(1,&_audioBuffer);
        alcMakeContextCurrent(NULL); alcDestroyContext(_audioContext);
    }
    if (_audioDevice) alcCloseDevice(_audioDevice);
    printf("S5LBOX-HLE-VISUAL TERMINATING exits=%d\n",exits); fflush(stdout);
}
@end
int main(int argc,char **argv) {
    (void)[[NSAutoreleasePool alloc] init];
    // Headless framework regression; normal visual/native runs skip this.
    if (argc>1 && !strcmp(argv[1],"--cancel-smoke")) {
        UIControl *control=[[UIControl alloc] init];
        CancelTarget *target=[[CancelTarget alloc] init];
        id touch=[[UITouch alloc] init],touches=[NSSet setWithObject:touch];
        [control addTarget:target action:@selector(ended:) forControlEvents:64];
        [control addTarget:target action:@selector(cancelled:) forControlEvents:256];
        [control touchesBegan:touches withEvent:0];
        unsigned failures=![control isTracking] || ![control isHighlighted];
        // Exercise guest ABI dispatch, not just Rust numeric conversions.
        NSNumber *wide=[NSNumber numberWithUnsignedLong:0xffffffffUL];
        failures += [wide unsignedLongValue]!=0xffffffffUL;
        failures += [wide unsignedLongLongValue]!=0xffffffffULL;
        failures += [wide longValue]!=-1;
        NSNumber *zero=[[NSNumber alloc] initWithUnsignedLong:0];
        failures += [zero unsignedLongValue]!=0;
        IMP numberMethod=[wide methodForSelector:@selector(unsignedLongValue)];
        // IMP is deliberately untyped; each call below uses the method's real ABI.
#if __has_warning("-Wcast-function-type-mismatch")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#endif
        failures += ((unsigned long (*)(id,SEL))numberMethod)(wide,@selector(unsignedLongValue))!=0xffffffffUL;
        failures += numberMethod!=[NSNumber instanceMethodForSelector:@selector(unsignedLongValue)];
        IMP factory=[NSNumber methodForSelector:@selector(numberWithUnsignedLong:)];
        NSNumber *direct=((id (*)(id,SEL,unsigned long))factory)([NSNumber class],@selector(numberWithUnsignedLong:),42);
        failures += [direct unsignedLongValue]!=42;
        IMP action=[target methodForSelector:@selector(ended:)];
        ((void (*)(id,SEL,id))action)(target,@selector(ended:),0);
#if __has_warning("-Wcast-function-type-mismatch")
#pragma clang diagnostic pop
#endif
        failures += target->ended!=1;
        target->ended=0;
        failures += sel_getUid("setText:")!=sel_registerName("setText:");
        failures += sel_getUid("fixtureNewSelector:")!=sel_registerName("fixtureNewSelector:");
        NSBundle *main=[NSBundle mainBundle];
        id resourcePath=[[main bundlePath] stringByAppendingPathComponent:@"Fixture.bundle"];
        NSBundle *resource=[NSBundle bundleWithPath:resourcePath];
        failures += resource==0;
        failures += [NSBundle bundleWithPath:[main bundlePath]]!=main;
        failures += [NSBundle bundleForClass:[CancelTarget class]]!=main;
        failures += [NSBundle bundleWithPath:[resourcePath stringByAppendingPathComponent:@"."]]!=resource;
        failures += [NSBundle bundleWithPath:@"relative.bundle"]!=0;
        failures += [NSBundle bundleWithPath:@"/missing.bundle"]!=0;
        failures += ![[resource bundleIdentifier] isEqualToString:@"org.s5lbox.fixture.resources"];
        failures += [resource executablePath]!=0;
        failures += [resource pathForResource:@"Info" ofType:@"plist"]==0;
        failures += ![[main localizedStringForKey:@"fixture-key" value:0 table:0] isEqualToString:@"fixture-key"];
        NSIndexPath *path=[NSIndexPath indexPathForRow:4 inSection:2];
        failures += [path row]!=4 || [path section]!=2 || [path length]!=2;
        failures += [path indexAtPosition:2]!=0x7fffffffU;
        NSIndexPath *child=[path indexPathByAddingIndex:7];
        failures += [path length]!=2 || [child length]!=3;
        failures += ![[child indexPathByRemovingLastIndex] isEqual:path];
        failures += [path compare:child]!=-1;
        TableSource *source=[[TableSource alloc] init];
        UITableView *table=[[UITableView alloc] initWithFrame:(CGRect){{0,0},{320,44}} style:0];
        [table setDataSource:source]; [table setDelegate:source]; [table reloadData];
        failures += [table numberOfSections]!=2 || [table numberOfRowsInSection:1]!=1000;
        failures += [table contentSize].height!=66000.0f;
        failures += [[table visibleCells] count]!=2 || source->created!=2;
        NSIndexPath *first=[NSIndexPath indexPathForRow:0 inSection:0];
        UITableViewCell *firstCell=[table cellForRowAtIndexPath:first];
        failures += ![[[firstCell textLabel] text] isEqualToString:@"row 0/0"];
        failures += [table cellForRowAtIndexPath:[NSIndexPath indexPathForRow:99 inSection:1]]!=0;
        // Scrolling thousands of rows must reuse cells, not instantiate every row.
        for (unsigned step=1;step<30;step++) [table setContentOffset:(CGPoint){0,step*44.0f}];
        failures += source->created>3 || [[table visibleCells] count]!=2;
        NSIndexPath *far=[NSIndexPath indexPathForRow:900 inSection:1];
        [table selectRowAtIndexPath:far animated:0 scrollPosition:1];
        failures += ![[table indexPathForSelectedRow] isEqual:far];
        failures += ![[table cellForRowAtIndexPath:far] isSelected];
        failures += source->selected!=0; // Programmatic selection has no delegate callback.
        [table deselectRowAtIndexPath:far animated:0];
        failures += [table indexPathForSelectedRow]!=0;
        [table setContentOffset:(CGPoint){0,0}];
        TableTouch *tableTouch=[[TableTouch alloc] init]; tableTouch->point=(CGPoint){10,10};
        id tableTouches=[NSSet setWithObject:tableTouch];
        [table touchesBegan:tableTouches withEvent:0]; [table touchesEnded:tableTouches withEvent:0];
        failures += source->selected!=1 || ![[table indexPathForSelectedRow] isEqual:first];
        [table touchesBegan:tableTouches withEvent:0]; [table touchesCancelled:tableTouches withEvent:0];
        [table touchesEnded:tableTouches withEvent:0]; failures += source->selected!=1;
        [table touchesBegan:tableTouches withEvent:0];
        tableTouch->previous=tableTouch->point; tableTouch->point=(CGPoint){10,-12};
        [table touchesMoved:tableTouches withEvent:0]; [table touchesEnded:tableTouches withEvent:0];
        failures += source->selected!=1;
        [table reloadData]; failures += [table indexPathForSelectedRow]!=0;
        [table release]; [source release];
        failures += ![[resource localizedStringForKey:@"fixture-key" value:0 table:0] isEqualToString:@"Resource value"];
        failures += ![[main localizedStringForKey:@"fixture-key" value:0 table:0] isEqualToString:@"fixture-key"];
        [control touchesCancelled:touches withEvent:0];
        failures += [control isTracking] || [control isHighlighted] || target->ended || target->cancelled!=1;
        [control touchesCancelled:touches withEvent:0]; // No duplicate action.
        failures += target->cancelled!=1;
        [control touchesBegan:touches withEvent:0];
        [control cancelTrackingWithEvent:0];
        failures += [control isTracking] || [control isHighlighted] || target->ended;
        printf("S5LBOX-HLE-CANCEL %s failures=%u\n",failures?"FAIL":"PASS",failures);
        fflush(stdout); return failures?1:0;
    }
    return UIApplicationMain(argc,argv,0,@"HLEVisualDelegate");
}
