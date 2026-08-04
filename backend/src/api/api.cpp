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

    void le_set_pan(LeHandle *handle, int64_t x, int64_t y)
    {
        if (!handle)
            return;
        handle->scene.set_pan(le::Point{.x = x, .y = y});
    }

    void le_set_scale(LeHandle *handle, double pixels_per_dbu)
    {
        if (!handle)
            return;
        handle->scene.set_scale(pixels_per_dbu);
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
