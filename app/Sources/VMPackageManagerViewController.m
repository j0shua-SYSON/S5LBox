// Copyright (c) 2026 j0shua-SYSON. MIT licensed.
#import "VMPackageManagerViewController.h"
#import "VMPackageCatalog.h"
#import "VMPackageRepository.h"
#import "VMPackageBridge.h"

@interface VMPackageReview : UITableViewController
@property (nonatomic, copy) NSArray<NSDictionary *> *changes;
@property (nonatomic, copy) NSString *explanation;
@property (nonatomic, copy) NSString *action;
@property (nonatomic, copy) void (^confirm)(void);
@end
@implementation VMPackageReview
- (void)viewDidLoad {
    [super viewDidLoad]; self.title = @"Review changes";
    self.tableView.rowHeight = UITableViewAutomaticDimension; self.tableView.estimatedRowHeight = 70;
    self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:self.action style:UIBarButtonItemStyleDone target:self action:@selector(apply)];
}
- (void)apply { if (self.confirm) self.confirm(); }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section { (void)tableView; (void)section; return self.changes.count; }
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section { (void)tableView; (void)section; return self.explanation; }
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    (void)tableView; NSDictionary *p=self.changes[path.row];
    UITableViewCell *cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.text=p[@"Name"] ?: p[@"Package"];
    cell.detailTextLabel.text=[NSString stringWithFormat:@"%@ • %@\n%@",p[@"Version"],p[@"_source"] ?: @"Installed",p[@"Description"] ?: @""];
    cell.textLabel.numberOfLines=cell.detailTextLabel.numberOfLines=0;
    cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    cell.textLabel.adjustsFontForContentSizeCategory=cell.detailTextLabel.adjustsFontForContentSizeCategory=YES;
    cell.selectionStyle=UITableViewCellSelectionStyleNone; return cell;
}
@end

@interface VMPackageManagerViewController () <UISearchResultsUpdating>
@end
@implementation VMPackageManagerViewController {
    VMPackageCatalog *_catalog;
    VMPackageRepository *_repository;
    VMPackageBridge *_bridge;
    NSMutableArray<NSDictionary *> *_sources;
    NSArray<NSDictionary *> *_visible;
    NSData *_status;
    UISegmentedControl *_sections;
    NSString *_message;
    dispatch_queue_t _queue;
    BOOL _busy, _idleWasDisabled;
    UIBackgroundTaskIdentifier _background;
    UITextView *_log;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self=[super initWithStyle:UITableViewStyleInsetGrouped];
    if (self) {
        _catalog=[VMPackageCatalog new]; _repository=[VMPackageRepository new];
        _bridge=[[VMPackageBridge alloc] initWithTransport:transport instanceID:identifier];
        _queue=dispatch_queue_create("com.j0shua.S5LBox.packages",DISPATCH_QUEUE_SERIAL);
        _background=UIBackgroundTaskInvalid;
        _sources=[[NSUserDefaults.standardUserDefaults arrayForKey:@"PackageManagerSources-v1"] mutableCopy];
        if (!_sources) _sources=[@[@{@"name":@"iOS 3 Party",@"base":@"https://ios3.party/",@"index":@"https://ios3.party/"},
            @{@"name":@"Saurik",@"base":@"https://apt.saurik.com/cydia/",@"index":@"https://apt.saurik.com/cydia/"}] mutableCopy];
        NSArray *cached=[NSArray arrayWithContentsOfURL:[[VMPackageRepository directory] URLByAppendingPathComponent:@"catalog.plist"]];
        NSMutableArray *activeCache=[NSMutableArray array];
        for (NSDictionary *p in cached) for (NSDictionary *source in _sources)
            if ([p[@"_base"] isEqual:source[@"base"]]) { [activeCache addObject:p]; break; }
        _catalog.packages=activeCache; _visible=@[];
        _message=@"Repositories and downloads run on your iPhone. Only dpkg and package scripts run inside the guest.";
    }
    return self;
}
- (void)viewDidLoad {
    [super viewDidLoad]; self.title=@"Packages";
    self.tableView.accessibilityIdentifier=@"s5lbox.packages";
    self.tableView.rowHeight=UITableViewAutomaticDimension; self.tableView.estimatedRowHeight=66;
    _sections=[[UISegmentedControl alloc] initWithItems:@[@"Browse",@"Installed",@"Sources"]]; _sections.selectedSegmentIndex=0;
    [_sections addTarget:self action:@selector(filter) forControlEvents:UIControlEventValueChanged];
    self.navigationItem.titleView=_sections;
    self.navigationItem.searchController=[[UISearchController alloc] initWithSearchResultsController:nil];
    self.navigationItem.searchController.searchResultsUpdater=self;
    self.navigationItem.searchController.obscuresBackgroundDuringPresentation=NO;
    self.navigationItem.searchController.searchBar.placeholder=@"Search names and descriptions";
    self.definesPresentationContext=YES;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(refresh)];
    __weak VMPackageManagerViewController *weakSelf=self;
    _repository.progress=^(NSString *text) { [weakSelf report:text]; };
    _bridge.progress=^(NSString *text) { [weakSelf report:text]; };
    [self filter]; [self refresh];
}
- (void)report:(NSString *)message {
    dispatch_async(dispatch_get_main_queue(),^{
        self->_message=message;
        if (self->_log) {
            NSString *text=[self->_log.text stringByAppendingFormat:@"\n%@",message];
            if (text.length>32768) text=[text substringFromIndex:text.length-32768];
            self->_log.text=text; [self->_log scrollRangeToVisible:NSMakeRange(text.length,0)];
        } else [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:0] withRowAnimation:UITableViewRowAnimationNone];
    });
}
- (void)setWorking:(BOOL)busy {
    _busy=busy; _sections.enabled=!busy;
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:busy ? @"Cancel" : @"Refresh"
        style:UIBarButtonItemStylePlain target:self action:busy ? @selector(cancel) : @selector(refresh)];
    self.navigationController.modalInPresentation=busy;
    self.navigationController.interactivePopGestureRecognizer.enabled=!busy;
    self.navigationController.topViewController.navigationItem.hidesBackButton=busy;
    self.navigationItem.searchController.searchBar.userInteractionEnabled=!busy;
    if (busy) {
        _bridge.canceled=NO; _repository.canceled=NO;
        _idleWasDisabled=UIApplication.sharedApplication.idleTimerDisabled;
        UIApplication.sharedApplication.idleTimerDisabled=YES;
        __weak VMPackageManagerViewController *weakSelf=self;
        _background=[UIApplication.sharedApplication beginBackgroundTaskWithExpirationHandler:^{
            VMPackageManagerViewController *owner=weakSelf;
            if (owner) { owner->_bridge.canceled=YES; owner->_repository.canceled=YES; }
        }];
    } else {
        UIApplication.sharedApplication.idleTimerDisabled=_idleWasDisabled;
        if (_background!=UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:_background];
        _background=UIBackgroundTaskInvalid;
    }
}
- (BOOL)readInstalled:(NSError **)error {
    NSData *status=[_bridge status:error]; if (!status) return NO;
    NSArray *installed=[VMPackageCatalog parse:status error:error]; if (!installed) return NO;
    _status=status; _catalog.installed=installed; return YES;
}
- (void)refresh {
    if (_busy) return;
    [self setWorking:YES]; [self report:@"Loading repositories…"];
    NSArray *sources=[_sources copy];
    dispatch_async(_queue,^{
        NSError *guestError=nil, *repoError=nil;
        NSArray *packages=[self->_repository refresh:sources error:&repoError];
        if (packages) self->_catalog.packages=packages;
        dispatch_sync(dispatch_get_main_queue(),^{ [self filter]; });
        [self report:@"Reading installed packages…"];
        BOOL guest=[self readInstalled:&guestError];
        dispatch_async(dispatch_get_main_queue(),^{
            [self setWorking:NO];
            self->_message=guestError.localizedDescription ?: repoError.localizedDescription ?: [NSString stringWithFormat:@"%lu package versions. Installed list is current.",(unsigned long)packages.count];
            if (guestError && repoError) self->_message=[NSString stringWithFormat:@"%@\nSources: %@",guestError.localizedDescription,repoError.localizedDescription];
            if (!guest) self->_status=nil;
            [self filter];
        });
    });
}
- (void)updateSearchResultsForSearchController:(UISearchController *)searchController { (void)searchController; if (!_busy) [self filter]; }
- (void)filter {
    NSInteger section=_sections.selectedSegmentIndex;
    NSString *query=self.navigationItem.searchController.searchBar.text ?: @"";
    NSArray *items=section==1 ? _catalog.installed : _catalog.packages;
    NSMutableArray *filtered=[NSMutableArray array];
    NSMutableSet *seen=[NSMutableSet set];
    NSArray *sorted=[items sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b) {
        NSComparisonResult n=[(a[@"Name"] ?: a[@"Package"]) localizedCaseInsensitiveCompare:(b[@"Name"] ?: b[@"Package"])];
        return n ?: -VMPackageVersionCompare(a[@"Version"],b[@"Version"]);
    }];
    for (NSDictionary *p in sorted) {
        if (section==1 && ![p[@"Status"] hasSuffix:@" installed"]) continue;
        if (query.length && ![p[@"Name"] localizedCaseInsensitiveContainsString:query] &&
            ![p[@"Package"] localizedCaseInsensitiveContainsString:query] && ![p[@"Description"] localizedCaseInsensitiveContainsString:query]) continue;
        // Browse retains versions: older firmware often needs an older release.
        NSString *key=[NSString stringWithFormat:@"%@/%@/%@",p[@"Package"],p[@"Version"],p[@"Filename"] ?: @""];
        if ([seen containsObject:key]) continue; [seen addObject:key]; [filtered addObject:p];
    }
    _visible=filtered; [self.tableView reloadData];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return 2; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; if (!section) return 1;
    return _sections.selectedSegmentIndex==2 ? _sources.count+2 : _visible.count;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView;
    if (section==1) return @"Use trusted sources only. SHA-256 checks downloads for corruption, but repository signatures are not verified. Legacy HTTP sources are unencrypted and can be altered in transit. Core package changes, automatic removals, downgrades and dependency cycles require Cydia.";
    return nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell=[tableView dequeueReusableCellWithIdentifier:@"package"];
    if (!cell) cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"package"];
    cell.textLabel.textColor=UIColor.labelColor; cell.accessoryType=UITableViewCellAccessoryNone;
    cell.textLabel.numberOfLines=cell.detailTextLabel.numberOfLines=0;
    cell.textLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    cell.detailTextLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleCaption1];
    cell.textLabel.adjustsFontForContentSizeCategory=cell.detailTextLabel.adjustsFontForContentSizeCategory=YES;
    cell.detailTextLabel.text=nil; cell.selectionStyle=UITableViewCellSelectionStyleNone;
    if (!path.section) { cell.textLabel.text=_message; return cell; }
    cell.selectionStyle=_busy ? UITableViewCellSelectionStyleNone : UITableViewCellSelectionStyleDefault;
    if (_sections.selectedSegmentIndex==2) {
        if ((NSUInteger)path.row < _sources.count) {
            NSDictionary *source=_sources[path.row]; cell.textLabel.text=source[@"name"]; cell.detailTextLabel.text=source[@"index"];
        } else { cell.textLabel.text=(NSUInteger)path.row==_sources.count ? @"Add source…" : @"Add BigBoss (HTTP)…"; cell.textLabel.textColor=UIColor.systemBlueColor; }
    } else {
        NSDictionary *p=_visible[path.row]; cell.textLabel.text=p[@"Name"] ?: p[@"Package"];
        cell.detailTextLabel.text=[NSString stringWithFormat:@"%@ • %@\n%@",p[@"Version"],p[@"_source"] ?: @"Installed",[p[@"Description"] componentsSeparatedByString:@"\n"].firstObject ?: @""];
        cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;
    }
    return cell;
}
- (void)error:(NSError *)error {
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Packages" message:error.localizedDescription preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
    [self.navigationController.topViewController presentViewController:alert animated:YES completion:nil];
}
- (void)addSource:(NSDictionary *)source {
    for (NSDictionary *old in _sources) if ([old[@"index"] isEqual:source[@"index"]]) return;
    [_sources addObject:source]; [NSUserDefaults.standardUserDefaults setObject:_sources forKey:@"PackageManagerSources-v1"];
    [self refresh];
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES]; if (_busy || !path.section) return;
    if (_sections.selectedSegmentIndex==2) {
        if ((NSUInteger)path.row < _sources.count) {
            NSDictionary *source=_sources[path.row];
            UIAlertController *alert=[UIAlertController alertControllerWithTitle:source[@"name"] message:@"Remove this source from S5LBox? Installed guest packages are kept." preferredStyle:UIAlertControllerStyleAlert];
            [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
            [alert addAction:[UIAlertAction actionWithTitle:@"Remove source" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *a) {
                [self->_sources removeObject:source]; [NSUserDefaults.standardUserDefaults setObject:self->_sources forKey:@"PackageManagerSources-v1"];
                // Removed sources cannot remain installable through stale cache.
                self->_catalog.packages=@[]; [self refresh];
            }]]; [self presentViewController:alert animated:YES completion:nil]; return;
        }
        BOOL bigboss=(NSUInteger)path.row>_sources.count;
        UIAlertController *alert=[UIAlertController alertControllerWithTitle:bigboss ? @"Add BigBoss?" : @"Add source"
            message:@"Packages run root scripts inside your guest. Signatures are not verified. HTTP sources are unencrypted and can be altered in transit. Add only a source you trust." preferredStyle:UIAlertControllerStyleAlert];
        if (!bigboss) [alert addTextFieldWithConfigurationHandler:^(UITextField *field) { field.placeholder=@"https://example.org/repo/"; field.keyboardType=UIKeyboardTypeURL; field.autocapitalizationType=UITextAutocapitalizationTypeNone; }];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Add source" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
            NSString *base=bigboss ? @"http://apt.thebigboss.org/repofiles/cydia/" : alert.textFields.firstObject.text;
            NSURL *url=[NSURL URLWithString:base];
            if (![@[@"https",@"http"] containsObject:url.scheme.lowercaseString ?: @""] || !url.host.length || url.user || url.password || url.query || url.fragment) { [self error:VMPackageError(@"Enter an HTTP or HTTPS repository directory URL.")]; return; }
            if (![base hasSuffix:@"/"]) base=[base stringByAppendingString:@"/"];
            [self addSource:@{@"name":bigboss ? @"BigBoss (HTTP)" : url.host,@"base":base,@"index":bigboss ? [base stringByAppendingString:@"dists/stable/main/binary-iphoneos-arm/"] : base,@"allowHTTP":@([url.scheme.lowercaseString isEqual:@"http"])}];
        }]]; [self presentViewController:alert animated:YES completion:nil]; return;
    }
    NSDictionary *package=_visible[path.row]; BOOL removing=_sections.selectedSegmentIndex==1;
    [self setWorking:YES]; [self report:@"Checking the guest and resolving dependencies…"];
    dispatch_async(_queue,^{
        NSError *error=nil; NSArray *plan=nil;
        if ([self readInstalled:&error]) {
            if (removing) { if ([self->_catalog canRemove:package error:&error]) plan=@[package]; }
            else plan=[self->_catalog planInstall:package error:&error];
        }
        NSData *status=self->_status;
        dispatch_async(dispatch_get_main_queue(),^{
            [self setWorking:NO];
            if (!plan) { [self error:error ?: VMPackageError(@"Unable to plan changes.")]; return; }
            if (!plan.count) { [self error:VMPackageError(@"This version is already installed.")]; return; }
            VMPackageReview *review=[[VMPackageReview alloc] initWithStyle:UITableViewStyleInsetGrouped];
            review.changes=plan; review.action=removing ? @"Remove" : @"Install";
            review.explanation=removing ? @"Only this package will be removed. Configuration files are kept. Dependent packages are never removed automatically." :
                @"All changes, including dependencies, are listed above. Packages run root scripts inside your guest. Sources are not signature-verified. Close Cydia before continuing. A guest restart may be needed for tweaks.";
            __weak VMPackageManagerViewController *weakSelf=self;
            for (NSDictionary *p in plan) if (!removing && !p[@"SHA256"]) {
                review.explanation=[review.explanation stringByAppendingString:@"\n\nLegacy repository: at least one package has only SHA-1/MD5. These weak checksums detect corruption, not authenticity; HTTPS is required for those downloads."]; break;
            }
            review.confirm=^{ [weakSelf execute:plan removing:removing status:status]; };
            [self.navigationController pushViewController:review animated:YES];
        });
    });
}
- (void)execute:(NSArray *)plan removing:(BOOL)removing status:(NSData *)status {
    if (_busy) return;
    UIViewController *screen=[UIViewController new]; screen.title=removing ? @"Removing package" : @"Installing packages";
    _log=[[UITextView alloc] initWithFrame:CGRectZero]; _log.editable=NO;
    _log.font=[UIFont preferredFontForTextStyle:UIFontTextStyleBody]; _log.adjustsFontForContentSizeCategory=YES;
    _log.text=@"Keep S5LBox open. Do not run Cydia at the same time.\n"; screen.view=_log;
    screen.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Stop waiting" style:UIBarButtonItemStylePlain target:self action:@selector(cancel)];
    [self.navigationController pushViewController:screen animated:YES]; [self setWorking:YES];
    dispatch_async(_queue,^{
        NSError *error=nil; NSData *result=nil; NSMutableArray *archives=[NSMutableArray array];
        if (removing) result=[self->_bridge remove:plan[0][@"Package"] status:status error:&error];
        else {
            for (NSDictionary *p in plan) {
                @autoreleasepool {
                    NSURL *url=[self->_repository download:p error:&error]; if (!url) break;
                    [archives addObject:url];
                }
            }
            if (archives.count==plan.count) result=[self->_bridge install:archives status:status error:&error];
        }
        NSArray *installed=result ? [VMPackageCatalog parse:result error:&error] : nil;
        if (!result && !error) error=VMPackageError(@"No completion was received from the guest. Refresh Installed before retrying.");
        // The terminal reply alone is not enough: confirm every requested state.
        if (installed) for (NSDictionary *wanted in plan) {
            BOOL found=NO;
            for (NSDictionary *p in installed) if ([p[@"Package"] isEqual:wanted[@"Package"]] &&
                [p[@"Status"] isEqual:@"install ok installed"] && (removing || [p[@"Version"] isEqual:wanted[@"Version"]])) found=YES;
            if (found==removing) { error=VMPackageError(@"dpkg returned, but the installed state does not match the plan. Refresh Installed and check Cydia."); break; }
        }
        if (installed) { self->_catalog.installed=installed; self->_status=result; }
        dispatch_async(dispatch_get_main_queue(),^{
            [self setWorking:NO]; screen.navigationItem.rightBarButtonItem=nil;
            screen.title=error ? @"Needs attention" : @"Complete";
            [self report:error.localizedDescription ?: @"Done. The guest's package database confirms the change. Restart the guest if the tweak requires it."];
            // Only downloaded, content-addressed archives from this attempt.
            for (NSURL *url in archives) [NSFileManager.defaultManager removeItemAtURL:url error:NULL];
        });
    });
}
- (void)cancel {
    _bridge.canceled=YES; _repository.canceled=YES;
    [self report:@"Stopping the host wait. If dpkg started, it keeps running inside the guest. Refresh Installed before retrying."];
}
- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated]; if (!_busy) { _log=nil; [self filter]; }
}
@end
