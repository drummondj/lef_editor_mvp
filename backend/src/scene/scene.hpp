#pragma once
#include "../database/database.hpp"
#include "../view_style/view_style.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace le
{
    // Selectable object references - the object kinds that currently have a
    // rendered geometric representation in an Abstract view. Extend this
    // variant as more kinds (e.g. Instance, once a Layout/placement view
    // exists) become selectable.
    using SelectionRef = std::variant<TerminalId, ObstructionId>;

    /// @brief The result of a point hit-test (UPDATES.md 7.1): which
    /// selectable object was hit, plus a copy of the specific piece of
    /// geometry that was actually hit (one RenderedShape's own Shape, not
    /// the whole Terminal/Obstruction's combined geometry - hover
    /// highlighting currently outlines just this piece; see
    /// Pipeline::hit_test_point). Stored as a geometry copy (not just
    /// `origin`) so Renderer can redraw the outline without needing Root
    /// access.
    struct HoverTarget
    {
        SelectionRef origin;
        Shape outline;
    };

    /// @brief Per-handle mutable view state: which Abstract is displayed,
    /// the viewport transform, per-layer visibility, and selection. Distinct
    /// from the persistent Root database - the pipeline reads from this,
    /// events write into it.
    class Scene
    {
    public:
        // --- Currently displayed Abstract ---
        // Switching Abstracts clears selection and hover - both hold
        // TerminalId/ObstructionId values scoped to whichever Abstract
        // they were selected/hovered in (they're plain {index,generation}
        // pool handles, not namespaced by Abstract), so leaving them set
        // after switching risks a stale reference that, at best, matches
        // nothing in the new Abstract (id from the old one simply isn't
        // present) and at worst - since Terminals/Obstructions across all
        // Abstracts share the same underlying Pool - happens to collide
        // with an unrelated object's reused pool slot in the new one,
        // highlighting/selecting the wrong shape entirely. A no-op (no
        // clear, no version bumps) if `id` is the same Abstract already
        // displayed.
        void set_current_abstract(AbstractId id)
        {
            if (id == current_abstract_)
                return;

            current_abstract_ = id;
            clear_selection();
            clear_hover();
        }
        AbstractId current_abstract() const { return current_abstract_; }

        // --- Viewport transform: pixel = (dbu - pan) * scale ---
        // pan/scale/viewport_size each bump viewport_version() - a cheap
        // change signal for callers (e.g. PipelineCache) that would
        // otherwise need to snapshot and compare these fields by value.
        void set_pan(Point pan)
        {
            pan_ = pan;
            ++viewport_version_;
        }
        Point pan() const { return pan_; }

        // Ignores non-positive values (keeps the last valid scale) rather
        // than let a bad zoom value from a caller divide-by-zero downstream
        // (e.g. the pipeline's `1px / scale` sub-pixel threshold).
        void set_scale(double pixels_per_dbu)
        {
            if (pixels_per_dbu > 0.0)
            {
                scale_ = pixels_per_dbu;
                ++viewport_version_;
            }
        }
        double scale() const { return scale_; }

        void set_viewport_size(int width_px, int height_px)
        {
            viewport_width_px_ = width_px;
            viewport_height_px_ = height_px;
            ++viewport_version_;
        }
        int viewport_width_px() const { return viewport_width_px_; }
        int viewport_height_px() const { return viewport_height_px_; }

        // Monotonic counter bumped by any of the three setters above -
        // cheap for a caller to compare instead of snapshotting pan/scale/
        // viewport size by value.
        uint64_t viewport_version() const { return viewport_version_; }

        // Fits `bbox` into the current viewport (already set via
        // set_viewport_size) with `padding_px` of margin on every side:
        // uniform scale (no stretch, bounded by whichever axis is tighter)
        // and pan centering the content. Falls back to scale 1.0 / pan
        // (0, 0) if bbox is nullopt (nothing to fit, e.g. an empty
        // Abstract) or the viewport has non-positive size, rather than
        // dividing by zero.
        void fit_to_content(std::optional<Rect> bbox, int64_t padding_px)
        {
            if (!bbox || viewport_width_px_ <= 0 || viewport_height_px_ <= 0)
            {
                set_scale(1.0);
                set_pan(Point{0, 0});
                return;
            }

            const double content_width = static_cast<double>(bbox->ur.x - bbox->ll.x);
            const double content_height = static_cast<double>(bbox->ur.y - bbox->ll.y);
            const double usable_width_px = viewport_width_px_ - 2 * padding_px;
            const double usable_height_px = viewport_height_px_ - 2 * padding_px;

            // Each axis's own limit on scale, skipped (treated as
            // unbounded) when that axis's content span is zero - a
            // degenerate single-line/point bbox shouldn't force scale to
            // infinity via a divide-by-zero.
            const double scale_x = content_width > 0 ? usable_width_px / content_width : std::numeric_limits<double>::infinity();
            const double scale_y = content_height > 0 ? usable_height_px / content_height : std::numeric_limits<double>::infinity();
            double scale = std::min(scale_x, scale_y);
            if (!std::isfinite(scale) || scale <= 0.0)
                scale = 1.0;

            // Renderer's pixel transform maps (dbu - pan) * scale to pixel
            // space, i.e. pan is the dbu point that lands at pixel (0, 0) -
            // not the viewport center. To center the content, pan is offset
            // from the bbox's own lower-left corner by half of the leftover
            // (non-content) space on each axis - using the full viewport
            // size here (not the padding-reduced usable size), since
            // padding is symmetric and cancels out of the centering offset.
            const int64_t pan_x = bbox->ll.x - static_cast<int64_t>((viewport_width_px_ / scale - content_width) / 2.0);
            const int64_t pan_y = bbox->ll.y - static_cast<int64_t>((viewport_height_px_ / scale - content_height) / 2.0);

            set_scale(scale);
            set_pan(Point{pan_x, pan_y});
        }

        // --- Grid spacing (dbu) ---
        // Defaults assume the common "1 dbu = 1nm" convention (i.e. a
        // Technology declared with DATABASE MICRONS 1000, which most real
        // PDKs use) - 5 and 50 dbu then read as the requested 5nm minor /
        // 50nm major defaults. Not otherwise unit-aware (Scene has no
        // Technology reference to convert against) - a caller on a
        // Technology with different units should set explicit dbu values.
        // Non-positive values are rejected (keeps the last valid spacing),
        // same guard as set_scale, and setters bump visibility_version()
        // since the grid is part of the rendered picture (see
        // Renderer::draw_grid) - unlike layer selectability below, this
        // does need to invalidate the render cache.
        void set_minor_grid_spacing(int64_t dbu)
        {
            if (dbu > 0)
            {
                minor_grid_spacing_ = dbu;
                ++visibility_version_;
            }
        }
        int64_t minor_grid_spacing() const { return minor_grid_spacing_; }

        void set_major_grid_spacing(int64_t dbu)
        {
            if (dbu > 0)
            {
                major_grid_spacing_ = dbu;
                ++visibility_version_;
            }
        }
        int64_t major_grid_spacing() const { return major_grid_spacing_; }

        // --- Mouse position (screen pixels) + grid-snapped dbu position ---
        // Set by the frontend on every pointer move (see le_set_mouse_position)
        // - screen/image pixel space (top-left origin, y down, matching
        // le_render_pixel_buffer()'s output and le_zoom's x/y), not
        // Renderer's own pre-Y-flip pixel space. Deliberately its own
        // version counter, not visibility_version - a mouse move must not
        // invalidate Renderer's (expensive, design-sized) rasterized
        // picture cache; see Renderer::compose_with_overlays for how the
        // mouse overlay stays cheap to redraw independently of it.
        void set_mouse_position(int32_t x_px, int32_t y_px)
        {
            mouse_x_px_ = x_px;
            mouse_y_px_ = y_px;
            has_mouse_position_ = true;
            ++mouse_version_;
        }

        // Call when the pointer leaves the viewport (or before the first
        // pointer event) so the cursor overlay stops showing a stale
        // position rather than sticking at the last-known one.
        void clear_mouse_position()
        {
            if (has_mouse_position_)
            {
                has_mouse_position_ = false;
                ++mouse_version_;
            }
        }

        bool has_mouse_position() const { return has_mouse_position_; }
        uint64_t mouse_version() const { return mouse_version_; }

        // The raw stored pixel position (screen/image space - see
        // set_mouse_position) - 0/0 if has_mouse_position() is false.
        // Exposed for callers that need the pixel coordinate itself, not
        // just its dbu equivalent (mouse_dbu_position) - e.g. a
        // keyboard-triggered zoom (le_key_down's LE_KEY_ZOOM) anchored at
        // "wherever the mouse currently is", the same anchor semantics
        // le_zoom's own x/y parameter already has.
        int32_t mouse_x_px() const { return mouse_x_px_; }
        int32_t mouse_y_px() const { return mouse_y_px_; }

        // Converts a screen/image pixel coordinate (top-left origin, y
        // down - same convention as set_mouse_position) to dbu space,
        // undoing rasterize()'s Y-flip the same way le_zoom's own
        // pixel->dbu conversion does - see render.hpp's PixelShape/
        // Renderer::rasterize comments for why pan/scale describe the
        // pre-flip transform while a screen pixel coordinate is
        // post-flip. scale_ is always positive by construction
        // (set_scale rejects non-positive values), so no divide-by-zero
        // guard is needed here. Shared by mouse_dbu_position() (the
        // currently stored mouse position) and click/drag-select
        // handling (an arbitrary x/y from a mouse-down/up event, not
        // necessarily the currently stored position - see
        // le_mouse_down/le_mouse_up).
        Point pixel_to_dbu(int32_t x_px, int32_t y_px) const
        {
            const double dbu_x = static_cast<double>(pan_.x) + static_cast<double>(x_px) / scale_;
            const double dbu_y = static_cast<double>(pan_.y) + (static_cast<double>(viewport_height_px_) - static_cast<double>(y_px)) / scale_;
            return Point{static_cast<int64_t>(dbu_x), static_cast<int64_t>(dbu_y)};
        }

        // The dbu point currently under the mouse - nullopt if no position
        // has been set yet (see has_mouse_position).
        std::optional<Point> mouse_dbu_position() const
        {
            if (!has_mouse_position_)
                return std::nullopt;

            return pixel_to_dbu(mouse_x_px_, mouse_y_px_);
        }

        // mouse_dbu_position() rounded to the nearest multiple of the
        // minor grid spacing (round-to-nearest, not truncation - a mouse
        // position exactly between two grid points snaps to whichever the
        // division rounds to). nullopt under the same conditions as
        // mouse_dbu_position().
        std::optional<Point> snapped_mouse_position() const
        {
            const std::optional<Point> dbu = mouse_dbu_position();
            if (!dbu)
                return std::nullopt;

            auto snap = [spacing = minor_grid_spacing_](int64_t v)
            {
                return static_cast<int64_t>(std::llround(static_cast<double>(v) / static_cast<double>(spacing))) * spacing;
            };
            return Point{snap(dbu->x), snap(dbu->y)};
        }

        // --- Drag-select gesture (UPDATES.md 7.1 items 5-6) ---
        // Tracks a mouse-down-to-mouse-up rubber-band gesture in screen
        // pixels (top-left origin, y down - same convention as
        // set_mouse_position). The frontend calls begin_drag on
        // mouse-down and end_drag on mouse-up (see le_mouse_down/
        // le_mouse_up), which decide there whether the gesture was a
        // plain click or an actual drag (by comparing the down/up pixel
        // distance against a small threshold) and perform the
        // corresponding selection - Scene itself doesn't know which
        // interpretation applies, it just tracks the raw gesture state
        // and derives drag_rect_dbu() from it plus the current mouse
        // position.
        void begin_drag(int32_t x_px, int32_t y_px)
        {
            dragging_ = true;
            drag_start_x_px_ = x_px;
            drag_start_y_px_ = y_px;
            ++mouse_version_; // so the drag-rect overlay (Renderer, see UPDATES.md 7.1 item 5's live rectangle) starts showing immediately
        }

        void end_drag()
        {
            dragging_ = false;
            ++mouse_version_; // so the drag-rect overlay stops showing
        }

        bool is_dragging() const { return dragging_; }
        int32_t drag_start_x_px() const { return drag_start_x_px_; }
        int32_t drag_start_y_px() const { return drag_start_y_px_; }

        // The drag rectangle in dbu space, normalized (ll <= ur regardless
        // of which direction the drag went) - nullopt if no drag is in
        // progress, or no mouse position has been set yet (the drag's
        // "current" corner - mirrors mouse_dbu_position()'s own nullopt
        // condition).
        std::optional<Rect> drag_rect_dbu() const
        {
            if (!dragging_)
                return std::nullopt;

            const std::optional<Point> current = mouse_dbu_position();
            if (!current)
                return std::nullopt;

            const Point start = pixel_to_dbu(drag_start_x_px_, drag_start_y_px_);
            return Rect{
                .ll = Point{std::min(start.x, current->x), std::min(start.y, current->y)},
                .ur = Point{std::max(start.x, current->x), std::max(start.y, current->y)},
            };
        }

        // --- Held keys (UPDATES.md 7) ---
        // A generic set of currently-held key codes, set by the frontend
        // via press_key/release_key (see le_key_down/le_key_up) on every
        // key-down/key-up event - decoupled from any specific gesture
        // (mouse clicks, future keyboard shortcuts) so those can query
        // "is X held" internally without needing modifier/key state
        // threaded through their own call's parameter list (e.g.
        // le_mouse_up reading is_key_held for shift-click/shift-drag,
        // rather than taking a shift parameter itself). Key codes are
        // opaque ints here - api.hpp's LeKeyCode enum gives them stable,
        // platform-independent meaning at the C API boundary; Scene
        // itself doesn't interpret them.
        void press_key(int32_t key_code) { held_keys_.insert(key_code); }
        void release_key(int32_t key_code) { held_keys_.erase(key_code); }
        bool is_key_held(int32_t key_code) const { return held_keys_.contains(key_code); }

        // Call when the widget/window receiving key events loses focus
        // (see le_clear_all_keys) - a key's matching release is not
        // guaranteed to still reach a widget that no longer has focus by
        // the time the physical key comes up, so without this a modifier
        // held at the moment of a focus loss would stay "held" from this
        // API's point of view indefinitely, silently changing later
        // gestures that consult it (e.g. every future click reading as
        // shift-click) until that same key happens to be pressed and
        // released again while focused.
        void clear_all_keys() { held_keys_.clear(); }

        // --- Hover (UPDATES.md 7.1) ---
        // Which selectable object (if any) the mouse currently sits over,
        // set by the frontend's pointer-move handler via a hit-test
        // against the currently rendered shapes (see le_set_mouse_position
        // and Pipeline::hit_test_point). No separate version counter,
        // unlike mouse position - set_hover/clear_hover are only ever
        // called from within le_set_mouse_position, which already
        // unconditionally bumps mouse_version_ on every call, so
        // Renderer's overlay picture (keyed on mouse_version_) already
        // redraws on every pointer move; a second counter would be
        // redundant bookkeeping for no extra invalidation precision.
        void set_hover(std::optional<HoverTarget> hover) { hovered_ = std::move(hover); }
        void clear_hover() { hovered_.reset(); }
        const std::optional<HoverTarget> &hover() const { return hovered_; }

        // --- Layer visibility (defaults to visible until toggled) ---
        // Two independent axes, deliberately *not* per-ViewLayerId: by
        // layer name (every purpose-column of that ViewLayerRow, e.g.
        // toggling "M1" off hides both M1/TERMINAL and M1/OBSTRUCTION) and
        // by purpose (every layer with that purpose, e.g. toggling
        // OBSTRUCTION off hides every layer's obstructions at once) -
        // matching a layer-visibility widget's row-header/column-header
        // checkboxes rather than one checkbox per grid cell. A given
        // ViewLayer's effective visibility is the AND of both axes - see
        // is_view_layer_visible().
        void set_layer_name_visible(std::string layer_name, bool visible)
        {
            layer_name_visible_[std::move(layer_name)] = visible;
            ++visibility_version_;
        }

        bool is_layer_name_visible(const std::string &layer_name) const
        {
            auto it = layer_name_visible_.find(layer_name);
            return it == layer_name_visible_.end() ? true : it->second;
        }

        void set_purpose_visible(ViewLayerPurpose purpose, bool visible)
        {
            purpose_visible_[purpose] = visible;
            ++visibility_version_;
        }

        bool is_purpose_visible(ViewLayerPurpose purpose) const
        {
            auto it = purpose_visible_.find(purpose);
            return it == purpose_visible_.end() ? true : it->second;
        }

        // The actual per-ViewLayer question Pipeline::filter_by_layer_visibility
        // filters on: visible only if both its layer-name axis and its
        // purpose axis are visible.
        bool is_view_layer_visible(const std::string &layer_name, ViewLayerPurpose purpose) const
        {
            return is_layer_name_visible(layer_name) && is_purpose_visible(purpose);
        }

        // Monotonic counter bumped by set_layer_name_visible/set_purpose_visible -
        // cheap for a caller to compare instead of comparing both maps by value.
        uint64_t visibility_version() const { return visibility_version_; }

        // --- Layer selectability (defaults to selectable until toggled) ---
        // Same two-axis shape as visibility above, but deliberately doesn't
        // bump any version counter: unlike visibility, nothing here caches
        // on it yet - it's consulted by a future hit-testing/click-to-select
        // path (not implemented yet), not by Pipeline/Renderer, so there's
        // no cache to invalidate.
        void set_layer_name_selectable(std::string layer_name, bool selectable)
        {
            layer_name_selectable_[std::move(layer_name)] = selectable;
        }

        bool is_layer_name_selectable(const std::string &layer_name) const
        {
            auto it = layer_name_selectable_.find(layer_name);
            return it == layer_name_selectable_.end() ? true : it->second;
        }

        void set_purpose_selectable(ViewLayerPurpose purpose, bool selectable)
        {
            purpose_selectable_[purpose] = selectable;
        }

        bool is_purpose_selectable(ViewLayerPurpose purpose) const
        {
            auto it = purpose_selectable_.find(purpose);
            return it == purpose_selectable_.end() ? true : it->second;
        }

        bool is_view_layer_selectable(const std::string &layer_name, ViewLayerPurpose purpose) const
        {
            return is_layer_name_selectable(layer_name) && is_purpose_selectable(purpose);
        }

        // --- Selection ---
        // Renderer draws a white outline around every selected shape (see
        // Renderer::build_picture/draw_selection_outline) -
        // selection_version_ lets build_picture's cache know when the
        // selection changes, decoupled from viewport_version_/
        // visibility_version_ (selection is neither a viewport nor a
        // layer-visibility concern). Only bumped on an actual change, not
        // a redundant no-op call (matches clear_mouse_position's own
        // convention) - a no-op bump would invalidate the design picture
        // cache for nothing.
        void select(SelectionRef ref)
        {
            if (!is_selected(ref))
            {
                selection_.push_back(ref);
                ++selection_version_;
            }
        }

        void deselect(SelectionRef ref)
        {
            if (std::erase(selection_, ref) > 0)
                ++selection_version_;
        }

        void clear_selection()
        {
            if (!selection_.empty())
            {
                selection_.clear();
                ++selection_version_;
            }
        }

        bool is_selected(SelectionRef ref) const
        {
            return std::find(selection_.begin(), selection_.end(), ref) != selection_.end();
        }

        const std::vector<SelectionRef> &selection() const { return selection_; }
        uint64_t selection_version() const { return selection_version_; }

    private:
        AbstractId current_abstract_;
        Point pan_{0, 0};
        double scale_ = 1.0;
        int viewport_width_px_ = 0;
        int viewport_height_px_ = 0;
        uint64_t viewport_version_ = 0;
        int64_t minor_grid_spacing_ = 5;
        int64_t major_grid_spacing_ = 50;
        int32_t mouse_x_px_ = 0;
        int32_t mouse_y_px_ = 0;
        bool has_mouse_position_ = false;
        uint64_t mouse_version_ = 0;
        bool dragging_ = false;
        int32_t drag_start_x_px_ = 0;
        int32_t drag_start_y_px_ = 0;
        std::unordered_set<int32_t> held_keys_;
        std::optional<HoverTarget> hovered_;
        std::unordered_map<std::string, bool> layer_name_visible_;
        std::unordered_map<ViewLayerPurpose, bool> purpose_visible_;
        uint64_t visibility_version_ = 0;
        std::unordered_map<std::string, bool> layer_name_selectable_;
        std::unordered_map<ViewLayerPurpose, bool> purpose_selectable_;
        std::vector<SelectionRef> selection_;
        uint64_t selection_version_ = 0;
    };
}
