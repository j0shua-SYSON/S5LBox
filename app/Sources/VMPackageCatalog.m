#import "VMPackageCatalog.h"

NSError *VMPackageError(NSString *message) {
    return [NSError errorWithDomain:@"S5LBox.Packages" code:1
        userInfo:@{NSLocalizedDescriptionKey:message ?: @"Package operation failed."}];
}
static NSString *Trim(NSString *s) {
    return [s stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}
static BOOL Match(NSString *s, NSString *pattern) {
    return [s rangeOfString:pattern options:NSRegularExpressionSearch].location != NSNotFound;
}
BOOL VMPackageIdentifierValid(NSString *name) {
    return name.length >= 2 && name.length <= 200 && Match(name, @"^[a-z0-9][a-z0-9+.-]+$");
}
static BOOL Digit(unsigned char c) { return c >= '0' && c <= '9'; }
static int Order(unsigned char c) {
    if (c == '~') return -1;
    if (!c || Digit(c)) return 0;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return c;
    return c + 256;
}
static int Part(const char *a, const char *b) {
    while (*a || *b) {
        while ((*a && !Digit(*a)) || (*b && !Digit(*b))) {
            int d = Order(*a) - Order(*b); if (d) return d;
            if (*a) a++; if (*b) b++;
        }
        while (*a == '0') a++;
        while (*b == '0') b++;
        int first = 0;
        while (Digit(*a) && Digit(*b)) { if (!first) first = *a - *b; a++; b++; }
        if (Digit(*a)) return 1;
        if (Digit(*b)) return -1;
        if (first) return first;
    }
    return 0;
}
NSComparisonResult VMPackageVersionCompare(NSString *a, NSString *b) {
    NSMutableArray *parts = [NSMutableArray array];
    for (NSString *v in @[a ?: @"", b ?: @""]) {
        NSRange colon = [v rangeOfString:@":"];
        NSString *epoch = colon.location == NSNotFound ? @"0" : [v substringToIndex:colon.location];
        NSString *rest = colon.location == NSNotFound ? v : [v substringFromIndex:colon.location + 1];
        NSRange dash = [rest rangeOfString:@"-" options:NSBackwardsSearch];
        [parts addObject:@[epoch, dash.location == NSNotFound ? rest : [rest substringToIndex:dash.location],
            dash.location == NSNotFound ? @"0" : [rest substringFromIndex:dash.location + 1]]];
    }
    for (NSUInteger i = 0; i < 3; i++) {
        int n = Part([parts[0][i] UTF8String], [parts[1][i] UTF8String]);
        if (n) return n < 0 ? NSOrderedAscending : NSOrderedDescending;
    }
    return NSOrderedSame;
}
// Each AND group contains OR alternatives [name, operator, version].
static NSArray *Relations(NSString *text, NSError **error) {
    if (!text.length) return @[];
    NSMutableArray *groups = [NSMutableArray array];
    NSRegularExpression *pattern = [NSRegularExpression regularExpressionWithPattern:
        @"^([a-z0-9][a-z0-9+.-]+)(?:\\s*\\((<<|<=|=|>=|>>)\\s*([^\\s()]+)\\))?$" options:0 error:NULL];
    for (NSString *group in [text componentsSeparatedByString:@","]) {
        NSMutableArray *alternatives = [NSMutableArray array];
        for (NSString *raw in [group componentsSeparatedByString:@"|"]) {
            NSString *s = Trim(raw);
            NSTextCheckingResult *m = [pattern firstMatchInString:s options:0 range:NSMakeRange(0,s.length)];
            if (!m || groups.count > 256 || alternatives.count > 64) {
                if (error) *error = VMPackageError([NSString stringWithFormat:@"Unsupported package relationship: %@", s]);
                return nil;
            }
            NSMutableArray *r = [NSMutableArray array];
            for (NSUInteger n = 1; n <= 3; n++) {
                NSRange range = [m rangeAtIndex:n];
                [r addObject:range.location == NSNotFound ? @"" : [s substringWithRange:range]];
            }
            [alternatives addObject:r];
        }
        [groups addObject:alternatives];
    }
    return groups;
}
static BOOL VersionMatches(NSString *version, NSArray *relation) {
    NSString *op = relation[1];
    if (!op.length) return YES;
    NSComparisonResult c = VMPackageVersionCompare(version, relation[2]);
    if ([op isEqual:@"="]) return c == 0;
    if ([op isEqual:@">="]) return c >= 0;
    if ([op isEqual:@"<="]) return c <= 0;
    if ([op isEqual:@">>"]) return c > 0;
    return c < 0;
}
static BOOL Satisfies(NSDictionary *p, NSArray *r) {
    if ([p[@"Package"] isEqual:r[0]]) return VersionMatches(p[@"Version"], r);
    // iPhone OS 3's dpkg does not implement versioned Provides.
    if ([r[1] length]) return NO;
    for (NSString *v in [p[@"Provides"] componentsSeparatedByString:@","])
        if ([Trim(v) isEqual:r[0]]) return YES;
    return NO;
}
static BOOL GroupSatisfied(NSArray *group, NSArray *packages) {
    for (NSArray *r in group) for (NSDictionary *p in packages) if (Satisfies(p,r)) return YES;
    return NO;
}
static BOOL IsInstalled(NSDictionary *p) { return [p[@"Status"] isEqual:@"install ok installed"] ||
    [p[@"Status"] isEqual:@"hold ok installed"]; }

@implementation VMPackageCatalog {
    NSUInteger _planningSteps;
}
- (instancetype)init { self = [super init]; if (self) { _packages = @[]; _installed = @[]; } return self; }
+ (NSArray<NSDictionary *> *)parse:(NSData *)data error:(NSError **)error {
    if (!data || data.length > 32u*1024u*1024u) {
        if (error) *error = VMPackageError(@"Repository index exceeds the 32 MB limit."); return nil;
    }
    NSString *text = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
    if (!text) text = [[NSString alloc] initWithData:data encoding:NSISOLatin1StringEncoding];
    if ([text rangeOfString:@"\0"].location != NSNotFound) {
        if (error) *error = VMPackageError(@"Invalid NUL in package index."); return nil;
    }
    NSMutableArray *result = [NSMutableArray array];
    NSMutableDictionary *record = [NSMutableDictionary dictionary]; NSString *last = nil;
    for (NSString *raw in [[text stringByAppendingString:@"\n\n"] componentsSeparatedByString:@"\n"]) {
        NSString *line = [raw hasSuffix:@"\r"] ? [raw substringToIndex:raw.length-1] : raw;
        if (!line.length) {
            if (record.count) {
                if (!VMPackageIdentifierValid(record[@"Package"]) || ![record[@"Version"] length] || result.count >= 100000) {
                    if (error) *error = VMPackageError(@"Invalid package identity or too many package records."); return nil;
                }
                [result addObject:[record copy]]; [record removeAllObjects]; last = nil;
            }
        } else if ([line hasPrefix:@" "] || [line hasPrefix:@"\t"]) {
            if (!last) { if (error) *error = VMPackageError(@"Orphan continuation in package index."); return nil; }
            record[last] = [record[last] stringByAppendingFormat:@"\n%@", [line substringFromIndex:1]];
        } else {
            NSRange colon = [line rangeOfString:@":"];
            if (colon.location == NSNotFound || !colon.location || line.length > 65536) {
                if (error) *error = VMPackageError(@"Malformed package index field."); return nil;
            }
            last = [line substringToIndex:colon.location];
            if (record[last]) { if (error) *error = VMPackageError(@"Duplicate package index field."); return nil; }
            record[last] = Trim([line substringFromIndex:colon.location+1]);
        }
    }
    return result;
}
+ (BOOL)protectedPackage:(NSDictionary *)p {
    NSString *name = p[@"Package"];
    return (p[@"Essential"] != nil && [p[@"Essential"] caseInsensitiveCompare:@"yes"] == NSOrderedSame) ||
        [p[@"Status"] hasPrefix:@"hold "] || [name hasPrefix:@"gsc."] ||
        [@[@"firmware", @"dpkg", @"apt", @"apt7", @"apt7-lib", @"apt7-key", @"apt7-https",
           @"cydia", @"cydia-lproj", @"base", @"base-files", @"bash", @"coreutils", @"coreutils-bin",
           @"darwintools", @"launchctl", @"system-cmds"] containsObject:name];
}
- (BOOL)visit:(NSDictionary *)p selected:(NSMutableDictionary *)selected
        active:(NSMutableSet *)active order:(NSMutableArray *)order error:(NSError **)error depth:(NSUInteger)depth {
    NSString *name = p[@"Package"];
    if (++_planningSteps > 4096 || depth > 64 || selected.count >= 128) { if (error) *error = VMPackageError(@"Dependency plan is too large. Use Cydia for this plan."); return NO; }
    if ([active containsObject:name]) { if (error) *error = VMPackageError(@"This dependency cycle requires Cydia. No changes were made."); return NO; }
    if (selected[name]) return [selected[name][@"Version"] isEqual:p[@"Version"]];
    for (NSDictionary *old in self.installed) if ([old[@"Package"] isEqual:name]) {
        if (IsInstalled(old) && [old[@"Version"] isEqual:p[@"Version"]]) return YES;
        if ([VMPackageCatalog protectedPackage:old] || VMPackageVersionCompare(p[@"Version"],old[@"Version"]) < 0) {
            if (error) *error = VMPackageError([NSString stringWithFormat:@"%@ is protected or would be downgraded. Use Cydia for this change.",name]); return NO;
        }
    }
    if ([VMPackageCatalog protectedPackage:p]) { if (error) *error = VMPackageError(@"Core package changes must be made in Cydia."); return NO; }
    if (![@[@"iphoneos-arm",@"all"] containsObject:p[@"Architecture"] ?: @""]) {
        if (error) *error = VMPackageError(@"This package is not for the guest's architecture."); return NO;
    }
    selected[name] = p; [active addObject:name];
    for (NSString *field in @[@"Pre-Depends",@"Depends"]) {
        NSArray *groups = Relations(p[field], error); if (!groups) return NO;
        for (NSArray *group in groups) {
            NSMutableArray *effective = [NSMutableArray arrayWithArray:selected.allValues];
            for (NSDictionary *old in self.installed) if (IsInstalled(old) && !selected[old[@"Package"]]) [effective addObject:old];
            // A selected but still active ancestor is not configured yet.
            NSIndexSet *pending = [effective indexesOfObjectsPassingTest:^BOOL(NSDictionary *q, NSUInteger i, BOOL *stop) {
                (void)i; (void)stop; return [active containsObject:q[@"Package"]];
            }];
            [effective removeObjectsAtIndexes:pending];
            if (GroupSatisfied(group,effective)) continue;
            BOOL found = NO; NSError *lastError = nil;
            for (NSArray *relation in group) {
                NSArray *candidates = [self.packages sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a, NSDictionary *b) {
                    return -VMPackageVersionCompare(a[@"Version"], b[@"Version"]);
                }];
                for (NSDictionary *candidate in candidates) {
                    if (!Satisfies(candidate,relation)) continue;
                    NSMutableDictionary *trial = [selected mutableCopy]; NSMutableSet *visiting = [active mutableCopy];
                    NSMutableArray *trialOrder = [order mutableCopy];
                    if ([self visit:candidate selected:trial active:visiting order:trialOrder error:&lastError depth:depth+1]) {
                        // Reject an alternate version that didn't actually satisfy the constraint.
                        if (!GroupSatisfied(group,trial.allValues)) continue;
                        [selected setDictionary:trial]; [order setArray:trialOrder]; found = YES; break;
                    }
                }
                if (found) break;
            }
            if (!found) { if (error) *error = lastError ?: VMPackageError([NSString stringWithFormat:@"Missing compatible dependency for %@: %@",name,p[field]]); return NO; }
        }
    }
    [active removeObject:name]; [order addObject:p]; return YES;
}
- (BOOL)validate:(NSArray *)effective error:(NSError **)error {
    for (NSDictionary *p in effective) {
        for (NSString *field in @[@"Depends", @"Pre-Depends", @"Conflicts", @"Breaks"]) {
            NSArray *groups = Relations(p[field], error); if (!groups) return NO;
            BOOL conflict = [field isEqual:@"Conflicts"] || [field isEqual:@"Breaks"];
            NSMutableArray *others = [effective mutableCopy]; if (conflict) [others removeObject:p];
            for (NSArray *group in groups) if (GroupSatisfied(group,others) == conflict) {
                if (error) *error = VMPackageError([NSString stringWithFormat:@"%@ has an unsatisfied %@ relationship: %@",p[@"Package"],field,p[field]]);
                return NO;
            }
        }
    }
    return YES;
}
- (NSArray<NSDictionary *> *)planInstall:(NSDictionary *)package error:(NSError **)error {
    _planningSteps=0;
    NSMutableDictionary *selected = [NSMutableDictionary dictionary]; NSMutableArray *order = [NSMutableArray array];
    if (![self visit:package selected:selected active:[NSMutableSet set] order:order error:error depth:0]) return nil;
    NSMutableArray *effective = [NSMutableArray arrayWithArray:selected.allValues];
    for (NSDictionary *old in self.installed) if (IsInstalled(old) && !selected[old[@"Package"]]) [effective addObject:old];
    return [self validate:effective error:error] ? order : nil;
}
- (BOOL)canRemove:(NSDictionary *)package error:(NSError **)error {
    if ([VMPackageCatalog protectedPackage:package]) { if (error) *error = VMPackageError(@"This core or held package is protected. Use Cydia."); return NO; }
    NSMutableArray *remaining = [NSMutableArray array];
    for (NSDictionary *p in self.installed) if (IsInstalled(p) && ![p[@"Package"] isEqual:package[@"Package"]]) [remaining addObject:p];
    return [self validate:remaining error:error];
}
@end
