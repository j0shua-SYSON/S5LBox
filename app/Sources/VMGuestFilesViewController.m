// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMGuestFilesViewController.h"
#import "VMGuestFiles.h"
#import "VMDocumentPicker.h"
#import "VMImportedDocument.h"
#import "VMUSBAFC.h"
#import <QuickLook/QuickLook.h>

@interface VMGuestFilesViewController () <UIDocumentPickerDelegate, QLPreviewControllerDataSource>
@end
@implementation VMGuestFilesViewController {
    VMUSBTransport *_transport;
    NSString *_identifier, *_path, *_message;
    BOOL _root, _busy, _leaving;
    VMGuestFiles *_files;
    NSArray<VMGuestFile *> *_items;
    NSURL *_exportDirectory, *_preview;
    dispatch_queue_t _worker;
    UIProgressView *_progress;
    UISegmentedControl *_scope;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (self) { _transport = transport; _identifier = [identifier copy]; _path = @"/";
        _worker = dispatch_queue_create("com.j0shua.s5lbox.files", DISPATCH_QUEUE_SERIAL);
        // Capture only previous-process exports before this process can create
        // one. Never prune active previews or unrelated cache directories.
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            NSURL *cache = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
            NSArray<NSURL *> *existing = [NSFileManager.defaultManager contentsOfDirectoryAtURL:cache
                includingPropertiesForKeys:@[NSURLIsDirectoryKey, NSURLIsSymbolicLinkKey] options:0 error:NULL];
            NSMutableArray<NSURL *> *stale = [NSMutableArray array];
            for (NSURL *url in existing) {
                NSString *name = url.lastPathComponent;
                if (![name hasPrefix:@"GuestFiles-"] || ![[NSUUID alloc] initWithUUIDString:[name substringFromIndex:11]]) continue;
                NSNumber *directory = nil, *link = nil;
                if ([url getResourceValue:&directory forKey:NSURLIsDirectoryKey error:NULL] &&
                    [url getResourceValue:&link forKey:NSURLIsSymbolicLinkKey error:NULL] && directory.boolValue && !link.boolValue) [stale addObject:url];
            }
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                for (NSURL *url in stale) [NSFileManager.defaultManager removeItemAtURL:url error:NULL];
            });
        });
    }
    return self;
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = [_path isEqual:@"/"] ? @"Files" : _path.lastPathComponent;
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.tableView.rowHeight = UITableViewAutomaticDimension; self.tableView.estimatedRowHeight = 60;
    self.tableView.accessibilityIdentifier = @"s5lbox.guest-files";
    self.refreshControl = [UIRefreshControl new]; [self.refreshControl addTarget:self action:@selector(refresh) forControlEvents:UIControlEventValueChanged];
    _progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _progress.frame = CGRectMake(20, 0, self.view.bounds.size.width - 40, 3);
    _progress.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    _progress.hidden = YES; self.tableView.tableHeaderView = _progress;
    if ([_path isEqual:@"/"]) {
        _scope = [[UISegmentedControl alloc] initWithItems:@[@"Media", @"Filesystem"]];
        _scope.selectedSegmentIndex = _root ? 1 : 0;
        [_scope addTarget:self action:@selector(scopeChanged) forControlEvents:UIControlEventValueChanged];
        _scope.accessibilityLabel = @"Guest filesystem access"; self.navigationItem.titleView = _scope;
    }
    [self configureFiles]; [self refresh];
}
- (void)configureFiles {
    _files = [[VMGuestFiles alloc] initWithTransport:_transport instanceID:_identifier root:_root];
    __weak VMGuestFilesViewController *weakSelf = self;
    _files.progress = ^(double fraction) { dispatch_async(dispatch_get_main_queue(), ^{
        VMGuestFilesViewController *self = weakSelf; if (!self || !self->_busy) return;
        self->_progress.hidden = NO; self->_progress.progress = fraction;
        self.navigationItem.prompt = [NSString stringWithFormat:@"Transferring · %.0f%%", fraction * 100];
    }); };
}
- (void)dealloc {
    _files.canceled = YES;
    if (_exportDirectory) [NSFileManager.defaultManager removeItemAtURL:_exportDirectory error:NULL];
}
- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    _leaving = self.isMovingFromParentViewController || self.navigationController.isBeingDismissed;
    if (_leaving) _files.canceled = YES;
}
- (void)viewWillAppear:(BOOL)animated { [super viewWillAppear:animated]; _leaving = NO; }
- (void)clearExport {
    if (_exportDirectory) [NSFileManager.defaultManager removeItemAtURL:_exportDirectory error:NULL];
    _exportDirectory = nil; _preview = nil;
}
- (void)viewDidDisappear:(BOOL)animated {
    [super viewDidDisappear:animated]; if (_leaving && !_busy) [self clearExport];
}
- (void)buttons {
    _scope.enabled = !_busy;
    self.navigationItem.rightBarButtonItem = _busy
        ? [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel target:self action:@selector(cancel)]
        : [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(add)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel = _busy ? @"Cancel file operation" : @"Add file or folder";
    self.navigationItem.rightBarButtonItem.enabled = _busy || _items != nil;
}
- (void)cancel { _files.canceled = YES; _message = @"Canceling…"; [self.tableView reloadData]; }
- (void)scopeChanged { if (_busy) return; _root = _scope.selectedSegmentIndex == 1; _items = nil; [self configureFiles]; [self refresh]; }
- (void)run:(id (^)(VMGuestFiles *, NSError **))work completion:(void (^)(id))completion {
    if (_busy) return;
    _busy = YES; _files.canceled = NO; _message = @"Connecting…"; [self buttons]; [self.tableView reloadData];
    self.navigationItem.prompt = @"Connecting…";
    VMGuestFiles *files = _files;
    dispatch_async(_worker, ^{
        NSError *error = nil; id result = work(files, &error);
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_busy = NO; self->_progress.hidden = YES; [self.refreshControl endRefreshing]; [self buttons];
            self.navigationItem.prompt = nil;
            self->_message = error.localizedDescription ?: (files.canceled ? @"Canceled. Pull to refresh." : nil);
            if (result && !files.canceled && !error) completion(result);
            [self buttons];
            [self.tableView reloadData];
            if (self->_leaving) [self clearExport];
            else if (error && !files.canceled && self->_items.count && self.view.window &&
                self.navigationController.topViewController == self && !self.presentedViewController) {
                UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Couldn’t Complete"
                    message:error.localizedDescription preferredStyle:UIAlertControllerStyleAlert];
                [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
                [self presentViewController:alert animated:YES completion:nil];
            }
        });
    });
}
- (void)refresh {
    if (_busy) { [self.refreshControl endRefreshing]; return; }
    NSString *path = _path;
    [self run:^id(VMGuestFiles *files, NSError **error) { return [files list:path error:error]; }
        completion:^(id items) { self->_items = items; }];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section; return _items.count ?: 1;
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _root ? _path : [@"/var/mobile/Media" stringByAppendingString:[_path isEqual:@"/"] ? @"" : _path];
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _message ?: (_root && [_path isEqual:@"/"] ? @"Full guest filesystem access. Changing system files can prevent this machine from booting." : nil);
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)index {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"file"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"file"];
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    cell.textLabel.adjustsFontForContentSizeCategory = cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.numberOfLines = 2; cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    cell.accessoryType = UITableViewCellAccessoryNone; cell.imageView.image = nil; cell.accessoryView = nil;
    cell.detailTextLabel.text = nil;
    if (!_items.count) {
        cell.textLabel.text = _busy ? @"Loading files…" : _message ? @"Try Again" : @"Empty Folder";
        if (_busy) { UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium]; [spinner startAnimating]; cell.accessoryView = spinner; }
        return cell;
    }
    VMGuestFile *file = _items[index.row]; BOOL folder = [file.kind isEqual:@"S_IFDIR"], link = [file.kind isEqual:@"S_IFLNK"];
    cell.textLabel.text = file.name;
    cell.detailTextLabel.text = folder ? @"Folder" : link ? @"Symbolic link" : [file.kind isEqual:@"S_IFREG"]
        ? [NSByteCountFormatter stringFromByteCount:(long long)MIN(file.size, (uint64_t)LLONG_MAX) countStyle:NSByteCountFormatterCountStyleFile] : @"Special file";
    cell.imageView.image = [UIImage systemImageNamed:folder ? @"folder.fill" : link ? @"link" : @"doc"];
    cell.accessoryType = folder ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)index {
    [tableView deselectRowAtIndexPath:index animated:YES]; if (_busy) return;
    if (!_items.count) { [self refresh]; return; }
    VMGuestFile *file = _items[index.row];
    if ([file.kind isEqual:@"S_IFDIR"]) {
        VMGuestFilesViewController *child = [[VMGuestFilesViewController alloc] initWithTransport:_transport instanceID:_identifier];
        child->_path = file.path; child->_root = _root; [self.navigationController pushViewController:child animated:YES];
    } else if ([file.kind isEqual:@"S_IFREG"]) [self exportFile:file preview:YES];
}
- (void)showNamePrompt:(NSString *)title value:(NSString *)value action:(void (^)(NSString *))action {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *field) { field.text = value; field.placeholder = @"Name"; field.autocapitalizationType = UITextAutocapitalizationTypeNone; field.autocorrectionType = UITextAutocorrectionTypeNo; }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
        NSString *name = alert.textFields.firstObject.text;
        if (![VMUSBAFC validFilename:name]) { self->_message = @"Enter a name without slashes or control characters."; [self.tableView reloadData]; return; }
        action(name);
    }]]; [self presentViewController:alert animated:YES completion:nil];
}
- (void)changed:(id)result { if ([result boolValue]) dispatch_async(dispatch_get_main_queue(), ^{ [self refresh]; }); }
- (void)add {
    if (_busy) return;
    UIAlertController *menu = [UIAlertController alertControllerWithTitle:nil message:nil preferredStyle:UIAlertControllerStyleActionSheet];
    [menu addAction:[UIAlertAction actionWithTitle:@"Import File" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
        UIDocumentPickerViewController *picker = VMCreateImportPicker();
        picker.delegate = self; picker.allowsMultipleSelection = NO; [self presentViewController:picker animated:YES completion:nil];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"New Folder" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
        [self showNamePrompt:@"New Folder" value:nil action:^(NSString *name) {
            NSString *path = [self->_path stringByAppendingPathComponent:name];
            [self run:^id(VMGuestFiles *files, NSError **error) { return @([files createDirectory:path error:error]); } completion:^(id result) { [self changed:result]; }];
        }];
    }]];
    [menu addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    menu.popoverPresentationController.barButtonItem = self.navigationItem.rightBarButtonItem;
    [self presentViewController:menu animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject; if (!url || _busy) return; NSString *path = _path;
    VMImportedDocument *document = [[VMImportedDocument alloc] initWithPickerURL:url];
    [controller dismissViewControllerAnimated:YES completion:nil];
    [self run:^id(VMGuestFiles *files, NSError **error) {
        return @([files upload:document.url directory:path error:error]);
    } completion:^(id result) { [self changed:result]; }];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentAtURL:(NSURL *)url {
    [self documentPicker:controller didPickDocumentsAtURLs:url ? @[url] : @[]];
}
- (UIContextMenuConfiguration *)tableView:(UITableView *)tableView contextMenuConfigurationForRowAtIndexPath:(NSIndexPath *)index point:(CGPoint)point {
    (void)tableView; (void)point; if (_busy || !_items.count) return nil; VMGuestFile *file = _items[index.row];
    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil actionProvider:^UIMenu *(__unused NSArray<UIMenuElement *> *suggested) {
        NSMutableArray *actions = [NSMutableArray array];
        if ([file.kind isEqual:@"S_IFREG"]) [actions addObject:[UIAction actionWithTitle:@"Export" image:[UIImage systemImageNamed:@"square.and.arrow.up"] identifier:nil handler:^(__unused UIAction *a) { [self exportFile:file preview:NO]; }]];
        [actions addObject:[UIAction actionWithTitle:@"Rename" image:[UIImage systemImageNamed:@"pencil"] identifier:nil handler:^(__unused UIAction *a) {
            [self showNamePrompt:@"Rename" value:file.name action:^(NSString *name) {
                [self run:^id(VMGuestFiles *files, NSError **error) { return @([files rename:file name:name error:error]); } completion:^(id result) { [self changed:result]; }];
            }];
        }]];
        UIAction *remove = [UIAction actionWithTitle:@"Delete" image:[UIImage systemImageNamed:@"trash"] identifier:nil handler:^(__unused UIAction *a) {
            UIAlertController *alert = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"Delete “%@”?", file.name]
                message:[NSString stringWithFormat:@"%@\nThis cannot be undone. Folders must be empty.", file.path] preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
            [alert addAction:[UIAlertAction actionWithTitle:@"Delete" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *b) {
                [self run:^id(VMGuestFiles *files, NSError **error) { return @([files remove:file error:error]); } completion:^(id result) { [self changed:result]; }];
            }]]; [self presentViewController:alert animated:YES completion:nil];
        }]; remove.attributes = UIMenuElementAttributesDestructive; [actions addObject:remove];
        return [UIMenu menuWithTitle:file.name children:actions];
    }];
}
- (void)exportFile:(VMGuestFile *)file preview:(BOOL)preview {
    if (_busy) return;
    if (_exportDirectory) [NSFileManager.defaultManager removeItemAtURL:_exportDirectory error:NULL];
    NSURL *base = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
    _exportDirectory = [base URLByAppendingPathComponent:[@"GuestFiles-" stringByAppendingString:NSUUID.UUID.UUIDString] isDirectory:YES];
    NSError *error = nil;
    if (![NSFileManager.defaultManager createDirectoryAtURL:_exportDirectory withIntermediateDirectories:YES attributes:nil error:&error]) {
        _message = error.localizedDescription; [self.tableView reloadData]; return;
    }
    NSURL *directory = _exportDirectory;
    [self run:^id(VMGuestFiles *files, NSError **failure) { return [files download:file intoDirectory:directory error:failure]; } completion:^(NSURL *url) {
        if (!self.view.window || self.navigationController.topViewController != self) return;
        if (preview && [QLPreviewController canPreviewItem:url]) {
            self->_preview = url; QLPreviewController *viewer = [QLPreviewController new]; viewer.dataSource = self;
            [self.navigationController pushViewController:viewer animated:YES];
        } else {
            UIActivityViewController *share = [[UIActivityViewController alloc] initWithActivityItems:@[url] applicationActivities:nil];
            share.popoverPresentationController.sourceView = self.view;
            share.popoverPresentationController.sourceRect = CGRectMake(self.view.bounds.size.width / 2, self.view.bounds.size.height / 2, 1, 1);
            [self presentViewController:share animated:YES completion:nil];
        }
    }];
}
- (NSInteger)numberOfPreviewItemsInPreviewController:(QLPreviewController *)controller { (void)controller; return _preview ? 1 : 0; }
- (id<QLPreviewItem>)previewController:(QLPreviewController *)controller previewItemAtIndex:(NSInteger)index { (void)controller; (void)index; return _preview; }
@end
