/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
//! Read-only UITableView data sources, reusable cells, scrolling and selection.
//! Editing, insertion animations and indexed section navigation are not yet implemented.

use crate::frameworks::core_graphics::{CGFloat, CGPoint, CGRect, CGSize};
use crate::frameworks::foundation::{ns_array, ns_string::to_rust_string, NSInteger};
use crate::objc::{
    autorelease, id, impl_HostObject_with_superclass, msg, msg_class, msg_super, nil, objc_classes,
    release, retain, ClassExports, NSZonePtr,
};
use crate::Environment;

type Position = (NSInteger, NSInteger); // section, row

#[derive(Clone, Copy)]
struct Row {
    position: Position,
    y: CGFloat,
    height: CGFloat,
}

struct Table {
    superclass: super::UIScrollViewHostObject,
    source: id, // weak, like the UIScrollView delegate
    style: NSInteger,
    row_height: CGFloat,
    counts: Vec<NSInteger>,
    rows: Vec<Row>,
    visible: Vec<(Position, id)>, // retained by the view hierarchy
    reuse: Vec<(String, id)>,     // retained until dequeued or deallocated
    decorations: Vec<id>,         // retained by the view hierarchy
    header: id,
    footer: id,
    selected: Option<Position>,
    allows_selection: bool,
    dirty: bool,
    laying_out: bool,
    touch_start: Option<CGPoint>,
    dragged: bool,
    separator_style: NSInteger,
    separator_color: id,
}
impl_HostObject_with_superclass!(Table);
impl Default for Table {
    fn default() -> Self {
        Self {
            superclass: Default::default(),
            source: nil,
            style: 0,
            row_height: 44.0,
            counts: vec![],
            rows: vec![],
            visible: vec![],
            reuse: vec![],
            decorations: vec![],
            header: nil,
            footer: nil,
            selected: None,
            allows_selection: true,
            dirty: true,
            laying_out: false,
            touch_start: None,
            dragged: false,
            separator_style: 1,
            separator_color: nil,
        }
    }
}

#[derive(Default)]
struct Cell {
    superclass: super::super::UIViewHostObject,
    identifier: id, // retained
    style: NSInteger,
    content: id,
    text: id,
    detail: id,
    image: id,
    selection: id,
    separator: id, // subviews
    selected: bool,
    highlighted: bool,
    selection_style: NSInteger,
}
impl_HostObject_with_superclass!(Cell);

fn responds(env: &mut Environment, object: id, selector: &str) -> bool {
    let sel = env
        .objc
        .register_host_selector(selector.into(), &mut env.mem);
    msg![env; object respondsToSelector:sel]
}
fn index_path(env: &mut Environment, (section, row): Position) -> id {
    msg_class![env; NSIndexPath indexPathForRow:row inSection:section]
}
fn position(env: &mut Environment, path: id) -> Option<Position> {
    if path == nil {
        None
    } else {
        Some((msg![env; path section], msg![env; path row]))
    }
}
fn rect(y: CGFloat, width: CGFloat, height: CGFloat) -> CGRect {
    CGRect {
        origin: CGPoint { x: 0.0, y },
        size: CGSize { width, height },
    }
}
fn child(env: &mut Environment, parent: id, class: &str) -> id {
    let class = env.objc.get_known_class(class, &mut env.mem);
    let view: id = msg![env; class alloc];
    let view: id = msg![env; view initWithFrame:(CGRect::default())];
    () = msg![env; parent addSubview:view];
    release(env, view);
    view
}

fn recycle(env: &mut Environment, table: id, cell: id) {
    let identifier: id = msg![env; cell reuseIdentifier];
    if identifier != nil {
        let key = to_rust_string(env, identifier).into_owned();
        retain(env, cell);
        env.objc.borrow_mut::<Table>(table).reuse.push((key, cell));
        if env.objc.borrow::<Table>(table).reuse.len() > 128 {
            let (_, oldest) = env.objc.borrow_mut::<Table>(table).reuse.remove(0);
            release(env, oldest);
        }
    }
    () = msg![env; cell removeFromSuperview];
}

fn rebuild(env: &mut Environment, table: id) {
    let old = std::mem::take(&mut env.objc.borrow_mut::<Table>(table).visible);
    for (_, cell) in old {
        recycle(env, table, cell);
    }
    let old = std::mem::take(&mut env.objc.borrow_mut::<Table>(table).decorations);
    for view in old {
        () = msg![env; view removeFromSuperview];
    }
    let (source, delegate, height, header, footer) = {
        let t = env.objc.borrow::<Table>(table);
        (
            t.source,
            t.superclass.delegate,
            t.row_height,
            t.header,
            t.footer,
        )
    };
    let bounds: CGRect = msg![env; table bounds];
    let width = bounds.size.width;
    let header_frame: CGRect = msg![env; header frame];
    let mut y = if header == nil {
        0.0
    } else {
        header_frame.size.height
    };
    if header != nil {
        () = msg![env; header setFrame:(rect(0.0, width, y))];
    }
    let sections: NSInteger = if source == nil {
        0
    } else if responds(env, source, "numberOfSectionsInTableView:") {
        msg![env; source numberOfSectionsInTableView:table]
    } else {
        1
    };
    assert!(
        (0..=10000).contains(&sections),
        "Invalid UITableView section count"
    );
    let custom_height = responds(env, delegate, "tableView:heightForRowAtIndexPath:");
    let mut counts = Vec::new();
    let mut rows = Vec::new();
    for section in 0..sections {
        for is_header in [true, false] {
            // Section titles participate in layout even when their rows are offscreen.
            let selector = if is_header {
                "tableView:titleForHeaderInSection:"
            } else {
                "tableView:titleForFooterInSection:"
            };
            let title: id = if !responds(env, source, selector) {
                nil
            } else if is_header {
                msg![env; source tableView:table titleForHeaderInSection:section]
            } else {
                msg![env; source tableView:table titleForFooterInSection:section]
            };
            if title != nil && !to_rust_string(env, title).is_empty() {
                let label = child(env, table, "UILabel");
                () = msg![env; label setText:title];
                () = msg![env; label setFrame:(rect(y, width, 24.0))];
                let color: id = msg_class![env; UIColor lightGrayColor];
                () = msg![env; label setBackgroundColor:color];
                env.objc.borrow_mut::<Table>(table).decorations.push(label);
                y += 24.0;
            }
            if !is_header {
                continue;
            }
            let count: NSInteger = msg![env; source tableView:table numberOfRowsInSection:section];
            assert!(
                count >= 0 && rows.len().saturating_add(count as usize) <= 1_000_000,
                "Invalid UITableView row count"
            );
            counts.push(count);
            for row in 0..count {
                let position = (section, row);
                let height: CGFloat = if custom_height {
                    let path = index_path(env, position);
                    msg![env; delegate tableView:table heightForRowAtIndexPath:path]
                } else {
                    height
                };
                assert!(
                    height.is_finite() && height >= 0.0,
                    "Invalid UITableView row height"
                );
                rows.push(Row {
                    position,
                    y,
                    height,
                });
                y += height;
            }
        }
    }
    if footer != nil {
        let frame: CGRect = msg![env; footer frame];
        () = msg![env; footer setFrame:(rect(y, width, frame.size.height))];
        y += frame.size.height;
    }
    let t = env.objc.borrow_mut::<Table>(table);
    t.counts = counts;
    t.rows = rows;
    t.dirty = false;
    () = msg![env; table setContentSize:(CGSize { width, height: y })];
}

fn layout(env: &mut Environment, table: id) {
    if env.objc.borrow::<Table>(table).laying_out {
        return;
    }
    env.objc.borrow_mut::<Table>(table).laying_out = true;
    if env.objc.borrow::<Table>(table).dirty {
        rebuild(env, table);
    }
    let bounds: CGRect = msg![env; table bounds];
    let (source, delegate, selected, wanted) = {
        let t = env.objc.borrow::<Table>(table);
        let start = t
            .rows
            .partition_point(|r| r.y + r.height <= bounds.origin.y);
        let wanted: Vec<Row> = t.rows[start..]
            .iter()
            .take_while(|r| r.y < bounds.origin.y + bounds.size.height)
            .filter(|r| r.height > 0.0)
            .copied()
            .collect();
        (t.source, t.superclass.delegate, t.selected, wanted)
    };
    let previous = std::mem::take(&mut env.objc.borrow_mut::<Table>(table).visible);
    let mut visible = vec![];
    for (pos, cell) in previous {
        if wanted.iter().any(|row| row.position == pos) {
            visible.push((pos, cell));
        } else {
            recycle(env, table, cell);
        }
    }
    for row in wanted {
        let cell = if let Some((_, cell)) = visible.iter().find(|(pos, _)| *pos == row.position) {
            *cell
        } else {
            let path = index_path(env, row.position);
            let cell: id = msg![env; source tableView:table cellForRowAtIndexPath:path];
            assert!(cell != nil, "UITableView data source returned a nil cell");
            () = msg![env; table addSubview:cell];
            visible.push((row.position, cell));
            if responds(
                env,
                delegate,
                "tableView:willDisplayCell:forRowAtIndexPath:",
            ) {
                () =
                    msg![env; delegate tableView:table willDisplayCell:cell forRowAtIndexPath:path];
            }
            cell
        };
        () = msg![env; cell setFrame:(rect(row.y, bounds.size.width, row.height))];
        () = msg![env; cell setSelected:(selected == Some(row.position)) animated:false];
        () = msg![env; cell layoutSubviews];
        let line = env.objc.borrow::<Cell>(cell).separator;
        let t = env.objc.borrow::<Table>(table);
        let (style, color) = (t.separator_style, t.separator_color);
        let color = if color == nil {
            msg_class![env; UIColor lightGrayColor]
        } else {
            color
        };
        () = msg![env; line setBackgroundColor:color];
        () = msg![env; line setHidden:(style == 0)];
    }
    let t = env.objc.borrow_mut::<Table>(table);
    t.visible = visible;
    t.laying_out = false;
}

fn touch_location(env: &mut Environment, touches: id, table: id) -> CGPoint {
    let touch: id = msg![env; touches anyObject];
    msg![env; touch locationInView:table]
}

pub const CLASSES: ClassExports = objc_classes! {
(env, this, _cmd);

@implementation UITableView: UIScrollView
+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<Table>::default(), &mut env.mem)
}
- (id)initWithFrame:(CGRect)frame {
    msg![env; this initWithFrame:frame style:0i32]
}
- (id)initWithFrame:(CGRect)frame style:(NSInteger)style {
    let this: id = msg_super![env; this initWithFrame:frame];
    env.objc.borrow_mut::<Table>(this).style = style;
    let white: id = msg_class![env; UIColor whiteColor];
    () = msg![env; this setBackgroundColor:white];
    () = msg![env; this setClipsToBounds:true];
    this
}
- (NSInteger)style { env.objc.borrow::<Table>(this).style }
- (NSInteger)separatorStyle { env.objc.borrow::<Table>(this).separator_style }
- (())setSeparatorStyle:(NSInteger)style { env.objc.borrow_mut::<Table>(this).separator_style = style; }
- (id)separatorColor { env.objc.borrow::<Table>(this).separator_color }
- (())setSeparatorColor:(id)color {
    retain(env, color);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<Table>(this).separator_color, color);
    release(env, old);
}
- (id)dataSource { env.objc.borrow::<Table>(this).source }
- (())setDataSource:(id)source {
    let t = env.objc.borrow_mut::<Table>(this);
    t.source = source; t.dirty = true;
}
- (CGFloat)rowHeight { env.objc.borrow::<Table>(this).row_height }
- (())setRowHeight:(CGFloat)height {
    let t = env.objc.borrow_mut::<Table>(this);
    t.row_height = height; t.dirty = true;
}
- (bool)allowsSelection { env.objc.borrow::<Table>(this).allows_selection }
- (())setAllowsSelection:(bool)value { env.objc.borrow_mut::<Table>(this).allows_selection = value; }
- (id)tableHeaderView { env.objc.borrow::<Table>(this).header }
- (())setTableHeaderView:(id)view {
    retain(env, view);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<Table>(this).header, view);
    () = msg![env; old removeFromSuperview]; release(env, old);
    if view != nil { () = msg![env; this addSubview:view]; }
    env.objc.borrow_mut::<Table>(this).dirty = true;
}
- (id)tableFooterView { env.objc.borrow::<Table>(this).footer }
- (())setTableFooterView:(id)view {
    retain(env, view);
    let old = std::mem::replace(&mut env.objc.borrow_mut::<Table>(this).footer, view);
    () = msg![env; old removeFromSuperview]; release(env, old);
    if view != nil { () = msg![env; this addSubview:view]; }
    env.objc.borrow_mut::<Table>(this).dirty = true;
}
- (())reloadData {
    let t = env.objc.borrow_mut::<Table>(this); t.dirty = true; t.selected = None;
    layout(env, this);
}
- (())layoutSubviews { layout(env, this); }
- (())setFrame:(CGRect)frame {
    () = msg_super![env; this setFrame:frame];
    layout(env, this);
}
- (())setContentOffset:(CGPoint)offset {
    () = msg_super![env; this setContentOffset:offset];
    layout(env, this);
}
- (())setContentOffset:(CGPoint)offset animated:(bool)_animated {
    () = msg![env; this setContentOffset:offset];
}
- (NSInteger)numberOfSections {
    layout(env, this);
    env.objc.borrow::<Table>(this).counts.len() as NSInteger
}
- (NSInteger)numberOfRowsInSection:(NSInteger)section {
    layout(env, this);
    env.objc.borrow::<Table>(this).counts.get(section as usize).copied().unwrap_or(0)
}
- (id)dequeueReusableCellWithIdentifier:(id)identifier {
    let key = to_rust_string(env, identifier).into_owned();
    let queue = &mut env.objc.borrow_mut::<Table>(this).reuse;
    let Some(index) = queue.iter().position(|(name, _)| *name == key) else { return nil; };
    let (_, cell) = queue.remove(index);
    () = msg![env; cell prepareForReuse];
    autorelease(env, cell)
}
- (id)cellForRowAtIndexPath:(id)path {
    let pos = position(env, path); layout(env, this);
    env.objc.borrow::<Table>(this).visible.iter().find(|(p, _)| Some(*p) == pos).map(|(_, cell)| *cell).unwrap_or(nil)
}
- (id)indexPathForCell:(id)cell {
    let pos = env.objc.borrow::<Table>(this).visible.iter().find(|(_, c)| *c == cell).map(|(p, _)| *p);
    pos.map(|pos| index_path(env, pos)).unwrap_or(nil)
}
- (id)indexPathForRowAtPoint:(CGPoint)point {
    layout(env, this);
    let bounds: CGRect = msg![env; this bounds];
    if point.x < 0.0 || point.x >= bounds.size.width { return nil; }
    let pos = env.objc.borrow::<Table>(this).rows.iter().find(|r| point.y >= r.y && point.y < r.y + r.height).map(|r| r.position);
    pos.map(|pos| index_path(env, pos)).unwrap_or(nil)
}
- (CGRect)rectForRowAtIndexPath:(id)path {
    let pos = position(env, path); layout(env, this);
    let bounds: CGRect = msg![env; this bounds];
    env.objc.borrow::<Table>(this).rows.iter().find(|r| Some(r.position) == pos)
        .map(|r| rect(r.y, bounds.size.width, r.height)).unwrap_or_default()
}
- (id)visibleCells {
    layout(env, this);
    let cells: Vec<id> = env.objc.borrow::<Table>(this).visible.iter().map(|(_, c)| *c).collect();
    for cell in &cells { retain(env, *cell); }
    let array = ns_array::from_vec(env, cells); autorelease(env, array)
}
- (id)indexPathForSelectedRow {
    env.objc.borrow::<Table>(this).selected.map(|pos| index_path(env, pos)).unwrap_or(nil)
}
- (())selectRowAtIndexPath:(id)path animated:(bool)_animated scrollPosition:(NSInteger)scroll {
    env.objc.borrow_mut::<Table>(this).selected = position(env, path);
    if scroll != 0 && path != nil { () = msg![env; this scrollToRowAtIndexPath:path atScrollPosition:scroll animated:false]; }
    layout(env, this);
}
- (())deselectRowAtIndexPath:(id)path animated:(bool)_animated {
    let pos = position(env, path);
    if env.objc.borrow::<Table>(this).selected == pos {
        env.objc.borrow_mut::<Table>(this).selected = None;
        layout(env, this);
    }
}
- (())scrollToRowAtIndexPath:(id)path atScrollPosition:(NSInteger)scroll animated:(bool)_animated {
    let row: CGRect = msg![env; this rectForRowAtIndexPath:path];
    let bounds: CGRect = msg![env; this bounds];
    let size: CGSize = msg![env; this contentSize];
    let y = match scroll {
        1 => row.origin.y,
        2 => row.origin.y - (bounds.size.height - row.size.height) / 2.0,
        3 => row.origin.y - bounds.size.height + row.size.height,
        _ => if row.origin.y < bounds.origin.y { row.origin.y }
            else if row.origin.y + row.size.height > bounds.origin.y + bounds.size.height { row.origin.y + row.size.height - bounds.size.height }
            else { bounds.origin.y }
    };
    () = msg![env; this setContentOffset:(CGPoint { x: 0.0, y: y.min((size.height - bounds.size.height).max(0.0)).max(0.0) })];
}
- (())touchesBegan:(id)touches withEvent:(id)_event {
    let point = touch_location(env, touches, this);
    let t = env.objc.borrow_mut::<Table>(this); t.touch_start = Some(point); t.dragged = false;
}
- (())touchesMoved:(id)touches withEvent:(id)event {
    let point = touch_location(env, touches, this);
    let t = env.objc.borrow_mut::<Table>(this);
    if t.touch_start.is_some_and(|start| (point.x-start.x).abs() + (point.y-start.y).abs() > 6.0) { t.dragged = true; }
    if t.dragged { () = msg_super![env; this touchesMoved:touches withEvent:event]; }
}
- (())touchesEnded:(id)touches withEvent:(id)_event {
    let t = env.objc.borrow_mut::<Table>(this);
    let select = t.touch_start.take().is_some() && !t.dragged && t.allows_selection;
    if !select { return; }
    let point = touch_location(env, touches, this);
    let mut path: id = msg![env; this indexPathForRowAtPoint:point];
    let delegate: id = msg![env; this delegate];
    if path != nil && responds(env, delegate, "tableView:willSelectRowAtIndexPath:") {
        path = msg![env; delegate tableView:this willSelectRowAtIndexPath:path];
    }
    if path == nil { return; }
    () = msg![env; this selectRowAtIndexPath:path animated:false scrollPosition:0i32];
    if responds(env, delegate, "tableView:didSelectRowAtIndexPath:") {
        () = msg![env; delegate tableView:this didSelectRowAtIndexPath:path];
    }
}
- (())touchesCancelled:(id)_touches withEvent:(id)_event {
    env.objc.borrow_mut::<Table>(this).touch_start = None;
}
- (())dealloc {
    let t = env.objc.borrow_mut::<Table>(this);
    let (reuse, header, footer, color) = (std::mem::take(&mut t.reuse), t.header, t.footer, t.separator_color);
    for (_, cell) in reuse { release(env, cell); }
    release(env, header); release(env, footer); release(env, color);
    () = msg_super![env; this dealloc];
}
@end

@implementation UITableViewCell: UIView
+ (id)allocWithZone:(NSZonePtr)_zone {
    env.objc.alloc_object(this, Box::<Cell>::default(), &mut env.mem)
}
- (id)initWithFrame:(CGRect)frame {
    msg![env; this initWithFrame:frame reuseIdentifier:nil]
}
- (id)initWithFrame:(CGRect)frame reuseIdentifier:(id)identifier {
    let this: id = msg![env; this initWithStyle:0i32 reuseIdentifier:identifier];
    () = msg![env; this setFrame:frame]; this
}
- (id)initWithStyle:(NSInteger)style reuseIdentifier:(id)identifier {
    let this: id = msg_super![env; this initWithFrame:(rect(0.0, 320.0, 44.0))];
    let identifier: id = msg![env; identifier copy];
    let selection = child(env, this, "UIView");
    let blue: id = msg_class![env; UIColor colorWithRed:0.12f32 green:0.40f32 blue:0.85f32 alpha:1.0f32];
    () = msg![env; selection setBackgroundColor:blue];
    () = msg![env; selection setHidden:true];
    let content = child(env, this, "UIView");
    () = msg![env; content setOpaque:false];
    let text = child(env, content, "UILabel");
    let detail = child(env, content, "UILabel");
    let image = child(env, content, "UIImageView");
    let separator = child(env, this, "UIView");
    for label in [text, detail] { () = msg![env; label setOpaque:false]; }
    let font: id = msg_class![env; UIFont systemFontOfSize:14.0f32];
    () = msg![env; detail setFont:font];
    let gray: id = msg_class![env; UIColor grayColor];
    () = msg![env; detail setTextColor:gray];
    let c = env.objc.borrow_mut::<Cell>(this);
    c.identifier = identifier; c.style = style; c.content = content; c.text = text;
    c.detail = detail; c.image = image; c.selection = selection; c.separator = separator; c.selection_style = 1;
    this
}
- (id)reuseIdentifier { env.objc.borrow::<Cell>(this).identifier }
- (id)contentView { env.objc.borrow::<Cell>(this).content }
- (id)textLabel { env.objc.borrow::<Cell>(this).text }
- (id)detailTextLabel { if env.objc.borrow::<Cell>(this).style == 0 { nil } else { env.objc.borrow::<Cell>(this).detail } }
- (id)imageView { env.objc.borrow::<Cell>(this).image }
// Pre-iPhone OS 3 table-cell API forwards to the corresponding content view.
- (id)text { let label = env.objc.borrow::<Cell>(this).text; msg![env; label text] }
- (())setText:(id)text { let label = env.objc.borrow::<Cell>(this).text; () = msg![env; label setText:text]; }
- (())setFont:(id)font { let label = env.objc.borrow::<Cell>(this).text; () = msg![env; label setFont:font]; }
- (())setTextColor:(id)color { let label = env.objc.borrow::<Cell>(this).text; () = msg![env; label setTextColor:color]; }
- (id)image { let view = env.objc.borrow::<Cell>(this).image; msg![env; view image] }
- (())setImage:(id)image { let view = env.objc.borrow::<Cell>(this).image; () = msg![env; view setImage:image]; }
- (bool)isSelected { env.objc.borrow::<Cell>(this).selected }
- (())setSelected:(bool)selected { () = msg![env; this setSelected:selected animated:false]; }
- (())setSelected:(bool)selected animated:(bool)_animated {
    let c = env.objc.borrow_mut::<Cell>(this); c.selected = selected;
    let (view, hidden) = (c.selection, c.selection_style == 0 || !(c.selected || c.highlighted));
    () = msg![env; view setHidden:hidden];
}
- (bool)isHighlighted { env.objc.borrow::<Cell>(this).highlighted }
- (())setHighlighted:(bool)highlighted { () = msg![env; this setHighlighted:highlighted animated:false]; }
- (())setHighlighted:(bool)highlighted animated:(bool)_animated {
    env.objc.borrow_mut::<Cell>(this).highlighted = highlighted;
    let selected = env.objc.borrow::<Cell>(this).selected;
    () = msg![env; this setSelected:selected];
}
- (NSInteger)selectionStyle { env.objc.borrow::<Cell>(this).selection_style }
- (())setSelectionStyle:(NSInteger)style {
    env.objc.borrow_mut::<Cell>(this).selection_style = style;
    let selected = env.objc.borrow::<Cell>(this).selected;
    () = msg![env; this setSelected:selected];
}
- (())prepareForReuse {
    () = msg![env; this setHighlighted:false];
    () = msg![env; this setSelected:false];
}
- (())layoutSubviews {
    let c = env.objc.borrow::<Cell>(this);
    let (content, text, detail, image, selection, separator, style) = (c.content, c.text, c.detail, c.image, c.selection, c.separator, c.style);
    if content == nil { return; }
    let bounds: CGRect = msg![env; this bounds];
    () = msg![env; content setFrame:bounds];
    () = msg![env; selection setFrame:bounds];
    () = msg![env; separator setFrame:(rect((bounds.size.height - 1.0).max(0.0), bounds.size.width, 1.0))];
    let picture: id = msg![env; image image];
    let edge = if picture == nil { 0.0 } else { bounds.size.height.min(40.0) };
    let image_frame = CGRect { origin: CGPoint { x: 8.0, y: (bounds.size.height-edge)/2.0 }, size: CGSize { width: edge, height: edge } };
    () = msg![env; image setFrame:image_frame];
    let x = if edge > 0.0 { edge + 16.0 } else { 10.0 };
    let mut frame = CGRect { origin: CGPoint { x, y: 0.0 }, size: CGSize { width: (bounds.size.width-x-10.0).max(0.0), height: bounds.size.height } };
    if style == 3 { frame.size.height *= 0.55; }
    else if style == 1 || style == 2 { frame.size.width *= 0.45; }
    () = msg![env; text setFrame:frame];
    if style == 3 { frame.origin.y = frame.size.height; frame.size.height = bounds.size.height-frame.origin.y; }
    else { frame.origin.x += frame.size.width; frame.size.width = bounds.size.width-frame.origin.x-10.0; }
    () = msg![env; detail setFrame:frame];
    () = msg![env; detail setHidden:(style == 0)];
}
- (())dealloc {
    let identifier = env.objc.borrow::<Cell>(this).identifier;
    release(env, identifier);
    () = msg_super![env; this dealloc];
}
@end
};
