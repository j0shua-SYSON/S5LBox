//
//  S5LBox -- VMFirmwareImportViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMFirmwareImportViewController.h"

#import "VMFirmwareImport.h"
#import "VMFirmwareImporter.h"
#import "VMFirmwareBoot.h"
#import "VMSettings.h"

#import <math.h>

/*
 * WHICH SECTIONS EXIST, AND WHY THIS IS A MAP RATHER THAN A RUN OF ifs.
 *
 * Four of the five sections come and go: there is no progress until something
 * is running, no results until something has finished, and no key rows unless
 * the report actually says a key is what is missing. The delegate methods are
 * indexed by VISIBLE section, so the translation happens once, in -sectionAt:,
 * and no other method has to know that a section can be absent. The bug that
 * shape prevents is a section drawn from one identity while its row count comes
 * from another.
 */
typedef NS_ENUM(NSInteger, VMImportSection) {
    /*
     * CAN THIS MACHINE BOOT REAL FIRMWARE RIGHT NOW, and if not, what is
     * missing. Always present, always first.
     *
     * The app boots Apple firmware automatically when the files are there --
     * there is no "boot firmware" button, because there is no choice to make.
     * The failure that produced this section is that the converse was also
     * silent: with a file missing it fell back to the synthetic guest and said
     * nothing, so a user who had imported an IPSW, typed keys and closed the
     * screen had no way to find out that nothing had been produced.
     */
    VMImportSectionReadiness = 0,
    VMImportSectionChoose,
    VMImportSectionProgress,
    VMImportSectionResults,
    VMImportSectionKeys,
    VMImportSectionReport
};

typedef NS_ENUM(NSInteger, VMImportChooseRow) {
    VMImportChooseRowPick = 0,
    /*
     * Whatever is sitting in the firmware folder. This is the row that makes
     * the Files-app route work end to end: copy an IPSW into S5LBox's folder,
     * come back here, tap once. No picker, no security-scoped URL, and no
     * second copy of a 239 MB file.
     */
    VMImportChooseRowDetect,
    VMImportChooseRowAgain,      /* only once a file has been picked           */
    VMImportChooseRowCount
};

typedef NS_ENUM(NSInteger, VMImportProgressRow) {
    VMImportProgressRowBar = 0,
    VMImportProgressRowCancel,
    VMImportProgressRowCount
};

static NSString *const kVMImportPlainCell    = @"plain";
static NSString *const kVMImportProgressCell = @"progress";

/* The progress row is the one fixed-height row on the screen. */
static const CGFloat kVMImportProgressRowHeight = 64.0;

static NSString *VMStringFromC(const char *text) {
    if (!text) return @"";
    return [NSString stringWithUTF8String:text] ?: @"";
}

static NSString *VMCapitalizeFirst(NSString *text) {
    if (text.length == 0) return text;
    return [[[text substringToIndex:1] uppercaseString]
            stringByAppendingString:[text substringFromIndex:1]];
}

/*
 * The core has a state_word() but keeps it to itself -- it is not declared in
 * VMFirmwareImport.h, so calling it would be reaching into another translation
 * unit's private text. These are this screen's own words for the same states,
 * and they say the same things.
 */
static NSString *VMImportStateWord(vm_fw_state_t state) {
    switch (state) {
        case VM_FW_STATE_NOT_STARTED:    return @"not started";
        case VM_FW_STATE_NOT_IN_ARCHIVE: return @"not in this IPSW";
        case VM_FW_STATE_FOUND:          return @"found, not produced";
        case VM_FW_STATE_NEEDS_KEY:      return @"needs a key you supply";
        case VM_FW_STATE_EXTRACTED:      return @"extracted, unverified";
        case VM_FW_STATE_VERIFIED:       return @"verified";
        case VM_FW_STATE_MISMATCH:       return @"wrong bytes";
        case VM_FW_STATE_FAILED:         return @"failed";
        default:                         return @"unknown";
    }
}

static UIColor *VMImportStateColor(vm_fw_state_t state) {
    switch (state) {
        /* Green only for the one state that means the bytes are provably the
         * right bytes. Everything else is a description, not an achievement. */
        case VM_FW_STATE_VERIFIED:       return [UIColor systemGreenColor];
        case VM_FW_STATE_NEEDS_KEY:      return [UIColor systemOrangeColor];
        case VM_FW_STATE_MISMATCH:
        case VM_FW_STATE_FAILED:
        case VM_FW_STATE_NOT_IN_ARCHIVE: return [UIColor systemRedColor];
        default:                         return [UIColor labelColor];
    }
}

/* ------------------------------------------------------------------------ */
/* The one cell that is not a stock cell                                     */
/* ------------------------------------------------------------------------ */
/*
 * A UIProgressView has no useful intrinsic width, so it cannot be an
 * accessoryView, and a cell that self-sizes around two labels leaves nowhere
 * predictable to put it. Laying it out by hand in a subclass is both shorter
 * and more certain than the alternatives, and it matches how the rest of this
 * app lays views out: frames and autoresizing, no constraints.
 */
@interface VMImportProgressCell : UITableViewCell
@property (nonatomic, readonly) UIProgressView *bar;
@end

@implementation VMImportProgressCell

- (instancetype)initWithStyle:(UITableViewCellStyle)style
              reuseIdentifier:(NSString *)reuseIdentifier {
    self = [super initWithStyle:style reuseIdentifier:reuseIdentifier];
    if (!self) return nil;
    _bar = [[UIProgressView alloc]
               initWithProgressViewStyle:UIProgressViewStyleDefault];
    [self.contentView addSubview:_bar];
    self.selectionStyle = UITableViewCellSelectionStyleNone;
    return self;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const CGRect bounds = self.contentView.bounds;
    const CGFloat inset = 16.0;
    CGFloat width = bounds.size.width - 2.0 * inset;
    if (width < 0.0) width = 0.0;
    _bar.frame = CGRectMake(inset, bounds.size.height - 14.0, width, 4.0);
}

@end

/* ------------------------------------------------------------------------ */

// Declared up front so every call below is checked against a prototype.
@interface VMFirmwareImportViewController ()
    <UIDocumentPickerDelegate, VMFirmwareImporterDelegate>
- (void)refresh;
- (void)rebuildSections;
- (NSInteger)sectionAt:(NSInteger)visible;
- (NSInteger)visibleIndexOfSection:(VMImportSection)section;
- (UITableViewCell *)cellWithIdentifier:(NSString *)identifier
                                  style:(UITableViewCellStyle)style;
- (void)configureProgressCell:(VMImportProgressCell *)cell;
- (void)presentPicker;
- (void)startImportOfURL:(NSURL *)url;
- (void)revealImportState;
- (void)presentKeyAlertForArtefact:(vm_fw_artefact_t)which;
- (void)applyKeyText:(NSString *)keyText
                  iv:(NSString *)ivText
         forArtefact:(vm_fw_artefact_t)which;
/* Not -copyReport:. A method whose first word is "copy" is in ARC's copy
 * family, which is about ownership of a returned object and has nothing to do
 * with the clipboard. */
- (void)putReportOnPasteboard;
@end

@implementation VMFirmwareImportViewController {
    VMFirmwareImporter *_importer;

    NSArray<NSNumber *> *_visible;   /* visible index -> VMImportSection      */
    NSArray<NSNumber *> *_keyRows;   /* key row       -> vm_fw_artefact_t     */
    NSInteger _chooseRowCount;

    UILabel *_intro;

    NSURL *_pickedURL;
    BOOL   _running;
    BOOL   _cancelRequested;
    BOOL   _copiedReport;

    BOOL             _haveReport;
    vm_fw_report_t   _report;

    vm_fw_stage_t    _stage;
    vm_fw_artefact_t _stageArtefact;
    double           _fraction;      /* negative when no total is known yet   */
}

/*
 * What vm_firmware_boot_probe() says about the firmware directory right now.
 * Opens nothing for writing and reads no file contents, so calling it every
 * time the table reloads is cheap.
 */
static BOOL VMProbeFirmware(vm_firmware_boot_state_t *out) {
    memset(out, 0, sizeof *out);
    NSString *dir = [[VMSettings sharedSettings] firmwareDirectory];
    if (dir.length == 0) return NO;

    /*
     * SPLIT, with an EMPTY work directory, and that is the whole correction.
     *
     * The first version used vm_firmware_boot_paths_shared(), which looks for
     * the writable work image in the firmware folder. That answers a question
     * nobody asked: the work image belongs to ONE machine and lives in that
     * machine's directory, so a shared probe can report READY about a machine
     * that does not exist yet. It did, and the machine then said "Preparing
     * iPhone OS" -- which is correct behaviour contradicting a promise this
     * screen had just made.
     *
     * VMEngine's own +firmwareReadinessSummary already got this right and says
     * why: claiming READY here would be claiming a fact about a machine the
     * caller has not identified. An empty work directory makes the probe
     * report NEEDS_WORK_IMAGE, which is exactly true of every machine that has
     * not been opened yet, and the row words it as that.
     */
    vm_firmware_boot_paths_t paths;
    if (!vm_firmware_boot_paths_split(&paths, [dir fileSystemRepresentation],
                                      ""))
        return NO;
    vm_firmware_boot_probe(&paths, out);
    return YES;
}

#pragma mark - Lifecycle

/* Grouped, not inset-grouped, for the same reason as the settings screen: the
 * sections carry long explanatory footers and the extra inset costs a
 * noticeable amount of their width on a phone. */
- (instancetype)init {
    return [self initWithStyle:UITableViewStyleGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];

    self.title = @"Import firmware";

    /*
     * The shared one. Keys the user typed on a previous visit are still here,
     * and an import started before this screen was dismissed is still running
     * -- both of which used to be destroyed with the view controller.
     */
    _importer = [VMFirmwareImporter sharedImporter];
    _importer.delegate = self;
    _running = [_importer isRunning];

    _stage         = VM_FW_STAGE_OPENING;
    _stageArtefact = VM_FW_KERNEL;
    _fraction      = -1.0;

    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 76.0;
    self.tableView.estimatedSectionHeaderHeight = 28.0;
    self.tableView.estimatedSectionFooterHeight = 44.0;

    /* The first thing on the screen is what this cannot do. */
    _intro = [[UILabel alloc] initWithFrame:CGRectZero];
    _intro.numberOfLines = 0;
    _intro.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _intro.adjustsFontForContentSizeCategory = YES;
    _intro.textColor = [UIColor secondaryLabelColor];
    _intro.text =
        @"Import iPhone OS 3.1.3 (7E18) for iPhone 3G. Public keys are selected "
        @"automatically; no key entry is needed. No Apple firmware is included or downloaded.";
    UIView *header = [[UIView alloc] initWithFrame:CGRectZero];
    [header addSubview:_intro];
    self.tableView.tableHeaderView = header;

    [self rebuildSections];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(checkDetectedArchive)
        name:UIApplicationDidBecomeActiveNotification object:nil];
}

/*
 * The header view is not a cell and does not self-size, so it is laid out by
 * hand. Re-assigning tableHeaderView is what makes the table adopt a new
 * height, and doing that unconditionally would re-enter layout forever -- so it
 * happens only when the height it should be has actually changed. Same shape as
 * VMSettingsViewController's banner, for the same reason.
 */
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];

    UIView *header = self.tableView.tableHeaderView;
    if (!header) return;

    const CGFloat width = self.tableView.bounds.size.width;
    if (width <= 0.0) return;

    const CGFloat inset = 20.0;
    const CGFloat margin = 14.0;
    CGSize fit = [_intro sizeThatFits:CGSizeMake(width - 2.0 * inset,
                                                 CGFLOAT_MAX)];
    const CGFloat textHeight = ceil(fit.height);
    _intro.frame = CGRectMake(inset, margin, width - 2.0 * inset, textHeight);

    const CGFloat wanted = textHeight + 2.0 * margin;
    if (fabs(header.frame.size.height - wanted) > 0.5 ||
        fabs(header.frame.size.width - width) > 0.5) {
        header.frame = CGRectMake(0.0, 0.0, width, wanted);
        self.tableView.tableHeaderView = header;
    }
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    // The shared operation survives navigation. Only Cancel cancels it.
    if (_importer.delegate == self) _importer.delegate = nil;
}

#pragma mark - Table shape

- (void)refresh {
    [self rebuildSections];
    [self.tableView reloadData];
}

/* Every count the data source reports is decided here, in one pass, so
 * -numberOfRowsInSection: and -cellForRowAtIndexPath: cannot disagree about
 * how many rows a section has. */
- (void)rebuildSections {
    NSMutableArray<NSNumber *> *visible = [NSMutableArray array];
    [visible addObject:@(VMImportSectionReadiness)];
    [visible addObject:@(VMImportSectionChoose)];
    if (_running) [visible addObject:@(VMImportSectionProgress)];
    if (_haveReport) [visible addObject:@(VMImportSectionResults)];

    /* Advanced overrides remain available independently of the last report;
     * ordinary imports resolve the bundled public keys without these rows. */
    _keyRows = @[@(VM_FW_KERNEL), @(VM_FW_DEVICE_TREE), @(VM_FW_ROOT_FILESYSTEM)];
    if (!_running) [visible addObject:@(VMImportSectionKeys)];

    if (_haveReport) [visible addObject:@(VMImportSectionReport)];
    _visible = [visible copy];

    _chooseRowCount = (_pickedURL && !_running) ? VMImportChooseRowCount
                                                : VMImportChooseRowCount - 1;
}

/* Visible index -> VMImportSection. Out of range answers Choose, which is the
 * one section that always exists, rather than an index into a section that may
 * not be on screen. */
- (NSInteger)sectionAt:(NSInteger)visible {
    if (visible < 0 || (NSUInteger)visible >= _visible.count)
        return VMImportSectionChoose;
    return _visible[(NSUInteger)visible].integerValue;
}

- (NSInteger)visibleIndexOfSection:(VMImportSection)section {
    for (NSUInteger i = 0; i < _visible.count; i++)
        if (_visible[i].integerValue == (NSInteger)section) return (NSInteger)i;
    return -1;
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    if (!_visible) [self rebuildSections];
    return (NSInteger)_visible.count;
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    switch ((VMImportSection)[self sectionAt:section]) {
        case VMImportSectionReadiness: return 1;
        case VMImportSectionChoose:   return _chooseRowCount;
        case VMImportSectionProgress: return VMImportProgressRowCount;
        case VMImportSectionResults:  return VM_FW_ARTEFACT_COUNT + 1;
        case VMImportSectionKeys:     return (NSInteger)_keyRows.count + 1;
        case VMImportSectionReport:   return 1;
        default:                      return 0;
    }
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    switch ((VMImportSection)[self sectionAt:section]) {
        case VMImportSectionProgress: return @"Running";
        case VMImportSectionResults:  return @"Results";
        case VMImportSectionKeys:     return @"Advanced · Manual key overrides";
        case VMImportSectionChoose:
        case VMImportSectionReport:
        default:                      return nil;
    }
}

- (NSString *)tableView:(UITableView *)tableView
titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    switch ((VMImportSection)[self sectionAt:section]) {
        case VMImportSectionChoose:
            return @"Choose an IPSW from Files, or place it in S5LBox > firmware. "
                    "Allow about 700 MB of free space for preparation. Keep the app open.";

        case VMImportSectionProgress:
            return @"Cancel discards this attempt. Previously prepared firmware is kept.";

        case VMImportSectionResults:
            return [NSString stringWithFormat:
                    @"Files are written to\n\n%@\n\n"
                    @"\"verified\" means the bytes are identical to the "
                    @"known-good file for this build. \"extracted, "
                    @"unverified\" means it was produced, but S5LBox holds no "
                    @"reference hash for this build to check it against.",
                    [[VMSettings sharedSettings] firmwareDirectory]
                        ?: @"(no documents directory)"];

        case VMImportSectionReadiness:
            return @"Firmware is shared. Each machine creates its own writable disk on first boot.";

        case VMImportSectionKeys:
            return @"Optional overrides, held only for this app session. Saving an override "
                    "retries the selected IPSW immediately. Use automatic keys to clear overrides.";

        case VMImportSectionReport:
            return @"Everything above as plain text, including the SHA-256 of "
                    "whatever was produced. Written for pasting into a bug "
                    "report.";

        default:
            return nil;
    }
}

- (CGFloat)tableView:(UITableView *)tableView
heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    if ([self sectionAt:indexPath.section] == VMImportSectionProgress &&
        indexPath.row == VMImportProgressRowBar)
        return kVMImportProgressRowHeight;
    return UITableViewAutomaticDimension;
}

- (CGFloat)tableView:(UITableView *)tableView
estimatedHeightForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    if ([self sectionAt:indexPath.section] == VMImportSectionProgress &&
        indexPath.row == VMImportProgressRowBar)
        return kVMImportProgressRowHeight;
    return 76.0;
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
     * carries a stale colour, accessory or selection style into a row that
     * means something different. */
    cell.accessoryView = nil;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
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

- (void)configureProgressCell:(VMImportProgressCell *)cell {
    cell.textLabel.numberOfLines = 1;
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.textColor = [UIColor labelColor];
    cell.textLabel.text =
        VMCapitalizeFirst(VMStringFromC(vm_fw_stage_name(_stage)));
    cell.detailTextLabel.numberOfLines = 1;
    cell.detailTextLabel.font =
        [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.detailTextLabel.textColor = [UIColor secondaryLabelColor];

    if (_fraction >= 0.0) {
        cell.detailTextLabel.text = [NSString stringWithFormat:@"%@  -  %d%%",
            VMStringFromC(vm_fw_artefact_title(_stageArtefact)),
            (int)(_fraction * 100.0 + 0.5)];
        cell.bar.hidden = NO;
        [cell.bar setProgress:(float)_fraction animated:NO];
    } else {
        /*
         * The early stages report no total, and the artefact they are tagged
         * with is a placeholder rather than a claim about which file is being
         * worked on. Drawing a bar at 0% for them would assert a precision
         * that does not exist, so there is no bar until there is a total.
         */
        cell.detailTextLabel.text = @"no total to measure against yet";
        cell.bar.hidden = YES;
        [cell.bar setProgress:0.0f animated:NO];
    }
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    const NSInteger section = [self sectionAt:indexPath.section];
    const NSInteger row = indexPath.row;

    switch ((VMImportSection)section) {
        case VMImportSectionReadiness: {
            UITableViewCell *cell =
                [self cellWithIdentifier:kVMImportPlainCell
                                   style:UITableViewCellStyleSubtitle];
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            cell.detailTextLabel.numberOfLines = 0;

            vm_firmware_boot_state_t st;
            if (!VMProbeFirmware(&st)) {
                cell.textLabel.text = @"Cannot check";
                cell.textLabel.textColor = [UIColor systemRedColor];
                cell.detailTextLabel.text =
                    @"there is no Documents directory to look in";
                return cell;
            }
            switch (st.readiness) {
                /*
                 * Both mean the same thing from here, because this screen
                 * cannot see a machine: the three shared files are present.
                 * What happens NEXT is per-machine, and saying so plainly is
                 * what stops "Preparing iPhone OS" reading as a fault.
                 */
                case VM_FW_BOOT_READY:
                case VM_FW_BOOT_NEEDS_WORK_IMAGE:
                    cell.textLabel.text = @"Firmware imported";
                    cell.textLabel.textColor = [UIColor systemGreenColor];
                    cell.detailTextLabel.text =
                        @"Open a machine to start iPhone OS. On first boot its writable disk "
                        @"is prepared automatically, with progress shown on screen.";
                    break;
                case VM_FW_BOOT_INCOMPLETE:
                default:
                    cell.textLabel.text = @"Not ready -- the synthetic guest "
                                          @"will run instead";
                    cell.textLabel.textColor = [UIColor systemOrangeColor];
                    cell.detailTextLabel.text = VMStringFromC(st.detail);
                    break;
            }
            return cell;
        }

        case VMImportSectionChoose: {
            UITableViewCell *cell =
                [self cellWithIdentifier:kVMImportPlainCell
                                   style:UITableViewCellStyleSubtitle];
            if (row == VMImportChooseRowAgain && _chooseRowCount > 1) {
                cell.textLabel.text = @"Import that file again";
                cell.detailTextLabel.text = _pickedURL.lastPathComponent
                    ?: @"the file you chose";
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
                return cell;
            }
            if (row == VMImportChooseRowDetect) {
                NSArray<NSString *> *found =
                    [[VMSettings sharedSettings] detectedArchivePaths];
                if (found.count == 0) {
                    cell.textLabel.text = @"No IPSW in the firmware folder";
                    cell.textLabel.textColor = [UIColor secondaryLabelColor];
                    cell.detailTextLabel.text =
                        @"copy one into S5LBox > firmware in the Files app";
                    cell.selectionStyle = UITableViewCellSelectionStyleNone;
                } else {
                    cell.textLabel.text = @"Detect IPSW";
                    cell.detailTextLabel.text = found.count == 1
                        ? [found.firstObject lastPathComponent]
                        : [NSString stringWithFormat:@"%@ and %lu more",
                             [found.firstObject lastPathComponent],
                             (unsigned long)(found.count - 1)];
                    cell.selectionStyle = _running
                        ? UITableViewCellSelectionStyleNone
                        : UITableViewCellSelectionStyleDefault;
                    if (_running)
                        cell.textLabel.textColor = [UIColor secondaryLabelColor];
                }
                return cell;
            }
            cell.textLabel.text = @"Choose an IPSW...";
            if (_running) {
                cell.textLabel.textColor = [UIColor secondaryLabelColor];
                cell.detailTextLabel.text = @"an import is already running";
            } else {
                cell.detailTextLabel.text =
                    @"opens the Files picker; the file is read where it is";
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            }
            return cell;
        }

        case VMImportSectionProgress: {
            if (row == VMImportProgressRowBar) {
                VMImportProgressCell *cell = (VMImportProgressCell *)
                    [tableView dequeueReusableCellWithIdentifier:kVMImportProgressCell];
                if (![cell isKindOfClass:[VMImportProgressCell class]])
                    cell = [[VMImportProgressCell alloc]
                               initWithStyle:UITableViewCellStyleSubtitle
                             reuseIdentifier:kVMImportProgressCell];
                [self configureProgressCell:cell];
                return cell;
            }

            UITableViewCell *cell =
                [self cellWithIdentifier:kVMImportPlainCell
                                   style:UITableViewCellStyleSubtitle];
            cell.textLabel.text = _cancelRequested ? @"Cancelling" : @"Cancel";
            cell.textLabel.textColor = _cancelRequested
                ? [UIColor secondaryLabelColor] : [UIColor systemRedColor];
            cell.detailTextLabel.text = _cancelRequested
                ? @"waiting for the current block to finish"
                : @"stops at the next block and deletes what was written";
            if (!_cancelRequested)
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            return cell;
        }

        case VMImportSectionResults: {
            UITableViewCell *cell =
                [self cellWithIdentifier:kVMImportPlainCell
                                   style:UITableViewCellStyleSubtitle];
            // Archive/provider failures happen before any artifact starts.
            // Keep their actual reason visible, not only in Copy report.
            if (row == 0) {
                cell.textLabel.text = _report.status == VM_FW_OK ? @"Import complete"
                    : _report.status == VM_FW_ERR_CANCELLED ? @"Import cancelled" : @"Import did not finish";
                cell.textLabel.textColor = _report.status == VM_FW_OK
                    ? UIColor.labelColor : UIColor.systemOrangeColor;
                cell.detailTextLabel.text = VMStringFromC(_report.detail[0]
                    ? _report.detail : vm_fw_strerror(_report.status));
                cell.accessibilityIdentifier = @"s5lbox.firmware-import.result";
                return cell;
            }
            if (row < 1 || row > VM_FW_ARTEFACT_COUNT) return cell;

            const vm_fw_artefact_t which = (vm_fw_artefact_t)(row - 1);
            const vm_fw_artefact_report_t *artefact = &_report.artefacts[row - 1];

            cell.textLabel.text = [NSString stringWithFormat:@"%@  -  %@",
                VMStringFromC(vm_fw_artefact_title(which)),
                VMImportStateWord(artefact->state)];
            cell.textLabel.textColor = VMImportStateColor(artefact->state);
            /* The core writes this sentence in plain language, and it is more
             * specific than anything this screen could say about the same
             * outcome. Shown as written. */
            cell.detailTextLabel.text = (artefact->detail[0] != '\0')
                ? VMStringFromC(artefact->detail)
                : [NSString stringWithFormat:@"target file name \"%@\"",
                   VMStringFromC(vm_fw_artefact_filename(which))];
            return cell;
        }

        case VMImportSectionKeys: {
            UITableViewCell *cell =
                [self cellWithIdentifier:kVMImportPlainCell
                                   style:UITableViewCellStyleSubtitle];
            if (row == (NSInteger)_keyRows.count) {
                cell.textLabel.text = @"Use automatic keys";
                cell.detailTextLabel.text = @"Clear manual overrides and retry the selected IPSW.";
                cell.selectionStyle = UITableViewCellSelectionStyleDefault;
                return cell;
            }
            if (row < 0 || (NSUInteger)row >= _keyRows.count) return cell;

            const vm_fw_artefact_t which =
                (vm_fw_artefact_t)_keyRows[(NSUInteger)row].intValue;
            const BOOL isRoot = (which == VM_FW_ROOT_FILESYSTEM);

            /* Make the distinction between automatic keys and manual overrides
             * visible on each row, not only in the section footer. */
            cell.textLabel.text = [NSString stringWithFormat:@"%@ %@",
                VMStringFromC(vm_fw_artefact_title(which)),
                isRoot ? @"key" : @"key and IV"];
            cell.detailTextLabel.text = [_importer haveKeyForArtefact:which]
                ? @"set for this session, tap to replace"
                : @"automatic for iPhone1,2 · 7E18";
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            return cell;
        }

        case VMImportSectionReport: {
            UITableViewCell *cell =
                [self cellWithIdentifier:kVMImportPlainCell
                                   style:UITableViewCellStyleSubtitle];
            cell.textLabel.text = @"Copy report";
            cell.detailTextLabel.text = _copiedReport
                ? @"copied to the clipboard" : @"tap to copy";
            cell.selectionStyle = UITableViewCellSelectionStyleDefault;
            return cell;
        }

        default:
            /* Unreachable while _visible holds only the five above, and still
             * a real cell rather than nil if that ever stops being true. */
            return [self cellWithIdentifier:kVMImportPlainCell
                                      style:UITableViewCellStyleSubtitle];
    }
}

#pragma mark - Selection

- (void)tableView:(UITableView *)tableView
didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];

    const NSInteger row = indexPath.row;
    switch ((VMImportSection)[self sectionAt:indexPath.section]) {
        case VMImportSectionChoose:
            if (_running) return;
            if (row == VMImportChooseRowDetect) {
                NSArray<NSString *> *found =
                    [[VMSettings sharedSettings] detectedArchivePaths];
                if (found.count == 0) return;
                /*
                 * A file inside our own container, so no security scope to
                 * start and none to stop -- that whole dance exists for files
                 * the picker hands over from somewhere else.
                 */
                _pickedURL = [NSURL fileURLWithPath:found.firstObject];
                [self startImportOfURL:_pickedURL];
                return;
            }
            if (row == VMImportChooseRowAgain && _chooseRowCount > 1)
                [self startImportOfURL:_pickedURL];
            else
                [self presentPicker];
            return;

        case VMImportSectionProgress:
            if (row != VMImportProgressRowCancel || _cancelRequested) return;
            _cancelRequested = YES;
            [_importer cancelImport];
            [self refresh];
            return;

        case VMImportSectionKeys: {
            if (_running) return;
            if (row == (NSInteger)_keyRows.count) {
                [_importer forgetKeys];
                [self refresh];
                [self startImportOfURL:_importer.selectedURL];
                return;
            }
            if (row < 0 || (NSUInteger)row >= _keyRows.count) return;
            [self presentKeyAlertForArtefact:
                (vm_fw_artefact_t)_keyRows[(NSUInteger)row].intValue];
            return;
        }

        case VMImportSectionReport:
            [self putReportOnPasteboard];
            return;

        case VMImportSectionResults:
        default:
            return;
    }
}

#pragma mark - Choosing a file

- (void)presentPicker {
    /*
     * WHY THE DEPRECATED INITIALISER, DELIBERATELY.
     *
     * -initForOpeningContentTypes: is the iOS 14 replacement and it takes
     * UTType objects, which live in UniformTypeIdentifiers.framework. That
     * framework is not in this target's dependency list and this file is not
     * allowed to add it, so reaching for the modern call would fail to link
     * rather than fail to compile. The deployment target is iOS 13, where the
     * modern call does not exist at all, so the string-based initialiser is the
     * only one that both exists and links here. The warning is silenced at this
     * one call site rather than for the file.
     *
     * MODE, also deliberately: Open, not Import. Import copies the chosen file
     * into the app's container first, which for a 430 MB IPSW means writing
     * half a gigabyte before any work starts and needing room for two copies.
     * Open hands back a security-scoped URL to the file where it already is,
     * which is exactly what a read-only pass over an archive wants.
     */
    UIDocumentPickerViewController *picker;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    picker = [[UIDocumentPickerViewController alloc]
                 initWithDocumentTypes:@[ @"public.data" ]
                                inMode:UIDocumentPickerModeOpen];
#pragma clang diagnostic pop

    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}

/* Some older providers still deliver the singular callback. Both feed one
 * selection path, and a duplicate delivery is refused by the running guard. */
- (void)documentPicker:(UIDocumentPickerViewController *)controller
didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject;
    if (!url) return;
    _pickedURL = url;
    _importer.delegate = self;
    [self startImportOfURL:url];
    [controller dismissViewControllerAnimated:YES completion:^{
        [self revealImportState];
    }];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
   didPickDocumentAtURL:(NSURL *)url {
    [self documentPicker:controller didPickDocumentsAtURLs:url ? @[url] : @[]];
}

- (void)revealImportState {
    NSInteger section = [self visibleIndexOfSection:
        _running ? VMImportSectionProgress : VMImportSectionResults];
    if (section >= 0 && [self.tableView numberOfRowsInSection:section] > 0)
        [self.tableView scrollToRowAtIndexPath:
            [NSIndexPath indexPathForRow:0 inSection:section]
            atScrollPosition:UITableViewScrollPositionTop animated:YES];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
    (void)controller;
    /* Nothing to undo: no state changes until a file comes back. */
}

- (void)startImportOfURL:(NSURL *)url {
    if (!url || _running || [_importer isRunning]) return;

    _running = YES;
    _cancelRequested = NO;
    _copiedReport = NO;
    /* The previous report describes a different run. Keeping it on screen next
     * to a running bar would invite reading a stale result as a live one. */
    _haveReport = NO;
    _stage = VM_FW_STAGE_OPENING;
    _stageArtefact = VM_FW_KERNEL;
    _fraction = -1.0;

    [self refresh];
    [_importer importIPSWAtURL:url];
    [self revealImportState];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    /*
     * The importer is shared, so another instance of this screen may hold the
     * delegate. Taking it back here is what makes progress appear on the
     * screen the user is actually looking at.
     */
    _importer.delegate = self;
    _running = [_importer isRunning];
    _pickedURL = _importer.selectedURL;
    _haveReport = [_importer getLastReport:&_report];
    _stage = _importer.stage;
    _stageArtefact = _importer.stageArtefact;
    _fraction = _importer.fraction;
    [self refresh];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self checkDetectedArchive];
}
- (void)checkDetectedArchive {
    if (!self.view.window || self.presentedViewController || self.navigationController.topViewController != self) return;
    _importer.delegate = self;
    [_importer importDetectedIPSWIfNeeded];
    _pickedURL = _importer.selectedURL;
    [self refresh];
}

#pragma mark - Keys the user supplies

- (void)presentKeyAlertForArtefact:(vm_fw_artefact_t)which {
    const BOOL isRoot = (which == VM_FW_ROOT_FILESYSTEM);

    NSString *title = [NSString stringWithFormat:@"%@ %@",
        VMStringFromC(vm_fw_artefact_title(which)),
        isRoot ? @"key" : @"key and IV"];

    NSString *message = isRoot
        ? [NSString stringWithFormat:
            @"Optional override: %u hexadecimal characters (AES + HMAC). "
            @"Leave automatic keys enabled for the supported IPSW.",
            (unsigned)(VMFW_DMG_KEY_BLOB_SIZE * 2u)]
        : @"Optional override: key (32, 48 or 64 hexadecimal characters) and IV "
          @"(32 characters). The published IV is not the wrapped value inside IMG3.";

    UIAlertController *alert =
        [UIAlertController alertControllerWithTitle:title
                                            message:message
                                     preferredStyle:UIAlertControllerStyleAlert];

    NSString *keyPlaceholder = isRoot
        ? [NSString stringWithFormat:@"key (hex, %u characters)",
           (unsigned)(VMFW_DMG_KEY_BLOB_SIZE * 2u)]
        : @"key (hex, 32, 48 or 64 characters)";

    /* Not secure text entry. A key is long, hand-copied and easy to mistype,
     * and a field the user cannot read is a field they cannot check. */
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.placeholder = keyPlaceholder;
        field.keyboardType = UIKeyboardTypeASCIICapable;
        field.autocorrectionType = UITextAutocorrectionTypeNo;
        field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        field.spellCheckingType = UITextSpellCheckingTypeNo;
        field.smartQuotesType = UITextSmartQuotesTypeNo;
        field.smartDashesType = UITextSmartDashesTypeNo;
        field.smartInsertDeleteType = UITextSmartInsertDeleteTypeNo;
        field.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    if (!isRoot) {
        [alert addTextFieldWithConfigurationHandler:^(UITextField *field) {
            field.placeholder = @"IV (hex, 32 characters)";
            field.keyboardType = UIKeyboardTypeASCIICapable;
            field.autocorrectionType = UITextAutocorrectionTypeNo;
            field.autocapitalizationType = UITextAutocapitalizationTypeNone;
            field.spellCheckingType = UITextSpellCheckingTypeNo;
            field.smartQuotesType = UITextSmartQuotesTypeNo;
            field.smartDashesType = UITextSmartDashesTypeNo;
            field.smartInsertDeleteType = UITextSmartInsertDeleteTypeNo;
            field.clearButtonMode = UITextFieldViewModeWhileEditing;
        }];
    }

    /* Weak both ways: the alert retains its actions, the actions retain their
     * blocks, and a block holding the alert or this controller strongly would
     * keep both alive after dismissal. */
    __weak VMFirmwareImportViewController *weakSelf = self;
    __weak UIAlertController *weakAlert = alert;

    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Use this key"
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        (void)action;
        NSArray<UITextField *> *fields = weakAlert.textFields;
        NSString *keyText = (fields.count > 0) ? (fields[0].text ?: @"") : @"";
        NSString *ivText  = (fields.count > 1) ? (fields[1].text ?: @"") : @"";
        [weakSelf applyKeyText:keyText iv:ivText forArtefact:which];
    }]];

    [self presentViewController:alert animated:YES completion:nil];
}

- (void)applyKeyText:(NSString *)keyText
                  iv:(NSString *)ivText
         forArtefact:(vm_fw_artefact_t)which {
    const vm_fw_status_t status = (which == VM_FW_ROOT_FILESYSTEM)
        ? [_importer setRootFilesystemKeyHex:keyText]
        : [_importer setKeyHex:keyText ivHex:ivText forArtefact:which];

    if (status != VM_FW_OK) {
        /*
         * The parser's own reason, not "invalid key". A wrong length and a
         * stray character are different mistakes with different fixes, and the
         * C setter stages the whole thing before committing, so a refusal
         * leaves nothing half-set behind.
         *
         * The message says what was wrong with the key. It never echoes it.
         */
        UIAlertController *refused = [UIAlertController
            alertControllerWithTitle:@"That key was not accepted"
                             message:VMStringFromC(vm_fw_strerror(status))
                      preferredStyle:UIAlertControllerStyleAlert];
        [refused addAction:[UIAlertAction actionWithTitle:@"OK"
                                                    style:UIAlertActionStyleDefault
                                                  handler:nil]];
        [self presentViewController:refused animated:YES completion:nil];
        return;
    }

    [self refresh];
    [self startImportOfURL:_importer.selectedURL];
}

#pragma mark - The report

- (void)putReportOnPasteboard {
    if (!_haveReport) return;
    NSString *text = [VMFirmwareImporter renderReport:&_report];
    if (text.length == 0) return;

    [UIPasteboard generalPasteboard].string = text;
    _copiedReport = YES;
    [self refresh];
}

#pragma mark - VMFirmwareImporterDelegate

- (void)importer:(VMFirmwareImporter *)importer
   didReachStage:(vm_fw_stage_t)stage
     forArtefact:(vm_fw_artefact_t)artefact
        fraction:(double)fraction {
    BOOL wasRunning = _running;
    _running = importer.isRunning;
    _pickedURL = importer.selectedURL;
    _haveReport = NO;
    _stage = stage;
    _stageArtefact = artefact;
    _fraction = fraction;
    if (!wasRunning) [self refresh];

    /*
     * The one cell that moves is updated in place. Reloading the row several
     * times a second would fight the user's scrolling and rebuild a cell that
     * only needs two strings and a float changed.
     */
    const NSInteger visible = [self visibleIndexOfSection:VMImportSectionProgress];
    if (visible < 0) return;
    NSIndexPath *path = [NSIndexPath indexPathForRow:VMImportProgressRowBar
                                           inSection:visible];
    UITableViewCell *cell = [self.tableView cellForRowAtIndexPath:path];
    if ([cell isKindOfClass:[VMImportProgressCell class]])
        [self configureProgressCell:(VMImportProgressCell *)cell];
}

- (void)importer:(VMFirmwareImporter *)importer
    didFinishWithStatus:(vm_fw_status_t)status
                 report:(const vm_fw_report_t *)report {
    (void)importer;
    (void)status;   /* report->status drives the visible result summary */

    if (report) {
        _report = *report;
        _haveReport = YES;
    }
    _running = NO;
    _cancelRequested = NO;
    _copiedReport = NO;
    _fraction = -1.0;
    _stage = VM_FW_STAGE_DONE;

    [self refresh];
    [self revealImportState];
}

@end
