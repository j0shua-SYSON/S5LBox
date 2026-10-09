// See VMRuntimeSettingsViewController.h.
// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMRuntimeSettingsViewController.h"
#import "VMSettings.h" // Read-only cap choices, not the shared preferences.
#import "VMIPALibraryViewController.h"
#import "VMIPAInstallViewController.h"
#import "VMPackageManagerViewController.h"

typedef NS_ENUM(NSInteger, VMRuntimeSection) {
    VMRuntimeSectionSession,
    VMRuntimeSectionApps,
    VMRuntimeSectionSnapshots,
    VMRuntimeSectionPower,
    VMRuntimeSectionDeveloper,
};

@implementation VMRuntimeSettingsViewController

- (instancetype)init {
    return [self initWithStyle:UITableViewStyleGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Machine Settings";
    self.navigationController.navigationBar.prefersLargeTitles = YES;
    self.tableView.accessibilityIdentifier = @"s5lbox.machine-settings";
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 65;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemDone
        target:self action:@selector(doneTapped)];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.tableView reloadData];
}

- (void)doneTapped {
    [self dismissViewControllerAnimated:YES completion:nil];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return self.showsDeveloperControls ? 5 : 4;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    switch ((VMRuntimeSection)section) {
        case VMRuntimeSectionSession: return 3;
        case VMRuntimeSectionApps: return 2;
        case VMRuntimeSectionSnapshots: return 1;
        case VMRuntimeSectionPower: return 4;
        case VMRuntimeSectionDeveloper: return self.showsDeveloperControls ? 2 : 0;
    }
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    switch ((VMRuntimeSection)section) {
        case VMRuntimeSectionSession: return self.machineName.length ? self.machineName : @"Current machine";
        case VMRuntimeSectionApps: return @"Guest apps";
        case VMRuntimeSectionSnapshots: return @"Saved states";
        case VMRuntimeSectionPower: return @"Power";
        case VMRuntimeSectionDeveloper: return @"Session diagnostics";
    }
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    if (section == VMRuntimeSectionSession)
        return @"Changes apply only to this open session. App defaults, firmware and jailbreak are in App Settings on Machines.";
    if (section == VMRuntimeSectionPower)
        return @"Save & close keeps your place. Shut down powers off iPhone OS for a fresh boot next time.";
    if (section == VMRuntimeSectionDeveloper)
        return @"These controls do not change defaults for other machines.";
    return nil;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"runtime"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle
                                           reuseIdentifier:@"runtime"];
    cell.accessoryView = nil;
    cell.accessoryType = UITableViewCellAccessoryNone;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    cell.textLabel.text = nil;
    cell.detailTextLabel.text = nil;
    cell.textLabel.textColor = [UIColor labelColor];
    cell.detailTextLabel.textColor = [UIColor secondaryLabelColor];
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    cell.textLabel.numberOfLines = cell.detailTextLabel.numberOfLines = 0;
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.accessibilityIdentifier = nil;

    id<VMRuntimeSettingsDelegate> owner = self.runtimeDelegate;
    if (path.section == VMRuntimeSectionSession ||
        (path.section == VMRuntimeSectionDeveloper && path.row == 0)) {
        UISwitch *toggle = [[UISwitch alloc] init];
        if (path.section == VMRuntimeSectionDeveloper) {
            cell.textLabel.text = @"Console under the screen";
            cell.detailTextLabel.text = @"Show live guest output beneath the display.";
            toggle.on = [owner runtimeInlineConsole];
            toggle.accessibilityIdentifier = @"s5lbox.machine-settings.console";
            [toggle addTarget:self action:@selector(consoleChanged:)
                      forControlEvents:UIControlEventValueChanged];
        } else if (path.row == 0) {
            cell.textLabel.text = @"Pause guest";
            toggle.on = [owner runtimePaused];
            toggle.enabled = [owner runtimeCanControlGuest];
            if (!toggle.enabled) cell.detailTextLabel.text = @"The guest is not running.";
            toggle.accessibilityIdentifier = @"s5lbox.machine-settings.pause";
            [toggle addTarget:self action:@selector(pauseChanged:)
                      forControlEvents:UIControlEventValueChanged];
        } else if (path.row == 1) {
            cell.textLabel.text = @"Pause in background";
            cell.detailTextLabel.text = @"Recommended. iOS may terminate the app if disabled.";
            toggle.on = [owner runtimePausesInBackground];
            toggle.accessibilityIdentifier = @"s5lbox.machine-settings.background";
            [toggle addTarget:self action:@selector(backgroundChanged:)
                      forControlEvents:UIControlEventValueChanged];
        } else {
            cell.textLabel.text = @"Guest microphone";
            cell.detailTextLabel.text = [owner runtimeAudioStatus];
            toggle.on = [owner runtimeMicrophoneEnabled];
            toggle.enabled = [owner runtimeCanControlGuest];
            toggle.accessibilityIdentifier = @"s5lbox.machine-settings.microphone";
            [toggle addTarget:self action:@selector(microphoneChanged:)
                      forControlEvents:UIControlEventValueChanged];
        }
        toggle.accessibilityLabel = cell.textLabel.text;
        cell.accessoryView = toggle;
        return cell;
    }
    BOOL enabled = owner != nil;
    if (path.section == VMRuntimeSectionApps) {
        cell.textLabel.text = path.row == 0 ? @"Install IPA…" : @"Packages…";
        enabled = self.usbTransport && [owner runtimeCanControlGuest] && ![owner runtimePaused];
        cell.detailTextLabel.text = [owner runtimePaused] ? @"Resume the guest first." : @"Choose an app from your IPA Library. Uses virtual USB, without shutting down.";
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.accessibilityIdentifier = @"s5lbox.machine-settings.ipa";
        if (path.row == 1) {
            cell.detailTextLabel.text = [owner runtimePaused] ? @"Resume the guest first." : @"Browse sources and install tweaks in the running guest.";
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.packages";
        }
    } else if (path.section == VMRuntimeSectionSnapshots) {
        cell.textLabel.text = @"Snapshots";
        enabled = enabled && self.snapshotsDirectory.length > 0;
        cell.accessoryType = enabled ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
        cell.accessibilityIdentifier = @"s5lbox.machine-settings.snapshots";
    } else if (path.section == VMRuntimeSectionPower) {
        if (path.row == VMRuntimeActionSaveAndClose) {
            cell.textLabel.text = [owner runtimeCanShutDown] ? @"Save & close" : @"Close machine";
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.close";
        } else if (path.row == VMRuntimeActionShutDown) {
            cell.textLabel.text = @"Shut down";
            enabled = [owner runtimeCanShutDown];
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.shutdown";
        } else if (path.row == VMRuntimeActionRestart) {
            cell.textLabel.text = @"Restart guest…";
            cell.detailTextLabel.text = @"Unsaved work may be lost.";
            cell.textLabel.textColor = [UIColor systemRedColor];
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.restart";
        } else {
            cell.textLabel.text = @"Force Power Off…";
            cell.detailTextLabel.text = @"Works during boot. Unsaved work may be lost.";
            enabled = [owner runtimeCanForcePowerOff];
            cell.textLabel.textColor = [UIColor systemRedColor];
            cell.accessibilityIdentifier = @"s5lbox.machine-settings.force-power-off";
        }
    } else {
        uint64_t cap = [owner runtimeInstructionCap];
        cell.textLabel.text = @"Instruction limit";
        cell.detailTextLabel.text = cap ? [NSString stringWithFormat:@"%llu total instructions", (unsigned long long)cap] : @"No limit";
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.accessibilityIdentifier = @"s5lbox.machine-settings.limit";
    }
    cell.selectionStyle = enabled ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    if (!enabled) cell.textLabel.textColor = [UIColor secondaryLabelColor];
    return cell;
}

- (void)pauseChanged:(UISwitch *)toggle {
    [self.runtimeDelegate setRuntimePaused:toggle.on];
    [self.tableView reloadData];
}
- (void)backgroundChanged:(UISwitch *)toggle {
    [self.runtimeDelegate setRuntimePausesInBackground:toggle.on];
}
- (void)microphoneChanged:(UISwitch *)toggle {
    toggle.enabled = NO;
    __weak VMRuntimeSettingsViewController *weakSelf = self;
    [self.runtimeDelegate setRuntimeMicrophoneEnabled:toggle.on completion:^(BOOL okay, NSString *message) {
        VMRuntimeSettingsViewController *self = weakSelf;
        if (!self) return;
        [self.tableView reloadData];
        if (!okay && self.view.window) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Microphone is off"
                message:message preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:alert animated:YES completion:nil];
        }
    }];
}
- (void)consoleChanged:(UISwitch *)toggle {
    [self.runtimeDelegate setRuntimeInlineConsole:toggle.on];
}

- (void)performAfterDismiss:(VMRuntimeAction)action {
    id<VMRuntimeSettingsDelegate> owner = self.runtimeDelegate;
    /* Dismiss the whole settings sheet, including any confirmation alert,
     * before the owner can present failure UI or navigate back to Machines. */
    UIViewController *presenter = self.navigationController.presentingViewController;
    if (!presenter) return;
    [presenter dismissViewControllerAnimated:YES completion:^{
        [owner performRuntimeAction:action];
    }];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (path.section == VMRuntimeSectionApps) {
        if (!self.usbTransport || ![self.runtimeDelegate runtimeCanControlGuest] || [self.runtimeDelegate runtimePaused]) return;
        if (path.row == 1) {
            VMPackageManagerViewController *packages = [[VMPackageManagerViewController alloc]
                initWithTransport:self.usbTransport instanceID:self.instanceID];
            packages.modalPresentationStyle = UIModalPresentationFullScreen;
            [self presentViewController:packages animated:YES completion:nil]; return;
        }
        VMIPALibraryViewController *library = [[VMIPALibraryViewController alloc] init];
        __weak VMRuntimeSettingsViewController *weakSelf = self;
        __weak VMIPALibraryViewController *weakLibrary = library;
        library.selectionHandler = ^(NSURL *url) {
            VMRuntimeSettingsViewController *owner = weakSelf;
            VMIPALibraryViewController *collection = weakLibrary;
            if (!owner || !collection || collection.navigationController.topViewController != collection) return;
            VMIPAInstallViewController *install = [[VMIPAInstallViewController alloc]
                initWithInstanceID:owner.instanceID machineName:owner.machineName];
            install.liveTransport = owner.usbTransport;
            install.readyHandler = ^{ [weakSelf dismissViewControllerAnimated:YES completion:nil]; };
            [collection.navigationController pushViewController:install animated:YES];
            [install inspectURL:url];
        };
        [self.navigationController pushViewController:library animated:YES];
    } else if (path.section == VMRuntimeSectionSnapshots) {
        if (!self.snapshotsDirectory.length || !self.runtimeDelegate) return;
        VMSnapshotListViewController *list = [[VMSnapshotListViewController alloc] init];
        list.snapshotsDirectory = self.snapshotsDirectory;
        list.delegate = self.runtimeDelegate;
        [self.navigationController pushViewController:list animated:YES];
    } else if (path.section == VMRuntimeSectionPower) {
        VMRuntimeAction action = (VMRuntimeAction)path.row;
        if (!self.runtimeDelegate || (action == VMRuntimeActionShutDown &&
            ![self.runtimeDelegate runtimeCanShutDown])) return;
        if (action == VMRuntimeActionForcePowerOff &&
            ![self.runtimeDelegate runtimeCanForcePowerOff]) return;
        if (action != VMRuntimeActionRestart) {
            [self performAfterDismiss:action];
            return;
        }
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Restart this guest?"
            message:@"This forces a fresh boot. Unsaved guest work may be lost."
            preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        __weak VMRuntimeSettingsViewController *weakSelf = self;
        [alert addAction:[UIAlertAction actionWithTitle:@"Restart" style:UIAlertActionStyleDestructive
            handler:^(__unused UIAlertAction *a) { [weakSelf performAfterDismiss:VMRuntimeActionRestart]; }]];
        [self presentViewController:alert animated:YES completion:nil];
    } else if (path.section == VMRuntimeSectionDeveloper && path.row == 1) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Instruction limit"
            message:@"Total instructions since boot. A limit already passed stops the guest immediately; it cannot resume that CPU state."
            preferredStyle:UIAlertControllerStyleActionSheet];
        __weak VMRuntimeSettingsViewController *weakSelf = self;
        for (NSNumber *choice in [VMSettings instructionCapChoices]) {
            uint64_t cap = choice.unsignedLongLongValue;
            NSString *label = !cap ? @"No limit" : cap >= 1000000000ull
                ? [NSString stringWithFormat:@"%llu billion", (unsigned long long)(cap / 1000000000ull)]
                : [NSString stringWithFormat:@"%llu million", (unsigned long long)(cap / 1000000ull)];
            [alert addAction:[UIAlertAction actionWithTitle:label
                style:cap ? UIAlertActionStyleDestructive : UIAlertActionStyleDefault
                handler:^(__unused UIAlertAction *a) {
                    [weakSelf.runtimeDelegate setRuntimeInstructionCap:cap];
                    [weakSelf.tableView reloadData];
                }]];
        }
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        alert.popoverPresentationController.sourceView = tableView;
        alert.popoverPresentationController.sourceRect = [tableView rectForRowAtIndexPath:path];
        [self presentViewController:alert animated:YES completion:nil];
    }
}
@end
