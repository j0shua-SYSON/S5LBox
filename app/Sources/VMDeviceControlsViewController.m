// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMDeviceControlsViewController.h"
#import "VMHardwareButton.h"

@implementation VMDeviceControlsViewController {
    NSMutableArray<VMHardwareButton *> *_hardware;
    UISwitch *_silent;
    UIButton *_pause, *_restart, *_save;
    UILabel *_availability;
    NSTimer *_timer;
}

static UILabel *ControlLabel(NSString *text, UIFontTextStyle style) {
    UILabel *label = [UILabel new];
    label.text = text;
    label.font = [UIFont preferredFontForTextStyle:style];
    label.adjustsFontForContentSizeCategory = YES;
    label.numberOfLines = 0;
    return label;
}

- (void)styleButton:(UIButton *)button title:(NSString *)title symbol:(NSString *)symbol {
    button.translatesAutoresizingMaskIntoConstraints = NO;
    button.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    button.layer.cornerRadius = 13;
    button.tintColor = UIColor.labelColor;
    [button setTitle:title forState:UIControlStateNormal];
    [button setTitleColor:UIColor.labelColor forState:UIControlStateNormal];
    [button setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    button.titleLabel.adjustsFontForContentSizeCategory = YES;
    button.titleLabel.numberOfLines = 0;
    button.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    button.contentEdgeInsets = UIEdgeInsetsMake(16, 18, 16, 18);
    button.titleEdgeInsets = UIEdgeInsetsMake(0, 12, 0, -12);
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:58].active = YES;
}

- (VMHardwareButton *)hardwareButton:(VMButton)which title:(NSString *)title symbol:(NSString *)symbol {
    VMHardwareButton *button = [[VMHardwareButton alloc] initWithFrame:CGRectZero];
    [self styleButton:button title:title symbol:symbol];
    button.accessibilityIdentifier = [NSString stringWithFormat:@"s5lbox.hardware.%lu", (unsigned long)which];
    __weak VMDeviceControlsViewController *weakSelf = self;
    button.pressChanged = ^(BOOL pressed) { [weakSelf.controlDelegate setDeviceButton:which pressed:pressed]; };
    [_hardware addObject:button];
    return button;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Device controls";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.view.accessibilityIdentifier = @"s5lbox.device-controls";
    _hardware = [NSMutableArray array];
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone target:self action:@selector(done)];
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    scroll.alwaysBounceVertical = YES;
    [self.view addSubview:scroll];
    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 12;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:16],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-28],
        [stack.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor constant:20],
        [stack.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor constant:-20],
        [stack.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor constant:-40]
    ]];
    UILabel *intro = ControlLabel(@"These buttons control the guest iPhone.", UIFontTextStyleSubheadline);
    intro.textColor = UIColor.secondaryLabelColor;
    [stack addArrangedSubview:intro];
    VMHardwareButton *power = [self hardwareButton:VMButtonPower title:@"Sleep / Wake" symbol:@"power"];
    power.accessibilityHint = @"Press and hold to show the guest power menu.";
    [stack addArrangedSubview:power];
    UILabel *hold = ControlLabel(@"Hold for the power menu.", UIFontTextStyleFootnote);
    hold.textColor = UIColor.secondaryLabelColor;
    [stack setCustomSpacing:5 afterView:power];
    [stack addArrangedSubview:hold];

    UIView *silentRow = [UIView new];
    silentRow.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    silentRow.layer.cornerRadius = 13;
    UILabel *silentLabel = ControlLabel(@"Silent mode", UIFontTextStyleBody);
    silentLabel.translatesAutoresizingMaskIntoConstraints = NO;
    _silent = [UISwitch new];
    _silent.translatesAutoresizingMaskIntoConstraints = NO;
    _silent.accessibilityLabel = @"Guest silent mode";
    _silent.accessibilityIdentifier = @"s5lbox.hardware.silent";
    [_silent addTarget:self action:@selector(silentChanged) forControlEvents:UIControlEventValueChanged];
    [silentRow addSubview:silentLabel];
    [silentRow addSubview:_silent];
    [NSLayoutConstraint activateConstraints:@[
        [silentRow.heightAnchor constraintGreaterThanOrEqualToConstant:62],
        [silentLabel.leadingAnchor constraintEqualToAnchor:silentRow.leadingAnchor constant:18],
        [silentLabel.topAnchor constraintEqualToAnchor:silentRow.topAnchor constant:16],
        [silentLabel.bottomAnchor constraintEqualToAnchor:silentRow.bottomAnchor constant:-16],
        [silentLabel.trailingAnchor constraintLessThanOrEqualToAnchor:_silent.leadingAnchor constant:-16],
        [_silent.trailingAnchor constraintEqualToAnchor:silentRow.trailingAnchor constant:-18],
        [_silent.centerYAnchor constraintEqualToAnchor:silentRow.centerYAnchor]
    ]];
    [stack addArrangedSubview:silentRow];
    UILabel *volumeLabel = ControlLabel(@"Volume", UIFontTextStyleSubheadline);
    volumeLabel.textColor = UIColor.secondaryLabelColor;
    [stack addArrangedSubview:volumeLabel];
    UIStackView *volume = [[UIStackView alloc] initWithArrangedSubviews:@[
        [self hardwareButton:VMButtonVolumeDown title:@"Quieter" symbol:@"speaker.fill"],
        [self hardwareButton:VMButtonVolumeUp title:@"Louder" symbol:@"plus"]]];
    volume.axis = UIContentSizeCategoryIsAccessibilityCategory(self.traitCollection.preferredContentSizeCategory)
        ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    volume.distribution = UIStackViewDistributionFillEqually;
    volume.spacing = 12;
    volume.tag = 410;
    [stack addArrangedSubview:volume];
    _availability = ControlLabel(@"", UIFontTextStyleFootnote);
    _availability.textColor = UIColor.secondaryLabelColor;
    [stack addArrangedSubview:_availability];
    UILabel *session = ControlLabel(@"Session", UIFontTextStyleSubheadline);
    session.textColor = UIColor.secondaryLabelColor;
    [stack setCustomSpacing:22 afterView:volume];
    [stack addArrangedSubview:session];
    _pause = [UIButton buttonWithType:UIButtonTypeSystem];
    [self styleButton:_pause title:@"Pause emulation" symbol:@"pause"];
    _pause.accessibilityIdentifier = @"s5lbox.controls.pause";
    [_pause addTarget:self action:@selector(pauseTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:_pause];
    _restart = [UIButton buttonWithType:UIButtonTypeSystem];
    [self styleButton:_restart title:@"Restart…" symbol:@"arrow.clockwise"];
    [_restart addTarget:self action:@selector(restartTapped) forControlEvents:UIControlEventTouchUpInside];
    _restart.accessibilityIdentifier = @"s5lbox.controls.restart";
    [stack addArrangedSubview:_restart];
    _save = [UIButton buttonWithType:UIButtonTypeSystem];
    [self styleButton:_save title:@"Save & close" symbol:@"tray.and.arrow.down"];
    [_save addTarget:self action:@selector(saveTapped) forControlEvents:UIControlEventTouchUpInside];
    [stack addArrangedSubview:_save];
    [self refresh];
}
- (void)traitCollectionDidChange:(UITraitCollection *)previous {
    [super traitCollectionDidChange:previous];
    UIStackView *volume = [self.view viewWithTag:410];
    volume.axis = UIContentSizeCategoryIsAccessibilityCategory(self.traitCollection.preferredContentSizeCategory)
        ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
}
- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self refresh];
    __weak VMDeviceControlsViewController *weakSelf = self;
    [_timer invalidate];
    _timer = [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(__unused NSTimer *timer) {
        [weakSelf refresh];
    }];
}
- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    for (VMHardwareButton *button in _hardware) [button releasePress];
}
- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated];
    [_timer invalidate]; _timer = nil;
}
- (void)dealloc { [_timer invalidate]; }
- (void)refresh {
    id<VMDeviceControlsDelegate> delegate = self.controlDelegate;
    NSString *reason = delegate ? [delegate deviceButtonUnavailableReason] : @"The machine is no longer open.";
    BOOL paused = [delegate runtimePaused];
    for (VMHardwareButton *button in _hardware) button.enabled = !reason.length && !paused;
    _silent.enabled = !reason.length && !paused;
    _silent.on = [delegate deviceSilent];
    _availability.text = paused ? @"Resume emulation to use the device buttons." : reason;
    _availability.hidden = !_availability.text.length;
    _pause.enabled = [delegate runtimeCanControlGuest];
    [_pause setTitle:paused ? @"Resume emulation" : @"Pause emulation" forState:UIControlStateNormal];
    [_pause setImage:[UIImage systemImageNamed:paused ? @"play" : @"pause"] forState:UIControlStateNormal];
    _restart.enabled = [delegate runtimeCanForcePowerOff];
    _save.enabled = [delegate runtimeCanControlGuest];
}
- (void)silentChanged { [self.controlDelegate setDeviceButton:VMButtonRingerSilent pressed:_silent.on]; [self refresh]; }
- (void)pauseTapped {
    for (VMHardwareButton *button in _hardware) [button releasePress];
    [self.controlDelegate setRuntimePaused:![self.controlDelegate runtimePaused]];
    [self refresh];
}
- (void)done { [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)performAfterDismiss:(VMRuntimeAction)action {
    for (VMHardwareButton *button in _hardware) [button releasePress];
    id<VMDeviceControlsDelegate> delegate = self.controlDelegate;
    [self dismissViewControllerAnimated:YES completion:^{ [delegate performRuntimeAction:action]; }];
}
- (void)saveTapped { [self performAfterDismiss:VMRuntimeActionSaveAndClose]; }
- (void)restartTapped {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Restart this guest?"
        message:@"This starts a fresh boot. Unsaved guest work may be lost." preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak VMDeviceControlsViewController *weakSelf = self;
    [alert addAction:[UIAlertAction actionWithTitle:@"Restart" style:UIAlertActionStyleDestructive
        handler:^(__unused UIAlertAction *action) { [weakSelf performAfterDismiss:VMRuntimeActionRestart]; }]];
    [self presentViewController:alert animated:YES completion:nil];
}
@end
