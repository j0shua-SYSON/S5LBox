//
//  S5LBox — the settings screen. See VMSettingsViewController.h.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMSettingsViewController.h"

#import "VMBootOptions.h"       /* what each switch actually does, per row */
#import "VMEngine.h"            /* +firmwareReadinessSummary, used below */
#import "VMJitProbe.h"          /* the explicit, recoverable execution test */
#import "VMFirmwareImportViewController.h"
#import "VMGuestInstall.h"
#import "VMInstanceStore.h"
#import "VMOptions.h"
#import "VMManualViewController.h"
#import "VMSnapshotListViewController.h"
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
        values[i] = [[VMSettings sharedSettings]
                        valueForNewMachineOptionIndex:i]
                        ? true : false;
    /* NULL request: nothing is being started, only described. */
    vm_boot_options_apply(values, count, NULL, report);
}

typedef NS_ENUM(NSInteger, VMSettingsSection) {
    /* The first VM_OPT_GROUP_COUNT sections are the option table's own groups,
     * in its order, so a row added to VMOptions.c appears here with no edit. */
    VMSettingsSectionGeneral = VM_OPT_GROUP_COUNT,
    VMSettingsSectionFirmware,
    VMSettingsSectionDiagnostics,
    VMSettingsSectionCommandLine,
    VMSettingsSectionReset,
    VMSettingsSectionCount
};

/* Keep this order mirrored explicitly in both the cell builder and selection
 * handler. Graphics mode sits before the switches because it is the one
 * performance choice a non-developer needs before opening a new machine. */
typedef NS_ENUM(NSInteger, VMGeneralRow) {
    VMGeneralRowManual = 0,
    VMGeneralRowGraphicsMode,
    VMGeneralRowJailbreak,
    VMGeneralRowDeveloperMode,
    VMGeneralRowSnapshots,
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
    /* Explicit, never automatic. See VMJitProbe.h -- this is the one control in
     * the app that can legitimately take the process down, so it may only ever
     * run because somebody tapped it. */
    VMDiagnosticsRowJitProbe,
    VMDiagnosticsRowCount
};

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
- (void)confirmGuestInstall;
- (void)chooseMachineForGuestInstall;
- (void)inlineConsoleChanged:(UISwitch *)sender;
- (void)developerModeToggled:(UISwitch *)sender;
- (void)chooseGraphicsMode;
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
    self.navigationController.navigationBar.prefersLargeTitles = YES;
    /* When presented by the black emulator, its navigation controller already
     * opts into dark appearance. From the machine list, follow the person's
     * system appearance instead of forcing a dark settings sheet globally. */

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
    _banner.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _banner.adjustsFontForContentSizeCategory = YES;
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
 * short lists of things a person can decide. On, it also shows the sixteen
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
        return (NSInteger)[self optionsInGroup:section].count;
    }
    switch ((VMSettingsSection)section) {
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
        case VMSettingsSectionFirmware:    return @"Firmware";
        case VMSettingsSectionDiagnostics: return @"Diagnostics";
        case VMSettingsSectionCommandLine: return @"Equivalent command line";
        default:                           return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView
titleForFooterInSection:(NSInteger)section {
    section = [self sectionAt:section];
    if (section == VMSettingsSectionGeneral) {
        return [[VMSettings sharedSettings] developerMode]
            ? @"Developer mode is on: the option table, the guest console and "
              @"the diagnostics are shown. Each option says whether it reaches "
              @"the boot, is fixed into a work image, or is unavailable here."
            : @"New here? Read the manual first.\n\n"
              @"Jailbreak downloads a pinned iPhone OS 3 package set from its "
              @"publisher's archive and installs it into one stopped machine's "
              @"own disk. The app bundles none of those packages.\n\n"
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
            return @"The instruction cap, background pause and inline console "
                    "are applied by the app. The JIT row is an explicit host "
                    "capability test, not an emulator speed switch.";
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
    cell.accessibilityIdentifier = nil;
    cell.textLabel.numberOfLines = 0;
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.textColor = [UIColor labelColor];
    cell.textLabel.text = nil;
    cell.detailTextLabel.numberOfLines = 0;
    cell.detailTextLabel.font =
        [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.detailTextLabel.textColor = [UIColor secondaryLabelColor];
    cell.detailTextLabel.text = nil;
    return cell;
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    const NSInteger section = [self sectionAt:indexPath.section];
    if (section == VMSettingsSectionGeneral) {
        UITableViewCell *cell = [self cellWithIdentifier:@"general"
                                                   style:UITableViewCellStyleSubtitle];
        if (indexPath.row == VMGeneralRowManual) {
            cell.textLabel.text = @"Manual";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        } else if (indexPath.row == VMGeneralRowGraphicsMode) {
            VMGraphicsMode mode = [_settings graphicsModeForNewMachines];
            cell.textLabel.text = @"Graphics for new machines";
            if (mode == VMGraphicsModeSoftware)
                cell.detailTextLabel.text =
                    @"CPU software renderer; MBX off (compatible default)";
            else if (mode == VMGraphicsModeExperimentalMBX)
                cell.detailTextLabel.text =
                    @"MBX on; CPU software renderer off (experimental)";
            else
                cell.detailTextLabel.text =
                    @"Custom developer switches; not a controlled test mode";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        } else if (indexPath.row == VMGeneralRowJailbreak) {
            cell.textLabel.text = @"Jailbreak…";
            cell.accessibilityIdentifier = @"s5lbox.settings.jailbreak";
            cell.detailTextLabel.text = self.guestInstallRequest
                ? @"Install Cydia, or repair and expand a compatible older guest"
                : @"Return to Machines and open Settings there before installing";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        } else if (indexPath.row == VMGeneralRowDeveloperMode) {
            cell.textLabel.text = @"Developer Mode";
            cell.accessoryType = UITableViewCellAccessoryNone;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            UISwitch *sw = [[UISwitch alloc] init];
            sw.on = [[VMSettings sharedSettings] developerMode];
            [sw addTarget:self action:@selector(developerModeToggled:)
                 forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = sw;
        } else if (indexPath.row == VMGeneralRowSnapshots) {
            BOOL hasMachine = self.snapshotsDirectory.length > 0;
            cell.textLabel.text = hasMachine ? @"Snapshots"
                                             : @"Snapshots — open a machine first";
            cell.textLabel.textColor = hasMachine ? [UIColor labelColor]
                                                  : [UIColor secondaryLabelColor];
            cell.accessoryType = hasMachine
                ? UITableViewCellAccessoryDisclosureIndicator
                : UITableViewCellAccessoryNone;
            cell.selectionStyle = hasMachine
                ? UITableViewCellSelectionStyleDefault
                : UITableViewCellSelectionStyleNone;
        }
        return cell;
    }

    if (section < VM_OPT_GROUP_COUNT) {
        NSArray<NSNumber *> *rows = [self optionsInGroup:section];
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
        toggle.on = [_settings valueForNewMachineOptionIndex:index];
        toggle.accessibilityLabel = VMStringFromC(option->title);
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

            if (indexPath.row == VMDiagnosticsRowJitProbe) {
                UITableViewCell *cell = [self cellWithIdentifier:kVMValueCell
                                                           style:UITableViewCellStyleSubtitle];
                cell.textLabel.text = @"JIT execution test";
                cell.detailTextLabel.text = [self jitProbeSubtitle];
                cell.selectionStyle = vm_jit_probe_supported()
                    ? UITableViewCellSelectionStyleDefault
                    : UITableViewCellSelectionStyleNone;
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
            cell.textLabel.text = @"Pause in background";
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
            UIFont *mono = [UIFont monospacedSystemFontOfSize:12.0
                                                       weight:UIFontWeightRegular];
            cell.textLabel.font =
                [[UIFontMetrics metricsForTextStyle:UIFontTextStyleCaption1]
                    scaledFontForFont:mono];
            cell.textLabel.adjustsFontForContentSizeCategory = YES;
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
        } else if (indexPath.row == VMGeneralRowGraphicsMode) {
            [self chooseGraphicsMode];
        } else if (indexPath.row == VMGeneralRowJailbreak) {
            [self confirmGuestInstall];
        } else if (indexPath.row == VMGeneralRowSnapshots) {
            if (self.snapshotsDirectory.length == 0) return;
            VMSnapshotListViewController *list =
                [[VMSnapshotListViewController alloc] init];
            /* Both come from whoever presented this screen. Settings is built
             * with a plain -init and holds no machine of its own, so a nil
             * directory here means "nobody told us which machine", and the
             * list shows an empty screen rather than another machine's. */
            list.snapshotsDirectory = self.snapshotsDirectory;
            list.delegate = self.snapshotDelegate;
            if (self.navigationController)
                [self.navigationController pushViewController:list animated:YES];
        }
        return;
    }
    switch ((VMSettingsSection)[self sectionAt:indexPath.section]) {
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
            if (indexPath.row == VMDiagnosticsRowJitProbe) {
                [self confirmAndRunJitProbeAt:indexPath inTable:tableView];
                return;
            }
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

- (void)confirmGuestInstall {
    NSString *message = self.guestInstallRequest
        ? @"For a new install, this downloads exact, pinned iPhone OS 3 "
           @"packages from the publisher's archive and replaces only the "
           @"selected virtual machine's writable disk. S5LBox does not bundle "
           @"the packages. A compatible older Cydia machine is instead copied "
           @"to expand it to 2 GiB and, when needed, repair the exact known "
           @"legacy Cydia executable permissions without redownloading or "
           @"deleting guest data. Unexpected files are refused.\n\n"
           @"Inside the emulated guest, signature enforcement is disabled and "
           @"Cydia is installed on the next cold boot. It does not modify or "
           @"jailbreak the host iPhone. Historical snapshots of that machine "
           @"must be removed first. Continue?"
        : @"A running virtual machine cannot have its disk replaced safely. "
           @"Close this Settings screen, leave the machine with Back, then "
           @"open Settings from the Machines screen.";
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:@"Jailbreak the guest?"
                         message:message
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    if (self.guestInstallRequest) {
        __weak VMSettingsViewController *weakSelf = self;
        [alert addAction:[UIAlertAction actionWithTitle:@"Continue"
                                                  style:UIAlertActionStyleDestructive
                                                handler:^(UIAlertAction *action) {
            (void)action;
            [weakSelf chooseMachineForGuestInstall];
        }]];
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)chooseMachineForGuestInstall {
    if (!self.guestInstallRequest) return;
    VMInstanceStore *store = [VMInstanceStore sharedStore];
    NSMutableArray<NSDictionary<NSString *, NSString *> *> *eligible =
        [NSMutableArray array];
    NSFileManager *files = [NSFileManager defaultManager];
    for (NSUInteger index = 0u; index < store.count; index++) {
        NSDictionary *row = [store instanceAtIndex:index];
        NSString *identifier = row[@"id"];
        NSString *name = row[@"name"] ?: @"iPhone OS 3.1.3";
        if (!identifier.length) continue;
        NSString *machine = [store directoryForInstanceWithID:identifier];
        NSString *leaf = [NSString stringWithUTF8String:
            VM_GUEST_INSTALL_LIVE_FILE];
        NSString *work = [machine stringByAppendingPathComponent:leaf];
        NSDictionary *attributes = [files attributesOfItemAtPath:work error:NULL];
        if ([attributes[NSFileSize] unsignedLongLongValue] == 0u) continue;
        [eligible addObject:@{ @"id": identifier, @"name": name }];
    }

    if (eligible.count == 0u) {
        UIAlertController *none = [UIAlertController
            alertControllerWithTitle:@"No prepared machine"
                             message:@"Open a machine once so its writable root "
                                      @"filesystem is prepared, return with Back, "
                                      @"then try again."
                      preferredStyle:UIAlertControllerStyleAlert];
        [none addAction:[UIAlertAction actionWithTitle:@"OK"
                                                  style:UIAlertActionStyleDefault
                                                handler:nil]];
        [self presentViewController:none animated:YES completion:nil];
        return;
    }

    void (^choose)(NSDictionary<NSString *, NSString *> *) =
        ^(NSDictionary<NSString *, NSString *> *row) {
        void (^request)(NSString *, NSString *) = [self.guestInstallRequest copy];
        [self dismissViewControllerAnimated:YES completion:^{
            if (request) request(row[@"id"], row[@"name"]);
        }];
    };
    if (eligible.count == 1u) {
        choose(eligible.firstObject);
        return;
    }

    UIAlertController *picker = [UIAlertController
        alertControllerWithTitle:@"Choose a stopped machine"
                         message:nil
                  preferredStyle:UIAlertControllerStyleActionSheet];
    for (NSDictionary<NSString *, NSString *> *row in eligible) {
        [picker addAction:[UIAlertAction actionWithTitle:row[@"name"]
                                                  style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction *action) {
            (void)action;
            choose(row);
        }]];
    }
    [picker addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    UIPopoverPresentationController *popover = picker.popoverPresentationController;
    popover.sourceView = self.view;
    popover.sourceRect = CGRectMake(CGRectGetMidX(self.view.bounds),
                                    CGRectGetMidY(self.view.bounds), 1.0, 1.0);
    popover.permittedArrowDirections = 0;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)chooseGraphicsMode {
    VMGraphicsMode current = [_settings graphicsModeForNewMachines];
    NSString *message =
        @"Choose before opening a new machine for the first time. It records "
        @"this exact graphics pair and reuses it whenever it starts, matching "
        @"the value "
        @"written into their writable image. Older machines created before "
        @"that record existed keep the app-wide setting because their saved "
        @"option bits cannot be trusted.\n\nMBX is substantially faster on "
        @"the tested phones, but it is not the default until repeated "
        @"cold-boot, navigation, sleep/wake, and resume soaks are stable.";
    UIAlertController *picker = [UIAlertController
        alertControllerWithTitle:@"Graphics for new machines"
                         message:message
                  preferredStyle:UIAlertControllerStyleAlert];

    NSString *softwareTitle = current == VMGraphicsModeSoftware
        ? @"CPU software (current)" : @"CPU software";
    NSString *mbxTitle = current == VMGraphicsModeExperimentalMBX
        ? @"Experimental MBX (current)" : @"Experimental MBX";
    __weak VMSettingsViewController *weakSelf = self;
    [picker addAction:[UIAlertAction actionWithTitle:softwareTitle
                                               style:UIAlertActionStyleDefault
                                             handler:^(UIAlertAction *action) {
        (void)action;
        VMSettingsViewController *self_ = weakSelf;
        if (!self_) return;
        [self_->_settings setGraphicsModeForNewMachines:VMGraphicsModeSoftware];
        self_->_copiedCommandLine = NO;
        [self_.tableView reloadData];
    }]];
    [picker addAction:[UIAlertAction actionWithTitle:mbxTitle
                                               style:UIAlertActionStyleDefault
                                             handler:^(UIAlertAction *action) {
        (void)action;
        VMSettingsViewController *self_ = weakSelf;
        if (!self_) return;
        [self_->_settings
            setGraphicsModeForNewMachines:VMGraphicsModeExperimentalMBX];
        self_->_copiedCommandLine = NO;
        [self_.tableView reloadData];
    }]];
    [picker addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                               style:UIAlertActionStyleCancel
                                             handler:nil]];
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)performReset {
    [_settings resetToDefaults];
    _copiedCommandLine = NO;
    [self.tableView reloadData];
}

#pragma mark - JIT execution test

/*
 * Two keys, and the first one is the whole safety story.
 *
 * VMJitProbe.c installs handlers for the four signals a refused branch can
 * raise, which covers a fault. It cannot cover a kernel that KILLS the process
 * for a code-signing violation -- no handler runs for SIGKILL, and the probe
 * does not outlive that to record anything.
 *
 * So the breadcrumb is written and FLUSHED before each strategy runs, and
 * removed after it returns. Finding one at launch is proof that the strategy it
 * names took the app down, and that strategy is then never offered again. This
 * is what stops a policy mismatch turning into the crash loop
 * EmulatorViewController's comment warns about.
 */
static NSString *const kVMJitBreadcrumbKey = @"VMJitProbeStrategyInFlight";
static NSString *const kVMJitResultsKey    = @"VMJitProbeResultsByStrategy";

/* -1 when nothing is recorded. */
- (NSInteger)jitCrashedStrategy {
    NSNumber *n = [[NSUserDefaults standardUserDefaults]
                      objectForKey:kVMJitBreadcrumbKey];
    return [n isKindOfClass:[NSNumber class]] ? n.integerValue : -1;
}

- (NSDictionary<NSString *, NSNumber *> *)jitResults {
    NSDictionary *d = [[NSUserDefaults standardUserDefaults]
                          objectForKey:kVMJitResultsKey];
    return [d isKindOfClass:[NSDictionary class]] ? d : @{};
}

- (NSString *)jitProbeSubtitle {
    if (!vm_jit_probe_supported())
        return @"Not available in this build -- no probe for this "
               @"architecture. Nothing to run.";

    NSInteger crashed = [self jitCrashedStrategy];
    if (crashed >= 0)
        return [NSString stringWithFormat:
            @"\"%s\" ended the app last time it ran. That IS the answer for "
            @"that strategy, and it will not be offered again.",
            vm_jit_strategy_name((vm_jit_strategy_t)crashed)];

    NSDictionary<NSString *, NSNumber *> *results = [self jitResults];
    if (results.count == 0)
        return @"Not run. Writes a two-instruction function, calls it, and "
               @"reports what iOS did. Decides whether a JIT is possible at "
               @"all on this device. Tap to run.";

    NSMutableArray<NSString *> *parts = [NSMutableArray array];
    for (int i = 0; i < (int)VM_JIT_STRATEGY_COUNT; i++) {
        const char *name = vm_jit_strategy_name((vm_jit_strategy_t)i);
        NSNumber *r = results[[NSString stringWithUTF8String:name]];
        if (!r) continue;
        [parts addObject:[NSString stringWithFormat:@"%s: %s", name,
            vm_jit_result_text((vm_jit_result_t)r.intValue)]];
    }
    [parts addObject:@"Tap to run again."];
    return [parts componentsJoinedByString:@"\n"];
}

- (void)confirmAndRunJitProbeAt:(NSIndexPath *)indexPath
                        inTable:(UITableView *)tableView {
    if (!vm_jit_probe_supported()) return;

    UIAlertController *confirm = [UIAlertController
        alertControllerWithTitle:@"Run the JIT execution test?"
                         message:@"This writes a tiny function into memory and "
                                  "calls it, to find out whether iOS lets this "
                                  "app run code it generated.\n\nIf the system "
                                  "refuses, the test reports that. If the "
                                  "system ends the app instead, that is also an "
                                  "answer -- it is recorded before the test "
                                  "runs, and that method will not be tried "
                                  "again."
                  preferredStyle:UIAlertControllerStyleAlert];

    __weak VMSettingsViewController *weakSelf = self;
    [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                                style:UIAlertActionStyleCancel
                                              handler:nil]];
    [confirm addAction:[UIAlertAction
        actionWithTitle:@"Run test"
                  style:UIAlertActionStyleDestructive
                handler:^(UIAlertAction *a) {
        (void)a;
        VMSettingsViewController *self2 = weakSelf;
        if (!self2) return;
        [self2 runJitProbe];
        [tableView reloadRowsAtIndexPaths:@[indexPath]
                         withRowAnimation:UITableViewRowAnimationNone];
    }]];
    [self presentViewController:confirm animated:YES completion:nil];
}

- (void)runJitProbe {
    NSUserDefaults *ud = [NSUserDefaults standardUserDefaults];
    NSMutableDictionary<NSString *, NSNumber *> *results =
        [[self jitResults] mutableCopy];
    const NSInteger alreadyCrashed = [self jitCrashedStrategy];

    for (int i = 0; i < (int)VM_JIT_STRATEGY_COUNT; i++) {
        const char *name = vm_jit_strategy_name((vm_jit_strategy_t)i);
        NSString *key = [NSString stringWithUTF8String:name];

        /* Never re-run something that has already killed the app once. */
        if (alreadyCrashed == (NSInteger)i) continue;

        /*
         * Write the breadcrumb and FLUSH it. -synchronize is deprecated and is
         * used deliberately: the point is that the value is on disk before the
         * next line runs, and normal deferred writes would be lost with the
         * process. There is no non-deprecated way to demand that.
         */
        [ud setObject:@(i) forKey:kVMJitBreadcrumbKey];
        [ud synchronize];

        uint32_t observed = 0u;
        vm_jit_result_t r = vm_jit_probe_run((vm_jit_strategy_t)i, &observed);

        [ud removeObjectForKey:kVMJitBreadcrumbKey];
        results[key] = @((int)r);
        [ud setObject:results forKey:kVMJitResultsKey];
        [ud synchronize];
    }
}

@end
