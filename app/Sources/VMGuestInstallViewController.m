//
//  S5LBox -- VMGuestInstallViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMGuestInstallViewController.h"
#import "VMStorageLease.h"
#import "VMPackageBridge.h"

#import "VMGuestInstallBuild.h"
#import "VMGuestPackageDownloader.h"
#import "VMInstanceStore.h"
#import "VMResumeCheckpoint.h"
#import "VMSettings.h"
#include <string.h>

@interface VMGuestInstallViewController ()
- (void)completeIfVisible:(NSNotification * _Nullable)notification;
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
    UILabel *_headline;
    UILabel *_detail;
    UILabel *_hint;
    UIActivityIndicatorView *_spinner;
    UIButton *_retry;
    UIProgressView *_progressBar;
    VMGuestPackageDownloader *_downloader;
    dispatch_queue_t _buildQueue;
    UIBackgroundTaskIdentifier _backgroundTask;
    BOOL _completionPending;
    BOOL _started;
    BOOL _finished;
    BOOL _packageFlow, _jailbreakNeeded, _openForShutdown, _keepsAwake, _previousIdle;
    BOOL _includePackages;
    BOOL _includeAppSync;
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
    _packageFlow = self.packageManagerOnly;
    self.title = _packageFlow ? @"Set Up Packages" : @"Jailbreak";
    self.view.backgroundColor = UIColor.systemGroupedBackgroundColor;
    self.view.accessibilityIdentifier = @"s5lbox.guest-install.screen";
    self.navigationItem.hidesBackButton = YES;
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;

    UIScrollView *scroll = [UIScrollView new];
    scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    _retry = [UIButton buttonWithType:UIButtonTypeSystem];
    _retry.translatesAutoresizingMaskIntoConstraints = NO;
    _retry.backgroundColor = UIColor.systemBlueColor;
    _retry.layer.cornerRadius = 14;
    [_retry setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    _retry.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _retry.titleLabel.adjustsFontForContentSizeCategory = YES;
    _retry.titleLabel.numberOfLines = 0;
    _retry.contentEdgeInsets = UIEdgeInsetsMake(14, 18, 14, 18);
    _retry.accessibilityIdentifier = @"s5lbox.guest-install.retry";
    [_retry addTarget:self action:@selector(retryTapped) forControlEvents:UIControlEventTouchUpInside];
    _retry.hidden = YES;
    [self.view addSubview:_retry];

    UIView *canvas = [UIView new];
    canvas.translatesAutoresizingMaskIntoConstraints = NO;
    [scroll addSubview:canvas];
    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 24;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [canvas addSubview:stack];
    UIImageView *symbol = [[UIImageView alloc] initWithImage:
        [UIImage systemImageNamed:_packageFlow ? @"shippingbox" : @"lock.open"]];
    symbol.contentMode = UIViewContentModeScaleAspectFit;
    symbol.tintColor = UIColor.systemBlueColor;
    symbol.isAccessibilityElement = NO;
    [symbol.heightAnchor constraintEqualToConstant:56].active = YES;
    [stack addArrangedSubview:symbol];
    UILabel *machine = [UILabel new];
    machine.text = _machineName ?: @"iPhone OS";
    machine.font = [UIFont preferredFontForTextStyle:UIFontTextStyleTitle2];
    machine.adjustsFontForContentSizeCategory = YES;
    machine.textAlignment = NSTextAlignmentCenter;
    machine.numberOfLines = 0;
    machine.lineBreakMode = NSLineBreakByWordWrapping;
    [stack addArrangedSubview:machine];

    UIView *card = [UIView new];
    card.backgroundColor = UIColor.secondarySystemGroupedBackgroundColor;
    card.layer.cornerRadius = 18;
    UIStackView *content = [UIStackView new];
    content.axis = UILayoutConstraintAxisVertical;
    content.spacing = 12;
    content.translatesAutoresizingMaskIntoConstraints = NO;
    [card addSubview:content];
    _headline = [UILabel new];
    _headline.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    _headline.textAlignment = NSTextAlignmentCenter;
    _headline.adjustsFontForContentSizeCategory = YES;
    _headline.numberOfLines = 0;
    _headline.lineBreakMode = NSLineBreakByWordWrapping;
    _headline.accessibilityIdentifier = @"s5lbox.guest-install.headline";
    _headline.text = @"Checking machine";
    [content addArrangedSubview:_headline];
    _detail = [UILabel new];
    _detail.font = [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    _detail.adjustsFontForContentSizeCategory = YES;
    _detail.textColor = UIColor.secondaryLabelColor;
    _detail.numberOfLines = 0;
    _detail.lineBreakMode = NSLineBreakByWordWrapping;
    _detail.accessibilityIdentifier = @"s5lbox.guest-install.detail";
    [content addArrangedSubview:_detail];
    _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    _spinner.hidesWhenStopped = YES;
    [content addArrangedSubview:_spinner];
    _progressBar = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _progressBar.accessibilityIdentifier = @"s5lbox.guest-install.progress";
    _progressBar.accessibilityLabel = @"Current step progress";
    [content addArrangedSubview:_progressBar];
    [stack addArrangedSubview:card];
    _hint = [UILabel new];
    _hint.text = @"Keep S5LBox open.";
    _hint.textColor = UIColor.secondaryLabelColor;
    _hint.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    _hint.adjustsFontForContentSizeCategory = YES;
    _hint.textAlignment = NSTextAlignmentCenter;
    _hint.numberOfLines = 0;
    [stack addArrangedSubview:_hint];

    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    NSLayoutConstraint *height = [canvas.heightAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.heightAnchor];
    height.priority = UILayoutPriorityDefaultLow;
    NSLayoutConstraint *width = [stack.widthAnchor constraintEqualToAnchor:safe.widthAnchor constant:-48];
    // Keep a stable card width as short progress labels replace longer errors.
    // Text/button intrinsic widths must not win over the page margins.
    width.priority = UILayoutPriorityRequired - 1;
    NSLayoutConstraint *buttonWidth = [_retry.widthAnchor constraintEqualToAnchor:stack.widthAnchor];
    buttonWidth.active = YES;
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:safe.topAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor],
        [scroll.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:_retry.topAnchor constant:-12],
        [_retry.bottomAnchor constraintEqualToAnchor:safe.bottomAnchor constant:-16],
        [_retry.centerXAnchor constraintEqualToAnchor:safe.centerXAnchor],
        [_retry.heightAnchor constraintGreaterThanOrEqualToConstant:54],
        [canvas.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor],
        [canvas.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor],
        [canvas.leadingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.leadingAnchor],
        [canvas.trailingAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.trailingAnchor],
        [canvas.widthAnchor constraintEqualToAnchor:scroll.frameLayoutGuide.widthAnchor],
        [canvas.heightAnchor constraintGreaterThanOrEqualToAnchor:scroll.frameLayoutGuide.heightAnchor], height,
        [stack.centerXAnchor constraintEqualToAnchor:canvas.centerXAnchor],
        [stack.centerYAnchor constraintEqualToAnchor:canvas.centerYAnchor],
        [stack.topAnchor constraintGreaterThanOrEqualToAnchor:canvas.topAnchor constant:24],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:canvas.bottomAnchor constant:-24],
        [stack.widthAnchor constraintLessThanOrEqualToAnchor:safe.widthAnchor constant:-48],
        [stack.widthAnchor constraintLessThanOrEqualToConstant:520], width,
        [content.topAnchor constraintEqualToAnchor:card.topAnchor constant:20],
        [content.bottomAnchor constraintEqualToAnchor:card.bottomAnchor constant:-20],
        [content.leadingAnchor constraintEqualToAnchor:card.leadingAnchor constant:20],
        [content.trailingAnchor constraintEqualToAnchor:card.trailingAnchor constant:-20]
    ]];
    for (UILabel *label in @[machine, _headline, _detail, _hint])
        [label setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisVertical];
    _buildQueue = dispatch_queue_create("com.j0shua.S5LBox.GuestInstallBuild", DISPATCH_QUEUE_SERIAL);
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(completeIfVisible:)
        name:UIApplicationDidBecomeActiveNotification object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    self.navigationController.interactivePopGestureRecognizer.enabled = _finished;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!_started) {
        _started = YES;
        [self startInstall];
    }
    [self completeIfVisible:nil];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    /* This recognizer belongs to the navigation controller, not this screen.
     * Never leave the rest of the app without swipe-back after our guarded
     * install flow is popped programmatically. */
    self.navigationController.interactivePopGestureRecognizer.enabled = YES;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    [_downloader cancel];
    if (_keepsAwake) UIApplication.sharedApplication.idleTimerDisabled = _previousIdle;
    if (_backgroundTask != UIBackgroundTaskInvalid)
        [[UIApplication sharedApplication] endBackgroundTask:_backgroundTask];
}

- (void)beginBackgroundTime {
    if (!_keepsAwake) {
        _previousIdle = UIApplication.sharedApplication.idleTimerDisabled;
        UIApplication.sharedApplication.idleTimerDisabled = YES;
        _keepsAwake = YES;
    }
    if (_backgroundTask != UIBackgroundTaskInvalid) return;
    __weak VMGuestInstallViewController *weakSelf = self;
    _backgroundTask = [[UIApplication sharedApplication]
        beginBackgroundTaskWithName:@"Preparing guest installation"
                  expirationHandler:^{
        VMGuestInstallViewController *self_ = weakSelf;
        if (!self_) return;
        [self_->_downloader cancel];
        if (self_ && self_->_backgroundTask != UIBackgroundTaskInvalid) {
            [[UIApplication sharedApplication]
                endBackgroundTask:self_->_backgroundTask];
            self_->_backgroundTask = UIBackgroundTaskInvalid;
        }
    }];
}

- (void)endBackgroundTime {
    if (_keepsAwake) UIApplication.sharedApplication.idleTimerDisabled = _previousIdle;
    _keepsAwake = NO;
    if (_backgroundTask == UIBackgroundTaskInvalid) return;
    [[UIApplication sharedApplication] endBackgroundTask:_backgroundTask];
    _backgroundTask = UIBackgroundTaskInvalid;
}

- (void)setFraction:(double)fraction stage:(NSString *)stage {
    NSAssert([NSThread isMainThread], @"guest install UI belongs on main");
    if (_finished) return;
    if (fraction > 1.0) fraction = 1.0;
    _progressBar.hidden = fraction < 0;
    if (fraction < 0) [_spinner startAnimating]; else [_spinner stopAnimating];
    if (fraction >= 0) [_progressBar setProgress:(float)fraction animated:NO];
    if (stage.length) _headline.text = stage;
    _detail.textAlignment = NSTextAlignmentCenter;
    _detail.text = fraction >= 0 ? [NSString stringWithFormat:@"%.0f%%", fraction * 100] : @"";
    _detail.hidden = !_detail.text.length;
}

- (void)failWithHeadline:(NSString *)headline
             description:(NSString *)description {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_finished) return;
        self->_finished = YES;
        [self endBackgroundTime];
        self->_headline.text = headline.length ? headline : @"Jailbreak failed";
        self->_headline.textColor = [UIColor systemRedColor];
        self->_detail.hidden = NO;
        self->_detail.textAlignment = NSTextAlignmentLeft;
        self->_detail.text = description.length ? description
                                                : @"Try again, or return to Machines.";
        self->_progressBar.hidden = YES;
        [self->_spinner stopAnimating];
        self->_hint.hidden = YES;
        self->_retry.hidden = NO;
        [self->_retry setTitle:self->_openForShutdown ? @"Open Machine" : self->_jailbreakNeeded ? @"Jailbreak & Set Up" : @"Try Again" forState:UIControlStateNormal];
        self.navigationItem.hidesBackButton = NO;
        self.navigationController.interactivePopGestureRecognizer.enabled = YES;
    });
}

- (void)failWithDescription:(NSString *)description {
    [self failWithHeadline:_packageFlow ? @"Couldn’t set up Packages" : @"Jailbreak failed" description:description];
}

- (void)retryTapped {
    if (!_finished) return;
    if (_openForShutdown) {
        _openPackagesWhenReady = NO;
        if (self.readyHandler) self.readyHandler();
        return;
    }
    if (!_jailbreakNeeded) { [self startInstall]; return; }
    UIAlertController *confirm = [UIAlertController alertControllerWithTitle:@"Jailbreak this machine?"
        message:[VMSettings sharedSettings].appSyncWithJailbreak
            ? @"Install Cydia, AppSync and set up Packages. The saved running state will be discarded."
            : @"Install Cydia and set up Packages. The saved running state will be discarded."
        preferredStyle:UIAlertControllerStyleAlert];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [confirm addAction:[UIAlertAction actionWithTitle:@"Jailbreak & Set Up" style:UIAlertActionStyleDestructive
        handler:^(__unused UIAlertAction *action) {
            self.packageManagerOnly = NO;
            self->_jailbreakNeeded = NO;
            [self startInstall];
        }]];
    [self presentViewController:confirm animated:YES completion:nil];
}

- (void)failBuildStatus:(vm_guest_install_build_status_t)status
                result:(const vm_guest_install_build_result_t *)result
                detail:(const char *)detail {
    // A force-off can leave the volume header behind its catalog writes.
    // Keep the disk validator strict; let iPhone OS finish and flush the disk.
    BOOL interruptedCatalog = status == VM_GUEST_INSTALL_BUILD_ERR_ROOTFS &&
        result->forced_power_off && result->rootfs.status == ROOTFS_WORK_PROVISION_CATALOG_CORRUPT &&
        detail && strstr(detail, "from nextCatalogID is already in use") != NULL;
    if (status == VM_GUEST_INSTALL_BUILD_ERR_STORAGE_NOT_CLEAN || interruptedCatalog) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_openForShutdown = YES;
            [self failWithHeadline:@"Shut down iPhone OS first"
                description:@"Open this machine and use Back \u2192 Shut down. Then try setup again."];
        });
        return;
    }
    NSString *why = detail && detail[0] ? [NSString stringWithUTF8String:detail] : nil;
    [self failWithDescription:why ?: (status == VM_GUEST_INSTALL_BUILD_OK ? @"Guest installation did not finish." :
        [NSString stringWithUTF8String:vm_guest_install_build_status_text(status)])];
}

- (BOOL)preparePackages:(NSError **)error {
    // Explicit package setup always wins; an ordinary jailbreak may opt out.
    if (!_includePackages) return YES;
    __weak VMGuestInstallViewController *weakSelf = self;
    return [VMPackageBridge prepareStoppedMachine:_machineDirectory instanceID:_instanceID
        progress:^(NSString *stage, double fraction) {
            dispatch_async(dispatch_get_main_queue(), ^{ [weakSelf setFraction:fraction stage:stage]; });
        } error:error];
}

- (void)completeInstallAlreadyPresent:(BOOL)alreadyPresent
                       storageUpgraded:(BOOL)storageUpgraded
               cydiaPrivilegesRepaired:(BOOL)cydiaPrivilegesRepaired
                    cydiaSourcesAdded:(BOOL)cydiaSourcesAdded
                    aptTrustInstalled:(BOOL)aptTrustInstalled
                    aptVerifierStaged:(BOOL)aptVerifierStaged
                      cydiaCacheStaged:(BOOL)cydiaCacheStaged {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_finished) return;
        self->_finished = YES;
        self->_jailbreakNotice = alreadyPresent ? nil : self->_includePackages
            ? @"First-boot setup is still running. The guest will respring automatically. After the respring, you may need to reboot the guest to use Packages."
            : @"First-boot setup is still running. Cydia may not be available yet. The guest will respring automatically.";
        [self endBackgroundTime];
        [self->_spinner stopAnimating];
        self->_hint.hidden = YES;
        [self->_progressBar setProgress:1.0f animated:YES];
        BOOL updated = cydiaCacheStaged || aptVerifierStaged || cydiaPrivilegesRepaired ||
            cydiaSourcesAdded || aptTrustInstalled;
        self->_headline.text = !alreadyPresent ? @"Finishing jailbreak"
            : updated ? @"Finishing Cydia setup"
            : storageUpgraded ? @"Storage expanded" : @"Jailbreak already installed";
        self->_detail.hidden = NO;
        self->_detail.textAlignment = NSTextAlignmentLeft;
        self->_detail.text = @"Ready to start iPhone OS.";
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,
                                     (int64_t)(0.8 * NSEC_PER_SEC)),
                       dispatch_get_main_queue(), ^{
            self->_completionPending = YES;
            [self completeIfVisible:nil];
        });
    });
}

- (void)completeIfVisible:(NSNotification *)notification {
    (void)notification;
    if (!_completionPending || !self.view.window || self.presentedViewController ||
        UIApplication.sharedApplication.applicationState != UIApplicationStateActive ||
        self.navigationController.topViewController != self) return;
    // A jailbreak returns straight to the guest. Only an explicit Packages
    // setup request reopens Packages once the guest is running.
    _completionPending = NO;
    _openPackagesWhenReady = _packageFlow;
    void (^ready)(void) = [self.readyHandler copy];
    if (ready) ready();
}

- (void)startInstall {
    _finished = NO;
    // Capture once on the main thread; retries get a fresh choice, but an
    // in-flight disk transaction never changes policy halfway through.
    _includePackages = _packageFlow || [VMSettings sharedSettings].packagesWithJailbreak;
    _includeAppSync = !self.packageManagerOnly && [VMSettings sharedSettings].appSyncWithJailbreak;
    _completionPending = NO;
    _openPackagesWhenReady = NO;
    _jailbreakNotice = nil;
    _openForShutdown = NO;
    _retry.hidden = YES;
    _hint.hidden = NO;
    _headline.textColor = UIColor.labelColor;
    self.navigationItem.hidesBackButton = YES;
    self.navigationController.interactivePopGestureRecognizer.enabled = NO;
    [self beginBackgroundTime];
    _machineDirectory = [[VMInstanceStore sharedStore]
        directoryForInstanceWithID:_instanceID];
    if (!_machineDirectory.length) {
        [self failWithDescription:@"This machine has no safe work directory."];
        return;
    }

    [self setFraction:-1 stage:@"Checking machine"];
    __weak VMGuestInstallViewController *weakSelf = self;
    dispatch_async(_buildQueue, ^{
        VMGuestInstallViewController *self_ = weakSelf;
        if (!self_) return;
#if S5LBOX_TOUCHHLE
        NSError *leaseError=nil;
        __attribute__((objc_precise_lifetime)) VMStorageLease *lease=[VMStorageLease acquireDirectory:self_->_machineDirectory error:&leaseError];
        if (!lease) { [self_ failWithDescription:leaseError.localizedDescription]; return; }
#endif
        if (self_.packageManagerOnly) {
            NSError *error = nil; char recovery[320] = {0};
            const char *work = self_->_machineDirectory.fileSystemRepresentation;
            if (vm_guest_maintenance_recover(work,NULL,NULL,NULL,recovery,sizeof recovery) != VM_GUEST_INSTALL_OK) {
                [self_ failWithHeadline:@"Package setup failed" description:[NSString stringWithUTF8String:recovery]]; return;
            }
            vm_guest_install_probe_t probe = vm_guest_install_probe(work, NULL, recovery, sizeof recovery);
            if (probe == VM_GUEST_INSTALL_PROBE_ABSENT) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    self_->_jailbreakNeeded = YES;
                    [self_ failWithHeadline:@"Jailbreak required" description:@"Jailbreak this machine to install tweaks with Packages."];
                });
                return;
            }
            if (probe != VM_GUEST_INSTALL_PROBE_VALID || !vm_resume_checkpoint_disarm(work,recovery,sizeof recovery)) {
                [self_ failWithDescription:[NSString stringWithUTF8String:recovery]]; return;
            }
            if (![self_ preparePackages:&error]) {
                [self_ failWithHeadline:@"Package setup failed" description:error.localizedDescription]; return;
            }
            dispatch_async(dispatch_get_main_queue(),^{
                self_->_openPackagesWhenReady = YES;
                [self_ endBackgroundTime];
                [self_ setFraction:1 stage:@"Starting iPhone OS"];
                self_->_finished = YES;
                self_->_hint.hidden = YES;
                self_->_completionPending = YES;
                [self_ completeIfVisible:nil];
            });
            return;
        }
        vm_guest_install_build_result_t result;
        char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
        vm_guest_install_build_status_t status =
            vm_guest_install_build_after_force_off_with_appsync(
                self_->_machineDirectory.fileSystemRepresentation,
                NULL, self_->_includeAppSync, VMGuestInstallBuildProgress, (__bridge void *)self_,
                &result, detail, sizeof detail);
        if (status == VM_GUEST_INSTALL_BUILD_OK && result.already_installed) {
            NSError *bridgeError = nil;
            if (![self_ preparePackages:&bridgeError]) {
                [self_ failWithDescription:bridgeError.localizedDescription]; return;
            }
            [self_ completeInstallAlreadyPresent:YES
                                 storageUpgraded:result.storage_upgraded
                         cydiaPrivilegesRepaired:
                             result.cydia_privileges_repaired
                              cydiaSourcesAdded:result.cydia_sources_added
                              aptTrustInstalled:result.apt_trust_installed
                              aptVerifierStaged:result.apt_verifier_staged
                                cydiaCacheStaged:result.cydia_cache_staged];
            return;
        }
        if (status != VM_GUEST_INSTALL_BUILD_ERR_ARGUMENT) {
            [self_ failBuildStatus:status result:&result detail:detail];
            return;
        }

        dispatch_async(dispatch_get_main_queue(), ^{
            [self_ setFraction:-1 stage:@"Preparing package download\u2026"];
            self_->_downloader = [[VMGuestPackageDownloader alloc] init];
            [self_->_downloader startIncludingAppSync:self_->_includeAppSync progress:
                ^(uint64_t completed, uint64_t total, NSString *stage) {
                    double part = total ? (double)completed / (double)total : 0.0;
                    [self_ setFraction:total ? part : -1 stage:stage];
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
    [self setFraction:-1 stage:@"Verifying packages\u2026"];
    __weak VMGuestInstallViewController *weakSelf = self;
    dispatch_async(_buildQueue, ^{
        VMGuestInstallViewController *self_ = weakSelf;
        if (!self_) return;
#if S5LBOX_TOUCHHLE
        NSError *leaseError=nil;
        __attribute__((objc_precise_lifetime)) VMStorageLease *lease=[VMStorageLease acquireDirectory:self_->_machineDirectory error:&leaseError];
        if (!lease) { [self_ failWithDescription:leaseError.localizedDescription]; return; }
#endif
        vm_guest_install_build_result_t result;
        char detail[VM_GUEST_INSTALL_BUILD_DETAIL_CAPACITY];
        vm_guest_install_build_status_t status =
            vm_guest_install_build_after_force_off_with_appsync(
                self_->_machineDirectory.fileSystemRepresentation,
                packageDirectory.fileSystemRepresentation,
                self_->_includeAppSync,
                VMGuestInstallBuildProgress, (__bridge void *)self_,
                &result, detail, sizeof detail);
        if (status != VM_GUEST_INSTALL_BUILD_OK ||
            !result.transaction.committed) {
            [self_ failBuildStatus:status result:&result detail:detail];
            return;
        }
        NSError *bridgeError = nil;
        if (![self_ preparePackages:&bridgeError]) {
            [self_ failWithDescription:bridgeError.localizedDescription]; return;
        }
        [self_ completeInstallAlreadyPresent:result.already_installed
                             storageUpgraded:result.storage_upgraded
                     cydiaPrivilegesRepaired:
                         result.cydia_privileges_repaired
                          cydiaSourcesAdded:result.cydia_sources_added
                          aptTrustInstalled:result.apt_trust_installed
                          aptVerifierStaged:result.apt_verifier_staged
                            cydiaCacheStaged:result.cydia_cache_staged];
    });
}

- (void)receiveBuildPhase:(vm_guest_install_build_phase_t)phase
                 completed:(uint64_t)completed total:(uint64_t)total {
    double fraction = total ? (double)completed / (double)total : -1;
    if (phase == VM_GUEST_INSTALL_BUILD_COMPLETE) fraction = 1;
    NSString *stage = [NSString stringWithUTF8String:
        vm_guest_install_build_phase_text(phase)] ?: @"Preparing guest disk";
    dispatch_async(dispatch_get_main_queue(), ^{
        [self setFraction:fraction stage:stage];
    });
}

@end
