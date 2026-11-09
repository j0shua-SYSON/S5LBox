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
typedef void *Method;
extern Method class_getInstanceMethod(Class,SEL);
extern Method class_getClassMethod(Class,SEL);
extern BOOL class_addMethod(Class,SEL,IMP,const char *);
extern IMP class_replaceMethod(Class,SEL,IMP,const char *);
extern IMP method_getImplementation(Method);
extern SEL method_getName(Method);
extern const char *method_getTypeEncoding(Method);
extern IMP method_setImplementation(Method,IMP);
extern void method_exchangeImplementations(Method,Method);
typedef void *Ivar;
extern Ivar class_getInstanceVariable(Class,const char *);
extern Ivar *class_copyIvarList(Class,unsigned *);
extern const char *ivar_getName(Ivar);
extern const char *ivar_getTypeEncoding(Ivar);
extern int ivar_getOffset(Ivar);
extern id object_getIvar(id,Ivar);
extern void object_setIvar(id,Ivar,id);
typedef struct CGPoint { float x,y; } CGPoint;
typedef struct CGSize { float width,height; } CGSize;
typedef struct CGRect { CGPoint origin; CGSize size; } CGRect;
@interface UIScreenMode
- (CGSize)size;
- (float)pixelAspectRatio;
@end
@interface UIScreen
+ (id)mainScreen;
- (id)currentMode;
- (id)preferredMode;
- (id)availableModes;
- (void)setCurrentMode:(id)mode;
- (CGRect)bounds;
@end
__attribute__((objc_root_class)) @interface NSObject { Class isa; }
+ (id)alloc;
+ (Class)class;
+ (void)load;
+ (IMP)methodForSelector:(SEL)selector;
+ (IMP)instanceMethodForSelector:(SEL)selector;
- (id)init;
- (id)retain;
- (id)autorelease;
- (id)copy;
- (id)mutableCopy;
- (void)release;
- (void)dealloc;
- (unsigned)retainCount;
- (id)valueForKey:(id)key;
- (id)valueForKeyPath:(id)path;
- (id)valueForUndefinedKey:(id)key;
- (void)setValue:(id)value forKey:(id)key;
- (void)setValue:(id)value forKeyPath:(id)path;
- (void)setValue:(id)value forUndefinedKey:(id)key;
- (void)setNilValueForKey:(id)key;
+ (BOOL)accessInstanceVariablesDirectly;
- (void)performSelector:(SEL)selector withObject:(id)object afterDelay:(double)delay;
+ (void)cancelPreviousPerformRequestsWithTarget:(id)target;
- (IMP)methodForSelector:(SEL)selector;
@end
@interface NSAutoreleasePool : NSObject @end
@interface RuntimeBase : NSObject { @public int count; id object; }
+ (int)classAnswer;
- (int)first;
- (int)second;
@end
@implementation RuntimeBase
+ (int)classAnswer { return 3; }
- (int)first { return 11; }
- (int)second { return 22; }
@end
@interface RuntimeChild : RuntimeBase { @public unsigned short flag; } @end
@implementation RuntimeChild @end
static int replacementMethod(id self,SEL selector) { (void)self; (void)selector; return 77; }

#if __has_warning("-Wcast-function-type-mismatch")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
#endif
static unsigned methods_smoke(void) {
    unsigned failures=0;
    RuntimeBase *base=[[RuntimeBase alloc] init];
    RuntimeChild *child=[[RuntimeChild alloc] init];
    Method first=class_getInstanceMethod([RuntimeBase class],@selector(first));
    Method second=class_getInstanceMethod([RuntimeBase class],@selector(second));
    failures += !first || !second || first==second;
    failures += first!=class_getInstanceMethod([RuntimeChild class],@selector(first));
    failures += method_getName(first)!=@selector(first);
    failures += !method_getTypeEncoding(first) || method_getTypeEncoding(first)[0]!='i';
    failures += class_getInstanceMethod([RuntimeBase class],@selector(missingMethod))!=NULL;
    Method factory=class_getClassMethod([RuntimeBase class],@selector(classAnswer));
    failures += !factory;
    failures += ((int (*)(id,SEL))method_getImplementation(factory))([RuntimeBase class],@selector(classAnswer))!=3;
    IMP original=method_getImplementation(first);
    method_exchangeImplementations(first,second);
    failures += [base first]!=22 || [child first]!=22 || [base second]!=11;
    failures += ((int (*)(id,SEL))original)(base,@selector(first))!=11;
    method_exchangeImplementations(first,second);
    failures += !class_addMethod([RuntimeChild class],@selector(first),(IMP)replacementMethod,method_getTypeEncoding(first));
    failures += class_addMethod([RuntimeChild class],@selector(first),original,"i@:");
    failures += [base first]!=11 || [child first]!=77;
    Method override=class_getInstanceMethod([RuntimeChild class],@selector(first));
    failures += override==first;
    method_setImplementation(override,original);
    failures += [child first]!=11;
    IMP previous=class_replaceMethod([RuntimeChild class],@selector(first),(IMP)replacementMethod,NULL);
    failures += previous!=original || [child first]!=77;
    // A host method's returned IMP must remain callable after replacement.
    Method description=class_getInstanceMethod([NSObject class],@selector(class));
    IMP host=method_getImplementation(description);
    failures += !host;
    method_setImplementation(description,(IMP)replacementMethod);
    failures += ((Class (*)(id,SEL))host)(base,@selector(class))!=[RuntimeBase class];
    method_setImplementation(description,host);
    Ivar count=class_getInstanceVariable([RuntimeBase class],"count");
    Ivar object=class_getInstanceVariable([RuntimeChild class],"object");
    failures += !count || !object || count!=class_getInstanceVariable([RuntimeChild class],"count");
    failures += class_getInstanceVariable([RuntimeBase class],"flag")!=NULL;
    failures += class_getInstanceVariable(0,"count")!=NULL || class_getInstanceVariable([RuntimeBase class],0)!=NULL;
    failures += strcmp(ivar_getName(count),"count") || strcmp(ivar_getTypeEncoding(count),"i");
    failures += strcmp(ivar_getTypeEncoding(object),"@");
    failures += ivar_getOffset(count)!=(char *)&base->count-(char *)base;
    base->count=123;
    failures += *(int *)((char *)base+ivar_getOffset(count))!=123;
    object_setIvar(child,object,base);
    failures += child->object!=base || object_getIvar(child,object)!=base || [base retainCount]!=1;
    object_setIvar(child,object,0);
    failures += object_getIvar(child,object)!=0 || object_getIvar(0,object)!=0;
    unsigned fields=99;
    Ivar *list=class_copyIvarList([RuntimeBase class],&fields);
    failures += !list || fields!=2 || list[2]!=NULL || list[0]!=count;
    free(list);
    list=class_copyIvarList([RuntimeChild class],&fields);
    failures += !list || fields!=1 || list[1]!=NULL || strcmp(ivar_getName(list[0]),"flag");
    free(list);
    failures += class_copyIvarList(0,&fields)!=NULL || fields!=0;
    [base release]; [child release];
    return failures;
}
#if __has_warning("-Wcast-function-type-mismatch")
#pragma clang diagnostic pop
#endif
@interface NSNumber : NSObject
+ (id)numberWithBool:(BOOL)value;
+ (id)numberWithInt:(int)value;
+ (id)numberWithDouble:(double)value;
+ (id)numberWithChar:(signed char)value;
+ (id)numberWithUnsignedLong:(unsigned long)value;
+ (id)numberWithLongLong:(long long)value;
+ (id)numberWithUnsignedLongLong:(unsigned long long)value;
- (id)initWithUnsignedLong:(unsigned long)value;
- (unsigned long)unsignedLongValue;
- (unsigned long long)unsignedLongLongValue;
- (long)longValue;
- (double)doubleValue;
- (long long)longLongValue;
@end
@interface NSValue : NSObject
+ (id)valueWithCGPoint:(CGPoint)point;
- (CGPoint)CGPointValue;
@end
@interface NSString : NSObject
+ (id)stringWithString:(id)text;
+ (id)stringWithUTF8String:(const char *)text;
- (const char *)UTF8String;
- (BOOL)isEqualToString:(id)other;
- (id)stringByAppendingPathComponent:(id)component;
@end
@interface NSMutableString : NSString
- (void)setString:(id)text;
@end
@interface NSDictionary : NSObject
+ (id)dictionary;
+ (id)dictionaryWithObject:(id)value forKey:(id)key;
- (id)objectForKey:(id)key;
@end
@interface NSMutableDictionary : NSDictionary @end
@interface NSNull : NSObject
+ (id)null;
@end
@interface NSDate : NSObject
+ (id)date;
@end
@interface NSScanner : NSObject
+ (id)scannerWithString:(id)string;
- (BOOL)scanHexInt:(unsigned *)value;
- (BOOL)scanHexLongLong:(unsigned long long *)value;
- (unsigned)scanLocation;
- (void)setScanLocation:(unsigned)position;
@end
@interface NSRunLoop : NSObject
+ (id)currentRunLoop;
- (void)cancelPerformSelectorsWithTarget:(id)target;
- (void)cancelPerformSelector:(SEL)selector target:(id)target argument:(id)argument;
@end
extern id kCFPreferencesCurrentApplication;
extern BOOL CFPreferencesGetAppBooleanValue(id,id,unsigned char *);
extern int CFPreferencesGetAppIntegerValue(id,id,unsigned char *);
extern void CFPreferencesSetAppValue(id,id,id);
extern id CFPreferencesCopyAppValue(id,id);
extern BOOL CFPreferencesAppSynchronize(id);
@interface NSThread : NSObject
+ (id)currentThread;
+ (void)exit;
+ (void)sleepForTimeInterval:(double)seconds;
- (id)initWithTarget:(id)target selector:(SEL)selector object:(id)object;
- (void)start;
- (void)cancel;
- (BOOL)isCancelled;
- (BOOL)isFinished;
- (BOOL)isExecuting;
@end
static volatile unsigned exitTargetReleased, exitWorkerReached;
@interface ExitTarget : NSObject
- (void)worker:(id)sender;
@end
@implementation ExitTarget
- (void)worker:(id)sender {
    (void)sender;
    [NSThread sleepForTimeInterval:0.001];
    exitWorkerReached++;
    [NSThread exit];
    exitWorkerReached+=100;
}
- (void)dealloc { exitTargetReleased++; [super dealloc]; }
@end
typedef unsigned CFTypeID;
extern CFTypeID CFGetTypeID(id object);
#define CF_TYPE(name) extern CFTypeID CF##name##GetTypeID(void)
CF_TYPE(String); CF_TYPE(Number); CF_TYPE(Boolean); CF_TYPE(Array);
CF_TYPE(Dictionary); CF_TYPE(Data); CF_TYPE(Date); CF_TYPE(URL);
CF_TYPE(Set); CF_TYPE(Null); CF_TYPE(CharacterSet); CF_TYPE(Locale);
CF_TYPE(TimeZone); CF_TYPE(RunLoop); CF_TYPE(RunLoopTimer); CF_TYPE(Bundle);
CF_TYPE(UUID); CF_TYPE(Error);
#undef CF_TYPE
extern id const kCFBooleanTrue,kCFBooleanFalse;
extern id CFUUIDCreate(void *allocator);
@interface NSBundle : NSObject
+ (id)mainBundle;
+ (id)allBundles;
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
@interface NSURL : NSObject
+ (id)fileURLWithPath:(id)path;
+ (id)URLWithString:(id)string;
@end
@interface NSData : NSObject
+ (id)dataWithContentsOfURL:(id)url options:(unsigned)options error:(id *)error;
+ (id)dataWithContentsOfFile:(id)path options:(unsigned)options error:(id *)error;
- (unsigned)length;
- (const void *)bytes;
@end
@interface NSError : NSObject
+ (id)errorWithDomain:(id)domain code:(int)code userInfo:(id)info;
- (int)code;
- (id)description;
- (id)localizedDescription;
- (id)domain;
- (id)userInfo;
@end
extern id const NSLocalizedDescriptionKey, NSLocalizedFailureReasonErrorKey;
@interface NSFileManager : NSObject
+ (id)defaultManager;
- (BOOL)createDirectoryAtPath:(id)path withIntermediateDirectories:(BOOL)intermediates attributes:(id)attributes error:(id *)error;
- (BOOL)createFileAtPath:(id)path contents:(id)contents attributes:(id)attributes;
- (BOOL)setAttributes:(id)attributes ofItemAtPath:(id)path error:(id *)error;
- (id)attributesOfItemAtPath:(id)path error:(id *)error;
- (BOOL)removeItemAtPath:(id)path error:(id *)error;
- (BOOL)fileExistsAtPath:(id)path;
@end
extern id NSTemporaryDirectory(void);
extern id const NSFilePosixPermissions;
static unsigned permissions_smoke(BOOL unavailable) {
    unsigned failures=0;
    NSFileManager *fm=[NSFileManager defaultManager];
    id base=[NSTemporaryDirectory() stringByAppendingPathComponent:@"permissions-fixture"];
    id child=[base stringByAppendingPathComponent:@"child"];
    id leaf=[child stringByAppendingPathComponent:@"save"];
    id blocked=[leaf stringByAppendingPathComponent:@"blocked"];
    id later=[base stringByAppendingPathComponent:@"later"];
    [fm removeItemAtPath:leaf error:0]; [fm removeItemAtPath:child error:0];
    [fm removeItemAtPath:later error:0]; [fm removeItemAtPath:base error:0];
    id mode750=[NSDictionary dictionaryWithObject:[NSNumber numberWithInt:0750] forKey:NSFilePosixPermissions];
    id error=0;
    BOOL created=[fm createDirectoryAtPath:child withIntermediateDirectories:1 attributes:mode750 error:&error];
    if (unavailable) {
        failures += created || [fm fileExistsAtPath:base] || error==0;
        return failures;
    }
    failures += !created || error!=0;
    id attrs=[fm attributesOfItemAtPath:base error:&error];
    failures += [[attrs objectForKey:NSFilePosixPermissions] unsignedLongValue]!=0750 || error!=0;
    attrs=[fm attributesOfItemAtPath:child error:&error];
    failures += [[attrs objectForKey:NSFilePosixPermissions] unsignedLongValue]!=0750;
    id mode700=[NSDictionary dictionaryWithObject:[NSNumber numberWithInt:0700] forKey:NSFilePosixPermissions];
    failures += ![fm setAttributes:mode700 ofItemAtPath:child error:&error] || error!=0;
    attrs=[fm attributesOfItemAtPath:child error:&error];
    failures += [[attrs objectForKey:NSFilePosixPermissions] unsignedLongValue]!=0700;
    failures += ![fm createDirectoryAtPath:later withIntermediateDirectories:1 attributes:mode700 error:&error];
    attrs=[fm attributesOfItemAtPath:base error:&error];
    failures += [[attrs objectForKey:NSFilePosixPermissions] unsignedLongValue]!=0750;
    id mode640=[NSDictionary dictionaryWithObject:[NSNumber numberWithInt:0640] forKey:NSFilePosixPermissions];
    failures += ![fm createFileAtPath:leaf contents:0 attributes:mode640];
    attrs=[fm attributesOfItemAtPath:leaf error:&error];
    failures += [[attrs objectForKey:NSFilePosixPermissions] unsignedLongValue]!=0640;
    error=0;
    failures += [fm createDirectoryAtPath:blocked withIntermediateDirectories:1 attributes:0 error:&error] || error==0;
    error=0;
    id invalid=[NSDictionary dictionaryWithObject:[NSNumber numberWithInt:04755] forKey:NSFilePosixPermissions];
    failures += [fm setAttributes:invalid ofItemAtPath:leaf error:&error] || error==0;
    attrs=[fm attributesOfItemAtPath:leaf error:0];
    failures += [[attrs objectForKey:NSFilePosixPermissions] unsignedLongValue]!=0640;
    error=0;
    failures += [fm attributesOfItemAtPath:@"/missing-permissions-fixture" error:&error]!=0;
    failures += [(NSError *)error code]!=260;
    [fm removeItemAtPath:leaf error:0]; [fm removeItemAtPath:child error:0];
    [fm removeItemAtPath:later error:0]; [fm removeItemAtPath:base error:0];
    failures += [fm fileExistsAtPath:base];
    return failures;
}
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
- (CGRect)frame;
- (void)layoutSubviews;
- (void)setBackgroundColor:(id)color;
- (void)addSubview:(id)view;
- (id)subviews;
- (id)superview;
- (void)removeFromSuperview;
- (BOOL)becomeFirstResponder;
- (BOOL)resignFirstResponder;
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
+ (id)array;
+ (id)arrayWithObjects:(id)first,...;
- (unsigned)count;
- (BOOL)containsObject:(id)object;
- (id)objectAtIndex:(unsigned)index;
- (id)sortedArrayUsingDescriptors:(id)descriptors;
@end
@interface NSMutableArray : NSArray
- (void)sortUsingDescriptors:(id)descriptors;
@end
@interface NSSortDescriptor : NSObject
- (id)initWithKey:(id)key ascending:(BOOL)ascending;
- (id)initWithKey:(id)key ascending:(BOOL)ascending selector:(SEL)selector;
+ (id)sortDescriptorWithKey:(id)key ascending:(BOOL)ascending;
+ (id)sortDescriptorWithKey:(id)key ascending:(BOOL)ascending selector:(SEL)selector;
- (id)key;
- (BOOL)ascending;
- (SEL)selector;
- (id)reversedSortDescriptor;
- (int)compareObject:(id)left toObject:(id)right;
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
- (void)touchesEnded:(id)touches withEvent:(id)event;
- (void)touchesCancelled:(id)touches withEvent:(id)event;
- (void)cancelTrackingWithEvent:(id)event;
- (BOOL)isTracking;
- (BOOL)isHighlighted;
- (BOOL)isEnabled;
@end
@interface UIButton : UIControl
+ (id)buttonWithType:(int)type;
- (void)setTitle:(id)title forState:(unsigned)state;
- (void)setImage:(id)image forState:(unsigned)state;
- (id)imageView;
- (id)titleLabel;
@end
@interface UIImage : NSObject
+ (id)imageWithCGImage:(void *)image;
@end
@interface NSSet : NSObject
+ (id)setWithObject:(id)object;
@end
@interface UIBarButtonItem : NSObject
- (id)initWithTitle:(id)title style:(int)style target:(id)target action:(SEL)action;
- (id)initWithBarButtonSystemItem:(int)item target:(id)target action:(SEL)action;
- (id)initWithCustomView:(id)view;
- (void)setTitle:(id)title;
- (id)title;
- (void)setEnabled:(BOOL)enabled;
- (BOOL)isEnabled;
- (void)setWidth:(float)width;
- (float)width;
- (id)target;
- (void)setTarget:(id)target;
- (SEL)action;
- (void)setAction:(SEL)action;
@end
@interface UIToolbar : UIView
- (id)items;
- (void)setItems:(id)items;
- (void)setItems:(id)items animated:(BOOL)animated;
- (void)sizeToFit;
- (CGSize)sizeThatFits:(CGSize)size;
@end
@interface ToolbarTarget : UIView { @public unsigned calls; id sender; UIToolbar *toolbar; BOOL clear; }
@end
@implementation ToolbarTarget
- (BOOL)canBecomeFirstResponder { return 1; }
- (void)pressed:(id)item { calls++; sender=item; if (clear) [toolbar setItems:0]; }
@end
static void toolbar_click(UIControl *button) {
    [button retain];
    TableTouch *touch=[[TableTouch alloc] init]; touch->point=(CGPoint){4,4};
    id touches=[NSSet setWithObject:touch];
    [button touchesBegan:touches withEvent:0]; [button touchesEnded:touches withEvent:0];
    [touch release]; [button release];
}
static unsigned toolbar_smoke(void) {
    unsigned failures=0;
    ToolbarTarget *target=[[ToolbarTarget alloc] initWithFrame:(CGRect){{0,0},{320,480}}];
    UIToolbar *bar=[[UIToolbar alloc] initWithFrame:(CGRect){{0,400},{320,44}}];
    target->toolbar=bar; [target addSubview:bar];
    NSMutableString *title=[@"First" mutableCopy];
    UIBarButtonItem *first=[[UIBarButtonItem alloc] initWithTitle:title style:0 target:target action:@selector(pressed:)];
    [title setString:@"Changed"]; [title release];
    failures += ![[first title] isEqualToString:@"First"] || ![first isEnabled];
    failures += [first target]!=target || [first action]!=@selector(pressed:) || [target retainCount]!=1;
    UIBarButtonItem *space=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:5 target:0 action:0];
    UIBarButtonItem *last=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:0 target:target action:@selector(pressed:)];
    failures += [space action]!=0;
    [first setWidth:50]; [last setWidth:60];
    [bar setItems:[NSArray arrayWithObjects:first,space,last,0] animated:1];
    NSArray *views=[bar subviews];
    failures += [[bar items] count]!=3 || [views count]!=2;
    CGRect a=[[views objectAtIndex:0] frame],b=[[views objectAtIndex:1] frame];
    failures += a.origin.x!=6 || a.size.width!=50 || b.origin.x!=254 || b.size.width!=60;
    toolbar_click([views objectAtIndex:0]);
    failures += target->calls!=1 || target->sender!=first;
    [first setEnabled:0];
    views=[bar subviews]; failures += [[views objectAtIndex:0] isEnabled];
    toolbar_click([views objectAtIndex:0]); failures += target->calls!=1;
    [first setEnabled:1]; [first setAction:0];
    toolbar_click([[bar subviews] objectAtIndex:0]); failures += target->calls!=1;
    [first setAction:@selector(pressed:)]; [first setTarget:0];
    toolbar_click([[bar subviews] objectAtIndex:0]);
    failures += target->calls!=2 || target->sender!=first;
    [target becomeFirstResponder];
    toolbar_click([[bar subviews] objectAtIndex:0]); failures += target->calls!=3;
    [target resignFirstResponder];
    [bar setFrame:(CGRect){{0,400},{200,44}}];
    b=[[[bar subviews] objectAtIndex:1] frame]; failures += b.origin.x!=134;
    UIView *custom=[[UIView alloc] initWithFrame:(CGRect){{0,0},{40,20}}];
    UIBarButtonItem *customItem=[[UIBarButtonItem alloc] initWithCustomView:custom];
    [bar setItems:[NSArray arrayWithObjects:customItem,0]];
    failures += [[[bar subviews] objectAtIndex:0] superview]!=bar || [custom superview]!=bar;
    a=[custom frame]; failures += a.origin.y!=12 || a.size.width!=40 || a.size.height!=20;
    [bar setItems:[NSArray arrayWithObjects:first,0]];
    failures += [custom superview]!=0;
    [first setTarget:target]; target->clear=1;
    toolbar_click([[bar subviews] objectAtIndex:0]);
    failures += target->calls!=4 || [bar items]!=0 || [[bar subviews] count]!=0;
    CGSize fit=[bar sizeThatFits:(CGSize){123,456}]; failures += fit.width!=123 || fit.height!=44;
    [bar sizeToFit]; failures += [bar frame].size.height!=44;
    [bar setItems:[NSArray arrayWithObjects:first,0]];
    [bar removeFromSuperview]; [bar release];
    // Items may outlive a toolbar; notifications must not reference freed views.
    [first setTitle:@"After removal"];
    [customItem release]; [custom release]; [first release]; [space release]; [last release]; [target release];
    printf("S5LBOX-HLE-TOOLBAR %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures;
}
extern void *CGColorSpaceCreateDeviceRGB(void);
extern void CGColorSpaceRelease(void *space);
extern void *CGBitmapContextCreate(void *data,unsigned width,unsigned height,unsigned bits,unsigned stride,void *space,unsigned bitmapInfo);
extern void *CGBitmapContextCreateImage(void *context);
extern void CGContextRelease(void *context);
extern void CGImageRelease(void *image);
typedef struct {
    unsigned version;
    const void *(*getBytePointer)(void *);
    void (*releaseBytePointer)(void *,const void *);
    unsigned (*getBytesAtPosition)(void *,void *,long long,unsigned);
    void (*releaseInfo)(void *);
} DirectCallbacks;
extern void *CGDataProviderCreateDirect(void *,long long,const DirectCallbacks *);
extern void *CGDataProviderRetain(void *);
extern void CGDataProviderRelease(void *);
extern NSData *CGDataProviderCopyData(void *);
typedef struct {
    unsigned pointers,reads,pointerReleases,infoReleases,available,position,failed;
    BOOL fallback;
} ProviderState;
static const char providerBytes[]="0123456789abcdef";
static const void *provider_pointer(void *opaque) {
    ProviderState *state=opaque; state->pointers++;
    return state->fallback?0:providerBytes;
}
static void provider_release_pointer(void *opaque,const void *pointer) {
    ProviderState *state=opaque; state->pointerReleases++;
    state->failed += pointer!=providerBytes || state->infoReleases!=0;
}
static unsigned provider_read(void *opaque,void *buffer,long long position,unsigned count) {
    ProviderState *state=opaque; state->reads++;
    state->failed += position!=state->position || position<0;
    if(position<0 || (unsigned long long)position>=state->available) return 0;
    unsigned n=state->available-(unsigned)position;
    if(n>count)n=count;
    if(n>3)n=3; // Deliberately return short reads across the 64-bit offset ABI.
    memcpy(buffer,providerBytes+position,n); state->position+=(unsigned)n;
    return n;
}
static void provider_release_info(void *opaque) { ((ProviderState *)opaque)->infoReleases++; }
static unsigned provider_smoke(void) {
    unsigned failures=0;
    DirectCallbacks callbacks={0,provider_pointer,provider_release_pointer,provider_read,provider_release_info};
    ProviderState direct={0}; direct.available=16;
    void *provider=CGDataProviderCreateDirect(&direct,16,&callbacks);
    failures += provider==0 || direct.pointers || direct.reads || direct.infoReleases;
    NSData *copy=CGDataProviderCopyData(provider);
    failures += [copy length]!=16 || memcmp([copy bytes],providerBytes,16)!=0;
    failures += direct.pointers!=1 || direct.reads!=0;
    CGDataProviderRetain(provider); CGDataProviderRelease(provider);
    failures += direct.pointerReleases || direct.infoReleases;
    CGDataProviderRelease(provider);
    failures += direct.pointerReleases!=1 || direct.infoReleases!=1 || direct.failed;
    failures += memcmp([copy bytes],providerBytes,16)!=0; [copy release];
    ProviderState fallback={0}; fallback.fallback=1; fallback.available=7;
    provider=CGDataProviderCreateDirect(&fallback,16,&callbacks);
    copy=CGDataProviderCopyData(provider);
    failures += [copy length]!=7 || memcmp([copy bytes],providerBytes,7)!=0;
    [copy release]; CGDataProviderRelease(provider);
    failures += fallback.pointers!=1 || fallback.reads!=4 || fallback.pointerReleases || fallback.infoReleases!=1 || fallback.failed;
    ProviderState positional={0}; positional.available=16;
    callbacks.getBytePointer=0;
    provider=CGDataProviderCreateDirect(&positional,16,&callbacks);
    copy=CGDataProviderCopyData(provider);
    failures += [copy length]!=16 || memcmp([copy bytes],providerBytes,16)!=0;
    [copy release]; CGDataProviderRelease(provider);
    failures += positional.reads!=6 || positional.pointerReleases || positional.infoReleases!=1 || positional.failed;
    ProviderState unread={0}; unread.available=16;
    provider=CGDataProviderCreateDirect(&unread,16,&callbacks); CGDataProviderRelease(provider);
    failures += unread.reads || unread.pointers || unread.pointerReleases || unread.infoReleases!=1;
    failures += CGDataProviderCreateDirect(&unread,-1,&callbacks)!=0;
    callbacks.version=1;
    failures += CGDataProviderCreateDirect(&unread,16,&callbacks)!=0;
    callbacks.version=0; callbacks.getBytesAtPosition=0;
    failures += CGDataProviderCreateDirect(&unread,16,&callbacks)!=0;
    failures += unread.infoReleases!=1;
    return failures;
}
typedef struct { unsigned location,length; } NSRange;
@interface UITextField : UIControl
- (void)setDelegate:(id)delegate;
- (void)setText:(id)text;
- (id)text;
- (BOOL)resignFirstResponder;
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
- (void)worker:(id)sender;
@end
static volatile unsigned workerStarted,workerCancelled;
@implementation CancelTarget
+ (void)load { [super load]; }
- (void)ended:(id)sender { (void)sender; ended++; }
- (void)cancelled:(id)sender { (void)sender; cancelled++; }
- (void)worker:(id)sender {
    (void)sender;
    NSThread *thread=[NSThread currentThread];
    workerStarted=1;
    for (unsigned i=0;i<2000 && ![thread isCancelled];i++)
        [NSThread sleepForTimeInterval:0.001];
    workerCancelled=[thread isCancelled];
}
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
@interface KVCFixture : NSObject {
@public int _count; signed char _tiny; long long _wide; unsigned long _unsigned;
    double _real; CGPoint _point; id _object;
    unsigned char _byte; short _short; unsigned short _ushort;
    unsigned long long _uwide; float _fraction; unsigned _nilCalls, _undefinedCalls;
}
- (int)getPriority;
- (int)priority;
- (float)fraction;
@end
@implementation KVCFixture
- (int)getPriority { return 3; }
- (int)priority { return 99; }
- (float)fraction { return _fraction; }
- (void)setPriority:(int)value { _count=value+1; }
- (void)_setPriority:(int)value { _count=value+100; }
- (void)setFraction:(float)value { _fraction=value; }
- (void)setReal:(double)value { _real=value; }
- (void)_setPoint:(CGPoint)value { _point=value; }
- (id)valueForUndefinedKey:(id)key { (void)key; return @"missing"; }
- (void)setValue:(id)value forUndefinedKey:(id)key { (void)value; (void)key; _undefinedCalls++; }
- (void)setNilValueForKey:(id)key { (void)key; _nilCalls++; }
@end
@interface KVCChild : KVCFixture @end
@implementation KVCChild @end
@interface KVCNoDirect : KVCFixture @end
@implementation KVCNoDirect
+ (BOOL)accessInstanceVariablesDirectly { return 0; }
@end
static unsigned kvc_smoke(void) {
    KVCFixture *object=[[KVCChild alloc] init];
    object->_count=-42; object->_tiny=-7; object->_wide=-9000000000LL;
    object->_unsigned=0xfedcba98UL; object->_real=3.125; object->_point=(CGPoint){1,2};
    object->_fraction=1.5f;
    object->_object=@"key";
    unsigned failures=[[object valueForKey:@"priority"] longValue]!=3;
    failures += [[object valueForKey:@"count"] longValue]!=-42;
    failures += [[object valueForKey:@"tiny"] longValue]!=-7;
    failures += [[object valueForKey:@"wide"] longLongValue]!=-9000000000LL;
    failures += [[object valueForKey:@"unsigned"] unsignedLongValue]!=0xfedcba98UL;
    failures += [[object valueForKey:@"real"] doubleValue]!=3.125;
    failures += [[object valueForKey:@"fraction"] doubleValue]!=1.5;
    CGPoint point=[[object valueForKey:@"point"] CGPointValue];
    failures += point.x!=1 || point.y!=2;
    failures += [object valueForKey:@"object"]!=object->_object;
    failures += [[object valueForKeyPath:@"object.length"] longValue]!=3;
    failures += [[object valueForKey:@"absent"] isEqualToString:@"missing"]==0;
    failures += [[object valueForKey:@""] isEqualToString:@"missing"]==0;
    failures += [[[NSNumber numberWithDouble:4.5] valueForKey:@"doubleValue"] doubleValue]!=4.5;
    object->_object=0;
    failures += [object valueForKeyPath:@"object.length"]!=0;
    [object setValue:[NSNumber numberWithInt:41] forKey:@"priority"];
    failures += object->_count!=42;
    [object setValue:[NSNumber numberWithInt:-56] forKey:@"count"];
    [object setValue:[NSNumber numberWithInt:-12] forKey:@"tiny"];
    [object setValue:[NSNumber numberWithInt:255] forKey:@"byte"];
    [object setValue:[NSNumber numberWithInt:-1234] forKey:@"short"];
    [object setValue:[NSNumber numberWithInt:65535] forKey:@"ushort"];
    [object setValue:[NSNumber numberWithLongLong:-123456789012LL] forKey:@"wide"];
    [object setValue:[NSNumber numberWithUnsignedLongLong:0xfedcba9876543210ULL] forKey:@"uwide"];
    [object setValue:[NSNumber numberWithUnsignedLong:0xffffffffUL] forKey:@"unsigned"];
    [object setValue:[NSNumber numberWithDouble:2.25] forKey:@"fraction"];
    [object setValue:[NSNumber numberWithDouble:-6.125] forKey:@"real"];
    [object setValue:[NSValue valueWithCGPoint:(CGPoint){7,8}] forKey:@"point"];
    failures += object->_count!=-56 || object->_tiny!=-12 || object->_byte!=255;
    failures += object->_short!=-1234 || object->_ushort!=65535;
    failures += object->_wide!=-123456789012LL || object->_uwide!=0xfedcba9876543210ULL;
    failures += object->_unsigned!=0xffffffffUL || object->_fraction!=2.25 || object->_real!=-6.125;
    failures += object->_point.x!=7 || object->_point.y!=8;
    [object setValue:0 forKey:@"count"];
    [object setValue:0 forKey:@"real"];
    failures += object->_nilCalls!=2 || object->_count!=-56 || object->_real!=-6.125;
    [object setValue:0 forKey:@""];
    [object setValue:0 forKey:@"absent"];
    failures += object->_undefinedCalls!=2;
    KVCFixture *child=[[KVCFixture alloc] init];
    [object setValue:child forKey:@"object"];
    failures += [child retainCount]!=2;
    [object setValue:child forKey:@"object"];
    failures += [child retainCount]!=2;
    [object setValue:[NSNumber numberWithInt:123] forKeyPath:@"object.count"];
    failures += child->_count!=123;
    [object setValue:[NSNumber numberWithInt:124] forKeyPath:@"count"];
    failures += object->_count!=124;
    [object setValue:0 forKey:@"object"];
    failures += [child retainCount]!=1 || object->_object!=0;
    [object setValue:0 forKeyPath:@"object.count"];
    [child release];
    [object release];
    KVCNoDirect *disabled=[[KVCNoDirect alloc] init];
    failures += [[disabled valueForKey:@"count"] isEqualToString:@"missing"]==0;
    [disabled setValue:[NSNumber numberWithInt:12] forKey:@"count"];
    failures += disabled->_count!=0 || disabled->_undefinedCalls!=1;
    [disabled release];
    NSScanner *scanner=[NSScanner scannerWithString:@"01234"];
    [scanner setValue:[NSNumber numberWithInt:3] forKey:@"scanLocation"];
    failures += [scanner scanLocation]!=3;
    [scanner setValue:0 forKey:@"charactersToBeSkipped"];
    failures += [scanner valueForKey:@"charactersToBeSkipped"]!=0;
    return failures;
}
static unsigned sort_smoke(void) {
    unsigned failures=0;
    NSMutableString *key=[@"score" mutableCopy];
    NSSortDescriptor *score=[[NSSortDescriptor alloc] initWithKey:key ascending:1];
    [key setString:@"changed"]; [key release];
    failures += ![[score key] isEqualToString:@"score"] || ![score ascending] || [score selector]!=@selector(compare:);
    id low=[NSDictionary dictionaryWithObject:[NSNumber numberWithInt:1] forKey:@"score"];
    id high=[NSDictionary dictionaryWithObject:[NSNumber numberWithInt:3] forKey:@"score"];
    id tie=[NSDictionary dictionaryWithObject:[NSNumber numberWithInt:1] forKey:@"score"];
    id missing=[NSDictionary dictionary];
    failures += [score compareObject:low toObject:high]>=0 || [score compareObject:high toObject:low]<=0;
    failures += [score compareObject:low toObject:tie]!=0;
    failures += [score compareObject:missing toObject:low]>=0 || [score compareObject:low toObject:missing]<=0;
    NSSortDescriptor *reverse=[score reversedSortDescriptor];
    failures += [reverse ascending] || ![[reverse key] isEqualToString:@"score"];
    failures += [reverse compareObject:low toObject:high]<=0;
    id copied=[score copy]; failures += copied!=score; [copied release];
    NSArray *source=[NSArray arrayWithObjects:high,low,tie,missing,0];
    NSArray *sorted=[source sortedArrayUsingDescriptors:[NSArray arrayWithObjects:score,0]];
    failures += [source objectAtIndex:0]!=high || [sorted count]!=4;
    failures += [sorted objectAtIndex:0]!=missing || [sorted objectAtIndex:1]!=low || [sorted objectAtIndex:2]!=tie || [sorted objectAtIndex:3]!=high;
    NSMutableArray *mutable=[source mutableCopy];
    [mutable sortUsingDescriptors:[NSArray arrayWithObjects:reverse,0]];
    failures += [mutable objectAtIndex:0]!=high || [mutable objectAtIndex:3]!=missing;
    [mutable sortUsingDescriptors:[NSArray array]];
    failures += [mutable objectAtIndex:0]!=high || [mutable count]!=4;
    [mutable release];
    id nestedLow=[NSDictionary dictionaryWithObject:low forKey:@"entry"];
    id nestedHigh=[NSDictionary dictionaryWithObject:high forKey:@"entry"];
    NSSortDescriptor *nested=[NSSortDescriptor sortDescriptorWithKey:@"entry.score" ascending:1];
    failures += [nested compareObject:nestedLow toObject:nestedHigh]>=0;
    NSSortDescriptor *text=[NSSortDescriptor sortDescriptorWithKey:0 ascending:1 selector:@selector(caseInsensitiveCompare:)];
    failures += [text key]!=0 || [text selector]!=@selector(caseInsensitiveCompare:);
    failures += [text compareObject:@"a" toObject:@"B"]>=0 || [text compareObject:@"A" toObject:@"a"]!=0;
    // A second descriptor must resolve equal primary values.
    id byLength=[NSSortDescriptor sortDescriptorWithKey:@"length" ascending:1];
    NSArray *strings=[NSArray arrayWithObjects:@"bb",@"a",@"AA",0];
    sorted=[strings sortedArrayUsingDescriptors:[NSArray arrayWithObjects:byLength,text,0]];
    failures += ![[sorted objectAtIndex:0] isEqualToString:@"a"] || ![[sorted objectAtIndex:1] isEqualToString:@"AA"];
    [score release];
    printf("S5LBOX-HLE-SORT %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures;
}
static unsigned foundation_compat_smoke(void) {
    UIScreen *screen=[UIScreen mainScreen];
    UIScreenMode *mode=[screen currentMode];
    CGSize size=[mode size]; CGRect bounds=[screen bounds];
    unsigned screenFailures=!mode || mode!=[screen preferredMode] || size.width!=320 || size.height!=480 ||
        bounds.size.width!=size.width || bounds.size.height!=size.height || [mode pixelAspectRatio]!=1;
    [screen setCurrentMode:mode];
    screenFailures += [screen currentMode]!=mode || [[screen availableModes] count]!=1;
    unsigned failures=0;
    id domain=kCFPreferencesCurrentApplication,key=@"CompatibilityValue";
    unsigned char valid=9;
    CFPreferencesSetAppValue(key,0,domain);
    failures += CFPreferencesGetAppBooleanValue(key,domain,&valid) || valid;
    CFPreferencesSetAppValue(key,@"yEs",domain);
    failures += !CFPreferencesGetAppBooleanValue(key,domain,&valid) || !valid;
    CFPreferencesSetAppValue(key,@"false",domain);
    failures += CFPreferencesGetAppBooleanValue(key,domain,&valid) || !valid;
    CFPreferencesSetAppValue(key,@"1",domain);
    failures += CFPreferencesGetAppBooleanValue(key,domain,&valid) || valid;
    failures += CFPreferencesGetAppIntegerValue(key,domain,&valid)!=1 || !valid;
    CFPreferencesSetAppValue(key,[NSNumber numberWithDouble:1.0],domain);
    failures += CFPreferencesGetAppBooleanValue(key,domain,&valid) || valid;
    failures += CFPreferencesGetAppIntegerValue(key,domain,&valid)!=0 || valid;
    CFPreferencesSetAppValue(key,[NSNumber numberWithBool:1],domain);
    failures += !CFPreferencesGetAppBooleanValue(key,domain,NULL);
    failures += CFPreferencesGetAppIntegerValue(key,domain,&valid)!=0 || valid;
    CFPreferencesSetAppValue(key,@" -42",domain);
    failures += CFPreferencesGetAppIntegerValue(key,domain,&valid)!=-42 || !valid;
    CFPreferencesSetAppValue(key,@"42oops",domain);
    failures += CFPreferencesGetAppIntegerValue(key,domain,&valid)!=0 || valid;
    CFPreferencesSetAppValue(key,0,domain);
    failures += CFPreferencesCopyAppValue(key,domain)!=0;
    failures += !CFPreferencesAppSynchronize(domain);

    unsigned value=0;
    NSScanner *scanner=[NSScanner scannerWithString:@" 0x12Ab! FFFFFFFFFFF."];
    failures += ![scanner scanHexInt:&value] || value!=0x12ab || [scanner scanLocation]!=7;
    [scanner setScanLocation:8];
    failures += ![scanner scanHexInt:&value] || value!=0xffffffffu || [scanner scanLocation]!=20;
    scanner=[NSScanner scannerWithString:@"0Xff!"];
    failures += ![scanner scanHexInt:NULL] || [scanner scanLocation]!=4;
    scanner=[NSScanner scannerWithString:@"0x"];
    failures += ![scanner scanHexInt:&value] || value || [scanner scanLocation]!=1;
    scanner=[NSScanner scannerWithString:@""];
    failures += [scanner scanHexInt:&value];
    scanner=[NSScanner scannerWithString:@"  xyz"];
    failures += [scanner scanHexInt:&value];
    unsigned long long wide=0;
    scanner=[NSScanner scannerWithString:@"10000000000000000!"];
    failures += ![scanner scanHexLongLong:&wide] || wide!=~0ULL || [scanner scanLocation]!=17;

    CancelTarget *target=[[CancelTarget alloc] init],*other=[[CancelTarget alloc] init];
    NSRunLoop *loop=[NSRunLoop currentRunLoop];
    [target performSelector:@selector(ended:) withObject:0 afterDelay:1000];
    [target performSelector:@selector(cancelled:) withObject:0 afterDelay:1000];
    [other performSelector:@selector(ended:) withObject:0 afterDelay:1000];
    failures += [target retainCount]!=3 || [other retainCount]!=2;
    [loop cancelPerformSelector:@selector(ended:) target:target argument:0];
    failures += [target retainCount]!=2 || [other retainCount]!=2;
    [loop cancelPerformSelectorsWithTarget:target];
    failures += [target retainCount]!=1 || [other retainCount]!=2;
    [NSObject cancelPreviousPerformRequestsWithTarget:other];
    failures += [other retainCount]!=1;
    [target release]; [other release];
    return failures+screenFailures+kvc_smoke()+sort_smoke()+toolbar_smoke();
}

int main(int argc,char **argv) {
    (void)[[NSAutoreleasePool alloc] init];
    if (argc>1 && !strcmp(argv[1],"--foundation-compat-smoke")) {
        unsigned failures=foundation_compat_smoke();
        printf("S5LBOX-HLE-FOUNDATION-COMPAT %s failures=%u\n",failures?"FAIL":"PASS",failures);
        return failures?1:0;
    }
    if (argc>1 && !strcmp(argv[1],"--methods-smoke")) {
        unsigned failures=methods_smoke();
        printf("S5LBOX-HLE-METHODS %s failures=%u\n",failures?"FAIL":"PASS",failures);
        return failures?1:0;
    }
    if (argc>1 && (!strcmp(argv[1],"--permissions-smoke") || !strcmp(argv[1],"--permissions-unavailable-smoke"))) {
        unsigned failures=permissions_smoke(!strcmp(argv[1],"--permissions-unavailable-smoke"));
        printf("S5LBOX-HLE-PERMISSIONS %s failures=%u\n",failures?"FAIL":"PASS",failures);
        return failures?1:0;
    }
    if (argc>1 && !strcmp(argv[1],"--provider-smoke")) {
        unsigned failures=provider_smoke();
        printf("S5LBOX-HLE-PROVIDER %s failures=%u\n",failures?"FAIL":"PASS",failures);
        return failures?1:0;
    }
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
        NSArray *initialBundles=[NSBundle allBundles];
        failures += [initialBundles count]!=1 || ![initialBundles containsObject:main];
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
        NSBundle *framework=[NSBundle bundleWithPath:[[main bundlePath] stringByAppendingPathComponent:@"Fixture.framework"]];
        failures += framework==0;
        NSArray *allBundles=[NSBundle allBundles];
        failures += [allBundles count]!=2 || ![allBundles containsObject:main] || ![allBundles containsObject:resource];
        failures += [allBundles containsObject:framework];
        failures += [initialBundles count]!=1; // Enumeration is an immutable snapshot.
        id readError=0;
        NSURL *resourceURL=[NSURL fileURLWithPath:[resource pathForResource:@"Info" ofType:@"plist"]];
        NSData *resourceData=[NSData dataWithContentsOfURL:resourceURL options:1 error:&readError];
        failures += [resourceData length]==0 || readError!=0;
        failures += [NSData dataWithContentsOfFile:@"/missing-fixture" options:0 error:&readError]!=0;
        failures += [(NSError *)readError code]!=260;
        readError=0;
        failures += [NSData dataWithContentsOfURL:[NSURL URLWithString:@"fixture:unsupported"] options:0 error:&readError]!=0;
        failures += [(NSError *)readError code]!=262;
        failures += ![[(NSError *)readError domain] isEqualToString:@"NSCocoaErrorDomain"];
        const char *errorDescription=[[(NSError *)readError description] UTF8String];
        failures += !errorDescription || !strstr(errorDescription,"NSCocoaErrorDomain") || !strstr(errorDescription,"262");
        id errorInfo=[NSDictionary dictionaryWithObject:@"Fixture failure" forKey:@"NSLocalizedDescription"];
        failures += ![NSLocalizedDescriptionKey isEqualToString:@"NSLocalizedDescription"];
        failures += ![NSLocalizedFailureReasonErrorKey isEqualToString:@"NSLocalizedFailureReason"];
        NSError *customError=[NSError errorWithDomain:@"FixtureDomain" code:42 userInfo:errorInfo];
        failures += [customError userInfo]!=errorInfo;
        failures += ![[customError localizedDescription] isEqualToString:@"Fixture failure"];
        errorDescription=[[customError description] UTF8String];
        failures += !errorDescription || !strstr(errorDescription,"FixtureDomain") || !strstr(errorDescription,"42") || !strstr(errorDescription,"Fixture failure");
        // Toll-free families retain their ID across mutable subclasses.
        CFTypeID typeIDs[]={CFStringGetTypeID(),CFNumberGetTypeID(),CFBooleanGetTypeID(),
            CFArrayGetTypeID(),CFDictionaryGetTypeID(),CFDataGetTypeID(),CFDateGetTypeID(),
            CFURLGetTypeID(),CFSetGetTypeID(),CFNullGetTypeID(),CFCharacterSetGetTypeID(),
            CFLocaleGetTypeID(),CFTimeZoneGetTypeID(),CFRunLoopGetTypeID(),
            CFRunLoopTimerGetTypeID(),CFBundleGetTypeID(),CFUUIDGetTypeID(),CFErrorGetTypeID()};
        for(unsigned i=0;i<sizeof(typeIDs)/sizeof(*typeIDs);i++) {
            failures += typeIDs[i]==0;
            for(unsigned j=0;j<i;j++) failures += typeIDs[i]==typeIDs[j];
        }
        failures += CFGetTypeID(@"constant")!=CFStringGetTypeID();
        failures += CFGetTypeID([NSMutableString stringWithString:@"mutable"])!=CFStringGetTypeID();
        failures += CFGetTypeID([NSArray array])!=CFArrayGetTypeID();
        failures += CFGetTypeID([NSMutableArray array])!=CFArrayGetTypeID();
        failures += CFGetTypeID([NSDictionary dictionary])!=CFDictionaryGetTypeID();
        failures += CFGetTypeID([NSMutableDictionary dictionary])!=CFDictionaryGetTypeID();
        failures += CFGetTypeID(resourceData)!=CFDataGetTypeID();
        failures += CFGetTypeID(resourceURL)!=CFURLGetTypeID();
        failures += CFGetTypeID([NSDate date])!=CFDateGetTypeID();
        failures += CFGetTypeID([NSNull null])!=CFNullGetTypeID();
        failures += CFGetTypeID(touches)!=CFSetGetTypeID();
        failures += CFGetTypeID(readError)!=CFErrorGetTypeID();
        failures += CFGetTypeID(main)!=CFBundleGetTypeID();
        id uuid=CFUUIDCreate(0);
        failures += CFGetTypeID(uuid)!=CFUUIDGetTypeID();
        [uuid release];
        failures += CFGetTypeID(kCFBooleanTrue)!=CFBooleanGetTypeID();
        failures += CFGetTypeID(kCFBooleanFalse)!=CFBooleanGetTypeID();
        failures += CFGetTypeID([NSNumber numberWithBool:1])!=CFBooleanGetTypeID();
        failures += CFGetTypeID([NSNumber numberWithInt:1])!=CFNumberGetTypeID();
        failures += CFGetTypeID([NSNumber numberWithInt:0])!=CFNumberGetTypeID();
        failures += CFGetTypeID([NSNumber numberWithChar:1])!=CFNumberGetTypeID();
        // A running worker must observe cancellation and exit cooperatively.
        NSThread *worker=[[NSThread alloc] initWithTarget:target selector:@selector(worker:) object:0];
        failures += [worker isCancelled] || [worker isFinished];
        [worker start];
        for(unsigned i=0;i<1000 && !workerStarted;i++) [NSThread sleepForTimeInterval:0.001];
        failures += !workerStarted || [worker isCancelled];
        [worker cancel]; [worker cancel];
        for(unsigned i=0;i<1000 && ![worker isFinished];i++) [NSThread sleepForTimeInterval:0.001];
        failures += ![worker isCancelled] || !workerCancelled || ![worker isFinished];
        [worker release];
        ExitTarget *exiting=[[ExitTarget alloc] init];
        worker=[[NSThread alloc] initWithTarget:exiting selector:@selector(worker:) object:0];
        [exiting release];
        failures += [worker isExecuting];
        [worker start];
        failures += ![worker isExecuting];
        for(unsigned i=0;i<1000 && ![worker isFinished];i++) [NSThread sleepForTimeInterval:0.001];
        failures += ![worker isFinished] || [worker isExecuting];
        failures += exitWorkerReached!=1 || exitTargetReleased!=1;
        [worker release];
        // Releasing the caller's NSThread reference must not kill its worker.
        exiting=[[ExitTarget alloc] init];
        worker=[[NSThread alloc] initWithTarget:exiting selector:@selector(worker:) object:0];
        [exiting release]; [worker start]; [worker release];
        for(unsigned i=0;i<1000 && exitTargetReleased!=2;i++) [NSThread sleepForTimeInterval:0.001];
        failures += exitWorkerReached!=2 || exitTargetReleased!=2;
        // Image-only buttons used to retain a zero-size UIImageView. Exercise
        // image sizing, frame changes, mixed image/title layout and clearing.
        void *space=CGColorSpaceCreateDeviceRGB();
        void *bitmap=CGBitmapContextCreate(0,20,10,8,80,space,1);
        void *cgImage=CGBitmapContextCreateImage(bitmap);
        UIImage *buttonImage=[UIImage imageWithCGImage:cgImage];
        UIButton *imageButton=[UIButton buttonWithType:0];
        [imageButton setFrame:(CGRect){{0,0},{120,44}}];
        [imageButton setImage:buttonImage forState:0];
        CGRect imageFrame=[[imageButton imageView] frame];
        failures += imageFrame.size.width!=20 || imageFrame.size.height!=10 || imageFrame.origin.x!=50 || imageFrame.origin.y!=17;
        [imageButton setFrame:(CGRect){{0,0},{200,60}}];
        imageFrame=[[imageButton imageView] frame];
        failures += imageFrame.origin.x!=90 || imageFrame.origin.y!=25;
        [imageButton setTitle:@"Open" forState:0];
        imageFrame=[[imageButton imageView] frame];
        CGRect titleFrame=[[imageButton titleLabel] frame];
        failures += titleFrame.size.width<=0 || imageFrame.origin.x+imageFrame.size.width>titleFrame.origin.x;
        [imageButton setImage:0 forState:0];
        imageFrame=[[imageButton imageView] frame];
        failures += imageFrame.size.width!=0 || [[imageButton titleLabel] frame].size.width!=200;
        CGImageRelease(cgImage); CGContextRelease(bitmap); CGColorSpaceRelease(space);
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
