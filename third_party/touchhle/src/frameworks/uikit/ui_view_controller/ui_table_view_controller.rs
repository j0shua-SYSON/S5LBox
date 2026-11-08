/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! UITableViewController's table ownership and default read-only data source.

use crate::frameworks::core_graphics::CGRect;
use crate::frameworks::foundation::NSInteger;
use crate::objc::{
    id, impl_HostObject_with_superclass, msg, msg_class, msg_super, nil, objc_classes, release,
    ClassExports, NSZonePtr,
};

struct Controller {
    superclass: super::UIViewControllerHostObject,
    style: NSInteger,
    clears_selection: bool,
}
impl_HostObject_with_superclass!(Controller);
impl Default for Controller {
    fn default() -> Self {
        Self {
            superclass: Default::default(),
            style: 0,
            clears_selection: true,
        }
    }
}

pub const CLASSES: ClassExports = objc_classes! {
(env, this, _cmd);
@implementation UITableViewController: UIViewController
+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<Controller>::default(), &mut env.mem)
}
- (id)initWithStyle:(NSInteger)style {
    let this: id = msg_super![env; this initWithNibName:nil bundle:nil];
    env.objc.borrow_mut::<Controller>(this).style = style;
    this
}
- (())loadView {
    if env.objc.borrow::<Controller>(this).superclass.nib_name != nil {
        () = msg_super![env; this loadView];
        return;
    }
    let screen: id = msg_class![env; UIScreen mainScreen];
    let frame: CGRect = msg![env; screen applicationFrame];
    let style = env.objc.borrow::<Controller>(this).style;
    let table: id = msg_class![env; UITableView alloc];
    let table: id = msg![env; table initWithFrame:frame style:style];
    () = msg![env; this setTableView:table];
    () = msg![env; table setDataSource:this];
    () = msg![env; table setDelegate:this];
    release(env, table);
}
- (id)tableView { msg![env; this view] }
- (())setTableView:(id)table { () = msg![env; this setView:table]; }
- (bool)clearsSelectionOnViewWillAppear { env.objc.borrow::<Controller>(this).clears_selection }
- (())setClearsSelectionOnViewWillAppear:(bool)clear {
    env.objc.borrow_mut::<Controller>(this).clears_selection = clear;
}
- (())viewWillAppear:(bool)animated {
    () = msg_super![env; this viewWillAppear:animated];
    let table: id = msg![env; this tableView];
    let path: id = msg![env; table indexPathForSelectedRow];
    if env.objc.borrow::<Controller>(this).clears_selection {
        () = msg![env; table deselectRowAtIndexPath:path animated:animated];
    }
    () = msg![env; table layoutSubviews];
}
- (NSInteger)numberOfSectionsInTableView:(id)_table { 1 }
- (NSInteger)tableView:(id)_table numberOfRowsInSection:(NSInteger)_section { 0 }
- (id)tableView:(id)_table cellForRowAtIndexPath:(id)_path {
    panic!("UITableViewController subclass must supply tableView:cellForRowAtIndexPath:");
}
- (())dealloc {
    let table = env.objc.borrow::<Controller>(this).superclass.view;
    if table != nil {
        () = msg![env; table setDataSource:nil];
        () = msg![env; table setDelegate:nil];
    }
    () = msg_super![env; this dealloc];
}
@end
};
