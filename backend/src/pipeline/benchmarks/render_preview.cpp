#include "../../geometry/geometry.hpp"
#include "../../io/lef_reader.hpp"
#include "../../render/render.hpp"
#include "../pipeline.hpp"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include <cstdio>
#include <filesystem>

using namespace le;

namespace
{
    constexpr int kOutputSize = 1000;
    constexpr int kPaddingPx = 20;

    // Fits abstract_id's content bbox into a kOutputSize x kOutputSize image:
    // uniform scale (no stretch) with kPaddingPx of margin on every side, and
    // pan set to center the content. Falls back to a fixed default scale/pan
    // if the abstract has no shapes (empty bbox), rather than dividing by
    // zero width/height.
    Scene fit_scene(const Root &root, AbstractId abstract_id, Pipeline &pipeline, const ViewLayerSet &view_layers)
    {
        Scene scene;
        scene.set_current_abstract(abstract_id);
        scene.set_viewport_size(kOutputSize, kOutputSize);

        const auto &generated = pipeline.generate_shapes(root, abstract_id, view_layers);

        std::vector<const Shape *> shape_ptrs;
        shape_ptrs.reserve(generated.size());
        for (const auto &rs : generated)
            shape_ptrs.push_back(&rs.shape);

        const std::optional<Rect> bbox = Geometry::bbox(shape_ptrs);
        if (!bbox)
        {
            scene.set_scale(1.0);
            scene.set_pan(Point{0, 0});
            return scene;
        }

        const double content_width = static_cast<double>(bbox->ur.x - bbox->ll.x);
        const double content_height = static_cast<double>(bbox->ur.y - bbox->ll.y);
        const double usable_px = kOutputSize - 2 * kPaddingPx;

        double scale = 1.0;
        if (content_width > 0 || content_height > 0)
            scale = usable_px / std::max(content_width, content_height);

        // Renderer's pixel transform maps (dbu - pan) * scale to pixel space,
        // i.e. pan is the dbu point that lands at pixel (0, 0) - not the
        // viewport center (see render.hpp's PixelShape comment). To center
        // the content, pan is offset from the bbox's own lower-left corner
        // by half of the leftover (non-content) space on each axis.
        const int64_t pan_x = bbox->ll.x - static_cast<int64_t>((kOutputSize / scale - content_width) / 2.0);
        const int64_t pan_y = bbox->ll.y - static_cast<int64_t>((kOutputSize / scale - content_height) / 2.0);

        scene.set_scale(scale);
        scene.set_pan(Point{pan_x, pan_y});
        return scene;
    }

    bool write_png(const SkPicture &picture, int width, int height, const std::string &output_path)
    {
        sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));

        // The render pipeline itself draws no background (SkPictureRecorder
        // just records draw calls) - this dark gray is purely a preview
        // convenience, not something Renderer produces.
        surface->getCanvas()->clear(SkColorSetRGB(30, 30, 30));
        surface->getCanvas()->drawPicture(&picture);

        SkPixmap pixmap;
        if (!surface->peekPixels(&pixmap))
        {
            fprintf(stderr, "Failed to access rasterized pixels for '%s'\n", output_path.c_str());
            return false;
        }

        SkFILEWStream stream(output_path.c_str());
        if (!stream.isValid())
        {
            fprintf(stderr, "Failed to open '%s' for writing\n", output_path.c_str());
            return false;
        }

        if (!SkPngEncoder::Encode(&stream, pixmap, SkPngEncoder::Options{}))
        {
            fprintf(stderr, "Failed to encode PNG for '%s'\n", output_path.c_str());
            return false;
        }

        printf("Wrote %dx%d PNG to %s\n", width, height, output_path.c_str());
        return true;
    }
}

// Dev-only visualization tool, not part of the render pipeline itself: reads
// every given LEF file into one shared Root, runs Pipeline + Renderer
// against every Design's Abstract found across all of them (Scene fitted to
// that Abstract's own content bbox, since real macros don't share the
// stress data's hardcoded scene - see fit_scene above), and writes one PNG
// per Design into preview/ - a quick way to sanity-check what real LEF
// files render as (layer colors, bottom-up z-order, Terminal labels)
// without waiting for Flutter texture wiring.
//
// A single shared Root (not one per file) matters because LEF is commonly
// split across a tech file (LAYER definitions, no macros) and one or more
// macro/cell files (MACRO/PIN definitions referencing those layers by
// name) - LEFReader::read_lef already supports this: it reuses an existing
// Technology instead of creating a new one when the Root already has one,
// so passing the tech file first lets later macro files' LAYER references
// resolve. Order matters for that reason - pass the tech file before any
// macro file that depends on it.
//
//   ./render_preview file1.lef [file2.lef ...]
//
// Usage: `cmake --build build --target render_preview && ./build/render_preview <lef files>`
int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <lef file> [lef file ...]\n", argv[0]);
        return 1;
    }

    const std::filesystem::path preview_dir = "preview";
    std::filesystem::create_directories(preview_dir);

    bool all_ok = true;
    Root root;

    for (int i = 1; i < argc; ++i)
    {
        const std::filesystem::path lef_path = argv[i];

        LEFReader reader;
        if (reader.read_lef(lef_path.string(), root, lef_path.stem().string()) != 0)
        {
            fprintf(stderr, "Failed to parse '%s'\n", lef_path.string().c_str());
            all_ok = false;
        }
    }

    const auto technology_ids = root.get_technology_ids();
    if (technology_ids.empty())
    {
        fprintf(stderr, "No technology declared across the given LEF files\n");
        return 1;
    }
    const ViewLayerSet view_layers = ViewLayerSet::build_for_technology(root, technology_ids.front());

    const auto design_ids = root.get_design_ids();
    if (design_ids.empty())
    {
        fprintf(stderr, "No designs declared across the given LEF files\n");
        return 1;
    }

    for (const DesignId design_id : design_ids)
    {
        const DesignData *design = root.get_design(design_id);
        const AbstractId abstract_id = root.get_design_abstract(design_id);

        Pipeline pipeline;
        Renderer renderer;
        Scene scene = fit_scene(root, abstract_id, pipeline, view_layers);

        const auto &shapes = pipeline.run(root, scene, view_layers);
        const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
        const auto &picture = renderer.build_picture(pixel_shapes, scene, view_layers);

        // The design's own library name (derived from whichever file's
        // read_lef call actually declared its MACRO - see the file-level
        // comment above) rather than a fixed input file's stem, since a
        // design here may come from any of the given files.
        const LibraryData *library = root.get_library(design->library);
        const std::string prefix = library ? library->name : "unknown";
        const std::string output_name = prefix + "__" + design->name + ".png";
        const std::filesystem::path output_path = preview_dir / output_name;

        if (!write_png(*picture, scene.viewport_width_px(), scene.viewport_height_px(), output_path.string()))
            all_ok = false;
    }

    return all_ok ? 0 : 1;
}
