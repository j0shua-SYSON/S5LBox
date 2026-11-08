/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! `UIScreen`.

use crate::frameworks::core_graphics::{CGFloat, CGPoint, CGRect, CGSize};
use crate::objc::{id, msg, msg_class, objc_classes, ClassExports, TrivialHostObject};
use crate::{window::DeviceFamily, Environment};

fn screen_size(env: &Environment) -> CGSize {
    let family = env
        .window
        .as_ref()
        .map(|window| window.device_family())
        .or(env.options.device_family)
        .unwrap_or_else(|| {
            let supported = env.bundle.device_family_array();
            if supported.len() == 1 {
                supported[0]
            } else {
                DeviceFamily::iPhone
            }
        });
    let (width, height) = family.portrait_size();
    CGSize {
        width: width as f32,
        height: height as f32,
    }
}

#[derive(Default)]
pub struct State {
    main_screen: Option<id>,
    main_mode: Option<id>,
}

pub const CLASSES: ClassExports = objc_classes! {

(env, this, _cmd);

// For now this is a singleton (the only instance is returned by mainScreen),
// so there are hardcoded assumptions related to that.
@implementation UIScreen: NSObject

+ (id)mainScreen {
    if let Some(screen) = env.framework_state.uikit.ui_screen.main_screen {
        screen
    } else {
        let new = env.objc.alloc_static_object(
            this,
            Box::new(TrivialHostObject),
            &mut env.mem
        );
        env.framework_state.uikit.ui_screen.main_screen = Some(new);
        new
   }
}

// We only model just one screen (main screen).
+ (id)screens {
    let main_screen: id = msg![env; this mainScreen];
    msg_class![env; NSArray arrayWithObject:main_screen]
}

- (id)retain { this }
- (())release {}
- (id)autorelease { this }

- (id)currentMode {
    if let Some(mode) = env.framework_state.uikit.ui_screen.main_mode { return mode; }
    let class = env.objc.get_known_class("UIScreenMode", &mut env.mem);
    let mode = env.objc.alloc_static_object(class, Box::new(TrivialHostObject), &mut env.mem);
    env.framework_state.uikit.ui_screen.main_mode = Some(mode);
    mode
}
- (id)preferredMode { msg![env; this currentMode] }
- (id)availableModes {
    let mode: id = msg![env; this currentMode];
    msg_class![env; NSArray arrayWithObject:mode]
}
- (())setCurrentMode:(id)mode {
    let current: id = msg![env; this currentMode];
    assert_eq!(mode, current, "Only this screen's available modes may be selected");
}

// TODO: more accessors

- (CGRect)bounds {
    // While Apple's documentation says this changes with the interface
    // orientation, https://useyourloaf.com/blog/uiscreen-bounds-in-ios-8/ says
    // ths wasn't the case prior to iOS 8.
    CGRect {
        origin: CGPoint { x: 0.0, y: 0.0 },
        size: screen_size(env),
    }
}

- (CGRect)applicationFrame {
    // FIXME: Does this change depending on the status bar orientation?
    let mut bounds: CGRect = msg![env; this bounds];
    const STATUS_BAR_HEIGHT: f32 = 20.0;
    if !env.framework_state.uikit.ui_application.status_bar_hidden {
        bounds.origin.y += STATUS_BAR_HEIGHT;
        bounds.size.height -= STATUS_BAR_HEIGHT;
    }
    bounds
}

- (CGFloat)scale {
    // TODO: support retina
    1.0
}

@end

@implementation UIScreenMode: NSObject
- (id)retain { this }
- (())release {}
- (id)autorelease { this }
- (CGSize)size {
    screen_size(env)
}
- (CGFloat)pixelAspectRatio { 1.0 }
@end

};
