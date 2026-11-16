// A native, resumable first-run path over the same importer used by Settings.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMSetupViewController.h"
#import "VMFirmwareImporter.h"
#import "VMFirmwareDownloader.h"
#import "VMSettingsViewController.h"
#import "VMSettings.h"
#import "VMFirmwareStore.h"
#include <math.h>

@interface VMSetupViewController () <UIDocumentPickerDelegate, VMFirmwareImporterDelegate>
@end

@implementation VMSetupViewController {
    VMFirmwareImporter *_importer;
    VMFirmwareDownloader *_downloader;
    UILabel *_status, *_detail, *_file, *_hint;
    UIImageView *_statusIcon;
    UIProgressView *_progress;
    UIActivityIndicatorView *_spinner;
    UIButton *_primary, *_chooseAnother;
    BOOL _ready;
    BOOL _manualAttempt;
}

static UILabel *SetupLabel(NSString *text, UIFontTextStyle style) {
    UILabel *label = [UILabel new];
    label.text = text;
    label.font = [UIFont preferredFontForTextStyle:style];
    label.adjustsFontForContentSizeCategory = YES;
    label.numberOfLines = 0;
    label.lineBreakMode = NSLineBreakByWordWrapping;
    [label setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    return label;
}

static NSString *SetupFailureDetail(const vm_fw_report_t *report) {
    switch (report->status) {
        case VM_FW_ERR_CANCELLED:
            return @"Try again to continue setup.";
        case VM_FW_ERR_NOT_AN_ARCHIVE:
        case VM_FW_ERR_ARCHIVE_MALFORMED:
            return @"This isn’t a valid IPSW. Choose a different file.";
        case VM_FW_ERR_ARCHIVE_UNREADABLE:
            return @"Couldn’t open this file. Download it in Files, then try again.";
        case VM_FW_ERR_NO_MANIFEST:
        case VM_FW_ERR_MANIFEST_TOO_BIG:
        case VM_FW_ERR_MANIFEST_MALFORMED:
        case VM_FW_ERR_MANIFEST_INCOMPLETE:
            return @"This IPSW is incomplete. Choose iPhone OS 3.1.3 firmware for iPhone 3G.";
        case VM_FW_ERR_UNSUPPORTED_DEVICE:
        case VM_FW_ERR_UNSUPPORTED_BUILD:
            return @"Choose iPhone OS 3.1.3 (7E18) firmware for iPhone 3G.";
        case VM_FW_ERR_VERIFICATION:
            return @"The firmware failed verification. Try an unmodified IPSW, or clear manual key overrides in Settings.";
        case VM_FW_ERR_OUT_OF_MEMORY:
            return @"Not enough memory. Close other apps and try again.";
        default: {
            NSString *detail = [NSString stringWithUTF8String:report->detail];
            return detail.length ? detail : @"Choose the supported IPSW and try again. Details are available in Settings.";
        }
    }
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Setup";
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.view.accessibilityIdentifier = @"s5lbox.setup";
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc]
        initWithTitle:@"Later" style:UIBarButtonItemStylePlain target:self action:@selector(closeTapped)];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithImage:([UIImage systemImageNamed:@"gearshape"] ?: [UIImage systemImageNamed:@"gear"])
        style:UIBarButtonItemStylePlain target:self action:@selector(settingsTapped)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel = @"Settings";
    _importer = VMFirmwareImporter.sharedImporter;
    _downloader = VMFirmwareDownloader.sharedDownloader;

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.alwaysBounceVertical = NO;
    [self.view addSubview:scroll];
    UIStackView *actions = [UIStackView new];
    actions.axis = UILayoutConstraintAxisVertical;
    actions.spacing = 8;
    actions.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:actions];
    UIView *canvas = [UIView new];
    canvas.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:canvas];
    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 28;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [canvas addSubview:stack];
    // Center the welcome group on roomy screens; allow scrolling on smaller
    // screens, in landscape, and at accessibility text sizes. The action stays
    // within the safe area rather than underneath the scrolling content.
    NSLayoutConstraint *canvasHeight = [canvas.heightAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.heightAnchor];
    canvasHeight.priority = UILayoutPriorityDefaultLow;
    NSLayoutConstraint *columnWidth = [stack.widthAnchor constraintEqualToAnchor:canvas.widthAnchor constant:-48];
    columnWidth.priority = UILayoutPriorityDefaultHigh;
    NSLayoutConstraint *actionWidth = [actions.widthAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.widthAnchor constant:-48];
    actionWidth.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:actions.topAnchor constant:-12],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [canvas.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [canvas.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [canvas.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [canvas.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [canvas.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
        [canvas.heightAnchor constraintGreaterThanOrEqualToAnchor:scroll.frameLayoutGuide.heightAnchor],
        canvasHeight,
        [stack.centerXAnchor constraintEqualToAnchor:canvas.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:canvas.centerYAnchor],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:canvas.topAnchor constant:24],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:canvas.bottomAnchor constant:-24],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:canvas.widthAnchor constant:-48],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:520], columnWidth,
        [actions.centerXAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.centerXAnchor],
        [actions.widthAnchor constraintLessThanOrEqualToAnchor:self.view.safeAreaLayoutGuide.widthAnchor constant:-48],
        [actions.widthAnchor constraintLessThanOrEqualToConstant:520], actionWidth,
        [actions.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-16]
    ]];
    NSDictionary *primaryIcon = NSBundle.mainBundle.infoDictionary[@"CFBundleIcons"][@"CFBundlePrimaryIcon"];
    NSString *iconName = [primaryIcon[@"CFBundleIconFiles"] lastObject];
    UIImage *icon = iconName.length ? [UIImage imageNamed:iconName] : nil;
    UIImageView *brand = [[UIImageView alloc] initWithImage:icon ?: [UIImage systemImageNamed:@"iphone"]];
    brand.tintColor = UIColor.systemBlueColor;
    brand.contentMode = UIViewContentModeScaleAspectFit;
    brand.layer.cornerRadius = 24;
    brand.layer.cornerCurve = kCACornerCurveContinuous;
    brand.clipsToBounds = YES;
    brand.isAccessibilityElement = NO;
    UIStackView *hero = [UIStackView new];
    hero.axis = UILayoutConstraintAxisVertical;
    hero.alignment = UIStackViewAlignmentCenter;
    hero.spacing = 24;
    [hero addArrangedSubview:brand];
    [brand.widthAnchor constraintEqualToConstant:104].active = YES;
    [brand.heightAnchor constraintEqualToAnchor:brand.widthAnchor].active = YES;
    UILabel *title = SetupLabel(@"Welcome to\nS5LBox", UIFontTextStyleLargeTitle);
    title.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleLargeTitle]
        scaledFontForFont:[UIFont systemFontOfSize:32 weight:UIFontWeightBold]];
    title.textAlignment = NSTextAlignmentCenter;
    title.accessibilityTraits |= UIAccessibilityTraitHeader;
    [hero addArrangedSubview:title];
    [title.widthAnchor constraintEqualToAnchor:hero.widthAnchor].active = YES;
    [stack addArrangedSubview:hero];

    UIView *card = [UIView new];
    card.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    card.layer.cornerRadius = 20;
    card.layer.cornerCurve = kCACornerCurveContinuous;
    UIStackView *contents = [UIStackView new];
    contents.axis = UILayoutConstraintAxisVertical;
    contents.spacing = 14;
    contents.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:contents];
    [NSLayoutConstraint activateConstraints:@[
        [contents.topAnchor constraintEqualToAnchor:card.topAnchor constant:20],
        [contents.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-20],
        [contents.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:20],
        [contents.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-20]
    ]];

    UIStackView *state = [UIStackView new];
    state.axis = UILayoutConstraintAxisHorizontal;
    state.alignment = UIStackViewAlignmentCenter;
    state.spacing = 14;
    _statusIcon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"square.and.arrow.down"]];
    _statusIcon.contentMode = UIViewContentModeScaleAspectFit;
    NSLayoutConstraint *statusWidth = [_statusIcon.widthAnchor constraintEqualToConstant:24];
    statusWidth.priority = UILayoutPriorityDefaultHigh;
    statusWidth.active = YES;
    [_statusIcon.heightAnchor constraintEqualToConstant:28].active = YES;
    _statusIcon.isAccessibilityElement = NO;
    _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _spinner.hidesWhenStopped = YES;
    _status = SetupLabel(@"Firmware", UIFontTextStyleHeadline);
    _status.accessibilityIdentifier = @"s5lbox.setup.status";
    [state addArrangedSubview:_statusIcon];
    [state addArrangedSubview:_spinner];
    UIStackView *description = [UIStackView new];
    description.axis = UILayoutConstraintAxisVertical;
    description.spacing = 4;
    [description addArrangedSubview:_status];
    _detail = SetupLabel(@"", UIFontTextStyleSubheadline);
    _detail.textColor = UIColor.secondaryLabelColor;
    _detail.accessibilityIdentifier = @"s5lbox.setup.detail";
    [description addArrangedSubview:_detail];
    [state addArrangedSubview:description];
    [contents addArrangedSubview:state];
    _file = SetupLabel(@"", UIFontTextStyleCaption1);
    _file.textColor = UIColor.secondaryLabelColor;
    _file.numberOfLines = 1;
    _file.lineBreakMode = NSLineBreakByTruncatingMiddle;
    [contents addArrangedSubview:_file];
    _progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _progress.accessibilityLabel = @"Firmware preparation progress";
    [contents addArrangedSubview:_progress];
    [stack addArrangedSubview:card];
    _hint = SetupLabel(@"Keep S5LBox open.", UIFontTextStyleFootnote);
    _hint.textColor = UIColor.secondaryLabelColor;
    _hint.textAlignment = NSTextAlignmentCenter;
    [stack addArrangedSubview:_hint];
    [stack setCustomSpacing:12 afterView:card];
    _primary = [UIButton buttonWithType:UIButtonTypeSystem];
    _primary.backgroundColor = UIColor.systemBlueColor;
    _primary.layer.cornerRadius = 16;
    _primary.layer.cornerCurve = kCACornerCurveContinuous;
    [_primary setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    _primary.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _primary.titleLabel.adjustsFontForContentSizeCategory = YES;
    _primary.titleLabel.numberOfLines = 0;
    _primary.titleLabel.textAlignment = NSTextAlignmentCenter;
    _primary.contentEdgeInsets = UIEdgeInsetsMake(14, 18, 14, 18);
    [_primary.heightAnchor constraintGreaterThanOrEqualToConstant:54].active = YES;
    [_primary addTarget:self action:@selector(primaryTapped) forControlEvents:UIControlEventTouchUpInside];
    _primary.accessibilityIdentifier = @"s5lbox.setup.primary";
    [actions addArrangedSubview:_primary];
    _chooseAnother = [UIButton buttonWithType:UIButtonTypeSystem];
    [_chooseAnother setTitle:@"Import IPSW" forState:UIControlStateNormal];
    _chooseAnother.accessibilityIdentifier = @"s5lbox.setup.import";
    _chooseAnother.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _chooseAnother.titleLabel.adjustsFontForContentSizeCategory = YES;
    _chooseAnother.titleLabel.numberOfLines = 0;
    _chooseAnother.titleLabel.textAlignment = NSTextAlignmentCenter;
    _chooseAnother.contentEdgeInsets = UIEdgeInsetsMake(10, 12, 10, 12);
    [_chooseAnother.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [_chooseAnother addTarget:self action:@selector(presentPicker) forControlEvents:UIControlEventTouchUpInside];
    [actions addArrangedSubview:_chooseAnother];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(becameActive)
        name:UIApplicationDidBecomeActiveNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(downloadChanged:)
        name:VMFirmwareDownloadDidChangeNotification object:_downloader];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}
- (void)downloadChanged:(NSNotification *)notification {
    (void)notification;
    [self refresh];
    if (!_downloader.running)
        UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, _status.text);
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    _importer.delegate = self;
    if (_importer.isRunning && _downloader.phase != VMFirmwareDownloadImporting) _manualAttempt = YES;
    [self refresh];
    [_importer replayStateToDelegate];
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self becameActive];
}
- (void)becameActive {
    if (!self.view.window || self.presentedViewController || self.navigationController.topViewController != self) return;
    _importer.delegate = self;
    if (!_downloader.running && [_importer importDetectedIPSWIfNeeded]) _manualAttempt = YES;
    [self refresh];
}
- (void)refresh {
    BOOL running = _importer.isRunning || _downloader.running;
    BOOL receiving = _downloader.running && _downloader.phase != VMFirmwareDownloadImporting;
    BOOL downloadFailed = !_manualAttempt && (_downloader.phase == VMFirmwareDownloadFailed ||
        _downloader.phase == VMFirmwareDownloadCancelled);
    _ready = !running && [VMFirmwareImporter hasConfiguredFirmware];
    vm_fw_report_t report;
    BOOL haveReport = [_importer getLastReport:&report];
    BOOL failed = !running && !_ready && (downloadFailed || (haveReport && !vm_fw_report_is_prepared(&report)));
    self.modalInPresentation = running;
    self.navigationController.modalInPresentation = running;
    self.navigationItem.leftBarButtonItem.title = running ? @"Cancel" : @"Later";
    self.navigationItem.leftBarButtonItem.enabled = !(_downloader.running && _downloader.cancellationRequested);
    self.navigationItem.rightBarButtonItem.enabled = !running;
    _primary.enabled = !running;
    _primary.alpha = running ? 0.55 : 1;
    [_primary setTitle:receiving ? @"Downloading…" : running ? @"Preparing…"
        : _ready ? @"Start iPhone OS" : downloadFailed ? @"Try again" : @"Download iPhone OS"
        forState:UIControlStateNormal];
    _chooseAnother.hidden = running || _ready;
    _file.text = _importer.selectedURL.lastPathComponent;
    _file.hidden = !_file.text.length || receiving || !_manualAttempt || (!running && !failed);
    _hint.hidden = !running;
    _statusIcon.hidden = running;
    _spinner.hidden = !running;
    if (running) [_spinner startAnimating]; else [_spinner stopAnimating];
    double fraction = receiving ? _downloader.fraction : _importer.fraction;
    _progress.accessibilityLabel = receiving ? @"Firmware download progress" : @"Firmware preparation progress";
    _progress.hidden = !running || fraction < 0;
    if (!_progress.hidden) _progress.progress = (float)fmax(0, fmin(1, fraction));
    if (_downloader.running && _downloader.cancellationRequested) {
        _status.text = @"Cancelling…";
        _detail.text = @"Finishing the current step.";
    } else if (receiving) {
        if (_downloader.phase == VMFirmwareDownloadResolving) {
            _status.text = @"Finding firmware";
            _detail.text = @"Connecting to ipsw.me…";
        } else if (_downloader.phase == VMFirmwareDownloadVerifying) {
            _status.text = @"Verifying download";
            _detail.text = @"Checking the firmware checksum…";
        } else {
            _status.text = @"Downloading firmware";
            _detail.text = [NSString stringWithFormat:@"From Apple · %.0f%%", fmax(0, fraction) * 100];
        }
    } else if (running) {
        _status.text = @"Preparing firmware";
        NSString *stage = [NSString stringWithUTF8String:vm_fw_stage_name(_importer.stage)];
        NSString *part = [NSString stringWithUTF8String:vm_fw_artefact_title(_importer.stageArtefact)];
        _detail.text = _importer.stage <= VM_FW_STAGE_LOCATING
            ? @"Opening IPSW…"
            : [NSString stringWithFormat:@"%@ — %@%@",
                part, stage, _importer.fraction < 0 ? @"" : [NSString stringWithFormat:@" · %.0f%%", _importer.fraction * 100]];
    } else if (failed) {
        BOOL cancelled = downloadFailed ? _downloader.phase == VMFirmwareDownloadCancelled : report.status == VM_FW_ERR_CANCELLED;
        _status.text = cancelled ? @"Setup cancelled" : @"Couldn’t prepare firmware";
        _statusIcon.image = [UIImage systemImageNamed:@"exclamationmark.circle"];
        _statusIcon.tintColor = UIColor.systemOrangeColor;
        _detail.text = downloadFailed ? (_downloader.error.localizedDescription ?: @"Try again to continue setup.") : SetupFailureDetail(&report);
    } else if (_ready) {
        _status.text = @"Firmware ready";
        _statusIcon.image = [UIImage systemImageNamed:@"checkmark.circle.fill"];
        _statusIcon.tintColor = UIColor.systemGreenColor;
        _detail.text = [VMFirmwareStore displayNameInDirectory:VMSettings.sharedSettings.firmwareDirectory];
    } else {
        _status.text = @"Firmware";
        _statusIcon.image = [UIImage systemImageNamed:@"square.and.arrow.down"];
        _statusIcon.tintColor = UIColor.systemBlueColor;
        _detail.text = @"iPhone OS 3.1.3 for iPhone 3G";
    }
}
- (void)primaryTapped {
    if (_importer.isRunning || _downloader.running) return;
    if (_ready) { if (self.completion) self.completion(YES); return; }
    _manualAttempt = NO;
    _importer.delegate = self;
    [_downloader start];
    [self refresh];
}
- (void)closeTapped {
    if (_downloader.running) { [_downloader cancel]; [self refresh]; return; }
    if (_importer.isRunning) { [_importer cancelImport]; _detail.text = @"Cancelling…"; return; }
    if (self.completion) self.completion(NO);
}
- (void)settingsTapped {
    VMSettingsViewController *settings = [VMSettingsViewController new];
    [self presentViewController:[[UINavigationController alloc] initWithRootViewController:settings]
        animated:YES completion:nil];
}
- (void)presentPicker {
    if (_importer.isRunning || _downloader.running || self.presentedViewController) return;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initWithDocumentTypes:@[@"public.data"] inMode:UIDocumentPickerModeOpen];
#pragma clang diagnostic pop
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    if (!urls.firstObject || _importer.isRunning || _downloader.running) return;
    _manualAttempt = YES;
    _importer.delegate = self;
    [_importer importIPSWAtURL:urls.firstObject]; // Take security scope before dismissal.
    [controller dismissViewControllerAnimated:YES completion:^{ [self refresh]; }];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentAtURL:(NSURL *)url {
    [self documentPicker:controller didPickDocumentsAtURLs:url ? @[url] : @[]];
}
- (void)importer:(VMFirmwareImporter *)importer didReachStage:(vm_fw_stage_t)stage
    forArtefact:(vm_fw_artefact_t)artefact fraction:(double)fraction {
    (void)importer; (void)stage; (void)artefact; (void)fraction;
    [self refresh];
}
- (void)importer:(VMFirmwareImporter *)importer didFinishWithStatus:(vm_fw_status_t)status report:(const vm_fw_report_t *)report {
    (void)importer; (void)status; (void)report;
    [self refresh];
    UIAccessibilityPostNotification(UIAccessibilityAnnouncementNotification, _status.text);
}
@end
