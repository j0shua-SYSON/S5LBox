//
//  VMSnapshotListViewController.m
//  S5LBox
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//

#import "VMSnapshotListViewController.h"

#include "VMSnapshotStore.h"

typedef NS_ENUM(NSInteger, VMSnapshotSection) {
    VMSnapshotSectionAction = 0,   /* the one thing you can DO */
    VMSnapshotSectionList,
    VMSnapshotSectionCount
};

@interface VMSnapshotListViewController ()
@property (nonatomic, strong) NSMutableArray<NSDictionary *> *snapshots;
@property (nonatomic, strong) UIView *progressScrim;
@property (nonatomic, strong) UILabel *progressLabel;
@property (nonatomic, strong) UIProgressView *progressBar;
@property (nonatomic, assign) BOOL busy;
@end

@implementation VMSnapshotListViewController

- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleGrouped];
    if (self) {
        _snapshots = [NSMutableArray array];
        self.title = @"Snapshots";
    }
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    [self buildProgressOverlay];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self reload];
}

#pragma mark - reading the store

/*
 * Read through VMSnapshotStore rather than with NSFileManager. The store owns
 * the id rules, and an id becomes a path component -- having two places that
 * decide what a valid snapshot looks like is how one of them ends up more
 * permissive than the other.
 */
- (void)reload {
    [self.snapshots removeAllObjects];

    NSString *dir = self.snapshotsDirectory;
    if (dir.length) {
        static const size_t kMax = VM_SNAPSHOT_MAX;
        vm_snapshot_info_t *items = calloc(kMax, sizeof *items);
        if (items) {
            size_t count = 0;
            char detail[256] = {0};
            vm_snapshot_status_t st =
                vm_snapshot_list(dir.fileSystemRepresentation, items, kMax,
                                 &count, detail, sizeof detail);
            /* TOO_MANY still filled the buffer, so the rows it did produce are
             * shown rather than discarded; only a real failure empties it. */
            if (st == VM_SNAPSHOT_OK || st == VM_SNAPSHOT_TOO_MANY) {
                for (size_t i = 0; i < count; i++) {
                    NSDate *when = [NSDate dateWithTimeIntervalSince1970:
                                        (NSTimeInterval)items[i].created_unix];
                    [self.snapshots addObject:@{
                        @"id":    [NSString stringWithUTF8String:items[i].id],
                        @"when":  when,
                        @"bytes": @(items[i].bytes),
                    }];
                }
            }
            free(items);
        }
    }
    [self.tableView reloadData];
}

/* Local time, not the id's UTC: the id exists to sort, the label exists to be
 * recognised, and a person recognises the time their own clock showed. */
- (NSString *)describeDate:(NSDate *)date {
    static NSDateFormatter *formatter;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        formatter = [[NSDateFormatter alloc] init];
        formatter.dateStyle = NSDateFormatterMediumStyle;
        formatter.timeStyle = NSDateFormatterShortStyle;
    });
    return [formatter stringFromDate:date];
}

- (NSString *)describeBytes:(unsigned long long)bytes {
    if (bytes == 0) return @"empty";
    if (bytes < 1024ULL * 1024ULL)
        return [NSString stringWithFormat:@"%llu KB", bytes / 1024ULL];
    double mb = (double)bytes / (1024.0 * 1024.0);
    if (mb < 1024.0) return [NSString stringWithFormat:@"%.0f MB", mb];
    return [NSString stringWithFormat:@"%.2f GB", mb / 1024.0];
}

#pragma mark - table

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    (void)tableView;
    return VMSnapshotSectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView
 numberOfRowsInSection:(NSInteger)section {
    (void)tableView;
    if (section == VMSnapshotSectionAction) return 1;
    return (NSInteger)self.snapshots.count;
}

- (NSString *)tableView:(UITableView *)tableView
titleForHeaderInSection:(NSInteger)section {
    (void)tableView;
    if (section == VMSnapshotSectionList && self.snapshots.count)
        return @"Saved states";
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView
titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    if (section == VMSnapshotSectionAction)
        return @"A snapshot saves the machine exactly as it is now, including "
               @"what is on the guest's disk, so you can come back to this "
               @"moment later.";
    if (section == VMSnapshotSectionList && self.snapshots.count == 0)
        return @"No snapshots yet.";
    /* The total is worth stating plainly: these live on the phone, and the
     * only place a person can see what they cost is here. */
    unsigned long long total = 0;
    for (NSDictionary *s in self.snapshots)
        total += [s[@"bytes"] unsignedLongLongValue];
    return [NSString stringWithFormat:@"%lu snapshot%@, %@ in total.",
            (unsigned long)self.snapshots.count,
            self.snapshots.count == 1 ? @"" : @"s",
            [self describeBytes:total]];
}

- (UITableViewCell *)tableView:(UITableView *)tableView
         cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    if (indexPath.section == VMSnapshotSectionAction) {
        UITableViewCell *cell =
            [tableView dequeueReusableCellWithIdentifier:@"action"];
        if (!cell)
            cell = [[UITableViewCell alloc]
                       initWithStyle:UITableViewCellStyleDefault
                     reuseIdentifier:@"action"];
        cell.textLabel.text = @"Take Snapshot";
        cell.textLabel.textColor = self.busy ? [UIColor systemGrayColor]
                                             : [UIColor systemBlueColor];
        cell.selectionStyle = self.busy ? UITableViewCellSelectionStyleNone
                                        : UITableViewCellSelectionStyleDefault;
        cell.accessoryType = UITableViewCellAccessoryNone;
        return cell;
    }

    UITableViewCell *cell =
        [tableView dequeueReusableCellWithIdentifier:@"snapshot"];
    if (!cell)
        cell = [[UITableViewCell alloc]
                   initWithStyle:UITableViewCellStyleSubtitle
                 reuseIdentifier:@"snapshot"];
    NSDictionary *s = self.snapshots[(NSUInteger)indexPath.row];
    cell.textLabel.text = [self describeDate:s[@"when"]];
    cell.detailTextLabel.text =
        [self describeBytes:[s[@"bytes"] unsignedLongLongValue]];
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    return cell;
}

- (void)tableView:(UITableView *)tableView
didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    if (self.busy) return;

    if (indexPath.section == VMSnapshotSectionAction) {
        [self takeSnapshot];
        return;
    }
    if ((NSUInteger)indexPath.row >= self.snapshots.count) return;
    NSDictionary *s = self.snapshots[(NSUInteger)indexPath.row];
    [self presentActionsFor:s
                   fromCell:[tableView cellForRowAtIndexPath:indexPath]];
}

#pragma mark - actions

- (void)presentActionsFor:(NSDictionary *)snapshot fromCell:(UITableViewCell *)cell {
    NSString *when = [self describeDate:snapshot[@"when"]];
    UIAlertController *sheet = [UIAlertController
        alertControllerWithTitle:when
                         message:[self describeBytes:
                                     [snapshot[@"bytes"] unsignedLongLongValue]]
                  preferredStyle:UIAlertControllerStyleActionSheet];

    NSString *blocked = nil;
    if ([self.delegate respondsToSelector:@selector(snapshotListOpenUnavailableReason:)])
        blocked = [self.delegate snapshotListOpenUnavailableReason:self];

    UIAlertAction *open = [UIAlertAction
        actionWithTitle:@"Open"
                  style:UIAlertActionStyleDefault
                handler:^(UIAlertAction *action) {
                    (void)action;
                    [self openSnapshot:snapshot[@"id"]];
                }];
    /* Disabled with the reason in the sheet's message rather than hidden: a
     * missing button reads as a bug, a disabled one with a sentence reads as
     * a decision. */
    if (blocked.length) {
        open.enabled = NO;
        sheet.message = blocked;
    }
    [sheet addAction:open];

    [sheet addAction:[UIAlertAction
        actionWithTitle:@"Delete"
                  style:UIAlertActionStyleDestructive
                handler:^(UIAlertAction *action) {
                    (void)action;
                    [self confirmDelete:snapshot];
                }]];
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];

    /* iPad presents an action sheet as a popover and throws without an anchor. */
    sheet.popoverPresentationController.sourceView = cell ?: self.view;
    sheet.popoverPresentationController.sourceRect =
        cell ? cell.bounds : CGRectMake(CGRectGetMidX(self.view.bounds),
                                        CGRectGetMidY(self.view.bounds), 1, 1);
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)confirmDelete:(NSDictionary *)snapshot {
    NSString *when = [self describeDate:snapshot[@"when"]];
    UIAlertController *ask = [UIAlertController
        alertControllerWithTitle:@"Delete this snapshot?"
                         message:[NSString stringWithFormat:
                                     @"%@ will be removed. This cannot be undone.",
                                     when]
                  preferredStyle:UIAlertControllerStyleAlert];
    [ask addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                            style:UIAlertActionStyleCancel
                                          handler:nil]];
    [ask addAction:[UIAlertAction
        actionWithTitle:@"Delete"
                  style:UIAlertActionStyleDestructive
                handler:^(UIAlertAction *action) {
                    (void)action;
                    [self deleteSnapshot:snapshot[@"id"]];
                }]];
    [self presentViewController:ask animated:YES completion:nil];
}

- (void)deleteSnapshot:(NSString *)snapshotId {
    char path[VM_FW_BOOT_PATH_CAPACITY];
    if (vm_snapshot_path(self.snapshotsDirectory.fileSystemRepresentation,
                         snapshotId.UTF8String, false,
                         path, sizeof path) != VM_SNAPSHOT_OK) {
        [self report:@"Could not delete" message:@"That snapshot's name is not "
                                                 @"one this app wrote."];
        return;
    }
    NSError *error = nil;
    NSString *p = [NSString stringWithUTF8String:path];
    if (![[NSFileManager defaultManager] removeItemAtPath:p error:&error]) {
        [self report:@"Could not delete"
             message:error.localizedDescription ?: @"The files are still there."];
    }
    [self reload];
}

- (void)takeSnapshot {
    if (![self.delegate respondsToSelector:
             @selector(snapshotList:takeSnapshotWithProgress:completion:)]) {
        /* Ask the delegate why. Guessing here is what produced a running
         * machine being told it was not running. */
        NSString *why = nil;
        if ([self.delegate respondsToSelector:
                 @selector(snapshotListTakeUnavailableReason:)])
            why = [self.delegate snapshotListTakeUnavailableReason:self];
        [self report:@"Not available"
             message:why.length ? why
                                : @"Saving is not wired up in this build."];
        return;
    }
    [self setBusy:YES stage:@"Saving the machine"];
    __weak VMSnapshotListViewController *weakSelf = self;
    [self.delegate snapshotList:self
       takeSnapshotWithProgress:^(double fraction, NSString *stage) {
           [weakSelf updateProgress:fraction stage:stage];
       }
                     completion:^(BOOL ok, NSString *message) {
           VMSnapshotListViewController *strongSelf = weakSelf;
           if (!strongSelf) return;
           [strongSelf setBusy:NO stage:nil];
           if (!ok) [strongSelf report:@"Could not save" message:message];
           [strongSelf reload];
       }];
}

- (void)openSnapshot:(NSString *)snapshotId {
    if (![self.delegate respondsToSelector:
             @selector(snapshotList:openSnapshotId:progress:completion:)]) {
        [self report:@"Not available" message:@"Nothing here can open a snapshot yet."];
        return;
    }
    [self setBusy:YES stage:@"Restoring the machine"];
    __weak VMSnapshotListViewController *weakSelf = self;
    [self.delegate snapshotList:self
                 openSnapshotId:snapshotId
                       progress:^(double fraction, NSString *stage) {
           [weakSelf updateProgress:fraction stage:stage];
       }
                     completion:^(BOOL ok, NSString *message) {
           VMSnapshotListViewController *strongSelf = weakSelf;
           if (!strongSelf) return;
           [strongSelf setBusy:NO stage:nil];
           if (!ok) { [strongSelf report:@"Could not open" message:message]; return; }
           /* On success the machine is now somewhere else entirely, so this
            * screen has nothing left to say; go back to it. */
           [strongSelf.navigationController popToRootViewControllerAnimated:YES];
       }];
}

- (void)report:(NSString *)title message:(NSString *)message {
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:title
                         message:message.length ? message : @"No further detail."
                  preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK"
                                          style:UIAlertActionStyleDefault
                                        handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

#pragma mark - progress

- (void)buildProgressOverlay {
    self.progressScrim = [[UIView alloc] initWithFrame:CGRectZero];
    self.progressScrim.backgroundColor =
        [UIColor colorWithWhite:0.0 alpha:0.55];
    self.progressScrim.hidden = YES;
    self.progressScrim.translatesAutoresizingMaskIntoConstraints = NO;

    self.progressLabel = [[UILabel alloc] initWithFrame:CGRectZero];
    self.progressLabel.textColor = [UIColor whiteColor];
    self.progressLabel.textAlignment = NSTextAlignmentCenter;
    self.progressLabel.numberOfLines = 0;
    self.progressLabel.font = [UIFont systemFontOfSize:15.0];
    self.progressLabel.translatesAutoresizingMaskIntoConstraints = NO;

    self.progressBar = [[UIProgressView alloc]
        initWithProgressViewStyle:UIProgressViewStyleDefault];
    self.progressBar.translatesAutoresizingMaskIntoConstraints = NO;

    [self.progressScrim addSubview:self.progressLabel];
    [self.progressScrim addSubview:self.progressBar];

    /* On the navigation controller's view, not the table's: the table scrolls
     * and a scrim that scrolls with it stops covering what it is blocking. */
    UIView *host = self.navigationController.view ?: self.view;
    [host addSubview:self.progressScrim];

    [NSLayoutConstraint activateConstraints:@[
        [self.progressScrim.topAnchor constraintEqualToAnchor:host.topAnchor],
        [self.progressScrim.bottomAnchor constraintEqualToAnchor:host.bottomAnchor],
        [self.progressScrim.leadingAnchor constraintEqualToAnchor:host.leadingAnchor],
        [self.progressScrim.trailingAnchor constraintEqualToAnchor:host.trailingAnchor],

        [self.progressLabel.centerXAnchor
            constraintEqualToAnchor:self.progressScrim.centerXAnchor],
        [self.progressLabel.centerYAnchor
            constraintEqualToAnchor:self.progressScrim.centerYAnchor constant:-24.0],
        [self.progressLabel.leadingAnchor
            constraintEqualToAnchor:self.progressScrim.leadingAnchor constant:32.0],
        [self.progressLabel.trailingAnchor
            constraintEqualToAnchor:self.progressScrim.trailingAnchor constant:-32.0],

        [self.progressBar.topAnchor
            constraintEqualToAnchor:self.progressLabel.bottomAnchor constant:16.0],
        [self.progressBar.leadingAnchor
            constraintEqualToAnchor:self.progressScrim.leadingAnchor constant:48.0],
        [self.progressBar.trailingAnchor
            constraintEqualToAnchor:self.progressScrim.trailingAnchor constant:-48.0],
    ]];
}

- (void)setBusy:(BOOL)busy stage:(NSString *)stage {
    self.busy = busy;
    self.progressScrim.hidden = !busy;
    if (busy) {
        [self.progressScrim.superview bringSubviewToFront:self.progressScrim];
        self.progressLabel.text = stage ?: @"Working";
        [self.progressBar setProgress:0.0 animated:NO];
    }
    /* Both directions: leaving this screen mid-save would run the completion
     * against a controller the user has already dismissed. */
    self.navigationItem.hidesBackButton = busy;
    [self.tableView reloadData];
}

- (void)updateProgress:(double)fraction stage:(NSString *)stage {
    if (fraction < 0.0) {
        /* Total unknown: say what is happening and do not draw a bar that
         * pretends to know how far along it is. */
        self.progressLabel.text = stage ?: @"Working";
        self.progressBar.hidden = YES;
        return;
    }
    self.progressBar.hidden = NO;
    if (fraction > 1.0) fraction = 1.0;
    self.progressLabel.text = stage.length
        ? [NSString stringWithFormat:@"%@\n%.0f%%", stage, fraction * 100.0]
        : [NSString stringWithFormat:@"%.0f%%", fraction * 100.0];
    [self.progressBar setProgress:(float)fraction animated:YES];
}

@end
