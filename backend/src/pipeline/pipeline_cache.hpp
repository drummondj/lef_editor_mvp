#pragma once
#include "pipeline.hpp"
#include <cstdint>
#include <vector>

namespace le
{
    /// @brief Cascading cache over Pipeline's four static stages, composed
    /// in a *different* order than Pipeline::run(): generate -> resolve ->
    /// viewport-filter -> layer-filter, not generate -> viewport-filter ->
    /// resolve -> layer-filter.
    ///
    /// Pipeline::run()'s order is right for a single uncached call (resolve
    /// only pays its Layer-by-name lookup on the ~25% of shapes that
    /// survive viewport culling, not all of them - see its class comment).
    /// But that finding assumes every stage reruns on every call. With
    /// caching, resolve_view_layers has no real dependency on the viewport
    /// at all - its only inputs are generate_shapes's output plus root/
    /// view_layers, both static at runtime today (no mutation API exists
    /// yet). Chaining it to the viewport filter (as a naive port of
    /// Pipeline::run()'s order would) means re-resolving on every pan/zoom
    /// even though nothing it actually depends on changed. Running it
    /// right after generate_shapes instead - on the full generated set,
    /// keyed on AbstractId alone - means it's paid once per Abstract
    /// selection (the rare event) instead of once per interactive frame
    /// (the frequent one), at the cost of resolving more shapes on that
    /// one rare call. See BENCHMARKS.md for the measured trade.
    ///
    /// A stage recomputes if its own trigger changed *or* the stage before
    /// it just recomputed (cascading invalidation) - an explicit chain
    /// matching Pipeline's own four fixed stages, not a generic reactive
    /// graph. Doesn't detect database mutation - no mutation API exists yet
    /// to invalidate against; revisit once events/editing lands.
    ///
    /// One PipelineCache per Scene-equivalent lifetime, owned by the
    /// caller - not embedded in Scene, which would otherwise have to depend
    /// on TaggedShape/RenderedShape, inverting today's dependency direction
    /// (pipeline depends on scene, not the reverse). Pipeline's own static
    /// stage methods and its stateless run() are unchanged; this wraps them
    /// rather than replacing them.
    class PipelineCache
    {
    public:
        const std::vector<RenderedShape> &run(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            return layer_filtered(root, scene, view_layers);
        }

        // Number of times each underlying stage actually recomputed -
        // exposed purely to make cache hits/misses observable in tests.
        uint64_t generate_calls() const { return generate_calls_; }
        uint64_t resolve_calls() const { return resolve_calls_; }
        uint64_t viewport_filter_calls() const { return viewport_filter_calls_; }
        uint64_t layer_filter_calls() const { return layer_filter_calls_; }

    private:
        const std::vector<TaggedShape> &generated(const Root &root, AbstractId abstract_id)
        {
            if (!generated_valid_ || abstract_id != cached_abstract_id_)
            {
                cached_generated_ = Pipeline::generate_shapes(root, abstract_id);
                cached_abstract_id_ = abstract_id;
                generated_valid_ = true;
                ++generate_calls_;
                resolved_valid_ = false;
                viewport_filtered_valid_ = false;
                layer_filtered_valid_ = false;
            }
            return cached_generated_;
        }

        // Runs on the *full* generated set (not viewport-filtered) so its
        // only trigger is AbstractId - see the class comment for why.
        const std::vector<RenderedShape> &resolved(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &shapes = generated(root, scene.current_abstract());
            if (!resolved_valid_)
            {
                cached_resolved_ = Pipeline::resolve_view_layers(shapes, root, view_layers);
                resolved_valid_ = true;
                ++resolve_calls_;
                viewport_filtered_valid_ = false;
                layer_filtered_valid_ = false;
            }
            return cached_resolved_;
        }

        const std::vector<RenderedShape> &viewport_filtered(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &shapes = resolved(root, scene, view_layers);
            if (!viewport_filtered_valid_ || scene.viewport_version() != cached_viewport_version_)
            {
                cached_viewport_filtered_ = Pipeline::filter_by_viewport_and_size(shapes, scene);
                cached_viewport_version_ = scene.viewport_version();
                viewport_filtered_valid_ = true;
                ++viewport_filter_calls_;
                layer_filtered_valid_ = false;
            }
            return cached_viewport_filtered_;
        }

        const std::vector<RenderedShape> &layer_filtered(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &shapes = viewport_filtered(root, scene, view_layers);
            if (!layer_filtered_valid_ || scene.visibility_version() != cached_visibility_version_)
            {
                cached_layer_filtered_ = Pipeline::filter_by_layer_visibility(shapes, scene);
                cached_visibility_version_ = scene.visibility_version();
                layer_filtered_valid_ = true;
                ++layer_filter_calls_;
            }
            return cached_layer_filtered_;
        }

        std::vector<TaggedShape> cached_generated_;
        AbstractId cached_abstract_id_;
        bool generated_valid_ = false;
        uint64_t generate_calls_ = 0;

        std::vector<RenderedShape> cached_resolved_;
        bool resolved_valid_ = false;
        uint64_t resolve_calls_ = 0;

        std::vector<RenderedShape> cached_viewport_filtered_;
        uint64_t cached_viewport_version_ = 0;
        bool viewport_filtered_valid_ = false;
        uint64_t viewport_filter_calls_ = 0;

        std::vector<RenderedShape> cached_layer_filtered_;
        uint64_t cached_visibility_version_ = 0;
        bool layer_filtered_valid_ = false;
        uint64_t layer_filter_calls_ = 0;
    };
}
