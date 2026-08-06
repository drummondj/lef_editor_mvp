#pragma once
#include "../database/database.hpp"
#include "../view_style/view_style.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace le
{
    // Selectable object references - the object kinds that currently have a
    // rendered geometric representation in an Abstract view. Extend this
    // variant as more kinds (e.g. Instance, once a Layout/placement view
    // exists) become selectable.
    using SelectionRef = std::variant<TerminalId, ObstructionId>;

    /// @brief Per-handle mutable view state: which Abstract is displayed,
    /// the viewport transform, per-layer visibility, and selection. Distinct
    /// from the persistent Root database - the pipeline reads from this,
    /// events write into it.
    class Scene
    {
    public:
        // --- Currently displayed Abstract ---
        void set_current_abstract(AbstractId id) { current_abstract_ = id; }
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
        void select(SelectionRef ref)
        {
            if (!is_selected(ref))
                selection_.push_back(ref);
        }

        void deselect(SelectionRef ref)
        {
            std::erase(selection_, ref);
        }

        void clear_selection() { selection_.clear(); }

        bool is_selected(SelectionRef ref) const
        {
            return std::find(selection_.begin(), selection_.end(), ref) != selection_.end();
        }

        const std::vector<SelectionRef> &selection() const { return selection_; }

    private:
        AbstractId current_abstract_;
        Point pan_{0, 0};
        double scale_ = 1.0;
        int viewport_width_px_ = 0;
        int viewport_height_px_ = 0;
        uint64_t viewport_version_ = 0;
        int64_t minor_grid_spacing_ = 5;
        int64_t major_grid_spacing_ = 50;
        std::unordered_map<std::string, bool> layer_name_visible_;
        std::unordered_map<ViewLayerPurpose, bool> purpose_visible_;
        uint64_t visibility_version_ = 0;
        std::unordered_map<std::string, bool> layer_name_selectable_;
        std::unordered_map<ViewLayerPurpose, bool> purpose_selectable_;
        std::vector<SelectionRef> selection_;
    };
}
