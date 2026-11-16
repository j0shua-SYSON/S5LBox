// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMIPALibraryViewController.h"
#import "VMIPALibrary.h"
#import "VMDocumentPicker.h"
#import "VMImportedDocument.h"
#import "VMIPAIcon.h"
#import "VMLegacyStoreViewController.h"
#import <ImageIO/ImageIO.h>

static UIImage *LibraryIconThumbnail(NSArray<NSData *> *candidates, CGFloat scale) {
    for (NSData *data in candidates) {
        CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
        CGImageRef thumbnail = source ? CGImageSourceCreateThumbnailAtIndex(source, 0,
            (__bridge CFDictionaryRef)@{(id)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
                (id)kCGImageSourceThumbnailMaxPixelSize:@(48 * scale),
                (id)kCGImageSourceCreateThumbnailWithTransform:@YES}) : NULL;
        UIImage *decoded = thumbnail ? [UIImage imageWithCGImage:thumbnail] : nil;
        if (thumbnail) CGImageRelease(thumbnail);
        if (source) CFRelease(source);
        // UIKit understands the optimized CgBI PNGs shipped in early iPhone
        // apps. Keep that native path when ImageIO cannot make a thumbnail.
        if (!decoded) decoded = [UIImage imageWithData:data];
        if (!decoded || decoded.size.width <= 0 || decoded.size.height <= 0) continue;
        UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
        format.scale = scale; format.opaque = NO;
        UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(48, 48) format:format];
        CGFloat ratio = MIN(48 / decoded.size.width, 48 / decoded.size.height);
        CGSize size = CGSizeMake(decoded.size.width * ratio, decoded.size.height * ratio);
        return [[renderer imageWithActions:^(__unused UIGraphicsImageRendererContext *context) {
            [decoded drawInRect:CGRectMake((48 - size.width) / 2, (48 - size.height) / 2, size.width, size.height)];
        }] imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
    }
    return nil;
}

@interface VMIPALibraryViewController () <UIDocumentPickerDelegate>
- (void)refreshLibrary;
- (void)importTapped;
- (void)installTapped:(UIButton *)button;
@end

@implementation VMIPALibraryViewController {
    NSArray<NSURL *> *_files;
    NSString *_status;
    BOOL _importing;
    NSCache<NSURL *, UIImage *> *_icons;
    NSOperationQueue *_iconQueue;
    NSUInteger _iconGeneration;
}
- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    if (!self) return nil;
    _files = @[];
    _storeTarget = [VMLegacyStoreTarget supportedGuest];
    _icons = [NSCache new];
    _icons.countLimit = 128;
    _iconQueue = [NSOperationQueue new];
    _iconQueue.maxConcurrentOperationCount = 1;
    _iconQueue.qualityOfService = NSQualityOfServiceUserInitiated;
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
- (void)dealloc {
    [_iconQueue cancelAllOperations];
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}
- (void)refreshLibrary {
    if (_importing) { [self.refreshControl endRefreshing]; return; }
    NSError *error = nil;
    _files = [VMIPALibrary filesWithError:&error] ?: @[];
    if (error) _status = error.localizedDescription;
    NSUInteger generation = ++_iconGeneration;
    [_iconQueue cancelAllOperations];
    [_icons removeAllObjects];
    [self.refreshControl endRefreshing];
    [self.tableView reloadData];
    CGFloat scale = self.view.window.screen.scale ?: UIScreen.mainScreen.scale;
    __weak VMIPALibraryViewController *weakSelf = self;
    for (NSURL *url in _files) {
        [_iconQueue addOperationWithBlock:^{
            @autoreleasepool {
                UIImage *icon = LibraryIconThumbnail([VMIPAIcon imageDataCandidatesForURL:url], scale);
                if (!icon) return;
                dispatch_async(dispatch_get_main_queue(), ^{
                    VMIPALibraryViewController *owner = weakSelf;
                    if (!owner || owner->_iconGeneration != generation) return;
                    NSUInteger row = [owner->_files indexOfObject:url];
                    if (row == NSNotFound) return;
                    [owner->_icons setObject:icon forKey:url];
                    UITableViewCell *cell = [owner.tableView cellForRowAtIndexPath:
                        [NSIndexPath indexPathForRow:(NSInteger)row inSection:1]];
                    cell.imageView.image = icon;
                    [cell setNeedsLayout];
                });
            }
        }];
    }
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { return 2; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    return section == 0 ? 2 : MAX((NSUInteger)1, _files.count);
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    return section == 1 ? @"Apps" : nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    cell.textLabel.adjustsFontForContentSizeCategory = cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.numberOfLines = cell.detailTextLabel.numberOfLines = 0;
    cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    cell.selectionStyle = UITableViewCellSelectionStyleNone;
    if (path.section == 0 && path.row == 1) {
        cell.textLabel.text = @"Legacy Store";
        cell.detailTextLabel.text = @"Find apps for your guest";
        cell.imageView.image = [UIImage systemImageNamed:@"bag"];
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
        cell.selectionStyle = UITableViewCellSelectionStyleDefault;
        cell.accessibilityIdentifier = @"s5lbox.ipa-library.legacy-store";
    } else if (path.section == 0) {
        cell.textLabel.text = @"Add apps";
        NSString *hint = @"Tap Import, or add files to S5LBox → IPAs in Files.";
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
        UIImage *icon = [_icons objectForKey:url];
        if (!icon) {
            // Reserve the actual icon's footprint so artwork arriving later
            // never moves the app name or Install button.
            UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(48, 48)];
            icon = [renderer imageWithActions:^(__unused UIGraphicsImageRendererContext *context) {
                [[[UIImage systemImageNamed:@"app.dashed"] imageWithTintColor:UIColor.secondaryLabelColor]
                    drawInRect:CGRectMake(10, 10, 28, 28)];
            }];
        }
        cell.imageView.image = icon;
        cell.imageView.tintColor = UIColor.secondaryLabelColor;
        cell.imageView.layer.cornerRadius = 10;
        cell.imageView.clipsToBounds = YES;
        cell.imageView.contentMode = UIViewContentModeScaleAspectFit;
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
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (!_importing && path.section == 0 && path.row == 1) {
        VMLegacyStoreViewController *store = [[VMLegacyStoreViewController alloc] initWithTarget:self.storeTarget];
        __weak VMIPALibraryViewController *weakSelf = self;
        store.selectionHandler = ^(NSURL *URL) {
            VMIPALibraryViewController *library = weakSelf;
            UINavigationController *navigation = library.navigationController;
            if (!library || ![navigation.viewControllers containsObject:library]) return;
            [navigation popToViewController:library animated:NO];
            if (library.selectionHandler) library.selectionHandler(URL);
        };
        [self.navigationController pushViewController:store animated:YES];
        return;
    }
    if (!_importing && path.section == 1 && (NSUInteger)path.row < _files.count && self.selectionHandler)
        self.selectionHandler(_files[(NSUInteger)path.row]);
}
- (void)importTapped {
    if (_importing) return;
    UIDocumentPickerViewController *picker = VMCreateImportPicker();
    picker.allowsMultipleSelection = NO;
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentAtURL:(NSURL *)url {
    [self documentPicker:controller didPickDocumentsAtURLs:url ? @[url] : @[]];
}
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject;
    if (!url || _importing) return;
    VMImportedDocument *document = [[VMImportedDocument alloc] initWithPickerURL:url];
    BOOL scoped = [url startAccessingSecurityScopedResource];
    _importing = YES;
    _status = [@"Importing " stringByAppendingString:url.lastPathComponent];
    self.navigationItem.rightBarButtonItem.enabled = NO;
    [self.tableView reloadData];
    [controller dismissViewControllerAnimated:YES completion:nil];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *error = nil;
        NSURL *saved = [VMIPALibrary importURL:document.url error:&error];
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
