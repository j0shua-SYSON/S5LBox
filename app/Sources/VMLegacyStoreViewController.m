// Native, guest-filtered browsing. Downloads never install themselves.
#import "VMLegacyStoreViewController.h"
#import "VMAdaptiveLayout.h"
#import "VMLegacyStoreDownload.h"
#import "VMIPALibrary.h"
#import "VMIPAPackage.h"
#import <ImageIO/ImageIO.h>

static VMLegacyStoreDownload *StoreDownload(void) {
    static VMLegacyStoreDownload *download;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSURL *cache = [NSFileManager.defaultManager URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject;
        download = [[VMLegacyStoreDownload alloc] initWithDirectory:[cache URLByAppendingPathComponent:@"LegacyStore" isDirectory:YES]
            configuration:NSURLSessionConfiguration.ephemeralSessionConfiguration
            publisher:^NSURL *(NSURL *file, VMLegacyStoreApp *app, NSError **error) {
                // Reuse the installer's ZIP, CRC, plist, architecture, encryption
                // and minimum-OS checks before adding anything to the library.
                VMIPAPackage *package = [[VMIPAPackage alloc] initWithURL:file error:error];
                if (!package) return nil;
                if (![package.bundleIdentifier isEqual:app.bundleIdentifier]) {
                    if (error) *error = VMLegacyStoreError(@"The downloaded app does not match the catalog.");
                    return nil;
                }
                return [VMIPALibrary importURL:file error:error];
            }];
    });
    return download;
}
static UILabel *StoreLabel(UIFontTextStyle style, UIColor *color) {
    UILabel *label = [UILabel new]; label.font = [UIFont preferredFontForTextStyle:style];
    label.adjustsFontForContentSizeCategory = YES; label.numberOfLines = 0; label.textColor = color;
    return label;
}
static UIButton *StoreButton(id target, SEL action) {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    button.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleHeadline];
    button.titleLabel.adjustsFontForContentSizeCategory = YES;
    button.contentEdgeInsets = UIEdgeInsetsMake(10, 22, 10, 22);
    button.backgroundColor = UIColor.tertiarySystemFillColor; button.layer.cornerRadius = 19;
    if (action) [button addTarget:target action:action forControlEvents:UIControlEventTouchUpInside];
    [button.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    return button;
}
static UIImage *StoreIcon(NSData *data) {
    if (!data.length) return nil;
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)data, NULL);
    if (!source) return nil;
    NSDictionary *properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, 0, NULL));
    double width = [properties[(id)kCGImagePropertyPixelWidth] doubleValue];
    double height = [properties[(id)kCGImagePropertyPixelHeight] doubleValue];
    CGImageRef image = width > 0 && height > 0 && width <= 4096 && height <= 4096
        ? CGImageSourceCreateThumbnailAtIndex(source, 0, (__bridge CFDictionaryRef)@{
            (id)kCGImageSourceCreateThumbnailFromImageAlways:@YES,
            (id)kCGImageSourceThumbnailMaxPixelSize:@240,
            (id)kCGImageSourceCreateThumbnailWithTransform:@YES}) : NULL;
    UIImage *result = image ? [UIImage imageWithCGImage:image] : nil;
    if (image) CGImageRelease(image);
    CFRelease(source);
    return result;
}

@interface VMLegacyStoreVersionsViewController : UITableViewController
@property (nonatomic, strong) VMLegacyStoreApp *app;
@property (nonatomic, copy) void (^choose)(VMLegacyStoreApp *app);
@end
@implementation VMLegacyStoreVersionsViewController {
    VMLegacyStoreVersions *_catalog;
    NSArray<VMLegacyStoreVersion *> *_versions;
    NSString *_message;
    NSInteger _checking;
    BOOL _loading;
}
- (instancetype)init { return [super initWithStyle:UITableViewStyleInsetGrouped]; }
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = @"Versions"; _checking = NSNotFound;
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.tableView.rowHeight = UITableViewAutomaticDimension; self.tableView.estimatedRowHeight = 64;
    _catalog = [[VMLegacyStoreVersions alloc] initWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    self.refreshControl = [UIRefreshControl new];
    [self.refreshControl addTarget:self action:@selector(reloadVersions) forControlEvents:UIControlEventValueChanged];
    [self reloadVersions];
}
- (void)reloadVersions {
    if (_loading || _checking != NSNotFound) { [self.refreshControl endRefreshing]; return; }
    _loading = YES; _message = @"Loading versions…"; [self.tableView reloadData];
    __weak VMLegacyStoreVersionsViewController *weakSelf = self;
    [_catalog loadApp:self.app completion:^(NSArray *versions, NSError *error) {
        VMLegacyStoreVersionsViewController *owner = weakSelf; if (!owner) return;
        owner->_loading = NO; owner->_versions = versions;
        owner->_message = error.localizedDescription ?: (versions.count ? @"" : @"No other compatible versions are available.");
        [owner.refreshControl endRefreshing]; [owner.tableView reloadData];
    }];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; (void)section; return _versions.count ?: 1;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return _versions.count ? (_message.length ? _message : [NSString stringWithFormat:@"Filtered for iPhone OS %@. The selected copy is verified before download.", self.app.target.OSVersion]) : nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"version"];
    if (!cell) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"version"];
    cell.textLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleFootnote];
    cell.textLabel.adjustsFontForContentSizeCategory = cell.detailTextLabel.adjustsFontForContentSizeCategory = YES;
    cell.textLabel.numberOfLines = cell.detailTextLabel.numberOfLines = 0;
    cell.accessoryView = nil; cell.accessoryType = UITableViewCellAccessoryNone;
    cell.detailTextLabel.textColor = UIColor.secondaryLabelColor;
    cell.textLabel.text = _message; cell.detailTextLabel.text = _loading ? nil : @"Tap to try again.";
    if (_versions.count) {
        VMLegacyStoreVersion *version = _versions[path.row];
        cell.textLabel.text = version.version;
        cell.detailTextLabel.text = [NSString stringWithFormat:@"Requires iPhone OS %@", version.minimumOS];
        cell.accessoryType = [version.version isEqual:self.app.version] ? UITableViewCellAccessoryCheckmark : UITableViewCellAccessoryNone;
    }
    if (_loading || _checking == path.row) {
        UIActivityIndicatorView *spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        [spinner startAnimating]; cell.accessoryView = spinner;
    }
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (_loading || _checking != NSNotFound) return;
    if (!_versions.count) { [self reloadVersions]; return; }
    _checking = path.row; _message = @"Checking compatibility…"; [tableView reloadData];
    __weak VMLegacyStoreVersionsViewController *weakSelf = self;
    [_catalog resolveVersion:_versions[path.row] app:self.app completion:^(VMLegacyStoreApp *app, NSError *error) {
        VMLegacyStoreVersionsViewController *owner = weakSelf; if (!owner) return;
        owner->_checking = NSNotFound;
        if (app) { if (owner.choose) owner.choose(app); [owner.navigationController popViewControllerAnimated:YES]; }
        else { owner->_message = error.localizedDescription; [owner.tableView reloadData]; }
    }];
}
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; [_catalog cancel]; }
@end

@interface VMLegacyStoreDetailViewController : UIViewController
@property (nonatomic, strong) VMLegacyStoreApp *app;
@property (nonatomic, strong) UIImage *artwork;
@property (nonatomic, copy) void (^selectionHandler)(NSURL *URL);
@end
@implementation VMLegacyStoreDetailViewController {
    UIButton *_get, *_cancel;
    UILabel *_status;
    UIProgressView *_progress;
    UIActivityIndicatorView *_spinner;
    VMLegacyStoreRequest *_iconRequest;
    UIButton *_version;
    UILabel *_minimumOS, *_downloadSize;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = self.app.name;
    self.navigationItem.largeTitleDisplayMode = UINavigationItemLargeTitleDisplayModeNever;
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    UIScrollView *scroll = [UIScrollView new]; scroll.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:scroll];
    UIStackView *stack = [[UIStackView alloc] init]; stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 24; stack.translatesAutoresizingMaskIntoConstraints = NO; [scroll addSubview:stack];
    [NSLayoutConstraint activateConstraints:@[
        [scroll.topAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.topAnchor],
        [scroll.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
        [scroll.leadingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.leadingAnchor], [scroll.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor],
        [stack.topAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.topAnchor constant:24],
        [stack.bottomAnchor constraintEqualToAnchor:scroll.contentLayoutGuide.bottomAnchor constant:-32]
    ]];
    VMConstrainReadableColumn(stack, scroll, 640);
    UIImageView *icon = [[UIImageView alloc] initWithImage:self.artwork ?: [UIImage systemImageNamed:@"app"]];
    icon.contentMode = UIViewContentModeScaleAspectFit; icon.clipsToBounds = YES; icon.layer.cornerRadius = 20;
    icon.tintColor = UIColor.tertiaryLabelColor; icon.isAccessibilityElement = NO;
    [icon.widthAnchor constraintEqualToConstant:88].active = YES; [icon.heightAnchor constraintEqualToConstant:88].active = YES;
    UILabel *name = StoreLabel(UIFontTextStyleTitle2, UIColor.labelColor); name.text = self.app.name;
    name.accessibilityTraits |= UIAccessibilityTraitHeader;
    UILabel *developer = StoreLabel(UIFontTextStyleSubheadline, UIColor.secondaryLabelColor); developer.text = self.app.developer;
    UIStackView *titles = [[UIStackView alloc] initWithArrangedSubviews:@[name, developer]];
    titles.axis = UILayoutConstraintAxisVertical; titles.spacing = 6;
    UIStackView *hero = [[UIStackView alloc] initWithArrangedSubviews:@[icon, titles]];
    hero.axis = UILayoutConstraintAxisHorizontal; hero.alignment = UIStackViewAlignmentCenter; hero.spacing = 18;
    [stack addArrangedSubview:hero];
    _get = StoreButton(self, @selector(getTapped)); _get.accessibilityIdentifier = @"s5lbox.legacy-store.get";
    _get.titleLabel.numberOfLines = 0; _get.titleLabel.textAlignment = NSTextAlignmentCenter;
    _get.backgroundColor = UIColor.systemBlueColor; [_get setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [_get setTitleColor:UIColor.secondaryLabelColor forState:UIControlStateDisabled];
    [_get.heightAnchor constraintGreaterThanOrEqualToConstant:52].active = YES;
    _get.layer.cornerRadius = 16;
    _cancel = [UIButton buttonWithType:UIButtonTypeSystem]; [_cancel setTitle:@"Cancel download" forState:UIControlStateNormal];
    [_cancel addTarget:self action:@selector(cancelTapped) forControlEvents:UIControlEventTouchUpInside];
    _cancel.contentEdgeInsets = UIEdgeInsetsMake(12, 12, 12, 12);
    _status = StoreLabel(UIFontTextStyleFootnote, UIColor.secondaryLabelColor); _status.textAlignment = NSTextAlignmentCenter;
    _progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
    _spinner = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    UIStackView *action = [[UIStackView alloc] initWithArrangedSubviews:@[_get, _spinner, _status, _progress, _cancel]];
    action.axis = UILayoutConstraintAxisVertical; action.spacing = 12; [stack addArrangedSubview:action];
    UIView *separator = [UIView new]; separator.backgroundColor = UIColor.separatorColor;
    [separator.heightAnchor constraintEqualToConstant:0.5].active = YES; [stack addArrangedSubview:separator];
    _version = [UIButton buttonWithType:UIButtonTypeSystem];
    _version.contentHorizontalAlignment = UIControlContentHorizontalAlignmentLeft;
    _version.titleLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _version.titleLabel.adjustsFontForContentSizeCategory = YES; _version.titleLabel.numberOfLines = 0;
    _version.contentEdgeInsets = UIEdgeInsetsMake(12, 0, 12, 0);
    [_version addTarget:self action:@selector(chooseVersion) forControlEvents:UIControlEventTouchUpInside];
    [_version.heightAnchor constraintGreaterThanOrEqualToConstant:44].active = YES;
    [stack addArrangedSubview:_version];
    for (NSArray *pair in @[@[@"Requires iPhone OS", self.app.minimumOS],
        @[@"Download size", [NSByteCountFormatter stringFromByteCount:(int64_t)self.app.size countStyle:NSByteCountFormatterCountStyleFile]]]) {
        UILabel *key = StoreLabel(UIFontTextStyleSubheadline, UIColor.secondaryLabelColor); key.text = pair[0];
        UILabel *value = StoreLabel(UIFontTextStyleBody, UIColor.labelColor); value.text = pair[1];
        UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[key, value]];
        row.axis = UILayoutConstraintAxisVertical; row.spacing = 4; [stack addArrangedSubview:row];
        if (!_minimumOS) _minimumOS = value; else _downloadSize = value;
    }
    UILabel *note = StoreLabel(UIFontTextStyleFootnote, UIColor.secondaryLabelColor);
    note.text = [NSString stringWithFormat:@"Selected for iPhone OS %@. Some apps may not work in S5LBox. AppSync may be required to install.\n\nCatalog by Legacy Store. Files hosted on Internet Archive.", self.app.target.OSVersion];
    [stack addArrangedSubview:note];
    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(updateDownload)
        name:VMLegacyStoreDownloadDidChangeNotification object:StoreDownload()];
    if (!self.artwork && self.app.iconURL) {
        __weak UIImageView *weakIcon = icon;
        _iconRequest = [[VMLegacyStoreRequest alloc] initWithURL:self.app.iconURL kind:VMLegacyStoreIconRequest
            configuration:NSURLSessionConfiguration.ephemeralSessionConfiguration destination:nil expectedSize:0 SHA1:nil progress:nil
            completion:^(NSData *data, NSError *error) {
                if (error) return;
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    UIImage *image = StoreIcon(data);
                    dispatch_async(dispatch_get_main_queue(), ^{ if (image) weakIcon.image = image; });
                });
            }];
    }
    [self updateDownload];
}
- (BOOL)isCurrent { return [StoreDownload().app.identifier isEqual:self.app.identifier]; }
- (void)updateDownload {
    VMLegacyStoreDownload *download = StoreDownload();
    BOOL current = [self isCurrent], receiving = current && download.state == VMLegacyStoreDownloading;
    BOOL checking = current && download.state == VMLegacyStoreChecking;
    BOOL ready = current && download.state == VMLegacyStoreDownloaded &&
        [NSFileManager.defaultManager fileExistsAtPath:download.libraryURL.path];
    _get.enabled = !download.busy;
    _version.enabled = !download.busy;
    [_version setTitle:[NSString stringWithFormat:@"Version %@  ›", self.app.version] forState:UIControlStateNormal];
    _version.accessibilityLabel = [NSString stringWithFormat:@"Choose version. Selected %@", self.app.version];
    _minimumOS.text = self.app.minimumOS;
    _downloadSize.text = [NSByteCountFormatter stringFromByteCount:(int64_t)self.app.size countStyle:NSByteCountFormatterCountStyleFile];
    _get.backgroundColor = download.busy ? UIColor.tertiarySystemFillColor : UIColor.systemBlueColor;
    NSString *title = ready ? @"Install" : current && download.state == VMLegacyStoreDownloadFailed ? @"Retry download" : @"Get";
    if (download.busy) title = current ? (checking ? @"Checking app…" : @"Downloading…") : @"Another download is in progress";
    [_get setTitle:title forState:UIControlStateNormal];
    _progress.hidden = !receiving; [_progress setProgress:(float)download.fraction animated:YES];
    _cancel.hidden = !receiving;
    if (checking) [_spinner startAnimating]; else [_spinner stopAnimating];
    _spinner.hidden = !checking;
    _status.text = receiving ? [NSString stringWithFormat:@"%.0f%%", download.fraction * 100] :
        ready ? @"Added to your IPA library" : current && download.state == VMLegacyStoreDownloadFailed ? download.error.localizedDescription :
        current && download.state == VMLegacyStoreDownloadCancelled ? @"Download cancelled" : @"";
    _status.hidden = !_status.text.length;
}
- (void)chooseVersion {
    if (StoreDownload().busy) return;
    VMLegacyStoreVersionsViewController *versions = [VMLegacyStoreVersionsViewController new]; versions.app = self.app;
    __weak VMLegacyStoreDetailViewController *weakSelf = self;
    versions.choose = ^(VMLegacyStoreApp *app) { weakSelf.app = app; [weakSelf updateDownload]; };
    [self.navigationController pushViewController:versions animated:YES];
}
- (void)getTapped {
    VMLegacyStoreDownload *download = StoreDownload();
    if (download.busy) return;
    if ([self isCurrent] && download.state == VMLegacyStoreDownloaded &&
        [NSFileManager.defaultManager fileExistsAtPath:download.libraryURL.path]) {
        if (self.selectionHandler) self.selectionHandler(download.libraryURL);
    } else [download startApp:self.app];
}
- (void)cancelTapped { if ([self isCurrent]) [StoreDownload() cancel]; }
- (void)dealloc { [_iconRequest cancel]; [NSNotificationCenter.defaultCenter removeObserver:self]; }
@end

@interface VMLegacyStoreCell : UITableViewCell
@property (nonatomic, strong) UIImageView *artwork;
@property (nonatomic, strong) UILabel *nameLabel;
@property (nonatomic, strong) UILabel *subtitleLabel;
@property (nonatomic, strong) UIButton *getButton;
@end
@implementation VMLegacyStoreCell {
    NSLayoutConstraint *_buttonWidth;
    UIStackView *_row;
}
- (instancetype)initWithStyle:(UITableViewCellStyle)style reuseIdentifier:(NSString *)identifier {
    self = [super initWithStyle:style reuseIdentifier:identifier]; if (!self) return nil;
    _artwork = [UIImageView new]; _artwork.contentMode = UIViewContentModeScaleAspectFit;
    _artwork.layer.cornerRadius = 13; _artwork.clipsToBounds = YES; _artwork.tintColor = UIColor.tertiaryLabelColor;
    _nameLabel = StoreLabel(UIFontTextStyleHeadline, UIColor.labelColor);
    _subtitleLabel = StoreLabel(UIFontTextStyleSubheadline, UIColor.secondaryLabelColor);
    _getButton = StoreButton(nil, NULL); [_getButton setTitle:@"Get" forState:UIControlStateNormal];
    _getButton.contentEdgeInsets = UIEdgeInsetsMake(10, 12, 10, 12);
    _buttonWidth = [_getButton.widthAnchor constraintEqualToConstant:84]; _buttonWidth.active = YES;
    [_getButton setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    [_getButton setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView *labels = [[UIStackView alloc] initWithArrangedSubviews:@[_nameLabel, _subtitleLabel]];
    labels.axis = UILayoutConstraintAxisVertical; labels.spacing = 3;
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[_artwork, labels, _getButton]];
    _row = row;
    row.axis = UILayoutConstraintAxisHorizontal; row.alignment = UIStackViewAlignmentCenter; row.spacing = 14;
    row.translatesAutoresizingMaskIntoConstraints = NO; [self.contentView addSubview:row];
    [NSLayoutConstraint activateConstraints:@[
        [_artwork.widthAnchor constraintEqualToConstant:60], [_artwork.heightAnchor constraintEqualToConstant:60],
        [row.topAnchor constraintEqualToAnchor:self.contentView.layoutMarginsGuide.topAnchor constant:4],
        [row.bottomAnchor constraintEqualToAnchor:self.contentView.layoutMarginsGuide.bottomAnchor constant:-4],
        [row.leadingAnchor constraintEqualToAnchor:self.contentView.layoutMarginsGuide.leadingAnchor],
        [row.trailingAnchor constraintEqualToAnchor:self.contentView.layoutMarginsGuide.trailingAnchor]
    ]];
    self.separatorInset = UIEdgeInsetsMake(0, 94, 0, 0);
    [self updateMetrics];
    return self;
}
- (void)updateMetrics {
    _buttonWidth.constant = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleHeadline] scaledValueForValue:84];
    BOOL accessible = UIContentSizeCategoryIsAccessibilityCategory(self.traitCollection.preferredContentSizeCategory);
    _row.axis = accessible ? UILayoutConstraintAxisVertical : UILayoutConstraintAxisHorizontal;
    _row.alignment = accessible ? UIStackViewAlignmentLeading : UIStackViewAlignmentCenter;
    _getButton.layer.cornerRadius = 22;
}
- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    [super traitCollectionDidChange:previousTraitCollection]; [self updateMetrics];
}
@end

@interface VMLegacyStoreViewController () <UISearchResultsUpdating, UISearchBarDelegate>
@end
@implementation VMLegacyStoreViewController {
    VMLegacyStoreTarget *_target;
    VMLegacyStoreCatalog *_catalog;
    NSArray<VMLegacyStoreApp *> *_apps;
    UISearchController *_search;
    NSTimer *_debounce;
    UILabel *_message;
    UIButton *_retry;
    UIActivityIndicatorView *_loading;
    NSCache<NSURL *, UIImage *> *_icons;
    NSMutableDictionary<NSURL *, VMLegacyStoreRequest *> *_iconRequests;
    NSMutableSet<NSURL *> *_failedIcons;
    NSUInteger _generation;
}
- (instancetype)initWithTarget:(VMLegacyStoreTarget *)target {
    self = [super initWithStyle:UITableViewStylePlain]; if (!self) return nil;
    _target = target; _apps = @[];
    _catalog = [[VMLegacyStoreCatalog alloc] initWithConfiguration:NSURLSessionConfiguration.ephemeralSessionConfiguration];
    _icons = [NSCache new]; _icons.countLimit = 100; _icons.totalCostLimit = 8 * 1024 * 1024;
    _iconRequests = [NSMutableDictionary dictionary]; _failedIcons = [NSMutableSet set];
    self.title = @"Legacy Store";
    return self;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    self.tableView.rowHeight = UITableViewAutomaticDimension; self.tableView.estimatedRowHeight = 90;
    self.tableView.keyboardDismissMode = UIScrollViewKeyboardDismissModeOnDrag;
    [self.tableView registerClass:VMLegacyStoreCell.class forCellReuseIdentifier:@"app"];
    _search = [[UISearchController alloc] initWithSearchResultsController:nil];
    _search.searchResultsUpdater = self; _search.searchBar.delegate = self;
    _search.obscuresBackgroundDuringPresentation = NO; _search.searchBar.placeholder = @"Search apps";
    self.navigationItem.searchController = _search; self.navigationItem.hidesSearchBarWhenScrolling = NO;
    self.definesPresentationContext = YES;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithImage:[UIImage systemImageNamed:@"arrow.down.circle"]
        style:UIBarButtonItemStylePlain target:self action:@selector(currentDownloadTapped)];
    self.navigationItem.rightBarButtonItem.accessibilityLabel = @"Current download";
    self.refreshControl = [UIRefreshControl new]; [self.refreshControl addTarget:self action:@selector(searchNow) forControlEvents:UIControlEventValueChanged];
    UIView *background = [UIView new];
    _message = StoreLabel(UIFontTextStyleBody, UIColor.secondaryLabelColor); _message.textAlignment = NSTextAlignmentCenter;
    _retry = [UIButton buttonWithType:UIButtonTypeSystem]; [_retry setTitle:@"Try again" forState:UIControlStateNormal];
    _retry.contentEdgeInsets = UIEdgeInsetsMake(12, 12, 12, 12); [_retry addTarget:self action:@selector(searchNow) forControlEvents:UIControlEventTouchUpInside];
    _loading = [[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
    UIStackView *status = [[UIStackView alloc] initWithArrangedSubviews:@[_loading, _message, _retry]];
    status.axis = UILayoutConstraintAxisVertical; status.spacing = 12; status.translatesAutoresizingMaskIntoConstraints = NO;
    [background addSubview:status];
    [NSLayoutConstraint activateConstraints:@[[status.centerYAnchor constraintEqualToAnchor:background.centerYAnchor],
        [status.leadingAnchor constraintEqualToAnchor:background.leadingAnchor constant:28],
        [status.trailingAnchor constraintEqualToAnchor:background.trailingAnchor constant:-28]]];
    self.tableView.backgroundView = background;
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(downloadChanged)
        name:VMLegacyStoreDownloadDidChangeNotification object:StoreDownload()];
    [self downloadChanged]; [self searchNow];
}
- (void)downloadChanged {
    self.navigationItem.rightBarButtonItem.enabled = StoreDownload().app != nil;
    for (NSIndexPath *path in self.tableView.indexPathsForVisibleRows) {
        if ((NSUInteger)path.row >= _apps.count) continue;
        VMLegacyStoreCell *cell = (id)[self.tableView cellForRowAtIndexPath:path];
        [self configureButton:cell.getButton app:_apps[(NSUInteger)path.row]];
    }
}
- (BOOL)downloadReadyForApp:(VMLegacyStoreApp *)app {
    VMLegacyStoreDownload *download = StoreDownload();
    return [download.app.identifier isEqual:app.identifier] && download.state == VMLegacyStoreDownloaded &&
        [NSFileManager.defaultManager fileExistsAtPath:download.libraryURL.path];
}
- (void)configureButton:(UIButton *)button app:(VMLegacyStoreApp *)app {
    VMLegacyStoreDownload *download = StoreDownload();
    BOOL current = [download.app.identifier isEqual:app.identifier];
    NSString *title = [self downloadReadyForApp:app] ? @"Install" :
        current && download.state == VMLegacyStoreDownloading ? [NSString stringWithFormat:@"%.0f%%", download.fraction * 100] :
        current && download.state == VMLegacyStoreChecking ? @"…" :
        current && download.state == VMLegacyStoreDownloadFailed ? @"Retry" : @"Get";
    [button setTitle:title forState:UIControlStateNormal];
    button.enabled = !download.busy || current;
    button.accessibilityLabel = [NSString stringWithFormat:@"%@ %@", current && download.busy ? @"Download" : title, app.name];
    button.accessibilityValue = current && download.busy ? (download.state == VMLegacyStoreChecking ? @"Checking app" : title) : nil;
}
- (void)updateSearchResultsForSearchController:(UISearchController *)searchController {
    (void)searchController;
    [_debounce invalidate]; [_catalog cancel];
    // Do not leave old-query apps tappable while the next query is pending.
    _apps = @[]; [self.tableView reloadData];
    self.tableView.backgroundView.hidden = NO;
    _message.text = @""; _retry.hidden = YES; [_loading startAnimating];
    __weak VMLegacyStoreViewController *weakSelf = self;
    _debounce = [NSTimer scheduledTimerWithTimeInterval:0.45 repeats:NO block:^(NSTimer *timer) { (void)timer; [weakSelf searchNow]; }];
}
- (void)searchBarSearchButtonClicked:(UISearchBar *)searchBar { [searchBar resignFirstResponder]; [self searchNow]; }
- (void)searchNow {
    [_debounce invalidate]; _debounce = nil;
    ++_generation;
    for (VMLegacyStoreRequest *request in _iconRequests.allValues) [request cancel];
    [_iconRequests removeAllObjects]; [_failedIcons removeAllObjects];
    _retry.hidden = YES; _message.text = @""; [_loading startAnimating];
    self.tableView.backgroundView.hidden = NO;
    __weak VMLegacyStoreViewController *weakSelf = self;
    [_catalog search:_search.searchBar.text ?: @"" target:_target completion:^(NSArray *apps, BOOL cached, NSError *error) {
        VMLegacyStoreViewController *owner = weakSelf; if (!owner) return;
        owner->_apps = apps; [owner->_loading stopAnimating]; [owner.refreshControl endRefreshing];
        owner->_message.text = error ? error.localizedDescription : @"No matching apps for this guest. Try another search.";
        owner->_retry.hidden = error == nil;
        owner.tableView.backgroundView.hidden = apps.count != 0;
        owner.navigationItem.prompt = cached ? @"Offline results — pull to retry" : nil;
        [owner.tableView reloadData]; [owner loadVisibleIcons];
    }];
}
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section { (void)tableView; (void)section; return _apps.count; }
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView; (void)section;
    return [NSString stringWithFormat:@"For iPhone OS %@", _target.OSVersion];
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    VMLegacyStoreApp *app = _apps[(NSUInteger)path.row];
    VMLegacyStoreCell *cell = [tableView dequeueReusableCellWithIdentifier:@"app" forIndexPath:path];
    cell.nameLabel.text = app.name;
    cell.subtitleLabel.text = app.developer.length ? [NSString stringWithFormat:@"%@\nVersion %@", app.developer, app.version] : [@"Version " stringByAppendingString:app.version];
    cell.artwork.image = app.iconURL ? [_icons objectForKey:app.iconURL] : nil;
    if (!cell.artwork.image) cell.artwork.image = [UIImage systemImageNamed:@"app"];
    cell.getButton.tag = path.row; [self configureButton:cell.getButton app:app];
    [cell.getButton removeTarget:nil action:NULL forControlEvents:UIControlEventTouchUpInside];
    [cell.getButton addTarget:self action:@selector(appButtonTapped:) forControlEvents:UIControlEventTouchUpInside];
    return cell;
}
- (void)tableView:(UITableView *)tableView willDisplayCell:(UITableViewCell *)cell forRowAtIndexPath:(NSIndexPath *)path {
    (void)tableView; (void)cell; (void)path;
    dispatch_async(dispatch_get_main_queue(), ^{ [self loadVisibleIcons]; });
}
- (void)loadVisibleIcons {
    NSUInteger generation = _generation;
    for (NSIndexPath *path in self.tableView.indexPathsForVisibleRows) {
        if (_iconRequests.count >= 3) break;
        if ((NSUInteger)path.row >= _apps.count) continue;
        NSURL *URL = _apps[(NSUInteger)path.row].iconURL;
        if (!URL || [_icons objectForKey:URL] || _iconRequests[URL] || [_failedIcons containsObject:URL]) continue;
        __weak VMLegacyStoreViewController *weakSelf = self;
        _iconRequests[URL] = [[VMLegacyStoreRequest alloc] initWithURL:URL kind:VMLegacyStoreIconRequest
            configuration:NSURLSessionConfiguration.ephemeralSessionConfiguration destination:nil expectedSize:0 SHA1:nil progress:nil
            completion:^(NSData *data, NSError *error) {
                dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    UIImage *icon = error ? nil : StoreIcon(data);
                    dispatch_async(dispatch_get_main_queue(), ^{
                        VMLegacyStoreViewController *owner = weakSelf;
                        if (!owner || owner->_generation != generation) return;
                        [owner->_iconRequests removeObjectForKey:URL];
                        if (icon) [owner->_icons setObject:icon forKey:URL cost:240 * 240 * 4];
                        else [owner->_failedIcons addObject:URL];
                        for (NSIndexPath *visible in owner.tableView.indexPathsForVisibleRows) {
                            if ((NSUInteger)visible.row < owner->_apps.count && [owner->_apps[(NSUInteger)visible.row].iconURL isEqual:URL]) {
                                VMLegacyStoreCell *cell = (id)[owner.tableView cellForRowAtIndexPath:visible];
                                if (icon) cell.artwork.image = icon;
                            }
                        }
                        [owner loadVisibleIcons];
                    });
                });
            }];
    }
}
- (void)showApp:(VMLegacyStoreApp *)app {
    if (self.navigationController.topViewController != self) return;
    [_search.searchBar resignFirstResponder];
    VMLegacyStoreDetailViewController *detail = [VMLegacyStoreDetailViewController new]; detail.app = app;
    detail.artwork = app.iconURL ? [_icons objectForKey:app.iconURL] : nil;
    detail.selectionHandler = self.selectionHandler;
    [self.navigationController pushViewController:detail animated:YES];
}
- (void)appButtonTapped:(UIButton *)button {
    if (button.tag < 0 || (NSUInteger)button.tag >= _apps.count) return;
    VMLegacyStoreApp *app = _apps[(NSUInteger)button.tag];
    if ([self downloadReadyForApp:app]) {
        if (self.selectionHandler) self.selectionHandler(StoreDownload().libraryURL);
        return;
    }
    [self showApp:app];
    VMLegacyStoreDownload *download = StoreDownload();
    if (!download.busy) [download startApp:app];
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES]; if ((NSUInteger)path.row < _apps.count) [self showApp:_apps[(NSUInteger)path.row]];
}
- (void)currentDownloadTapped { if (StoreDownload().app) [self showApp:StoreDownload().app]; }
- (void)dealloc {
    [_debounce invalidate]; [_catalog cancel];
    for (VMLegacyStoreRequest *request in _iconRequests.allValues) [request cancel];
    [NSNotificationCenter.defaultCenter removeObserver:self];
}
@end
