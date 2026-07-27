//
//  S5LBox — VMConsoleViewController. See the header.
//
//  Copyright (c) 2026 j0shua-SYSON. MIT licensed.
//
#import "VMConsoleViewController.h"

@implementation VMConsoleViewController {
    UITextView *_view;
    UILabel    *_empty;
    BOOL        _pinnedToBottom;
}

- (instancetype)init {
    self = [super init];
    if (!self) return nil;
    self.title = @"Console";
    _text = @"";
    _pinnedToBottom = YES;
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];

    _view = [[UITextView alloc] initWithFrame:CGRectZero];
    _view.editable = NO;
    _view.font = [UIFont monospacedSystemFontOfSize:11.0
                                             weight:UIFontWeightRegular];
    _view.alwaysBounceVertical = YES;
    _view.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_view];

    /*
     * An empty console is the NORMAL state for this app, and saying so is the
     * whole reason this label exists: the built-in test guest prints a banner
     * and then almost nothing, so a blank black rectangle would read as a bug.
     */
    _empty = [[UILabel alloc] initWithFrame:CGRectZero];
    _empty.numberOfLines = 0;
    _empty.textAlignment = NSTextAlignmentCenter;
    _empty.textColor = [UIColor secondaryLabelColor];
    _empty.font = [UIFont systemFontOfSize:14.0];
    _empty.text = @"Nothing printed yet.\n\nThis is the guest's serial port — "
                  @"where a real iPhone's kernel log goes. The built-in test "
                  @"program prints a short banner and little else, so an almost "
                  @"empty console here is expected when no firmware has been "
                  @"imported. A firmware boot fills it.";
    _empty.autoresizingMask =
        UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:_empty];

    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemAction
                                                      target:self
                                                      action:@selector(share)];
    [self refresh];
}

- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    _view.frame = self.view.bounds;
    CGFloat inset = 28.0;
    _empty.frame = CGRectInset(self.view.bounds, inset, inset);
}

- (void)setText:(NSString *)text {
    _text = [text copy] ?: @"";
    [self refresh];
}

- (void)appendText:(NSString *)more {
    if (!more.length) return;
    _text = [(_text ?: @"") stringByAppendingString:more];
    [self refresh];
}

- (void)refresh {
    if (!_view) return;              /* not loaded yet; viewDidLoad will call */
    BOOL empty = (_text.length == 0);
    _empty.hidden = !empty;
    _view.hidden = empty;
    _view.text = _text;

    /* Follow the tail only while the reader is already at the bottom. Yanking
     * the view down while somebody is reading earlier output is the thing that
     * makes a live log unusable. */
    if (_pinnedToBottom && _text.length) {
        NSRange end = NSMakeRange(_text.length - 1, 1);
        [_view scrollRangeToVisible:end];
    }
}

- (void)scrollViewDidScroll:(UIScrollView *)scrollView {
    CGFloat bottom = scrollView.contentSize.height - scrollView.bounds.size.height;
    _pinnedToBottom = (scrollView.contentOffset.y >= bottom - 24.0);
}

- (void)share {
    if (!_text.length) return;
    UIActivityViewController *a =
        [[UIActivityViewController alloc] initWithActivityItems:@[ _text ]
                                          applicationActivities:nil];
    /* iPad presents this as a popover and throws without an anchor. */
    a.popoverPresentationController.barButtonItem =
        self.navigationItem.rightBarButtonItem;
    [self presentViewController:a animated:YES completion:nil];
}

@end
