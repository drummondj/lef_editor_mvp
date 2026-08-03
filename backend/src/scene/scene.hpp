#pragma once
#include "../database/database.hpp"
#include "../view_style/view_style.hpp"
#include <algorithm>
#include <cstdint>
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

        // --- Layer visibility (defaults to visible until toggled) ---
        // Keyed by ViewLayerId (purpose-tagged: e.g. "M1 terminals" and "M1
        // obstructions" toggle independently), not the physical LayerId -
        // see view_style.hpp for why.
        void set_layer_visible(ViewLayerId id, bool visible)
        {
            layer_visible_[id] = visible;
            ++visibility_version_;
        }

        // Monotonic counter bumped by set_layer_visible - cheap for a
        // caller to compare instead of comparing the visibility map by value.
        uint64_t visibility_version() const { return visibility_version_; }
        bool is_layer_visible(ViewLayerId id) const
        {
            auto it = layer_visible_.find(id);
            return it == layer_visible_.end() ? true : it->second;
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
        std::unordered_map<ViewLayerId, bool> layer_visible_;
        uint64_t visibility_version_ = 0;
        std::vector<SelectionRef> selection_;
    };
}
