#pragma once
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"

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

    /// @brief Generates, then viewport/size-filters, then layer-resolves and
    /// -filters, the shapes for a Scene's currently displayed Abstract - a
    /// single straight-line pass (no node/task framework, no caching) to
    /// prove shapes-in/shapes-out end-to-end before generalizing into
    /// anything heavier.
    ///
    /// Stage order is benchmark-confirmed at 1M shapes (see
    /// src/pipeline/benchmarks/): resolve_view_layers is deliberately its
    /// own stage *after* filter_by_viewport_and_size, not folded into
    /// generate_shapes. Tagging every shape with a ViewLayerId at
    /// generation time was tried first and made the full pipeline ~46%
    /// slower (40.8ms -> 59.8ms) - it pays a Layer-by-name hashmap lookup
    /// on all 1M shapes instead of only the ~25% that survive viewport
    /// culling. Same lesson as the viewport-before-layer-filter finding,
    /// one level deeper: run the cheap, effective filter before paying for
    /// expensive per-shape resolution, not after.
    class Pipeline
    {
    public:
        /// @brief Collect every Shape from the Abstract's Terminals' Ports,
        /// its Obstructions, and its boundary polygons, unfiltered, in
        /// dbu-space, tagged with which of those three it came from (cheap -
        /// no lookup yet, see resolve_view_layers). An unknown AbstractId
        /// (nothing created yet, or a stale/erased one) yields an empty
        /// result rather than an error - Root's own lookups already
        /// degrade gracefully for that.
        static std::vector<TaggedShape> generate_shapes(const Root &root, AbstractId abstract_id)
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
        }

        /// @brief Drop shapes outside the Scene's viewport, and shapes whose
        /// bbox is under 1 pixel (at the Scene's scale) in BOTH dimensions -
        /// not just one, so a long thin wire survives even if its width
        /// alone is sub-pixel; only true "invisible dot" shapes are culled.
        /// Shapes with no rects/polygons/paths (e.g. text-only shapes -
        /// Geometry::bbox doesn't account for Shape::texts) have no bbox
        /// and are dropped for now; text rendering isn't subject to this
        /// kind of culling anyway and needs its own handling later.
        static std::vector<TaggedShape> filter_by_viewport_and_size(const std::vector<TaggedShape> &shapes, const Scene &scene)
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

            std::vector<TaggedShape> result;
            result.reserve(shapes.size());

            for (const auto &ts : shapes)
            {
                auto bbox = Geometry::bbox(ts.shape);
                if (!bbox)
                    continue;

                if (!Geometry::rects_overlap(*bbox, viewport))
                    continue;

                const double width = static_cast<double>(bbox->ur.x - bbox->ll.x);
                const double height = static_cast<double>(bbox->ur.y - bbox->ll.y);
                if (width < min_visible_dbu && height < min_visible_dbu)
                    continue;

                result.push_back(ts);
            }

            return result;
        }

        /// @brief Resolve each shape's ViewLayerId from its layer_name +
        /// purpose (a Layer-by-name hashmap lookup) - see the class comment
        /// for why this runs after, not during, generation. BOUNDARY shapes
        /// skip the lookup entirely; they're always view_layers.boundary_view_layer().
        static std::vector<RenderedShape> resolve_view_layers(const std::vector<TaggedShape> &shapes, const Root &root, const ViewLayerSet &view_layers)
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
        }

        /// @brief Drop shapes on a ViewLayer the Scene has hidden. A shape
        /// whose ViewLayerId is invalid (its layer_name didn't resolve to a
        /// known Layer) is kept - there's no visibility toggle to check it
        /// against.
        static std::vector<RenderedShape> filter_by_layer_visibility(const std::vector<RenderedShape> &shapes, const Scene &scene)
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
        }

        /// @brief Run all four stages for the Scene's current_abstract().
        static std::vector<RenderedShape> run(const Root &root, const Scene &scene, const ViewLayerSet &view_layers)
        {
            auto shapes = generate_shapes(root, scene.current_abstract());
            shapes = filter_by_viewport_and_size(shapes, scene);
            auto rendered = resolve_view_layers(shapes, root, view_layers);
            return filter_by_layer_visibility(rendered, scene);
        }
    };
}
