//
//  S5LBox — VMInstanceListViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMInstanceListViewController.h"

#import "EmulatorViewController.h"
#import "VMEngine.h"
#import "VMGuestInstallViewController.h"
#import "VMGuestInstall.h"
#import "VMIPAInstallViewController.h"
#import "VMIPALibraryViewController.h"
#import "VMGuest.h"
#import "VMResumeCheckpoint.h"
#import "VMInstanceStore.h"
#import "VMInstances.h"
#import "VMSettings.h"
#import "VMSettingsViewController.h"
#import "VMFirmwareImporter.h"
#import "VMSetupViewController.h"
#include <math.h>

static NSString *const kCell = @"machine";
static NSString *const kAutomationMachinePrefix = @"s5lbox.machine.";

@interface VMInstanceListViewController () <UIGestureRecognizerDelegate>
- (NSUInteger)indexForMachineID:(NSString *)identifier;
- (BOOL)performMachineAction:(NSString *)name identifier:(NSString *)identifier;
- (BOOL)openInstanceAtIndex:(NSUInteger)index animated:(BOOL)animated
             afterShutdown:(void (^)(void))afterShutdown;
- (BOOL)openInstanceAtIndex:(NSUInteger)index animated:(BOOL)animated
             afterShutdown:(void (^)(void))afterShutdown openPackages:(BOOL)openPackages;
- (void)prepareGuestInstall:(UIViewController *)install
                instanceID:(NSString *)identifier returningThrough:(UIViewController *)library;
- (void)chooseMachineForIPA:(NSURL *)url library:(VMIPALibraryViewController *)library;
- (void)inspectIPA:(NSURL *)url library:(VMIPALibraryViewController *)library
       instanceID:(NSString *)identifier machineName:(NSString *)name;
@end

@implementation VMInstanceListViewController {
    BOOL _presentedSetup;
    UIPanGestureRecognizer *_rowSwipeGuard;
}

- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (!self) return nil;
    self.title = @"Machines";
    _showsSetupWhenNeeded = YES;
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    self.navigationItem.largeTitleDisplayMode =
        UINavigationItemLargeTitleDisplayModeAlways;

    UIBarButtonItem *add = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                             target:self action:@selector(addTapped)];
    UIBarButtonItem *settings = [[UIBarButtonItem alloc]
        initWithImage:([UIImage systemImageNamed:@"gearshape"] ?: [UIImage systemImageNamed:@"gear"])
                 style:UIBarButtonItemStylePlain
                target:self action:@selector(settingsTapped)];
    settings.accessibilityLabel = @"Settings";
    add.accessibilityIdentifier = @"s5lbox.machines.add";
    settings.accessibilityIdentifier = @"s5lbox.machines.settings";
    self.editButtonItem.accessibilityIdentifier = @"s5lbox.machines.edit";
    self.tableView.accessibilityIdentifier = @"s5lbox.machines.list";
    /* The first item is nearest the trailing edge. Keep Create in the familiar
     * top-right position and put global app setup beside it. Previously the
     * only route to firmware import was to open a machine and start its engine
     * first, which made initial setup feel backwards. */
    self.navigationItem.rightBarButtonItems = @[ add, settings ];
    self.navigationItem.leftBarButtonItem = self.editButtonItem;
    /* With swipe actions removed, UIKit can interpret a horizontal drag that
     * ends inside a cell as selection. Consume that drag without opening a VM. */
    _rowSwipeGuard = [[UIPanGestureRecognizer alloc] initWithTarget:self action:@selector(ignoreRowSwipe:)];
    _rowSwipeGuard.delegate = self;
    _rowSwipeGuard.maximumNumberOfTouches = 1;
    [self.tableView addGestureRecognizer:_rowSwipeGuard];

    [[NSNotificationCenter defaultCenter]
        addObserver:self
           selector:@selector(storeChanged)
               name:VMInstanceStoreDidChangeNotification
             object:nil];

    /* A first launch with no machines is an empty table and nothing to do, so
     * it starts with one rather than an empty screen and an unexplained plus
     * button. */
    if ([[VMInstanceStore sharedStore] count] == 0)
        [[VMInstanceStore sharedStore] createInstanceNamed:@"iPhone OS 3.1.3"
                                                     error:NULL];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)storeChanged {
    [self.tableView reloadData];
}

- (void)ignoreRowSwipe:(UIPanGestureRecognizer *)gesture { (void)gesture; }
- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer *)gesture {
    if (gesture != _rowSwipeGuard) return YES;
    CGPoint velocity = [_rowSwipeGuard velocityInView:self.tableView];
    return fabs(velocity.x) > fabs(velocity.y);
}
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gesture
    shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)other {
    return (gesture == _rowSwipeGuard && other == self.tableView.panGestureRecognizer) ||
           (other == _rowSwipeGuard && gesture == self.tableView.panGestureRecognizer);
}

/* The footer's claim about what opening a machine does depends on files this
 * screen does not own -- importing firmware happens two screens away and does
 * not touch the instance store, so -storeChanged never fires for it. Reload on
 * every appearance so returning from the importer cannot leave the old,
 * now-false sentence on screen. */
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self.tableView reloadData];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (!_showsSetupWhenNeeded || _presentedSetup || self.presentedViewController ||
        [VMFirmwareImporter hasConfiguredFirmware]) return;
    _presentedSetup = YES;
    VMSetupViewController *setup = [VMSetupViewController new];
    UINavigationController *navigation = [[UINavigationController alloc] initWithRootViewController:setup];
    navigation.modalPresentationStyle = UIModalPresentationFullScreen;
    __weak VMInstanceListViewController *weakSelf = self;
    setup.completion = ^(BOOL start) {
        [weakSelf dismissViewControllerAnimated:YES completion:^{
            VMInstanceListViewController *list = weakSelf;
            if (!start || !list) return;
            if (VMInstanceStore.sharedStore.count == 0) {
                NSError *error = nil;
                if (![VMInstanceStore.sharedStore createInstanceNamed:@"iPhone OS 3.1.3" error:&error]) {
                    [list showError:error doing:@"Could not create the machine"];
                    return;
                }
            }
            [list openInstanceAtIndex:0 animated:YES];
        }];
    };
    [self presentViewController:navigation animated:YES completion:nil];
}

#pragma mark - Alerts

- (void)showError:(NSError *)error doing:(NSString *)what {
    NSString *why = error.localizedDescription ?: @"unknown";
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:what
                         message:why
                  preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK"
                                          style:UIAlertActionStyleDefault
                                        handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

/* One prompt shape for create and rename, because they differ only in the
 * title, the starting text and what they do with the result. */
- (void)promptWithTitle:(NSString *)title
                   text:(NSString *)text
                 accept:(NSString *)accept
                 handler:(void (^)(NSString *name))handler {
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:title
                         message:nil
                  preferredStyle:UIAlertControllerStyleAlert];
    [a addTextFieldWithConfigurationHandler:^(UITextField *field) {
        field.text = text;
        field.placeholder = @"Name";
        field.autocapitalizationType = UITextAutocapitalizationTypeWords;
        field.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                          style:UIAlertActionStyleCancel
                                        handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:accept
                                          style:UIAlertActionStyleDefault
                                        handler:^(UIAlertAction *action) {
        (void)action;
        NSString *raw = a.textFields.firstObject.text ?: @"";
        /* Trim here rather than in the C: the model deliberately does not
         * guess what the user meant, so somebody has to decide, and a text
         * field is where trailing spaces come from. */
        NSString *name = [raw stringByTrimmingCharactersInSet:
            [NSCharacterSet whitespaceCharacterSet]];
        [a dismissViewControllerAnimated:YES completion:^{ handler(name); }];
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

#pragma mark - Actions

- (void)addTapped {
    [self promptWithTitle:@"New Machine"
                     text:@""
                   accept:@"Next"
                  handler:^(NSString *name) {
        UIAlertController *sizes = [UIAlertController alertControllerWithTitle:@"Disk Size"
            message:@"Maximum guest capacity. Host storage grows as files are added. This choice applies only to the new machine."
            preferredStyle:UIAlertControllerStyleActionSheet];
        for (NSNumber *value in @[@2, @4, @8]) {
            NSString *title = [NSString stringWithFormat:@"%@ GiB%@", value,
                value.unsignedIntegerValue == 2 ? @" (Default)" : @""];
            [sizes addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault
                handler:^(UIAlertAction *action) {
                    (void)action;
                    NSError *err = nil;
                    if (![[VMInstanceStore sharedStore] createInstanceNamed:name
                        diskSizeGiB:value.unsignedIntegerValue error:&err])
                        [self showError:err doing:@"Could not create the machine"];
                }]];
        }
        [sizes addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        sizes.popoverPresentationController.barButtonItem = self.navigationItem.rightBarButtonItems.firstObject;
        [self presentViewController:sizes animated:YES completion:nil];
    }];
}

- (void)beginGuestInstallForID:(NSString *)identifier name:(NSString *)name packagesOnly:(BOOL)packagesOnly {
    __weak VMInstanceListViewController *weakSelf = self;
        VMInstanceListViewController *self_ = weakSelf;
        UINavigationController *navigation = self_.navigationController;
        if (!self_ || !navigation || navigation.topViewController != self_)
            return;
        VMGuestInstallViewController *install =
            [[VMGuestInstallViewController alloc] initWithInstanceID:identifier
                                                         machineName:name];
        __weak VMGuestInstallViewController *weakInstall = install;
        install.packageManagerOnly = packagesOnly;
        install.readyHandler = ^{
            VMInstanceListViewController *list = weakSelf;
            VMGuestInstallViewController *screen = weakInstall;
            UINavigationController *nav = list.navigationController;
            if (!list || !screen || nav.topViewController != screen) return;
            [nav popViewControllerAnimated:NO];
            VMInstanceStore *store = [VMInstanceStore sharedStore];
            for (NSUInteger index = 0u; index < store.count; index++) {
                NSDictionary *row = [store instanceAtIndex:index];
                if ([row[@"id"] isEqualToString:identifier]) {
                    [list openInstanceAtIndex:index animated:YES afterShutdown:nil
                                 openPackages:screen.openPackagesWhenReady];
                    return;
                }
            }
            NSError *missing = [NSError errorWithDomain:
                @"com.j0shua.S5LBox.GuestInstall"
                                                   code:1
                                               userInfo:@{
                NSLocalizedDescriptionKey:
                    @"Its disk was installed, but its Machines entry was removed."
            }];
            [list showError:missing
                      doing:@"The installed machine no longer exists"];
        };
        /* Machines owns no running engine: every exit waits for disk teardown.
         * Jailbreak explicitly discards saved CPU state and strictly validates
         * the stopped disk. Do not boot it just to drive a guest Power slider. */
        [navigation pushViewController:install animated:YES];
}

- (void)settingsTapped {
    VMSettingsViewController *settings = [[VMSettingsViewController alloc] init];
    __weak VMInstanceListViewController *weakSelf = self;
    settings.guestInstallRequest = ^(NSString *identifier, NSString *name) {
        [weakSelf beginGuestInstallForID:identifier name:name packagesOnly:NO];
    };
    settings.guestPackageSetupRequest = ^(NSString *identifier, NSString *name) {
        [weakSelf beginGuestInstallForID:identifier name:name packagesOnly:YES];
    };
    settings.guestIPARequest = ^{
        VMInstanceListViewController *list = weakSelf;
        UINavigationController *navigation = list.navigationController;
        if (!list || navigation.topViewController != list) return;
        VMIPALibraryViewController *library = [[VMIPALibraryViewController alloc] init];
        __weak VMIPALibraryViewController *weakLibrary = library;
        library.selectionHandler = ^(NSURL *url) {
            VMInstanceListViewController *current = weakSelf;
            VMIPALibraryViewController *collection = weakLibrary;
            if (!current || !collection || current.navigationController.topViewController != collection) return;
            [current chooseMachineForIPA:url library:collection];
        };
        [navigation pushViewController:library animated:YES];
    };
    UINavigationController *nav = [[UINavigationController alloc]
        initWithRootViewController:settings];
    nav.navigationBar.prefersLargeTitles = YES;
    [self presentViewController:nav animated:YES completion:nil];
}

- (void)chooseMachineForIPA:(NSURL *)url library:(VMIPALibraryViewController *)library {
    VMInstanceStore *store = VMInstanceStore.sharedStore;
    NSMutableArray<NSDictionary *> *eligible = [NSMutableArray array];
    for (NSUInteger i = 0; i < store.count; i++) {
        NSDictionary *row = [store instanceAtIndex:i];
        NSString *directory = [store directoryForInstanceWithID:row[@"id"]];
        if (directory.length && vm_guest_install_probe(directory.fileSystemRepresentation,
            NULL, NULL, 0) == VM_GUEST_INSTALL_PROBE_VALID) [eligible addObject:row];
    }
    if (eligible.count == 1) {
        [self inspectIPA:url library:library instanceID:eligible[0][@"id"] machineName:eligible[0][@"name"]];
        return;
    }
    UIAlertController *picker = [UIAlertController alertControllerWithTitle:
        eligible.count ? @"Install in which machine?" : @"Jailbreak a machine first"
        message:eligible.count ? nil : @"Your IPAs can stay in this library. Prepare a machine and use App Settings → Jailbreak before installing apps."
        preferredStyle:eligible.count ? UIAlertControllerStyleActionSheet : UIAlertControllerStyleAlert];
    for (NSDictionary *row in eligible) {
        [picker addAction:[UIAlertAction actionWithTitle:row[@"name"] style:UIAlertActionStyleDefault
            handler:^(UIAlertAction *action) {
                [self inspectIPA:url library:library instanceID:row[@"id"] machineName:row[@"name"]];
            }]];
    }
    [picker addAction:[UIAlertAction actionWithTitle:eligible.count ? @"Cancel" : @"OK"
        style:UIAlertActionStyleCancel handler:nil]];
    picker.popoverPresentationController.sourceView = library.view;
    picker.popoverPresentationController.sourceRect = CGRectMake(CGRectGetMidX(library.view.bounds),
        CGRectGetMidY(library.view.bounds), 1, 1);
    picker.popoverPresentationController.permittedArrowDirections = 0;
    [library presentViewController:picker animated:YES completion:nil];
}

- (void)inspectIPA:(NSURL *)url library:(VMIPALibraryViewController *)library
       instanceID:(NSString *)identifier machineName:(NSString *)name {
    UINavigationController *navigation = self.navigationController;
    if (navigation.topViewController != library) return;
    VMIPAInstallViewController *install = [[VMIPAInstallViewController alloc]
        initWithInstanceID:identifier machineName:name];
    __weak VMInstanceListViewController *weakSelf = self;
    __weak VMIPALibraryViewController *weakLibrary = library;
    __weak VMIPAInstallViewController *weakInstall = install;
    install.prepareHandler = ^{
        VMInstanceListViewController *owner = weakSelf;
        VMIPAInstallViewController *screen = weakInstall;
        VMIPALibraryViewController *back = weakLibrary;
        UINavigationController *stack = owner.navigationController;
        if (!owner || !screen || !back || stack.topViewController != screen) return;
        [stack popToViewController:owner animated:NO];
        [owner prepareGuestInstall:screen instanceID:identifier returningThrough:back];
    };
    install.readyHandler = ^{
        VMInstanceListViewController *owner = weakSelf;
        VMIPAInstallViewController *screen = weakInstall;
        UINavigationController *stack = owner.navigationController;
        if (!owner || !screen || stack.topViewController != screen) return;
        [stack popToViewController:owner animated:NO];
        VMInstanceStore *store = VMInstanceStore.sharedStore;
        for (NSUInteger i = 0; i < store.count; i++) {
            if ([[store instanceAtIndex:i][@"id"] isEqualToString:identifier]) {
                [owner openInstanceAtIndex:i animated:YES];
                break;
            }
        }
    };
    [navigation pushViewController:install animated:YES];
    [install inspectURL:url];
}

- (void)prepareGuestInstall:(UIViewController *)install
                instanceID:(NSString *)identifier returningThrough:(UIViewController *)library {
    NSString *directory = [[VMInstanceStore sharedStore]
        directoryForInstanceWithID:identifier];
    UINavigationController *navigation = self.navigationController;
    void (^presentInstall)(VMInstanceListViewController *) = ^(VMInstanceListViewController *list) {
        if (library) [navigation setViewControllers:@[list, library, install] animated:YES];
        else [navigation pushViewController:install animated:YES];
    };
    /* Do not boot an already powered-off guest just to shut it down again.
     * The existing read-only probe validates the complete checkpoint pair,
     * checksum, disk geometry and PMU witness; existence alone is not enough.
     * Keep its snapshot I/O off the main thread, and prevent another open
     * from racing the check. The installer still performs its own preflight. */
    navigation.view.userInteractionEnabled = NO;
    __weak VMInstanceListViewController *weakSelf = self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSDictionary *attributes = [[NSFileManager defaultManager]
            attributesOfItemAtPath:[directory stringByAppendingPathComponent:
                @"rootfs-work.img"] error:NULL];
        uint64_t size = [attributes fileSize];
        BOOL poweredOff = size && vm_resume_checkpoint_probe_state(
            directory.fileSystemRepresentation, size,
            VM_GUEST_RAM_BASE, VM_GUEST_RAM_SIZE, NULL, 0u) ==
                VM_RESUME_CHECKPOINT_POWERED_OFF;
        dispatch_async(dispatch_get_main_queue(), ^{
            navigation.view.userInteractionEnabled = YES;
            VMInstanceListViewController *list = weakSelf;
            if (!list || navigation.topViewController != list) return;
            if (poweredOff) {
                presentInstall(list);
                return;
            }
            /* Otherwise use the visible machine lifecycle. A failed shutdown
             * leaves that machine open; there is no hidden writable engine. */
            VMInstanceStore *store = [VMInstanceStore sharedStore];
            for (NSUInteger index = 0u; index < store.count; index++) {
                if ([[store instanceAtIndex:index][@"id"] isEqualToString:identifier]) {
                    [list openInstanceAtIndex:index animated:YES afterShutdown:^{
                        VMInstanceListViewController *current = weakSelf;
                        if (current && navigation.topViewController == current)
                            presentInstall(current);
                    }];
                    return;
                }
            }
            NSError *missing = [NSError errorWithDomain:@"com.j0shua.S5LBox.GuestInstall"
                code:1 userInfo:@{ NSLocalizedDescriptionKey:
                    @"The selected machine no longer exists. No disk changes were started." }];
            [list showError:missing doing:@"Could not open the machine"];
        });
    });
}

- (void)renameAtIndex:(NSUInteger)index {
    NSDictionary *row = [[VMInstanceStore sharedStore] instanceAtIndex:index];
    if (!row) return;
    NSString *identifier = row[@"id"];
    [self promptWithTitle:@"Rename"
                     text:row[@"name"]
                   accept:@"Rename"
                  handler:^(NSString *name) {
        NSError *err = nil;
        NSUInteger current = [self indexForMachineID:identifier];
        if (current == NSNotFound) return;
        if (![[VMInstanceStore sharedStore] renameInstanceAtIndex:current
                                                                to:name
                                                             error:&err])
            [self showError:err doing:@"Could not rename the machine"];
    }];
}

/*
 * The CONFIGURATION, not the disk. VMInstances.h is explicit that duplicating
 * copies the option values and nothing on the filesystem, and now that a
 * machine has a filesystem that matters: the copy gets a fresh work image
 * built from the pristine rootfs.img the first time it is opened, and says so
 * while it is doing it. Copying a 465 MB image behind a swipe action, with no
 * progress and no way to cancel, is the alternative and it is worse.
 */
- (void)duplicateAtIndex:(NSUInteger)index {
    NSError *err = nil;
    if (![[VMInstanceStore sharedStore] duplicateInstanceAtIndex:index error:&err])
        [self showError:err doing:@"Could not duplicate the machine"];
}

- (void)confirmDeleteAtIndex:(NSUInteger)index {
    NSDictionary *row = [[VMInstanceStore sharedStore] instanceAtIndex:index];
    if (!row) return;
    NSString *identifier = row[@"id"];
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:[NSString stringWithFormat:@"Delete “%@”?",
                                  row[@"name"]]
                         message:@"Its saved files are deleted too. This cannot be undone."
                  preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                          style:UIAlertActionStyleCancel
                                        handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"Delete"
                                          style:UIAlertActionStyleDestructive
                                        handler:^(UIAlertAction *action) {
        (void)action;
        NSError *err = nil;
        NSUInteger current = [self indexForMachineID:identifier];
        if (current == NSNotFound) return;
        if (![[VMInstanceStore sharedStore] deleteInstanceAtIndex:current error:&err])
            [self showError:err doing:@"Could not delete the machine"];
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

#pragma mark - Table

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return 1;
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return (NSInteger)[[VMInstanceStore sharedStore] count];
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return @"Machines";
}

/*
 * What opening a machine actually does, on the first screen rather than buried
 * in settings.
 *
 * This used to be a constant saying no machine boots Apple's firmware. That is
 * no longer true when firmware has been imported, and a fixed string is
 * exactly how a UI ends up lying about it — so the sentence comes from
 * +[VMEngine firmwareReadinessSummary], which asks the same C code the engine
 * itself will ask a moment later. The two cannot disagree.
 */
- (NSString *)tableView:(UITableView *)tableView
titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return @"Touch and hold a machine to rename, duplicate, or delete it. Only one machine runs at a time.";
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    /* Deliberately not -registerClass:/-dequeue...forIndexPath:. A registered
     * UITableViewCell is created with UITableViewCellStyleDefault, whose
     * detailTextLabel is nil, so the subtitle below would silently go nowhere.
     * Choosing the subtitle style requires constructing the cell here. */
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:kCell];
    if (!cell)
        cell = [[UITableViewCell alloc]
                   initWithStyle:UITableViewCellStyleSubtitle
                 reuseIdentifier:kCell];
    NSDictionary *row =
        [[VMInstanceStore sharedStore] instanceAtIndex:(NSUInteger)indexPath.row];

    cell.textLabel.text = row[@"name"] ?: @"?";
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    cell.textLabel.adjustsFontForContentSizeCategory = YES;
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    cell.imageView.image = [UIImage systemImageNamed:@"iphone"];
    cell.imageView.tintColor = [UIColor systemBlueColor];

    /* The subtitle is the machine's history, and it says "never opened"
     * rather than showing a 1970 date for a zero stamp. */
    uint64_t opened = [row[@"opened"] unsignedLongLongValue];
    uint64_t retired = [row[@"retired"] unsignedLongLongValue];
    NSString *when;
    if (opened == 0u) {
        when = @"never opened";
    } else {
        NSDate *d = [NSDate dateWithTimeIntervalSince1970:(NSTimeInterval)opened];
        NSDateFormatter *f = [[NSDateFormatter alloc] init];
        f.dateStyle = NSDateFormatterMediumStyle;
        f.timeStyle = NSDateFormatterShortStyle;
        when = [NSString stringWithFormat:@"opened %@", [f stringFromDate:d]];
    }
    NSString *work = (retired == 0u)
        ? @""
        : [NSString stringWithFormat:@" · %.2f B instructions",
           (double)retired / 1e9];
    cell.detailTextLabel.text = [when stringByAppendingString:work];
    if ([row[@"diskSizeGiB"] unsignedIntegerValue])
        cell.detailTextLabel.text = [cell.detailTextLabel.text stringByAppendingFormat:
            @" · %@ GiB disk", row[@"diskSizeGiB"]];
    cell.detailTextLabel.font =
        [UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.detailTextLabel.textColor = [UIColor secondaryLabelColor];
    cell.accessibilityHint = @"Opens this machine. Actions are available to rename, duplicate, or delete it.";
    NSString *identifier = row[@"id"];
    __weak VMInstanceListViewController *weakSelf = self;
    NSMutableArray *actions = [NSMutableArray array];
    for (NSString *name in @[@"Rename", @"Duplicate configuration", @"Delete"]) {
        [actions addObject:[[UIAccessibilityCustomAction alloc] initWithName:name
            actionHandler:^BOOL(__unused UIAccessibilityCustomAction *action) {
                return [weakSelf performMachineAction:name identifier:identifier];
            }]];
    }
    cell.accessibilityCustomActions = actions;
    cell.accessibilityIdentifier = identifier.length
        ? [kAutomationMachinePrefix stringByAppendingString:identifier]
        : @"s5lbox.machine.unknown";
    return cell;
}

- (BOOL)openInstanceAtIndex:(NSUInteger)index animated:(BOOL)animated {
    return [self openInstanceAtIndex:index animated:animated afterShutdown:nil];
}

- (BOOL)openInstanceAtIndex:(NSUInteger)index animated:(BOOL)animated
             afterShutdown:(void (^)(void))afterShutdown {
    return [self openInstanceAtIndex:index animated:animated afterShutdown:afterShutdown
                       openPackages:NO];
}

- (BOOL)openInstanceAtIndex:(NSUInteger)index animated:(BOOL)animated
             afterShutdown:(void (^)(void))afterShutdown openPackages:(BOOL)openPackages {
    NSDictionary *row = [[VMInstanceStore sharedStore] instanceAtIndex:index];
    UINavigationController *navigation = self.navigationController;

    /* Refuse ambiguous automation state. A hidden list must not push another
     * emulator over whichever controller is currently visible. */
    if (!row || !navigation || navigation.topViewController != self) return NO;

    VMInstanceStore *store = [VMInstanceStore sharedStore];
    BOOL mbxEnabled = NO, softwareRendererEnabled = NO;
    NSError *graphicsError = nil;
    BOOL hasRecordedGraphics =
        [store graphicsForOpeningInstanceWithID:row[@"id"]
                                     mbxEnabled:&mbxEnabled
                        softwareRendererEnabled:&softwareRendererEnabled
                                          error:&graphicsError];
    if (graphicsError) {
        [self showError:graphicsError doing:@"Could not open the machine"];
        return NO;
    }
    if (hasRecordedGraphics) {
        [[VMSettings sharedSettings]
            useRecordedGraphicsForMachineWithMBX:mbxEnabled
                                softwareRenderer:softwareRendererEnabled];
    } else {
        /* This is compatibility, not migration. Historical bits did not
         * necessarily prepare the image, so an old machine keeps exactly the
         * global launch behaviour it had before this feature. */
        [[VMSettings sharedSettings] clearRecordedGraphicsForMachine];
    }

    [store noteOpenedInstanceWithID:row[@"id"]];

    EmulatorViewController *vc = [[EmulatorViewController alloc] init];
    vc.title = row[@"name"];
    /* The identity, not just the name: it is what decides which root
     * filesystem this machine gets. Without it the engine will not boot
     * firmware at all, which is deliberate -- see -[VMEngine
     * initWithInstanceID:]. */
    vc.instanceID = row[@"id"];
    vc.guestShutdownCompletion = afterShutdown;
    /* Configure the actual controller before pushing. UIKit may defer the push
     * during a preceding pop; topViewController can still be Machines then. */
    vc.openPackagesWhenRunning = openPackages;
    __weak VMInstanceListViewController *weakSelf = self;
    NSString *identifier = row[@"id"], *name = row[@"name"];
    vc.packageSetupRequest = ^{
        [weakSelf beginGuestInstallForID:identifier name:name packagesOnly:YES];
    };
    [navigation pushViewController:vc animated:animated];
    return YES;
}

- (BOOL)openFirstMachineForAutomation {
    /* -viewDidLoad creates the initial machine when this is a fresh container.
     * Loading here makes that contract explicit instead of depending on the
     * navigation controller's current view-loading timing. */
    [self loadViewIfNeeded];
    if ([[VMInstanceStore sharedStore] count] == 0) return NO;
    return [self openInstanceAtIndex:0 animated:NO];
}

- (void)tableView:(UITableView *)tableView
didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    [self openInstanceAtIndex:(NSUInteger)indexPath.row animated:YES];
}

/* Resolve the identity when an action is invoked, not the row at menu creation.
 * The list can change while a context menu or a confirmation alert is open. */
- (NSUInteger)indexForMachineID:(NSString *)identifier {
    VMInstanceStore *store = VMInstanceStore.sharedStore;
    for (NSUInteger index = 0; index < store.count; ++index)
        if ([[store instanceAtIndex:index][@"id"] isEqual:identifier]) return index;
    return NSNotFound;
}

- (BOOL)performMachineAction:(NSString *)name identifier:(NSString *)identifier {
    if (self.navigationController.topViewController != self || self.presentedViewController) return NO;
    NSUInteger index = [self indexForMachineID:identifier];
    if (index == NSNotFound) return NO;
    if ([name isEqualToString:@"Rename"]) [self renameAtIndex:index];
    else if ([name isEqualToString:@"Duplicate configuration"]) [self duplicateAtIndex:index];
    else if ([name isEqualToString:@"Delete"]) [self confirmDeleteAtIndex:index];
    else return NO;
    return YES;
}

- (UIContextMenuConfiguration *)tableView:(UITableView *)tableView
    contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath point:(CGPoint)point {
    (void)tableView; (void)point;
    if (self.editing) return nil;
    NSDictionary *row = [VMInstanceStore.sharedStore instanceAtIndex:(NSUInteger)indexPath.row];
    NSString *identifier = row[@"id"];
    if (!identifier.length) return nil;
    __weak VMInstanceListViewController *weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:identifier previewProvider:nil
        actionProvider:^UIMenu *(__unused NSArray<UIMenuElement *> *suggested) {
            NSMutableArray *actions = [NSMutableArray array];
            NSArray *names = @[@"Rename", @"Duplicate configuration", @"Delete"];
            NSArray *symbols = @[@"pencil", @"doc.on.doc", @"trash"];
            for (NSUInteger i = 0; i < names.count; ++i) {
                NSString *name = names[i];
                UIAction *action = [UIAction actionWithTitle:(i == 1 ? @"Duplicate" : name) image:[UIImage systemImageNamed:symbols[i]]
                    identifier:nil handler:^(__unused UIAction *selected) {
                        [weakSelf performMachineAction:name identifier:identifier];
                    }];
                if (i == 2) action.attributes = UIMenuElementAttributesDestructive;
                [actions addObject:action];
            }
            return [UIMenu menuWithTitle:@"" children:actions];
        }];
}

/* Empty, not nil: nil permits UIKit's fallback swipe-to-delete because this
 * table still intentionally supports the Edit button's delete controls. */
- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    return [UISwipeActionsConfiguration configurationWithActions:@[]];
}
- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
    leadingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView; (void)indexPath;
    return [UISwipeActionsConfiguration configurationWithActions:@[]];
}

/* Edit-mode delete, for the same reason: it is what the Edit button implies. */
- (void)tableView:(UITableView *)tableView
commitEditingStyle:(UITableViewCellEditingStyle)style
forRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    if (style == UITableViewCellEditingStyleDelete)
        [self confirmDeleteAtIndex:(NSUInteger)indexPath.row];
}

@end
