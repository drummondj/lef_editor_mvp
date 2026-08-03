#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <cstdint>
#include <utility>

namespace le
{
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

    /// @brief Generates and filters the shapes for a Scene's currently
    /// displayed Abstract. Each stage owns its own CachedStage (see above)
    /// and chains to the previous stage internally, so a single Pipeline
    /// instance transparently reuses whatever hasn't changed - construct
    /// one per Scene-equivalent lifetime and reuse it across repeated
    /// calls (e.g. every interactive frame); a fresh instance recomputes
    /// everything on its first call.
    ///
    /// Cache key per stage (each includes every upstream trigger it
    /// transitively depends on, so invalidation cascades for free via key
    /// comparison - no manual "invalidate downstream" bookkeeping):
    ///
    ///   generate_shapes             <- AbstractId
    ///   filter_by_viewport_and_size <- AbstractId, Scene::viewport_version()
    ///   filter_by_layer_visibility  <- AbstractId, viewport_version(), Scene::visibility_version()
    ///
    /// generate_shapes collects every Shape from the Abstract's Terminals'
    /// Ports, Obstructions, and boundary polygon, resolving each straight
    /// to its ViewLayerId (a Layer-by-name + purpose lookup) in the same
    /// pass - there's no separate "tagged but unresolved" intermediate
    /// vector/type. This used to be two stages (generate, then a distinct
    /// resolve_view_layers keyed the same way - see BENCHMARKS.md), merged
    /// once the caching redesign made resolve_view_layers always run on
    /// generate_shapes's full output rather than a filtered subset: two
    /// stages sharing one cache key just meant two full copies of the
    /// shape data (one per stage) where one now suffices. Keeping the
    /// lookup *inside* generation like this was tried once before splitting
    /// these stages apart and found ~46% slower - but that finding was for
    /// a design where viewport-filtering ran *between* generate and
    /// resolve, culling to ~25% of shapes before the lookup; that's not
    /// true here, so it doesn't apply to this merge (confirmed by
    /// benchmark, not assumed - see BENCHMARKS.md).
    class Pipeline
    {
    public:
        /// @brief Collect every Shape from the Abstract's Terminals' Ports,
        /// its Obstructions, and its boundary polygon, each resolved to its
        /// ViewLayerId, in dbu-space. BOUNDARY shapes skip the lookup
        /// entirely; they're always view_layers.boundary_view_layer(). A
        /// shape whose layer_name doesn't resolve to a known Layer (e.g. an
        /// undeclared/typo'd name) keeps an invalid ViewLayerId rather than
        /// being dropped - there's no visibility toggle to check it
        /// against yet. An unknown AbstractId (nothing created yet, or a
        /// stale/erased one) yields an empty result rather than an error -
        /// Root's own lookups already degrade gracefully for that.
        const std::vector<RenderedShape> &generate_shapes(const Root &root, AbstractId abstract_id, const ViewLayerSet &view_layers)
        {
            return generated_.get(abstract_id, [&]
            {
                std::vector<RenderedShape> shapes;
                const auto &terminals = root.get_abstract_terminals(abstract_id);
                const auto &obstructions = root.get_abstract_obstructions(abstract_id);
                shapes.reserve(terminals.size() + obstructions.size());

                auto resolve = [&](const Shape &shape, ViewLayerPurpose purpose)
                {
                    return view_layers.find(root.get_layer_by_name(shape.layer_name), purpose);
                };

                for (auto terminal_id : terminals)
                {
                    for (auto port_id : root.get_terminal_ports(terminal_id))
                    {
                        const auto *port = root.get_terminal_port(port_id);
                        for (const auto &shape : port->shapes)
                            shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::TERMINAL)});
                    }
                }

                for (auto obstruction_id : obstructions)
                {
                    const auto *obstruction = root.get_obstruction(obstruction_id);
                    for (const auto &shape : obstruction->shapes)
                        shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::OBSTRUCTION)});
                }

                if (const AbstractData *abstract = root.get_abstract(abstract_id); abstract && !abstract->boundary.empty())
                {
                    shapes.push_back(RenderedShape{
                        .shape = Shape{.layer_name = "BOUNDARY", .polygons = abstract->boundary},
                        .view_layer = view_layers.boundary_view_layer(),
                    });
                }

                return shapes;
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

        /// @brief Run all three stages for the Scene's current_abstract().
        const std::vector<RenderedShape> &run(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &generated = generate_shapes(root, scene.current_abstract(), view_layers);
            const auto &viewport_filtered = filter_by_viewport_and_size(generated, scene);
            return filter_by_layer_visibility(viewport_filtered, scene);
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t generate_calls() const { return generated_.call_count(); }
        uint64_t viewport_filter_calls() const { return viewport_filtered_.call_count(); }
        uint64_t layer_filter_calls() const { return layer_filtered_.call_count(); }

    private:
        CachedStage<AbstractId, std::vector<RenderedShape>> generated_;
        CachedStage<std::pair<AbstractId, uint64_t>, std::vector<RenderedShape>> viewport_filtered_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::vector<RenderedShape>> layer_filtered_;
    };
}
