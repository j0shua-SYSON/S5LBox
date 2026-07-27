//
//  S5LBox — VMInstanceListViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMInstanceListViewController.h"

#import "EmulatorViewController.h"
#import "VMInstanceStore.h"
#import "VMInstances.h"

static NSString *const kCell = @"machine";

@implementation VMInstanceListViewController

- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleGrouped];
    if (!self) return nil;
    self.title = @"Machines";
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd
                                                      target:self
                                                      action:@selector(addTapped)];
    self.navigationItem.leftBarButtonItem = self.editButtonItem;

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
        handler(name);
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

#pragma mark - Actions

- (void)addTapped {
    [self promptWithTitle:@"New Machine"
                     text:@""
                   accept:@"Create"
                  handler:^(NSString *name) {
        NSError *err = nil;
        if (![[VMInstanceStore sharedStore] createInstanceNamed:name error:&err])
            [self showError:err doing:@"Could not create the machine"];
    }];
}

- (void)renameAtIndex:(NSUInteger)index {
    NSDictionary *row = [[VMInstanceStore sharedStore] instanceAtIndex:index];
    if (!row) return;
    [self promptWithTitle:@"Rename"
                     text:row[@"name"]
                   accept:@"Rename"
                  handler:^(NSString *name) {
        NSError *err = nil;
        if (![[VMInstanceStore sharedStore] renameInstanceAtIndex:index
                                                                to:name
                                                             error:&err])
            [self showError:err doing:@"Could not rename the machine"];
    }];
}

- (void)duplicateAtIndex:(NSUInteger)index {
    NSError *err = nil;
    if (![[VMInstanceStore sharedStore] duplicateInstanceAtIndex:index error:&err])
        [self showError:err doing:@"Could not duplicate the machine"];
}

- (void)confirmDeleteAtIndex:(NSUInteger)index {
    NSDictionary *row = [[VMInstanceStore sharedStore] instanceAtIndex:index];
    if (!row) return;
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
        if (![[VMInstanceStore sharedStore] deleteInstanceAtIndex:index error:&err])
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

/* The standing caveat, on the first screen rather than buried in settings. */
- (NSString *)tableView:(UITableView *)tableView
titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return @"Each machine keeps its own options and its own files. None of "
           @"them boots Apple's firmware yet — opening one runs the built-in "
           @"test guest, which exercises the processor, the serial port and "
           @"the screen. Only one machine runs at a time.";
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
    cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;

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
    return cell;
}

- (void)tableView:(UITableView *)tableView
didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];

    NSDictionary *row =
        [[VMInstanceStore sharedStore] instanceAtIndex:(NSUInteger)indexPath.row];
    if (!row) return;

    [[VMInstanceStore sharedStore] noteOpenedInstanceWithID:row[@"id"]];

    EmulatorViewController *vc = [[EmulatorViewController alloc] init];
    vc.title = row[@"name"];
    [self.navigationController pushViewController:vc animated:YES];
}

/* Swipe actions rather than only an edit-mode delete: rename and duplicate are
 * the two things people reach for most, and burying them costs more than the
 * few lines this takes. */
- (UISwipeActionsConfiguration *)tableView:(UITableView *)tableView
    trailingSwipeActionsConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath {
    (void)tableView;
    NSUInteger index = (NSUInteger)indexPath.row;

    UIContextualAction *del = [UIContextualAction
        contextualActionWithStyle:UIContextualActionStyleDestructive
                            title:@"Delete"
                          handler:^(UIContextualAction *a, UIView *v,
                                    void (^done)(BOOL)) {
        (void)a; (void)v;
        [self confirmDeleteAtIndex:index];
        done(NO);       /* the alert decides; do not animate the row away yet */
    }];

    UIContextualAction *dup = [UIContextualAction
        contextualActionWithStyle:UIContextualActionStyleNormal
                            title:@"Duplicate"
                          handler:^(UIContextualAction *a, UIView *v,
                                    void (^done)(BOOL)) {
        (void)a; (void)v;
        [self duplicateAtIndex:index];
        done(YES);
    }];

    UIContextualAction *ren = [UIContextualAction
        contextualActionWithStyle:UIContextualActionStyleNormal
                            title:@"Rename"
                          handler:^(UIContextualAction *a, UIView *v,
                                    void (^done)(BOOL)) {
        (void)a; (void)v;
        [self renameAtIndex:index];
        done(YES);
    }];

    /*
     * Three actions need three colours. UIContextualActionStyleNormal has no
     * colour of its own, so Duplicate and Rename both came out the same grey
     * and were told apart only by reading them — which is the one thing a
     * swipe action is meant to avoid, since the row is under your thumb.
     *
     * Delete keeps the red its destructive style gives it. Blue sits between
     * red and orange in the swipe order, so the two warm colours are never
     * adjacent — red beside orange is the pair most likely to be misread at a
     * glance, and it is the pair where the mistake is unrecoverable.
     *
     * The colours are an aid, not the signal: every action still carries its
     * own word, which is what a colour-blind reader and VoiceOver both use.
     */
    dup.backgroundColor = [UIColor systemBlueColor];
    ren.backgroundColor = [UIColor systemOrangeColor];

    return [UISwipeActionsConfiguration
        configurationWithActions:@[ del, dup, ren ]];
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
