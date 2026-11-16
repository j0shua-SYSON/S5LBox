//
//  S5LBox — the settings screen. See VMSettingsViewController.h.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMSettingsViewController.h"

#import "VMBootOptions.h"       /* what each switch actually does, per row */
#import "VMJitProbe.h"          /* the explicit, recoverable execution test */
#import "VMFirmwareImportViewController.h"
#import "VMFirmwareLibraryViewController.h"
#import "VMFirmwareImporter.h"
#import "VMGuestInstall.h"
#import "VMInstanceStore.h"
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
    VMSettingsSectionApps,
    VMSettingsSectionHelp,
    VMSettingsSectionCount
};

/* Stable action identifiers; generalRowsInSection: owns their presentation. */
typedef NS_ENUM(NSInteger, VMGeneralRow) {
    VMGeneralRowManual = 0,
    VMGeneralRowInterface,
    VMGeneralRowGraphicsMode,
    VMGeneralRowDiskSize,
    VMGeneralRowJailbreak,
    VMGeneralRowInstallIPA,
    VMGeneralRowPackageSetup,
    VMGeneralRowPackagesWithJailbreak,
    VMGeneralRowAppSyncWithJailbreak,
    VMGeneralRowGuestMicrophone,
    VMGeneralRowPauseInBackground,
    VMGeneralRowDeveloperMode,
    VMGeneralRowFirmware,
    VMGeneralRowAdvanced,
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
- (void)chooseMachineWithRequest:(void (^)(NSString *, NSString *))request;
- (void)inlineConsoleChanged:(UISwitch *)sender;
- (void)developerModeToggled:(UISwitch *)sender;
- (void)packagesWithJailbreakToggled:(UISwitch *)sender;
- (void)appSyncWithJailbreakToggled:(UISwitch *)sender;
- (void)guestMicrophoneToggled:(UISwitch *)sender;
- (void)chooseGraphicsMode;
- (void)chooseDiskSize;
- (void)chooseEmulatorInterface;
- (NSArray<NSNumber *> *)generalRowsInSection:(NSInteger)section;
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
    BOOL _advanced;
}

#pragma mark - Lifecycle

- (instancetype)init {
    return [self initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];

    _settings = [VMSettings sharedSettings];
    self.title = _advanced ? @"Advanced" : @"App Settings";
    self.tableView.accessibilityIdentifier = _advanced
        ? @"s5lbox.advanced-settings" : @"s5lbox.app-settings";
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    /* App Settings is presented from Machines. Follow the person's
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

    // Technical limitations belong with the controls they describe, not in
    // a warning banner above everyday settings.
    [self rebuildVisibleSections];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(firmwareChanged)
        name:VMFirmwareImportDidChangeNotification object:nil];
}

- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)firmwareChanged {
    [self refreshBanner];
    [self.tableView reloadData];
    [self.view setNeedsLayout];
}

/* Coming back from the importer, the three firmware rows may be describing
 * files that did not exist when this screen was last drawn. Nothing else here
 * is expensive enough for a full reload to matter. */
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self rebuildVisibleSections];
    [self refreshBanner];
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



- (void)inlineConsoleChanged:(UISwitch *)sender {
    [[VMSettings sharedSettings] setInlineConsole:sender.isOn];
}

- (void)refreshBanner {
    if (!_advanced || !_settings.developerMode) {
        self.tableView.tableHeaderView = nil;
        return;
    }
    if (!_banner) {
        _banner = [[UILabel alloc] initWithFrame:CGRectZero];
        _banner.numberOfLines = 0;
        _banner.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
        _banner.adjustsFontForContentSizeCategory = YES;
        _banner.textColor = UIColor.secondaryLabelColor;
    }
    if (!self.tableView.tableHeaderView) {
        UIView *header = [[UIView alloc] initWithFrame:CGRectZero];
        [header addSubview:_banner];
        self.tableView.tableHeaderView = header;
    }
    vm_boot_options_report_t report;
    VMResolveOptions(&report);
    NSMutableString *text = [NSMutableString stringWithString:
        @"Boot options are defaults for new machines. Each switch explains "
         "whether it takes effect; some are unavailable or fixed into an existing disk."];
    if (report.overridden > 0u) {
        NSString *summary =
            [NSString stringWithUTF8String:report.summary] ?: @"";
        if (summary.length) [text appendFormat:@"\n\n%@", summary];
    }
    _banner.text = text;
    [self.view setNeedsLayout];
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

- (void)packagesWithJailbreakToggled:(UISwitch *)sender {
    _settings.packagesWithJailbreak = sender.isOn;
}

- (void)appSyncWithJailbreakToggled:(UISwitch *)sender {
    _settings.appSyncWithJailbreak = sender.isOn;
}

- (void)guestMicrophoneToggled:(UISwitch *)sender {
    _settings.guestMicrophoneEnabled = sender.isOn;
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


// Cell construction and selection use this same mapping. Developer Mode
// changes only Advanced, never the shape of the everyday settings page.
- (NSArray<NSNumber *> *)generalRowsInSection:(NSInteger)section {
    if (section == VMSettingsSectionGeneral) {
        if (!_advanced) return @[];
        NSMutableArray<NSNumber *> *rows = [NSMutableArray arrayWithArray:
            @[@(VMGeneralRowInterface), @(VMGeneralRowFirmware), @(VMGeneralRowGraphicsMode)]];
        [rows addObject:@(VMGeneralRowDiskSize)];
        [rows addObject:@(VMGeneralRowPackagesWithJailbreak)];
        [rows addObject:@(VMGeneralRowAppSyncWithJailbreak)];
        [rows addObject:@(VMGeneralRowGuestMicrophone)];
        [rows addObject:@(VMGeneralRowPauseInBackground)];
        if (self.guestPackageSetupRequest) [rows addObject:@(VMGeneralRowPackageSetup)];
        [rows addObject:@(VMGeneralRowDeveloperMode)];
        return rows;
    }
    if (section == VMSettingsSectionApps) {
        NSMutableArray<NSNumber *> *rows = [NSMutableArray array];
        if (self.guestInstallRequest) [rows addObject:@(VMGeneralRowJailbreak)];
        if (self.guestIPARequest) [rows addObject:@(VMGeneralRowInstallIPA)];
        return rows;
    }
    if (section == VMSettingsSectionHelp)
        return @[@(VMGeneralRowManual), @(VMGeneralRowAdvanced)];
    return nil;
}

- (void)rebuildVisibleSections {
    if (!_advanced) {
        NSMutableArray<NSNumber *> *sections = [NSMutableArray array];
        if ([self generalRowsInSection:VMSettingsSectionApps].count) [sections addObject:@(VMSettingsSectionApps)];
        [sections addObject:@(VMSettingsSectionHelp)];
        _visible = sections;
        return;
    }
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
    NSArray<NSNumber *> *general = [self generalRowsInSection:section];
    if (general) return (NSInteger)general.count;
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
        case VMSettingsSectionApps:        return @"Guest apps";
        case VMSettingsSectionFirmware:    return @"Firmware files";
        case VMSettingsSectionDiagnostics: return @"Runtime defaults & diagnostics";
        case VMSettingsSectionCommandLine: return @"Equivalent command line";
        default:                           return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView
titleForFooterInSection:(NSInteger)section {
    section = [self sectionAt:section];
    if (section == VMSettingsSectionGeneral) {
        return _advanced ? @"Developer Mode reveals boot options and diagnostics." : nil;
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
            return @"Import prepares these files automatically. File presence alone does not verify their contents.";
        case VMSettingsSectionDiagnostics:
            return @"Defaults for newly opened sessions. The JIT test checks host capability; it does not speed up the emulator.";
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
    NSArray<NSNumber *> *general = [self generalRowsInSection:section];
    if (general) {
        VMGeneralRow row = (VMGeneralRow)general[(NSUInteger)indexPath.row].integerValue;
        BOOL valueRow = (row == VMGeneralRowInterface || row == VMGeneralRowFirmware ||
            row == VMGeneralRowDiskSize) &&
            !UIContentSizeCategoryIsAccessibilityCategory(self.traitCollection.preferredContentSizeCategory);
        UITableViewCell *cell = [self cellWithIdentifier:valueRow ? @"general-value" : @"general"
            style:valueRow ? UITableViewCellStyleValue1 : UITableViewCellStyleSubtitle];
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        if (valueRow)
            cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        if (row == VMGeneralRowFirmware) {
            cell.textLabel.text = @"Firmware";
            cell.detailTextLabel.text = [VMFirmwareImporter sharedImporter].isRunning ? @"Preparing…"
                : [VMFirmwareImporter hasConfiguredFirmware]
                    ? @"Imported" : @"Setup needed";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.accessibilityIdentifier = @"s5lbox.settings.firmware";
        } else if (row == VMGeneralRowAdvanced) {
            cell.textLabel.text = @"Advanced";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.accessibilityIdentifier = @"s5lbox.settings.advanced";
        } else if (row == VMGeneralRowManual) {
            cell.textLabel.text = @"Manual";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        } else if (row == VMGeneralRowInterface) {
            cell.textLabel.text = @"Emulator interface";
            cell.detailTextLabel.text = _settings.legacyEmulatorUI ? @"Legacy" :
                _settings.homeOnlyEmulatorUI ? @"Home only" : @"Modern";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.accessibilityIdentifier = @"s5lbox.settings.interface";
        } else if (row == VMGeneralRowGraphicsMode) {
            VMGraphicsMode mode = [_settings graphicsModeForNewMachines];
            cell.textLabel.text = @"Graphics for new machines";
            if (mode == VMGraphicsModeSoftware)
                cell.detailTextLabel.text =
                    @"Software (compatibility)";
            else if (mode == VMGraphicsModeExperimentalMBX)
                cell.detailTextLabel.text =
                    @"MBX (experimental)";
            else
                cell.detailTextLabel.text =
                    @"Custom";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        } else if (row == VMGeneralRowDiskSize) {
            cell.textLabel.text = @"Storage for new machines";
            cell.detailTextLabel.text = [NSString stringWithFormat:@"%lu GB",
                (unsigned long)_settings.newMachineDiskSizeGiB];
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.accessibilityIdentifier = @"s5lbox.settings.new-machine-storage";
        } else if (row == VMGeneralRowJailbreak) {
            cell.textLabel.text = @"Jailbreak";
            cell.accessibilityIdentifier = @"s5lbox.settings.jailbreak";
            cell.detailTextLabel.text = _settings.packagesWithJailbreak
                ? @"Install Cydia and set up Packages." : @"Install or repair Cydia.";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        } else if (row == VMGeneralRowInstallIPA) {
            cell.textLabel.text = @"Install IPA";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.accessibilityIdentifier = @"s5lbox.settings.install-ipa";
        } else if (row == VMGeneralRowPackageSetup) {
            cell.textLabel.text = @"Set up package manager";
            cell.detailTextLabel.text = @"For previously jailbroken machines.";
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            cell.accessibilityIdentifier = @"s5lbox.settings.package-setup";
        } else if (row == VMGeneralRowPackagesWithJailbreak || row == VMGeneralRowAppSyncWithJailbreak) {
            BOOL appSync = row == VMGeneralRowAppSyncWithJailbreak;
            cell.textLabel.text = appSync ? @"AppSync with jailbreak" : @"Packages with jailbreak";
            cell.detailTextLabel.text = appSync ? @"For new jailbreaks." : nil;
            cell.accessoryType = UITableViewCellAccessoryNone;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            UISwitch *sw = [[UISwitch alloc] init];
            sw.on = appSync ? _settings.appSyncWithJailbreak : _settings.packagesWithJailbreak;
            sw.accessibilityLabel = cell.textLabel.text;
            sw.accessibilityIdentifier = appSync ? @"s5lbox.settings.appsync-with-jailbreak"
                : @"s5lbox.settings.packages-with-jailbreak";
            [sw addTarget:self action:appSync ? @selector(appSyncWithJailbreakToggled:) : @selector(packagesWithJailbreakToggled:)
                 forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = sw;
        } else if (row == VMGeneralRowGuestMicrophone || row == VMGeneralRowPauseInBackground) {
            BOOL microphone = row == VMGeneralRowGuestMicrophone;
            cell.textLabel.text = microphone ? @"Guest microphone" : @"Pause in background";
            cell.detailTextLabel.text = microphone ? @"Allow access by default."
                : @"Recommended. iOS may terminate the app if disabled.";
            cell.accessoryType = UITableViewCellAccessoryNone;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            UISwitch *sw = [[UISwitch alloc] init];
            sw.on = microphone ? _settings.guestMicrophoneEnabled : _settings.pausesInBackground;
            sw.accessibilityLabel = cell.textLabel.text;
            sw.accessibilityIdentifier = microphone ? @"s5lbox.settings.guest-microphone"
                : @"s5lbox.settings.background";
            [sw addTarget:self action:microphone ? @selector(guestMicrophoneToggled:) : @selector(pauseSwitchChanged:)
                 forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = sw;
        } else if (row == VMGeneralRowDeveloperMode) {
            cell.textLabel.text = @"Developer Mode";
            cell.accessoryType = UITableViewCellAccessoryNone;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            UISwitch *sw = [[UISwitch alloc] init];
            sw.on = [[VMSettings sharedSettings] developerMode];
            sw.accessibilityLabel = cell.textLabel.text;
            sw.accessibilityIdentifier = @"s5lbox.settings.developer-mode";
            [sw addTarget:self action:@selector(developerModeToggled:)
                 forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = sw;
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
                cell.textLabel.text = @"Import IPSW";
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
                cell.detailTextLabel.text = VMDescribeInstructionCap([_settings instructionCap]);
                cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
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
                    @"Show guest output when Developer Mode is enabled.";
                UISwitch *t = [[UISwitch alloc] initWithFrame:CGRectZero];
                t.on = [[VMSettings sharedSettings] inlineConsole];
                [t addTarget:self action:@selector(inlineConsoleChanged:)
                    forControlEvents:UIControlEventValueChanged];
                cell.accessoryView = t;
                cell.selectionStyle = UITableViewCellSelectionStyleNone;
                return cell;
            }

            return [self cellWithIdentifier:kVMPlainCell style:UITableViewCellStyleSubtitle];
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
            cell.textLabel.text = @"Reset settings";
            cell.textLabel.textColor = [UIColor systemRedColor];
            cell.detailTextLabel.text =
                @"Your machines and files are kept.";
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

    NSArray<NSNumber *> *general = [self generalRowsInSection:[self sectionAt:indexPath.section]];
    if (general) {
        VMGeneralRow row = (VMGeneralRow)general[(NSUInteger)indexPath.row].integerValue;
        if (row == VMGeneralRowFirmware) {
            [self.navigationController pushViewController:[VMFirmwareLibraryViewController new] animated:YES];
        } else if (row == VMGeneralRowAdvanced) {
            VMSettingsViewController *advanced = [[VMSettingsViewController alloc] init];
            advanced->_advanced = YES;
            advanced.guestInstallRequest = self.guestInstallRequest;
            advanced.guestIPARequest = self.guestIPARequest;
            advanced.guestPackageSetupRequest = self.guestPackageSetupRequest;
            [self.navigationController pushViewController:advanced animated:YES];
        } else if (row == VMGeneralRowManual) {
            VMManualViewController *m = [[VMManualViewController alloc] init];
            if (self.navigationController)
                [self.navigationController pushViewController:m animated:YES];
        } else if (row == VMGeneralRowGraphicsMode) {
            [self chooseGraphicsMode];
        } else if (row == VMGeneralRowDiskSize) {
            [self chooseDiskSize];
        } else if (row == VMGeneralRowInterface) {
            [self chooseEmulatorInterface];
        } else if (row == VMGeneralRowJailbreak) {
            [self confirmGuestInstall];
        } else if (row == VMGeneralRowInstallIPA) {
            void (^request)(void) = [self.guestIPARequest copy];
            if (request) [self dismissViewControllerAnimated:YES completion:request];
        } else if (row == VMGeneralRowPackageSetup) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Set up package manager?"
                message:@"The saved running state will be replaced. Unsaved work may be lost."
                preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
            [alert addAction:[UIAlertAction actionWithTitle:@"Set Up Packages" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *a) {
                [self chooseMachineWithRequest:self.guestPackageSetupRequest];
            }]];
            [self presentViewController:alert animated:YES completion:nil];
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
                alertControllerWithTitle:@"Reset settings?"
                                 message:@"Restore app settings to their defaults? Your machines, firmware and files will not be deleted."
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
    NSString *components = _settings.appSyncWithJailbreak ? @"Cydia and AppSync" : @"Cydia";
    NSString *install = [NSString stringWithFormat:@"Install %@%@?", components,
        _settings.packagesWithJailbreak ? @" and set up Packages" : @""];
    NSString *message = self.guestInstallRequest
        ? [install stringByAppendingString:@" The machine will power off and its saved running state "
           @"will be discarded. Unsaved work may be lost."]
        : @"Return to Machines using Back, then open Settings > Jailbreak. "
           @"The guest must be shut down before its disk can be changed.";
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:@"Jailbreak machine?"
                         message:message
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    if (self.guestInstallRequest) {
        __weak VMSettingsViewController *weakSelf = self;
        [alert addAction:[UIAlertAction actionWithTitle:@"Force Off & Jailbreak"
                                                  style:UIAlertActionStyleDestructive
                                                handler:^(UIAlertAction *action) {
            (void)action;
            [weakSelf chooseMachineForGuestInstall];
        }]];
    }
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)chooseMachineForGuestInstall {
    [self chooseMachineWithRequest:self.guestInstallRequest];
}
- (void)chooseMachineWithRequest:(void (^)(NSString *, NSString *))request {
    if (!request) return;
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
        [self dismissViewControllerAnimated:YES completion:^{
            if (request) request(row[@"id"], row[@"name"]);
        }];
    };
    if (eligible.count == 1u) {
        choose(eligible.firstObject);
        return;
    }

    UIAlertController *picker = [UIAlertController
        alertControllerWithTitle:@"Choose a machine"
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

- (void)chooseEmulatorInterface {
    UIAlertController *picker = [UIAlertController alertControllerWithTitle:@"Emulator interface"
        message:@"In Home only, tap outside the guest display to show or hide controls."
        preferredStyle:UIAlertControllerStyleAlert];
    __weak VMSettingsViewController *weakSelf = self;
    NSUInteger selected = _settings.legacyEmulatorUI ? 2 : _settings.homeOnlyEmulatorUI ? 1 : 0;
    for (NSNumber *choice in @[@0, @1, @2]) {
        NSUInteger mode = choice.unsignedIntegerValue;
        NSString *title = @[@"Modern", @"Home only", @"Legacy"][mode];
        if (selected == mode) title = [title stringByAppendingString:@" (current)"];
        [picker addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault
            handler:^(__unused UIAlertAction *action) {
                VMSettingsViewController *controller = weakSelf;
                if (!controller) return;
                controller->_settings.homeOnlyEmulatorUI = mode == 1;
                controller->_settings.legacyEmulatorUI = mode == 2;
                [controller.tableView reloadData];
            }]];
    }
    [picker addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)chooseDiskSize {
    UIAlertController *picker = [UIAlertController alertControllerWithTitle:@"Storage for new machines"
        message:@"Uses space as needed, up to this limit. Existing machines stay the same."
        preferredStyle:UIAlertControllerStyleAlert];
    __weak VMSettingsViewController *weakSelf = self;
    for (NSNumber *choice in @[@8, @4, @2]) {
        NSString *title = [NSString stringWithFormat:@"%@ GB%@", choice,
            choice.unsignedIntegerValue == 8 ? @" (Default)" : @""];
        if (_settings.newMachineDiskSizeGiB == choice.unsignedIntegerValue)
            title = [title stringByAppendingString:@" \u2713"];
        [picker addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault
            handler:^(__unused UIAlertAction *action) {
                VMSettingsViewController *controller = weakSelf;
                if (!controller) return;
                controller->_settings.newMachineDiskSizeGiB = choice.unsignedIntegerValue;
                [controller.tableView reloadData];
            }]];
    }
    [picker addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)chooseGraphicsMode {
    VMGraphicsMode current = [_settings graphicsModeForNewMachines];
    NSString *message =
        @"New machines use this setting. Existing machines keep their saved graphics mode; "
        @"older machines without a saved mode follow this default.\n\n"
        @"MBX is experimental. Software is a compatibility option. Existing disks are not converted.";
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
    [self rebuildVisibleSections];
    [self refreshBanner];
    [self.view setNeedsLayout];
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
