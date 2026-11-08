/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Utilities for presenting frames to the window using an abstract OpenGL ES
//! implementation.

use super::gles11_raw as gles11; // constants and types only
use super::GLES;
use crate::matrix::Matrix;
use std::time::{Duration, Instant};

/// Settings for runtime-owned screen/layer textures, not guest GL textures.
/// Apple's limited-NPOT ES 1.1 path treats a 320x480 texture with the default
/// REPEAT wrap as incomplete: drawing it becomes an untextured white quad.
pub unsafe fn configure_frame_texture(gles: &mut dyn GLES) {
    for name in [gles11::TEXTURE_WRAP_S, gles11::TEXTURE_WRAP_T] {
        gles.TexParameteri(gles11::TEXTURE_2D, name, gles11::CLAMP_TO_EDGE as _);
    }
    for name in [gles11::TEXTURE_MIN_FILTER, gles11::TEXTURE_MAG_FILTER] {
        gles.TexParameteri(gles11::TEXTURE_2D, name, gles11::LINEAR as _);
    }
}

fn frame_tex_coords(rotation: &Matrix<2>) -> [f32; 12] {
    let mut coords = [0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0];
    // Rotate about the texture's center. Rotation about (0,0) produces negative
    // coordinates in landscape, which CLAMP_TO_EDGE turns into stretched edge
    // pixels. Relying on REPEAT to wrap them breaks NPOT ES 1.1 frame textures.
    for uv in coords.chunks_exact_mut(2) {
        let transformed = rotation.transform([uv[0] - 0.5, uv[1] - 0.5]);
        uv[0] = transformed[0] + 0.5;
        uv[1] = transformed[1] + 0.5;
    }
    coords
}

pub struct FpsCounter {
    time: std::time::Instant,
    frames: u32,
}
impl FpsCounter {
    pub fn start() -> Self {
        FpsCounter {
            time: Instant::now(),
            frames: 0,
        }
    }

    pub fn count_frame(&mut self, label: std::fmt::Arguments<'_>) {
        self.frames += 1;
        let now = Instant::now();
        let duration = now - self.time;
        if duration >= Duration::from_secs(1) {
            self.time = now;
            echo!(
                "touchHLE: {} FPS: {:.2}",
                label,
                std::mem::take(&mut self.frames) as f32 / duration.as_secs_f32()
            );
        }
    }
}

/// Present the the latest frame (e.g. the app's splash screen or rendering
/// output), provided as a texture bound to `GL_TEXTURE_2D`, by drawing it on
/// the window. It may be rotated, scaled and/or letterboxed as necessary. The
/// virtual cursor is also drawn if it should be currently visible.
///
/// The provided context must be current.
pub unsafe fn present_frame(
    gles: &mut dyn GLES,
    viewport: (u32, u32, u32, u32),
    rotation_matrix: Matrix<2>,
    virtual_cursor_visible_at: Option<(f32, f32, bool)>,
) {
    // While this is a generic utility, it is closely tied to
    // crate::frameworks::opengles::eagl::present_renderbuffer, which handles
    // backing up and restoring OpenGL ES state that this function might touch,
    // so these need to be updated in tandem.

    use gles11::types::*;

    // Draw the quad
    gles.Viewport(
        viewport.0 as _,
        viewport.1 as _,
        viewport.2 as _,
        viewport.3 as _,
    );
    gles.ClearColor(0.0, 0.0, 0.0, 1.0);
    gles.Clear(gles11::COLOR_BUFFER_BIT | gles11::DEPTH_BUFFER_BIT | gles11::STENCIL_BUFFER_BIT);
    gles.BindBuffer(gles11::ARRAY_BUFFER, 0);
    let vertices: [f32; 12] = [
        -1.0, -1.0, -1.0, 1.0, 1.0, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, 1.0,
    ];
    gles.EnableClientState(gles11::VERTEX_ARRAY);
    gles.VertexPointer(2, gles11::FLOAT, 0, vertices.as_ptr() as *const GLvoid);
    let tex_coords = frame_tex_coords(&rotation_matrix);
    gles.EnableClientState(gles11::TEXTURE_COORD_ARRAY);
    gles.TexCoordPointer(2, gles11::FLOAT, 0, tex_coords.as_ptr() as *const GLvoid);
    gles.MatrixMode(gles11::TEXTURE);
    gles.LoadIdentity();
    gles.Enable(gles11::TEXTURE_2D);
    gles.DrawArrays(gles11::TRIANGLES, 0, 6);
    // clean this up so we don't need to worry about it in e.g. Core Animation
    gles.LoadIdentity();

    // Display virtual cursor
    if let Some((x, y, pressed)) = virtual_cursor_visible_at {
        let (vx, vy, vw, vh) = viewport;
        let x = x - vx as f32;
        let y = y - vy as f32;

        gles.DisableClientState(gles11::TEXTURE_COORD_ARRAY);
        gles.Disable(gles11::TEXTURE_2D);

        gles.Enable(gles11::BLEND);
        gles.BlendFunc(gles11::ONE, gles11::ONE_MINUS_SRC_ALPHA);
        gles.Color4f(0.0, 0.0, 0.0, if pressed { 2.0 / 3.0 } else { 1.0 / 3.0 });

        let radius = 10.0;

        let mut vertices = vertices;
        for i in (0..vertices.len()).step_by(2) {
            vertices[i] = (vertices[i] * radius + x) / (vw as f32 / 2.0) - 1.0;
            vertices[i + 1] = 1.0 - (vertices[i + 1] * radius + y) / (vh as f32 / 2.0);
        }
        gles.VertexPointer(2, gles11::FLOAT, 0, vertices.as_ptr() as *const GLvoid);
        gles.DrawArrays(gles11::TRIANGLES, 0, 6);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rotated_frame_preserves_all_four_corners_with_edge_clamping() {
        use std::f32::consts::FRAC_PI_2;
        for (turns, first) in [
            (0, [0.0, 0.0]),
            (1, [1.0, 0.0]),
            (2, [1.0, 1.0]),
            (3, [0.0, 1.0]),
        ] {
            let coords = frame_tex_coords(&Matrix::z_rotation(turns as f32 * FRAC_PI_2));
            assert!((coords[0] - first[0]).abs() < 0.000001);
            assert!((coords[1] - first[1]).abs() < 0.000001);
            let corners = coords
                .chunks_exact(2)
                .map(|uv| {
                    assert!(uv
                        .iter()
                        .all(|value| *value >= -0.000001 && *value <= 1.000001));
                    (uv[0].round() as i32, uv[1].round() as i32)
                })
                .collect::<std::collections::HashSet<_>>();
            assert_eq!(
                corners,
                [(0, 0), (0, 1), (1, 0), (1, 1)].into_iter().collect()
            );
        }
    }
}
