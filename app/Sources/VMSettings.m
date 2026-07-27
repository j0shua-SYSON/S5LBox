//
//  S5LBox — the app's settings. See VMSettings.h.
//
//  Every key is read through -objectForKey: and falls back to the table's own
//  default when absent, rather than being seeded with -registerDefaults:. That
//  is not a style preference: registered defaults are a snapshot taken at
//  launch, so a default changed in VMOptions.c would keep the old value for
//  anyone who had already run the app, and the whole reason that table exists
//  is that a default which quietly means two different things falsifies every
//  run recorded against it.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMSettings.h"

#import "VMOptions.h"

NSString *const VMSettingsDidChangeNotification =
    @"VMSettingsDidChangeNotification";

/* The names the importer writes and the emulator accepts, per
 * docs/BOOT_CHAIN.md's "Regenerating the three accepted inputs". They were
 * "kernelcache", "DeviceTree" and "rootfs.dmg" until the importer existed,
 * which named the IPSW's own members rather than anything produced from them. */
NSString *const VMFirmwareKernelFile           = @"kernel.macho";
NSString *const VMFirmwareDeviceTreeFile       = @"devicetree.bin";
NSString *const VMFirmwareRootFilesystemFile   = @"rootfs.img";
NSString *const VMFirmwareJailbreakPayloadFile = @"jailbreak-payload";

static NSString *const kVMOptionKeyPrefix   = @"vm.option.";
static NSString *const kVMInstructionCapKey = @"vm.diag.instructionCap";
static NSString *const kVMPauseInBackground = @"vm.diag.pauseInBackground";
static NSString *const kVMDeveloperMode = @"VMDeveloperMode";
static NSString *const kVMInlineConsole = @"VMInlineConsole";

/*
 * The instruction caps the screen cycles through. 0 first because no limit is
 * the default and the cycle should start where the app starts; the rest are
 * spaced an order of magnitude apart because that is the resolution at which
 * "how far did it get" is actually a question — the demo guest retires a few
 * million a second, and a real boot reaches 4.97e9 before it draws anything.
 */
static const uint64_t kVMInstructionCaps[] = {
    0ull, 10000000ull, 100000000ull, 1000000000ull, 10000000000ull
};
#define kVMInstructionCapCount \
    ((NSUInteger)(sizeof kVMInstructionCaps / sizeof kVMInstructionCaps[0]))

// Declared up front so every call below is checked against a prototype.
@interface VMSettings ()
- (NSUserDefaults *)defaults;
- (NSString *)keyForOptionIndex:(NSUInteger)index;
- (void)publishChange;
@end

@implementation VMSettings

+ (instancetype)sharedSettings {
    static VMSettings *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ shared = [[VMSettings alloc] init]; });
    return shared;
}

- (NSUserDefaults *)defaults {
    return [NSUserDefaults standardUserDefaults];
}

- (void)publishChange {
    /* Posted synchronously on whichever thread wrote, which is always the main
     * thread here: everything that writes is a control in a table view. */
    [[NSNotificationCenter defaultCenter]
        postNotificationName:VMSettingsDidChangeNotification object:self];
}

#pragma mark - Recorded only

- (NSString *)keyForOptionIndex:(NSUInteger)index {
    const vm_option_t *option = vm_option_at((unsigned)index);
    if (!option || !option->name) return nil;
    return [kVMOptionKeyPrefix stringByAppendingString:
            [NSString stringWithUTF8String:option->name]];
}

- (BOOL)valueForOptionIndex:(NSUInteger)index {
    const vm_option_t *option = vm_option_at((unsigned)index);
    if (!option) return NO;

    NSString *key = [self keyForOptionIndex:index];
    NSNumber *stored = key ? [[self defaults] objectForKey:key] : nil;
    if (![stored isKindOfClass:[NSNumber class]]) return option->def ? YES : NO;
    return stored.boolValue;
}

- (void)setValue:(BOOL)value forOptionIndex:(NSUInteger)index {
    NSString *key = [self keyForOptionIndex:index];
    if (!key) return;
    [[self defaults] setBool:value forKey:key];
    [self publishChange];
}

- (NSString *)equivalentToggleArguments {
    const unsigned count = vm_option_count();
    if (count == 0) return @"(no options)";

    /* NSMutableData rather than malloc: the buffer is then owned by ARC and
     * cannot be leaked by an early return added later. */
    NSMutableData *values = [NSMutableData dataWithLength:count * sizeof(bool)];
    bool *slots = (bool *)values.mutableBytes;
    if (!slots) return @"(unavailable)";
    for (unsigned i = 0; i < count; i++)
        slots[i] = [self valueForOptionIndex:i] ? true : false;

    const size_t needed = vm_option_command_line(slots, count, NULL, 0);
    if (needed == 0) return @"(every option is at its default)";

    NSMutableData *text = [NSMutableData dataWithLength:needed + 1];
    char *out = (char *)text.mutableBytes;
    if (!out) return @"(unavailable)";
    vm_option_command_line(slots, count, out, needed + 1);

    NSString *rendered = [NSString stringWithUTF8String:out];
    return rendered ?: @"(unavailable)";
}

#pragma mark - Applied

- (uint64_t)instructionCap {
    NSNumber *stored = [[self defaults] objectForKey:kVMInstructionCapKey];
    if (![stored isKindOfClass:[NSNumber class]]) return 0;
    long long value = stored.longLongValue;
    return value > 0 ? (uint64_t)value : 0;
}

- (void)setInstructionCap:(uint64_t)cap {
    /* Stored as a signed long long because that is what a property list can
     * hold; the caps offered are nowhere near the boundary. */
    [[self defaults] setObject:@((long long)cap) forKey:kVMInstructionCapKey];
    [self publishChange];
}

- (uint64_t)nextInstructionCap {
    const uint64_t current = [self instructionCap];
    for (NSUInteger i = 0; i < kVMInstructionCapCount; i++) {
        if (kVMInstructionCaps[i] != current) continue;
        return kVMInstructionCaps[(i + 1) % kVMInstructionCapCount];
    }
    // A value from an older build, or none: rejoin the cycle at the start.
    return kVMInstructionCaps[0];
}

- (BOOL)jailbreakEnabled {
    /* Both halves, or it is not on. A half-jailbroken machine is a state the
     * harness supports and this switch deliberately cannot express. */
    int cs = vm_option_index("jb-codesign");
    int pl = vm_option_index("jb-payload");
    if (cs < 0 || pl < 0) return NO;
    return [self valueForOptionIndex:(NSUInteger)cs] &&
           [self valueForOptionIndex:(NSUInteger)pl];
}

- (void)setJailbreakEnabled:(BOOL)enabled {
    int cs = vm_option_index("jb-codesign");
    int pl = vm_option_index("jb-payload");
    if (cs >= 0) [self setValue:enabled forOptionIndex:(NSUInteger)cs];
    if (pl >= 0) [self setValue:enabled forOptionIndex:(NSUInteger)pl];
}

- (BOOL)inlineConsole {
    return [[self defaults] boolForKey:kVMInlineConsole];
}

- (void)setInlineConsole:(BOOL)inline_ {
    [[self defaults] setBool:inline_ forKey:kVMInlineConsole];
    [self publishChange];
}

- (BOOL)developerMode {
    /* Absent means off. A first launch is somebody who has not asked for the
     * harness's option table, so they do not get it. */
    return [[self defaults] boolForKey:kVMDeveloperMode];
}

- (void)setDeveloperMode:(BOOL)enabled {
    [[self defaults] setBool:enabled forKey:kVMDeveloperMode];
    [self publishChange];
}

- (BOOL)pausesInBackground {
    NSNumber *stored = [[self defaults] objectForKey:kVMPauseInBackground];
    if (![stored isKindOfClass:[NSNumber class]]) return YES;
    return stored.boolValue;
}

- (void)setPausesInBackground:(BOOL)pauses {
    [[self defaults] setBool:pauses forKey:kVMPauseInBackground];
    [self publishChange];
}

#pragma mark - Firmware

- (NSString *)firmwareDirectory {
    NSArray<NSString *> *documents = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *root = documents.firstObject;
    if (!root) return nil;
    return [root stringByAppendingPathComponent:@"firmware"];
}

- (NSString *)firmwarePathForFile:(NSString *)file {
    NSString *directory = [self firmwareDirectory];
    if (!directory || file.length == 0) return nil;

    NSString *path = [directory stringByAppendingPathComponent:file];
    BOOL isDirectory = NO;
    if (![[NSFileManager defaultManager] fileExistsAtPath:path
                                              isDirectory:&isDirectory])
        return nil;
    return isDirectory ? nil : path;
}

- (NSString *)statusForFirmwareFile:(NSString *)file {
    NSString *path = [self firmwarePathForFile:file];
    if (!path) return @"not supplied";

    NSError *error = nil;
    NSDictionary<NSFileAttributeKey, id> *attributes =
        [[NSFileManager defaultManager] attributesOfItemAtPath:path error:&error];
    NSNumber *size = attributes[NSFileSize];
    if (![size isKindOfClass:[NSNumber class]]) return @"present, size unknown";

    return [NSString stringWithFormat:@"present  ·  %@",
            [NSByteCountFormatter stringFromByteCount:size.longLongValue
                                           countStyle:NSByteCountFormatterCountStyleFile]];
}

#pragma mark - Housekeeping

- (void)resetToDefaults {
    NSUserDefaults *defaults = [self defaults];
    for (unsigned i = 0; i < vm_option_count(); i++) {
        NSString *key = [self keyForOptionIndex:i];
        if (key) [defaults removeObjectForKey:key];
    }
    [defaults removeObjectForKey:kVMInstructionCapKey];
    [defaults removeObjectForKey:kVMPauseInBackground];
    [self publishChange];
}

@end
