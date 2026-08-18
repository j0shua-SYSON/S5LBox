//
//  S5LBox -- VMGuestInstallViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMGuestInstallViewController.h"

#import "VMGuestInstallBuild.h"
#import "VMGuestPackageDownloader.h"
#import "VMInstanceStore.h"

@interface VMGuestInstallViewController ()
- (void)startInstall;
- (void)runBuilderWithPackageDirectory:(NSURL *)packageDirectory;
- (void)failWithHeadline:(NSString *)headline
             description:(NSString *)description;
- (void)receiveBuildPhase:(vm_guest_install_build_phase_t)phase
                 completed:(uint64_t)completed total:(uint64_t)total;
@end

static void VMGuestInstallBuildProgress(
    void *context, vm_guest_install_build_phase_t phase,
    uint64_t completed, uint64_t total) {
    VMGuestInstallViewController *controller =
        (__bridge VMGuestInstallViewController *)context;
    [controller receiveBuildPhase:phase completed:completed total:total];
}

@implementation VMGuestInstallViewController {
    NSString *_instanceID;
    NSString *_machineName;
    NSString *_machineDirectory;
    UIView *_guestScreen;
    UILabel *_headline;
    UILabel *_detail;
    UIProgressView *_progressBar;
    VMGuestPackageDownloader *_downloader;
    dispatch_queue_t _buildQueue;
    UIBackgroundTaskIdentifier _backgroundTask;
    BOOL _started;
    BOOL _finished;
}

- (instancetype)initWithInstanceID:(NSString *)instanceID
                        machineName:(NSString *)machineName {
    self = [super initWithNibName:nil bundle:nil];
    if (!self) return nil;
    _instanceID = [instanceID copy];
    _machineName = [machineName copy];
    _backgroundTask = UIBackgroundTaskInvalid;
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = _machineName.length ? _machineName : @"iPhone OS 3.1.3";
    self.view.backgroundColor = [UIColor blackColor];
    self.navigationItem.hidesBackButton = YES;
    self.navigationItem.largeTitleDisplayMode =
        UINavigationItemLargeTitleDisplayModeNever;

    _guestScreen = [[UIView alloc] initWithFrame:CGRectZero];
    _guestScreen.translatesAutoresizingMaskIntoConstraints = NO;
    _guestScreen.backgroundColor = [UIColor blackColor];
    _guestScreen.layer.borderWidth = 1.0;
    _guestScreen.layer.borderColor = [UIColor darkGrayColor].CGColor;
    _guestScreen.accessibilityIdentifier = @"s5lbox.guest-install.screen";
    [self.view addSubview:_guestScreen];

    _headline = [[UILabel alloc] initWithFrame:CGRectZero];
    _headline.translatesAutoresizingMaskIntoConstraints = NO;
    _headline.text = @"Jailbreaking\u2026";
    _headline.textColor = [UIColor whiteColor];
    _headline.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    _headline.adjustsFontForContentSizeCategory = YES;
    _headline.textAlignment = NSTextAlignmentCenter;
    _headline.accessibilityIdentifier = @"s5lbox.guest-install.headline";
    [_guestScreen addSubview:_headline];

    _detail = [[UILabel alloc] initWithFrame:CGRectZero];
    _detail.translatesAutoresizingMaskIntoConstraints = NO;
    _detail.text = @"Checking this machine\u2026";
    _detail.textColor = [UIColor lightGrayColor];
    _detail.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _detail.adjustsFontForContentSizeCategory = YES;
    _detail.textAlignment = NSTextAlignmentCenter;
    _detail.numberOfLines = 0;
    [_guestScreen addSubview:_detail];

    _progressBar = [[UIProgressView alloc]
        initWithProgressViewStyle:UIProgressViewStyleDefault];
    _progressBar.translatesAutoresizingMaskIntoConstraints = NO;
    _progressBar.progress = 0.0f;
    _progressBar.accessibilityIdentifier = @"s5lbox.guest-install.progress";
    [_guestScreen addSubview:_progressBar];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint *idealWidth =
        [_guestScreen.widthAnchor constraintEqualToConstant:320.0];
    idealWidth.priority = UILayoutPriorityDefaultHigh;
    [NSLayoutConstraint activateConstraints:@[
        [_guestScreen.topAnchor constraintGreaterThanOrEqualToAnchor:safe.topAnchor
                                                            constant:16.0],
        [_guestScreen.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor
                                                            constant:-16.0],
        [_guestScreen.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [_guestScreen.centerYAnchor constraintEqualToAnchor:safe.centerYAnchor],
        [_guestScreen.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor
                                                           multiplier:0.84],
        idealWidth,
        [_guestScreen.heightAnchor constraintEqualToAnchor:_guestScreen.widthAnchor
                                                 multiplier:1.5],
        [_guestScreen.heightAnchor constraintLessThanOrEqualToAnchor:safe.heightAnchor
                                                            constant:-32.0],

        [_headline.leadingAnchor constraintEqualToAnchor:_guestScreen.leadingAnchor
                                                constant:20.0],
        [_headline.trailingAnchor constraintEqualToAnchor:_guestScreen.trailingAnchor
                                                 constant:-20.0],
        [_headline.centerYAnchor constraintEqualToAnchor:_guestScreen.centerYAnchor
                                                constant:-30.0],
        [_detail.topAnchor constraintEqualToAnchor:_headline.bottomAnchor
                                          constant:14.0],
        [_detail.leadingAnchor constraintEqualToAnchor:_guestScreen.leadingAnchor
                                              constant:20.0],
        [_detail.trailingAnchor constraintEqualToAnchor:_guestScreen.trailingAnchor
                                               constant:-20.0],
        [_progressBar.topAnchor constraintEqualToAnchor:_detail.bottomAnchor
                                               constant:22.0],
        [_progressBar.leadingAnchor constraintEqualToAnchor:_guestScreen.leadingAnchor
                                                   constant:28.0],
        [_progressBar.trailingAnchor constraintEqualToAnchor:_guestScreen.trailingAnchor
                                                    constant:-28.0]
    ]];

    _buildQueue = dispatch_queue_create(
        "com.j0shua.S5LBox.GuestInstallBuild", DISPATCH_QUEUE_SERIAL);
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    self.navigationController.interactivePopGestureRecognizer.enabled = NO;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!_started) {
        _started = YES;
        [self startInstall];
    }
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    /* This recognizer belongs to the navigation controller, not this screen.
     * Never leave the rest of the app without swipe-back after our guarded
     * install flow is popped programmatically. */
    self.navigationController.interactivePopGestureRecognizer.enabled = YES;
}

- (void)dealloc {
    [_downloader cancel];
    if (_backgroundTask != UIBackgroundTaskInvalid)
        [[UIApplication sharedApplication] endBackgroundTask:_backgroundTask];
}

- (void)beginBackgroundTime {
    if (_backgroundTask != UIBackgroundTaskInvalid) return;
    __weak VMGuestInstallViewController *weakSelf = self;
    _backgroundTask = [[UIApplication sharedApplication]
        beginBackgroundTaskWithName:@"Preparing guest installation"
                  expirationHandler:^{
        VMGuestInstallViewController *self_ = weakSelf;
        [self_->_downloader cancel];
        if (self_ && self_->_backgroundTask != UIBackgroundTaskInvalid) {
            [[UIApplication sharedApplication]
                endBackgroundTask:self_->_backgroundTask];
            self_->_backgroundTask = UIBackgroundTaskInvalid;
        }
    }];
}

- (void)endBackgroundTime {
    if (_backgroundTask == UIBackgroundTaskInvalid) return;
    [[UIApplication sharedApplication] endBackgroundTask:_backgroundTask];
    _backgroundTask = UIBackgroundTaskInvalid;
}

- (void)setFraction:(double)fraction stage:(NSString *)stage {
    NSAssert([NSThread isMainThread], @"guest install UI belongs on main");
    if (_finished) return;
    if (fraction < 0.0) fraction = 0.0;
    if (fraction > 1.0) fraction = 1.0;
    [_progressBar setProgress:(float)fraction animated:YES];
    if (stage.length) _detail.text = stage;
}

- (void)failWithHeadline:(NSString *)headline
             description:(NSString *)description {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_finished) return;
        self->_finished = YES;
        [self endBackgroundTime];
        self->_headline.text = headline.length ? headline : @"Jailbreak failed";
        self->_headline.textColor = [UIColor systemRedColor];
        self->_detail.text = description.length ? description
                                                : @"The guest disk was not changed.";
        self->_progressBar.progressTintColor = [UIColor systemRedColor];
        self.navigationItem.hidesBackButton = NO;
        self.navigationController.interactivePopGestureRecognizer.enabled = YES;
    });
}

- (void)failWithDescription:(NSString *)description {
    [self failWithHeadline:@"Jailbreak failed" description:description];
}

- (void)completeInstallAlreadyPresent:(BOOL)alreadyPresent
                       storageUpgraded:(BOOL)storageUpgraded
               cydiaPrivilegesRepaired:(BOOL)cydiaPrivilegesRepaired
                    cydiaSourcesAdded:(BOOL)cydiaSourcesAdded
                    aptTrustInstalled:(BOOL)aptTrustInstalled
                    aptVerifierStaged:(BOOL)aptVerifierStaged {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_finished) return;
        self->_finished = YES;
        [self endBackgroundTime];
        [self->_progressBar setProgress:1.0f animated:YES];
        self->_headline.text = aptVerifierStaged ? @"Cydia verifier ready"
                              : (cydiaPrivilegesRepaired ? @"Cydia repaired"
                              : (cydiaSourcesAdded ? @"Cydia repositories ready"
                              : (aptTrustInstalled ? @"Cydia trust ready"
                              : (storageUpgraded ? @"Storage upgraded"
                              : (alreadyPresent ? @"Jailbreak already installed"
                                                : @"Jailbreak ready")))));
        NSString *baseDetail = cydiaPrivilegesRepaired && cydiaSourcesAdded
            ? (storageUpgraded
                ? @"Starting iPhone OS with a 2 GiB guest disk. Cydia's executable permissions were repaired and the period-compatible BigBoss repository was added without removing existing data."
                : @"Starting iPhone OS. Cydia's executable permissions were repaired and the period-compatible BigBoss repository was added without removing guest data.")
            : (cydiaSourcesAdded
                ? (storageUpgraded
                    ? @"Starting iPhone OS with a 2 GiB guest disk. The period-compatible BigBoss repository was added and existing Cydia data was preserved."
                    : @"Starting iPhone OS. The period-compatible BigBoss repository was added without replacing existing APT configuration.")
            : (cydiaPrivilegesRepaired
            ? (storageUpgraded
                ? @"Starting iPhone OS with a 2 GiB guest disk. The exact legacy Cydia executable permissions were repaired and existing data was preserved."
                : @"Starting iPhone OS. The exact legacy Cydia executable permissions were repaired; no guest data was removed.")
            : (storageUpgraded
                ? @"Starting iPhone OS with a 2 GiB guest disk. Existing Cydia data was preserved."
                : @"Starting iPhone OS. The first boot finishes package configuration inside the guest.")));
        NSString *trustDetail = aptTrustInstalled
            ? [baseDetail stringByAppendingString:
                @" BigBoss's verified legacy public key was installed; APT signature checks remain enabled."]
            : baseDetail;
        self->_detail.text = aptVerifierStaged
            ? [trustDetail stringByAppendingString:
                @" Signature-verifier support is provisioned; guest dpkg completes it during boot if needed."]
            : trustDetail;
        void (^ready)(void) = [self.readyHandler copy];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                     (int64_t)(0.8 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            if (ready) ready();
        });
    });
}

- (void)startInstall {
    [self beginBackgroundTime];
    _machineDirectory = [[VMInstanceStore sharedStore]
        directoryForInstanceWithID:_instanceID];
    if (!_machineDirectory.length) {
        [self failWithDescription:@"This machine has no safe work directory."];
        return;
    }

    [self setFraction:0.01 stage:@"Checking this machine\u2026"];
    __weak VMGuestInstallViewController *weakSelf = self;
    dispatch_async(_buildQueue, ^{
        VMGuestInstallViewController *self_ = weakSelf;
        if (!self_) return;
        vm_guest_install_build_result_t result;
        char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
        vm_guest_install_build_status_t status =
            vm_guest_install_build_from_directory(
                self_->_machineDirectory.fileSystemRepresentation,
                NULL, VMGuestInstallBuildProgress, (__bridge void *)self_,
                &result, detail, sizeof detail);
        if (status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed) {
            [self_ completeInstallAlreadyPresent:YES
                                 storageUpgraded:result.storage_upgraded
                         cydiaPrivilegesRepaired:
                             result.cydia_privileges_repaired
                              cydiaSourcesAdded:result.cydia_sources_added
                              aptTrustInstalled:result.apt_trust_installed
                              aptVerifierStaged:result.apt_verifier_staged];
            return;
        }
        if (status != VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT) {
            NSString *why = detail[0]
                ? [NSString stringWithUTF8String:detail] : nil;
            NSString *headline =
                status == VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN
                    ? @"Shut down this guest first"
                    : @"Jailbreak failed";
            [self_ failWithHeadline:headline description:why ?:
                [NSString stringWithUTF8String:
                    vm_guest_install_build_status_text(status)]];
            return;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            [self_ setFraction:0.03 stage:@"Preparing package download\u2026"];
            self_->_downloader = [[VMGuestPackageDownloader alloc] init];
            [self_->_downloader startWithProgress:
                ^(uint64_t completed, uint64_t total, NSString *stage) {
                    double part = total ? (double)completed / (double)total : 0.0;
                    [self_ setFraction:0.03 + 0.27 * part stage:stage];
                }
                completion:^(NSURL *directory, NSError *error) {
                    if (error) {
                        [self_ failWithDescription:error.localizedDescription];
                        return;
                    }
                    [self_ runBuilderWithPackageDirectory:directory];
                }];
        });
    });
}

- (void)runBuilderWithPackageDirectory:(NSURL *)packageDirectory {
    [self setFraction:0.31 stage:@"Verifying packages\u2026"];
    __weak VMGuestInstallViewController *weakSelf = self;
    dispatch_async(_buildQueue, ^{
        VMGuestInstallViewController *self_ = weakSelf;
        if (!self_) return;
        vm_guest_install_build_result_t result;
        char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
        vm_guest_install_build_status_t status =
            vm_guest_install_build_from_directory(
                self_->_machineDirectory.fileSystemRepresentation,
                packageDirectory.fileSystemRepresentation,
                VMGuestInstallBuildProgress, (__bridge void *)self_,
                &result, detail, sizeof detail);
        if (status != VM_GUEST_INSTALL_BUILD_OK ||
            !result.transaction.committed) {
            NSString *why = detail[0]
                ? [NSString stringWithUTF8String:detail] : nil;
            NSString *headline =
                status == VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN
                    ? @"Shut down this guest first"
                    : @"Jailbreak failed";
            [self_ failWithHeadline:headline description:why ?:
                [NSString stringWithUTF8String:
                    vm_guest_install_build_status_text(status)]];
            return;
        }
        [self_ completeInstallAlreadyPresent:result.already_installed
                             storageUpgraded:result.storage_upgraded
                     cydiaPrivilegesRepaired:
                         result.cydia_privileges_repaired
                          cydiaSourcesAdded:result.cydia_sources_added
                          aptTrustInstalled:result.apt_trust_installed
                          aptVerifierStaged:result.apt_verifier_staged];
    });
}

- (void)receiveBuildPhase:(vm_guest_install_build_phase_t)phase
                 completed:(uint64_t)completed total:(uint64_t)total {
    double within = total ? (double)completed / (double)total : 0.0;
    double fraction = 0.31;
    switch (phase) {
        case VM_GUEST_INSTALL_BUILD_RECOVERING: fraction = 0.31 + 0.01 * within; break;
        case VM_GUEST_INSTALL_BUILD_PLANNING:   fraction = 0.32 + 0.05 * within; break;
        case VM_GUEST_INSTALL_BUILD_STAGING:    fraction = 0.37 + 0.03 * within; break;
        case VM_GUEST_INSTALL_BUILD_COPYING:    fraction = 0.40 + 0.54 * within; break;
        case VM_GUEST_INSTALL_BUILD_PUBLISHING: fraction = 0.94 + 0.05 * within; break;
        case VM_GUEST_INSTALL_BUILD_COMPLETE:   fraction = 1.0; break;
    }
    NSString *stage = [NSString stringWithUTF8String:
        vm_guest_install_build_phase_text(phase)] ?: @"Preparing guest disk";
    dispatch_async(dispatch_get_main_queue(), ^{
        [self setFraction:fraction stage:stage];
    });
}

@end
