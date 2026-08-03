//
//  S5LBox — VMManualViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMManualViewController.h"

#import "VMSettings.h"

/*
 * The manual, as data. Each entry is a heading and a body; the table renders
 * them and does nothing else. Kept here rather than in a bundled RTF or a web
 * view because the app ships no resources beyond an icon, and a manual that
 * cannot be read without a network is not a manual.
 */
typedef struct {
    __unsafe_unretained NSString *heading;
    __unsafe_unretained NSString *body;
} vm_manual_entry_t;

@implementation VMManualViewController {
    NSArray<NSDictionary<NSString *, NSString *> *> *_entries;
}

- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (!self) return nil;
    self.title = @"Manual";
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.tableView.allowsSelection = NO;
    [self rebuild];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                      target:self
                                                      action:@selector(dismissSelf)];

    [[NSNotificationCenter defaultCenter]
        addObserver:self selector:@selector(rebuild)
               name:VMSettingsDidChangeNotification object:nil];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)dismissSelf {
    if (self.presentingViewController)
        [self dismissViewControllerAnimated:YES completion:nil];
    else
        [self.navigationController popViewControllerAnimated:YES];
}

/* The last two sections only exist in developer mode: somebody who has not
 * turned it on has no switches to be confused by and no console to find. */
- (void)rebuild {
    BOOL dev = [[VMSettings sharedSettings] developerMode];
    NSMutableArray *e = [NSMutableArray array];

    [e addObject:@{ @"h": @"What this is",
        @"b": @"S5LBox emulates the chip inside an iPhone 3G — the Samsung "
              @"S5L8900 — instruction by instruction, from scratch. The goal "
              @"is to run a real, unmodified copy of iPhone OS 3 on it.\n\n"
              @"It is not a simulator and not a skin. Nothing here pretends to "
              @"be an old iPhone; it runs the same processor instructions the "
              @"real one did." }];

    [e addObject:@{ @"h": @"What this app does today",
        @"b": @"With no firmware imported, it runs a small built-in test "
              @"program. That program exercises the parts of the emulator the "
              @"phone version needs — the processor, the serial port and the "
              @"screen — and draws to the display so you can see it working."
              @"\n\nOnce you have imported a kernel, a device tree and a root "
              @"filesystem, opening a machine boots Apple's own iPhone OS "
              @"3.1.3 kernel instead: the same segments at the same addresses, "
              @"the same device tree with the same values written into it, and "
              @"the same boot arguments the desktop version of this project "
              @"uses. The status line under the screen says which of the two "
              @"is running, and it is never a guess — it reports what was "
              @"actually installed.\n\nOn-device runs have booted Apple's "
              @"kernel and launchd. The newest complete SpringBoard and MBX "
              @"graphics evidence still comes from the desktop harness, and "
              @"no current phone run has demonstrated 30 fps. Those are two "
              @"open measurements, not accomplishments this manual will "
              @"borrow from another host." }];

    [e addObject:@{ @"h": @"Machines",
        @"b": @"The first screen is a list of machines. Each one has its own "
              @"disk — its own writable copy of the filesystem, made the first "
              @"time you open it — so what one machine does to iPhone OS does "
              @"not happen to the others.\n\nWhat they SHARE is the firmware "
              @"you imported, which nothing ever writes to, and the switches in "
              @"Settings, which are still one set for the whole app.\n\n"
              @"Swipe a machine for Rename, Duplicate and Delete. Duplicate "
              @"copies the settings, not the disk: the copy builds its own the "
              @"first time you open it. Delete removes that machine's disk too, "
              @"which is most of the space it uses.\n\n"
              @"The gear on this screen opens Settings and firmware import "
              @"without starting a machine first. Tap a machine to open it."
              @"\n\nOnly one machine runs at a time. Each "
              @"needs 128 MB of memory to itself, and the emulator core runs "
              @"one machine per thread by design — so this is a limit of how it "
              @"is built, not a setting." }];

    [e addObject:@{ @"h": @"The screen and the buttons",
        @"b": @"The picture is the guest's own display, copied out of emulated "
              @"video memory when its pixels change and published at most "
              @"thirty times a second. Thirty is a host-side cap, not a claim "
              @"that the guest currently produces 30 fps. Nothing on it is "
              @"drawn by iOS.\n\nThe buttons below it are the five an iPhone 3G "
              @"has: Home, Power, Volume Up, Volume Down and the Ringer switch. "
              @"Touching the screen is passed to the guest as a real touch "
              @"report, in the same format the original digitizer used.\n\n"
              @"If the guest is not listening — and the built-in test program "
              @"never is — the app says so rather than pretending the press "
              @"worked." }];

    [e addObject:@{ @"h": @"Why it needs firmware, and why none is included",
        @"b": @"To run iPhone OS this needs Apple's own software: the kernel, "
              @"the drivers and the filesystem from a real firmware image.\n\n"
              @"None of it is shipped, bundled or downloaded by this app, and "
              @"it never will be — it is Apple's, not ours. You supply your "
              @"own copy. Whatever you supply is never modified: each machine "
              @"gets its own writable copy of the filesystem, and every run "
              @"works on that. Your import is only ever read." }];

    [e addObject:@{ @"h": @"Settings",
        @"b": dev
            ? @"Developer mode is ON, so Settings shows the full option table: "
              @"sixteen switches that mirror the desktop tool exactly, plus "
              @"the instruction cap and the diagnostics pages.\n\nEach row says "
              @"whether it reaches the boot, is fixed into a work image, or "
              @"is unavailable here. That answer comes from the same mapping "
              @"the engine uses rather than a separate UI promise. None "
              @"of them changes the built-in test program, which has no device "
              @"tree to configure.\n\nThe screen also renders them as a command "
              @"line you can run on the desktop, where all sixteen are resolved."
            : @"Settings is deliberately short. The switches that control how a "
              @"real firmware boot is set up — which pieces of hardware the "
              @"guest is told about, which compatibility fixes are applied — "
              @"live behind Developer Mode.\n\nThey are not hidden because they "
              @"are dangerous. Some are experiments, some describe hardware "
              @"that remains absent, and they only matter once you have "
              @"imported firmware. "
              @"They are hidden because the first thing you meet should not be "
              @"a list of device-tree paths." }];

    [e addObject:@{ @"h": @"Developer mode",
        @"b": @"Turning it on adds: the full sixteen-switch option table, the "
              @"raw guest console, the instruction cap, and diagnostics showing "
              @"what the emulated machine has actually done.\n\nIt changes what "
              @"you can see and set. It does not unlock a faster or more "
              @"complete emulator — there is only one engine." }];

    [e addObject:@{ @"h": @"Graphics status",
        @"b": @"The default remains Apple's CPU software renderer because it "
              @"has the strongest cold-boot history. Developer Mode exposes "
              @"the experimental MBX path. The current model completes the "
              @"measured reset, command-ring, 2D and 3D workloads, including "
              @"a live 1,388/1,388 2D and 8,888/8,888 3D interval with no "
              @"decoder rejection or recovery.\n\nThat is substantial graphics "
              @"coverage. It is still not the required final cold boot and it "
              @"is not a measured 30 fps phone result, so the app does not "
              @"silently make it the default." }];

    if (dev) {
        [e addObject:@{ @"h": @"Console",
            @"b": @"Everything the guest has printed to its serial port, which "
                  @"is where a real iPhone's kernel logs go. On a real firmware "
                  @"boot this is the primary evidence of how far it got.\n\n"
                  @"The built-in test program prints a short banner and then "
                  @"very little, so a nearly-empty console here is correct "
                  @"when no firmware is imported. A firmware boot fills it." }];

        [e addObject:@{ @"h": @"A note on the numbers",
            @"b": @"Instruction counts are the honest unit of progress in this "
                  @"project, not seconds — the same boot takes the same number "
                  @"of instructions every time, and runs are bit-exact "
                  @"reproducible.\n\nWall-clock speed depends on your phone and "
                  @"is not yet optimised: there is no dynamic recompiler in the "
                  @"run loop, so every guest instruction is interpreted." }];
    }

    _entries = e;
    [self.tableView reloadData];
}

#pragma mark - Table

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return (NSInteger)_entries.count;
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return 1;
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    return _entries[(NSUInteger)section][@"h"];
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    static NSString *const kCell = @"manual";
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kCell];
    if (!cell)
        cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault
                                      reuseIdentifier:kCell];
    /* numberOfLines = 0 plus an automatic row height is what makes a paragraph
     * render as a paragraph instead of one clipped line. */
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.text = _entries[(NSUInteger)indexPath.section][@"b"];
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    return cell;
}

- (CGFloat)tableView:(UITableView *)tableView
heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    return UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView *)tableView
estimatedHeightForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    return 160.0;
}

@end
