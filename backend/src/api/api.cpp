#include "api.hpp"
#include "../database/database.hpp"
#include "../geometry/geometry.hpp"
#include "../io/lef_reader.hpp"
#include "../pipeline/pipeline.hpp"
#include "../render/render.hpp"
#include "../scene/scene.hpp"
#include "../view_style/view_style.hpp"
#include <filesystem>

// The real, C++-only definition behind the opaque LeHandle - never exposed
// in api.hpp. Owns everything needed to load a LEF file and render it: one
// Pipeline and one Renderer per handle (not one per call), matching the
// "reuse across repeated calls" lifetime their own internal CachedStage
// caching is designed around.
struct LeHandle
{
    le::Root root;
    le::ViewLayerSet view_layers;
    le::Scene scene;
    le::Pipeline pipeline;
    le::Renderer renderer;
};

namespace
{
    LeLibraryId to_c(le::LibraryId id) { return LeLibraryId{.index = id.index, .generation = id.generation}; }
    LeDesignId to_c(le::DesignId id) { return LeDesignId{.index = id.index, .generation = id.generation}; }
    LeAbstractId to_c(le::AbstractId id) { return LeAbstractId{.index = id.index, .generation = id.generation}; }

    le::DesignId from_c(LeDesignId id) { return le::DesignId{.index = id.index, .generation = id.generation}; }
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
        return static_cast<int32_t>(handle->root.get_design_size());
    }

    const char *le_design_name(LeHandle *handle, int32_t index)
    {
        if (!handle || index < 0)
            return nullptr;

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
        return static_cast<int32_t>(handle->root.get_library_size());
    }

    LeLibraryInfo le_library_at(LeHandle *handle, int32_t index)
    {
        const LeLibraryInfo invalid{.id = {UINT32_MAX, 0}, .name = nullptr};
        if (!handle || index < 0)
            return invalid;

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

        const le::DesignId id = from_c(design_id);
        if (!handle->root.get_design(id))
            return 1;

        handle->scene.set_current_abstract(handle->root.get_design_abstract(id));
        return 0;
    }

    void le_zoom(LeHandle *handle, double factor, int32_t x, int32_t y)
    {
        if (!handle)
            return;

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
        const int64_t pan_x = static_cast<int64_t>(dbu_x - static_cast<double>(x) / new_scale);
        const int64_t pan_y = static_cast<int64_t>(dbu_y - (viewport_height - static_cast<double>(y)) / new_scale);

        handle->scene.set_scale(new_scale);
        handle->scene.set_pan(le::Point{.x = pan_x, .y = pan_y});
    }

    void le_pan(LeHandle *handle, double x_factor, double y_factor)
    {
        if (!handle)
            return;

        const double scale = handle->scene.scale();
        const le::Point pan = handle->scene.pan();

        const int64_t dx = static_cast<int64_t>(x_factor * handle->scene.viewport_width_px() / scale);
        const int64_t dy = static_cast<int64_t>(y_factor * handle->scene.viewport_height_px() / scale);

        handle->scene.set_pan(le::Point{.x = pan.x + dx, .y = pan.y + dy});
    }

    void le_set_viewport_size(LeHandle *handle, int32_t width_px, int32_t height_px)
    {
        if (!handle)
            return;
        handle->scene.set_viewport_size(width_px, height_px);
    }

    void le_fit_scene(LeHandle *handle, int32_t padding_px)
    {
        if (!handle)
            return;

        const auto &generated = handle->pipeline.generate_shapes(handle->root, handle->scene.current_abstract(), handle->view_layers);

        std::vector<const le::Shape *> shape_ptrs;
        shape_ptrs.reserve(generated.size());
        for (const auto &rs : generated)
            shape_ptrs.push_back(&rs.shape);

        handle->scene.fit_to_content(le::Geometry::bbox(shape_ptrs), padding_px);
    }

    LePixelBuffer le_render_pixel_buffer(LeHandle *handle)
    {
        if (!handle)
            return LePixelBuffer{.data = nullptr, .width = 0, .height = 0, .row_bytes = 0};

        const auto &shapes = handle->pipeline.run(handle->root, handle->scene, handle->view_layers);
        const auto &pixel_shapes = handle->renderer.transform_to_pixels(shapes, handle->scene);
        const auto &picture = handle->renderer.build_picture(pixel_shapes, handle->scene, handle->view_layers);
        const auto &buffer = handle->renderer.rasterize(picture, handle->scene);

        return LePixelBuffer{
            .data = buffer.data,
            .width = buffer.width,
            .height = buffer.height,
            .row_bytes = static_cast<int64_t>(buffer.row_bytes),
        };
    }
}
