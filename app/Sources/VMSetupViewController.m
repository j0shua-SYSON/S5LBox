// A native, resumable first-run path over the same importer used by Settings.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMSetupViewController.h"
#import "VMFirmwareImporter.h"
#import "VMSettingsViewController.h"
#include <math.h>

@interface VMSetupViewController () <UIDocumentPickerDelegate, VMFirmwareImporterDelegate>
@end

@implementation VMSetupViewController {
    VMFirmwareImporter *_importer;
    UILabel *_status, *_detail, *_file;
    UIImageView *_statusIcon;
    UIProgressView *_progress;
    UIActivityIndicatorView *_spinner;
    UIButton *_primary, *_chooseAnother;
    BOOL _ready;
}

static UILabel *SetupLabel(NSString *text, UIFontTextStyle style) {
    UILabel *label = [UILabel new];
    label.text = text;
    label.font = [UIFont preferredFontForTextStyle:style];
    label.adjustsFontForContentSizeCategory = YES;
    label.numberOfLines = 0;
    return label;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Welcome";
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.view.accessibilityIdentifier = @"s5lbox.setup";
    self.navigationItem.leftBarButtonItem = [[UIBarButtonItem alloc]
        initWithTitle:@"Not now" style:UIBarButtonItemStylePlain target:self action:@selector(closeTapped)];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithTitle:@"Settings" style:UIBarButtonItemStylePlain target:self action:@selector(settingsTapped)];
    _importer = VMFirmwareImporter.sharedImporter;

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.alwaysBounceVertical = YES;
    [self.view addSubview:scroll];
    UIStackView *actions = [UIStackView new];
    actions.axis = UILayoutConstraintAxisVertical;
    actions.spacing = 4;
    actions.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:actions];
    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 20;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:actions.topAnchor constant:-12],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:28],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-28],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:24],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-24],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-48],
        [actions.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor constant:24],
        [actions.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-24],
        [actions.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor constant:-16]
    ]];
    UIImageView *phone = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"iphone"]];
    phone.tintColor = UIColor.systemBlueColor;
    phone.contentMode = UIViewContentModeScaleAspectFit;
    phone.isAccessibilityElement = NO;
    [phone.heightAnchor constraintEqualToConstant:52].active = YES;
    [stack addArrangedSubview:phone];
    UILabel *title = SetupLabel(@"Set up S5LBox", UIFontTextStyleTitle1);
    title.accessibilityTraits |= UIAccessibilityTraitHeader;
    [stack addArrangedSubview:title];
    UILabel *intro = SetupLabel(@"Bring your own IPSW. S5LBox prepares it automatically, with no keys to enter.", UIFontTextStyleBody);
    intro.textColor = UIColor.secondaryLabelColor;
    [stack addArrangedSubview:intro];
    UILabel *supported = SetupLabel(@"iPhone 3G · iPhone OS 3.1.3\nBuild 7E18 · iPhone1,2", UIFontTextStyleSubheadline);
    supported.accessibilityLabel = @"Supported firmware: iPhone 3G, iPhone OS 3.1.3, build 7E18, iPhone1,2.";
    [stack addArrangedSubview:supported];

    UIStackView *state = [UIStackView new];
    state.axis = UILayoutConstraintAxisHorizontal;
    state.alignment = UIStackViewAlignmentCenter;
    state.spacing = 10;
    _statusIcon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"square.and.arrow.down"]];
    _statusIcon.contentMode = UIViewContentModeScaleAspectFit;
    [_statusIcon.widthAnchor constraintEqualToConstant:24].active = YES;
    _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _spinner.hidesWhenStopped = YES;
    _status = SetupLabel(@"Choose your firmware", UIFontTextStyleHeadline);
    _status.accessibilityIdentifier = @"s5lbox.setup.status";
    [state addArrangedSubview:_statusIcon];
    [state addArrangedSubview:_spinner];
    [state addArrangedSubview:_status];
    [stack addArrangedSubview:state];
    _file = SetupLabel(@"", UIFontTextStyleCaption1);
    _file.textColor = UIColor.secondaryLabelColor;
    [stack addArrangedSubview:_file];
    _progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _progress.accessibilityLabel = @"Firmware preparation progress";
    [stack addArrangedSubview:_progress];
    _detail = SetupLabel(@"Choose an IPSW from Files, or place it in S5LBox’s firmware folder and return here.", UIFontTextStyleSubheadline);
    _detail.textColor = UIColor.secondaryLabelColor;
    _detail.accessibilityIdentifier = @"s5lbox.setup.detail";
    [stack addArrangedSubview:_detail];
    _primary = [UIButton buttonWithType:UIButtonTypeSystem];
    _primary.backgroundColor = UIColor.systemBlueColor;
    _primary.layer.cornerRadius = 12;
    [_primary setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    _primary.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _primary.titleLabel.adjustsFontForContentSizeCategory = YES;
    _primary.titleLabel.numberOfLines = 0;
    _primary.titleLabel.textAlignment = NSTextAlignmentCenter;
    _primary.contentEdgeInsets = UIEdgeInsetsMake(14, 18, 14, 18);
    [_primary.heightAnchor constraintGreaterThanOrEqualToConstant:50].active = YES;
    [_primary addTarget:self action:@selector(primaryTapped) forControlEvents:UIControlEventTouchUpInside];
    _primary.accessibilityIdentifier = @"s5lbox.setup.primary";
    [actions addArrangedSubview:_primary];
    _chooseAnother = [UIButton buttonWithType:UIButtonTypeSystem];
    [_chooseAnother setTitle:@"Choose another IPSW" forState:UIControlStateNormal];
    _chooseAnother.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _chooseAnother.titleLabel.adjustsFontForContentSizeCategory = YES;
    [_chooseAnother.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [_chooseAnother addTarget:self action:@selector(presentPicker) forControlEvents:UIControlEventTouchUpInside];
    [actions addArrangedSubview:_chooseAnother];
    UILabel *notice = SetupLabel(@"No Apple firmware is included or downloaded. Use an IPSW you are entitled to use. Advanced options are in Settings.", UIFontTextStyleFootnote);
    notice.textColor = UIColor.secondaryLabelColor;
    [stack addArrangedSubview:notice];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(becameActive)
        name:UIApplicationDidBecomeActiveNotification object:nil];
}

- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    _importer.delegate = self;
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
    [_importer importDetectedIPSWIfNeeded];
    [self refresh];
}
- (void)refresh {
    BOOL running = _importer.isRunning;
    _ready = !running && [VMFirmwareImporter hasConfiguredFirmware];
    vm_fw_report_t report;
    BOOL haveReport = [_importer getLastReport:&report];
    BOOL failed = !running && haveReport && !vm_fw_report_is_verified(&report);
    self.modalInPresentation = running;
    self.navigationController.modalInPresentation = running;
    self.navigationItem.leftBarButtonItem.title = running ? @"Cancel" : @"Not now";
    self.navigationItem.rightBarButtonItem.enabled = !running;
    _primary.enabled = !running;
    _primary.alpha = running ? 0.55 : 1;
    [_primary setTitle:running ? @"Preparing…" : _ready ? @"Start iPhone OS" : failed ? @"Try again" : @"Import IPSW"
        forState:UIControlStateNormal];
    _chooseAnother.hidden = running || !haveReport || _ready;
    _file.text = _importer.selectedURL.lastPathComponent;
    _file.hidden = !_file.text.length;
    _statusIcon.hidden = running;
    _spinner.hidden = !running;
    if (running) [_spinner startAnimating]; else [_spinner stopAnimating];
    _progress.hidden = !running || _importer.fraction < 0;
    if (!_progress.hidden) _progress.progress = (float)fmax(0, fmin(1, _importer.fraction));
    if (running) {
        _status.text = @"Preparing firmware";
        NSString *stage = [NSString stringWithUTF8String:vm_fw_stage_name(_importer.stage)];
        NSString *part = [NSString stringWithUTF8String:vm_fw_artefact_title(_importer.stageArtefact)];
        _detail.text = _importer.stage <= VM_FW_STAGE_LOCATING
            ? @"Opening and identifying the IPSW… Keep S5LBox open while it prepares your firmware."
            : [NSString stringWithFormat:@"%@ — %@%@\nKeep S5LBox open. Your existing firmware stays safe until verification finishes.",
                part, stage, _importer.fraction < 0 ? @"" : [NSString stringWithFormat:@" · %.0f%%", _importer.fraction * 100]];
    } else if (failed) {
        _status.text = report.status == VM_FW_ERR_CANCELLED ? @"Import cancelled" : @"Couldn’t prepare firmware";
        _statusIcon.image = [UIImage systemImageNamed:@"exclamationmark.circle"];
        _statusIcon.tintColor = UIColor.systemOrangeColor;
        _detail.text = [NSString stringWithUTF8String:report.detail] ?: @"Choose the supported IPSW and try again.";
        if (_ready) _detail.text = [_detail.text stringByAppendingString:@"\nYour previously configured firmware is still available."];
    } else if (_ready) {
        _status.text = @"Ready to start";
        _statusIcon.image = [UIImage systemImageNamed:@"checkmark.circle.fill"];
        _statusIcon.tintColor = UIColor.systemGreenColor;
        _detail.text = @"Your firmware is prepared. S5LBox will create the machine’s writable disk and start iPhone OS automatically.";
    } else {
        _status.text = @"Choose your firmware";
        _statusIcon.image = [UIImage systemImageNamed:@"square.and.arrow.down"];
        _statusIcon.tintColor = UIColor.systemBlueColor;
        _detail.text = @"Choose an IPSW from Files, or place it in S5LBox’s firmware folder and return here.";
    }
}
- (void)primaryTapped {
    if (_importer.isRunning) return;
    if (_ready) { if (self.completion) self.completion(YES); return; }
    if (_importer.selectedURL) [_importer importIPSWAtURL:_importer.selectedURL];
    else [self presentPicker];
    [self refresh];
}
- (void)closeTapped {
    if (_importer.isRunning) { [_importer cancelImport]; _detail.text = @"Cancelling safely…"; return; }
    if (self.completion) self.completion(NO);
}
- (void)settingsTapped {
    VMSettingsViewController *settings = [VMSettingsViewController new];
    [self presentViewController:[[UINavigationController alloc] initWithRootViewController:settings]
        animated:YES completion:nil];
}
- (void)presentPicker {
    if (_importer.isRunning || self.presentedViewController) return;
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
    if (!urls.firstObject || _importer.isRunning) return;
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
