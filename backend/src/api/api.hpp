#pragma once
#include <stdint.h>

// The C API surface a Flutter plugin's Dart FFI binds to (see README's
// `api` module entry and "Recommended build order" step 5 - the minimal
// slice: init/destroy, load-file, render_pixel_buffer only, before
// zoom/pan/selection grow into a real events module). Deliberately plain
// C (not C++) in every public declaration - no std:: types, no default
// arguments, no overloads - so this header parses cleanly for Dart's
// ffigen (or any other C FFI generator) and so LeHandle/LePixelBuffer have
// a stable, toolchain-independent ABI between this project's macOS dev
// machine and its Linux deployment target.

#ifdef __cplusplus
extern "C"
{
#endif

    /// @brief Opaque handle to one editor instance: a Root (database),
    /// ViewLayerSet, Scene (view state), Pipeline, and Renderer, all with
    /// the "one instance per Scene-equivalent lifetime, reused across
    /// repeated calls" lifetime Pipeline/Renderer are designed around (see
    /// pipeline.hpp/render.hpp) - a fresh Pipeline/Renderer per call would
    /// defeat their internal caching entirely. Opaque so this header stays
    /// C-compatible; the real struct is defined only in api.cpp.
    typedef struct LeHandle LeHandle;

    /// @brief Raw RGBA8888 pixel buffer, mirroring render::PixelBuffer but
    /// using explicit fixed-width types (not `int`/`size_t`, whose width
    /// isn't guaranteed identical across toolchains) for a stable FFI ABI.
    /// `data` points into memory owned by the LeHandle's Renderer - valid
    /// only until the next le_render_pixel_buffer() call on the same
    /// handle (or le_destroy()), never owned by the caller and never to be
    /// freed by it. Premultiplied alpha, row-major, top-to-bottom; row_bytes
    /// may exceed width * 4 - always index by it, never assume a tight
    /// stride (see render.hpp's PixelBuffer for the full format contract).
    typedef struct LePixelBuffer
    {
        const uint8_t *data;
        int32_t width;
        int32_t height;
        int64_t row_bytes;
    } LePixelBuffer;

    /// @brief Allocate a new, empty editor instance (no LEF loaded, no
    /// Design selected, 0x0 viewport). Never returns null.
    LeHandle *le_create(void);

    /// @brief Destroy an instance created by le_create(). Safe to call
    /// with a null handle (no-op), matching free()'s convention.
    void le_destroy(LeHandle *handle);

    /// @brief Read a LEF file into this handle's shared Root, deriving a
    /// library name from the file's stem. Safe to call multiple times on
    /// the same handle - e.g. a tech file (LAYER definitions, no macros)
    /// followed by one or more macro files that reference those layers by
    /// name, matching LEFReader::read_lef's own existing-Technology reuse
    /// (pass the tech file first when a macro file depends on it - see
    /// render_preview.cpp for the same convention already used there).
    /// Returns 0 on success, matching LEFReader::read_lef's own result
    /// code (nonzero otherwise, including if handle or path is null).
    int le_read_lef(LeHandle *handle, const char *path);

    /// @brief Number of Designs currently loaded across every LEF file
    /// read into this handle so far. 0 if handle is null.
    int32_t le_design_count(LeHandle *handle);

    /// @brief Name of the Design at `index` (0..le_design_count()-1).
    /// Returns null if handle is null or index is out of range. The
    /// returned pointer is owned by the handle's Root - valid until the
    /// handle is destroyed (no Design-removal API exists yet), never
    /// owned by the caller.
    const char *le_design_name(LeHandle *handle, int32_t index);

    /// @brief Select the Design at `index` as the one le_render_pixel_buffer()
    /// renders (its Abstract view). Returns 0 on success, nonzero if
    /// handle is null or index is out of range - the current selection is
    /// left unchanged on failure.
    int le_set_current_design(LeHandle *handle, int32_t index);

    /// @brief Set the viewport pan: the dbu point that lands at pixel
    /// (0, 0) - see render.hpp's PixelShape comment - not the viewport's
    /// center. Mirrors Scene::set_pan directly.
    void le_set_pan(LeHandle *handle, int64_t x, int64_t y);

    /// @brief Set the viewport scale in pixels per dbu. Mirrors
    /// Scene::set_scale directly.
    void le_set_scale(LeHandle *handle, double pixels_per_dbu);

    /// @brief Set the viewport size in pixels - also the size of the
    /// buffer le_render_pixel_buffer() produces. Mirrors
    /// Scene::set_viewport_size directly.
    void le_set_viewport_size(LeHandle *handle, int32_t width_px, int32_t height_px);

    /// @brief Fit the viewport's pan/scale to the currently selected
    /// Design's content bbox: uniform scale (no stretch) so the content
    /// fills the viewport set via le_set_viewport_size() with `padding_px`
    /// of margin on every side, pan centering it. Mirrors
    /// Scene::fit_to_content, using Pipeline::generate_shapes' output for
    /// the bbox (same shapes le_render_pixel_buffer() would draw). A no-op
    /// if handle is null; degrades to scale 1.0 / pan (0, 0) if no Design
    /// is selected or its Abstract has no shapes, rather than crashing.
    void le_fit_scene(LeHandle *handle, int32_t padding_px);

    /// @brief Run the full pipeline+render chain (generate -> filter ->
    /// filter -> transform -> picture -> rasterize) for the currently
    /// selected Design and viewport, returning the resulting pixel
    /// buffer. Each stage is cached internally (see Pipeline/Renderer) -
    /// calling this again with nothing changed since the last call is
    /// close to free; only viewport/selection changes actually recompute.
    /// Returns an all-zero/null LePixelBuffer if handle is null. No
    /// Design selected (le_set_current_design was never called) degrades
    /// gracefully to an empty (but correctly-sized, non-null) buffer
    /// rather than crashing - Root/Pipeline's own lookups already degrade
    /// gracefully for an unset AbstractId (see pipeline.hpp).
    LePixelBuffer le_render_pixel_buffer(LeHandle *handle);

#ifdef __cplusplus
}
#endif
