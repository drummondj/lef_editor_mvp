#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <cstdint>
#include <utility>

namespace le
{
    /// @brief A Shape tagged with which kind of object it came from - cheap
    /// to attach at generation time (no lookup), unlike its ViewLayerId
    /// (see RenderedShape), which needs a Layer-by-name lookup.
    struct TaggedShape
    {
        Shape shape;
        ViewLayerPurpose purpose;
    };

    /// @brief A Shape resolved to the ViewLayer it belongs to.
    struct RenderedShape
    {
        Shape shape;
        ViewLayerId view_layer;
    };

    /// @brief Remembers the last (key, value) pair produced by a single
    /// compute function; recomputes only when the key differs from last
    /// time. Not a general reactive/dependency-graph primitive - just
    /// enough to give each Pipeline stage its own cache slot without
    /// hand-written invalidation flags scattered through the class.
    template <typename Key, typename Value>
    class CachedStage
    {
    public:
        template <typename Fn>
        const Value &get(const Key &key, Fn &&compute)
        {
            if (!valid_ || key != last_key_)
            {
                value_ = compute();
                last_key_ = key;
                valid_ = true;
                ++call_count_;
            }
            return value_;
        }

        // Number of times compute() actually ran - exposed purely to make
        // cache hits/misses observable in tests.
        uint64_t call_count() const { return call_count_; }

    private:
        Key last_key_{};
        Value value_{};
        bool valid_ = false;
        uint64_t call_count_ = 0;
    };

    /// @brief Generates, resolves, and filters the shapes for a Scene's
    /// currently displayed Abstract. Each stage owns its own CachedStage
    /// (see above) and chains to the previous stage internally, so a
    /// single Pipeline instance transparently reuses whatever hasn't
    /// changed - construct one per Scene-equivalent lifetime and reuse it
    /// across repeated calls (e.g. every interactive frame); a fresh
    /// instance recomputes everything on its first call.
    ///
    /// Cache key per stage (each includes every upstream trigger it
    /// transitively depends on, so invalidation cascades for free via key
    /// comparison - no manual "invalidate downstream" bookkeeping):
    ///
    ///   generate_shapes            <- AbstractId
    ///   resolve_view_layers        <- AbstractId
    ///   filter_by_viewport_and_size <- AbstractId, Scene::viewport_version()
    ///   filter_by_layer_visibility  <- AbstractId, viewport_version(), Scene::visibility_version()
    ///
    /// generate_shapes and resolve_view_layers share the same key
    /// (AbstractId alone) rather than resolve_view_layers being keyed on
    /// the viewport filter's output: resolve_view_layers's real inputs
    /// (root/view_layers) don't change on pan/zoom, so chaining it to the
    /// viewport filter would mean re-resolving on every interactive frame
    /// for no reason. The trade: resolve_view_layers runs on the full
    /// generated set instead of the smaller post-viewport-filter survivor
    /// set, so a fresh Pipeline's first call costs more than a design
    /// ordered for a single one-shot call would (~+42ms at 1M shapes) -
    /// accepted because the real usage pattern is read LEF once -> pick an
    /// Abstract -> many pan/zoom/selection changes on the same instance,
    /// and a one-time Abstract switch can show a loading spinner (up to
    /// ~1-2s is fine). See BENCHMARKS.md for the full measurement,
    /// including why resolving the full set costs ~40ms and not the ~7ms
    /// a naive per-shape extrapolation would suggest.
    class Pipeline
    {
    public:
        /// @brief Collect every Shape from the Abstract's Terminals' Ports,
        /// its Obstructions, and its boundary polygons, unfiltered, in
        /// dbu-space, tagged with which of those three it came from. An
        /// unknown AbstractId (nothing created yet, or a stale/erased one)
        /// yields an empty result rather than an error - Root's own
        /// lookups already degrade gracefully for that.
        const std::vector<TaggedShape> &generate_shapes(const Root &root, AbstractId abstract_id)
        {
            return generated_.get(abstract_id, [&]
            {
                std::vector<TaggedShape> shapes;
                const auto &terminals = root.get_abstract_terminals(abstract_id);
                const auto &obstructions = root.get_abstract_obstructions(abstract_id);
                shapes.reserve(terminals.size() + obstructions.size());

                for (auto terminal_id : terminals)
                {
                    for (auto port_id : root.get_terminal_ports(terminal_id))
                    {
                        const auto *port = root.get_terminal_port(port_id);
                        for (const auto &shape : port->shapes)
                            shapes.push_back(TaggedShape{.shape = shape, .purpose = ViewLayerPurpose::TERMINAL});
                    }
                }

                for (auto obstruction_id : obstructions)
                {
                    const auto *obstruction = root.get_obstruction(obstruction_id);
                    for (const auto &shape : obstruction->shapes)
                        shapes.push_back(TaggedShape{.shape = shape, .purpose = ViewLayerPurpose::OBSTRUCTION});
                }

                if (const AbstractData *abstract = root.get_abstract(abstract_id); abstract && !abstract->boundary.empty())
                {
                    shapes.push_back(TaggedShape{
                        .shape = Shape{.layer_name = "BOUNDARY", .polygons = abstract->boundary},
                        .purpose = ViewLayerPurpose::BOUNDARY,
                    });
                }

                return shapes;
            });
        }

        /// @brief Resolve each shape's ViewLayerId from its layer_name +
        /// purpose (a Layer-by-name hashmap lookup). BOUNDARY shapes skip
        /// the lookup entirely; they're always view_layers.boundary_view_layer().
        /// `abstract_id` is the cache key - callers going through run()
        /// pass the same AbstractId `shapes` was generated from; direct
        /// callers (e.g. tests) supplying hand-built `shapes` can pass any
        /// consistent value, since a fresh Pipeline's first call always
        /// recomputes regardless of key.
        const std::vector<RenderedShape> &resolve_view_layers(const std::vector<TaggedShape> &shapes, AbstractId abstract_id, const Root &root, const ViewLayerSet &view_layers)
        {
            return resolved_.get(abstract_id, [&]
            {
                std::vector<RenderedShape> result;
                result.reserve(shapes.size());

                for (const auto &ts : shapes)
                {
                    ViewLayerId view_layer;
                    if (ts.purpose == ViewLayerPurpose::BOUNDARY)
                        view_layer = view_layers.boundary_view_layer();
                    else
                        view_layer = view_layers.find(root.get_layer_by_name(ts.shape.layer_name), ts.purpose);

                    result.push_back(RenderedShape{.shape = ts.shape, .view_layer = view_layer});
                }

                return result;
            });
        }

        /// @brief Drop shapes outside the Scene's viewport, and shapes
        /// whose bbox is under 1 pixel (at the Scene's scale) in BOTH
        /// dimensions - not just one, so a long thin wire survives even
        /// if its width alone is sub-pixel; only true "invisible dot"
        /// shapes are culled. Shapes with no rects/polygons/paths (e.g.
        /// text-only shapes - Geometry::bbox doesn't account for
        /// Shape::texts) have no bbox and are dropped for now; text
        /// rendering isn't subject to this kind of culling anyway and
        /// needs its own handling later.
        const std::vector<RenderedShape> &filter_by_viewport_and_size(const std::vector<RenderedShape> &shapes, const Scene &scene)
        {
            const auto key = std::pair{scene.current_abstract(), scene.viewport_version()};
            return viewport_filtered_.get(key, [&]
            {
                const double scale = scene.scale();
                const double min_visible_dbu = 1.0 / scale;

                const Point viewport_ll = scene.pan();
                const Rect viewport{
                    .ll = viewport_ll,
                    .ur = Point{
                        viewport_ll.x + static_cast<int64_t>(scene.viewport_width_px() / scale),
                        viewport_ll.y + static_cast<int64_t>(scene.viewport_height_px() / scale),
                    },
                };

                std::vector<RenderedShape> result;
                result.reserve(shapes.size());

                for (const auto &s : shapes)
                {
                    auto bbox = Geometry::bbox(s.shape);
                    if (!bbox)
                        continue;

                    if (!Geometry::rects_overlap(*bbox, viewport))
                        continue;

                    const double width = static_cast<double>(bbox->ur.x - bbox->ll.x);
                    const double height = static_cast<double>(bbox->ur.y - bbox->ll.y);
                    if (width < min_visible_dbu && height < min_visible_dbu)
                        continue;

                    result.push_back(s);
                }

                return result;
            });
        }

        /// @brief Drop shapes on a ViewLayer the Scene has hidden. A shape
        /// whose ViewLayerId is invalid (its layer_name didn't resolve to
        /// a known Layer) is kept - there's no visibility toggle to check
        /// it against.
        const std::vector<RenderedShape> &filter_by_layer_visibility(const std::vector<RenderedShape> &shapes, const Scene &scene)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version()};
            return layer_filtered_.get(key, [&]
            {
                std::vector<RenderedShape> result;
                result.reserve(shapes.size());

                for (const auto &rs : shapes)
                {
                    if (rs.view_layer.valid() && !scene.is_layer_visible(rs.view_layer))
                        continue;

                    result.push_back(rs);
                }

                return result;
            });
        }

        /// @brief Run all four stages for the Scene's current_abstract().
        const std::vector<RenderedShape> &run(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &generated = generate_shapes(root, scene.current_abstract());
            const auto &resolved = resolve_view_layers(generated, scene.current_abstract(), root, view_layers);
            const auto &viewport_filtered = filter_by_viewport_and_size(resolved, scene);
            return filter_by_layer_visibility(viewport_filtered, scene);
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t generate_calls() const { return generated_.call_count(); }
        uint64_t resolve_calls() const { return resolved_.call_count(); }
        uint64_t viewport_filter_calls() const { return viewport_filtered_.call_count(); }
        uint64_t layer_filter_calls() const { return layer_filtered_.call_count(); }

    private:
        CachedStage<AbstractId, std::vector<TaggedShape>> generated_;
        CachedStage<AbstractId, std::vector<RenderedShape>> resolved_;
        CachedStage<std::pair<AbstractId, uint64_t>, std::vector<RenderedShape>> viewport_filtered_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::vector<RenderedShape>> layer_filtered_;
    };
}
