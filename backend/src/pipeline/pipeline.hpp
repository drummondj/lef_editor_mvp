#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <cstdint>
#include <map>
#include <memory>
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

        /// The selectable object this Shape came from (UPDATES.md 7.1) -
        /// nullopt for the BOUNDARY shape, which isn't tied to any
        /// Terminal/Obstruction and isn't selectable.
        std::optional<SelectionRef> origin;

        /// (*path_outlines)[i] == Geometry::path_to_polygons(shape.paths[i]) -
        /// each path's buffered outline (flat ends, miter joins),
        /// precomputed once here (generate_shapes is cached per-AbstractId,
        /// not per-frame - see its own doc comment) rather than in
        /// Renderer::transform_to_pixels (which reruns on every pan/zoom,
        /// viewport_version is in its cache key) or draw_group (which
        /// reruns on every build_picture recompute). Lets draw_group fill/
        /// outline a PATH the same way it already does a POLYGON - a real
        /// filled region with a thin boundary - instead of a solid stroke
        /// along the centerline, which used to read as an opaque block
        /// regardless of the layer's fill pattern (see BENCHMARKS.md).
        ///
        /// shared_ptr, not a plain vector: filter_by_viewport_and_size
        /// copies surviving RenderedShapes wholesale on every pan/zoom
        /// (result.push_back(s) below) - that was already true before this
        /// field existed (RenderedShape::shape's own rects/polygons/paths
        /// get copied the same way), but a plain vector-of-vector-of-Polygon
        /// here made every such copy meaningfully heavier (confirmed via
        /// BM_RunReused_PanOnly regressing ~8ms to ~50ms - see
        /// BENCHMARKS.md). A shared_ptr makes copying a RenderedShape cost
        /// the same O(1) refcount bump regardless of how much outline
        /// geometry a path-heavy Shape holds. Default member initializer
        /// (an empty vector, not null) means every RenderedShape - not
        /// just the ones generate_shapes explicitly sets this for - has a
        /// safely dereferenceable path_outlines, including the BOUNDARY
        /// shape (no paths, never sets this) and every test that
        /// constructs a RenderedShape directly without mentioning it.
        std::shared_ptr<const std::vector<std::vector<Polygon>>> path_outlines = std::make_shared<const std::vector<std::vector<Polygon>>>();
    };

    /// @brief A shape too small to render normally (bbox under 1 pixel in
    /// both dimensions at the Scene's current scale - see
    /// Pipeline::tiny_shapes_by_viewport) - just enough to draw a single
    /// device-pixel dot so the user can see *something* is there, per
    /// UPDATES.md item 6. Deliberately not a RenderedShape: no `origin`
    /// (SelectionRef) at all, so Pipeline::hit_test_point/hit_test_rect
    /// (which only ever see the *normal* filter_by_viewport_and_size/
    /// filter_by_layer_visibility output, never this type) can't select
    /// one even by accident - "not selectable" holds by construction, not
    /// by an exclusion check somewhere.
    struct TinyShapeDot
    {
        Point location; // dbu-space bbox center
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
    ///   generate_shapes             <- AbstractId, ViewLayerSet::generation()
    ///   filter_by_viewport_and_size <- AbstractId, Scene::viewport_version()
    ///   filter_by_layer_visibility  <- AbstractId, viewport_version(), Scene::visibility_version()
    ///
    /// generate_shapes's key includes ViewLayerSet::generation() (not just
    /// AbstractId) because it resolves every shape straight to a
    /// ViewLayerId using the *given* ViewLayerSet - api.cpp's le_read_lef
    /// rebuilds its handle's ViewLayerSet from scratch (a new Pool, not an
    /// in-place update) on every call, so re-reading a LEF file while
    /// viewing an already-cached Abstract must invalidate this stage even
    /// though the AbstractId itself hasn't changed, or it would keep
    /// returning RenderedShapes whose ViewLayerIds were resolved against a
    /// now-discarded ViewLayerSet.
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
        /// and sized via Geometry::local_width_at on the union of that
        /// layer's geometry across all the Terminal's Ports (not per-Port;
        /// several Ports can share a layer, and the label represents all
        /// of a Terminal's geometry on that layer). Both calls share the
        /// same combined geometry and anchor point, so the label is sized
        /// to whichever piece it actually landed on, not the layer's
        /// overall bbox. A
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
        ///
        /// Also computes each Shape's RenderedShape::path_outlines here
        /// (one Geometry::path_to_polygons buffer op per path) - this is
        /// the right cache tier for that real geometry cost (~768ns/call,
        /// see BM_PathToPolygonsSingleCall) since this stage only
        /// recomputes on an Abstract switch or ViewLayerSet rebuild, not
        /// on every pan/zoom the way Renderer::transform_to_pixels does.
        const std::vector<RenderedShape> &generate_shapes(const Root &root, AbstractId abstract_id, const ViewLayerSet &view_layers)
        {
            return generated_.get(std::tuple{abstract_id, view_layers.generation()}, [&]
            {
                std::vector<RenderedShape> shapes;
                const auto &terminals = root.get_abstract_terminals(abstract_id);
                const auto &obstructions = root.get_abstract_obstructions(abstract_id);
                shapes.reserve(terminals.size() + obstructions.size());

                auto resolve = [&](const Shape &shape, ViewLayerPurpose purpose)
                {
                    return view_layers.find(root.get_layer_by_name(shape.layer_name), purpose);
                };

                // UPDATES.md 12 Phase 1's ITERATE rework - LEFReader stores
                // RECT/PATH/POLYGON ITERATE statements raw (rect_iterates/
                // path_iterates/polygon_iterates) rather than pre-expanding
                // them at parse time, so LEFWriter can re-emit compact
                // ITERATE syntax on write. This is the one place downstream
                // Pipeline/Renderer code needs to know about that - expands
                // each into concrete Rect/Path/Polygon entries appended to
                // the same Shape's rects/paths/polygons, so everything below
                // (label accumulation, merge_overlapping_fills,
                // compute_path_outlines, RenderedShape itself) sees a
                // conventional fully-expanded Shape exactly as before this
                // rework. Bounds each statement's own num_x*num_y the same
                // way LEFReader::safe_iteration_count did at parse time -
                // defense in depth, not just trusting the database's
                // already-validated values.
                auto expand_iterates = [](Shape shape)
                {
                    constexpr int kMaxReasonableCount = 1'000'000;

                    for (const RectIterate &it : shape.rect_iterates)
                    {
                        if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                            continue;
                        shape.rects.reserve(shape.rects.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                        for (int ix = 0; ix < it.num_x; ix++)
                            for (int iy = 0; iy < it.num_y; iy++)
                                shape.rects.push_back(Rect{
                                    .ll = Point{.x = it.rect.ll.x + ix * it.space_x, .y = it.rect.ll.y + iy * it.space_y},
                                    .ur = Point{.x = it.rect.ur.x + ix * it.space_x, .y = it.rect.ur.y + iy * it.space_y},
                                });
                    }
                    shape.rect_iterates.clear();

                    for (const PathIterate &it : shape.path_iterates)
                    {
                        if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                            continue;
                        shape.paths.reserve(shape.paths.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                        for (int ix = 0; ix < it.num_x; ix++)
                            for (int iy = 0; iy < it.num_y; iy++)
                            {
                                const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                                shape.paths.push_back(Path{.width = it.path.width, .polygon = Geometry::transform(it.path.polygon, offset)});
                            }
                    }
                    shape.path_iterates.clear();

                    for (const PolygonIterate &it : shape.polygon_iterates)
                    {
                        if (it.num_x <= 0 || it.num_y <= 0 || it.num_x > kMaxReasonableCount || it.num_y > kMaxReasonableCount)
                            continue;
                        shape.polygons.reserve(shape.polygons.size() + static_cast<size_t>(it.num_x) * static_cast<size_t>(it.num_y));
                        for (int ix = 0; ix < it.num_x; ix++)
                            for (int iy = 0; iy < it.num_y; iy++)
                            {
                                const Point offset{.x = ix * it.space_x, .y = iy * it.space_y};
                                shape.polygons.push_back(Geometry::transform(it.polygon, offset));
                            }
                    }
                    shape.polygon_iterates.clear();

                    return shape;
                };

                // One buffered outline per path, computed once here (see
                // RenderedShape::path_outlines's own comment for why this
                // is the right cache tier, and why it's shared_ptr-wrapped).
                auto compute_path_outlines = [](const Shape &shape)
                {
                    std::vector<std::vector<Polygon>> outlines;
                    outlines.reserve(shape.paths.size());
                    for (const Path &path : shape.paths)
                        outlines.push_back(Geometry::path_to_polygons(path));
                    return std::make_shared<const std::vector<std::vector<Polygon>>>(std::move(outlines));
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
                        for (const auto &shape_id : root.get_terminal_port_shapes(port_id))
                        {
                            const auto *raw_shape = root.get_shape(shape_id);
                            if (!raw_shape)
                                continue;
                            const Shape shape = expand_iterates(*raw_shape);
                            auto [it, inserted] = by_layer.try_emplace(shape.layer_name);
                            if (inserted)
                                it->second.first_shape_index = shapes.size();

                            Shape &combined = it->second.combined;
                            combined.rects.insert(combined.rects.end(), shape.rects.begin(), shape.rects.end());
                            combined.polygons.insert(combined.polygons.end(), shape.polygons.begin(), shape.polygons.end());
                            combined.paths.insert(combined.paths.end(), shape.paths.begin(), shape.paths.end());

                            shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::TERMINAL), .origin = SelectionRef{terminal_id}, .path_outlines = compute_path_outlines(shape)});
                            // Merges the pushed copy's own rects/polygons in
                            // place (after combined's accumulated from the
                            // pre-merge shape above - get_label_location
                            // unions its own input either way, so which one
                            // it sees doesn't change the label location) -
                            // see Geometry::merge_overlapping_fills for why.
                            // Paths (and therefore path_outlines, computed
                            // from the pre-merge shape above) are left
                            // untouched by this - see its own doc comment.
                            Geometry::merge_overlapping_fills(shapes.back().shape);
                        }
                    }

                    if (by_layer.empty())
                        continue;

                    if (const TerminalData *terminal = root.get_terminal(terminal_id))
                    {
                        for (const auto &[layer_name, acc] : by_layer)
                        {
                            const Point location = Geometry::get_label_location(acc.combined);
                            shapes[acc.first_shape_index].shape.texts.push_back(Text{
                                .label = terminal->name,
                                .location = location,
                                .size = Geometry::local_width_at(acc.combined, location),
                            });
                        }
                    }
                }

                for (auto obstruction_id : obstructions)
                {
                    for (const auto &shape_id : root.get_obstruction_shapes(obstruction_id))
                    {
                        const auto *raw_shape = root.get_shape(shape_id);
                        if (!raw_shape)
                            continue;
                        const Shape shape = expand_iterates(*raw_shape);
                        shapes.push_back(RenderedShape{.shape = shape, .view_layer = resolve(shape, ViewLayerPurpose::OBSTRUCTION), .origin = SelectionRef{obstruction_id}, .path_outlines = compute_path_outlines(shape)});
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
        const std::vector<RenderedShape> &filter_by_viewport_and_size(const std::vector<RenderedShape> &shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            // view_layers.generation() is here even though this stage
            // never reads view_layers itself - `shapes` is generate_shapes's
            // output, and its own key includes generation() (see that
            // function's doc comment on why); if this stage's key omitted
            // it, a cache hit here would keep returning a filtered view of
            // the *previous* `shapes` even after generate_shapes correctly
            // recomputed a new one.
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), view_layers.generation()};
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
            // view_layers.generation() - same transitive-staleness reasoning
            // as filter_by_viewport_and_size's own key above: `shapes` here
            // is that stage's output, which changes whenever generate_shapes
            // does, even when current_abstract()/viewport_version()/
            // visibility_version() don't.
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), view_layers.generation()};
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

        /// @brief The population filter_by_viewport_and_size *drops* for
        /// being under 1 pixel in both dimensions (UPDATES.md item 6) -
        /// one TinyShapeDot per such shape (its bbox center), instead of
        /// nothing, so the caller can render a single-pixel fallback
        /// rather than have the shape silently vanish when zoomed out.
        /// Deliberately a separate stage over the same generate_shapes
        /// output, not a second return value bolted onto
        /// filter_by_viewport_and_size: keeps that stage's own signature
        /// (and everything downstream of it - hit_test_point/
        /// hit_test_rect, existing tests) untouched, at the cost of a
        /// second pass over generate_shapes's output - cheap relative to
        /// generate_shapes itself (no Boost calls here, only bbox/overlap
        /// arithmetic, the same per-shape cost filter_by_viewport_and_size
        /// already pays - see BENCHMARKS.md for the measured cost).
        /// Mirrors filter_by_viewport_and_size's own viewport-overlap
        /// check exactly (same bbox, same viewport rect, same
        /// Geometry::rects_overlap call) so the two stages can never
        /// disagree about which shapes are "tiny" vs "normal" - only the
        /// size-threshold branch differs.
        const std::vector<TinyShapeDot> &tiny_shapes_by_viewport(const Root &root, AbstractId abstract_id, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto key = std::tuple{abstract_id, scene.viewport_version(), view_layers.generation()};
            return tiny_shapes_viewport_filtered_.get(key, [&]
            {
                const auto &shapes = generate_shapes(root, abstract_id, view_layers);

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

                std::vector<TinyShapeDot> result;

                for (const auto &s : shapes)
                {
                    auto bbox = Geometry::bbox(s.shape);
                    if (!bbox)
                        continue;

                    if (!Geometry::rects_overlap(*bbox, viewport))
                        continue;

                    const double width = static_cast<double>(bbox->ur.x - bbox->ll.x);
                    const double height = static_cast<double>(bbox->ur.y - bbox->ll.y);
                    if (!(width < min_visible_dbu && height < min_visible_dbu))
                        continue;

                    result.push_back(TinyShapeDot{
                        .location = Point{(bbox->ll.x + bbox->ur.x) / 2, (bbox->ll.y + bbox->ur.y) / 2},
                        .view_layer = s.view_layer,
                    });
                }

                return result;
            });
        }

        /// @brief Groups tiny_shapes_by_viewport's output by ViewLayerId,
        /// dropping any whose ViewLayer the Scene has hidden - mirrors
        /// filter_by_layer_visibility exactly, but for TinyShapeDot
        /// (grouping by ViewLayerId drops the now-redundant view_layer
        /// field per dot, same convention PixelShape already uses -
        /// callers get it from the map key).
        const std::map<ViewLayerId, std::vector<Point>> &tiny_shapes_by_layer_visibility(const std::vector<TinyShapeDot> &tiny_shapes, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto key = std::tuple{scene.current_abstract(), scene.viewport_version(), scene.visibility_version(), view_layers.generation()};
            return tiny_shapes_layer_filtered_.get(key, [&]
            {
                std::map<ViewLayerId, std::vector<Point>> grouped;
                for (const auto &dot : tiny_shapes)
                    grouped[dot.view_layer].push_back(dot.location);

                for (auto it = grouped.begin(); it != grouped.end();)
                {
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
            const auto &viewport_filtered = filter_by_viewport_and_size(generated, scene, view_layers);
            return filter_by_layer_visibility(viewport_filtered, scene, view_layers);
        }

        /// @brief Run both tiny-shape stages for the Scene's
        /// current_abstract() - mirrors `run` above for the parallel
        /// sub-pixel-dot path (UPDATES.md item 6, see TinyShapeDot's own
        /// comment for why this is a separate chain rather than folded
        /// into `run`'s own).
        const std::map<ViewLayerId, std::vector<Point>> &run_tiny_shapes(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            const auto &tiny_shapes = tiny_shapes_by_viewport(root, scene.current_abstract(), scene, view_layers);
            return tiny_shapes_by_layer_visibility(tiny_shapes, scene, view_layers);
        }

        /// @brief Topmost-layer-first point hit-test (UPDATES.md 7.1 items
        /// 1-3) against an already-filtered map (typically `run`'s own
        /// output - already viewport-culled and visibility-filtered, so
        /// this only ever scans what's actually on screen, not the whole
        /// design). Not a CachedStage-backed stage like the methods
        /// above - `dbu_point` changes on every call, so there's no
        /// reusable output to cache. Reverse-iterates `shapes` (its
        /// std::map key order is bottom-up stacking order - see
        /// filter_by_layer_visibility's own comment - so reverse means
        /// topmost first), skipping a ViewLayer the Scene has made
        /// unselectable (Scene::is_view_layer_selectable) and any
        /// RenderedShape with no `origin` (the BOUNDARY shape - not
        /// selectable). Returns the first hit's origin plus a copy of
        /// just the single rect/polygon/path piece that was actually hit
        /// (Geometry::find_hit_piece) - not the whole RenderedShape's
        /// Shape, which can bundle several rects/polygons/paths together
        /// (e.g. several RECTs in one LEF PORT); hovering must highlight
        /// only the one piece under the cursor, not the whole group.
        /// First-match-within-a-layer wins for two overlapping shapes on
        /// the same layer, an accepted MVP limitation. nullopt if nothing
        /// was hit.
        static std::optional<HoverTarget> hit_test_point(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const ViewLayerSet &view_layers, const Scene &scene, Point dbu_point)
        {
            for (auto it = shapes.rbegin(); it != shapes.rend(); ++it)
            {
                const ViewLayerData *data = view_layers.get(it->first);
                if (data && !scene.is_view_layer_selectable(data->layer_name, data->purpose))
                    continue;

                for (const auto &rs : it->second)
                {
                    if (!rs.origin)
                        continue;

                    if (auto piece = Geometry::find_hit_piece(rs.shape, dbu_point))
                        return HoverTarget{.origin = *rs.origin, .outline = *piece};
                }
            }

            return std::nullopt;
        }

        /// @brief Rubber-band enclosure hit-test (UPDATES.md 7.1 item 5:
        /// "all selectable shapes on all layers completely enclosed by
        /// the selection rectangle") against an already-filtered map
        /// (typically `run`'s own output). Unlike hit_test_point, scans
        /// every layer (no topmost-only restriction - rule 5 says "all
        /// layers") and collects every match, in no particular order.
        /// Same unselectable-layer/no-origin skip as hit_test_point.
        ///
        /// Piece-level, like hit_test_point (via Geometry::
        /// fully_enclosed_pieces rather than the coarser fully_enclosed) -
        /// one RenderedShape can bundle several rects/polygons/paths
        /// together (e.g. several RECT statements in one PORT), and a
        /// drag enclosing only some of them must report only those, not
        /// treat the whole bundle - and by extension every other piece
        /// sharing the same origin, however many there are - as one
        /// selected unit. Returns one HoverTarget per enclosed piece, so
        /// dragging over several disjoint pieces of the same object (e.g.
        /// a Terminal with multiple ports, or an Obstruction with many
        /// RECT/PATH/POLYGON items) yields one independently-selected
        /// entry per piece, exactly like shift-clicking each individually
        /// would (see Scene::select's dedup-by-(origin, piece)).
        static std::vector<HoverTarget> hit_test_rect(const std::map<ViewLayerId, std::vector<RenderedShape>> &shapes, const ViewLayerSet &view_layers, const Scene &scene, Rect dbu_rect)
        {
            std::vector<HoverTarget> result;

            for (const auto &[view_layer_id, group] : shapes)
            {
                const ViewLayerData *data = view_layers.get(view_layer_id);
                if (data && !scene.is_view_layer_selectable(data->layer_name, data->purpose))
                    continue;

                for (const auto &rs : group)
                {
                    if (!rs.origin)
                        continue;

                    for (auto &piece : Geometry::fully_enclosed_pieces(dbu_rect, rs.shape))
                        result.push_back(HoverTarget{.origin = *rs.origin, .outline = std::move(piece)});
                }
            }

            return result;
        }

        // Number of times each stage actually recomputed - exposed purely
        // to make cache hits/misses observable in tests.
        uint64_t generate_calls() const { return generated_.call_count(); }
        uint64_t viewport_filter_calls() const { return viewport_filtered_.call_count(); }
        uint64_t layer_filter_calls() const { return layer_filtered_.call_count(); }
        uint64_t tiny_shapes_viewport_filter_calls() const { return tiny_shapes_viewport_filtered_.call_count(); }
        uint64_t tiny_shapes_layer_filter_calls() const { return tiny_shapes_layer_filtered_.call_count(); }

    private:
        CachedStage<std::tuple<AbstractId, uint64_t>, std::vector<RenderedShape>> generated_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::vector<RenderedShape>> viewport_filtered_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<RenderedShape>>> layer_filtered_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t>, std::vector<TinyShapeDot>> tiny_shapes_viewport_filtered_;
        CachedStage<std::tuple<AbstractId, uint64_t, uint64_t, uint64_t>, std::map<ViewLayerId, std::vector<Point>>> tiny_shapes_layer_filtered_;
    };
}
