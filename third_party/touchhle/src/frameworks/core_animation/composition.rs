/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! The implementation of layer compositing.
//!
//! This is completely original; I don't think Apple document how this works and
//! I haven't attempted to reverse-engineer the details. As such, it probably
//! diverges wildly from what the real iPhone OS does.
#![allow(clippy::zero_ptr)] // alas, as you know, opengl

use super::ca_animation::TransitionFrame;
use super::ca_eagl_layer::find_fullscreen_eagl_layer;
use super::ca_layer::CALayerHostObject;
use crate::frameworks::core_animation::animation;
use crate::frameworks::core_graphics::cg_color::CGColorHostObject;
use crate::frameworks::core_graphics::{cg_bitmap_context, cg_image, CGFloat, CGRect};
use crate::gles::gles11_raw as gles11; // constants only
use crate::gles::gles11_raw::types::*;
use crate::gles::present::{present_frame, FpsCounter};
use crate::gles::GLES; // constants only
use crate::image::Image;
use crate::matrix::Matrix;
use crate::mem::SafeWrite;
use crate::objc::{id, msg, msg_class, nil, ObjC};
use crate::Environment;
use std::collections::HashMap;
use std::rc::{Rc, Weak};
use std::time::{Duration, Instant};

#[derive(Default)]
pub(super) struct State {
    texture_framebuffer: Option<(GLuint, GLuint)>,
    recomposite_next: Option<Instant>,
    fps_counter: Option<FpsCounter>,
    misc_gl_objects: Option<MiscGlObjects>,
    previous_layers: HashMap<id, Rc<RenderLayer>>,
    textures: Vec<(GLuint, Weak<GLuint>)>,
}

#[derive(Clone)]
pub(super) struct RenderLayer {
    bounds: CGRect,
    transform: Matrix<4>,
    hidden: bool,
    opaque: bool,
    opacity: f32,
    background_color: Option<CGColorHostObject>,
    corner_radius: CGFloat,
    texture: Option<Rc<GLuint>>,
    flipped: bool,
    children: Vec<Rc<RenderLayer>>,
    transition: Option<TransitionFrame>,
}

pub(super) fn previous_layer(env: &Environment, layer: id) -> Option<Rc<RenderLayer>> {
    env.framework_state
        .core_animation
        .composition
        .previous_layers
        .get(&layer)
        .cloned()
}

pub(super) fn forget_layer(env: &mut Environment, layer: id) {
    env.framework_state
        .core_animation
        .composition
        .previous_layers
        .remove(&layer);
}

struct MiscGlObjects {
    /// Texture containing a single rounded corner.
    rounded_corner_texture: GLuint,
    /// [BASIC_SQUARE_POINTS], used as both vertex and texture co-ords for
    /// drawing simple textured quads.
    basic_square_buffer: GLuint,
    /// [FLIPPED_SQUARE_POINTS], used as texture co-ords for some textured
    /// quads.
    flipped_square_buffer: GLuint,
    /// 9-patch rounded corner texture co-ords (always the same).
    rounded_vertex_buffer: GLuint,
    /// 9-patch rounded corner vertex co-ords (varies with ratio of corner
    /// radius to overall rectangle size).
    rounded_tex_coord_buffer: GLuint,
    /// Index buffer for 9-patch (first 6 elements can be used for square).
    index_buffer: GLuint,
}

unsafe fn load_matrix(gles: &mut dyn GLES, matrix: Matrix<4>) {
    gles.LoadMatrixf(matrix.columns().as_ptr() as *const _);
}

/// For use by `NSRunLoop`: call this 60 times per second. Composites the app's
/// visible layers (i.e. UI) and presents it to the screen. Does nothing if
/// composition isn't in use or it's too soon (the latter check is skipped if
/// `force` is set to [true]).
///
/// Returns the time a recomposite is due, if any.
pub fn recomposite_if_necessary(env: &mut Environment, force: bool) -> Option<Instant> {
    let mut animation_state = animation::State::default();
    let windows = env.framework_state.uikit.ui_view.ui_window.windows.clone();
    if !windows.iter().any(|&window| !msg![env; window isHidden]) {
        log_dbg!("No visible windows, skipping composition");
        return None;
    }

    if find_fullscreen_eagl_layer(env) != nil {
        // No composition done, EAGLContext will present directly.
        // Earlier UIKit trees no longer describe the displayed frame.
        env.framework_state
            .core_animation
            .composition
            .previous_layers
            .clear();
        log_dbg!("Using CAEAGLLayer fast path, skipping composition");
        return None;
    }

    if env.options.print_fps {
        env.framework_state
            .core_animation
            .composition
            .fps_counter
            .get_or_insert_with(FpsCounter::start)
            .count_frame(format_args!("Core Animation compositor"));
    }

    let now = Instant::now();
    let interval = 1.0 / 60.0; // 60Hz
    let new_recomposite_next = if let Some(recomposite_next) = env
        .framework_state
        .core_animation
        .composition
        .recomposite_next
    {
        if !force && recomposite_next > now {
            log_dbg!("Not recompositing yet, wait {:?}", recomposite_next - now);
            return Some(recomposite_next);
        }

        // See NSTimer implementation for a discussion of what this does.
        let overdue_by = now.duration_since(recomposite_next);
        log_dbg!("Recompositing, overdue by {:?}", overdue_by);
        // TODO: Use `.div_duration_f64()` once that is stabilized.
        let advance_by = (overdue_by.as_secs_f64() / interval).max(1.0).ceil();
        assert!(advance_by == (advance_by as u32) as f64);
        let advance_by = advance_by as u32;
        if advance_by > 1 {
            log_dbg!("Warning: compositor is lagging. It is overdue by {}s and has missed {} interval(s)!", overdue_by.as_secs_f64(), advance_by - 1);
        }
        let advance_by = Duration::from_secs_f64(interval)
            .checked_mul(advance_by)
            .unwrap();
        Some(recomposite_next.checked_add(advance_by).unwrap())
    } else {
        Some(now.checked_add(Duration::from_secs_f64(interval)).unwrap())
    };
    env.framework_state
        .core_animation
        .composition
        .recomposite_next = new_recomposite_next;

    let window_layers: Vec<id> = windows
        .into_iter()
        .map(|window| {
            let layer: id = msg![env; window layer];
            // Ensure layer bitmaps are up to date.
            display_layers(env, layer);
            layer
        })
        .collect();

    env.framework_state
        .core_animation
        .composition
        .previous_layers
        .clear();
    let render_layers: Vec<_> = window_layers
        .iter()
        .map(|&layer| prepare_render_layer(env, &mut animation_state, layer))
        .collect();

    let screen_bounds: CGRect = {
        let screen: id = msg_class![env; UIScreen mainScreen];
        msg![env; screen bounds]
    };
    let scale_hack: u32 = env.options.scale_hack.get();
    let fb_width = screen_bounds.size.width as u32 * scale_hack;
    let fb_height = screen_bounds.size.height as u32 * scale_hack;
    let present_frame_args = (
        env.window().viewport(),
        env.window().rotation_matrix(),
        env.window().virtual_cursor_visible_at(),
    );

    // TODO: draw status bar if it's not hidden

    // Initial state for layer tree traversal (see composite_layer_recursive)
    let cumulative_transform = Matrix::<4>::identity();
    let opacity = 1.0;

    let window = env.window.as_mut().unwrap();
    let mut gles = window.make_internal_gl_ctx_current();

    // Set up GL objects needed for render-to-texture. We could draw directly
    // to the screen instead, but this way we can reuse the code for scaling and
    // rotating the screen and drawing the virtual cursor.
    let texture = if let Some((texture, framebuffer)) = env
        .framework_state
        .core_animation
        .composition
        .texture_framebuffer
    {
        unsafe {
            gles.BindFramebufferOES(gles11::FRAMEBUFFER_OES, framebuffer);
        };
        texture
    } else {
        let mut texture = 0;
        let mut framebuffer = 0;
        unsafe {
            gles.GenTextures(1, &mut texture);
            gles.BindTexture(gles11::TEXTURE_2D, texture);
            crate::gles::present::configure_frame_texture(gles.as_mut());
            gles.TexImage2D(
                gles11::TEXTURE_2D,
                0,
                gles11::RGBA as _,
                fb_width as _,
                fb_height as _,
                0,
                gles11::RGBA,
                gles11::UNSIGNED_BYTE,
                std::ptr::null(),
            );
            gles.TexParameteri(
                gles11::TEXTURE_2D,
                gles11::TEXTURE_MIN_FILTER,
                gles11::LINEAR as _,
            );
            gles.TexParameteri(
                gles11::TEXTURE_2D,
                gles11::TEXTURE_MAG_FILTER,
                gles11::LINEAR as _,
            );

            gles.GenFramebuffersOES(1, &mut framebuffer);
            gles.BindFramebufferOES(gles11::FRAMEBUFFER_OES, framebuffer);
            gles.FramebufferTexture2DOES(
                gles11::FRAMEBUFFER_OES,
                gles11::COLOR_ATTACHMENT0_OES,
                gles11::TEXTURE_2D,
                texture,
                0,
            );
            assert_eq!(gles.GetError(), 0);
            assert_eq!(
                gles.CheckFramebufferStatusOES(gles11::FRAMEBUFFER_OES),
                gles11::FRAMEBUFFER_COMPLETE_OES
            );
        }
        env.framework_state
            .core_animation
            .composition
            .texture_framebuffer = Some((texture, framebuffer));
        texture
    };

    // Set up various other GL objects that will be reused on every frame.
    let misc_gl_objects = env
        .framework_state
        .core_animation
        .composition
        .misc_gl_objects
        .get_or_insert_with(|| {
            let dimension = 512usize; // way larger than any reasonable corner
            let mut image = Image::from_pixel_vec(
                vec![255u8; dimension * dimension * 4],
                (dimension as _, dimension as _),
            );
            image.round_corners(dimension as _, /* four_corners: */ false, /* add_sheen: */ false);

            let mut rounded_corner_texture = 0;
            unsafe {
                gles.GenTextures(1, &mut rounded_corner_texture);
                gles.BindTexture(gles11::TEXTURE_2D, rounded_corner_texture);
                // GENERATE_MIPMAP must be set before the texture upload.
                gles.TexParameteri(
                    gles11::TEXTURE_2D,
                    gles11::GENERATE_MIPMAP,
                    gles11::TRUE as _,
                );
                upload_rgba8_pixels(gles.as_mut(), image.pixels(), (dimension as _, dimension as _));
                gles.TexParameteri(
                    gles11::TEXTURE_2D,
                    gles11::TEXTURE_MIN_FILTER,
                    gles11::LINEAR_MIPMAP_LINEAR as _,
                );
                gles.TexParameteri(
                    gles11::TEXTURE_2D,
                    gles11::TEXTURE_WRAP_S,
                    gles11::CLAMP_TO_EDGE as _,
                );
                gles.TexParameteri(
                    gles11::TEXTURE_2D,
                    gles11::TEXTURE_WRAP_T,
                    gles11::CLAMP_TO_EDGE as _,
                );
            }

            let [basic_square_buffer, flipped_square_buffer, rounded_vertex_buffer, rounded_tex_coord_buffer, index_buffer] = unsafe {
                let mut array_buffers = [0; 5];
                gles.GenBuffers(5, array_buffers.as_mut_ptr());
                array_buffers
            };
            unsafe {
                gles.BindBuffer(gles11::ARRAY_BUFFER, basic_square_buffer);
                upload_slice(gles.as_mut(), gles11::ARRAY_BUFFER, &BASIC_SQUARE_POINTS, gles11::STATIC_DRAW);
                gles.BindBuffer(gles11::ARRAY_BUFFER, flipped_square_buffer);
                upload_slice(gles.as_mut(), gles11::ARRAY_BUFFER, &FLIPPED_SQUARE_POINTS, gles11::STATIC_DRAW);
                gles.BindBuffer(gles11::ARRAY_BUFFER, rounded_vertex_buffer);
                upload_slice(gles.as_mut(), gles11::ARRAY_BUFFER, &[0f32; FLOATS_PER_9PATCH], gles11::DYNAMIC_DRAW);
                gles.BindBuffer(gles11::ARRAY_BUFFER, rounded_tex_coord_buffer);
                upload_slice(
                    gles.as_mut(),
                    gles11::ARRAY_BUFFER,
                    &make_9patch_coords([0.0, 1.0, 1.0, 0.0], [0.0, 1.0, 1.0, 0.0]),
                    gles11::STATIC_DRAW,
                );
                // Prevent accidental subsequent use.
                gles.BindBuffer(gles11::ARRAY_BUFFER, 0);

                gles.BindBuffer(gles11::ELEMENT_ARRAY_BUFFER, index_buffer);
                upload_slice(gles.as_mut(), gles11::ELEMENT_ARRAY_BUFFER, &make_9patch_indices(), gles11::STATIC_DRAW);
                // Prevent accidental subsequent use.
                gles.BindBuffer(gles11::ELEMENT_ARRAY_BUFFER, 0);
            }

            MiscGlObjects {
                rounded_corner_texture,
                basic_square_buffer,
                flipped_square_buffer,
                rounded_vertex_buffer,
                rounded_tex_coord_buffer,
                index_buffer,
            }
        });

    // Clear the framebuffer and set up state to prepare for rendering
    unsafe {
        gles.Disable(gles11::SCISSOR_TEST);
        gles.Viewport(0, 0, fb_width as _, fb_height as _);
        gles.ClearColor(0.0, 0.0, 0.0, 1.0);
        gles.Clear(gles11::COLOR_BUFFER_BIT);
        gles.Color4f(1.0, 1.0, 1.0, 1.0);

        gles.MatrixMode(gles11::PROJECTION);
        // Scale down screen-space to normalized device co-ordinates, shift the
        // origin to be at the top-left rather than the center, and flip the
        // Y axis (OpenGL's points up, Core Animation's points down).
        // Using the projection matrix for this is more convenient than adding
        // an extra multiply to composite_layer_recursive.
        load_matrix(
            gles.as_mut(),
            Matrix::from(&Matrix::scale_2d(
                2.0 / screen_bounds.size.width,
                -2.0 / screen_bounds.size.height,
            ))
            .multiply(&Matrix::translate_3d(-1.0, 1.0, 0.0)),
        );
        gles.MatrixMode(gles11::MODELVIEW);
        gles.LoadIdentity();

        // One index buffer to rule them all
        gles.BindBuffer(gles11::ELEMENT_ARRAY_BUFFER, misc_gl_objects.index_buffer);
    }
    std::mem::drop(gles);

    // Render trees retain the exact texture versions used by outgoing content.
    // Only textures referenced by a transition need copying on later updates.
    let window = env.window.as_mut().unwrap();
    let mut gles = window.make_internal_gl_ctx_current();
    let state = &mut env.framework_state.core_animation.composition;
    state.textures.retain(|(texture, owner)| {
        if owner.strong_count() == 0 {
            unsafe {
                gles.DeleteTextures(1, texture);
            }
            false
        } else {
            true
        }
    });
    let misc = state.misc_gl_objects.as_ref().unwrap();
    for layer in render_layers {
        unsafe {
            composite_render_layer(
                gles.as_mut(),
                misc,
                &layer,
                cumulative_transform,
                opacity,
                scale_hack,
                fb_height,
                None,
                [0.0, 0.0],
                false,
            );
        }
    }
    std::mem::drop(gles);

    // Re-borrow
    let window = env.window.as_mut().unwrap();
    let mut gles = window.make_internal_gl_ctx_current();

    // Clean up some GL state
    unsafe {
        gles.Disable(gles11::SCISSOR_TEST);
        gles.Viewport(0, 0, fb_width as _, fb_height as _);
        gles.Color4f(1.0, 1.0, 1.0, 1.0);
        gles.Disable(gles11::BLEND);
        gles.MatrixMode(gles11::PROJECTION);
        gles.LoadIdentity();
        gles.MatrixMode(gles11::MODELVIEW);
        gles.LoadIdentity();
        gles.BindBuffer(gles11::ARRAY_BUFFER, 0);
        gles.BindBuffer(gles11::ELEMENT_ARRAY_BUFFER, 0);
        assert_eq!(gles.GetError(), 0);
    }

    // Present our rendered frame (bound to TEXTURE_2D). This copies it to the
    // default framebuffer (0) so we need to unbind our internal framebuffer.
    unsafe {
        gles.BindTexture(gles11::TEXTURE_2D, texture);
        gles.BindFramebufferOES(gles11::FRAMEBUFFER_OES, 0);
        present_frame(
            gles.as_mut(),
            present_frame_args.0,
            present_frame_args.1,
            present_frame_args.2,
        );
    }
    std::mem::drop(gles);
    window.swap_window();

    animation_state.update_started_and_finished_animations(env);

    new_recomposite_next
}

/// Call `displayIfNeeded` on all relevant layers in the tree, so their bitmaps
/// are up to date before compositing.
fn display_layers(env: &mut Environment, root_layer: id) {
    // Tell layers to redraw themselves if needed.

    fn traverse(objc: &ObjC, layer: id, layers_needing_display: &mut Vec<id>) {
        let host_obj = objc.borrow::<CALayerHostObject>(layer);
        if host_obj.hidden {
            return;
        }
        if host_obj.needs_display {
            layers_needing_display.push(layer);
        }
        for &layer in &host_obj.sublayers {
            traverse(objc, layer, layers_needing_display);
        }
    }

    let mut layers_needing_display = Vec::new();
    traverse(&env.objc, root_layer, &mut layers_needing_display);

    for layer in layers_needing_display {
        () = msg![env; layer displayIfNeeded];
    }
}

/// Prepare immutable visual state without holding guest object references.
fn prepare_render_layer(
    env: &mut Environment,
    animation_state: &mut animation::State,
    layer: id,
) -> Rc<RenderLayer> {
    let (mut host_obj, transition) = animation_state.create_presentation_layer(env, layer);
    let child_layers = host_obj.sublayers.clone();
    if !host_obj.hidden {
        let window = env.window.as_mut().unwrap();
        let mut gles = window.make_internal_gl_ctx_current();
        unsafe {
            let need_texture = host_obj.presented_pixels.is_some()
                || host_obj.contents != nil
                || host_obj.cg_context.is_some();
            let need_update = need_texture && !host_obj.gles_texture_is_up_to_date;

            if need_texture {
                // The model and this presentation copy are the normal two owners.
                // A retained outgoing render tree adds another owner and freezes this
                // texture version until its transition completes.
                let frozen = host_obj
                    .gles_texture
                    .as_ref()
                    .is_some_and(|texture| Rc::strong_count(texture) > 2);
                if host_obj.gles_texture.is_none() || (need_update && frozen) {
                    let mut name = 0;
                    gles.GenTextures(1, &mut name);
                    let texture = Rc::new(name);
                    env.framework_state
                        .core_animation
                        .composition
                        .textures
                        .push((name, Rc::downgrade(&texture)));
                    env.objc.borrow_mut::<CALayerHostObject>(layer).gles_texture =
                        Some(texture.clone());
                    host_obj.gles_texture = Some(texture);
                }
                gles.BindTexture(
                    gles11::TEXTURE_2D,
                    **host_obj.gles_texture.as_ref().unwrap(),
                );
            }

            // Update original layer texture with CAEAGLLayer pixels (slow path), if any
            if need_update {
                let original_host_obj = env.objc.borrow_mut::<CALayerHostObject>(layer);
                if let Some((ref mut pixels, width, height)) = original_host_obj.presented_pixels {
                    // The pixels are always RGBA, but if the layer is opaque then the
                    // alpha channel is meant to be ignored. glTexImage2D() has no
                    // option to ignore it, so let's manually set them to 255.
                    if original_host_obj.opaque {
                        let mut i = 3;
                        while i < pixels.len() {
                            pixels[i] = 255;
                            i += 4;
                        }
                    }

                    upload_rgba8_pixels(gles.as_mut(), pixels, (width, height));
                }
            }

            // Update texture with CGImageRef or CGContextRef pixels, if any
            if need_update {
                if host_obj.contents != nil {
                    let image = cg_image::borrow_image(&env.objc, host_obj.contents);

                    // No special handling for opacity is needed here: the alpha channel
                    // on an image is meaningful and won't be ignored.
                    upload_rgba8_pixels(gles.as_mut(), image.pixels(), image.dimensions());
                } else if let Some(cg_context) = host_obj.cg_context {
                    // Make sure this is in sync with the code in ca_layer.rs that
                    // sets up the context!
                    let (width, height, data) = cg_bitmap_context::get_data(&env.objc, cg_context);
                    let size = width * height * 4;
                    let pixels = env.mem.bytes_at(data.cast(), size);
                    upload_rgba8_pixels(gles.as_mut(), pixels, (width, height));
                }
            }

            if need_update {
                // Update original layer field
                env.objc
                    .borrow_mut::<CALayerHostObject>(layer)
                    .gles_texture_is_up_to_date = true;
            }
        }
    }
    let children = if host_obj.hidden {
        Vec::new()
    } else {
        child_layers
            .iter()
            .map(|&child| prepare_render_layer(env, animation_state, child))
            .collect()
    };
    let render = Rc::new(RenderLayer {
        bounds: host_obj.bounds,
        transform: host_obj.superlayer_to_layer_transform().into(),
        hidden: host_obj.hidden,
        opaque: host_obj.opaque,
        opacity: host_obj.opacity,
        background_color: host_obj.background_color,
        corner_radius: host_obj.corner_radius,
        texture: if host_obj.presented_pixels.is_some()
            || host_obj.contents != nil
            || host_obj.cg_context.is_some()
        {
            host_obj.gles_texture
        } else {
            None
        },
        flipped: host_obj.contents == nil,
        children,
        transition,
    });
    env.framework_state
        .core_animation
        .composition
        .previous_layers
        .insert(layer, render.clone());
    render
}

/// Draw a cached subtree. No guest messages or mutable guest resources are used
/// here, so removed views and changed bitmaps remain valid during a transition.
unsafe fn composite_render_layer(
    gles: &mut dyn GLES,
    misc: &MiscGlObjects,
    host_obj: &RenderLayer,
    parent_transform: Matrix<4>,
    opacity: CGFloat,
    scale: u32,
    fb_height: u32,
    clip: Option<[i32; 4]>,
    offset: [f32; 2],
    skip_transition: bool,
) {
    let local_transform =
        Matrix::translate_3d(offset[0], offset[1], 0.0).multiply(&host_obj.transform);
    let cumulative_transform = local_transform.multiply(&parent_transform);
    if !skip_transition {
        if let Some(transition) = &host_obj.transition {
            let bounds_clip = transition_clip(
                host_obj.bounds,
                cumulative_transform,
                scale,
                fb_height,
                clip,
            );
            let progress = transition.progress.clamp(0.0, 1.0);
            let direction = match transition.subtype.as_deref() {
                Some("fromLeft") => [-1.0, 0.0],
                Some("fromTop") => [0.0, -1.0],
                Some("fromBottom") => [0.0, 1.0],
                _ => [1.0, 0.0],
            };
            let displacement = [
                direction[0] * host_obj.bounds.size.width,
                direction[1] * host_obj.bounds.size.height,
            ];
            let incoming = [
                displacement[0] * (1.0 - progress),
                displacement[1] * (1.0 - progress),
            ];
            let outgoing = [-displacement[0] * progress, -displacement[1] * progress];
            let previous = transition.previous.as_deref();
            let draw_old = |gles: &mut dyn GLES, shift: [f32; 2], alpha| {
                if let Some(previous) = previous {
                    composite_render_layer(
                        gles,
                        misc,
                        previous,
                        parent_transform,
                        opacity * alpha,
                        scale,
                        fb_height,
                        bounds_clip,
                        [offset[0] + shift[0], offset[1] + shift[1]],
                        false,
                    );
                }
            };
            let draw_new = |gles: &mut dyn GLES, shift: [f32; 2], alpha| {
                composite_render_layer(
                    gles,
                    misc,
                    host_obj,
                    parent_transform,
                    opacity * alpha,
                    scale,
                    fb_height,
                    bounds_clip,
                    [offset[0] + shift[0], offset[1] + shift[1]],
                    true,
                );
            };
            match transition.kind.as_str() {
                "push" => {
                    draw_old(gles, outgoing, 1.0);
                    draw_new(gles, incoming, 1.0);
                }
                "moveIn" => {
                    draw_old(gles, [0.0, 0.0], 1.0);
                    draw_new(gles, incoming, 1.0);
                }
                "reveal" => {
                    draw_new(gles, [0.0, 0.0], 1.0);
                    draw_old(gles, outgoing, 1.0);
                }
                _ => {
                    // Opaque incoming content can blend directly over the old
                    // image. Translucent subtrees currently use per-layer alpha.
                    let opaque = !host_obj.hidden
                        && host_obj.opacity == 1.0
                        && (host_obj.opaque
                            || host_obj.background_color.is_some_and(|c| c.a == 1.0));
                    draw_old(gles, [0.0, 0.0], if opaque { 1.0 } else { 1.0 - progress });
                    draw_new(gles, [0.0, 0.0], progress);
                }
            }
            return;
        }
    }
    if host_obj.hidden {
        return;
    }
    if let Some([x, y, w, h]) = clip {
        gles.Enable(gles11::SCISSOR_TEST);
        gles.Scissor(x, y, w, h);
    } else {
        gles.Disable(gles11::SCISSOR_TEST);
    }
    let opacity = opacity * host_obj.opacity;
    let bounds = host_obj.bounds;
    gles.MatrixMode(gles11::MODELVIEW);
    load_matrix(
        gles,
        Matrix::<4>::from(&Matrix::scale_2d(bounds.size.width, bounds.size.height))
            .multiply(&Matrix::translate_3d(bounds.origin.x, bounds.origin.y, 0.0))
            .multiply(&cumulative_transform),
    );

    // Draw background color, if any
    let have_background = if let Some(background_color) = host_obj.background_color {
        let CGColorHostObject { r, g, b, a, .. } = background_color;
        gles.Color4f(
            r * a * opacity,
            g * a * opacity,
            b * a * opacity,
            a * opacity,
        );
        gles.Enable(gles11::BLEND);
        gles.BlendFunc(gles11::ONE, gles11::ONE_MINUS_SRC_ALPHA);

        let radius = host_obj.corner_radius;
        if radius == 0.0 {
            gles.Disable(gles11::TEXTURE_2D);
            gles.DisableClientState(gles11::TEXTURE_COORD_ARRAY);

            gles.EnableClientState(gles11::VERTEX_ARRAY);
            gles.BindBuffer(gles11::ARRAY_BUFFER, misc.basic_square_buffer);
            gles.VertexPointer(2, gles11::FLOAT, 0, 0 as *const GLvoid);

            gles.DrawElements(
                gles11::TRIANGLES,
                SQUARE_INDICES.len() as _,
                gles11::UNSIGNED_BYTE,
                0 as *const GLvoid,
            );
        } else {
            gles.Enable(gles11::TEXTURE_2D);
            gles.BindTexture(gles11::TEXTURE_2D, misc.rounded_corner_texture);
            gles.EnableClientState(gles11::TEXTURE_COORD_ARRAY);
            gles.BindBuffer(gles11::ARRAY_BUFFER, misc.rounded_tex_coord_buffer);
            gles.TexCoordPointer(2, gles11::FLOAT, 0, 0 as *const GLvoid);

            gles.EnableClientState(gles11::VERTEX_ARRAY);
            gles.BindBuffer(gles11::ARRAY_BUFFER, misc.rounded_vertex_buffer);
            upload_slice(
                gles,
                gles11::ARRAY_BUFFER,
                &make_9patch_coords(
                    [
                        0.0,
                        (radius / host_obj.bounds.size.width).min(0.5),
                        (1.0 - radius / host_obj.bounds.size.width).max(0.5),
                        1.0,
                    ],
                    [
                        0.0,
                        (radius / host_obj.bounds.size.height).min(0.5),
                        (1.0 - radius / host_obj.bounds.size.height).max(0.5),
                        1.0,
                    ],
                ),
                gles11::DYNAMIC_DRAW,
            );
            gles.VertexPointer(2, gles11::FLOAT, 0, 0 as *const GLvoid);

            gles.DrawElements(
                gles11::TRIANGLES,
                INDICES_PER_9PATCH as _,
                gles11::UNSIGNED_BYTE,
                0 as *const GLvoid,
            );
        };

        true
    } else {
        false
    };

    // Draw texture, if any
    if let Some(texture) = &host_obj.texture {
        gles.BindTexture(gles11::TEXTURE_2D, **texture);
        gles.Color4f(opacity, opacity, opacity, opacity);
        if opacity == 1.0 && host_obj.opaque && !have_background {
            gles.Disable(gles11::BLEND);
        } else {
            gles.Enable(gles11::BLEND);
            gles.BlendFunc(gles11::ONE, gles11::ONE_MINUS_SRC_ALPHA);
        }

        gles.EnableClientState(gles11::VERTEX_ARRAY);
        gles.BindBuffer(gles11::ARRAY_BUFFER, misc.basic_square_buffer);
        gles.VertexPointer(2, gles11::FLOAT, 0, 0 as *const GLvoid);

        gles.EnableClientState(gles11::TEXTURE_COORD_ARRAY);
        // Normal images will have top-to-bottom row order, but OpenGL ES
        // expects bottom-to-top, so flip the UVs in that case.
        gles.BindBuffer(
            gles11::ARRAY_BUFFER,
            if !host_obj.flipped {
                misc.basic_square_buffer
            } else {
                misc.flipped_square_buffer
            },
        );
        gles.TexCoordPointer(2, gles11::FLOAT, 0, 0 as *const GLvoid);
        gles.Enable(gles11::TEXTURE_2D);
        gles.DrawElements(
            gles11::TRIANGLES,
            SQUARE_INDICES.len() as _,
            gles11::UNSIGNED_BYTE,
            0 as *const GLvoid,
        );
    }

    for child in &host_obj.children {
        composite_render_layer(
            gles,
            misc,
            child,
            cumulative_transform,
            opacity,
            scale,
            fb_height,
            clip,
            [0.0, 0.0],
            false,
        );
    }
}

/// Axis-aligned clipping in framebuffer coordinates; quarter-turn guest
/// orientations preserve exact rectangular clipping.
fn transition_clip(
    bounds: CGRect,
    transform: Matrix<4>,
    scale: u32,
    fb_height: u32,
    parent: Option<[i32; 4]>,
) -> Option<[i32; 4]> {
    let corners = [
        (bounds.origin.x, bounds.origin.y),
        (bounds.origin.x + bounds.size.width, bounds.origin.y),
        (bounds.origin.x, bounds.origin.y + bounds.size.height),
        (
            bounds.origin.x + bounds.size.width,
            bounds.origin.y + bounds.size.height,
        ),
    ];
    let mut min = [f32::INFINITY; 2];
    let mut max = [f32::NEG_INFINITY; 2];
    for (x, y) in corners {
        let point = transform.transform([x, y, 0.0, 1.0]);
        let p = [
            point[0] * scale as f32,
            fb_height as f32 - point[1] * scale as f32,
        ];
        for i in 0..2 {
            min[i] = min[i].min(p[i]);
            max[i] = max[i].max(p[i]);
        }
    }
    let mut left = min[0].floor() as i32;
    let mut bottom = min[1].floor() as i32;
    let mut right = max[0].ceil() as i32;
    let mut top = max[1].ceil() as i32;
    if let Some([x, y, w, h]) = parent {
        left = left.max(x);
        bottom = bottom.max(y);
        right = right.min(x + w);
        top = top.min(y + h);
    }
    Some([left, bottom, (right - left).max(0), (top - bottom).max(0)])
}

const FLOATS_PER_POINT: usize = 2;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::frameworks::core_graphics::{CGPoint, CGSize};

    #[test]
    fn clips_transitions_in_rotated_and_nested_layer_coordinates() {
        let bounds = CGRect {
            origin: CGPoint { x: 0.0, y: 0.0 },
            size: CGSize {
                width: 100.0,
                height: 50.0,
            },
        };
        assert_eq!(
            transition_clip(bounds, Matrix::identity(), 1, 480, None),
            Some([0, 430, 100, 50])
        );
        let rotated = Matrix::from_columns([
            [0.0, 1.0, 0.0, 0.0],
            [-1.0, 0.0, 0.0, 0.0],
            [0.0, 0.0, 1.0, 0.0],
            [320.0, 0.0, 0.0, 1.0],
        ]);
        assert_eq!(
            transition_clip(bounds, rotated, 2, 960, None),
            Some([540, 760, 100, 200])
        );
        assert_eq!(
            transition_clip(bounds, Matrix::identity(), 1, 480, Some([20, 440, 60, 20])),
            Some([20, 440, 60, 20])
        );
        assert_eq!(
            transition_clip(bounds, Matrix::identity(), 1, 480, Some([200, 0, 20, 20])),
            Some([200, 430, 0, 0])
        );
    }
}
const BASIC_SQUARE_POINTS: [f32; 4 * FLOATS_PER_POINT] = [0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.0];
const SQUARE_INDICES: [u8; 6] = [0, 1, 2, 2, 1, 3];
const FLIPPED_SQUARE_POINTS: [f32; 4 * FLOATS_PER_POINT] = [0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0, 1.0];
const FLOATS_PER_9PATCH: usize = BASIC_SQUARE_POINTS.len() * 3 * 3;
const INDICES_PER_9PATCH: usize = SQUARE_INDICES.len() * 3 * 3;

fn make_9patch_coords(x_edges: [f32; 4], y_edges: [f32; 4]) -> [f32; FLOATS_PER_9PATCH] {
    let mut out_points = [0.0; FLOATS_PER_9PATCH];
    #[allow(clippy::chunks_exact_to_as_chunks)]
    for (i, out_points_chunk) in out_points
        .chunks_exact_mut(BASIC_SQUARE_POINTS.len())
        .enumerate()
    {
        let (x, y) = (i % 3, i / 3);

        for (dst_xy, src_xy) in out_points_chunk
            .chunks_exact_mut(2)
            .zip(BASIC_SQUARE_POINTS.chunks_exact(2))
        {
            let (x1, x2) = (x_edges[x], x_edges[x + 1]);
            let (y1, y2) = (y_edges[y], y_edges[y + 1]);
            dst_xy[0] = x1 + src_xy[0] * (x2 - x1);
            dst_xy[1] = y1 + src_xy[1] * (y2 - y1);
        }
    }
    out_points
}

fn make_9patch_indices() -> [u8; INDICES_PER_9PATCH] {
    let mut out_indices = [0; SQUARE_INDICES.len() * 3 * 3];
    #[allow(clippy::chunks_exact_to_as_chunks)]
    for (i, out_indices_chunk) in out_indices
        .chunks_exact_mut(SQUARE_INDICES.len())
        .enumerate()
    {
        for (out_index, in_index) in out_indices_chunk
            .iter_mut()
            .zip(SQUARE_INDICES.iter().copied())
        {
            *out_index = in_index + i as u8 * (BASIC_SQUARE_POINTS.len() / FLOATS_PER_POINT) as u8;
        }
    }
    out_indices
}

unsafe fn upload_slice<T: SafeWrite>(
    gles: &mut dyn GLES,
    target: GLenum,
    data: &[T],
    usage: GLenum,
) {
    gles.BufferData(
        target,
        std::mem::size_of_val(data) as _,
        data.as_ptr() as *const _,
        usage,
    )
}

unsafe fn upload_rgba8_pixels(gles: &mut dyn GLES, pixels: &[u8], dimensions: (u32, u32)) {
    crate::gles::present::configure_frame_texture(gles);
    gles.TexImage2D(
        gles11::TEXTURE_2D,
        0,
        gles11::RGBA as _,
        dimensions.0 as _,
        dimensions.1 as _,
        0,
        gles11::RGBA,
        gles11::UNSIGNED_BYTE,
        pixels.as_ptr() as *const _,
    );
    gles.TexParameteri(
        gles11::TEXTURE_2D,
        gles11::TEXTURE_MIN_FILTER,
        gles11::LINEAR as _,
    );
    gles.TexParameteri(
        gles11::TEXTURE_2D,
        gles11::TEXTURE_MAG_FILTER,
        gles11::LINEAR as _,
    );
}
