// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMIPAInstallViewController.h"
#import "VMIPAPackage.h"
#import "VMInstanceStore.h"

@interface VMIPAInstallViewController () <UIDocumentPickerDelegate>
- (void)setBusy:(BOOL)busy message:(NSString *)message;
- (void)installPackage;
- (void)chooseFile;
- (void)confirmInstall;
@end

@implementation VMIPAInstallViewController {
    NSString *_identifier;
    NSString *_machineName;
    NSString *_filename;
    NSString *_status;
    VMIPAPackage *_package;
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
    return @"For jailbroken iPhone OS 3.1.3 guests. Choose an unencrypted, ARMv6-compatible IPA. Apps are added to /Applications; App Store-style sandbox installation and replacing existing apps are not supported.";
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
        cell.textLabel.text = _package.displayName ?: @"Choose IPA File…";
        cell.detailTextLabel.text = _package ? [NSString stringWithFormat:@"%@\n%@", _filename, _package.bundleIdentifier] : _filename;
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.accessibilityIdentifier = @"s5lbox.ipa.choose";
        cell.textLabel.textColor = _busy ? UIColor.secondaryLabelColor : UIColor.systemBlueColor;
        cell.userInteractionEnabled = !_busy;
    } else if (path.section == 1) {
        BOOL enabled = !_busy && (_installed || _package != nil);
        cell.textLabel.text = _installed ? @"Start Machine" : @"Install in Guest";
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
    if (path.section == 0) [self chooseFile];
    else if (path.section == 1) {
        if (_installed) { if (self.readyHandler) self.readyHandler(); }
        else if (_package) [self confirmInstall];
    }
}

- (void)chooseFile {
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initWithDocumentTypes:@[@"public.data"] inMode:UIDocumentPickerModeOpen];
    picker.delegate = self;
    picker.allowsMultipleSelection = NO;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentAtURL:(NSURL *)url {
    [self documentPicker:controller didPickDocumentsAtURLs:@[url]];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject;
    if (!url || _busy) return;
    // Hold security scope immediately, before scheduling provider I/O off-main.
    BOOL scoped = [url startAccessingSecurityScopedResource];
    _filename = url.lastPathComponent;
    _package = nil;
    _installed = NO;
    [self setBusy:YES message:@"Opening and checking the selected file…"];
    [controller dismissViewControllerAnimated:YES completion:nil];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        VMIPAPackage *package = [[VMIPAPackage alloc] initWithURL:url error:&error];
        if (scoped) [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_package = package;
            [self setBusy:NO message:package ? @"Ready to install. The guest disk has not been changed."
                : error.localizedDescription ?: @"The selected file could not be read."];
        });
    });
}
- (void)confirmInstall {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:
        [NSString stringWithFormat:@"Install %@?", _package.displayName]
        message:[NSString stringWithFormat:@"%@ will shut down first. Existing apps will not be replaced.", _machineName]
        preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Install" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        if (!self.prepareHandler) return;
        self->_installOnReturn = YES;
        self.prepareHandler();
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)installPackage {
    if (!_package || _busy) return;
    [self setBusy:YES message:@"Preparing a recoverable disk copy and installing the app…"];
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
        BOOL installed = [package installInDirectory:directory error:&error];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_installed = installed;
            if (installed) self->_package = nil;
            [self setBusy:NO message:installed
                ? @"App added to the guest disk. Start the machine to refresh its Home screen icon. Compatibility checks do not guarantee the app will run."
                : error.localizedDescription ?: @"Installation did not finish."];
            if (self->_backgroundTask != UIBackgroundTaskInvalid) {
                [app endBackgroundTask:self->_backgroundTask];
                self->_backgroundTask = UIBackgroundTaskInvalid;
            }
        });
    });
}
@end
