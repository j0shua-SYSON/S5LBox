// Native Cydia-style package workspace. Copyright (c) 2026 j0shua-SYSON. MIT.
#import "VMPackageManagerViewController.h"
#import "VMPackageCatalog.h"
#import "VMPackageRepository.h"
#import "VMPackageBridge.h"

static BOOL Installed(NSDictionary *p) {
    return [p[@"Status"] isEqual:@"install ok installed"] || [p[@"Status"] isEqual:@"hold ok installed"];
}
static NSString *Name(NSDictionary *p) { return p[@"Name"] ?: p[@"Package"] ?: @"Package"; }
static NSString *Summary(NSDictionary *p) { return [p[@"Description"] componentsSeparatedByString:@"\n"].firstObject ?: @""; }
static NSString *SizeText(unsigned long long bytes) {
    return [NSByteCountFormatter stringFromByteCount:(long long)bytes countStyle:NSByteCountFormatterCountStyleFile];
}
static NSString *Symbol(NSString *category) {
    NSString *s=category.lowercaseString;
    if ([s containsString:@"theme"]) return @"paintbrush.fill";
    if ([s containsString:@"tweak"]) return @"slider.horizontal.3";
    if ([s containsString:@"game"]) return @"gamecontroller.fill";
    if ([s containsString:@"network"]) return @"network";
    if ([s containsString:@"multimedia"]) return @"play.rectangle.fill";
    if ([s containsString:@"development"]) return @"hammer.fill";
    if ([s containsString:@"system"]) return @"gear";
    return @"shippingbox.fill";
}
static NSDictionary *Row(NSString *title, NSString *detail, NSString *symbol, NSString *action) {
    return @{@"title":title ?: @"", @"detail":detail ?: @"", @"symbol":symbol ?: @"", @"action":action ?: @""};
}
static NSDictionary *Section(NSString *title, NSArray *rows, NSString *footer) {
    return @{@"title":title ?: @"", @"rows":rows, @"footer":footer ?: @""};
}
static UIImage *Tile(NSString *symbol) {
    // Local category symbols, not remote repository artwork or tracking URLs.
    UIGraphicsImageRenderer *renderer=[[UIGraphicsImageRenderer alloc] initWithSize:CGSizeMake(44,44)];
    return [[renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        (void)context;
        [[UIColor colorWithRed:0.58 green:0.42 blue:0.29 alpha:0.13] setFill];
        [[UIBezierPath bezierPathWithRoundedRect:CGRectMake(0,0,44,44) cornerRadius:10] fill];
        UIImageSymbolConfiguration *config=[UIImageSymbolConfiguration configurationWithPointSize:23 weight:UIImageSymbolWeightRegular];
        UIImage *icon=[UIImage systemImageNamed:symbol withConfiguration:config] ?: [UIImage systemImageNamed:@"shippingbox.fill" withConfiguration:config];
        icon=[icon imageWithTintColor:[UIColor colorWithRed:0.58 green:0.42 blue:0.29 alpha:1] renderingMode:UIImageRenderingModeAlwaysOriginal];
        CGFloat scale=MIN(26/icon.size.width,26/icon.size.height);
        CGSize size=CGSizeMake(icon.size.width*scale,icon.size.height*scale);
        [icon drawInRect:CGRectMake((44-size.width)/2,(44-size.height)/2,size.width,size.height)];
    }] imageWithRenderingMode:UIImageRenderingModeAlwaysOriginal];
}

@class VMPackageScreen;
@interface VMPackageManagerViewController ()
- (void)render:(VMPackageScreen *)screen;
- (void)select:(NSDictionary *)row screen:(VMPackageScreen *)screen;
- (void)refresh;
- (void)closePackages;
- (void)addSource;
- (void)cancel;
@end

// A small shared table shell keeps lists, details and review screens consistent.
// All navigation/state and all mutations remain in the owning workspace.
@interface VMPackageScreen : UITableViewController <UISearchResultsUpdating>
@property (nonatomic, weak) VMPackageManagerViewController *owner;
@property (nonatomic, copy) NSString *kind;
@property (nonatomic, copy) NSString *query;
@property (nonatomic, copy) NSDictionary *context;
@property (nonatomic, copy) NSArray<NSDictionary *> *sections;
@property (nonatomic, copy) NSArray<NSDictionary *> *changes;
@property (nonatomic, copy) NSData *status;
@property (nonatomic) BOOL removing;
@property (nonatomic) BOOL detailsVisible;
@property (nonatomic, copy) NSString *operationState;
@property (nonatomic, copy) NSString *operationDetail;
@property (nonatomic, copy) NSString *log;
@property (nonatomic) NSInteger outcome; // 0 working, 1 verified, -1 attention
@end
@implementation VMPackageScreen
- (void)viewDidLoad {
    [super viewDidLoad];
    self.tableView.rowHeight=UITableViewAutomaticDimension;
    self.tableView.estimatedRowHeight=72;
    self.tableView.accessibilityIdentifier=[@"s5lbox.packages." stringByAppendingString:self.kind];
    self.tableView.keyboardDismissMode=UIScrollViewKeyboardDismissModeOnDrag;
    if ([@[@"search",@"installed",@"list"] containsObject:self.kind]) {
        UISearchController *search=[[UISearchController alloc] initWithSearchResultsController:nil];
        search.searchResultsUpdater=self; search.obscuresBackgroundDuringPresentation=NO;
        search.searchBar.placeholder=[self.kind isEqual:@"installed"] ? @"Search installed packages" : @"Packages, authors, descriptions";
        self.navigationItem.searchController=search;
        self.navigationItem.hidesSearchBarWhenScrolling=NO; self.definesPresentationContext=YES;
    }
    if ([@[@"home",@"sources",@"changes",@"installed",@"search"] containsObject:self.kind]) {
        self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Done" style:UIBarButtonItemStyleDone target:self.owner action:@selector(closePackages)];
        self.refreshControl=[UIRefreshControl new];
        [self.refreshControl addTarget:self.owner action:@selector(refresh) forControlEvents:UIControlEventValueChanged];
    }
    [self.owner render:self];
}
- (void)viewWillAppear:(BOOL)animated { [super viewWillAppear:animated]; [self.owner render:self]; }
- (void)updateSearchResultsForSearchController:(UISearchController *)search {
    self.query=search.searchBar.text ?: @""; [self.owner render:self];
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { (void)tableView; return self.sections.count; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    (void)tableView; return [self.sections[section][@"rows"] count];
}
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    (void)tableView; NSString *s=self.sections[section][@"title"]; return s.length ? s : nil;
}
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    (void)tableView; NSString *s=self.sections[section][@"footer"]; return s.length ? s : nil;
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    (void)tableView; NSDictionary *row=self.sections[path.section][@"rows"][path.row];
    UITableViewCell *cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.text=row[@"title"]; cell.detailTextLabel.text=row[@"detail"];
    cell.textLabel.font=[UIFont preferredFontForTextStyle:[row[@"hero"] boolValue] ? UIFontTextStyleTitle2 : UIFontTextStyleBody];
    cell.detailTextLabel.font=[UIFont preferredFontForTextStyle:UIFontTextStyleSubheadline];
    cell.textLabel.adjustsFontForContentSizeCategory=cell.detailTextLabel.adjustsFontForContentSizeCategory=YES;
    cell.textLabel.numberOfLines=0;
    cell.detailTextLabel.numberOfLines=row[@"package"] ? 2 : 0;
    cell.detailTextLabel.textColor=UIColor.secondaryLabelColor;
    NSString *symbol=row[@"symbol"];
    if (symbol.length) cell.imageView.image=Tile(symbol);
    BOOL action=[row[@"action"] length]>0;
    cell.accessoryType=action ? UITableViewCellAccessoryDisclosureIndicator : UITableViewCellAccessoryNone;
    cell.selectionStyle=action ? UITableViewCellSelectionStyleDefault : UITableViewCellSelectionStyleNone;
    if ([row[@"destructive"] boolValue]) cell.textLabel.textColor=UIColor.systemRedColor;
    if ([row[@"button"] boolValue]) cell.textLabel.textColor=UIColor.systemBlueColor;
    if ([row[@"spinner"] boolValue]) {
        UIActivityIndicatorView *spinner=[[UIActivityIndicatorView alloc] initWithActivityIndicatorStyle:UIActivityIndicatorViewStyleMedium];
        [spinner startAnimating]; cell.accessoryView=spinner;
    }
    if ([row[@"log"] boolValue]) {
        cell.textLabel.font=[UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
        cell.textLabel.textColor=UIColor.secondaryLabelColor;
    }
    cell.accessibilityIdentifier=row[@"identifier"];
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    [self.owner select:self.sections[path.section][@"rows"][path.row] screen:self];
}
@end

@implementation VMPackageManagerViewController {
    VMPackageCatalog *_catalog;
    VMPackageRepository *_repository;
    VMPackageBridge *_bridge;
    NSMutableArray<NSDictionary *> *_sources;
    dispatch_queue_t _queue;
    NSData *_status;
    NSString *_message;
    BOOL _busy, _idleWasDisabled;
    UIBackgroundTaskIdentifier _background;
    __weak VMPackageScreen *_operation;
}
- (instancetype)initWithTransport:(VMUSBTransport *)transport instanceID:(NSString *)identifier {
    self=[super init]; if (!self) return nil;
    _catalog=[VMPackageCatalog new]; _repository=[VMPackageRepository new];
    _bridge=[[VMPackageBridge alloc] initWithTransport:transport instanceID:identifier];
    _queue=dispatch_queue_create("com.j0shua.S5LBox.packages",DISPATCH_QUEUE_SERIAL);
    _background=UIBackgroundTaskInvalid;
    _sources=[[NSUserDefaults.standardUserDefaults arrayForKey:@"PackageManagerSources-v1"] mutableCopy];
    if (!_sources) _sources=[@[@{@"name":@"iOS 3 Party",@"base":@"https://ios3.party/",@"index":@"https://ios3.party/"},
        @{@"name":@"Saurik",@"base":@"https://apt.saurik.com/cydia/",@"index":@"https://apt.saurik.com/cydia/"}] mutableCopy];
    NSArray *cached=[NSArray arrayWithContentsOfURL:[[VMPackageRepository directory] URLByAppendingPathComponent:@"catalog.plist"]];
    NSMutableArray *active=[NSMutableArray array];
    for (NSDictionary *p in cached) for (NSDictionary *source in _sources)
        if ([p[@"_base"] isEqual:source[@"base"]]) { [active addObject:p]; break; }
    _catalog.packages=active;
    _message=@"Refresh sources to get started.";
    // On iOS 15 UITabBarController may load its view from super's init.
    // Build only after the worker queue, bridge and catalog actually exist.
    if (self.isViewLoaded) [self buildWorkspace];
    return self;
}
- (VMPackageScreen *)screen:(NSString *)kind title:(NSString *)title context:(NSDictionary *)context {
    VMPackageScreen *screen=[[VMPackageScreen alloc] initWithStyle:UITableViewStyleInsetGrouped];
    screen.owner=self; screen.kind=kind; screen.title=title; screen.context=context ?: @{};
    screen.sections=@[]; screen.query=@""; return screen;
}
- (void)viewDidLoad {
    [super viewDidLoad];
    if (_queue) [self buildWorkspace];
}
- (void)buildWorkspace {
    if (self.viewControllers.count) return;
    self.view.backgroundColor=UIColor.systemBackgroundColor;
    NSMutableArray *tabs=[NSMutableArray array];
    NSArray *specs=@[@[@"home",@"Packages",@"shippingbox"],@[@"sources",@"Sources",@"tray.2"],
        @[@"changes",@"Changes",@"arrow.down.circle"],@[@"installed",@"Installed",@"checkmark.seal"],@[@"search",@"Search",@"magnifyingglass"]];
    for (NSArray *spec in specs) {
        VMPackageScreen *screen=[self screen:spec[0] title:spec[1] context:nil];
        UINavigationController *nav=[[UINavigationController alloc] initWithRootViewController:screen];
        nav.navigationBar.prefersLargeTitles=YES;
        nav.tabBarItem=[[UITabBarItem alloc] initWithTitle:spec[1] image:[UIImage systemImageNamed:spec[2]] tag:tabs.count];
        [tabs addObject:nav];
    }
    self.viewControllers=tabs;
    __weak VMPackageManagerViewController *weakSelf=self;
    _repository.progress=^(NSString *text) { [weakSelf report:text]; };
    _bridge.progress=^(NSString *text) { [weakSelf report:text]; };
    [self refresh];
}
- (UINavigationController *)activeNavigation { return (UINavigationController *)self.selectedViewController; }
- (NSArray *)installedPackages {
    NSMutableArray *items=[NSMutableArray array];
    for (NSDictionary *p in _catalog.installed) if (Installed(p)) [items addObject:p];
    return items;
}
- (NSDictionary *)installed:(NSString *)name {
    for (NSDictionary *p in _catalog.installed) if (Installed(p) && [p[@"Package"] isEqual:name]) return p;
    return nil;
}
- (NSArray *)packagesFor:(VMPackageScreen *)screen {
    NSArray *items=[screen.kind isEqual:@"installed"] ? [self installedPackages] : _catalog.packages;
    NSMutableArray *result=[NSMutableArray array]; NSMutableSet *seen=[NSMutableSet set];
    NSArray *sorted=[items sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *a,NSDictionary *b) {
        NSComparisonResult n=[Name(a) localizedCaseInsensitiveCompare:Name(b)];
        return n ?: -VMPackageVersionCompare(a[@"Version"],b[@"Version"]);
    }];
    for (NSDictionary *p in sorted) {
        if (screen.context[@"source"] && ![p[@"_base"] isEqual:screen.context[@"source"]]) continue;
        if (screen.context[@"category"] && ![(p[@"Section"] ?: @"Other") isEqual:screen.context[@"category"]]) continue;
        if (screen.context[@"versions"] && ![p[@"Package"] isEqual:screen.context[@"versions"]]) continue;
        NSString *q=screen.query;
        if (q.length && ![Name(p) localizedCaseInsensitiveContainsString:q] && ![p[@"Package"] localizedCaseInsensitiveContainsString:q] &&
            ![p[@"Description"] localizedCaseInsensitiveContainsString:q] && ![p[@"Author"] localizedCaseInsensitiveContainsString:q]) continue;
        NSString *key=screen.context[@"versions"] ? [NSString stringWithFormat:@"%@/%@",p[@"Version"],p[@"_base"]] : p[@"Package"];
        if ([seen containsObject:key]) continue; [seen addObject:key];
        if ([screen.kind isEqual:@"changes"]) {
            NSDictionary *old=[self installed:p[@"Package"]];
            if (!old || [VMPackageCatalog protectedPackage:old] || VMPackageVersionCompare(p[@"Version"],old[@"Version"])<=0) continue;
        }
        [result addObject:p];
    }
    return result;
}
- (NSDictionary *)packageRow:(NSDictionary *)p {
    NSDictionary *old=[self installed:p[@"Package"]];
    NSString *installed=old ? [old[@"Version"] isEqual:p[@"Version"]] ? @"  •  Installed" : [@"  •  Installed: " stringByAppendingString:old[@"Version"]] : @"";
    NSString *version=[NSString stringWithFormat:@"%@%@",p[@"Version"] ?: @"",installed];
    NSMutableDictionary *row=[Row(Name(p),[NSString stringWithFormat:@"%@\n%@",Summary(p),version],Symbol(p[@"Section"]),@"package") mutableCopy];
    row[@"package"]=p; row[@"identifier"]=[@"package." stringByAppendingString:p[@"Package"]]; return row;
}
- (void)render:(VMPackageScreen *)screen {
    if (!screen.isViewLoaded) return;
    NSString *kind=screen.kind; NSMutableArray *sections=[NSMutableArray array];
    if ([@[@"home",@"sources",@"changes",@"installed",@"search"] containsObject:kind]) {
        UIBarButtonItem *refresh=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemRefresh target:self action:@selector(refresh)];
        UIBarButtonItem *cancel=[[UIBarButtonItem alloc] initWithTitle:@"Cancel" style:UIBarButtonItemStylePlain target:self action:@selector(cancel)];
        screen.navigationItem.rightBarButtonItems=_busy ? @[cancel] : [kind isEqual:@"sources"] ?
            @[[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAdd target:self action:@selector(addSource)],refresh] : @[refresh];
        screen.navigationItem.leftBarButtonItem.enabled=!_busy;
        if (!_busy) [screen.refreshControl endRefreshing];
    }
    if ([kind isEqual:@"home"]) {
        NSMutableDictionary *hero=[Row(@"S5LBox Packages",@"Tweaks and tools for iPhone OS 3",@"shippingbox.fill",nil) mutableCopy]; hero[@"hero"]=@YES;
        [sections addObject:Section(nil,@[hero],nil)];
        [sections addObject:Section(@"Discover",@[
            Row(@"All packages",@"Browse your sources",@"square.grid.2x2",@"all"),
            Row(@"Categories",@"Tweaks, themes, utilities and more",@"square.stack.3d.up",@"categories")],nil)];
        NSMutableDictionary *status=[Row(_busy ? @"Refreshing packages" : _status ? @"Connected to your guest" : @"Guest connection needed",
            _message,_status ? @"checkmark.circle" : @"cable.connector",_busy ? nil : @"refresh") mutableCopy];
        status[@"spinner"]=@(_busy);
        [sections addObject:Section(@"Your machine",@[status,
            Row(@"Installed packages",[NSString stringWithFormat:@"%lu packages",(unsigned long)[self installedPackages].count],@"checkmark.seal",@"installed"),
            Row(@"Sources",[NSString stringWithFormat:@"%lu repositories",(unsigned long)_sources.count],@"tray.2",@"sources")],
            @"Browsing and downloads run natively. Installation runs inside your guest.")];
        [sections addObject:Section(nil,@[Row(@"About package safety",@"Compatibility, sources and recovery",@"info.circle",@"safety")],nil)];
    } else if ([kind isEqual:@"sources"]) {
        NSMutableArray *rows=[NSMutableArray array];
        for (NSDictionary *source in _sources) {
            NSMutableSet *names=[NSMutableSet set];
            for (NSDictionary *p in _catalog.packages) if ([p[@"_base"] isEqual:source[@"base"]]) [names addObject:p[@"Package"]];
            NSMutableDictionary *row=[Row(source[@"name"],[NSString stringWithFormat:@"%@\n%lu packages",source[@"base"],(unsigned long)names.count],@"tray.2.fill",@"source") mutableCopy];
            row[@"source"]=source; [rows addObject:row];
        }
        [sections addObject:Section(@"Repositories",rows.count ? rows : @[Row(@"No sources yet",@"Tap + to add a repository.",@"tray",nil)],nil)];
        [sections addObject:Section(@"More sources",@[Row(@"BigBoss",@"Legacy HTTP repository",@"globe",@"bigboss")],
            @"Only add sources you trust. Repository signatures are not verified; HTTP sources can be altered in transit.")];
        [sections addObject:Section(nil,@[Row(_busy ? @"Refreshing sources" : @"Last refresh",_message,nil,nil)],nil)];
    } else if ([kind isEqual:@"categories"]) {
        NSMutableDictionary *counts=[NSMutableDictionary dictionary]; NSMutableSet *seen=[NSMutableSet set];
        for (NSDictionary *p in _catalog.packages) {
            if ([seen containsObject:p[@"Package"]]) continue; [seen addObject:p[@"Package"]];
            NSString *category=p[@"Section"] ?: @"Other"; counts[category]=@([counts[category] unsignedIntegerValue]+1);
        }
        NSMutableArray *rows=[NSMutableArray array];
        for (NSString *category in [counts.allKeys sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)]) {
            NSMutableDictionary *row=[Row(category,[NSString stringWithFormat:@"%@ packages",counts[category]],Symbol(category),@"category") mutableCopy];
            row[@"category"]=category; [rows addObject:row];
        }
        [sections addObject:Section(nil,rows.count ? rows : @[Row(@"No categories yet",@"Refresh your sources first.",@"tray",nil)],nil)];
    } else if ([@[@"search",@"installed",@"changes",@"list"] containsObject:kind]) {
        NSArray *packages=[self packagesFor:screen]; NSMutableArray *rows=[NSMutableArray array];
        for (NSDictionary *p in packages) [rows addObject:[self packageRow:p]];
        NSString *empty=[kind isEqual:@"changes"] ? @"No available updates" : @"No packages found";
        NSString *hint=screen.query.length ? @"Try another name or description." : [kind isEqual:@"changes"] ? @"Refresh to check your sources. Core packages are managed in Cydia." : @"Refresh sources or check the guest connection.";
        NSString *title=[kind isEqual:@"changes"] ? @"Available updates" : [NSString stringWithFormat:@"%lu packages",(unsigned long)packages.count];
        if ([kind isEqual:@"installed"] && !_status) [sections addObject:Section(nil,@[Row(@"Installed list unavailable",_message,@"exclamationmark.circle",@"refresh")],nil)];
        [sections addObject:Section(title,rows.count ? rows : @[Row(empty,hint,@"tray",nil)],
            [kind isEqual:@"changes"] ? @"Choose an update to review compatibility and dependencies before installing." : nil)];
        if (screen.context[@"source"]) {
            NSMutableDictionary *remove=[Row(@"Remove source",@"Installed packages will be kept.",nil,@"remove-source") mutableCopy];
            remove[@"destructive"]=@YES; [sections addObject:Section(nil,@[remove],nil)];
        }
    } else if ([kind isEqual:@"detail"]) {
        NSDictionary *p=screen.context; NSDictionary *old=[self installed:p[@"Package"]];
        BOOL available=p[@"Filename"]!=nil;
        BOOL protected=[VMPackageCatalog protectedPackage:p] || (old && [VMPackageCatalog protectedPackage:old]);
        BOOL newer=old && VMPackageVersionCompare(p[@"Version"],old[@"Version"])>0;
        BOOL older=old && VMPackageVersionCompare(p[@"Version"],old[@"Version"])<0;
        NSString *action=available ? old && [old[@"Version"] isEqual:p[@"Version"]] ? @"Installed" : newer ? @"Update" : @"Install" : @"Remove";
        UIBarButtonItem *button=[[UIBarButtonItem alloc] initWithTitle:action style:UIBarButtonItemStyleDone target:self action:@selector(packageAction)];
        button.enabled=!_busy && !protected && !older && ![action isEqual:@"Installed"];
        screen.navigationItem.rightBarButtonItem=button;
        NSMutableDictionary *hero=[[self packageRow:p] mutableCopy]; hero[@"action"]=@""; hero[@"hero"]=@YES;
        hero[@"detail"]=[NSString stringWithFormat:@"%@\n%@",p[@"Version"] ?: @"",old ? [@"Installed: " stringByAppendingString:old[@"Version"]] : @"Not installed"];
        [hero removeObjectForKey:@"package"];
        [sections addObject:Section(nil,@[hero],nil)];
        if (protected || older) [sections addObject:Section(nil,@[Row(@"Managed in Cydia",older ? @"Downgrades must be performed in the guest's package manager." : @"Core and held packages cannot be changed here.",@"lock",nil)],nil)];
        [sections addObject:Section(@"Description",@[Row(p[@"Description"] ?: @"No description provided.",nil,nil,nil)],nil)];
        NSString *author=[[p[@"Author"] ?: p[@"Maintainer"] ?: @"Not provided" componentsSeparatedByString:@"<"].firstObject stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSMutableArray *info=[NSMutableArray arrayWithArray:@[Row(@"Version",p[@"Version"],nil,nil),Row(@"Author",author,nil,nil),
            Row(@"Identifier",p[@"Package"],nil,nil),Row(@"Section",p[@"Section"] ?: @"Other",nil,nil),Row(@"Source",p[@"_source"] ?: @"Installed package",nil,nil)]];
        if (p[@"Size"]) [info addObject:Row(@"Download size",SizeText([p[@"Size"] longLongValue]),nil,nil)];
        [sections addObject:Section(@"Information",info,nil)];
        if ([p[@"Depends"] length] || [p[@"Pre-Depends"] length]) [sections addObject:Section(@"Dependencies",@[
            Row(@"Required packages",[NSString stringWithFormat:@"%@%@%@",p[@"Pre-Depends"] ?: @"",p[@"Pre-Depends"] && p[@"Depends"] ? @", " : @"",p[@"Depends"] ?: @""],nil,nil)],@"The complete changes will be shown before you confirm.")];
        NSMutableArray *actions=[NSMutableArray arrayWithObject:Row(@"Other versions",@"Choose a release from your sources",@"clock",@"versions")];
        if (old && available && !protected) { NSMutableDictionary *remove=[Row(@"Remove package",@"Keep configuration files",@"trash",@"remove-package") mutableCopy]; remove[@"destructive"]=@YES; [actions addObject:remove]; }
        [sections addObject:Section(nil,actions,@"Packages may run root scripts inside the guest. Sources are not signature-verified. A successful install does not guarantee tweak compatibility.")];
    } else if ([kind isEqual:@"review"]) {
        NSMutableArray *rows=[NSMutableArray array]; unsigned long long bytes=0; BOOL weak=NO;
        for (NSDictionary *p in screen.changes) { NSMutableDictionary *row=[[self packageRow:p] mutableCopy]; row[@"action"]=@""; [rows addObject:row]; bytes+=[p[@"Size"] longLongValue]; weak|=!p[@"SHA256"]; }
        [sections addObject:Section(screen.removing ? @"Remove" : @"Install",rows,nil)];
        [sections addObject:Section(@"Summary",@[Row([NSString stringWithFormat:@"%lu package%@",(unsigned long)rows.count,rows.count==1 ? @"" : @"s"],screen.removing ? @"Configuration files will be kept." : [SizeText(bytes) stringByAppendingString:@" to download"],nil,nil)],
            screen.removing ? @"Dependent packages are never removed automatically." : @"Dependencies are included above. Close Cydia before confirming. A guest restart may be needed.")];
        if (!screen.removing) [sections addObject:Section(@"Source trust",@[Row(@"Repository signatures are not verified",weak ? @"This plan includes legacy SHA-1/MD5 checksums. Those downloads require verified HTTPS; the checksums only detect corruption." : @"Downloads are checked against the repository's SHA-256. Add only sources you trust.",@"exclamationmark.shield",nil)],nil)];
        screen.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:@"Confirm" style:UIBarButtonItemStyleDone target:self action:@selector(confirmChanges)];
        screen.navigationItem.rightBarButtonItem.enabled=!_busy;
    } else if ([kind isEqual:@"operation"]) {
        NSMutableDictionary *hero=[Row(screen.operationState,screen.operationDetail,screen.outcome>0 ? @"checkmark.circle.fill" : screen.outcome<0 ? @"exclamationmark.triangle.fill" : @"shippingbox.fill",nil) mutableCopy];
        hero[@"hero"]=@YES; hero[@"spinner"]=@(screen.outcome==0);
        [sections addObject:Section(nil,@[hero],screen.outcome==0 ? @"Keep S5LBox open. Do not run Cydia or power off the guest." : nil)];
        NSMutableArray *rows=[NSMutableArray array];
        for (NSDictionary *p in screen.changes) [rows addObject:Row(Name(p),p[@"Version"],Symbol(p[@"Section"]),nil)];
        [sections addObject:Section(screen.removing ? @"Removal" : @"Installation",rows,nil)];
        [sections addObject:Section(nil,@[Row(screen.detailsVisible ? @"Hide details" : @"Show details",@"Download and guest installer output",@"text.alignleft",@"details")],nil)];
        if (screen.detailsVisible) { NSMutableDictionary *log=[Row(screen.log ?: @"Waiting for output…",nil,nil,nil) mutableCopy]; log[@"log"]=@YES; [sections addObject:Section(nil,@[log],nil)]; }
        screen.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithTitle:screen.outcome ? @"Done" : @"Stop waiting" style:UIBarButtonItemStyleDone target:self action:screen.outcome ? @selector(finishOperation) : @selector(cancel)];
    }
    screen.sections=sections; [screen.tableView reloadData];
}
- (void)renderAll {
    for (UINavigationController *nav in self.viewControllers) for (VMPackageScreen *screen in nav.viewControllers) [self render:screen];
    NSArray *updates=[self packagesFor:(VMPackageScreen *)[(UINavigationController *)self.viewControllers[2] viewControllers].firstObject];
    self.viewControllers[2].tabBarItem.badgeValue=updates.count ? [NSString stringWithFormat:@"%lu",(unsigned long)updates.count] : nil;
}
- (void)push:(VMPackageScreen *)screen { [self.activeNavigation pushViewController:screen animated:YES]; }
- (void)select:(NSDictionary *)row screen:(VMPackageScreen *)screen {
    NSString *action=row[@"action"]; if (!action.length) return;
    if ([action isEqual:@"details"]) { screen.detailsVisible=!screen.detailsVisible; [self render:screen]; return; }
    if (_busy) return;
    [screen.view endEditing:YES];
    if ([action isEqual:@"package"]) [self push:[self screen:@"detail" title:Name(row[@"package"]) context:row[@"package"]]];
    else if ([action isEqual:@"all"]) [self push:[self screen:@"list" title:@"All packages" context:nil]];
    else if ([action isEqual:@"categories"]) [self push:[self screen:@"categories" title:@"Categories" context:nil]];
    else if ([action isEqual:@"category"]) [self push:[self screen:@"list" title:row[@"category"] context:@{@"category":row[@"category"]}]];
    else if ([action isEqual:@"source"]) [self push:[self screen:@"list" title:row[@"source"][@"name"] context:@{@"source":row[@"source"][@"base"],@"sourceRecord":row[@"source"]}]];
    else if ([action isEqual:@"versions"]) [self push:[self screen:@"list" title:@"Versions" context:@{@"versions":screen.context[@"Package"]}]];
    else if ([action isEqual:@"installed"]) self.selectedIndex=3;
    else if ([action isEqual:@"sources"]) self.selectedIndex=1;
    else if ([action isEqual:@"refresh"]) [self refresh];
    else if ([action isEqual:@"bigboss"]) [self sourcePrompt:YES];
    else if ([action isEqual:@"remove-source"]) [self removeSource:screen.context[@"sourceRecord"]];
    else if ([action isEqual:@"remove-package"]) [self plan:[self installed:screen.context[@"Package"]] removing:YES];
    else if ([action isEqual:@"safety"]) [self alert:@"Package safety" message:@"Only add trusted sources: packages can execute root scripts inside the guest. Repository signatures are not verified. Core changes, downgrades and unsupported dependency plans still require Cydia.\n\nAfter an interrupted install, refresh Installed before retrying. Some tweaks need a guest restart. Stop waiting does not stop guest dpkg."];
}
- (void)alert:(NSString *)title message:(NSString *)message {
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleCancel handler:nil]];
    [self.activeNavigation.topViewController presentViewController:alert animated:YES completion:nil];
}
- (void)closePackages { if (!_busy) [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)setWorking:(BOOL)busy {
    _busy=busy; self.modalInPresentation=busy;
    for (UINavigationController *nav in self.viewControllers) {
        nav.tabBarItem.enabled=!busy; nav.interactivePopGestureRecognizer.enabled=!busy;
        nav.topViewController.navigationItem.hidesBackButton=busy;
        nav.viewControllers.firstObject.navigationItem.leftBarButtonItem.enabled=!busy;
        nav.topViewController.navigationItem.searchController.searchBar.userInteractionEnabled=!busy;
    }
    if (busy) {
        _bridge.canceled=NO; _repository.canceled=NO;
        _idleWasDisabled=UIApplication.sharedApplication.idleTimerDisabled; UIApplication.sharedApplication.idleTimerDisabled=YES;
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
- (void)report:(NSString *)message {
    dispatch_async(dispatch_get_main_queue(),^{
        self->_message=message;
        VMPackageScreen *op=self->_operation;
        if (op) {
            NSString *text=[(op.log ?: @"") stringByAppendingFormat:@"\n%@",message];
            op.log=text.length>32768 ? [text substringFromIndex:text.length-32768] : text;
            if (!op.outcome) {
                if ([message hasPrefix:@"Downloading"]) { op.operationState=@"Downloading packages"; op.operationDetail=message; }
                else if ([message hasPrefix:@"Transferred"]) { op.operationState=@"Sending to your guest"; op.operationDetail=@"Package download verified."; }
                else if ([message containsString:@"Running guest dpkg"]) { op.operationState=op.removing ? @"Removing package" : @"Installing packages"; op.operationDetail=@"Your guest is applying the changes."; }
            }
            [self render:op];
        } else [self renderAll];
    });
}
- (BOOL)readInstalled:(NSError **)error {
    NSData *status=[_bridge status:error]; if (!status) return NO;
    NSArray *records=[VMPackageCatalog parse:status error:error]; if (!records) return NO;
    dispatch_sync(dispatch_get_main_queue(),^{ self->_status=status; self->_catalog.installed=records; });
    return YES;
}
- (void)refresh {
    if (!_queue || _busy) return; _operation=nil; [self setWorking:YES]; [self report:@"Loading repositories…"];
    NSArray *sources=[_sources copy];
    dispatch_async(_queue,^{
        NSError *guestError=nil,*repoError=nil;
        NSArray *packages=[self->_repository refresh:sources error:&repoError];
        if (packages) dispatch_sync(dispatch_get_main_queue(),^{ self->_catalog.packages=packages; });
        [self report:@"Connecting to your guest…"];
        BOOL guest=[self readInstalled:&guestError];
        dispatch_async(dispatch_get_main_queue(),^{
            [self setWorking:NO];
            if (!guest) self->_status=nil;
            self->_message=guestError.localizedDescription ?: repoError.localizedDescription ?: [NSString stringWithFormat:@"%lu package versions. Installed list is current.",(unsigned long)self->_catalog.packages.count];
            if (guestError && repoError) self->_message=[NSString stringWithFormat:@"%@\nSources: %@",guestError.localizedDescription,repoError.localizedDescription];
            [self renderAll];
        });
    });
}
- (void)addSource { [self sourcePrompt:NO]; }
- (void)sourcePrompt:(BOOL)bigboss {
    if (_busy) return;
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:bigboss ? @"Add BigBoss?" : @"Add source"
        message:bigboss ? @"This legacy source uses unencrypted HTTP. Downloads can be altered in transit. Add only if you trust it." : @"Enter a flat repository URL. Packages can run root scripts inside the guest. Signatures are not verified." preferredStyle:UIAlertControllerStyleAlert];
    if (!bigboss) [alert addTextFieldWithConfigurationHandler:^(UITextField *field) { field.placeholder=@"https://example.org/repo/"; field.keyboardType=UIKeyboardTypeURL; field.autocapitalizationType=UITextAutocapitalizationTypeNone; field.autocorrectionType=UITextAutocorrectionTypeNo; }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Add source" style:UIAlertActionStyleDefault handler:^(__unused UIAlertAction *a) {
        NSString *base=bigboss ? @"http://apt.thebigboss.org/repofiles/cydia/" : [alert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
        NSURL *url=[NSURL URLWithString:base];
        if (![@[@"https",@"http"] containsObject:url.scheme.lowercaseString ?: @""] || !url.host.length || url.user || url.password || url.query || url.fragment) { [self alert:@"Invalid source" message:@"Enter an HTTP or HTTPS repository directory URL."]; return; }
        if (![base hasSuffix:@"/"]) base=[base stringByAppendingString:@"/"];
        NSString *index=bigboss ? [base stringByAppendingString:@"dists/stable/main/binary-iphoneos-arm/"] : base;
        for (NSDictionary *old in self->_sources) if ([old[@"index"] isEqual:index]) return;
        [self->_sources addObject:@{@"name":bigboss ? @"BigBoss (HTTP)" : url.host,@"base":base,@"index":index,@"allowHTTP":@([url.scheme.lowercaseString isEqual:@"http"])}];
        [NSUserDefaults.standardUserDefaults setObject:self->_sources forKey:@"PackageManagerSources-v1"]; [self refresh];
    }]];
    [self.activeNavigation.topViewController presentViewController:alert animated:YES completion:nil];
}
- (void)removeSource:(NSDictionary *)source {
    UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Remove this source?" message:@"Installed packages will be kept." preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Remove source" style:UIAlertActionStyleDestructive handler:^(__unused UIAlertAction *a) {
        [self->_sources removeObject:source]; [NSUserDefaults.standardUserDefaults setObject:self->_sources forKey:@"PackageManagerSources-v1"];
        self->_catalog.packages=@[]; [self.activeNavigation popToRootViewControllerAnimated:YES]; [self refresh];
    }]]; [self.activeNavigation.topViewController presentViewController:alert animated:YES completion:nil];
}
- (void)packageAction {
    VMPackageScreen *screen=(VMPackageScreen *)self.activeNavigation.topViewController;
    [self plan:screen.context removing:screen.context[@"Filename"]==nil];
}
- (void)plan:(NSDictionary *)package removing:(BOOL)removing {
    if (_busy || !package) return; [self setWorking:YES]; [self report:@"Checking dependencies…"];
    dispatch_async(_queue,^{
        NSError *error=nil; NSArray *plan=nil;
        if ([self readInstalled:&error]) {
            if (removing) { if ([self->_catalog canRemove:package error:&error]) plan=@[package]; }
            else plan=[self->_catalog planInstall:package error:&error];
        }
        NSData *status=self->_status;
        dispatch_async(dispatch_get_main_queue(),^{
            [self setWorking:NO]; [self renderAll];
            if (!plan) { [self alert:@"Cannot apply these changes" message:error.localizedDescription ?: @"Unable to plan changes."]; return; }
            if (!plan.count) { [self alert:@"Already installed" message:@"This version is already installed in your guest."]; return; }
            VMPackageScreen *review=[self screen:@"review" title:@"Confirm changes" context:nil];
            review.changes=plan; review.status=status; review.removing=removing; [self push:review];
        });
    });
}
- (void)confirmChanges {
    if (_busy) return;
    VMPackageScreen *review=(VMPackageScreen *)self.activeNavigation.topViewController;
    VMPackageScreen *op=[self screen:@"operation" title:@"Package activity" context:nil];
    op.changes=review.changes; op.removing=review.removing;
    op.operationState=op.removing ? @"Preparing removal" : @"Preparing installation";
    op.operationDetail=@"Connecting to your guest…"; op.log=@"Keep S5LBox open. Do not run Cydia at the same time.\n";
    _operation=op; [self push:op]; [self setWorking:YES];
    NSArray *plan=review.changes; NSData *status=review.status; BOOL removing=review.removing;
    dispatch_async(_queue,^{
        NSError *error=nil; NSData *result=nil; NSMutableArray *archives=[NSMutableArray array];
        if (removing) result=[self->_bridge remove:plan[0][@"Package"] status:status error:&error];
        else {
            for (NSDictionary *p in plan) {
                @autoreleasepool { NSURL *url=[self->_repository download:p error:&error]; if (!url) break; [archives addObject:url]; }
            }
            if (archives.count==plan.count) result=[self->_bridge install:archives status:status error:&error];
        }
        NSArray *installed=result ? [VMPackageCatalog parse:result error:&error] : nil;
        if (!result && !error) error=VMPackageError(@"No completion was received from the guest. Refresh Installed before retrying.");
        if (installed) for (NSDictionary *wanted in plan) {
            BOOL found=NO;
            for (NSDictionary *p in installed) if ([p[@"Package"] isEqual:wanted[@"Package"]] && Installed(p) &&
                (removing || [p[@"Version"] isEqual:wanted[@"Version"]])) found=YES;
            if (found==removing) { error=VMPackageError(@"The installed state does not match the plan. Refresh Installed and check Cydia."); break; }
        }
        for (NSURL *url in archives) [NSFileManager.defaultManager removeItemAtURL:url error:NULL];
        dispatch_async(dispatch_get_main_queue(),^{
            if (installed) { self->_catalog.installed=installed; self->_status=result; }
            else if (error) self->_status=nil;
            [self setWorking:NO]; op.outcome=error ? -1 : 1;
            op.operationState=error ? @"Needs attention" : removing ? @"Removal complete" : @"Installation complete";
            op.operationDetail=error.localizedDescription ?: @"Verified in your guest. Restart the guest if the tweak requires it.";
            [self report:op.operationDetail]; [self renderAll];
        });
    });
}
- (void)finishOperation {
    if (_busy) return; _operation=nil; [self.activeNavigation popToRootViewControllerAnimated:YES]; [self renderAll];
}
- (void)cancel {
    _bridge.canceled=YES; _repository.canceled=YES;
    [self report:@"Stopping the host wait. If dpkg started, it keeps running inside the guest. Refresh Installed before retrying."];
}
@end
