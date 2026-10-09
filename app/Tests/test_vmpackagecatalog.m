// Native host planning tests, not guest installation proof. MIT licensed.
#import <Foundation/Foundation.h>
#import "VMPackageCatalog.h"
#include <stdio.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while(0)
static NSDictionary *Pkg(NSString *name,NSString *version,NSString *depends) {
    return @{@"Package":name,@"Version":version,@"Depends":depends,@"Architecture":@"iphoneos-arm"};
}
static NSDictionary *Installed(NSDictionary *p) { NSMutableDictionary *v=[p mutableCopy]; v[@"Status"]=@"install ok installed"; return v; }
int main(void) { @autoreleasepool {
    for (NSArray *pair in @[@[@"1",@"2"],@[@"1.9",@"1.10"],@[@"1.0~rc1",@"1.0"],@[@"1.0",@"1.0-1"],
        @[@"9.9",@"1:1.0"],@[@"1.0a",@"1.0+"],@[@"1.0-1",@"1.0-2"],@[@"1.0~~",@"1.0~"],@[@"1.00",@"1.0a"]]) {
        CHECK(VMPackageVersionCompare(pair[0],pair[1])<0); CHECK(VMPackageVersionCompare(pair[1],pair[0])>0);
    }
    CHECK(VMPackageVersionCompare(@"1.0",@"1.0-0")==0); CHECK(VMPackageVersionCompare(@"0:1.01",@"1.1")==0);
    CHECK(VMPackageIdentifierValid(@"com.example.tweak")); CHECK(!VMPackageIdentifierValid(@"--root")); CHECK(!VMPackageIdentifierValid(@"x;id"));
    NSError *error=nil;
    NSArray *parsed=[VMPackageCatalog parse:[@"Package: sample\r\nVersion: 1\r\nDescription: hello\r\n world\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error];
    CHECK(parsed.count==1); CHECK([parsed[0][@"Description"] isEqual:@"hello\nworld"]);
    CHECK(![VMPackageCatalog parse:[@"Package: sample\nPackage: duplicate\nVersion: 1\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error]);
    CHECK(![VMPackageCatalog parse:[@" orphan\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error]);
    VMPackageCatalog *c=[VMPackageCatalog new];
    NSDictionary *firmware=Installed(Pkg(@"firmware",@"3.1.3",@""));
    NSDictionary *dep=Pkg(@"support",@"2",@"firmware (>= 3.0), firmware (<< 4.0)");
    NSDictionary *app=Pkg(@"sample",@"1",@"missing | support (>= 2)");
    c.installed=@[firmware]; c.packages=@[app,dep];
    NSArray *plan=[c planInstall:app error:&error]; CHECK(plan.count==2); CHECK([plan[0][@"Package"] isEqual:@"support"]);
    c.installed=@[firmware,Installed(dep)]; CHECK([c planInstall:app error:&error].count==1);
    c.installed=@[firmware,Installed(dep),Installed(app)]; CHECK([c planInstall:app error:&error].count==0);
    CHECK(![c canRemove:dep error:&error]); CHECK([c canRemove:app error:&error]); CHECK(![c canRemove:firmware error:&error]);
    CHECK(![c planInstall:Pkg(@"sample",@"0.9",@"") error:&error]);
    CHECK(![c planInstall:Pkg(@"newapp",@"1",@"firmware (>= 4.0)") error:&error]);
    NSMutableDictionary *provider=[Pkg(@"provider",@"2",@"") mutableCopy]; provider[@"Provides"]=@"virtual";
    c.installed=@[firmware]; c.packages=@[provider];
    CHECK([c planInstall:Pkg(@"newapp",@"1",@"virtual") error:&error].count==2);
    CHECK(![c planInstall:Pkg(@"newapp",@"1",@"virtual (>= 1)") error:&error]);
    NSMutableDictionary *conflict=[Pkg(@"badapp",@"1",@"") mutableCopy]; conflict[@"Conflicts"]=@"firmware";
    CHECK(![c planInstall:conflict error:&error]);
    c.packages=@[Pkg(@"circlea",@"1",@"circleb"),Pkg(@"circleb",@"1",@"circlea")];
    CHECK(![c planInstall:c.packages[0] error:&error]);
    CHECK(![c planInstall:Pkg(@"newapp",@"1",@"support:any") error:&error]);
    CHECK(![c planInstall:Pkg(@"dpkg",@"9",@"") error:&error]);
    NSDictionary *low=Pkg(@"support",@"1",@""), *high=Pkg(@"support",@"2",@"");
    c.packages=@[low,high];
    CHECK(![c planInstall:Pkg(@"newapp",@"1",@"support (>= 2), support (<< 2)") error:&error]);
    CHECK([c planInstall:Pkg(@"newapp",@"1",@"support (>= 2)") error:&error].count==2);
    // A dependency upgrade must not break an already-installed reverse dependency.
    c.installed=@[Installed(low),Installed(Pkg(@"existing",@"1",@"support (<< 2)"))];
    CHECK(![c planInstall:Pkg(@"newapp",@"1",@"support (>= 2)") error:&error]);
    fprintf(stderr,"native package catalog: %u checks, %u failures\n",checks,failures);
    return failures ? 1 : 0;
} }
