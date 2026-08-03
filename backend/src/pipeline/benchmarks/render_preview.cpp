#include "../../render/render.hpp"
#include "../pipeline.hpp"
#include "stress_data.hpp"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include <cstdio>

using namespace le;

// Dev-only visualization tool, not part of the render pipeline itself: runs
// Pipeline + Renderer against the exact same 1M-shape stress data and Scene
// used by pipeline_benchmark.cpp (see stress_data.hpp), rasterizes the
// resulting SkPicture, and writes it as a PNG - a quick way to sanity-check
// what's actually being benchmarked (layer colors, bottom-up z-order,
// Terminal labels) without waiting for Flutter texture wiring.
//
//   ./render_preview [output.png]
//
// Usage: `cmake --build build --target render_preview && ./build/render_preview`
int main(int argc, char **argv)
{
    const char *output_path = argc > 1 ? argv[1] : "render_preview.png";

    const auto &data = stress_data();
    Scene scene = make_scene(data);

    Pipeline pipeline;
    Renderer renderer;
    const auto &shapes = pipeline.run(data.root, scene, data.view_layers);
    const auto &pixel_shapes = renderer.transform_to_pixels(shapes, scene);
    const auto &picture = renderer.build_picture(pixel_shapes, scene, data.view_layers);

    const int width = scene.viewport_width_px();
    const int height = scene.viewport_height_px();
    sk_sp<SkSurface> surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));

    // The render pipeline itself draws no background (SkPictureRecorder
    // just records draw calls) - this dark gray is purely a preview
    // convenience, not something Renderer produces, so sparse content
    // against a mostly-empty 2000x2000 canvas is actually legible instead
    // of a mostly-transparent PNG.
    surface->getCanvas()->clear(SkColorSetRGB(30, 30, 30));
    surface->getCanvas()->drawPicture(picture);

    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap))
    {
        fprintf(stderr, "Failed to access rasterized pixels\n");
        return 1;
    }

    SkFILEWStream stream(output_path);
    if (!stream.isValid())
    {
        fprintf(stderr, "Failed to open '%s' for writing\n", output_path);
        return 1;
    }

    if (!SkPngEncoder::Encode(&stream, pixmap, SkPngEncoder::Options{}))
    {
        fprintf(stderr, "Failed to encode PNG\n");
        return 1;
    }

    printf("Wrote %dx%d PNG to %s\n", width, height, output_path);
    return 0;
}
