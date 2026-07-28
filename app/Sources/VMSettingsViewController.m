//
//  S5LBox — the settings screen. See VMSettingsViewController.h.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMSettingsViewController.h"

#import "VMBootOptions.h"       /* what each switch actually does, per row */
#import "VMEngine.h"            /* +firmwareReadinessSummary, used below */
#import "VMFirmwareImportViewController.h"
#import "VMOptions.h"
#import "VMManualViewController.h"
#import "VMSettings.h"

#import <math.h>

/*
 * WHAT THE SWITCHES DO, ASKED OF THE CODE THAT DOES IT.
 *
 * This screen used to state, in a banner and in every group footer, that none
 * of these rows was applied. That was true and then it was not, and nothing
 * would have made it stop being displayed. It now asks VMBootOptions for the
 * fate of each row with the values currently set, and prints what it is told;
 * a row whose mapping changes changes here with no edit.
 *
 * Recomputed on every reload rather than cached: it depends on the switches,
 * and the switches change on this screen.
 */
static void VMResolveOptions(vm_boot_options_report_t *report) {
    bool values[VM_BOOT_OPTION_MAX];
    unsigned count = vm_option_count();
    if (count > VM_BOOT_OPTION_MAX) count = VM_BOOT_OPTION_MAX;
    for (unsigned i = 0; i < count; i++)
        values[i] = [[VMSettings sharedSettings] valueForOptionIndex:i]
                        ? true : false;
    /* NULL request: nothing is being started, only described. */
    vm_boot_options_apply(values, count, NULL, report);
}

typedef NS_ENUM(NSInteger, VMSettingsSection) {
    /* The first VM_OPT_GROUP_COUNT sections are the option table's own groups,
     * in its order, so a row added to VMOptions.c appears here with no edit. */
    VMSettingsSectionGeneral = VM_OPT_GROUP_COUNT,
    VMSettingsSectionSnapshots,
    VMSettingsSectionFirmware,
    VMSettingsSectionDiagnostics,
    VMSettingsSectionCommandLine,
    VMSettingsSectionReset,
    VMSettingsSectionCount
};

/*
 * Automatic snapshots, and this section is deliberately NOT behind developer
 * mode. Deciding how much work you are willing to lose is an ordinary thing
 * to want, not a diagnostic, and someone who never turns developer mode on is
 * exactly the person who would rather not lose a boot they waited ten minutes
 * for.
 *
 * The keep count only appears when pruning is on. A row that says "keep 5"
 * above a switch that is off is a row that lies about what will happen.
 */
typedef NS_ENUM(NSInteger, VMSnapshotRow) {
    VMSnapshotRowAuto = 0,
    VMSnapshotRowInterval,
    VMSnapshotRowPrune,
    VMSnapshotRowKeep,
    VMSnapshotRowCount
};

typedef NS_ENUM(NSInteger, VMGeneralRow) {
    VMGeneralRowManual = 0,
    VMGeneralRowJailbreak,
    VMGeneralRowDeveloperMode,
    VMGeneralRowCount
};

/* The three status rows keep the indices they always had, and the one thing a
 * person can DO in this section is added after them. */
typedef NS_ENUM(NSInteger, VMFirmwareRow) {
    VMFirmwareRowKernel = 0,
    VMFirmwareRowDeviceTree,
    VMFirmwareRowRootFilesystem,
    VMFirmwareRowImport,
    VMFirmwareRowCount
};

typedef NS_ENUM(NSInteger, VMDiagnosticsRow) {
    VMDiagnosticsRowInstructionCap = 0,
    VMDiagnosticsRowPauseInBackground,
    VMDiagnosticsRowInlineConsole,
    VMDiagnosticsRowCount
};

/* The guest-state group carries one extra row the option table cannot: the
 * payload the filesystem half of --jailbreak would install. */
static const NSInteger kVMGuestStateExtraRows = 1;

static NSString *const kVMSwitchCell = @"switch";
static NSString *const kVMValueCell  = @"value";
static NSString *const kVMPlainCell  = @"plain";

static NSString *VMDescribeInstructionCap(uint64_t cap) {
    if (cap == 0) return @"no limit";
    if (cap >= 1000000000ull)
        return [NSString stringWithFormat:@"%llu G instructions",
                (unsigned long long)(cap / 1000000000ull)];
    if (cap >= 1000000ull)
        return [NSString stringWithFormat:@"%llu M instructions",
                (unsigned long long)(cap / 1000000ull)];
    return [NSString stringWithFormat:@"%llu instructions",
            (unsigned long long)cap];
}

static NSString *VMStringFromC(const char *text) {
    if (!text) return @"";
    return [NSString stringWithUTF8String:text] ?: @"";
}

// Declared up front so every call below is checked against a prototype.
@interface VMSettingsViewController ()
- (void)doneTapped:(id)sender;
- (void)optionSwitchChanged:(UISwitch *)sender;
- (void)pauseSwitchChanged:(UISwitch *)sender;
- (NSArray<NSNumber *> *)optionsInGroup:(NSInteger)group;
- (UITableViewCell *)cellWithIdentifier:(NSString *)identifier
                                  style:(UITableViewCellStyle)style;
- (void)performReset;
- (void)refreshBanner;
- (void)jailbreakToggled:(UISwitch *)sender;
- (void)inlineConsoleChanged:(UISwitch *)sender;
- (void)developerModeToggled:(UISwitch *)sender;
- (void)autoSnapshotToggled:(UISwitch *)sender;
- (void)autoSnapshotPruneToggled:(UISwitch *)sender;
- (NSString *)intervalTextFor:(NSInteger)seconds;
- (void)presentChoices:(NSArray<NSNumber *> *)values
                titled:(NSString *)title
             formatter:(NSString *(^)(NSInteger))format
               current:(NSInteger)current
                chosen:(void (^)(NSInteger))chosen
             fromIndex:(NSIndexPath *)indexPath;
/* These two were missing, which the comment above says cannot happen: clang
 * late-parses method bodies inside an @implementation, so a call before the
 * definition compiles anyway and the invariant this block exists to hold was
 * quietly not held. -rebuildVisibleSections is called from -viewDidLoad, well
 * above its definition. */
- (void)rebuildVisibleSections;
- (NSInteger)sectionAt:(NSInteger)visible;
@end

@implementation VMSettingsViewController {
    NSArray<NSNumber *> *_visible;
    VMSettings *_settings;
    NSArray<NSArray<NSNumber *> *> *_optionsByGroup;
    UILabel *_banner;
    BOOL _copiedCommandLine;
}

#pragma mark - Lifecycle

/* Grouped, not inset-grouped: the sections carry long explanatory footers and
 * the extra inset costs a noticeable amount of their width on a phone. Routed
 * through -initWithStyle:, which is UITableViewController's designated
 * initializer, so this stays a plain convenience initializer. */
- (instancetype)init {
    return [self initWithStyle:UITableViewStyleGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];

    _settings = [VMSettings sharedSettings];
    self.title = @"Settings";
    /* The emulator screen is black by policy, so this one follows rather than
     * flashing white over it when the phone is in light mode. */
    self.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                      target:self
                                                      action:@selector(doneTapped:)];

    NSMutableArray<NSArray<NSNumber *> *> *groups =
        [NSMutableArray arrayWithCapacity:VM_OPT_GROUP_COUNT];
    for (unsigned group = 0; group < (unsigned)VM_OPT_GROUP_COUNT; group++) {
        NSMutableArray<NSNumber *> *rows = [NSMutableArray array];
        for (unsigned i = 0; i < vm_option_count(); i++) {
            const vm_option_t *option = vm_option_at(i);
            if (option && option->group == group)
                [rows addObject:@(i)];
        }
        [groups addObject:[rows copy]];
    }
    _optionsByGroup = [groups copy];

    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 76.0;
    self.tableView.estimatedSectionHeaderHeight = 28.0;
    self.tableView.estimatedSectionFooterHeight = 44.0;

    /* The first thing on the screen is the limitation, not the switches. */
    _banner = [[UILabel alloc] initWithFrame:CGRectZero];
    _banner.numberOfLines = 0;
    _banner.font = [UIFont systemFontOfSize:13.0];
    _banner.textColor = [UIColor systemOrangeColor];
    [self refreshBanner];
    UIView *header = [[UIView alloc] initWithFrame:CGRectZero];
    [header addSubview:_banner];
    self.tableView.tableHeaderView = header;
}

/* Coming back from the importer, the three firmware rows may be describing
 * files that did not exist when this screen was last drawn. Nothing else here
 * is expensive enough for a full reload to matter. */
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.tableView reloadData];
}

/* The header view is laid out by hand because it is not a cell and does not
 * self-size. Re-assigning tableHeaderView is what makes the table adopt a new
 * height, and doing that unconditionally would re-enter layout forever, so it
 * happens only when the height it should be has actually changed. */
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    UIView *header = self.tableView.tableHeaderView;
    if (!header) return;

    const CGFloat width = self.tableView.bounds.size.width;
    if (width <= 0.0) return;

    const CGFloat inset = 20.0;
    const CGFloat margin = 14.0;
    CGSize fit = [_banner sizeThatFits:CGSizeMake(width - 2.0 * inset,
                                                  CGFLOAT_MAX)];
    const CGFloat textHeight = ceil(fit.height);
    _banner.frame = CGRectMake(inset, margin, width - 2.0 * inset, textHeight);

    const CGFloat wanted = textHeight + 2.0 * margin;
    if (fabs(header.frame.size.height - wanted) > 0.5 ||
        fabs(header.frame.size.width - width) > 0.5) {
        header.frame = CGRectMake(0.0, 0.0, width, wanted);
        self.tableView.tableHeaderView = header;
    }
}



/* The banner says the same true thing in both modes, but it cannot name "the
 * three sections below" in a mode that has none -- a caveat that describes a
 * screen the reader is not looking at reads as a bug in the caveat. */

- (void)jailbreakToggled:(UISwitch *)sender {
    [[VMSettings sharedSettings] setJailbreakEnabled:sender.isOn];
    /* Developer mode shows the two halves as separate switches, and they have
     * just both moved. Reload so the screen cannot show this on and one half
     * off at the same time. */
    if ([[VMSettings sharedSettings] developerMode]) [self.tableView reloadData];
}

- (void)inlineConsoleChanged:(UISwitch *)sender {
    [[VMSettings sharedSettings] setInlineConsole:sender.isOn];
}

/*
 * The banner counted the switches nobody applies. It counted them by hand, in
 * a sentence, and the count was "all of them" -- which stopped being true and
 * would have gone on being displayed. The numbers come from VMBootOptions now,
 * computed from the switches as they are actually set, so the banner cannot
 * claim more or less than the mapping does.
 */
- (void)refreshBanner {
    BOOL dev = [[VMSettings sharedSettings] developerMode];
    NSString *readiness = [VMEngine firmwareReadinessSummary];
    if (!dev) {
        _banner.text = [readiness stringByAppendingString:@" See the Manual."];
        return;
    }

    vm_boot_options_report_t report;
    VMResolveOptions(&report);
    NSMutableString *text = [NSMutableString stringWithString:readiness];
    [text appendFormat:
        @"\n\n%u of the %u option switches below %@ a firmware boot, and %u %@ "
        @"written into a machine's work image when that image is made. The "
        @"rest change nothing, and each says so under its own switch.",
        report.applied, report.count,
        report.applied == 1u ? @"reaches" : @"reach",
        report.provisioned, report.provisioned == 1u ? @"is" : @"are"];
    if (report.overridden > 0u) {
        NSString *summary =
            [NSString stringWithUTF8String:report.summary] ?: @"";
        if (summary.length) [text appendFormat:@"\n\n%@", summary];
    }
    [text appendString:
        @"\n\nOnly the rows under Diagnostics take effect immediately."];
    _banner.text = text;
}

#pragma mark - Automatic snapshots

/* Minutes and hours where they read better than seconds, because the value is
 * chosen by a person deciding how much work they can afford to lose. */
- (NSString *)intervalTextFor:(NSInteger)seconds {
    if (seconds % 3600 == 0 && seconds >= 3600)
        return [NSString stringWithFormat:@"%ld hour%s",
                (long)(seconds / 3600), seconds == 3600 ? "" : "s"];
    if (seconds % 60 == 0 && seconds >= 60)
        return [NSString stringWithFormat:@"%ld min", (long)(seconds / 60)];
    return [NSString stringWithFormat:@"%ld sec", (long)seconds];
}

- (void)autoSnapshotToggled:(UISwitch *)sender {
    [[VMSettings sharedSettings] setAutoSnapshotEnabled:sender.on];
    /* The interval row's colour follows this switch, so the section is redrawn
     * rather than just the switch's own cell. */
    [self.tableView reloadData];
}

- (void)autoSnapshotPruneToggled:(UISwitch *)sender {
    [[VMSettings sharedSettings] setAutoSnapshotPruneEnabled:sender.on];
    /* This one adds or removes the Keep row beneath it. */
    [self.tableView reloadData];
}

/*
 * A short list of values in an action sheet.
 *
 * A stepper or a text field would let a user ask for four seconds or four
 * hundred thousand, and VMSettings would clamp it silently -- which is the
 * behaviour that makes a settings screen feel broken. Offering the values that
 * are actually honoured means what is shown is what happens.
 */
- (void)presentChoices:(NSArray<NSNumber *> *)values
                titled:(NSString *)title
             formatter:(NSString *(^)(NSInteger))format
               current:(NSInteger)current
                chosen:(void (^)(NSInteger))chosen
             fromIndex:(NSIndexPath *)indexPath {
    UIAlertController *sheet = [UIAlertController
        alertControllerWithTitle:title
                         message:nil
                  preferredStyle:UIAlertControllerStyleActionSheet];

    for (NSNumber *v in values) {
        NSInteger value = v.integerValue;
        NSString *label = format(value);
        if (value == current) label = [@"✓ " stringByAppendingString:label];
        [sheet addAction:[UIAlertAction actionWithTitle:label
                                                  style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction *a) {
            (void)a;
            chosen(value);
            [self.tableView reloadData];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    /* iPad presents an action sheet as a popover and throws without an anchor. */
    UIPopoverPresentationController *pop = sheet.popoverPresentationController;
    if (pop) {
        UITableViewCell *cell = [self.tableView cellForRowAtIndexPath:indexPath];
        pop.sourceView = cell ?: self.tableView;
        pop.sourceRect = cell ? cell.bounds : CGRectZero;
    }
    [self.tableView deselectRowAtIndexPath:indexPath animated:YES];
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)developerModeToggled:(UISwitch *)sender {
    [[VMSettings sharedSettings] setDeveloperMode:sender.isOn];
    [self rebuildVisibleSections];
    [self refreshBanner];
    [self.view setNeedsLayout];
    /* A full reload rather than an animated section insert: turning the mode on
     * adds five sections at once and removes them again, and an animation that
     * has to be right in both directions is more ways to be wrong than this
     * screen is worth. */
    [self.tableView reloadData];
}

- (void)doneTapped:(id)sender {
    (void)sender;
    [self dismissViewControllerAnimated:YES completion:nil];
}

#pragma mark - Table shape

- (NSArray<NSNumber *> *)optionsInGroup:(NSInteger)group {
    if (group < 0 || group >= (NSInteger)_optionsByGroup.count) return @[];
    return _optionsByGroup[(NSUInteger)group];
}


/*
 * WHICH SECTIONS EXIST, AND WHY THIS IS A MAP RATHER THAN AN if.
 *
 * Off developer mode this screen shows General, Firmware and Reset: three
 * short lists of things a person can decide. On, it also shows the fourteen
 * option-table rows, the diagnostics, and the rendered command line.
 *
 * The table's delegate methods are indexed by VISIBLE section, and every one
 * of them switches on VMSettingsSection. Translating once here means none of
 * them has to know the mode exists -- and, more to the point, means a section
 * cannot be shown while its row count comes from a different one, which is
 * exactly the bug an `if (dev) section += 3` scattered through six methods
 * produces.
 */
- (void)rebuildVisibleSections {
    BOOL dev = [[VMSettings sharedSettings] developerMode];
    NSMutableArray<NSNumber *> *v = [NSMutableArray array];
    [v addObject:@(VMSettingsSectionGeneral)];
    /* Both modes. See the VMSnapshotRow comment. */
    [v addObject:@(VMSettingsSectionSnapshots)];
    if (dev)
        for (NSInteger g = 0; g < VM_OPT_GROUP_COUNT; g++)
            [v addObject:@(g)];
    [v addObject:@(VMSettingsSectionFirmware)];
    if (dev) {
        [v addObject:@(VMSettingsSectionDiagnostics)];
        [v addObject:@(VMSettingsSectionCommandLine)];
    }
    [v addObject:@(VMSettingsSectionReset)];
    _visible = v;
}

/* Visible index -> VMSettingsSection. Out of range returns Reset rather than
 * an option group, because a stale index must not silently address a switch. */
- (NSInteger)sectionAt:(NSInteger)visible {
    if (visible < 0 || (NSUInteger)visible >= _visible.count)
        return VMSettingsSectionReset;
    return _visible[(NSUInteger)visible].integerValue;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    if (!_visible) [self rebuildVisibleSections];
    return (NSInteger)_visible.count;
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section {
    section = [self sectionAt:section];
    if (section == VMSettingsSectionGeneral) return VMGeneralRowCount;
    (void)tableView;
    if (section < VM_OPT_GROUP_COUNT) {
        NSInteger rows = (NSInteger)[self optionsInGroup:section].count;
        if (section == VM_OPT_GROUP_GUEST_STATE) rows += kVMGuestStateExtraRows;
        return rows;
    }
    switch ((VMSettingsSection)section) {
        case VMSettingsSectionSnapshots:
            /* The keep count is hidden while pruning is off, so the row and
             * the behaviour cannot disagree. */
            return [[VMSettings sharedSettings] autoSnapshotPruneEnabled]
                 ? VMSnapshotRowCount : VMSnapshotRowCount - 1;
        case VMSettingsSectionFirmware:    return VMFirmwareRowCount;
        case VMSettingsSectionDiagnostics: return VMDiagnosticsRowCount;
        case VMSettingsSectionCommandLine: return 1;
        case VMSettingsSectionReset:       return 1;
        default:                           return 0;
    }
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section {
    section = [self sectionAt:section];
    if (section == VMSettingsSectionGeneral) return nil;
    (void)tableView;
    if (section < VM_OPT_GROUP_COUNT)
        return VMStringFromC(vm_option_group_title((unsigned)section));
    switch ((VMSettingsSection)section) {
        case VMSettingsSectionSnapshots:   return @"Automatic snapshots";
        case VMSettingsSectionFirmware:    return @"Firmware";
        case VMSettingsSectionDiagnostics: return @"Diagnostics";
        case VMSettingsSectionCommandLine: return @"Equivalent command line";
        default:                           return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView
titleForFooterInSection:(NSInteger)section {
    section = [self sectionAt:section];
    if (section == VMSettingsSectionSnapshots) {
        VMSettings *set = [VMSettings sharedSettings];
        if (![set autoSnapshotEnabled])
            return @"Off. A snapshot is the whole machine, about the size "
                   @"of the guest's memory, so this is left to you rather "
                   @"than filling the disk on its own.";
        return [set autoSnapshotPruneEnabled]
            ? @"Snapshots are taken while a machine is running and kept "
              @"beside it, so they are copied and deleted with it. Only "
              @"AUTOMATIC snapshots are deleted here — ones you take and "
              @"name yourself are never removed."
            : @"Snapshots are taken while a machine is running and kept "
              @"beside it. Nothing is deleted, so they will accumulate "
              @"until you remove them.";
    }
    if (section == VMSettingsSectionGeneral) {
        return [[VMSettings sharedSettings] developerMode]
            ? @"Developer mode is on: the option table, the guest console and "
              @"the diagnostics are shown. Most of the option switches change "
              @"nothing; each one says, under itself, what it does."
            : @"New here? Read the manual first.\n\n"
              @"Jailbreak disables the guest's signature checking and installs "
              @"Cydia into that machine's own files. It applies to a real "
              @"firmware boot, and this app does not perform it yet — today it "
              @"is recorded and not performed.\n\n"
              @"Developer mode adds the full option table, the guest console "
              @"and diagnostics — useful for working on the emulator, noise "
              @"otherwise.";
    }
    (void)tableView;

    if (section < VM_OPT_GROUP_COUNT) {
        NSString *note = VMStringFromC(vm_option_group_note((unsigned)section));
        /*
         * The blanket "RECORDED, NOT APPLIED" that used to be appended here is
         * gone, because it is no longer true of every row and a caveat that is
         * wrong about some rows teaches people to skip it on all of them. The
         * per-group count comes from the mapping; the per-row sentence is
         * under each switch.
         */
        vm_boot_options_report_t report;
        VMResolveOptions(&report);
        unsigned reaching = 0, total = 0;
        for (unsigned i = 0; i < report.count; i++) {
            const vm_option_t *option = vm_option_at(i);
            if (!option || option->group != (unsigned char)section) continue;
            total++;
            if (report.row[i].outcome != VM_BOOT_OPTION_IGNORED) reaching++;
        }
        if (reaching == 0u)
            return [note stringByAppendingString:
                    @"\n\nNONE of the switches in this group reaches the app's "
                    @"own boot. Each says underneath what happens instead."];
        return [note stringByAppendingFormat:
                @"\n\n%u of these %u switches %@ the app's own boot; the rest "
                @"say underneath what happens instead.",
                reaching, total, reaching == 1u ? @"reaches" : @"reach"];
    }

    switch ((VMSettingsSection)section) {
        case VMSettingsSectionFirmware:
            return [NSString stringWithFormat:
                    @"The project ships no Apple firmware, bundles none, and "
                    @"downloads none. The three rows above report whether a "
                    @"file with the expected name is present in\n\n%@\n\n"
                    @"Import unpacks an IPSW you already have into exactly "
                    @"those three files. It cannot finish on its own: every "
                    @"payload in a 3.x IPSW is encrypted with a key that is "
                    @"not in the archive, and this app has none of them and "
                    @"fetches none — it asks you for the ones it needs.\n\n"
                    @"With all three present, opening a machine boots Apple's "
                    @"own kernel. The first open makes a writable copy of the "
                    @"root filesystem beside them — about 450 MB, once — and "
                    @"runs the built-in test guest while it does; reopen the "
                    @"machine afterwards. If a boot cannot be started, the "
                    @"machine says why instead of pretending.\n\n%@",
                    [_settings firmwareDirectory] ?: @"(no documents directory)",
                    [VMEngine firmwareReadinessSummary]];
        case VMSettingsSectionDiagnostics:
            return @"These two are applied. The cap stops the emulator thread "
                    "at a chosen instruction count and leaves the last frame on "
                    "screen; pausing in the background is on by default because "
                    "iOS terminates apps that keep a core busy while hidden.";
        case VMSettingsSectionCommandLine:
            return @"What these switches would spell on a tools/bootkernel "
                    "command line, so a phone session and a desktop session can "
                    "be compared. The app does not run bootkernel.";
        default:
            return nil;
    }
}

#pragma mark - Cells

- (UITableViewCell *)cellWithIdentifier:(NSString *)identifier
                                  style:(UITableViewCellStyle)style {
    UITableViewCell *cell =
        [self.tableView dequeueReusableCellWithIdentifier:identifier];
    if (!cell)
        cell = [[UITableViewCell alloc] initWithStyle:style
                                      reuseIdentifier:identifier];

    /* Reset everything a previous use may have set, so a reused cell never
     * carries a stale switch, colour or accessory into a different row. */
    cell.accessoryView = nil;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.font = [UIFont systemFontOfSize:16.0];
    cell.textLabel.textColor = [UIColor labelColor];
    cell.textLabel.text = nil;
    cell.detailTextLabel.numberOfLines = 0;
    cell.detailTextLabel.font = [UIFont systemFontOfSize:12.0];
    cell.detailTextLabel.textColor = [UIColor secondaryLabelColor];
    cell.detailTextLabel.text = nil;
    return cell;
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    const NSInteger section = [self sectionAt:indexPath.section];
    if (section == VMSettingsSectionGeneral) {
        UITableViewCell *cell =
            [tableView dequeueReusableCellWithIdentifier:@"general"];
        if (!cell)
            cell = [[UITableViewCell alloc]
                       initWithStyle:UITableViewCellStyleDefault
                     reuseIdentifier:@"general"];
        cell.accessoryView = nil;
        if (indexPath.row == VMGeneralRowManual) {
            cell.textLabel.text = @"Manual";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        } else if (indexPath.row == VMGeneralRowJailbreak) {
            cell.textLabel.text = @"Jailbreak";
            cell.accessoryType = UITableViewCellAccessoryNone;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            UISwitch *sw = [[UISwitch alloc] init];
            sw.on = [[VMSettings sharedSettings] jailbreakEnabled];
            [sw addTarget:self action:@selector(jailbreakToggled:)
                 forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = sw;
        } else {
            cell.textLabel.text = @"Developer Mode";
            cell.accessoryType = UITableViewCellAccessoryNone;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            UISwitch *sw = [[UISwitch alloc] init];
            sw.on = [[VMSettings sharedSettings] developerMode];
            [sw addTarget:self action:@selector(developerModeToggled:)
                 forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = sw;
        }
        return cell;
    }

    if ([self sectionAt:indexPath.section] == VMSettingsSectionSnapshots) {
        VMSettings *set = [VMSettings sharedSettings];
        UITableViewCell *cell = [self cellWithIdentifier:kVMPlainCell
                                                   style:UITableViewCellStyleValue1];
        cell.accessoryView = nil;
        cell.accessoryType = UITableViewCellAccessoryNone;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        cell.detailTextLabel.text = nil;
        cell.textLabel.textColor = [UIColor labelColor];

        switch ((VMSnapshotRow)indexPath.row) {
            case VMSnapshotRowAuto: {
                cell.textLabel.text = @"Snapshot automatically";
                UISwitch *sw = [[UISwitch alloc] init];
                sw.on = [set autoSnapshotEnabled];
                [sw addTarget:self action:@selector(autoSnapshotToggled:)
                     forControlEvents:UIControlEventValueChanged];
                cell.accessoryView = sw;
                break;
            }
            case VMSnapshotRowInterval:
                cell.textLabel.text = @"Every";
                cell.detailTextLabel.text =
                    [self intervalTextFor:[set autoSnapshotIntervalSeconds]];
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
                cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
                /* Dimmed rather than hidden while automatic snapshots are off:
                 * the setting still exists and is still what will be used when
                 * the switch above goes on. */
                if (![set autoSnapshotEnabled])
                    cell.textLabel.textColor = [UIColor secondaryLabelColor];
                break;
            case VMSnapshotRowPrune: {
                cell.textLabel.text = @"Delete old automatic snapshots";
                UISwitch *sw = [[UISwitch alloc] init];
                sw.on = [set autoSnapshotPruneEnabled];
                [sw addTarget:self action:@selector(autoSnapshotPruneToggled:)
                     forControlEvents:UIControlEventValueChanged];
                cell.accessoryView = sw;
                break;
            }
            default:
                cell.textLabel.text = @"Keep";
                cell.detailTextLabel.text = [NSString stringWithFormat:
                    @"%ld", (long)[set autoSnapshotKeep]];
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
                cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
                break;
        }
        return cell;
    }

    if (section < VM_OPT_GROUP_COUNT) {
        NSArray<NSNumber *> *rows = [self optionsInGroup:section];

        if (indexPath.row >= (NSInteger)rows.count) {
            // The payload path, which belongs with the jailbreak halves above it.
            UITableViewCell *cell = [self cellWithIdentifier:kVMPlainCell
                                                       style:UITableViewCellStyleSubtitle];
            cell.textLabel.text = @"Jailbreak payload";
            cell.textLabel.textColor = [UIColor secondaryLabelColor];
            cell.detailTextLabel.text = [NSString stringWithFormat:
                @"%@  ·  expected file name \"%@\"",
                [_settings statusForFirmwareFile:VMFirmwareJailbreakPayloadFile],
                VMFirmwareJailbreakPayloadFile];
            return cell;
        }

        const NSUInteger index = rows[(NSUInteger)indexPath.row].unsignedIntegerValue;
        const vm_option_t *option = vm_option_at((unsigned)index);
        UITableViewCell *cell = [self cellWithIdentifier:kVMSwitchCell
                                                   style:UITableViewCellStyleSubtitle];
        if (!option) return cell;

        /*
         * THE ROW'S OWN FATE, UNDER THE ROW. Asked of the same C the engine
         * calls, so a switch cannot be described here one way and honoured
         * another. An ignored row says what the machine does instead; a row
         * whose value is already fixed in a work image says that too.
         */
        vm_boot_options_report_t report;
        VMResolveOptions(&report);
        const vm_boot_option_status_t *fate =
            index < report.count ? &report.row[index] : NULL;

        cell.textLabel.text = VMStringFromC(option->title);
        NSMutableString *detail = [NSMutableString stringWithFormat:
            @"--%s  ·  %@", option->name, VMStringFromC(option->detail)];
        if (fate && fate->note) {
            /*
             * Three prefixes, because the three ways a switch can fail to mean
             * what it looks like are different problems. A PROVISIONED row is
             * flagged even when its position agrees with the mapping: the
             * value that is actually in an existing work image is whatever was
             * set when that image was made, and nothing here can read it back,
             * so "off" next to a machine that has one is not a claim this
             * screen is entitled to make silently.
             */
            NSString *prefix = @"";
            if (fate->effective != fate->requested) prefix = @"NOT APPLIED: ";
            else if (fate->outcome == VM_BOOT_OPTION_PROVISIONED)
                prefix = @"NOT READ AT BOOT: ";
            [detail appendFormat:@"\n%@%@", prefix, VMStringFromC(fate->note)];
        }
        cell.detailTextLabel.text = detail;

        UISwitch *toggle = [[UISwitch alloc] initWithFrame:CGRectZero];
        toggle.tag = (NSInteger)index;
        toggle.on = [_settings valueForOptionIndex:index];
        /*
         * Grey for a row the machine does not read, the system tint for one it
         * does. That distinction did not exist when nothing was applied and
         * everything was grey; now that two rows really do change the boot,
         * making them look the same as the twelve that do not would understate
         * them exactly as badly as the old banner overstated the rest.
         *
         * These are left MOVABLE rather than disabled, which is the one place
         * on this screen that judgement was needed. Recording an intended
         * configuration is a thing the app really can do, and bootkernel's own
         * --activate sets the precedent: it is settable, defaults on, and every
         * run header then says it was requested and not applied. So the
         * treatment here is the same — say NOT APPLIED loudly, under the switch
         * itself, rather than take the switch away and leave the user unable to
         * record anything at all. The controls whose OWN function is missing,
         * the device keys and the firmware rows, are disabled outright instead.
         */
        /* nil restores the system's own "this works" green. Written as an
         * assignment rather than a ternary because `cond ? nil : aColor` makes
         * clang pick the common type of `nil` and UIColor*, and this file has
         * no reason to make a reader check that. */
        UIColor *tint = [UIColor systemGrayColor];
        if (fate && fate->outcome == VM_BOOT_OPTION_APPLIED) tint = nil;
        toggle.onTintColor = tint;
        [toggle addTarget:self
                   action:@selector(optionSwitchChanged:)
         forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle;
        return cell;
    }

    switch ((VMSettingsSection)section) {
        case VMSettingsSectionFirmware: {
            if (indexPath.row == VMFirmwareRowImport) {
                UITableViewCell *cell = [self cellWithIdentifier:kVMPlainCell
                                                           style:UITableViewCellStyleSubtitle];
                cell.textLabel.text = @"Import from an IPSW";
                cell.detailTextLabel.text =
                    @"Unpacks an IPSW you already have into the three files "
                     "above. Some of them need a key that you supply.";
                cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
                return cell;
            }

            NSString *title = @"";
            NSString *file  = @"";
            switch (indexPath.row) {
                case VMFirmwareRowKernel:
                    title = @"Kernel";          file = VMFirmwareKernelFile; break;
                case VMFirmwareRowDeviceTree:
                    title = @"Device tree";     file = VMFirmwareDeviceTreeFile; break;
                case VMFirmwareRowRootFilesystem:
                default:
                    title = @"Root filesystem"; file = VMFirmwareRootFilesystemFile; break;
            }
            UITableViewCell *cell = [self cellWithIdentifier:kVMPlainCell
                                                       style:UITableViewCellStyleSubtitle];
            cell.textLabel.text = title;
            cell.textLabel.textColor = [UIColor secondaryLabelColor];
            cell.detailTextLabel.text = [NSString stringWithFormat:
                @"%@  ·  expected file name \"%@\"",
                [_settings statusForFirmwareFile:file], file];
            return cell;
        }

        case VMSettingsSectionDiagnostics: {
            if (indexPath.row == VMDiagnosticsRowInstructionCap) {
                UITableViewCell *cell = [self cellWithIdentifier:kVMValueCell
                                                           style:UITableViewCellStyleSubtitle];
                cell.textLabel.text = @"Instruction cap";
                cell.detailTextLabel.text = [NSString stringWithFormat:
                    @"%@  ·  applied  ·  tap to change",
                    VMDescribeInstructionCap([_settings instructionCap])];
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
                return cell;
            }

            if (indexPath.row == VMDiagnosticsRowInlineConsole) {
                UITableViewCell *cell = [self cellWithIdentifier:kVMSwitchCell
                                                           style:UITableViewCellStyleSubtitle];
                cell.textLabel.text = @"Console under the screen";
                cell.detailTextLabel.text =
                    @"Applied. Puts the guest's serial output back beneath the "
                     "picture for live debugging, as it was before it moved to "
                     "its own screen. Costs about a third of the picture.";
                UISwitch *t = [[UISwitch alloc] initWithFrame:CGRectZero];
                t.on = [[VMSettings sharedSettings] inlineConsole];
                [t addTarget:self action:@selector(inlineConsoleChanged:)
                    forControlEvents:UIControlEventValueChanged];
                cell.accessoryView = t;
                cell.selectionStyle = UITableViewCellSelectionStyleNone;
                return cell;
            }

            UITableViewCell *cell = [self cellWithIdentifier:kVMSwitchCell
                                                       style:UITableViewCellStyleSubtitle];
            cell.textLabel.text = @"Pause when not frontmost";
            cell.detailTextLabel.text =
                @"Applied. Off keeps the interpreter running in the background, "
                 "which iOS may end the app for.";
            UISwitch *toggle = [[UISwitch alloc] initWithFrame:CGRectZero];
            toggle.on = [_settings pausesInBackground];
            [toggle addTarget:self
                       action:@selector(pauseSwitchChanged:)
             forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = toggle;
            return cell;
        }

        case VMSettingsSectionCommandLine: {
            UITableViewCell *cell = [self cellWithIdentifier:kVMValueCell
                                                       style:UITableViewCellStyleSubtitle];
            cell.textLabel.font = [UIFont fontWithName:@"Menlo" size:12]
                                  ?: [UIFont systemFontOfSize:12];
            cell.textLabel.text = [_settings equivalentToggleArguments];
            cell.detailTextLabel.text = _copiedCommandLine
                ? @"copied to the clipboard" : @"tap to copy";
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            return cell;
        }

        case VMSettingsSectionReset: {
            UITableViewCell *cell = [self cellWithIdentifier:kVMPlainCell
                                                       style:UITableViewCellStyleSubtitle];
            cell.textLabel.text = @"Reset to defaults";
            cell.textLabel.textColor = [UIColor systemRedColor];
            cell.detailTextLabel.text =
                @"Forget every value above, including the two applied ones.";
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            return cell;
        }

        default:
            return [self cellWithIdentifier:kVMPlainCell
                                      style:UITableViewCellStyleSubtitle];
    }
}

#pragma mark - Actions

- (void)optionSwitchChanged:(UISwitch *)sender {
    [_settings setValue:sender.isOn forOptionIndex:(NSUInteger)sender.tag];

    /* The clipboard still holds the OLD arguments, so the "copied" caption
     * would now be attached to a line nobody has copied. On this screen of all
     * screens, a caption that is not true is not acceptable. */
    _copiedCommandLine = NO;

    /*
     * A full reload rather than the command-line section alone. Every row now
     * carries a sentence about what the machine will actually do with it, and
     * "NOT APPLIED" appears on a row exactly when its position disagrees with
     * the machine -- which is a property of the switch that was just moved.
     * Reloading one section would leave the row the user touched showing the
     * verdict for the position they just left.
     */
    [self refreshBanner];
    [self.tableView reloadData];
}

- (void)pauseSwitchChanged:(UISwitch *)sender {
    [_settings setPausesInBackground:sender.isOn];
}

- (void)tableView:(UITableView *)tableView
didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];

    if ([self sectionAt:indexPath.section] == VMSettingsSectionGeneral) {
        [tableView deselectRowAtIndexPath:indexPath animated:YES];
        if (indexPath.row == VMGeneralRowManual) {
            VMManualViewController *m = [[VMManualViewController alloc] init];
            if (self.navigationController)
                [self.navigationController pushViewController:m animated:YES];
        }
        return;
    }
    switch ((VMSettingsSection)[self sectionAt:indexPath.section]) {
        case VMSettingsSectionSnapshots: {
            VMSettings *set = [VMSettings sharedSettings];
            if (indexPath.row == VMSnapshotRowInterval) {
                if (![set autoSnapshotEnabled]) {
                    [tableView deselectRowAtIndexPath:indexPath animated:YES];
                    return;
                }
                [self presentChoices:@[@30, @60, @300, @900, @1800, @3600]
                             titled:@"Snapshot every"
                          formatter:^NSString *(NSInteger v) {
                              return [self intervalTextFor:v];
                          }
                            current:[set autoSnapshotIntervalSeconds]
                             chosen:^(NSInteger v) {
                                 [set setAutoSnapshotIntervalSeconds:v];
                             }
                          fromIndex:indexPath];
                return;
            }
            if (indexPath.row == VMSnapshotRowKeep) {
                [self presentChoices:@[@1, @3, @5, @10, @25, @50]
                             titled:@"Keep how many"
                          formatter:^NSString *(NSInteger v) {
                              return [NSString stringWithFormat:@"%ld", (long)v];
                          }
                            current:[set autoSnapshotKeep]
                             chosen:^(NSInteger v) {
                                 [set setAutoSnapshotKeep:v];
                             }
                          fromIndex:indexPath];
                return;
            }
            [tableView deselectRowAtIndexPath:indexPath animated:YES];
            return;
        }
        case VMSettingsSectionFirmware:
            /* The three status rows above are still not selectable; only the
             * one row that leads somewhere does anything. */
            if (indexPath.row != VMFirmwareRowImport) return;
            if (self.navigationController)
                [self.navigationController
                    pushViewController:[[VMFirmwareImportViewController alloc] init]
                              animated:YES];
            return;

        case VMSettingsSectionDiagnostics:
            if (indexPath.row != VMDiagnosticsRowInstructionCap) return;
            [_settings setInstructionCap:[_settings nextInstructionCap]];
            [tableView reloadRowsAtIndexPaths:@[indexPath]
                             withRowAnimation:UITableViewRowAnimationNone];
            return;

        case VMSettingsSectionCommandLine:
            [UIPasteboard generalPasteboard].string =
                [_settings equivalentToggleArguments];
            _copiedCommandLine = YES;
            [tableView reloadRowsAtIndexPaths:@[indexPath]
                             withRowAnimation:UITableViewRowAnimationNone];
            return;

        case VMSettingsSectionReset: {
            UIAlertController *confirm = [UIAlertController
                alertControllerWithTitle:@"Reset to defaults"
                                 message:@"Every switch goes back to the value "
                                          "tools/bootkernel uses with nothing on "
                                          "its command line."
                          preferredStyle:UIAlertControllerStyleAlert];
            /* Weak, then strong inside: the alert retains the block and the
             * block must not retain the controller back. */
            __weak VMSettingsViewController *weakSelf = self;
            [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                                        style:UIAlertActionStyleCancel
                                                      handler:nil]];
            [confirm addAction:[UIAlertAction actionWithTitle:@"Reset"
                                                        style:UIAlertActionStyleDestructive
                                                      handler:^(UIAlertAction *action) {
                (void)action;
                [weakSelf performReset];
            }]];
            [self presentViewController:confirm animated:YES completion:nil];
            return;
        }

        default:
            return;
    }
}

- (void)performReset {
    [_settings resetToDefaults];
    _copiedCommandLine = NO;
    [self.tableView reloadData];
}

@end
