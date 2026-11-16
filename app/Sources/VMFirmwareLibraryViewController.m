// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMFirmwareLibraryViewController.h"
#import "VMFirmwareImportViewController.h"
#import "VMFirmwareImporter.h"
#import "VMFirmwareStore.h"
#import "VMSettings.h"

@implementation VMFirmwareLibraryViewController {
    NSArray<NSDictionary<NSString *, id> *> *_firmwares;
}
- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }
- (NSString *)firmwareRoot {
    return [VMSettings.sharedSettings.documentsDirectory stringByAppendingPathComponent:@"firmware"];
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"Firmware";
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc]
        initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(importTapped)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel = @"Import firmware";
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(reloadFirmware)
        name:VMFirmwareImportDidChangeNotification object:nil];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self reloadFirmware];
}
- (void)dealloc { [NSNotificationCenter.defaultCenter removeObserver:self]; }
- (void)reloadFirmware {
    _firmwares = [VMFirmwareStore firmwaresInRoot:self.firmwareRoot];
    [self.tableView reloadData];
    UILabel *empty = nil;
    if (!_firmwares.count) {
        empty = [UILabel new];
        empty.text = @"Import an IPSW to add firmware.";
        empty.textAlignment = NSTextAlignmentCenter;
        empty.textColor = UIColor.secondaryLabelColor;
        empty.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
        empty.adjustsFontForContentSizeCategory = YES;
        empty.numberOfLines = 0;
    }
    self.tableView.backgroundView = empty;
}
- (void)importTapped {
    [self.navigationController pushViewController:[VMFirmwareImportViewController new] animated:YES];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _firmwares.count;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _firmwares.count ? @"Selected by default for new machines." : nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"firmware"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"firmware"];
    NSDictionary *entry = _firmwares[indexPath.row];
    cell.textLabel.text = entry[@"name"];
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · %@ (%@)",
        entry[@"device"], entry[@"version"], entry[@"build"]];
    cell.detailTextLabel.numberOfLines = 0;
    cell.imageView.image = [UIImage systemImageNamed:@"internaldrive"];
    cell.accessoryType = [entry[@"default"] boolValue] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    cell.accessibilityIdentifier = [@"s5lbox.firmware." stringByAppendingString:entry[@"id"]];
    return cell;
}
- (void)showError:(NSError *)error {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Firmware"
        message:error.localizedDescription preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    NSError *error = nil;
    if (![VMFirmwareStore selectFirmware:_firmwares[indexPath.row][@"id"] inRoot:self.firmwareRoot error:&error])
        [self showError:error];
    [self reloadFirmware];
}
- (UIContextMenuConfiguration *)tableView:(UITableView *)tableView
    contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)indexPath point:(CGPoint)point {
    (void)tableView; (void)point;
    NSDictionary *entry = _firmwares[indexPath.row];
    __weak typeof(self) weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil
        actionProvider:^UIMenu *(NSArray<UIMenuElement *> *suggested) {
        (void)suggested;
        UIAction *remove = [UIAction actionWithTitle:@"Remove" image:[UIImage systemImageNamed:@"trash"]
            identifier:nil handler:^(__unused UIAction *action) { [weakSelf confirmRemoval:entry]; }];
        remove.attributes = UIMenuElementAttributesDestructive |
            ([entry[@"removable"] boolValue] ? 0 : UIMenuElementAttributesDisabled);
        NSUInteger count = [entry[@"machines"] unsignedIntegerValue];
        NSString *title = count
            ? [NSString stringWithFormat:count == 1 ? @"Used by %@ machine" : @"Used by %@ machines", entry[@"machines"]]
            : ([entry[@"default"] boolValue] ? @"Default firmware" : @"");
        return [UIMenu menuWithTitle:title children:@[remove]];
    }];
}
- (void)confirmRemoval:(NSDictionary *)entry {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Remove firmware?"
        message:entry[@"name"] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Remove" style:UIAlertActionStyleDestructive
        handler:^(__unused UIAlertAction *action) {
        NSError *error = nil;
        if (![VMFirmwareStore removeFirmware:entry[@"id"] inRoot:self.firmwareRoot error:&error])
            [self showError:error];
        [self reloadFirmware];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}
@end
