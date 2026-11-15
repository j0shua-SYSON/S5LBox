// Native host planning tests, not guest installation proof. MIT licensed.
#import <Foundation/Foundation.h>
#import "VMPackageCatalog.h"
#import "VMPackageRepository.h"
#include <stdio.h>
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr,"line %d: %s\n",__LINE__,#x); } } while(0)
static NSDictionary *Pkg(NSString *name,NSString *version,NSString *depends) {
    return @{@"Package":name,@"Version":version,@"Depends":depends,@"Architecture":@"iphoneos-arm"};
}
static NSDictionary *Installed(NSDictionary *p) { NSMutableDictionary *v=[p mutableCopy]; v[@"Status"]=@"install ok installed"; return v; }

static NSURL *repositoryDirectory;
@interface RepositoryFixture : VMPackageRepository
@property(nonatomic, copy) NSDictionary<NSString *, NSData *> *responses;
@end
@implementation RepositoryFixture
+ (NSURL *)directory { return repositoryDirectory; }
- (NSData *)fetch:(NSURL *)url limit:(NSUInteger)limit error:(NSError **)error {
    NSData *data = self.responses[url.absoluteString];
    if (!data || data.length > limit) {
        if (error) *error = VMPackageError(@"Fixture source unavailable.");
        return nil;
    }
    return data;
}
@end

static void TestSources(void) {
    NSString *suite = [@"com.example.package-tests." stringByAppendingString:NSUUID.UUID.UUIDString];
    NSUserDefaults *defaults = [[NSUserDefaults alloc] initWithSuiteName:suite];
    NSArray *sources = [VMPackageRepository sourcesForDefaults:defaults];
    CHECK(sources.count == 3);
    CHECK([sources.lastObject isEqual:[VMPackageRepository bigBossSource]]);
    CHECK([VMPackageRepository sourcesForDefaults:defaults].count == 3);
    [defaults setObject:@[] forKey:@"PackageManagerSources-v1"];
    CHECK([VMPackageRepository sourcesForDefaults:defaults].count == 0); // removal stays removed
    [defaults removePersistentDomainForName:suite];
    NSDictionary *custom = @{@"name":@"Custom", @"base":@"https://example.org/", @"index":@"https://example.org/"};
    [defaults setObject:@[custom] forKey:@"PackageManagerSources-v1"];
    sources = [VMPackageRepository sourcesForDefaults:defaults];
    CHECK(sources.count == 2 && [sources.firstObject isEqual:custom]);
    [defaults removePersistentDomainForName:suite];
    [defaults setObject:@[[VMPackageRepository bigBossSource]] forKey:@"PackageManagerSources-v1"];
    CHECK([VMPackageRepository sourcesForDefaults:defaults].count == 1);
    [defaults removePersistentDomainForName:suite];

    repositoryDirectory = [[NSURL fileURLWithPath:NSTemporaryDirectory() isDirectory:YES]
        URLByAppendingPathComponent:NSUUID.UUID.UUIDString isDirectory:YES];
    [NSFileManager.defaultManager createDirectoryAtURL:repositoryDirectory withIntermediateDirectories:YES attributes:nil error:NULL];
    NSDictionary *bigboss = [VMPackageRepository bigBossSource];
    NSString *index = [bigboss[@"index"] stringByAppendingString:@"Packages"];
    NSData *legacy = [@"Package: legacy.MixedCase\nVersion: 1\nArchitecture: iphoneos-arm\n\nPackage: valid-package\nVersion: 1\nArchitecture: iphoneos-arm\n\n" dataUsingEncoding:NSUTF8StringEncoding];
    NSUInteger skipped = 0; NSError *error = nil;
    CHECK(![VMPackageCatalog parse:legacy error:&error]); // installed state remains strict
    NSArray *records = [VMPackageCatalog parseRepository:legacy skipped:&skipped error:&error];
    CHECK(records.count == 1 && skipped == 1);
    RepositoryFixture *repo = [RepositoryFixture new]; repo.responses = @{index:legacy};
    error = nil;
    records = [repo refresh:@[bigboss,custom] error:&error];
    CHECK(records.count == 1 && error != nil);
    CHECK([records.firstObject[@"Package"] isEqual:@"valid-package"]);
    CHECK([records.firstObject[@"_allowHTTP"] boolValue]);
    repo.responses = @{@"https://example.org/Packages":[@"Package: custom-package\nVersion: 2\nArchitecture: all\n\n" dataUsingEncoding:NSUTF8StringEncoding]};
    error = nil;
    records = [repo refresh:@[bigboss,custom] error:&error];
    CHECK(records.count == 2 && error != nil); // retain failed BigBoss; update the healthy source
    records = [repo refresh:@[custom] error:&error];
    CHECK(records.count == 1 && [records.firstObject[@"Package"] isEqual:@"custom-package"]);
    repo.canceled = YES;
    CHECK(![repo refresh:@[bigboss] error:&error]);
    CHECK([NSArray arrayWithContentsOfURL:[repositoryDirectory URLByAppendingPathComponent:@"catalog.plist"]].count == 1);
    [NSFileManager.defaultManager removeItemAtURL:repositoryDirectory error:NULL];
}
int main(void) { @autoreleasepool {
    TestSources();
    for (NSArray *pair in @[@[@"1",@"2"],@[@"1.9",@"1.10"],@[@"1.0~rc1",@"1.0"],@[@"1.0",@"1.0-1"],
        @[@"9.9",@"1:1.0"],@[@"1.0a",@"1.0+"],@[@"1.0-1",@"1.0-2"],@[@"1.0~~",@"1.0~"],@[@"1.00",@"1.0a"]]) {
        CHECK(VMPackageVersionCompare(pair[0],pair[1])<0); CHECK(VMPackageVersionCompare(pair[1],pair[0])>0);
    }
    CHECK(VMPackageVersionCompare(@"1.0",@"1.0-0")==0); CHECK(VMPackageVersionCompare(@"0:1.01",@"1.1")==0);
    CHECK(VMPackageIdentifierValid(@"com.example.tweak")); CHECK(!VMPackageIdentifierValid(@"--root")); CHECK(!VMPackageIdentifierValid(@"x;id"));
    NSError *error=nil;
    CHECK([VMPackageCatalog requiresRespring:@[Pkg(@"tweak",@"1",@"mobilesubstrate (>= 0.9), firmware")]]);
    CHECK([VMPackageCatalog requiresRespring:@[@{@"Pre-Depends":@"alternative | mobilesubstrate"}]]);
    CHECK([VMPackageCatalog requiresRespring:@[Pkg(@"mobilesubstrate",@"1",@"")]]);
    CHECK(![VMPackageCatalog requiresRespring:@[Pkg(@"tool",@"1",@"not-mobilesubstrate, mobilesubstrate-extra")]]);
    CHECK(![VMPackageCatalog requiresRespring:@[Pkg(@"tool",@"1",@"")]]);
    NSData *abc=[@"abc" dataUsingEncoding:NSASCIIStringEncoding];
    NSMutableDictionary *digest=[@{@"Size":@"3",@"_base":@"https://example.org/",@"MD5sum":@"900150983cd24fb0d6963f7d28e17f72"} mutableCopy];
    CHECK([VMPackageRepository verify:abc package:digest]);
    digest[@"_base"]=@"http://example.org/"; CHECK(![VMPackageRepository verify:abc package:digest]);
    digest[@"_base"]=@"https://example.org/"; digest[@"SHA256"]=@"bad";
    CHECK(![VMPackageRepository verify:abc package:digest]); // no downgrade to matching MD5
    digest[@"SHA256"]=[VMPackageRepository sha256:abc]; CHECK([VMPackageRepository verify:abc package:digest]);
    digest[@"Size"]=@"4"; CHECK(![VMPackageRepository verify:abc package:digest]);
    NSArray *parsed=[VMPackageCatalog parse:[@"Package: sample\r\nVersion: 1\r\nDescription: hello\r\n world\r\n\r\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error];
    CHECK(parsed.count==1); CHECK([parsed[0][@"Description"] isEqual:@"hello\nworld"]);
    CHECK(![VMPackageCatalog parse:[@"Package: sample\nPackage: duplicate\nVersion: 1\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error]);
    CHECK(![VMPackageCatalog parse:[@" orphan\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error]);
    NSArray *removed=[VMPackageCatalog parse:[@"Package: p7zip\nStatus: deinstall ok not-installed\n\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error];
    CHECK(removed.count==1); CHECK(!removed.firstObject[@"Version"]);
    CHECK(![VMPackageCatalog parse:[@"Package: sample\nStatus: install ok installed\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error]);
    CHECK(![VMPackageCatalog parse:[@"Package: sample\n" dataUsingEncoding:NSUTF8StringEncoding] error:&error]);
    VMPackageCatalog *c=[VMPackageCatalog new];
    c.installed=removed;
    CHECK([c planInstall:Pkg(@"p7zip",@"4.57-3p",@"") error:&error].count==1);
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
