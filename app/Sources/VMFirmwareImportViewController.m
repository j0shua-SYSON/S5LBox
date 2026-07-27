//
//  S5LBox -- VMFirmwareImportViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMFirmwareImportViewController.h"

#import "VMFirmwareImport.h"
#import "VMFirmwareImporter.h"
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
    VMImportSectionChoose = 0,
    VMImportSectionProgress,
    VMImportSectionResults,
    VMImportSectionKeys,
    VMImportSectionReport
};

typedef NS_ENUM(NSInteger, VMImportChooseRow) {
    VMImportChooseRowPick = 0,
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

    _importer = [[VMFirmwareImporter alloc] init];
    _importer.delegate = self;

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
    _intro.font = [UIFont systemFontOfSize:13.0];
    _intro.textColor = [UIColor secondaryLabelColor];
    _intro.text =
        @"This turns an IPSW you already have into the three files the "
        @"emulator accepts: kernel.macho, devicetree.bin and rootfs.img. "
        @"Nothing is downloaded, and no firmware ships with the app.\n\n"
        @"Every payload inside a 3.x IPSW is encrypted, and the keys are not "
        @"in the archive and cannot be worked out from it. S5LBox has none of "
        @"them. Where one is needed, this screen says which file needs it and "
        @"what kind it is, and you supply it.\n\n"
        @"Producing the three files is all this does. The app cannot boot them "
        @"yet.";
    UIView *header = [[UIView alloc] initWithFrame:CGRectZero];
    [header addSubview:_intro];
    self.tableView.tableHeaderView = header;

    [self rebuildSections];
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
    /*
     * Backing out of this screen is a decision to stop. The core deletes every
     * partly written output when a run is cancelled, so nothing half-made is
     * left behind in the firmware directory for the emulator's own gate to
     * reject later.
     */
    [_importer cancelImport];
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
    [visible addObject:@(VMImportSectionChoose)];
    if (_running) [visible addObject:@(VMImportSectionProgress)];
    if (_haveReport) [visible addObject:@(VMImportSectionResults)];

    /* A key row exists only where the report says a key is what is missing --
     * not merely where an artefact failed. A row offering the wrong fix is
     * worse than no row. */
    NSMutableArray<NSNumber *> *keyRows = [NSMutableArray array];
    if (_haveReport) {
        for (int i = 0; i < VM_FW_ARTEFACT_COUNT; i++)
            if (_report.artefacts[i].awaiting_key) [keyRows addObject:@(i)];
    }
    _keyRows = [keyRows copy];
    if (_keyRows.count > 0) [visible addObject:@(VMImportSectionKeys)];

    if (_haveReport) [visible addObject:@(VMImportSectionReport)];
    _visible = [visible copy];

    _chooseRowCount = (_pickedURL && !_running) ? VMImportChooseRowCount : 1;
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
        case VMImportSectionChoose:   return _chooseRowCount;
        case VMImportSectionProgress: return VMImportProgressRowCount;
        case VMImportSectionResults:  return VM_FW_ARTEFACT_COUNT;
        case VMImportSectionKeys:     return (NSInteger)_keyRows.count;
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
        case VMImportSectionKeys:     return @"Keys you supply";
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
            return @"The archive is read where it sits -- nothing is copied "
                    "into the app first, because an IPSW is around 430 MB. "
                    "Unpacking needs room for the finished root filesystem, "
                    "433 MB for a 3.1.3 iPhone1,2 build, and for a temporary "
                    "copy of the encrypted disk image alongside it. Leave the "
                    "app in front while it runs: iOS suspends what it cannot "
                    "see.";

        case VMImportSectionProgress:
            return @"Cancelling stops at the next block. Everything written so "
                    "far is deleted -- a half-written file that the emulator "
                    "would reject only after you had trusted it is worse than "
                    "no file at all.";

        case VMImportSectionResults:
            return [NSString stringWithFormat:
                    @"Files are written to\n\n%@\n\n"
                    @"\"verified\" means the bytes are identical to the "
                    @"known-good file for this build. \"extracted, "
                    @"unverified\" means it was produced, but S5LBox holds no "
                    @"reference hash for this build to check it against.",
                    [[VMSettings sharedSettings] firmwareDirectory]
                        ?: @"(no documents directory)"];

        case VMImportSectionKeys:
            return @"These are yours, not the app's. S5LBox ships no keys, "
                    "downloads none, and cannot compute any -- they were "
                    "recovered from hardware, this app has no list of them, "
                    "and it will not fetch one or suggest where to look.\n\n"
                    "Anything typed here is held in memory for this session "
                    "only. It is not written to a file, not put in the app's "
                    "settings or the keychain, and not printed to any log.\n\n"
                    "You are responsible for using firmware you are entitled "
                    "to use.\n\n"
                    "Setting a key does not re-read the archive. Use the "
                    "import row at the top once the keys are in.";

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
    cell.textLabel.font = [UIFont systemFontOfSize:16.0];
    cell.textLabel.textColor = [UIColor labelColor];
    cell.textLabel.text = nil;
    cell.detailTextLabel.numberOfLines = 0;
    cell.detailTextLabel.font = [UIFont systemFontOfSize:12.0];
    cell.detailTextLabel.textColor = [UIColor secondaryLabelColor];
    cell.detailTextLabel.text = nil;
    return cell;
}

- (void)configureProgressCell:(VMImportProgressCell *)cell {
    cell.textLabel.numberOfLines = 1;
    cell.textLabel.font = [UIFont systemFontOfSize:16.0];
    cell.textLabel.textColor = [UIColor labelColor];
    cell.textLabel.text =
        VMCapitalizeFirst(VMStringFromC(vm_fw_stage_name(_stage)));
    cell.detailTextLabel.numberOfLines = 1;
    cell.detailTextLabel.font = [UIFont systemFontOfSize:12.0];
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
            if (row < 0 || row >= VM_FW_ARTEFACT_COUNT) return cell;

            const vm_fw_artefact_t which = (vm_fw_artefact_t)row;
            const vm_fw_artefact_report_t *artefact = &_report.artefacts[row];

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
            if (row < 0 || (NSUInteger)row >= _keyRows.count) return cell;

            const vm_fw_artefact_t which =
                (vm_fw_artefact_t)_keyRows[(NSUInteger)row].intValue;
            const BOOL isRoot = (which == VM_FW_ROOT_FILESYSTEM);

            /* "you supply" in the row itself, not only in the footer: a row
             * that just said "Kernel key" could be read as a key the app has. */
            cell.textLabel.text = [NSString stringWithFormat:@"%@ %@  -  you supply",
                VMStringFromC(vm_fw_artefact_title(which)),
                isRoot ? @"key" : @"key and IV"];
            cell.detailTextLabel.text = [_importer haveKeyForArtefact:which]
                ? @"set for this session, tap to replace"
                : @"not set, tap to type or paste one";
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

/* The plural callback. The singular one is deprecated and is not implemented,
 * so there is no second path that could disagree with this one. */
- (void)documentPicker:(UIDocumentPickerViewController *)controller
didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    (void)controller;
    NSURL *url = urls.firstObject;
    if (!url) return;
    _pickedURL = url;
    [self startImportOfURL:url];
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
}

#pragma mark - Keys the user supplies

- (void)presentKeyAlertForArtefact:(vm_fw_artefact_t)which {
    const BOOL isRoot = (which == VM_FW_ROOT_FILESYSTEM);

    NSString *title = [NSString stringWithFormat:@"%@ %@",
        VMStringFromC(vm_fw_artefact_title(which)),
        isRoot ? @"key" : @"key and IV"];

    NSString *message = isRoot
        ? [NSString stringWithFormat:
            @"One hexadecimal value that you supply: %u characters, an AES key "
            @"followed by an HMAC key. S5LBox does not have it, cannot work it "
            @"out from the IPSW, and will not look for it.",
            (unsigned)(VMFW_DMG_KEY_BLOB_SIZE * 2u)]
        : @"Two hexadecimal values that you supply: the key (32, 48 or 64 "
          @"characters) and the IV (32 characters). S5LBox has neither, cannot "
          @"work either out from the IPSW, and will not look for them.\n\n"
          @"The IV is the published one, not the value inside the container -- "
          @"that one is wrapped and would corrupt the first block.";

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
    (void)importer;
    _stage = stage;
    _stageArtefact = artefact;
    _fraction = fraction;

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
    (void)status;   /* it is report->status too, and the rows say more */

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
}

@end
