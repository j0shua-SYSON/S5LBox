// S5LBox -- individually installable guest IPA library. MIT licensed.
#import <UIKit/UIKit.h>

@interface VMIPALibraryViewController : UITableViewController
- (instancetype)init;
@property (nonatomic, copy) void (^selectionHandler)(NSURL *url);
@end
