// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMIPAInstallViewController.h"
#import "VMIPAPackage.h"
#import "VMInstanceStore.h"
#import "VMUSBInstaller.h"
#import "VMTouchHLEOfflineFiles.h"

@interface VMIPAInstallViewController ()
- (void)setBusy:(BOOL)busy message:(NSString *)message;
- (void)installPackage;
- (void)confirmInstall;
- (void)installLive;
@end

@implementation VMIPAInstallViewController {
    NSString *_identifier;
    NSString *_machineName;
    NSString *_filename;
    NSString *_status;
    VMIPAPackage *_package;
    NSURL *_liveURL;
    VMUSBInstaller *_usbInstaller;
    BOOL _busy, _installOnReturn, _installed, _wasIdleDisabled;
    UIBackgroundTaskIdentifier _backgroundTask;
}

- (instancetype)initWithInstanceID:(NSString *)identifier machineName:(NSString *)name {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (!self) return nil;
    _identifier = [identifier copy];
    _machineName = [name copy];
    _backgroundTask = UIBackgroundTaskInvalid;
    self.title = @"Install IPA";
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 70;
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (_installOnReturn) {
        _installOnReturn = NO;
        [self installPackage];
    }
}

- (void)setBusy:(BOOL)busy message:(NSString *)message {
    if (busy != _busy) {
        if (busy) {
            _wasIdleDisabled = UIApplication.sharedApplication.idleTimerDisabled;
            UIApplication.sharedApplication.idleTimerDisabled = YES;
        } else {
            UIApplication.sharedApplication.idleTimerDisabled = _wasIdleDisabled;
        }
    }
    _busy = busy;
    _status = [message copy];
    self.navigationItem.hidesBackButton = busy;
    self.navigationController.interactivePopGestureRecognizer.enabled = !busy;
    SEL contentPop = NSSelectorFromString(@"interactiveContentPopGestureRecognizer");
    if ([self.navigationController respondsToSelector:contentPop]) {
        UIGestureRecognizer *gesture = [self.navigationController valueForKey:NSStringFromSelector(contentPop)];
        gesture.enabled = !busy;
    }
    // Also gates iOS 26's content-wide pop gesture while a disk is in use.
    self.navigationController.view.userInteractionEnabled = !busy || (_liveTransport && _usbInstaller);
    if (_liveTransport) self.navigationController.modalInPresentation = busy;
    self.navigationItem.rightBarButtonItem = busy && _usbInstaller ? [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemCancel target:self action:@selector(cancelLive)] : nil;
    [self.tableView reloadData];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { return 3; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return section == 2 && !_status.length ? 0 : 1;
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    return section == 0 ? _machineName : section == 2 && _status.length ? @"Status" : nil;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section != 0) return nil;
    if (_liveTransport) return @"An existing version of this app may be replaced.";
    if (_offlineTouchHLE) return @"Updates keep this profile's app data.";
    return @"Requires an unencrypted ARMv6 app for iPhone OS 3.1.3 or earlier. Updates require a running machine.";
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.numberOfLines = cell.detailTextLabel.numberOfLines = 0;
    cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    if (path.section == 0) {
        cell.textLabel.text = _package.displayName ?: _filename.stringByDeletingPathExtension;
        cell.detailTextLabel.text = _package ? [NSString stringWithFormat:@"%@\n%@", _filename, _package.bundleIdentifier] : _filename;
        cell.accessibilityIdentifier = @"s5lbox.ipa.package";
    } else if (path.section == 1) {
        BOOL enabled = !_busy && (_installed || _package != nil);
        cell.textLabel.text = _installed ? (_offlineTouchHLE ? @"Return to Apps" : _liveTransport ? @"Return to machine" : @"Start machine") : @"Install";
        cell.accessibilityIdentifier = @"s5lbox.ipa.install";
        cell.textLabel.textColor = enabled ? UIColor.systemBlueColor : UIColor.tertiaryLabelColor;
        cell.userInteractionEnabled = enabled;
    } else {
        cell.textLabel.text = _status;
        cell.accessibilityIdentifier = @"s5lbox.ipa.status";
        if (_busy) {
            UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
            [spinner startAnimating];
            cell.accessoryView = spinner;
        }
    }
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (_busy) return;
    if (path.section == 1) {
        if (_installed) { if (self.readyHandler) self.readyHandler(); }
        else if (_package) [self confirmInstall];
    }
}

- (void)inspectURL:(NSURL *)url {
    if (!url || _busy) return;
    // Hold security scope immediately, before scheduling provider I/O off-main.
    BOOL scoped = [url startAccessingSecurityScopedResource];
    _filename = url.lastPathComponent;
    _liveURL = _liveTransport ? url : nil;
    _package = nil;
    _installed = NO;
    [self setBusy:YES message:@"Opening and checking the selected file…"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        VMIPAPackage *package = [[VMIPAPackage alloc] initWithURL:url error:&error];
        if (scoped) [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_package = package;
            [self setBusy:NO message:package ? @"Ready to install."
                : error.localizedDescription ?: @"The selected file could not be read."];
        });
    });
}
- (void)confirmInstall {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:
        [NSString stringWithFormat:@"Install %@?", _package.displayName]
        message:(_liveTransport || _offlineTouchHLE) ? [NSString stringWithFormat:@"Install in %@?", _machineName]
            : [NSString stringWithFormat:@"%@ will shut down first. Existing apps will not be replaced.", _machineName]
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Install" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        if (self->_liveTransport) { [self installLive]; return; }
        if (self->_offlineTouchHLE) { [self installPackage]; return; }
        if (!self.prepareHandler) return;
        self->_installOnReturn = YES;
        self.prepareHandler();
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)cancelLive {
    _usbInstaller.canceled = YES;
    self.navigationItem.rightBarButtonItem.enabled = NO;
    _status = @"Canceling… If installation has reached the guest, it may still finish.";
    [self.tableView reloadData];
}
- (void)installLive {
    if (_busy || !_liveTransport || !_liveURL || !_package) return;
    _usbInstaller = [[VMUSBInstaller alloc] initWithTransport:_liveTransport instanceID:_identifier];
    [self setBusy:YES message:@"Connecting to the guest…"];
    __weak VMIPAInstallViewController *weakSelf = self;
    _usbInstaller.progress = ^(NSString *message, double fraction) {
        dispatch_async(dispatch_get_main_queue(), ^{
            VMIPAInstallViewController *screen = weakSelf;
            if (!screen || !screen->_busy || screen->_usbInstaller.canceled) return;
            screen->_status = fraction >= 0 && fraction < 1 ?
                [NSString stringWithFormat:@"%@ %.0f%%", message, fraction * 100] : message;
            [screen.tableView reloadData];
        });
    };
    UIApplication *app = UIApplication.sharedApplication;
    _backgroundTask = [app beginBackgroundTaskWithExpirationHandler:^{
        self->_usbInstaller.canceled = YES;
        if (self->_backgroundTask != UIBackgroundTaskInvalid) {
            [app endBackgroundTask:self->_backgroundTask]; self->_backgroundTask = UIBackgroundTaskInvalid;
        }
    }];
    VMUSBInstaller *installer = _usbInstaller;
    NSURL *url = _liveURL;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        BOOL result = [installer installURL:url error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_installed = result;
            if (result) self->_package = nil; // release the inspection's extracted cache
            [self setBusy:NO message:result ? @"Installed. Open the app from the Home screen."
                : error.localizedDescription ?: @"Installation did not finish."];
            self->_usbInstaller = nil;
            if (self->_backgroundTask != UIBackgroundTaskInvalid) {
                [app endBackgroundTask:self->_backgroundTask]; self->_backgroundTask = UIBackgroundTaskInvalid;
            }
        });
    });
}
- (void)installPackage {
    if (!_package || _busy) return;
    [self setBusy:YES message:@"Installing…"];
    UIApplication *app = UIApplication.sharedApplication;
    _backgroundTask = [app beginBackgroundTaskWithExpirationHandler:^{
        if (self->_backgroundTask != UIBackgroundTaskInvalid) {
            [app endBackgroundTask:self->_backgroundTask];
            self->_backgroundTask = UIBackgroundTaskInvalid;
        }
    }];
    NSString *directory = [[VMInstanceStore sharedStore] directoryForInstanceWithID:_identifier];
    VMIPAPackage *package = _package;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        BOOL installed=NO;
#if S5LBOX_TOUCHHLE
        if (self->_offlineTouchHLE) {
            VMTouchHLEOfflineFiles *files=[[VMTouchHLEOfflineFiles alloc] initWithInstanceID:self->_identifier error:&error];
            installed=[files installPackage:package error:&error]; [files close];
        } else
#endif
        installed=[package installInDirectory:directory error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_installed = installed;
            if (installed) self->_package = nil;
            [self setBusy:NO message:installed
                ? (self->_offlineTouchHLE ? @"Installed. Open it from Apps." : @"App added to the guest disk. Start the machine to refresh its Home screen icon. Compatibility checks do not guarantee the app will run.")
                : error.localizedDescription ?: @"Installation did not finish."];
            if (self->_backgroundTask != UIBackgroundTaskInvalid) {
                [app endBackgroundTask:self->_backgroundTask];
                self->_backgroundTask = UIBackgroundTaskInvalid;
            }
        });
    });
}
@end
