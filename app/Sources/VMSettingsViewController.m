//
//  S5LBox — the settings screen. See VMSettingsViewController.h.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMSettingsViewController.h"

#import "VMOptions.h"
#import "VMManualViewController.h"
#import "VMSettings.h"

#import <math.h>

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

typedef NS_ENUM(NSInteger, VMGeneralRow) {
    VMGeneralRowManual = 0,
    VMGeneralRowJailbreak,
    VMGeneralRowDeveloperMode,
    VMGeneralRowCount
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

- (void)refreshBanner {
    BOOL dev = [[VMSettings sharedSettings] developerMode];
    _banner.text = dev
        ? @"This app boots no firmware. None of the option switches below "
          @"changes the machine on the previous screen — it is running the "
          @"synthetic guest in VMGuest.c, not iPhone OS. They are recorded, and "
          @"rendered back as a command line for the desktop harness. Only the "
          @"two rows under Diagnostics are applied here."
        : @"This app boots no firmware yet. The machine on the previous screen "
          @"runs a small built-in test program — see the Manual.";
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
        case VMSettingsSectionFirmware:    return 3;
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
              @"the diagnostics are shown. None of those switches changes this "
              @"app's built-in test program."
            : @"New here? Read the manual first.\n\n"
              @"Jailbreak disables the guest's signature checking and installs "
              @"Cydia into that machine's own files. It applies to a real "
              @"firmware boot — this app boots none yet, so today it is "
              @"recorded and not performed.\n\n"
              @"Developer mode adds the full option table, the guest console "
              @"and diagnostics — useful for working on the emulator, noise "
              @"otherwise.";
    }
    (void)tableView;

    if (section < VM_OPT_GROUP_COUNT) {
        NSString *note = VMStringFromC(vm_option_group_note((unsigned)section));
        return [note stringByAppendingString:
                @"\n\nRECORDED, NOT APPLIED: these describe a firmware boot, "
                @"and this app has no firmware to boot."];
    }

    switch ((VMSettingsSection)section) {
        case VMSettingsSectionFirmware:
            return [NSString stringWithFormat:
                    @"The project ships no Apple firmware, bundles none, and "
                    @"downloads none. There is also no file picker yet, so "
                    @"these rows are read-only: the app reports whether a file "
                    @"with the expected name is present in\n\n%@\n\n"
                    @"Even with all three present, the app cannot boot them — "
                    @"that is the desktop harness's job today.",
                    [_settings firmwareDirectory] ?: @"(no documents directory)"];
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

        cell.textLabel.text = VMStringFromC(option->title);
        cell.detailTextLabel.text = [NSString stringWithFormat:@"--%s  ·  %@",
                                     option->name,
                                     VMStringFromC(option->detail)];

        UISwitch *toggle = [[UISwitch alloc] initWithFrame:CGRectZero];
        toggle.tag = (NSInteger)index;
        toggle.on = [_settings valueForOptionIndex:index];
        /*
         * Grey, not green. An "on" switch that looks like every other working
         * switch in iOS would be claiming an effect this one does not have.
         *
         * These are left MOVABLE rather than disabled, which is the one place
         * on this screen that judgement was needed. Recording an intended
         * configuration is a thing the app really can do, and bootkernel's own
         * --activate sets the precedent: it is settable, defaults on, and every
         * run header then says it was requested and not applied. So the
         * treatment here is the same — say NOT APPLIED loudly, in the banner
         * and in every footer, rather than take the switch away and leave the
         * user unable to record anything at all. The controls whose OWN
         * function is missing, the device keys and the firmware rows, are
         * disabled outright instead.
         *
         * To take the other view, this is the line: set toggle.enabled = NO.
         */
        toggle.onTintColor = [UIColor systemGrayColor];
        [toggle addTarget:self
                   action:@selector(optionSwitchChanged:)
         forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = toggle;
        return cell;
    }

    switch ((VMSettingsSection)section) {
        case VMSettingsSectionFirmware: {
            NSString *title = @"";
            NSString *file  = @"";
            switch (indexPath.row) {
                case 0: title = @"Kernel";          file = VMFirmwareKernelFile; break;
                case 1: title = @"Device tree";     file = VMFirmwareDeviceTreeFile; break;
                default: title = @"Root filesystem"; file = VMFirmwareRootFilesystemFile; break;
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

    // The rendered command line is derived from every switch, so it moves too.
    [self.tableView reloadSections:
        [NSIndexSet indexSetWithIndex:VMSettingsSectionCommandLine]
                  withRowAnimation:UITableViewRowAnimationNone];
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
