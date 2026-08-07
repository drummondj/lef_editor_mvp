#include "api.hpp"
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../io/lef_reader.hpp"
#include "../pipeline/pipeline.hpp"
#include "../render/render.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <algorithm>
#include <cmath>
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
};

namespace
{
    LeLibraryId to_c(le::LibraryId id) { return LeLibraryId{.index = id.index, .generation = id.generation}; }
    LeDesignId to_c(le::DesignId id) { return LeDesignId{.index = id.index, .generation = id.generation}; }
    LeAbstractId to_c(le::AbstractId id) { return LeAbstractId{.index = id.index, .generation = id.generation}; }

    le::DesignId from_c(LeDesignId id) { return le::DesignId{.index = id.index, .generation = id.generation}; }

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
                std::vector<le::Shape> shapes;
                shapes.reserve(port_ids.size()); // a lower bound (each port usually has one Shape) but avoids most reallocations
                for (const le::TerminalPortId port_id : port_ids)
                {
                    const le::TerminalPortData *port = root.get_terminal_port(port_id);
                    if (port)
                        shapes.insert(shapes.end(), port->shapes.begin(), port->shapes.end());
                }
                push_bbox_property(properties, le::Geometry::bbox(shapes), dbu_per_um);
            }
        }
        else if (const le::ObstructionId *obstruction_id = std::get_if<le::ObstructionId>(&selected.origin))
        {
            const le::ObstructionData *obstruction = root.get_obstruction(*obstruction_id);
            if (!obstruction)
                return properties;

            properties = le::to_properties(*obstruction); // shapes_count

            if (selected.piece)
            {
                push_bbox_property(properties, le::Geometry::bbox(*selected.piece), dbu_per_um);
                properties.push_back(le::PropertyValue::make_string("layer_name", selected.piece->layer_name));
            }
            else
            {
                push_bbox_property(properties, le::Geometry::bbox(obstruction->shapes), dbu_per_um);
            }
        }

        return properties;
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
        if (!handle || !path)
            return 1;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const std::filesystem::path lef_path(path);
        le::LEFReader reader;
        const int result = reader.read_lef(lef_path.string(), handle->root, lef_path.stem().string());
        if (result != 0)
            return result;

        // Rebuilt after every successful read, not just the first, so a
        // later LEF file's own new physical layers (e.g. a second macro
        // file with inline LAYER declarations) are picked up too - cheap
        // relative to a full LEF parse, so correctness here wins over the
        // small extra cost without needing a benchmark to justify it.
        const auto technology_ids = handle->root.get_technology_ids();
        if (!technology_ids.empty())
            handle->view_layers = le::ViewLayerSet::build_for_technology(handle->root, technology_ids.front());

        return 0;
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

    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y)
    {
        if (!handle || !handle->scene.is_dragging())
            return;
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const int32_t dx = x - handle->scene.drag_start_x_px();
        const int32_t dy = y - handle->scene.drag_start_y_px();
        const bool is_click = dx * dx + dy * dy < kClickDragThresholdPx * kClickDragThresholdPx;

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

            for (const le::SelectionRef &ref : le::Pipeline::hit_test_rect(shapes, handle->view_layers, handle->scene, drag_rect))
                handle->scene.select(ref);
        }

        handle->scene.end_drag();
    }

    int32_t le_selection_count(LeHandle *handle)
    {
        return handle ? static_cast<int32_t>(handle->scene.selection().size()) : 0;
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

    LePixelBuffer le_render_pixel_buffer(LeHandle *handle)
    {
        if (!handle)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};
        std::lock_guard<std::mutex> lock(handle->mutex_);

        const auto &shapes = handle->pipeline.run(handle->root, handle->scene, handle->view_layers);
        const auto &pixel_shapes = handle->renderer.transform_to_pixels(shapes, handle->scene);
        const auto &picture = handle->renderer.build_picture(pixel_shapes, handle->scene, handle->view_layers, handle->root);
        const auto &overlay_picture = handle->renderer.build_overlay_picture(handle->scene);
        const auto &buffer = handle->renderer.compose_with_overlays(picture, overlay_picture, handle->scene);

        return LePixelBuffer{
            .data = buffer.data,
            .width = buffer.width,
            .height = buffer.height,
            .row_bytes = static_cast<int64_t>(buffer.row_bytes),
        };
    }
}
