/* Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 * ARMv6 UIKit alert lifecycle, touch dispatch and ownership regression. */
#include <stdio.h>
#include <stdlib.h>
typedef signed char BOOL;
typedef struct { float x,y; } CGPoint;
typedef struct { float width,height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
__attribute__((objc_root_class)) @interface NSObject { Class isa; }
+ (id)alloc; + (id)new; + (Class)class;
- (id)init; - (id)retain; - (void)release; - (BOOL)isKindOfClass:(Class)class;
- (void)dealloc;
- (unsigned)retainCount;
@end
@interface NSAutoreleasePool : NSObject @end
@interface NSString : NSObject
- (BOOL)isEqualToString:(id)string;
@end
@interface NSArray : NSObject
- (unsigned)count; - (id)objectAtIndex:(unsigned)index;
@end
@interface NSSet : NSObject
+ (id)setWithObject:(id)object;
@end
@interface UIView : NSObject
- (id)initWithFrame:(CGRect)frame; - (id)subviews;
- (int)tag; - (void)setHidden:(BOOL)hidden;
@end
@interface UIWindow : UIView
- (void)makeKeyAndVisible;
@end
@interface UIApplication : NSObject
+ (id)sharedApplication; - (id)keyWindow;
@end
@interface UIButton : UIView
- (void)touchesBegan:(id)touches withEvent:(id)event;
- (void)touchesEnded:(id)touches withEvent:(id)event;
@end
@interface UIAlertView : UIView
- (id)initWithTitle:(id)title message:(id)message delegate:(id)delegate
    cancelButtonTitle:(id)cancel otherButtonTitles:(id)other,...;
- (void)show; - (BOOL)isVisible;
- (int)numberOfButtons; - (int)cancelButtonIndex; - (int)firstOtherButtonIndex;
- (void)setCancelButtonIndex:(int)index;
- (id)buttonTitleAtIndex:(int)index; - (int)addButtonWithTitle:(id)title;
- (id)title; - (void)setTitle:(id)title;
- (void)setDelegate:(id)delegate;
- (void)dismissWithClickedButtonIndex:(int)index animated:(BOOL)animated;
@end
static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)
@interface AlertTouch : NSObject @end
@implementation AlertTouch
- (CGPoint)locationInView:(id)view { (void)view; return (CGPoint){5,5}; }
@end
@interface AlertObserver : NSObject { @public unsigned sequence; int clicked,mode; }
@end
@implementation AlertObserver
- (void)willPresentAlertView:(UIAlertView *)alert {
    sequence=sequence*10+1; CHECK([alert isVisible]);
    if (mode==2) { mode=0; [alert dismissWithClickedButtonIndex:-1 animated:0]; }
}
- (void)didPresentAlertView:(UIAlertView *)alert { sequence=sequence*10+2; CHECK([alert isVisible]); }
- (void)alertView:(UIAlertView *)alert clickedButtonAtIndex:(int)index {
    sequence=sequence*10+3; clicked=index; CHECK([alert isVisible]);
    if (mode==1) {
        mode=0; [alert dismissWithClickedButtonIndex:index animated:0]; [alert show];
    }
}
- (void)alertView:(UIAlertView *)alert willDismissWithButtonIndex:(int)index {
    (void)index; sequence=sequence*10+4; CHECK([alert isVisible]);
}
- (void)alertView:(UIAlertView *)alert didDismissWithButtonIndex:(int)index {
    (void)index; sequence=sequence*10+5; CHECK(![alert isVisible]);
}
@end
static unsigned releasedObservers,lastSequence;
@interface SelfReleasingObserver : AlertObserver @end
@implementation SelfReleasingObserver
- (void)alertView:(UIAlertView *)alert clickedButtonAtIndex:(int)index {
    [super alertView:alert clickedButtonAtIndex:index];
    if (mode==3) [self release];
}
- (void)alertView:(UIAlertView *)alert willDismissWithButtonIndex:(int)index {
    [super alertView:alert willDismissWithButtonIndex:index];
    if (mode==4) [self release];
}
- (void)dealloc { releasedObservers++; lastSequence=sequence; [super dealloc]; }
@end
static UIButton *find_button(UIView *view,int index) {
    if ([view isKindOfClass:[UIButton class]] && [view tag]==index) return (UIButton *)view;
    NSArray *children=[view subviews];
    for (unsigned i=0;i<[children count];i++) {
        UIButton *found=find_button([children objectAtIndex:i],index);
        if (found) return found;
    }
    return 0;
}
static void click(UIAlertView *alert,int index) {
    UIButton *button=[find_button(alert,index) retain]; CHECK(button!=0);
    if (!button) return;
    id touch=[AlertTouch new],touches=[NSSet setWithObject:touch];
    [button touchesBegan:touches withEvent:0];
    [button touchesEnded:touches withEvent:0];
    [touch release]; [button release];
}
int main(void) {
    setbuf(stdout,NULL);
    NSAutoreleasePool *pool=[NSAutoreleasePool new];
    UIApplication *application=[UIApplication new];
    CHECK([UIApplication sharedApplication]==application);
    UIWindow *base=[[UIWindow alloc] initWithFrame:(CGRect){{0,0},{320,480}}];
    [base makeKeyAndVisible];
    CHECK([application keyWindow]==base);
    AlertObserver *observer=[AlertObserver new]; observer->clicked=-1;
    UIAlertView *alert=[[UIAlertView alloc] initWithTitle:@"Pause" message:@"Choose an action."
        delegate:observer cancelButtonTitle:@"Cancel" otherButtonTitles:@"Resume",@"Quit",@"Help",(id)0];
    CHECK([alert numberOfButtons]==4 && [alert cancelButtonIndex]==0 && [alert firstOtherButtonIndex]==1);
    CHECK([[alert buttonTitleAtIndex:3] isEqualToString:@"Help"]);
    CHECK([alert addButtonWithTitle:@"More"]==4 && [alert numberOfButtons]==5);
    CHECK([observer retainCount]==1); // Delegate is assigned, not retained.
    CHECK(![alert isVisible]);
    [alert show]; [alert show];
    CHECK(observer->sequence==12 && [alert isVisible] && [application keyWindow]!=base);
    [alert setTitle:@"Updated pause"];
    CHECK([[alert title] isEqualToString:@"Updated pause"]);
    click(alert,2);
    CHECK(observer->sequence==12345 && observer->clicked==2 && ![alert isVisible]);
    CHECK([application keyWindow]==base);
    [alert dismissWithClickedButtonIndex:0 animated:0]; CHECK(observer->sequence==12345);

    observer->sequence=0; observer->clicked=-1;
    [alert show]; [alert dismissWithClickedButtonIndex:0 animated:0];
    CHECK(observer->sequence==1245 && observer->clicked==-1);
    CHECK([application keyWindow]==base);

    observer->sequence=0; observer->mode=1;
    [alert show]; click(alert,1);
    CHECK(observer->sequence==1234512 && [alert isVisible]);
    observer->sequence=0; [alert dismissWithClickedButtonIndex:0 animated:0];
    CHECK(observer->sequence==45 && [application keyWindow]==base);

    observer->sequence=0; observer->mode=2;
    [alert show]; CHECK(observer->sequence==145 && ![alert isVisible]);
    CHECK([application keyWindow]==base);

    UIAlertView *nested=[[UIAlertView alloc] initWithTitle:0 message:0 delegate:0
        cancelButtonTitle:0 otherButtonTitles:@"OK",(id)0];
    CHECK([nested cancelButtonIndex]==-1 && [nested firstOtherButtonIndex]==0);
    observer->sequence=0; [alert show]; [nested show];
    id top=[application keyWindow];
    [alert dismissWithClickedButtonIndex:0 animated:0]; CHECK([application keyWindow]==top);
    click(nested,0); CHECK([application keyWindow]==base && ![nested isVisible]);
    for (int mode=3;mode<=4;mode++) {
        SelfReleasingObserver *temporary=[SelfReleasingObserver new]; temporary->mode=mode;
        UIAlertView *transient=[[UIAlertView alloc] initWithTitle:@"Release during callback" message:0
            delegate:temporary cancelButtonTitle:@"Done" otherButtonTitles:(id)0];
        CHECK([temporary retainCount]==1);
        [transient show];
        if (mode==3) click(transient,0);
        else [transient dismissWithClickedButtonIndex:0 animated:0];
        CHECK(releasedObservers==(unsigned)(mode-2));
        CHECK(lastSequence==(mode==3?12345u:1245u));
        CHECK(![transient isVisible] && [application keyWindow]==base);
        [transient setDelegate:0]; [transient release];
    }
    [nested release]; [alert release]; [observer release]; [base release];
    printf("S5LBOX-HLE-ALERTS %s failures=%u\n",failures?"FAIL":"PASS",failures);
    [pool release]; return failures?1:0;
}
