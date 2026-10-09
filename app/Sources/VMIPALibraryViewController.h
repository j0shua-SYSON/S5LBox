// S5LBox -- individually installable guest IPA library. MIT licensed.
#import <UIKit/UIKit.h>

@interface VMIPALibraryViewController : UITableViewController
- (instancetype)initWithMachineName:(NSString *)name;
@property (nonatomic, copy) void (^selectionHandler)(NSURL *url);
@end
