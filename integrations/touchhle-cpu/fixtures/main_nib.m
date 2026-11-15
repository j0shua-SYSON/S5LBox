// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
// The application owns the main nib graph even when an outlet is non-retaining.
#include <stdio.h>
#include <stdlib.h>
__attribute__((objc_root_class)) @interface NSObject { Class isa; }
+ (id)alloc;
- (id)init;
- (void)release;
- (void)dealloc;
- (void)performSelector:(SEL)selector withObject:(id)object afterDelay:(double)delay;
@end
@interface NSAutoreleasePool : NSObject @end
@interface NSArray : NSObject
- (unsigned)count;
@end
@interface UIWindow : NSObject
- (void)setTag:(int)tag;
- (int)tag;
@end
@interface UIApplication : NSObject
+ (id)sharedApplication;
- (NSArray *)windows;
@end
@interface UINib : NSObject
+ (id)nibWithNibName:(id)name bundle:(id)bundle;
- (id)instantiateWithOwner:(id)owner options:(id)options;
@end
extern int UIApplicationMain(int,char **,id,id);
static unsigned releasedTokens;
@interface MainNibWindow : UIWindow @end
@implementation MainNibWindow
- (void)dealloc { puts("S5LBOX-HLE-MAIN-NIB FAIL early window destruction"); [super dealloc]; exit(42); }
@end
@interface SecondaryNibToken : NSObject @end
@implementation SecondaryNibToken
- (id)initWithCoder:(id)coder { (void)coder; return [super init]; }
- (void)dealloc { releasedTokens++; [super dealloc]; }
@end
@interface MainNibDelegate : NSObject { UIWindow *_window; }
- (void)setWindow:(UIWindow *)window;
@end
@implementation MainNibDelegate
- (id)initWithCoder:(id)coder { (void)coder; return [super init]; }
- (void)setWindow:(UIWindow *)window { _window=window; }
- (void)applicationDidFinishLaunching:(id)application {
    (void)application;
    [_window setTag:123];
    id pool=[[NSAutoreleasePool alloc] init];
    id nib=[UINib nibWithNibName:@"Secondary" bundle:0];
    NSArray *objects=[nib instantiateWithOwner:0 options:0];
    if([objects count]!=1 || releasedTokens) exit(43);
    [pool release];
    if(releasedTokens!=1) exit(44);
    [self performSelector:@selector(finish) withObject:0 afterDelay:0.01];
}
- (void)finish {
    unsigned failures= !_window || [_window tag]!=123 ||
        [[[UIApplication sharedApplication] windows] count]!=1;
    printf("S5LBOX-HLE-MAIN-NIB %s failures=%u\n",failures?"FAIL":"PASS",failures);
    exit(failures?1:0);
}
@end
int main(int argc,char **argv) {
    return UIApplicationMain(argc,argv,0,0);
}
