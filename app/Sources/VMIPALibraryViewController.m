// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMIPALibraryViewController.h"
#import "VMIPALibrary.h"

@interface VMIPALibraryViewController () <UIDocumentPickerDelegate>
- (void)refreshLibrary;
- (void)importTapped;
- (void)installTapped:(UIButton *)button;
@end

@implementation VMIPALibraryViewController {
    NSArray<NSURL *> *_files;
    NSString *_machineName;
    NSString *_status;
    BOOL _importing;
}
- (instancetype)initWithMachineName:(NSString *)name {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (!self) return nil;
    _machineName = [name copy];
    _files = @[];
    self.title = @"IPA Library";
    return self;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Import"
        style:UIBarButtonItemStylePlain target:self action:@selector(importTapped)];
    self.navigationItem.rightBarButtonItem.accessibilityIdentifier = @"s5lbox.ipa-library.import";
    self.tableView.rowHeight = UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight = 80;
    self.refreshControl = [[UIRefreshControl alloc] init];
    [self.refreshControl addTarget:self action:@selector(refreshLibrary) forControlEvents:UIControlEventValueChanged];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(refreshLibrary)
        name:UIApplicationDidBecomeActiveNotification object:nil];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self refreshLibrary];
}
- (void)dealloc { [[NSNotificationCenter defaultCenter] removeObserver:self]; }
- (void)refreshLibrary {
    if (_importing) { [self.refreshControl endRefreshing]; return; }
    NSError *error = nil;
    _files = [VMIPALibrary filesWithError:&error] ?: @[];
    if (error) _status = error.localizedDescription;
    [self.refreshControl endRefreshing];
    [self.tableView reloadData];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { return 2; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return section == 0 ? 1 : MAX((NSUInteger)1, _files.count);
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    return section == 1 ? [NSString stringWithFormat:@"Install in %@", _machineName] : nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    cell.textLabel.adjustsFontForContentSizeCategory = cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.numberOfLines = cell.detailTextLabel.numberOfLines = 0;
    cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    if (path.section == 0) {
        cell.textLabel.text = @"Your IPA collection";
        NSString *hint = @"Add files to Files → On My iPhone → S5LBox → IPAs, or tap Import. Install each app when you want; adding files never installs them automatically.";
        cell.detailTextLabel.text = _status.length ? [NSString stringWithFormat:@"%@\n\n%@", hint, _status] : hint;
        if (_importing) {
            UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
            [spinner startAnimating]; cell.accessoryView = spinner;
        }
    } else if (!_files.count) {
        cell.textLabel.text = @"No IPAs yet";
        cell.detailTextLabel.text = @"Import an IPA, or copy one into the IPAs folder in Files. Pull down to refresh.";
    } else {
        NSURL *url = _files[(NSUInteger)path.row];
        cell.textLabel.text = url.lastPathComponent.stringByDeletingPathExtension;
        NSDictionary *attributes = [NSFileManager.defaultManager attributesOfItemAtPath:url.path error:NULL];
        cell.detailTextLabel.text = [NSByteCountFormatter stringFromByteCount:(long long)[attributes fileSize] countStyle:NSByteCountFormatterCountStyleFile];
        cell.imageView.image = [UIImage systemImageNamed:@"app.dashed"];
        cell.imageView.tintColor = UIColor.secondaryLabelColor;
        UIButton *install = [UIButton buttonWithType:UIButtonTypeSystem];
        [install setTitle:@"Install" forState:UIControlStateNormal];
        install.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
        install.titleLabel.adjustsFontForContentSizeCategory = YES;
        install.contentEdgeInsets = UIEdgeInsetsMake(10, 12, 10, 12);
        install.backgroundColor = UIColor.tertiarySystemFillColor;
        install.layer.cornerRadius = 16;
        [install sizeToFit];
        install.tag = path.row;
        install.enabled = !_importing;
        install.accessibilityLabel = [@"Install " stringByAppendingString:url.lastPathComponent.stringByDeletingPathExtension];
        [install addTarget:self action:@selector(installTapped:) forControlEvents:UIControlEventTouchUpInside];
        cell.accessoryView = install;
    }
    return cell;
}
- (void)installTapped:(UIButton *)button {
    if (_importing || button.tag < 0 || (NSUInteger)button.tag >= _files.count) return;
    if (self.selectionHandler) self.selectionHandler(_files[(NSUInteger)button.tag]);
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    if (!_importing && path.section == 1 && (NSUInteger)path.row < _files.count && self.selectionHandler)
        self.selectionHandler(_files[(NSUInteger)path.row]);
}
- (void)importTapped {
    if (_importing) return;
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initWithDocumentTypes:@[@"public.data"] inMode:UIDocumentPickerModeOpen];
    picker.allowsMultipleSelection = NO;
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentAtURL:(NSURL *)url {
    [self documentPicker:controller didPickDocumentsAtURLs:@[url]];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject;
    if (!url || _importing) return;
    BOOL scoped = [url startAccessingSecurityScopedResource];
    _importing = YES;
    _status = [@"Importing " stringByAppendingString:url.lastPathComponent];
    self.navigationItem.rightBarButtonItem.enabled = NO;
    [self.tableView reloadData];
    [controller dismissViewControllerAnimated:YES completion:nil];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSURL *saved = [VMIPALibrary importURL:url error:&error];
        if (scoped) [url stopAccessingSecurityScopedResource];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_importing = NO;
            self->_status = saved ? [NSString stringWithFormat:@"Added %@. Tap Install when you’re ready.", saved.lastPathComponent]
                : error.localizedDescription ?: @"Import did not finish. Try again.";
            self.navigationItem.rightBarButtonItem.enabled = YES;
            [self refreshLibrary];
        });
    });
}
@end
