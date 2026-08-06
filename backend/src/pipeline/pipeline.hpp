#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
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
    /// benchmark, not assumed - see BENCHMARKS.md). It also attaches one
    /// text label per Terminal (see its own doc comment for placement/
    /// survival-through-filtering details).
    ///
    /// filter_by_layer_visibility's output is grouped by ViewLayerId (a
    /// std::map, not a flat vector) rather than PipelineCache-era's
    /// flat-then-filter - see its own doc comment for why std::map's
    /// ordering gives correct bottom-up draw order for free.
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
        ///
        /// Each Terminal gets one text label *per distinct layer_name* it
        /// has geometry on - its name, placed via Geometry::get_label_location
        /// on the union of that layer's geometry across all the Terminal's
        /// Ports (not per-Port; several Ports can share a layer, and the
        /// label represents all of a Terminal's geometry on that layer). A
        /// single-layer Terminal (the common case) gets exactly one label;
        /// a Terminal with shapes on e.g. both M1 and M2 gets two, each
        /// correctly tied to its own layer's ViewLayerId so it only shows
        /// alongside that layer, not the Terminal as a whole. Each label is
        /// appended to the .texts of its layer's *first* Port Shape rather
        /// than emitted as its own text-only Shape: filter_by_viewport_and_size
        /// drops shapes with no bbox, and Geometry::bbox doesn't account for
        /// Shape::texts, so a standalone label would be silently culled.
        /// Riding along on a real geometric shape means it inherits that
        /// shape's bbox (and ViewLayerId) for free. Known gap: if that
        /// specific first shape happens to have no geometry of its own
        /// (unusual but possible), its label is dropped with it - not
        /// handled specially.
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
                    // Single pass over this Terminal's Ports/Shapes: push
                    // each one into `shapes` immediately (preserving its own
                    // layer_name/ViewLayerId - combining shapes across
                    // layers would break per-layer visibility for
                    // multi-layer Terminals, and coarsen viewport/sub-pixel
                    // culling to a combined bbox instead of each piece's
                    // own), while also accumulating just the geometry
                    // primitives (not whole Shapes) into a per-layer_name
                    // combined Shape for label placement only. Each layer's
                    // label is attached after the loop, once its location
                    // is known, to the first Shape pushed for *that layer*
                    // (remembered by index - safe across any reallocation
                    // `shapes.push_back` triggers, since vector indices
                    // stay valid across growth, only references/iterators
                    // taken before it don't).
                    struct LabelAccumulator
                    {
                        Shape combined;
                        size_t first_shape_index = 0;
                    };
                    std::unordered_map<std::string, LabelAccumulator> by_layer;

                    for (auto port_id : root.get_terminal_ports(terminal_id))
                    {
                        const auto *port = root.get_terminal_port(port_id);
                        for (const auto &shape : port->shapes)
                        {
                            auto [it, inserted] = by_layer.try_emplace(shape.layer_name);
                            if (inserted)
                                it->second.first_shape_index = shapes.size();

                            Shape &combined = it->second.combined;
                            combined.rects.insert(combined.rects.end(), shape.rects.begin(), shape.rects.end());
                            combined.polygons.insert(combined.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                            combined.paths.insert(combined.paths.end(), shape.paths.begin(), shape.paths.end());

                            shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::TERMINAL)});
                            // Merges the pushed copy's own rects/polygons in
                            // place (after combined's accumulated from the
                            // pre-merge shape above - get_label_location
                            // unions its own input either way, so which one
                            // it sees doesn't change the label location) -
                            // see Geometry::merge_overlapping_fills for why.
                            Geometry::merge_overlapping_fills(shapes.back().shape);
                        }
                    }

                    if (by_layer.empty())
                        continue;

                    if (const TerminalData *terminal = root.get_terminal(terminal_id))
                    {
                        for (const auto &[layer_name, acc] : by_layer)
                        {
                            shapes[acc.first_shape_index].shape.texts.push_back(Text{
                                .label = terminal->name,
                                .location = Geometry::get_label_location(acc.combined),
                            });
                        }
                    }
                }

                for (auto obstruction_id : obstructions)
                {
                    const auto *obstruction = root.get_obstruction(obstruction_id);
                    for (const auto &shape : obstruction->shapes)
                    {
                        shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::OBSTRUCTION)});
                        Geometry::merge_overlapping_fills(shapes.back().shape);
                    }
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

        /// @brief Group surviving shapes by ViewLayerId, dropping any whose
        /// ViewLayer the Scene has hidden - a shape whose ViewLayerId is
        /// invalid (its layer_name didn't resolve to a known Layer) is
        /// kept, grouped under that invalid id - there's no visibility
        /// toggle to check it against. Visibility is checked once per
        /// distinct ViewLayerId actually present, not once per shape.
        ///
        /// std::map (not unordered_map) is deliberate: ViewLayerId's
        /// natural ordering (its {index, generation} via the defaulted
        /// operator<=>) matches LEF-declared physical layer stacking order
        /// exactly - ViewLayerSet::build_for_technology creates each
        /// Layer's TERMINAL then OBSTRUCTION ViewLayer while iterating
        /// Root::get_technology_layers in LEF declaration order (bottom-up:
        /// M1, M2, ...), with BOUNDARY created last (highest index). So
        /// iterating this map in key order - which callers drawing it are
        /// expected to do - draws bottom-up with the boundary outline on
        /// top, with no separate sort/ordering step needed.
        const std::map<ViewLayerId, std::vector<RenderedShape>> &filter_by_layer_visibility(const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version()};
            return layer_filtered_.get(key, [&]
            {
                std::map<ViewLayerId, std::vector<RenderedShape>> grouped;
                for (const auto &rs : shapes)
                    grouped[rs.view_layer].push_back(rs);

                for (auto it = grouped.begin(); it != grouped.end();)
                {
                    // Scene's visibility is keyed by (layer name, purpose) -
                    // not ViewLayerId directly (see its own comment) - so an
                    // unresolved ViewLayerId (no ViewLayerData behind it)
                    // has no visibility toggle to check, same as before.
                    const ViewLayerData *data = view_layers.get(it->first);
                    if (data && !scene.is_view_layer_visible(data->layer_name, data->purpose))
                        it = grouped.erase(it);
                    else
                        ++it;
                }

                return grouped;
            });
        }

        /// @brief Run all three stages for the Scene's current_abstract().
        const std::map<ViewLayerId, std::vector<RenderedShape>> &run(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &generated = generate_shapes(root, scene.current_abstract(), view_layers);
            const auto &viewport_filtered = filter_by_viewport_and_size(generated, scene);
            return filter_by_layer_visibility(viewport_filtered, scene, view_layers);
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t generate_calls() const { return generated_.call_count(); }
        uint64_t viewport_filter_calls() const { return viewport_filtered_.call_count(); }
        uint64_t layer_filter_calls() const { return layer_filtered_.call_count(); }

    private:
        CachedStage<AbstractId, std::vector<RenderedShape>> generated_;
        CachedStage<std::pair<AbstractId, uint64_t>, std::vector<RenderedShape>> viewport_filtered_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<RenderedShape>>> layer_filtered_;
    };
}
