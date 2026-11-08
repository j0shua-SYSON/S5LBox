// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
// Linker-created constants must not run guest initializers before +load.
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
__attribute__((objc_root_class)) @interface NSObject { Class isa; }
+ (Class)class;
+ (void)load;
+ (void)initialize;
- (id)retain;
- (void)release;
@end
@interface NSString : NSObject
- (unsigned)length;
@end
@interface NSNumber : NSObject
- (signed char)boolValue;
@end
extern id NSDefaultRunLoopMode;
extern id kCFBooleanTrue;
extern id kCFBooleanFalse;
extern double kCFCoreFoundationVersionNumber;
extern SEL NSSelectorFromString(id);
extern id NSStringFromSelector(SEL);
extern Class NSClassFromString(id);
extern Class objc_getClass(const char *);
static volatile int loaded, strings, numbers, failures;
#define CHECK(value) do { if (!(value)) { failures++; printf("startup check failed at line %d\n",__LINE__); } } while (0)
@interface StartupOrder : NSObject @end
@implementation StartupOrder
+ (void)load { loaded=1; }
@end
@implementation NSString (StartupProbe)
+ (void)initialize {
    CHECK(loaded);
    // Yielding here used to crash outside a scheduler coroutine.
    usleep(1);
    strings++;
}
@end
@implementation NSNumber (StartupProbe)
+ (void)initialize { CHECK(loaded); usleep(1); numbers++; }
@end
static int parentInitializations, childInitializations, slowInitializations;
@interface ReentrantParent : NSObject @end
@interface ReentrantChild : ReentrantParent @end
@implementation ReentrantParent
+ (void)initialize { parentInitializations++; [ReentrantChild class]; }
@end
@implementation ReentrantChild
+ (void)initialize { childInitializations++; }
@end
@interface ConcurrentInitialization : NSObject
+ (int)answer;
@end
@implementation ConcurrentInitialization
+ (void)initialize { slowInitializations++; usleep(10000); }
+ (int)answer { return 42; }
@end
static void *initialize_on_worker(void *unused) {
    (void)unused;
    return (void *)([ConcurrentInitialization answer]==42 ? 42 : 0);
}
int main(void) {
    CHECK(kCFCoreFoundationVersionNumber==478.26);
    CHECK(loaded && !strings && !numbers);
    CHECK([NSDefaultRunLoopMode length]>0);
    CHECK([kCFBooleanTrue boolValue] && ![kCFBooleanFalse boolValue]);
    [kCFBooleanTrue retain]; [kCFBooleanTrue release];
    [kCFBooleanFalse retain]; [kCFBooleanFalse release];
    CHECK(strings && numbers);
    CHECK(NSSelectorFromString(0)==0 && NSStringFromSelector(0)==0);
    CHECK(NSClassFromString(@"NoSuchS5LBoxClass")==0);
    CHECK(objc_getClass("NoSuchS5LBoxClass")==0 && objc_getClass(0)==0);
    CHECK(NSClassFromString(@"NSString")==[NSString class]);
    CHECK(objc_getClass("NSNumber")==[NSNumber class]);
    SEL selector=NSSelectorFromString(@"length");
    CHECK(selector && [NSStringFromSelector(selector) length]==6);
    [ReentrantChild class];
    CHECK(parentInitializations==1 && childInitializations==1);
    pthread_t a,b; void *first=0,*second=0;
    CHECK(pthread_create(&a,0,initialize_on_worker,0)==0);
    CHECK(pthread_create(&b,0,initialize_on_worker,0)==0);
    CHECK(pthread_join(a,&first)==0 && pthread_join(b,&second)==0);
    CHECK(first==(void *)42 && second==(void *)42 && slowInitializations==1);
    printf("S5LBOX-HLE-STARTUP %s failures=%d\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
