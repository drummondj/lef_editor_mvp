#include "api.hpp"
#include "../database/database.hpp"
#include "../database/filter.hpp"
#include "../geometry/geometry.hpp"
#include "../io/lef_reader.hpp"
#include "../pipeline/pipeline.hpp"
#include "../render/render.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <fmt/format.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <limits>
#include <mutex>

// The real, C++-only definition behind the opaque LeHandle - never exposed
// in api.hpp. Owns everything needed to load a LEF file and render it: one
// Pipeline and one Renderer per handle (not one per call), matching the
// "reuse across repeated calls" lifetime their own internal CachedStage
// caching is designed around.
//
// `mutex_` exists because this handle genuinely is called from more than
// one thread today, not as defensive-but-unnecessary caution: Flutter's
// external-texture API invokes le_render_pixel_buffer (via
// LeTexture.copyPixelBuffer()) from its own dedicated raster thread once
// per frame, while ordinary pointer/FFI calls (le_set_mouse_position,
// le_mouse_down/up, le_zoom, ...) run on the platform thread - both
// threads reach the same Pipeline/Scene/Root state. See every exported
// function's own std::lock_guard for the actual enforcement; see
// le_destroy's doc comment in api.hpp for the one function that can't be
// covered by the handle's own mutex.
struct LeHandle
{
    le::Root root;
    le::ViewLayerSet view_layers;
    le::Scene scene;
    le::Pipeline pipeline;
    le::Renderer renderer;
    std::mutex mutex_;

    // Single-slot cache backing le_selected_object_property_count/
    // le_selected_object_property_at - rebuilt whenever a different
    // selection_index is requested (see build_selected_object_properties).
    // le::PropertyValue (generated/property.hpp) doubles as LeProperty's
    // string-owning backing store directly - no separate wrapper type
    // needed, its shape already matches LeProperty field-for-field.
    int32_t cached_property_selection_index = -1;
    std::vector<le::PropertyValue> cached_properties;

    // Same single-slot-cache pattern as cached_properties above, but for
    // le_terminal_property_count/_at (Phase 4's by-id CRUD surface,
    // TCL_EXPLORATION.md) - keyed by TerminalId instead of a selection
    // index, since there's no "current selection" involved here.
    le::TerminalId cached_terminal_property_id{};
    std::vector<le::PropertyValue> cached_terminal_properties;

    // Backs le_search_terminal/le_search_result_terminal_at - the ids a
    // filter-expression search matched, cached until the next
    // le_search_terminal call overwrites it, same "valid until the next
    // call" convention as cached_properties above.
    std::vector<le::TerminalId> terminal_search_results;

    // Same two patterns as cached_terminal_property_id/_properties and
    // terminal_search_results above, repeated for TerminalPort and
    // Obstruction (Phase 4, continued).
    le::TerminalPortId cached_terminal_port_property_id{};
    std::vector<le::PropertyValue> cached_terminal_port_properties;
    std::vector<le::TerminalPortId> terminal_port_search_results;

    le::ObstructionId cached_obstruction_property_id{};
    std::vector<le::PropertyValue> cached_obstruction_properties;
    std::vector<le::ObstructionId> obstruction_search_results;

    // Backs le_message_count/le_message_at (UPDATES.md item 3) - every
    // error/warning/info message produced by this handle's backend
    // operations so far (currently just le_read_lef), in order, never
    // cleared or reordered. std::deque, deliberately not std::vector:
    // le_message_at() hands out a `const char*` promised valid "until
    // the handle is destroyed" (this API's usual string-ownership
    // convention), but std::vector::push_back can reallocate on growth,
    // move-relocating every contained std::string - for a short (SSO)
    // string that relocates its character buffer inline, invalidating
    // any .c_str() a caller is still holding from an earlier call.
    // std::deque::push_back never invalidates references/pointers to
    // existing elements (only iterators), so it's the correct container
    // here.
    std::deque<std::string> messages;
};

namespace
{
    LeLibraryId to_c(le::LibraryId id) { return LeLibraryId{.index = id.index, .generation = id.generation}; }
    LeDesignId to_c(le::DesignId id) { return LeDesignId{.index = id.index, .generation = id.generation}; }
    LeAbstractId to_c(le::AbstractId id) { return LeAbstractId{.index = id.index, .generation = id.generation}; }
    LeTerminalId to_c(le::TerminalId id) { return LeTerminalId{.index = id.index, .generation = id.generation}; }
    LeTerminalPortId to_c(le::TerminalPortId id) { return LeTerminalPortId{.index = id.index, .generation = id.generation}; }
    LeObstructionId to_c(le::ObstructionId id) { return LeObstructionId{.index = id.index, .generation = id.generation}; }
    LeShapeId to_c(le::ShapeId id) { return LeShapeId{.index = id.index, .generation = id.generation}; }

    le::DesignId from_c(LeDesignId id) { return le::DesignId{.index = id.index, .generation = id.generation}; }
    le::AbstractId from_c(LeAbstractId id) { return le::AbstractId{.index = id.index, .generation = id.generation}; }
    le::TerminalId from_c(LeTerminalId id) { return le::TerminalId{.index = id.index, .generation = id.generation}; }
    le::TerminalPortId from_c(LeTerminalPortId id) { return le::TerminalPortId{.index = id.index, .generation = id.generation}; }
    le::ObstructionId from_c(LeObstructionId id) { return le::ObstructionId{.index = id.index, .generation = id.generation}; }
    le::ShapeId from_c(LeShapeId id) { return le::ShapeId{.index = id.index, .generation = id.generation}; }

    // Below this many pixels of down-to-up movement, le_mouse_up treats a
    // gesture as a click rather than a drag-select - small enough that an
    // intended click with a little hand tremor still registers as one,
    // large enough that a real rubber-band drag never gets misread as a
    // click. Not exposed/configurable - an implementation detail of the
    // click-vs-drag decision, not a Scene-persistent concern.
    constexpr int32_t kClickDragThresholdPx = 4;

    // Fixed step sizes for the keyboard-triggered canvas-navigation
    // commands (le_key_down's LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_*) - not
    // exposed/configurable, same reasoning as kClickDragThresholdPx.
    constexpr double kKeyZoomFactor = 0.3;
    constexpr int32_t kKeyFitPaddingPx = 10;
    constexpr double kKeyPanFactor = 0.25;

    // LE_KEY_SELECT_ALL's own cap (UPDATES.md 9.1) - a design can have
    // far more selectable shapes than are reasonable to hold in the
    // selection at once (Scene::select() is O(1) average per call, but
    // the resulting selection itself, and every later FFI round-trip
    // over it, still scales with however many objects are in it).
    constexpr int32_t kMaxSelectAllCount = 10000;

    // le_tooltip_message's own text (UPDATES.md item 7.3) - only one
    // interaction mode (Select) exists today, so this is a single fixed
    // string rather than a lookup keyed on some not-yet-existing mode
    // enum; when a second mode is added, le_tooltip_message grows a
    // branch, not this constant a sibling.
    constexpr const char *kSelectModeTooltip =
        "Left click to select. Shift for multi-select. Left click and drag for rectangle multi-select.";

    // Maps le::PropertyValue::Type (generated/property.hpp) to the C API's
    // LePropertyType - kept as an explicit switch rather than a bare
    // static_cast so a future reordering of either enum fails to compile
    // here instead of silently mislabeling a row's type.
    int32_t to_c_property_type(le::PropertyValue::Type type)
    {
        switch (type)
        {
        case le::PropertyValue::Type::STRING:
            return LE_PROPERTY_TYPE_STRING;
        case le::PropertyValue::Type::INT:
            return LE_PROPERTY_TYPE_INT;
        case le::PropertyValue::Type::DOUBLE:
            return LE_PROPERTY_TYPE_DOUBLE;
        }
        return LE_PROPERTY_TYPE_STRING;
    }

    // LeProperty conversion, factored out of le_selected_object_property_at
    // (below) so Phase 4's by-id property accessors (le_terminal_property_at
    // et al) can build the same row shape from a le::PropertyValue without
    // duplicating the field-by-field mapping.
    LeProperty to_c(const le::PropertyValue &property)
    {
        return LeProperty{
            .name = property.name.c_str(),
            .type = to_c_property_type(property.type),
            .string_value = property.string_value.c_str(),
            .int_value = property.int_value,
            .double_value = property.double_value,
        };
    }

    // Single shared/global Technology, same assumption
    // le_snapped_mouse_position's own lookup makes - nullopt if none has
    // been read yet (le_read_lef) or it has no usable scale.
    std::optional<double> database_units_microns(const le::Root &root)
    {
        const auto technology_ids = root.get_technology_ids();
        if (technology_ids.empty())
            return std::nullopt;

        const le::TechnologyData *technology = root.get_technology(technology_ids.front());
        if (!technology || technology->database_units_microns <= 0.0)
            return std::nullopt;

        return technology->database_units_microns;
    }

    // Formats a coordinate value the way UPDATES.md 7.2 wants it shown:
    // std::to_string's own fixed 6 decimal digits, then trailing zeros
    // stripped in whole groups of three - never a partial group - down
    // to whichever group first contains a non-zero digit (or entirely,
    // dropping the decimal point too, if every digit is zero). So an
    // exact-micron value like 1.0 collapses to "1", a 3-decimal-precise
    // value like 0.34 collapses to "0.340" (one trailing zero kept,
    // since the next group of three - "340" - isn't all zero), and a
    // value that genuinely needs the full 6 digits of precision is left
    // alone. Groups of three (not one at a time) since that's this
    // project's own dbu/um convention - a DATABASE MICRONS value like
    // 1000 gives exactly 3 significant decimal digits, so a *partial*
    // trim (e.g. "0.34" instead of "0.340") would misrepresent that
    // precision as coarser than it is.
    std::string format_coordinate_um(double value)
    {
        std::string formatted = std::to_string(value); // always exactly 6 decimal digits
        const size_t dot = formatted.find('.');
        if (dot == std::string::npos)
            return formatted;

        size_t end = formatted.size();
        while (end - dot - 1 >= 3 && formatted.compare(end - 3, 3, "000") == 0)
            end -= 3;

        if (end == dot + 1) // stripped every decimal digit - drop the bare "." too
            end = dot;

        return formatted.substr(0, end);
    }

    // Appends a single "bbox_um" row ("ll_x ll_y ur_x ur_y", space-
    // separated, matching how e.g. a LEF RECT statement itself lists four
    // coordinates) if `bbox` and `dbu_per_um` are both present - silently
    // omitted otherwise (an object with no shapes yet, or no Technology
    // loaded), rather than reporting a misleading all-zero box. Always
    // hand-written, never schema-generated: a shape's own bounding box is
    // computed from nested geometry (TerminalPort/Obstruction shapes),
    // not a stored field on Terminal/Obstruction itself, so no amount of
    // struct-field reflection can produce it.
    void push_bbox_property(std::vector<le::PropertyValue> &properties, const std::optional<le::Rect> &bbox, std::optional<double> dbu_per_um)
    {
        if (!bbox || !dbu_per_um)
            return;

        const double ll_x = static_cast<double>(bbox->ll.x) / *dbu_per_um;
        const double ll_y = static_cast<double>(bbox->ll.y) / *dbu_per_um;
        const double ur_x = static_cast<double>(bbox->ur.x) / *dbu_per_um;
        const double ur_y = static_cast<double>(bbox->ur.y) / *dbu_per_um;
        properties.push_back(le::PropertyValue::make_string(
            "bbox_um",
            format_coordinate_um(ll_x) + " " + format_coordinate_um(ll_y) + " " + format_coordinate_um(ur_x) + " " + format_coordinate_um(ur_y)));
    }

    // Builds the full property-row list (UPDATES.md 7.2) for one selected
    // object - see le_selected_object_property_at's doc comment for the
    // exact row list per LeSelectionKind. Every plain/enum field on
    // TerminalData/ObstructionData itself (schema.py's Field metadata)
    // comes for free from the generated le::to_properties() - this only
    // hand-appends what's genuinely derived and can never be schema-
    // generated: child-list counts that need Root's own index (ports
    // isn't a struct field at all in INDEXED_POOLS style - see
    // get_struct_fields()'s child-reference filtering) and the bbox_um
    // computed from nested geometry (see push_bbox_property).
    //
    // If `selected.piece` is set (a click hit one specific rect/polygon/
    // path - see SelectedObject), bbox_um is scoped to just that piece
    // instead of the whole object's union, and a "layer_name" row is
    // added from the piece's own Shape::layer_name - meaningful now that
    // bbox is piece-scoped, since a Terminal can have ports on different
    // layers. Name/direction/port_count (or shapes_count) stay parent-
    // level context either way, so the table still says *which*
    // Terminal/pin this piece belongs to.
    std::vector<le::PropertyValue> build_selected_object_properties(const le::Root &root, const le::SelectedObject &selected)
    {
        std::vector<le::PropertyValue> properties;
        const std::optional<double> dbu_per_um = database_units_microns(root);

        if (const le::TerminalId *terminal_id = std::get_if<le::TerminalId>(&selected.origin))
        {
            const le::TerminalData *terminal = root.get_terminal(*terminal_id);
            if (!terminal)
                return properties;

            properties = le::to_properties(*terminal); // name, direction

            const std::vector<le::TerminalPortId> &port_ids = root.get_terminal_ports(*terminal_id);
            properties.push_back(le::PropertyValue::make_int("port_count", static_cast<int64_t>(port_ids.size())));

            if (selected.piece)
            {
                push_bbox_property(properties, le::Geometry::bbox(*selected.piece), dbu_per_um);
                properties.push_back(le::PropertyValue::make_string("layer_name", selected.piece->layer_name));
            }
            else
            {
                std::vector<const le::Shape *> shapes;
                shapes.reserve(port_ids.size()); // a lower bound (each port usually has one Shape) but avoids most reallocations
                for (const le::TerminalPortId port_id : port_ids)
                    for (const le::ShapeId shape_id : root.get_terminal_port_shapes(port_id))
                        if (const le::Shape *shape = root.get_shape(shape_id))
                            shapes.push_back(shape);
                push_bbox_property(properties, le::Geometry::bbox(shapes), dbu_per_um);
            }
        }
        else if (const le::ObstructionId *obstruction_id = std::get_if<le::ObstructionId>(&selected.origin))
        {
            const le::ObstructionData *obstruction = root.get_obstruction(*obstruction_id);
            if (!obstruction)
                return properties;

            properties = le::to_properties(*obstruction);
            const std::vector<le::ShapeId> &shape_ids = root.get_obstruction_shapes(*obstruction_id);
            properties.push_back(le::PropertyValue::make_int("shapes_count", static_cast<int64_t>(shape_ids.size())));

            if (selected.piece)
            {
                push_bbox_property(properties, le::Geometry::bbox(*selected.piece), dbu_per_um);
                properties.push_back(le::PropertyValue::make_string("layer_name", selected.piece->layer_name));
            }
            else
            {
                std::vector<const le::Shape *> shapes;
                shapes.reserve(shape_ids.size());
                for (const le::ShapeId shape_id : shape_ids)
                    if (const le::Shape *shape = root.get_shape(shape_id))
                        shapes.push_back(shape);
                push_bbox_property(properties, le::Geometry::bbox(shapes), dbu_per_um);
            }
        }

        return properties;
    }

    // Backs le_terminal_property_count/_at (Phase 4's by-id CRUD surface) -
    // unlike build_selected_object_properties above, not piece-scoped
    // (there's no click-selection concept for an arbitrary id lookup), so
    // no bbox_um/layer_name rows - just cmg's generated to_properties()
    // plus the same "port_count" derived row build_selected_object_
    // properties adds for LE_SELECTION_KIND_TERMINAL. Returns an empty
    // vector if id doesn't name a Terminal on this handle.
    std::vector<le::PropertyValue> build_terminal_properties(const le::Root &root, le::TerminalId id)
    {
        const le::TerminalData *terminal = root.get_terminal(id);
        if (!terminal)
            return {};

        std::vector<le::PropertyValue> properties = le::to_properties(*terminal);
        properties.push_back(le::PropertyValue::make_int("port_count", static_cast<int64_t>(root.get_terminal_ports(id).size())));
        return properties;
    }

    // Same pattern as build_terminal_properties, for TerminalPort/
    // Obstruction. "shapes_count" is a derived row (Root::get_x_shapes()'s
    // own size), not part of cmg's generated to_properties() - `shapes` is
    // an is_child field (Shape is pooled, TCL_EXPLORATION.md Phase 3), so
    // it isn't a struct field at all, same reasoning as Terminal's own
    // "port_count" row in build_terminal_properties above.
    std::vector<le::PropertyValue> build_terminal_port_properties(const le::Root &root, le::TerminalPortId id)
    {
        const le::TerminalPortData *port = root.get_terminal_port(id);
        if (!port)
            return {};
        std::vector<le::PropertyValue> properties = le::to_properties(*port);
        properties.push_back(le::PropertyValue::make_int("shapes_count", static_cast<int64_t>(root.get_terminal_port_shapes(id).size())));
        return properties;
    }

    std::vector<le::PropertyValue> build_obstruction_properties(const le::Root &root, le::ObstructionId id)
    {
        const le::ObstructionData *obstruction = root.get_obstruction(id);
        if (!obstruction)
            return {};
        std::vector<le::PropertyValue> properties = le::to_properties(*obstruction);
        properties.push_back(le::PropertyValue::make_int("shapes_count", static_cast<int64_t>(root.get_obstruction_shapes(id).size())));
        return properties;
    }

    // Rounds to the nearest dbu rather than truncating - a caller passing
    // e.g. 0.1um at 1000 dbu/um should get exactly 100 dbu, not silently
    // lose precision to a fractional-dbu rounding direction they didn't
    // choose.
    int64_t to_dbu(double value_um, double dbu_per_um)
    {
        return static_cast<int64_t>(std::llround(value_um * dbu_per_um));
    }

    double to_um(int64_t value_dbu, double dbu_per_um)
    {
        return static_cast<double>(value_dbu) / dbu_per_um;
    }

    // Shared by le_add_shape_polygon/le_add_shape_path - builds a Polygon
    // from a flat microns array (alternating x/y). nullopt on any invalid
    // input (null points_um, point_coord_count not a positive even number
    // of at least `min_points` points, or no Technology read yet to
    // convert microns to dbu with) - the caller turns that into a nonzero
    // return, same as every other creation failure in this API.
    std::optional<le::Polygon> build_polygon_from_flat_points(const le::Root &root, const double *points_um, int32_t point_coord_count, int32_t min_points)
    {
        if (!points_um || point_coord_count <= 0 || point_coord_count % 2 != 0 || point_coord_count < min_points * 2)
            return std::nullopt;
        const std::optional<double> dbu_per_um = database_units_microns(root);
        if (!dbu_per_um)
            return std::nullopt;

        le::Polygon polygon;
        for (int32_t i = 0; i < point_coord_count; i += 2)
            polygon.points.push_back(le::Point{.x = to_dbu(points_um[i], *dbu_per_um), .y = to_dbu(points_um[i + 1], *dbu_per_um)});
        return polygon;
    }

    // Lock-free bodies of le_zoom/le_pan/le_fit_scene - factored out so
    // le_key_down's LE_KEY_ZOOM/FIT/PAN_* handling can call them directly
    // while it's already holding handle->mutex_ (std::mutex isn't
    // recursive - calling back into le_zoom/le_pan/le_fit_scene itself
    // from inside le_key_down would deadlock the calling thread against
    // itself). Callers must have already null-checked `handle` and locked
    // its mutex; the public le_zoom/le_pan/le_fit_scene below do exactly
    // that and then delegate here, so the real logic exists in exactly
    // one place either way.
    void zoom_unlocked(LeHandle *handle, double factor, int32_t x, int32_t y)
    {
        const double old_scale = handle->scene.scale();
        const double new_scale = old_scale * (1.0 + factor);
        if (new_scale <= 0.0)
            return;

        const le::Point old_pan = handle->scene.pan();
        const double viewport_height = handle->scene.viewport_height_px();

        // Undo rasterize()'s Y-flip to get from the caller's image-pixel
        // (x, y) - top-left origin, y down - to the dbu point it currently
        // shows, using the *old* scale/pan (see render.hpp's PixelShape /
        // Renderer::rasterize comments for why pan/scale describe the
        // pre-flip transform while (x, y) here is post-flip).
        const double dbu_x = static_cast<double>(old_pan.x) + static_cast<double>(x) / old_scale;
        const double dbu_y = static_cast<double>(old_pan.y) + (viewport_height - static_cast<double>(y)) / old_scale;

        // Re-solve pan so that same dbu point still lands under (x, y) at
        // the new scale, keeping the zoom visually anchored there.
        const double pan_x_double = dbu_x - static_cast<double>(x) / new_scale;
        const double pan_y_double = dbu_y - (viewport_height - static_cast<double>(y)) / new_scale;

        // A `factor` close enough to -1.0 (an ordinary finite double, not
        // just the already-rejected exact -1.0 above) drives new_scale
        // toward zero, blowing up x/new_scale - reject before casting to
        // int64_t below, since casting an out-of-range (or non-finite)
        // double to an integer type is undefined behavior in C++, not a
        // safe wrap or saturation. Matches new_scale <= 0.0's existing
        // "invalid zoom, no-op" behavior rather than clamping to some
        // arbitrary minimum scale.
        constexpr double kInt64Max = static_cast<double>(std::numeric_limits<int64_t>::max());
        if (!std::isfinite(pan_x_double) || !std::isfinite(pan_y_double) ||
            std::abs(pan_x_double) >= kInt64Max || std::abs(pan_y_double) >= kInt64Max)
            return;

        const int64_t pan_x = static_cast<int64_t>(pan_x_double);
        const int64_t pan_y = static_cast<int64_t>(pan_y_double);

        handle->scene.set_scale(new_scale);
        handle->scene.set_pan(le::Point{.x = pan_x, .y = pan_y});
    }

    void pan_unlocked(LeHandle *handle, double x_factor, double y_factor)
    {
        const double scale = handle->scene.scale();
        const le::Point pan = handle->scene.pan();

        const int64_t dx = static_cast<int64_t>(x_factor * handle->scene.viewport_width_px() / scale);
        const int64_t dy = static_cast<int64_t>(y_factor * handle->scene.viewport_height_px() / scale);

        handle->scene.set_pan(le::Point{.x = pan.x + dx, .y = pan.y + dy});
    }

    void fit_scene_unlocked(LeHandle *handle, int32_t padding_px)
    {
        const auto &generated = handle->pipeline.generate_shapes(handle->root, handle->scene.current_abstract(), handle->view_layers);

        std::vector<const le::Shape *> shape_ptrs;
        shape_ptrs.reserve(generated.size());
        for (const auto &rs : generated)
            shape_ptrs.push_back(&rs.shape);

        handle->scene.fit_to_content(le::Geometry::bbox(shape_ptrs), padding_px);
    }

    // LE_KEY_FIT's Ctrl-held branch (UPDATES.md 9.6) - fits the viewport
    // to the current selection's own combined bbox instead of the whole
    // design's. Collects a pointer per selection entry into the same
    // owning storage build_selected_object_properties's bbox_um property
    // reads from (Root::get_terminal_port_shapes()/get_obstruction_shapes()
    // resolved through Root::get_shape(), or the SelectedObject's own
    // `piece` in `scene` - both outlive this call, no copy needed), then
    // a single Geometry::bbox call unions them - mirrors
    // fit_scene_unlocked's own shape_ptrs pattern above. A no-op (view
    // unchanged) if nothing is selected, unlike fit_scene_unlocked, which
    // always has the whole design to fall back to.
    void fit_selected_unlocked(LeHandle *handle, int32_t padding_px)
    {
        std::vector<const le::Shape *> shape_ptrs;

        for (const le::SelectedObject &selected : handle->scene.selection())
        {
            if (selected.piece)
            {
                shape_ptrs.push_back(&*selected.piece);
                continue;
            }

            if (const le::TerminalId *terminal_id = std::get_if<le::TerminalId>(&selected.origin))
            {
                for (const le::TerminalPortId port_id : handle->root.get_terminal_ports(*terminal_id))
                    for (const le::ShapeId shape_id : handle->root.get_terminal_port_shapes(port_id))
                        if (const le::Shape *shape = handle->root.get_shape(shape_id))
                            shape_ptrs.push_back(shape);
            }
            else if (const le::ObstructionId *obstruction_id = std::get_if<le::ObstructionId>(&selected.origin))
            {
                for (const le::ShapeId shape_id : handle->root.get_obstruction_shapes(*obstruction_id))
                    if (const le::Shape *shape = handle->root.get_shape(shape_id))
                        shape_ptrs.push_back(shape);
            }
        }

        if (shape_ptrs.empty())
            return;

        handle->scene.fit_to_content(le::Geometry::bbox(shape_ptrs), padding_px);
    }

    // LE_KEY_SELECT_ALL's own body (UPDATES.md 9.1) - unlocked variant,
    // same reasoning as zoom_unlocked/pan_unlocked/fit_scene_unlocked
    // above (called from inside le_key_down, which already holds
    // handle->mutex_). Deliberately uses generate_shapes +
    // filter_by_layer_visibility directly, *not* pipeline.run() - run()
    // also applies filter_by_viewport_and_size, which would silently
    // exclude anything currently off-screen or sub-pixel from "select
    // all" (the same viewport-independence fit_scene_unlocked's own
    // generate_shapes-direct call above needs, for the same reason).
    // Every selectable shape's own bbox is trivially inside the whole
    // Abstract's bbox, so hit_test_rect against that bbox correctly
    // enumerates "everything selectable" with no new traversal.
    void select_all_unlocked(LeHandle *handle)
    {
        const auto &generated = handle->pipeline.generate_shapes(handle->root, handle->scene.current_abstract(), handle->view_layers);
        const auto &filtered = handle->pipeline.filter_by_layer_visibility(handle->root, generated, handle->scene, handle->view_layers);

        std::vector<const le::Shape *> shape_ptrs;
        shape_ptrs.reserve(generated.size());
        for (const auto &rs : generated)
            shape_ptrs.push_back(&rs.shape);
        const auto bbox = le::Geometry::bbox(shape_ptrs);
        if (!bbox)
            return;

        handle->scene.clear_selection();

        const auto hits = le::Pipeline::hit_test_rect(filtered, handle->view_layers, handle->scene, *bbox);
        for (const le::HoverTarget &hit : hits)
        {
            if (static_cast<int32_t>(handle->scene.selection().size()) >= kMaxSelectAllCount)
                break;
            handle->scene.select(hit.origin, hit.outline);
        }

        if (hits.size() > static_cast<size_t>(kMaxSelectAllCount))
            handle->messages.push_back(fmt::format("WARNING: Selection capped at {} objects - design has more.", kMaxSelectAllCount));
    }

    // Every ROUTING-type layer in `technology_id`'s own declaration
    // order (UPDATES.md 9.4) - LE_KEY_1 maps to index 0 here, LE_KEY_2
    // to index 1, etc.
    std::vector<le::LayerId> ordered_routing_layers(const le::Root &root, le::TechnologyId technology_id)
    {
        std::vector<le::LayerId> result;
        for (le::LayerId layer_id : root.get_technology_layers(technology_id))
        {
            const le::LayerData *layer = root.get_layer(layer_id);
            if (layer && layer->type == "ROUTING")
                result.push_back(layer_id);
        }
        return result;
    }

    // Every CUT-type layer strictly between `a` and `b`'s own positions
    // in root.get_technology_layers(technology_id)'s declaration order
    // (UPDATES.md 9.4 - LEF has no distinct "VIA" layer type, vias are
    // TYPE CUT layers - see LeKeyCode's own doc comment). Order-
    // independent (a/b can be passed either way); usually exactly one,
    // but every CUT layer in the gap is returned, not just the first,
    // for an unusual technology that declares more than one.
    std::vector<le::LayerId> cut_layers_between(const le::Root &root, le::TechnologyId technology_id, le::LayerId a, le::LayerId b)
    {
        const auto &layers = root.get_technology_layers(technology_id);

        auto index_of = [&](le::LayerId id) -> std::optional<size_t>
        {
            for (size_t i = 0; i < layers.size(); ++i)
                if (layers[i] == id)
                    return i;
            return std::nullopt;
        };

        const auto index_a = index_of(a);
        const auto index_b = index_of(b);
        if (!index_a || !index_b)
            return {};

        const size_t lo = std::min(*index_a, *index_b);
        const size_t hi = std::max(*index_a, *index_b);

        std::vector<le::LayerId> result;
        for (size_t i = lo + 1; i < hi; ++i)
        {
            const le::LayerData *layer = root.get_layer(layers[i]);
            if (layer && layer->type == "CUT")
                result.push_back(layers[i]);
        }
        return result;
    }

    // LE_KEY_0..LE_KEY_9's own body (UPDATES.md 9.4/9.7) - unlocked-style
    // helper (already inside le_key_down's held mutex, matches the other
    // *_unlocked helpers' own convention above). `routing_index` is
    // 0-based (e.g. LE_KEY_1 with Ctrl not held -> 0, LE_KEY_1 with Ctrl
    // held -> 10, LE_KEY_0 -> 9 - see le_key_down's own switch for the
    // full mapping). No-op if there's no ROUTING layer at that index or
    // no Technology has been read yet.
    void toggle_routing_layer_visibility_unlocked(LeHandle *handle, int routing_index)
    {
        if (handle->root.get_technology_ids().empty())
            return;
        const le::TechnologyId technology_id = handle->root.get_technology_ids().front();

        const auto routing_layers = ordered_routing_layers(handle->root, technology_id);
        if (routing_index < 0 || static_cast<size_t>(routing_index) >= routing_layers.size())
            return;

        const le::LayerData *toggled = handle->root.get_layer(routing_layers[static_cast<size_t>(routing_index)]);
        if (!toggled)
            return;

        handle->scene.set_layer_name_visible(toggled->name, !handle->scene.is_layer_name_visible(toggled->name));

        // Only on this keyboard path (never from a direct
        // le_set_layer_name_visible() call) - re-check every adjacent
        // routing-layer pair and sync the CUT layer(s) between them to
        // "both visible" (UPDATES.md 9.4). Recomputed as a full pass,
        // not just the pairs touching the just-toggled layer - simpler
        // to reason about/test, and a technology has at most a few
        // dozen routing layers so the cost is trivial.
        for (size_t i = 0; i + 1 < routing_layers.size(); ++i)
        {
            const le::LayerData *first = handle->root.get_layer(routing_layers[i]);
            const le::LayerData *second = handle->root.get_layer(routing_layers[i + 1]);
            if (!first || !second)
                continue;

            const bool both_visible = handle->scene.is_layer_name_visible(first->name) && handle->scene.is_layer_name_visible(second->name);
            for (le::LayerId cut_id : cut_layers_between(handle->root, technology_id, routing_layers[i], routing_layers[i + 1]))
            {
                const le::LayerData *cut = handle->root.get_layer(cut_id);
                if (cut)
                    handle->scene.set_layer_name_visible(cut->name, both_visible);
            }
        }
    }
}

extern "C"
{
    LeHandle *le_create(void)
    {
        return new LeHandle();
    }

    void le_destroy(LeHandle *handle)
    {
        delete handle;
    }

    int le_read_lef(LeHandle *handle, const char *path)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        if (!path)
        {
            handle->messages.push_back("ERROR: le_read_lef: path is null");
            return 1;
        }

        // UPDATES.md 10 - snapshot the Technology's own layer count before
        // this read, so the default-visibility pass below (after the read)
        // can tell which physical layers this specific call newly
        // introduced, as opposed to ones a prior le_read_lef call already
        // defaulted - LEF layers are only ever appended to a Technology
        // within a session (see LEFReader's own is_technology_empty()-
        // gated create-vs-reuse), so get_technology_layers' existing
        // declaration-order prefix stays stable across reads and re-
        // running the default over it would silently re-hide a layer the
        // user has since made visible via the layer manager.
        size_t old_layer_count = 0;
        {
            const auto existing_technology_ids = handle->root.get_technology_ids();
            if (!existing_technology_ids.empty())
                old_layer_count = handle->root.get_technology_layers(existing_technology_ids.front()).size();
        }

        const std::filesystem::path lef_path(path);
        le::LEFReader reader;
        const int result = reader.read_lef(lef_path.string(), handle->root, lef_path.stem().string());
        for (const auto &msg : reader.messages())
            handle->messages.push_back(msg);
        // if (result == 0)
        //     handle->messages.push_back(fmt::format("INFO: Loaded {}.", path));
        if (result != 0)
            return result;

        // Rebuilt after every successful read, not just the first, so a
        // later LEF file's own new physical layers (e.g. a second macro
        // file with inline LAYER declarations) are picked up too - cheap
        // relative to a full LEF parse, so correctness here wins over the
        // small extra cost without needing a benchmark to justify it.
        const auto technology_ids = handle->root.get_technology_ids();
        if (!technology_ids.empty())
        {
            // UPDATES.md 10 - every physical layer this read newly
            // introduced defaults to hidden unless it's ROUTING or CUT.
            // BOUNDARY isn't a physical layer (no LayerId of its own - see
            // ViewLayerSet::build_for_technology) so it's untouched here,
            // staying visible via Scene::is_layer_name_visible's own
            // default-true-until-toggled behavior.
            const auto &layers = handle->root.get_technology_layers(technology_ids.front());
            for (size_t i = old_layer_count; i < layers.size(); ++i)
            {
                const le::LayerData *layer = handle->root.get_layer(layers[i]);
                if (layer && layer->type != "ROUTING" && layer->type != "CUT")
                    handle->scene.set_layer_name_visible(layer->name, false);
            }

            handle->view_layers = le::ViewLayerSet::build_for_technology(handle->root, technology_ids.front());
        }

        return 0;
    }

    int32_t le_message_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->messages.size());
    }

    const char *le_message_at(LeHandle *handle, int32_t index)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        if (index < 0 || static_cast<size_t>(index) >= handle->messages.size())
            return nullptr;
        return handle->messages[static_cast<size_t>(index)].c_str();
    }

    int32_t le_design_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_design_size());
    }

    const char *le_design_name(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return nullptr;

        const le::DesignData *design = handle->root.get_design(design_ids[static_cast<size_t>(index)]);
        return design ? design->name.c_str() : nullptr;
    }

    int le_set_current_design(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto design_ids = handle->root.get_design_ids();
        if (static_cast<size_t>(index) >= design_ids.size())
            return 1;

        handle->scene.set_current_abstract(handle->root.get_design_abstract(design_ids[static_cast<size_t>(index)]));
        return 0;
    }

    int32_t le_library_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_library_size());
    }

    LeLibraryInfo le_library_at(LeHandle *handle, int32_t index)
    {
        const LeLibraryInfo invalid{.id = {UINT32_MAX, 0}, .name = nullptr};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(index) >= library_ids.size())
            return invalid;

        const le::LibraryId id = library_ids[static_cast<size_t>(index)];
        const le::LibraryData *library = handle->root.get_library(id);
        return LeLibraryInfo{.id = to_c(id), .name = library ? library->name.c_str() : nullptr};
    }

    int32_t le_library_design_count(LeHandle *handle, int32_t library_index)
    {
        if (!handle || library_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(library_index) >= library_ids.size())
            return 0;

        return static_cast<int32_t>(handle->root.get_library_designs(library_ids[static_cast<size_t>(library_index)]).size());
    }

    LeDesignInfo le_library_design_at(LeHandle *handle, int32_t library_index, int32_t design_index)
    {
        const LeDesignInfo invalid{.library_id = {UINT32_MAX, 0}, .id = {UINT32_MAX, 0}, .abstract_id = {UINT32_MAX, 0}, .name = nullptr};
        if (!handle || library_index < 0 || design_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto library_ids = handle->root.get_library_ids();
        if (static_cast<size_t>(library_index) >= library_ids.size())
            return invalid;

        const le::LibraryId library_id = library_ids[static_cast<size_t>(library_index)];
        const auto &design_ids = handle->root.get_library_designs(library_id);
        if (static_cast<size_t>(design_index) >= design_ids.size())
            return invalid;

        const le::DesignId design_id = design_ids[static_cast<size_t>(design_index)];
        const le::DesignData *design = handle->root.get_design(design_id);
        return LeDesignInfo{
            .library_id = to_c(library_id),
            .id = to_c(design_id),
            .abstract_id = to_c(handle->root.get_design_abstract(design_id)),
            .name = design ? design->name.c_str() : nullptr,
        };
    }

    int le_set_current_design_by_id(LeHandle *handle, LeDesignId design_id)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::DesignId id = from_c(design_id);
        if (!handle->root.get_design(id))
            return 1;

        handle->scene.set_current_abstract(handle->root.get_design_abstract(id));
        return 0;
    }

    int32_t le_layer_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->view_layers.rows().size());
    }

    LeLayerRow le_layer_at(LeHandle *handle, int32_t row_index)
    {
        const LeLayerRow invalid{.name = nullptr, .color_r = 0, .color_g = 0, .color_b = 0};
        if (!handle || row_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto &rows = handle->view_layers.rows();
        if (static_cast<size_t>(row_index) >= rows.size())
            return invalid;

        const le::ViewLayerRow &row = rows[static_cast<size_t>(row_index)];
        const le::ViewLayerData *first_column = row.columns.empty() ? nullptr : handle->view_layers.get(row.columns.front().id);

        return LeLayerRow{
            .name = row.name.c_str(),
            .color_r = first_column ? first_column->style.outline_color.r : uint8_t{0},
            .color_g = first_column ? first_column->style.outline_color.g : uint8_t{0},
            .color_b = first_column ? first_column->style.outline_color.b : uint8_t{0},
        };
    }

    int32_t le_purpose_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->view_layers.purposes().size());
    }

    int32_t le_purpose_at(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return -1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto purposes = handle->view_layers.purposes();
        if (static_cast<size_t>(index) >= purposes.size())
            return -1;

        return static_cast<int32_t>(purposes[static_cast<size_t>(index)]);
    }

    int32_t le_is_layer_name_visible(LeHandle *handle, const char *layer_name)
    {
        if (!handle || !layer_name)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.is_layer_name_visible(layer_name) ? 1 : 0;
    }

    void le_set_layer_name_visible(LeHandle *handle, const char *layer_name, int32_t visible)
    {
        if (!handle || !layer_name)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_layer_name_visible(layer_name, visible != 0);
    }

    int32_t le_is_purpose_visible(LeHandle *handle, int32_t purpose)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.is_purpose_visible(static_cast<le::ViewLayerPurpose>(purpose)) ? 1 : 0;
    }

    void le_set_purpose_visible(LeHandle *handle, int32_t purpose, int32_t visible)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_purpose_visible(static_cast<le::ViewLayerPurpose>(purpose), visible != 0);
    }

    int32_t le_is_layer_name_selectable(LeHandle *handle, const char *layer_name)
    {
        if (!handle || !layer_name)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.is_layer_name_selectable(layer_name) ? 1 : 0;
    }

    void le_set_layer_name_selectable(LeHandle *handle, const char *layer_name, int32_t selectable)
    {
        if (!handle || !layer_name)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_layer_name_selectable(layer_name, selectable != 0);
    }

    int32_t le_is_purpose_selectable(LeHandle *handle, int32_t purpose)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.is_purpose_selectable(static_cast<le::ViewLayerPurpose>(purpose)) ? 1 : 0;
    }

    void le_set_purpose_selectable(LeHandle *handle, int32_t purpose, int32_t selectable)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_purpose_selectable(static_cast<le::ViewLayerPurpose>(purpose), selectable != 0);
    }

    void le_zoom(LeHandle *handle, double factor, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        zoom_unlocked(handle, factor, x, y);
    }

    void le_pan(LeHandle *handle, double x_factor, double y_factor)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        pan_unlocked(handle, x_factor, y_factor);
    }

    void le_set_viewport_size(LeHandle *handle, int32_t width_px, int32_t height_px)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_viewport_size(width_px, height_px);
    }

    void le_fit_scene(LeHandle *handle, int32_t padding_px)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        fit_scene_unlocked(handle, padding_px);
    }

    int64_t le_minor_grid_spacing(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.minor_grid_spacing();
    }

    void le_set_minor_grid_spacing(LeHandle *handle, int64_t dbu)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_minor_grid_spacing(dbu);
    }

    int64_t le_major_grid_spacing(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return handle->scene.major_grid_spacing();
    }

    void le_set_major_grid_spacing(LeHandle *handle, int64_t dbu)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.set_major_grid_spacing(dbu);
    }

    void le_set_mouse_position(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        handle->scene.set_mouse_position(x, y);

        const auto &shapes = handle->pipeline.run(handle->root, handle->scene, handle->view_layers);
        handle->scene.set_hover(le::Pipeline::hit_test_point(shapes, handle->view_layers, handle->scene, *handle->scene.mouse_dbu_position()));
    }

    void le_clear_mouse_position(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.clear_mouse_position();
        handle->scene.clear_hover();
    }

    LeSnappedMousePosition le_snapped_mouse_position(LeHandle *handle)
    {
        if (!handle)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::optional<le::Point> snapped = handle->scene.snapped_mouse_position();
        if (!snapped)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        // Single shared/global Technology, same assumption le_read_lef's
        // own ViewLayerSet::build_for_technology(..., technology_ids.front())
        // call already makes.
        const auto technology_ids = handle->root.get_technology_ids();
        if (technology_ids.empty())
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        const le::TechnologyData *technology = handle->root.get_technology(technology_ids.front());
        if (!technology || technology->database_units_microns <= 0.0)
            return LeSnappedMousePosition{.x_um = 0.0, .y_um = 0.0, .has_position = 0};

        return LeSnappedMousePosition{
            .x_um = static_cast<double>(snapped->x) / technology->database_units_microns,
            .y_um = static_cast<double>(snapped->y) / technology->database_units_microns,
            .has_position = 1,
        };
    }

    void le_key_down(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.press_key(key_code);

        switch (key_code)
        {
        case LE_KEY_ZOOM:
        {
            // Unlocked variants (see their own comment) - handle->mutex_
            // is already held above; le_zoom/le_fit_scene/le_pan
            // themselves would re-lock it and deadlock.
            const double factor = handle->scene.is_key_held(LE_KEY_SHIFT) ? -kKeyZoomFactor : kKeyZoomFactor;
            zoom_unlocked(handle, factor, handle->scene.mouse_x_px(), handle->scene.mouse_y_px());
            break;
        }
        case LE_KEY_FIT:
            if (handle->scene.is_key_held(LE_KEY_CTRL))
                fit_selected_unlocked(handle, kKeyFitPaddingPx);
            else
                fit_scene_unlocked(handle, kKeyFitPaddingPx);
            break;
        case LE_KEY_PAN_LEFT:
            pan_unlocked(handle, -kKeyPanFactor, 0.0);
            break;
        case LE_KEY_PAN_RIGHT:
            pan_unlocked(handle, kKeyPanFactor, 0.0);
            break;
        case LE_KEY_PAN_UP:
            pan_unlocked(handle, 0.0, kKeyPanFactor);
            break;
        case LE_KEY_PAN_DOWN:
            pan_unlocked(handle, 0.0, -kKeyPanFactor);
            break;
        case LE_KEY_SELECT_ALL:
            if (handle->scene.is_key_held(LE_KEY_CTRL))
                select_all_unlocked(handle);
            break;
        case LE_KEY_1:
        case LE_KEY_2:
        case LE_KEY_3:
        case LE_KEY_4:
        case LE_KEY_5:
        case LE_KEY_6:
        case LE_KEY_7:
        case LE_KEY_8:
        case LE_KEY_9:
        {
            // UPDATES.md 9.7 - the same physical 1-9 keys address the
            // 11th..19th ROUTING layer instead of the 1st..9th while Ctrl
            // is held (LE_KEY_0, not gated on Ctrl, covers the 10th - see
            // its own comment in api.hpp).
            const int base_index = key_code - LE_KEY_1; // 0-8
            const int routing_index = handle->scene.is_key_held(LE_KEY_CTRL) ? base_index + 10 : base_index;
            toggle_routing_layer_visibility_unlocked(handle, routing_index);
            break;
        }
        case LE_KEY_0:
            toggle_routing_layer_visibility_unlocked(handle, 9); // the 10th ROUTING layer
            break;
        case LE_KEY_DESELECT_ALL:
            if (handle->scene.is_key_held(LE_KEY_CTRL))
                handle->scene.clear_selection();
            break;
        default:
            break;
        }
    }

    void le_key_up(LeHandle *handle, int32_t key_code)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.release_key(key_code);
    }

    int32_t le_is_key_held(LeHandle *handle, int32_t key_code)
    {
        return handle && handle->scene.is_key_held(key_code) ? 1 : 0;
    }

    void le_clear_all_keys(LeHandle *handle)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.clear_all_keys();
    }

    void le_mouse_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.begin_drag(x, y);
    }

    void le_zoom_drag_down(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle)
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        handle->scene.begin_drag(x, y, le::Scene::DragKind::ZOOM);
    }

    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle || !handle->scene.is_dragging())
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const int32_t dx = x - handle->scene.drag_start_x_px();
        const int32_t dy = y - handle->scene.drag_start_y_px();
        const bool is_click = dx * dx + dy * dy < kClickDragThresholdPx * kClickDragThresholdPx;

        if (handle->scene.drag_kind() == le::Scene::DragKind::ZOOM)
        {
            // Rectangle-zoom (UPDATES.md 9.3) - purely navigational,
            // selection is untouched. A click-sized release is a no-op
            // (fitting to a near-zero-size rect would produce an absurd
            // scale) - same threshold used for the select gesture below.
            if (!is_click)
            {
                const le::Point start = handle->scene.pixel_to_dbu(handle->scene.drag_start_x_px(), handle->scene.drag_start_y_px());
                const le::Point end = handle->scene.pixel_to_dbu(x, y);
                const le::Rect zoom_rect{
                    .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                    .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
                };
                handle->scene.fit_to_content(zoom_rect, 0);
            }
            handle->scene.end_drag();
            return;
        }

        const auto &shapes = handle->pipeline.run(handle->root, handle->scene, handle->view_layers);
        const bool shift = handle->scene.is_key_held(LE_KEY_SHIFT);

        if (!shift)
            handle->scene.clear_selection();

        if (is_click)
        {
            // Computed straight from this call's own x/y, not
            // Scene::drag_rect_dbu()/mouse_dbu_position() - those read the
            // separately-tracked *stored* mouse position (see
            // le_set_mouse_position), which this call has no guaranteed
            // ordering against.
            const auto hit = le::Pipeline::hit_test_point(shapes, handle->view_layers, handle->scene, handle->scene.pixel_to_dbu(x, y));
            if (hit)
                handle->scene.select(hit->origin, hit->outline);
        }
        else
        {
            const le::Point start = handle->scene.pixel_to_dbu(handle->scene.drag_start_x_px(), handle->scene.drag_start_y_px());
            const le::Point end = handle->scene.pixel_to_dbu(x, y);
            const le::Rect drag_rect{
                .ll = le::Point{std::min(start.x, end.x), std::min(start.y, end.y)},
                .ur = le::Point{std::max(start.x, end.x), std::max(start.y, end.y)},
            };

            for (const le::HoverTarget &hit : le::Pipeline::hit_test_rect(shapes, handle->view_layers, handle->scene, drag_rect))
                handle->scene.select(hit.origin, hit.outline);
        }

        handle->scene.end_drag();
    }

    const char *le_tooltip_message(LeHandle *handle)
    {
        if (!handle)
            return nullptr;
        return kSelectModeTooltip;
    }

    int32_t le_selection_count(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        return static_cast<int32_t>(handle->scene.selection().size());
    }

    int64_t le_selection_version(LeHandle *handle)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        return static_cast<int64_t>(handle->scene.selection_version());
    }

    int32_t le_selected_object_kind(LeHandle *handle, int32_t selection_index)
    {
        if (!handle || selection_index < 0)
            return -1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::vector<le::SelectedObject> &selection = handle->scene.selection();
        if (static_cast<size_t>(selection_index) >= selection.size())
            return -1;

        return std::holds_alternative<le::TerminalId>(selection[static_cast<size_t>(selection_index)].origin)
                   ? LE_SELECTION_KIND_TERMINAL
                   : LE_SELECTION_KIND_OBSTRUCTION;
    }

    int32_t le_selected_object_property_count(LeHandle *handle, int32_t selection_index)
    {
        if (!handle || selection_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::vector<le::SelectedObject> &selection = handle->scene.selection();
        if (static_cast<size_t>(selection_index) >= selection.size())
            return 0;

        handle->cached_properties = build_selected_object_properties(handle->root, selection[static_cast<size_t>(selection_index)]);
        handle->cached_property_selection_index = selection_index;
        return static_cast<int32_t>(handle->cached_properties.size());
    }

    LeProperty le_selected_object_property_at(LeHandle *handle, int32_t selection_index, int32_t property_index)
    {
        const LeProperty invalid{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
        if (!handle || selection_index < 0 || property_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::vector<le::SelectedObject> &selection = handle->scene.selection();
        if (static_cast<size_t>(selection_index) >= selection.size())
            return invalid;

        if (handle->cached_property_selection_index != selection_index)
        {
            handle->cached_properties = build_selected_object_properties(handle->root, selection[static_cast<size_t>(selection_index)]);
            handle->cached_property_selection_index = selection_index;
        }

        if (static_cast<size_t>(property_index) >= handle->cached_properties.size())
            return invalid;

        const le::PropertyValue &property = handle->cached_properties[static_cast<size_t>(property_index)];
        return LeProperty{
            .name = property.name.c_str(),
            .type = to_c_property_type(property.type),
            .string_value = property.string_value.c_str(),
            .int_value = property.int_value,
            .double_value = property.double_value,
        };
    }

    LeTerminalId le_create_terminal(LeHandle *handle, LeAbstractId abstract_id, const char *name, int32_t direction)
    {
        const LeTerminalId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::AbstractId abstract = from_c(abstract_id);
        if (!handle->root.get_abstract(abstract))
            return invalid;

        const LeTerminalId result = to_c(handle->root.create_terminal(le::TerminalData{
            .abstract = abstract,
            .name = name,
            .direction = static_cast<le::SignalDirection>(direction),
        }));
        handle->root.bump_mutation_version();
        return result;
    }

    int32_t le_terminal_property_count(LeHandle *handle, LeTerminalId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalId terminal_id = from_c(id);
        handle->cached_terminal_properties = build_terminal_properties(handle->root, terminal_id);
        handle->cached_terminal_property_id = terminal_id;
        return static_cast<int32_t>(handle->cached_terminal_properties.size());
    }

    LeProperty le_terminal_property_at(LeHandle *handle, LeTerminalId id, int32_t index)
    {
        const LeProperty invalid{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalId terminal_id = from_c(id);
        if (handle->cached_terminal_property_id != terminal_id)
        {
            handle->cached_terminal_properties = build_terminal_properties(handle->root, terminal_id);
            handle->cached_terminal_property_id = terminal_id;
        }

        if (static_cast<size_t>(index) >= handle->cached_terminal_properties.size())
            return invalid;
        return to_c(handle->cached_terminal_properties[static_cast<size_t>(index)]);
    }

    int le_set_terminal_name(LeHandle *handle, LeTerminalId id, const char *name)
    {
        if (!handle || !name)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::TerminalData *terminal = handle->root.get_terminal(from_c(id));
        if (!terminal)
            return 1;
        terminal->name = name;
        handle->root.bump_mutation_version();
        return 0;
    }

    int le_set_terminal_direction(LeHandle *handle, LeTerminalId id, int32_t direction)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::TerminalData *terminal = handle->root.get_terminal(from_c(id));
        if (!terminal)
            return 1;
        terminal->direction = static_cast<le::SignalDirection>(direction);
        handle->root.bump_mutation_version();
        return 0;
    }

    int le_delete_terminal(LeHandle *handle, LeTerminalId id)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalId terminal_id = from_c(id);
        if (!handle->root.get_terminal(terminal_id))
            return 1;

        // Cascade first (see le_delete_terminal's own doc comment for why
        // this API layer does this rather than leaving it to
        // Root::delete_terminal's generic no-cascade default) - copy the
        // port id list first since deleting a port mutates the same
        // index Root::get_terminal_ports() reads from.
        const std::vector<le::TerminalPortId> port_ids = handle->root.get_terminal_ports(terminal_id);
        for (const le::TerminalPortId port_id : port_ids)
            handle->root.delete_terminal_port(port_id);

        const bool deleted = handle->root.delete_terminal(terminal_id);
        handle->root.bump_mutation_version();
        return deleted ? 0 : 1;
    }

    int32_t le_search_terminal(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            handle->messages.push_back(fmt::format("ERROR: le_search_terminal: {}", expr.error()));
            return -1;
        }

        handle->terminal_search_results = handle->root.search_terminal(
            [&expr](const le::Root &root, le::TerminalId id, const le::TerminalData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->terminal_search_results.size());
    }

    LeTerminalId le_search_result_terminal_at(LeHandle *handle, int32_t index)
    {
        const LeTerminalId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        if (static_cast<size_t>(index) >= handle->terminal_search_results.size())
            return invalid;
        return to_c(handle->terminal_search_results[static_cast<size_t>(index)]);
    }

    LeTerminalPortId le_create_terminal_port(LeHandle *handle, LeTerminalId terminal_id)
    {
        const LeTerminalPortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalId terminal = from_c(terminal_id);
        if (!handle->root.get_terminal(terminal))
            return invalid;

        const LeTerminalPortId result = to_c(handle->root.create_terminal_port(le::TerminalPortData{.terminal = terminal}));
        handle->root.bump_mutation_version();
        return result;
    }

    int32_t le_terminal_port_property_count(LeHandle *handle, LeTerminalPortId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalPortId port_id = from_c(id);
        handle->cached_terminal_port_properties = build_terminal_port_properties(handle->root, port_id);
        handle->cached_terminal_port_property_id = port_id;
        return static_cast<int32_t>(handle->cached_terminal_port_properties.size());
    }

    LeProperty le_terminal_port_property_at(LeHandle *handle, LeTerminalPortId id, int32_t index)
    {
        const LeProperty invalid{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalPortId port_id = from_c(id);
        if (handle->cached_terminal_port_property_id != port_id)
        {
            handle->cached_terminal_port_properties = build_terminal_port_properties(handle->root, port_id);
            handle->cached_terminal_port_property_id = port_id;
        }

        if (static_cast<size_t>(index) >= handle->cached_terminal_port_properties.size())
            return invalid;
        return to_c(handle->cached_terminal_port_properties[static_cast<size_t>(index)]);
    }

    int le_delete_terminal_port(LeHandle *handle, LeTerminalPortId id)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalPortId port_id = from_c(id);
        if (!handle->root.get_terminal_port(port_id))
            return 1;

        // Cascade first (see le_delete_terminal_port's own doc comment) -
        // copy the shape id list first since deleting a shape mutates the
        // same index Root::get_terminal_port_shapes() reads from.
        const std::vector<le::ShapeId> shape_ids = handle->root.get_terminal_port_shapes(port_id);
        for (const le::ShapeId shape_id : shape_ids)
            handle->root.delete_shape(shape_id);

        const bool deleted = handle->root.delete_terminal_port(port_id);
        handle->root.bump_mutation_version();
        return deleted ? 0 : 1;
    }

    int32_t le_search_terminal_port(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            handle->messages.push_back(fmt::format("ERROR: le_search_terminal_port: {}", expr.error()));
            return -1;
        }

        handle->terminal_port_search_results = handle->root.search_terminal_port(
            [&expr](const le::Root &root, le::TerminalPortId id, const le::TerminalPortData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->terminal_port_search_results.size());
    }

    LeTerminalPortId le_search_result_terminal_port_at(LeHandle *handle, int32_t index)
    {
        const LeTerminalPortId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        if (static_cast<size_t>(index) >= handle->terminal_port_search_results.size())
            return invalid;
        return to_c(handle->terminal_port_search_results[static_cast<size_t>(index)]);
    }

    LeObstructionId le_create_obstruction(LeHandle *handle, LeAbstractId abstract_id)
    {
        const LeObstructionId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::AbstractId abstract = from_c(abstract_id);
        if (!handle->root.get_abstract(abstract))
            return invalid;

        const LeObstructionId result = to_c(handle->root.create_obstruction(le::ObstructionData{.abstract = abstract}));
        handle->root.bump_mutation_version();
        return result;
    }

    int32_t le_obstruction_property_count(LeHandle *handle, LeObstructionId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ObstructionId obstruction_id = from_c(id);
        handle->cached_obstruction_properties = build_obstruction_properties(handle->root, obstruction_id);
        handle->cached_obstruction_property_id = obstruction_id;
        return static_cast<int32_t>(handle->cached_obstruction_properties.size());
    }

    LeProperty le_obstruction_property_at(LeHandle *handle, LeObstructionId id, int32_t index)
    {
        const LeProperty invalid{.name = nullptr, .type = LE_PROPERTY_TYPE_STRING, .string_value = nullptr, .int_value = 0, .double_value = 0.0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ObstructionId obstruction_id = from_c(id);
        if (handle->cached_obstruction_property_id != obstruction_id)
        {
            handle->cached_obstruction_properties = build_obstruction_properties(handle->root, obstruction_id);
            handle->cached_obstruction_property_id = obstruction_id;
        }

        if (static_cast<size_t>(index) >= handle->cached_obstruction_properties.size())
            return invalid;
        return to_c(handle->cached_obstruction_properties[static_cast<size_t>(index)]);
    }

    int le_delete_obstruction(LeHandle *handle, LeObstructionId id)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ObstructionId obstruction_id = from_c(id);
        if (!handle->root.get_obstruction(obstruction_id))
            return 1;

        // Cascade first (see le_delete_obstruction's own doc comment).
        const std::vector<le::ShapeId> shape_ids = handle->root.get_obstruction_shapes(obstruction_id);
        for (const le::ShapeId shape_id : shape_ids)
            handle->root.delete_shape(shape_id);

        const bool deleted = handle->root.delete_obstruction(obstruction_id);
        handle->root.bump_mutation_version();
        return deleted ? 0 : 1;
    }

    int32_t le_search_obstruction(LeHandle *handle, const char *filter_expression)
    {
        if (!handle || !filter_expression)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        auto expr = le::parse_filter_expression(filter_expression);
        if (!expr)
        {
            handle->messages.push_back(fmt::format("ERROR: le_search_obstruction: {}", expr.error()));
            return -1;
        }

        handle->obstruction_search_results = handle->root.search_obstruction(
            [&expr](const le::Root &root, le::ObstructionId id, const le::ObstructionData &data)
            { return le::evaluate_filter(*expr, root, id, data); });
        return static_cast<int32_t>(handle->obstruction_search_results.size());
    }

    LeObstructionId le_search_result_obstruction_at(LeHandle *handle, int32_t index)
    {
        const LeObstructionId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        if (static_cast<size_t>(index) >= handle->obstruction_search_results.size())
            return invalid;
        return to_c(handle->obstruction_search_results[static_cast<size_t>(index)]);
    }

    int le_update_abstract_boundary(LeHandle *handle, LeAbstractId id, const double *coords_um, int32_t coord_count)
    {
        if (!handle || !coords_um || coord_count < 6 || coord_count % 2 != 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::AbstractData *abstract = handle->root.get_abstract(from_c(id));
        if (!abstract)
            return 1;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return 1;

        le::Polygon polygon;
        for (int32_t i = 0; i < coord_count; i += 2)
            polygon.points.push_back(le::Point{.x = to_dbu(coords_um[i], *dbu_per_um), .y = to_dbu(coords_um[i + 1], *dbu_per_um)});

        abstract->boundary = {std::move(polygon)};
        handle->root.bump_mutation_version();
        return 0;
    }

    LeShapeId le_create_terminal_port_shape(LeHandle *handle, LeTerminalPortId port_id, const char *layer_name)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !layer_name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::TerminalPortId port = from_c(port_id);
        if (!handle->root.get_terminal_port(port))
            return invalid;

        const LeShapeId result = to_c(handle->root.create_shape(le::ShapeData{.terminal_port = port, .layer_name = layer_name}));
        handle->root.bump_mutation_version();
        return result;
    }

    LeShapeId le_create_obstruction_shape(LeHandle *handle, LeObstructionId obstruction_id, const char *layer_name)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || !layer_name)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ObstructionId obstruction = from_c(obstruction_id);
        if (!handle->root.get_obstruction(obstruction))
            return invalid;

        const LeShapeId result = to_c(handle->root.create_shape(le::ShapeData{.obstruction = obstruction, .layer_name = layer_name}));
        handle->root.bump_mutation_version();
        return result;
    }

    int32_t le_terminal_port_shape_count(LeHandle *handle, LeTerminalPortId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_terminal_port_shapes(from_c(id)).size());
    }

    LeShapeId le_terminal_port_shape_at(LeHandle *handle, LeTerminalPortId id, int32_t index)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::vector<le::ShapeId> &shapes = handle->root.get_terminal_port_shapes(from_c(id));
        if (static_cast<size_t>(index) >= shapes.size())
            return invalid;
        return to_c(shapes[static_cast<size_t>(index)]);
    }

    int32_t le_obstruction_shape_count(LeHandle *handle, LeObstructionId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        return static_cast<int32_t>(handle->root.get_obstruction_shapes(from_c(id)).size());
    }

    LeShapeId le_obstruction_shape_at(LeHandle *handle, LeObstructionId id, int32_t index)
    {
        const LeShapeId invalid{.index = UINT32_MAX, .generation = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::vector<le::ShapeId> &shapes = handle->root.get_obstruction_shapes(from_c(id));
        if (static_cast<size_t>(index) >= shapes.size())
            return invalid;
        return to_c(shapes[static_cast<size_t>(index)]);
    }

    const char *le_shape_layer_name(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return nullptr;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? shape->layer_name.c_str() : nullptr;
    }

    int le_set_shape_layer_name(LeHandle *handle, LeShapeId id, const char *layer_name)
    {
        if (!handle || !layer_name)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape)
            return 1;
        shape->layer_name = layer_name;
        handle->root.bump_mutation_version();
        return 0;
    }

    int le_delete_shape(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);
        const bool deleted = handle->root.delete_shape(from_c(id));
        handle->root.bump_mutation_version();
        return deleted ? 0 : 1;
    }

    int32_t le_shape_rect_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->rects.size()) : 0;
    }

    LeRectUm le_shape_rect_at(LeHandle *handle, LeShapeId id, int32_t index)
    {
        const LeRectUm invalid{.ll_x_um = 0, .ll_y_um = 0, .ur_x_um = 0, .ur_y_um = 0};
        if (!handle || index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(index) >= shape->rects.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Rect &rect = shape->rects[static_cast<size_t>(index)];
        return LeRectUm{
            .ll_x_um = to_um(rect.ll.x, *dbu_per_um),
            .ll_y_um = to_um(rect.ll.y, *dbu_per_um),
            .ur_x_um = to_um(rect.ur.x, *dbu_per_um),
            .ur_y_um = to_um(rect.ur.y, *dbu_per_um),
        };
    }

    int le_add_shape_rect(LeHandle *handle, LeShapeId id, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape)
            return 1;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return 1;

        shape->rects.push_back(le::Rect{
            .ll = le::Point{.x = to_dbu(ll_x_um, *dbu_per_um), .y = to_dbu(ll_y_um, *dbu_per_um)},
            .ur = le::Point{.x = to_dbu(ur_x_um, *dbu_per_um), .y = to_dbu(ur_y_um, *dbu_per_um)},
        });
        handle->root.bump_mutation_version();
        return 0;
    }

    int le_remove_shape_rect(LeHandle *handle, LeShapeId id, int32_t index)
    {
        if (!handle || index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(index) >= shape->rects.size())
            return 1;
        shape->rects.erase(shape->rects.begin() + index);
        handle->root.bump_mutation_version();
        return 0;
    }

    int32_t le_shape_polygon_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->polygons.size()) : 0;
    }

    int32_t le_shape_polygon_point_count(LeHandle *handle, LeShapeId id, int32_t polygon_index)
    {
        if (!handle || polygon_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return 0;
        return static_cast<int32_t>(shape->polygons[static_cast<size_t>(polygon_index)].points.size());
    }

    LePointUm le_shape_polygon_point_at(LeHandle *handle, LeShapeId id, int32_t polygon_index, int32_t point_index)
    {
        const LePointUm invalid{.x_um = 0, .y_um = 0};
        if (!handle || polygon_index < 0 || point_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return invalid;
        const std::vector<le::Point> &points = shape->polygons[static_cast<size_t>(polygon_index)].points;
        if (static_cast<size_t>(point_index) >= points.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Point &point = points[static_cast<size_t>(point_index)];
        return LePointUm{.x_um = to_um(point.x, *dbu_per_um), .y_um = to_um(point.y, *dbu_per_um)};
    }

    int le_add_shape_polygon(LeHandle *handle, LeShapeId id, const double *points_um, int32_t point_coord_count)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape)
            return 1;

        std::optional<le::Polygon> polygon = build_polygon_from_flat_points(handle->root, points_um, point_coord_count, /*min_points=*/3);
        if (!polygon)
            return 1;

        shape->polygons.push_back(std::move(*polygon));
        handle->root.bump_mutation_version();
        return 0;
    }

    int le_remove_shape_polygon(LeHandle *handle, LeShapeId id, int32_t polygon_index)
    {
        if (!handle || polygon_index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(polygon_index) >= shape->polygons.size())
            return 1;
        shape->polygons.erase(shape->polygons.begin() + polygon_index);
        handle->root.bump_mutation_version();
        return 0;
    }

    int32_t le_shape_path_count(LeHandle *handle, LeShapeId id)
    {
        if (!handle)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        return shape ? static_cast<int32_t>(shape->paths.size()) : 0;
    }

    double le_shape_path_width_um(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 0;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return 0;
        return to_um(static_cast<int64_t>(shape->paths[static_cast<size_t>(path_index)].width), *dbu_per_um);
    }

    int32_t le_shape_path_point_count(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 0;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 0;
        return static_cast<int32_t>(shape->paths[static_cast<size_t>(path_index)].polygon.points.size());
    }

    LePointUm le_shape_path_point_at(LeHandle *handle, LeShapeId id, int32_t path_index, int32_t point_index)
    {
        const LePointUm invalid{.x_um = 0, .y_um = 0};
        if (!handle || path_index < 0 || point_index < 0)
            return invalid;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return invalid;
        const std::vector<le::Point> &points = shape->paths[static_cast<size_t>(path_index)].polygon.points;
        if (static_cast<size_t>(point_index) >= points.size())
            return invalid;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return invalid;

        const le::Point &point = points[static_cast<size_t>(point_index)];
        return LePointUm{.x_um = to_um(point.x, *dbu_per_um), .y_um = to_um(point.y, *dbu_per_um)};
    }

    int le_add_shape_path(LeHandle *handle, LeShapeId id, double width_um, const double *points_um, int32_t point_coord_count)
    {
        if (!handle)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape)
            return 1;

        std::optional<le::Polygon> polygon = build_polygon_from_flat_points(handle->root, points_um, point_coord_count, /*min_points=*/2);
        if (!polygon)
            return 1;

        const std::optional<double> dbu_per_um = database_units_microns(handle->root);
        if (!dbu_per_um)
            return 1;

        shape->paths.push_back(le::Path{.polygon = std::move(*polygon), .width = static_cast<uint64_t>(to_dbu(width_um, *dbu_per_um))});
        handle->root.bump_mutation_version();
        return 0;
    }

    int le_remove_shape_path(LeHandle *handle, LeShapeId id, int32_t path_index)
    {
        if (!handle || path_index < 0)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        le::ShapeData *shape = handle->root.get_shape(from_c(id));
        if (!shape || static_cast<size_t>(path_index) >= shape->paths.size())
            return 1;
        shape->paths.erase(shape->paths.begin() + path_index);
        handle->root.bump_mutation_version();
        return 0;
    }

    LePixelBuffer le_render_pixel_buffer(LeHandle *handle)
    {
        if (!handle)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto &shapes = handle->pipeline.run(handle->root, handle->scene, handle->view_layers);
        const auto &pixel_shapes = handle->renderer.transform_to_pixels(handle->root, shapes, handle->scene);
        const auto &picture = handle->renderer.build_picture(pixel_shapes, handle->scene, handle->view_layers, handle->root);
        const auto &tiny_shapes = handle->pipeline.run_tiny_shapes(handle->root, handle->scene, handle->view_layers);
        const auto &tiny_pixel_shapes = handle->renderer.transform_tiny_shapes_to_pixels(handle->root, tiny_shapes, handle->scene);
        const auto &tiny_shapes_picture = handle->renderer.build_tiny_shapes_picture(handle->root, tiny_pixel_shapes, handle->scene, handle->view_layers);
        const auto &overlay_picture = handle->renderer.build_overlay_picture(handle->scene);
        const auto &selection_overlay_picture = handle->renderer.build_selection_overlay_picture(handle->scene);
        const auto &buffer = handle->renderer.compose_with_overlays(handle->root, picture, tiny_shapes_picture, overlay_picture, selection_overlay_picture, handle->scene);

        return LePixelBuffer{
            .data = buffer.data,
            .width = buffer.width,
            .height = buffer.height,
            .row_bytes = static_cast<int64_t>(buffer.row_bytes),
        };
    }
}
