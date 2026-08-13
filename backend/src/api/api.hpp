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
//
// Thread safety: every function below is safe to call concurrently from
// multiple threads on the *same* LeHandle, except le_destroy() (see its
// own doc comment) - each internally locks a mutex owned by the handle.
// This isn't defensive-but-unneeded caution: Flutter's own external-
// texture API calls le_render_pixel_buffer() from a dedicated raster
// thread once per frame, while ordinary pointer/FFI calls
// (le_set_mouse_position(), le_mouse_down()/up(), le_zoom(), ...) run on
// the platform thread - both reach the same handle. A real crash
// (concurrent, unsynchronized std::map mutation inside Pipeline) shipped
// before this was added.

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

    /// @brief Mirrors the database's LibraryId handle (generated/ids.hpp's
    /// `Id<LibraryTag>`) for the FFI boundary: a stable identity for one
    /// Library, safe to hold onto and pass back into e.g.
    /// le_set_current_design_by_id() later without re-deriving it from a
    /// position in le_library_at()'s enumeration. `index ==
    /// UINT32_MAX` (matching `Id<Tag>::valid()`) marks it invalid - never
    /// construct one by hand, only copy one returned by this API.
    typedef struct LeLibraryId
    {
        uint32_t index;
        uint32_t generation;
    } LeLibraryId;

    /// @brief Mirrors the database's DesignId handle - see LeLibraryId's
    /// comment for the general contract.
    typedef struct LeDesignId
    {
        uint32_t index;
        uint32_t generation;
    } LeDesignId;

    /// @brief Mirrors the database's AbstractId handle - see LeLibraryId's
    /// comment for the general contract.
    typedef struct LeAbstractId
    {
        uint32_t index;
        uint32_t generation;
    } LeAbstractId;

    /// @brief One row of le_library_at(): a Library's identity and name.
    typedef struct LeLibraryInfo
    {
        LeLibraryId id;
        /// Owned by the handle's Root - valid until the handle is
        /// destroyed, never owned by the caller. Null if this row is
        /// invalid (out-of-range index or null handle).
        const char *name;
    } LeLibraryInfo;

    /// @brief One row of le_library_design_at(): a Design's identity
    /// (plus its parent Library's) and name.
    typedef struct LeDesignInfo
    {
        LeLibraryId library_id;
        LeDesignId id;
        /// Invalid (index == UINT32_MAX) if this Design has no Abstract
        /// view yet - no DEF/placement-driven Design exists in this
        /// project yet, so every Design read via le_read_lef() has one,
        /// but the field degrades gracefully rather than assume that.
        LeAbstractId abstract_id;
        /// Owned by the handle's Root - valid until the handle is
        /// destroyed, never owned by the caller. Null if this row is
        /// invalid (out-of-range index or null handle).
        const char *name;
    } LeDesignInfo;

    /// @brief One row of le_layer_at(): a layer-visibility/selectability
    /// widget's row-header - a name plus a swatch color, *not* tied to a
    /// physical Layer existing - BOUNDARY is a row like any other, and any
    /// future non-Technology-derived ("extra") ViewLayer becomes a row the
    /// same way. Visibility/selectability are set by this name directly
    /// (le_set_layer_name_visible()/le_set_layer_name_selectable()), not
    /// by any id here - there's no per-row column list to address (see
    /// le_purpose_count()/le_purpose_at() for the other, row-independent
    /// "columns" axis).
    typedef struct LeLayerRow
    {
        /// Owned by the handle's Root - valid until the handle is
        /// destroyed, never owned by the caller. Null if this row is
        /// invalid (out-of-range index or null handle).
        const char *name;
        /// This row's own outline color, for a swatch next to its name -
        /// not the fill color or FillPattern, which draw_group already
        /// resolves per shape.
        uint8_t color_r;
        uint8_t color_g;
        uint8_t color_b;
    } LeLayerRow;

    /// @brief Result of le_snapped_mouse_position(): the current mouse
    /// position's coordinates, snapped to the minor grid (mirrors
    /// Scene::snapped_mouse_position - see UPDATES.md 5.3) and converted
    /// from dbu to microns via the Root's Technology::database_units_microns
    /// (e.g. 1000 dbu/um -> 3 decimal digits of representable precision;
    /// dividing the already-integral snapped dbu value by this is exact to
    /// the finest resolution the database itself supports - no additional
    /// rounding is meaningful beyond it). `has_position` is 0 (with
    /// x_um/y_um both 0) if no mouse position has been set yet (see
    /// le_set_mouse_position), the handle is null, or no Technology has
    /// been read yet (le_read_lef) to convert with - checked explicitly
    /// rather than a sentinel x_um/y_um value, since any coordinate
    /// including 0 is otherwise legitimate. Mirrors this project's single
    /// shared/global Technology assumption (see ViewLayerSet::
    /// build_for_technology's own caller in le_read_lef).
    typedef struct LeSnappedMousePosition
    {
        double x_um;
        double y_um;
        int32_t has_position;
    } LeSnappedMousePosition;

    /// @brief Allocate a new, empty editor instance (no LEF loaded, no
    /// Design selected, 0x0 viewport). Never returns null.
    LeHandle *le_create(void);

    /// @brief Destroy an instance created by le_create(). Safe to call
    /// with a null handle (no-op), matching free()'s convention. The one
    /// function this header's own thread-safety guarantee doesn't cover -
    /// the handle's mutex is destroyed along with everything else, so it
    /// can't protect this call itself. The caller must ensure no other
    /// thread is still calling into this handle when le_destroy() runs
    /// (the same standard contract as destroying any C++ object with
    /// live references elsewhere - not a new constraint this introduces).
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

    /// @brief Total number of error/warning/info messages produced by
    /// this handle's backend operations so far (currently just
    /// le_read_lef() - file-open/parse errors, parser warnings, parser
    /// info notes, and a success summary, each already formatted with
    /// its own "ERROR "/"WARNING "/"INFO " prefix). Monotonically
    /// increasing - entries are never removed, cleared, or reordered -
    /// so a caller can poll this like le_selection_version() and only
    /// fetch le_message_at() for indices at or past what it last saw,
    /// rather than re-reading everything on every check. 0 if handle is
    /// null.
    int32_t le_message_count(LeHandle *handle);

    /// @brief The message at `index` (0..le_message_count()-1). Returns
    /// null if handle is null or index is out of range. The returned
    /// pointer is owned by the handle - valid until the handle is
    /// destroyed (messages are never removed/reordered, so unlike a
    /// "last error" design this pointer is never invalidated by a later
    /// le_read_lef() call).
    const char *le_message_at(LeHandle *handle, int32_t index);

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

    /// @brief Number of Libraries currently loaded - one per le_read_lef()
    /// call so far (see its own comment: each derives a fresh Library from
    /// its file's stem). 0 if handle is null. The top level of a
    /// Library -> Design -> Abstract browser widget; see
    /// le_library_design_count()/le_library_design_at() for the next level.
    int32_t le_library_count(LeHandle *handle);

    /// @brief The Library at `index` (0..le_library_count()-1). An
    /// all-invalid/null row (id.index == UINT32_MAX, name == null) if
    /// handle is null or index is out of range, rather than crashing.
    LeLibraryInfo le_library_at(LeHandle *handle, int32_t index);

    /// @brief Number of Designs belonging to the Library at
    /// `library_index` (into the same enumeration le_library_at() uses).
    /// 0 if handle is null or library_index is out of range.
    int32_t le_library_design_count(LeHandle *handle, int32_t library_index);

    /// @brief The Design at `design_index` within the Library at
    /// `library_index` (0..le_library_design_count(library_index)-1). An
    /// all-invalid/null row if handle is null or either index is out of
    /// range, rather than crashing.
    LeDesignInfo le_library_design_at(LeHandle *handle, int32_t library_index, int32_t design_index);

    /// @brief Select a Design by its stable LeDesignId (e.g. one read from
    /// le_library_design_at()'s LeDesignInfo::id) as the one
    /// le_render_pixel_buffer() renders, same effect as
    /// le_set_current_design() but addressed by identity instead of a
    /// position in the flat le_design_count() list - the natural fit for a
    /// browser widget's row click, which already has the Design's
    /// LeDesignId on hand and shouldn't need to re-derive a flat index for
    /// it. Returns 0 on success, nonzero if handle is null or design_id
    /// doesn't name a Design currently loaded on this handle - the current
    /// selection is left unchanged on failure.
    int le_set_current_design_by_id(LeHandle *handle, LeDesignId design_id);

    /// @brief Look up the Design named `name` (exact match) on this
    /// handle - lets a caller resolve a name straight to a LeDesignId
    /// (e.g. for le_set_current_design_by_id) without walking the flat
    /// le_library_design_at() enumeration by hand (UPDATES.md item 17's
    /// `open_design <name>`). Returns an invalid id (index == UINT32_MAX)
    /// if handle or name is null, or no Design with that name is loaded
    /// on this handle.
    LeDesignId le_design_by_name(LeHandle *handle, const char *name);

    // --- UPDATES.md item 19.1: get_<type> enumeration (Library/Design/
    // Abstract) ---
    //
    // A unified `get_<type> [<name-expr>] [-of <parent>] [-filter <expr>]`
    // TCL command pattern sits on top of these - see le_get_terminals'
    // own comment for the general contract (name-expression glob
    // matching, filter-expression validation, "invalid parent id means
    // use the default scope" convention) shared by every le_get_* function
    // in this API, not just this section's three.

    /// @brief Look up the Library named `name` (exact match) on this
    /// handle - same contract as le_design_by_name, one level up. Returns
    /// an invalid id (index == UINT32_MAX) if handle or name is null, or
    /// no Library with that name is loaded on this handle.
    LeLibraryId le_library_by_name(LeHandle *handle, const char *name);

    /// @brief The Library at `id`'s own name - a direct field accessor
    /// (same convention as le_terminal_name), for a caller that has an id
    /// (e.g. from le_search_result_library_at) rather than a flat index
    /// (le_library_at's own addressing). Returned pointer is owned by the
    /// handle's Root - valid until the next call that mutates this
    /// handle's Library pool - copy out immediately, don't hold across
    /// another call. Returns nullptr if handle is null or id doesn't name
    /// a Library on this handle.
    const char *le_library_name(LeHandle *handle, LeLibraryId id);

    /// @brief The Design at `id`'s own name - same contract as
    /// le_library_name, one level down. Distinct from le_design_name
    /// above (which is flat-index addressed, like le_library_at/
    /// le_library_design_at's own enumeration) since this plain-C API
    /// can't overload on parameter type.
    const char *le_design_name_by_id(LeHandle *handle, LeDesignId id);

    /// @brief Search every Library on this handle - unlike
    /// le_get_terminals/le_get_designs/le_get_abstracts, Library has no
    /// parent type, so there is no `-of`/default-scope concept here; this
    /// always searches every Library loaded so far. `name_expression`
    /// (glob-matched against Library::name, Tcl `string match` semantics)
    /// and `filter_expression` (see backend/src/database/filter.hpp) are
    /// each optional - pass null or "" to skip that axis. Returns the
    /// match count (0 if handle is null or nothing matched), or -1 if
    /// filter_expression fails to parse or references an unknown
    /// field/hop (see le_message_count/le_message_at for either error).
    /// Read results back via le_search_result_library_at, same "valid
    /// until the next call" convention as this API's other cached-result
    /// accessors.
    int32_t le_get_libraries(LeHandle *handle, const char *name_expression, const char *filter_expression);

    /// @brief The LeLibraryId at `index` (0..le_get_libraries' last
    /// return value - 1) from the most recent le_get_libraries call.
    /// Returns an invalid id (index == UINT32_MAX) if handle is null or
    /// index is out of range.
    LeLibraryId le_search_result_library_at(LeHandle *handle, int32_t index);

    /// @brief Search Designs. `of_library` scopes to one Library's own
    /// Designs (see le_library_by_name) - pass an invalid id (e.g. a
    /// default-constructed LeLibraryId) to use the default scope instead:
    /// the current view's own Design's Library if a Design is currently
    /// selected (le_set_current_design/le_set_current_design_by_id), else
    /// every Design loaded on this handle. `name_expression`/
    /// `filter_expression` - see le_get_libraries' own comment. Returns
    /// the match count, or -1 on a filter parse/validation error.
    int32_t le_get_designs(LeHandle *handle, LeLibraryId of_library, const char *name_expression, const char *filter_expression);

    /// @brief The LeDesignId at `index` from the most recent le_get_designs
    /// call - see le_search_result_library_at's own contract.
    LeDesignId le_search_result_design_at(LeHandle *handle, int32_t index);

    /// @brief Search Abstracts. `of_design` scopes to one Design's own
    /// Abstract (today always exactly one, per Design - see
    /// le_design_by_name) - pass an invalid id to use the default scope:
    /// the currently selected Design's Abstract if one is selected, else
    /// none. Abstract has no name field (LEF has no concept of naming an
    /// Abstract independently of its Design), so there is no
    /// `name_expression` parameter here, only `filter_expression` - see
    /// le_get_libraries' own comment. Returns the match count, or -1 on a
    /// filter parse/validation error.
    int32_t le_get_abstracts(LeHandle *handle, LeDesignId of_design, const char *filter_expression);

    /// @brief The LeAbstractId at `index` from the most recent
    /// le_get_abstracts call - see le_search_result_library_at's own
    /// contract.
    LeAbstractId le_search_result_abstract_at(LeHandle *handle, int32_t index);

    /// @brief Number of layer-widget rows currently available - mirrors
    /// ViewLayerSet::rows() directly (see LeLayerRow's own comment: this
    /// includes BOUNDARY and any future non-Technology-derived "extra"
    /// row, not just physical Layers), so this doesn't care whether a
    /// Technology has even been declared yet - it's simply however many
    /// rows the handle's current ViewLayerSet happens to have (0 if
    /// handle is null or none has been built yet, e.g. before the first
    /// le_read_lef() call). See le_layer_at() for each row's contents.
    int32_t le_layer_count(LeHandle *handle);

    /// @brief The row at `row_index` (0..le_layer_count()-1). An
    /// all-invalid/null row (name == null) if handle is null or row_index
    /// is out of range, rather than crashing.
    LeLayerRow le_layer_at(LeHandle *handle, int32_t row_index);

    /// @brief Number of distinct purposes across the handle's current
    /// ViewLayerSet - mirrors ViewLayerSet::purposes() directly. The
    /// "columns" axis of a layer visibility/selectability widget,
    /// independent of any row/layer (see le_purpose_at()). 0 if handle is
    /// null or no ViewLayerSet has been built yet.
    int32_t le_purpose_count(LeHandle *handle);

    /// @brief The purpose at `index` (0..le_purpose_count()-1) - mirrors
    /// le::ViewLayerPurpose's declaration order: 0 = TERMINAL,
    /// 1 = OBSTRUCTION, 2 = BOUNDARY. Returns -1 if handle is null or
    /// index is out of range, rather than crashing.
    int32_t le_purpose_at(LeHandle *handle, int32_t index);

    /// @brief Current visibility of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` (case-sensitive exact match) - i.e. a whole row
    /// (every purpose-column of it) together, not one column - a
    /// coarser-grained "layer visibility widget" model than one toggle per
    /// grid cell: see le_is_purpose_visible() for the other axis, and
    /// Scene::is_view_layer_visible for how a specific column's effective
    /// visibility combines both. Visible by default until toggled. Returns
    /// nonzero (visible) if handle or layer_name is null, matching Scene's
    /// own "unknown name defaults to visible" default.
    int32_t le_is_layer_name_visible(LeHandle *handle, const char *layer_name);

    /// @brief Set the visibility of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` - e.g. a layer-visibility widget's row-header
    /// checkbox. Mirrors Scene::set_layer_name_visible directly (affects
    /// rendering - see Pipeline::filter_by_layer_visibility). A no-op if
    /// handle or layer_name is null.
    void le_set_layer_name_visible(LeHandle *handle, const char *layer_name, int32_t visible);

    /// @brief Current visibility of every ViewLayer whose purpose is
    /// `purpose` (mirrors le::ViewLayerPurpose's declaration order: 0 =
    /// TERMINAL, 1 = OBSTRUCTION, 2 = BOUNDARY), across every layer - i.e.
    /// a whole column, not one row. Visible by default until toggled.
    /// Returns nonzero (visible) if handle is null.
    int32_t le_is_purpose_visible(LeHandle *handle, int32_t purpose);

    /// @brief Set the visibility of every ViewLayer whose purpose is
    /// `purpose`, across every layer - e.g. a layer-visibility widget's
    /// column-header checkbox. Mirrors Scene::set_purpose_visible directly
    /// (affects rendering). A no-op if handle is null.
    void le_set_purpose_visible(LeHandle *handle, int32_t purpose, int32_t visible);

    /// @brief Current selectability of every ViewLayer whose LeLayerRow::name
    /// is `layer_name` - see le_is_layer_name_visible()'s comment for the
    /// general row/column model this mirrors. Selectable by default until
    /// toggled. Purely an interaction-layer concern (no hit-testing/
    /// click-to-select API exists yet to consult it) - doesn't affect
    /// rendering. Returns nonzero (selectable) if handle or layer_name is
    /// null.
    int32_t le_is_layer_name_selectable(LeHandle *handle, const char *layer_name);

    /// @brief Set the selectability of every ViewLayer whose LeLayerRow::name
    /// is `layer_name`. Mirrors Scene::set_layer_name_selectable directly.
    /// A no-op if handle or layer_name is null.
    void le_set_layer_name_selectable(LeHandle *handle, const char *layer_name, int32_t selectable);

    /// @brief Current selectability of every ViewLayer whose purpose is
    /// `purpose`, across every layer. Selectable by default until toggled.
    /// Returns nonzero (selectable) if handle is null.
    int32_t le_is_purpose_selectable(LeHandle *handle, int32_t purpose);

    /// @brief Set the selectability of every ViewLayer whose purpose is
    /// `purpose`, across every layer. Mirrors Scene::set_purpose_selectable
    /// directly. A no-op if handle is null.
    void le_set_purpose_selectable(LeHandle *handle, int32_t purpose, int32_t selectable);

    /// @brief Zoom the viewport, keeping the dbu point under screen pixel
    /// (x, y) fixed on screen. `factor` is a signed fractional step applied
    /// to the current scale (new_scale = scale * (1 + factor)) - positive
    /// zooms in, negative zooms out (e.g. 0.1 zooms in 10%, -0.1 zooms out
    /// 10%); a factor <= -1.0 (which would make new_scale non-positive) is
    /// ignored, same guard as Scene::set_scale. `x`/`y` are in the same
    /// pixel space as le_render_pixel_buffer()'s output image - top-left
    /// origin, y increasing downward (see api.hpp's LePixelBuffer) - not
    /// Renderer's own pre-Y-flip pixel space, since this is meant to be fed
    /// straight from a pointer/tap event on the rendered image. A no-op if
    /// handle is null. Backend now owns pan/scale entirely - there is no
    /// direct pan/scale setter; use le_fit_scene() to reset to a known view.
    void le_zoom(LeHandle *handle, double factor, int32_t x, int32_t y);

    /// @brief Pan the viewport by a fraction of its own size, in dbu-space
    /// directions (positive x_factor/y_factor move the view toward
    /// increasing dbu x/y - the same "up is positive" convention as the
    /// database itself, not screen space): pan += (x_factor, y_factor) *
    /// viewport_size / scale. E.g. x_factor = 1.0 pans right by exactly one
    /// full viewport width of content. A no-op if handle is null.
    void le_pan(LeHandle *handle, double x_factor, double y_factor);

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

    /// @brief Spacing (dbu) between minor grid dots, drawn behind the
    /// design by le_render_pixel_buffer() - see Renderer::draw_grid.
    /// Mirrors Scene::minor_grid_spacing directly. Defaults to 5 (dbu),
    /// matching a 5nm minor grid under the common "1 dbu = 1nm" Technology
    /// convention. Returns 0 if handle is null.
    int64_t le_minor_grid_spacing(LeHandle *handle);

    /// @brief Set the minor grid dot spacing (dbu). Mirrors
    /// Scene::set_minor_grid_spacing directly (affects rendering) - values
    /// <= 0 are ignored, same guard as le_set_scale. A no-op if handle is
    /// null.
    void le_set_minor_grid_spacing(LeHandle *handle, int64_t dbu);

    /// @brief Spacing (dbu) between major grid dots (drawn bolder than
    /// minor ones - see Renderer::draw_grid). Mirrors
    /// Scene::major_grid_spacing directly. Defaults to 50 (dbu), matching
    /// a 50nm major grid under the common "1 dbu = 1nm" Technology
    /// convention. Returns 0 if handle is null.
    int64_t le_major_grid_spacing(LeHandle *handle);

    /// @brief Set the major grid dot spacing (dbu). Mirrors
    /// Scene::set_major_grid_spacing directly (affects rendering) - values
    /// <= 0 are ignored. A no-op if handle is null.
    void le_set_major_grid_spacing(LeHandle *handle, int64_t dbu);

    /// @brief Set the current mouse position, in the same pixel space as
    /// le_render_pixel_buffer()'s output image (top-left origin, y
    /// increasing downward) and le_zoom()'s x/y - meant to be fed straight
    /// from a pointer-move event. Drives the grid-snap indicator box drawn
    /// by le_render_pixel_buffer() (see Renderer::draw_cursor), and
    /// updates which selectable shape (if any) is hovered (UPDATES.md
    /// 7.1 item 1 - Renderer::draw_hover_outline draws its yellow
    /// outline) via a hit-test against the shapes currently on screen. A
    /// no-op if handle is null. Never invalidates the (potentially
    /// design-sized) rasterized design cache - only the small overlay
    /// picture, see Renderer::compose_with_overlays - but unlike before
    /// hover was added, this is no longer O(1): the hit-test is bounded
    /// by the number of shapes currently visible (already viewport-culled
    /// by Pipeline), not the whole design. See BENCHMARKS.md for measured
    /// cost on a 1M-shape design.
    void le_set_mouse_position(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Clear the current mouse position (e.g. on a pointer-leave
    /// event) so the grid-snap indicator box and any hover outline stop
    /// showing at/for the last known position. A no-op if handle is null.
    void le_clear_mouse_position(LeHandle *handle);

    /// @brief The current mouse position's coordinates in microns, snapped
    /// to the minor grid (UPDATES.md 5.3) - the same point the grid-snap
    /// indicator box drawn by le_render_pixel_buffer() is centered on (see
    /// Renderer::draw_cursor), for a Flutter UI to display as coordinate
    /// text. See LeSnappedMousePosition's own comment for the dbu-to-micron
    /// conversion and its degrade-gracefully cases (null handle, no mouse
    /// position set, no Technology read yet).
    LeSnappedMousePosition le_snapped_mouse_position(LeHandle *handle);

    /// @brief Named logical keys this API tracks the held/released state
    /// of via le_key_down()/le_key_up() - stable across platforms, so
    /// this API's meaning doesn't depend on OS/Flutter scan codes; the
    /// frontend maps its own key values to these before calling in.
    /// Extend as future commands need to know about more keys (UPDATES.md
    /// 7's other interaction modes will need their own shortcuts).
    ///
    /// LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_* are canvas-navigation
    /// commands, not modifiers - le_key_down() triggers the
    /// corresponding action immediately (see its own doc comment) rather
    /// than only recording held state. Frontends map a single physical
    /// key to each regardless of shift (e.g. both "z" and "Z" - the same
    /// LogicalKeyboardKey.keyZ in Flutter - map to LE_KEY_ZOOM); shift's
    /// own held state (LE_KEY_SHIFT, tracked exactly like any other key)
    /// decides zoom in vs. out, the same "backend reads modifier state
    /// rather than the frontend pre-deciding" split as le_mouse_up's
    /// shift-click/shift-drag.
    enum LeKeyCode
    {
        LE_KEY_SHIFT = 1,
        LE_KEY_ZOOM = 2,
        LE_KEY_FIT = 3,
        LE_KEY_PAN_LEFT = 4,
        LE_KEY_PAN_RIGHT = 5,
        LE_KEY_PAN_UP = 6,
        LE_KEY_PAN_DOWN = 7,
        /// A pure modifier, tracked exactly like LE_KEY_SHIFT - held
        /// state only, no immediate action of its own. Read by
        /// LE_KEY_SELECT_ALL (UPDATES.md 9.1), LE_KEY_DESELECT_ALL
        /// (UPDATES.md 9.5), LE_KEY_FIT (UPDATES.md 9.6), and
        /// LE_KEY_1..LE_KEY_9 (UPDATES.md 9.7).
        LE_KEY_CTRL = 8,
        /// Select-all (UPDATES.md 9.1) - an "action" code like
        /// LE_KEY_ZOOM/LE_KEY_FIT/LE_KEY_PAN_*, but only actually does
        /// anything while LE_KEY_CTRL is currently held (see le_key_down's
        /// own doc comment) - a bare "a" press with Ctrl not held is a
        /// deliberate no-op, not "select nothing."
        LE_KEY_SELECT_ALL = 9,
        /// Digit keys 1-9 toggle a ROUTING layer's visibility (UPDATES.md
        /// 9.4/9.7) - which one depends on LE_KEY_CTRL's held state (the
        /// same "backend reads modifier state" split LE_KEY_ZOOM/
        /// LE_KEY_FIT already use): with Ctrl not held, LE_KEY_1 is the
        /// first ROUTING layer in the current Technology's own
        /// declaration order, LE_KEY_2 the second, etc. through
        /// LE_KEY_9's ninth; with Ctrl held, the same keys address the
        /// *eleventh* through *nineteenth* ROUTING layer instead (see
        /// LE_KEY_0 for the tenth) - a no-op if there's no ROUTING layer
        /// at the addressed position. See le_key_down's own doc comment
        /// for the VIA-pairing behavior this triggers that a direct
        /// le_set_layer_name_visible() call deliberately does not.
        LE_KEY_1 = 10,
        LE_KEY_2 = 11,
        LE_KEY_3 = 12,
        LE_KEY_4 = 13,
        LE_KEY_5 = 14,
        LE_KEY_6 = 15,
        LE_KEY_7 = 16,
        LE_KEY_8 = 17,
        LE_KEY_9 = 18,
        /// Deselect-all (UPDATES.md 9.5) - same "action code, gated on
        /// LE_KEY_CTRL" shape as LE_KEY_SELECT_ALL, but simply clears the
        /// current selection rather than building a new one; a bare "d"
        /// press with Ctrl not held is a deliberate no-op, same reasoning
        /// as LE_KEY_SELECT_ALL's own comment.
        LE_KEY_DESELECT_ALL = 19,
        /// Toggles the tenth ROUTING layer's visibility (UPDATES.md 9.7) -
        /// not Ctrl-gated, unlike LE_KEY_1..LE_KEY_9's own Ctrl-held
        /// branch, since "0" has no bare-digit slot left below it to
        /// double up on (LE_KEY_1..LE_KEY_9 already cover the first
        /// nine). Same VIA-pairing behavior as LE_KEY_1..LE_KEY_9.
        LE_KEY_0 = 20,
    };

    /// @brief Mark `key_code` (an LeKeyCode value) as currently held,
    /// e.g. on a key-down *or* key-repeat event - queried internally by
    /// commands that care (e.g. le_mouse_up's shift-click/shift-drag
    /// behavior). For the canvas-navigation codes (LE_KEY_ZOOM/
    /// LE_KEY_FIT/LE_KEY_PAN_*), also triggers that action immediately,
    /// once per call - so a held key that keeps re-delivering key-repeat
    /// events (e.g. an arrow key auto-repeating) keeps panning/zooming
    /// once per repeat, matching a real keyboard's own repeat behavior,
    /// not a one-shot trigger on first press only:
    ///
    /// - LE_KEY_ZOOM: le_zoom() by a fixed factor, anchored at the
    ///   current mouse position (Scene::mouse_x_px/mouse_y_px - i.e.
    ///   wherever le_set_mouse_position was last called for); zooms in,
    ///   or out if LE_KEY_SHIFT is currently held (le_is_key_held).
    /// - LE_KEY_FIT: le_fit_scene() with a fixed padding - or, if
    ///   LE_KEY_CTRL is currently held (UPDATES.md 9.6, "Ctrl-F fit
    ///   selected"), fits the viewport to the current selection's own
    ///   combined bbox instead (same fixed padding; piece-scoped for any
    ///   selection entry with a piece set, same scoping
    ///   build_selected_object_properties's own bbox_um property uses -
    ///   see api.cpp), leaving the view unchanged if nothing is selected
    ///   rather than falling back to the whole-content fit.
    /// - LE_KEY_PAN_LEFT/RIGHT/UP/DOWN: le_pan() by a fixed
    ///   viewport-fraction step in the corresponding direction.
    /// - LE_KEY_SELECT_ALL (UPDATES.md 9.1): only while LE_KEY_CTRL is
    ///   currently held - clears the current selection, then selects
    ///   every currently selectable shape in the current Abstract
    ///   regardless of viewport (not just what's on screen), up to a
    ///   fixed cap of 10,000 objects. If the design has more selectable
    ///   shapes than that, the selection stops at the cap and a
    ///   "WARNING: Selection capped..." entry is appended to the
    ///   le_message_count()/le_message_at() queue (UPDATES.md item 3) -
    ///   there's no separate "was it capped" return value, this is the
    ///   same mechanism any other backend-originated message uses.
    /// - LE_KEY_1..LE_KEY_9 (UPDATES.md 9.4/9.7): toggles a ROUTING
    ///   layer's visibility - the 1st..9th if LE_KEY_CTRL is not
    ///   currently held, the 11th..19th if it is (a no-op if there's no
    ///   ROUTING layer at that position) - then, either way, - only via
    ///   this keyboard path, never from a direct
    ///   le_set_layer_name_visible() call - re-checks every pair of
    ///   adjacent ROUTING layers: if both are now visible, every CUT
    ///   layer between them (LEF has no distinct "VIA" layer type - vias
    ///   are TYPE CUT layers - see this header's own LeKeyCode comment)
    ///   becomes visible too; if not, those CUT layers become invisible.
    /// - LE_KEY_0 (UPDATES.md 9.7): toggles the 10th ROUTING layer's
    ///   visibility, unconditional on LE_KEY_CTRL - same VIA-pairing
    ///   re-check as LE_KEY_1..LE_KEY_9 above.
    /// - LE_KEY_DESELECT_ALL (UPDATES.md 9.5): only while LE_KEY_CTRL is
    ///   currently held - clears the current selection. A no-op (not an
    ///   error) if the selection was already empty.
    ///
    /// A no-op if handle is null.
    void le_key_down(LeHandle *handle, int32_t key_code);

    /// @brief Mark `key_code` as no longer held, e.g. on a key-up event.
    /// A no-op if handle is null.
    void le_key_up(LeHandle *handle, int32_t key_code);

    /// @brief True (nonzero) if `key_code` is currently held (see
    /// le_key_down). Returns 0 if handle is null.
    int32_t le_is_key_held(LeHandle *handle, int32_t key_code);

    /// @brief Clear every currently-held key at once - call when the
    /// widget/window receiving key events loses focus (e.g. a Flutter
    /// `Focus` widget's `onFocusChange(false)`). A key's matching
    /// key-up event is not guaranteed to still reach a widget that no
    /// longer has focus by the time the physical key is released, so
    /// without calling this on a focus loss, a modifier (e.g. shift)
    /// held at that moment stays "held" from this API's point of view
    /// indefinitely - silently changing the behavior of every later
    /// gesture that consults it (le_mouse_up's shift-click/shift-drag
    /// decision) until that same key happens to be pressed and released
    /// again while focused. A no-op if handle is null.
    void le_clear_all_keys(LeHandle *handle);

    /// @brief Begin a mouse gesture at the given pixel position (top-left
    /// origin, y down - same convention as le_set_mouse_position/le_zoom),
    /// e.g. on a pointer-down event. Records the gesture's anchor point;
    /// le_mouse_up() later decides whether the gesture was a click or a
    /// rubber-band drag-select by comparing the down/up pixel distance
    /// against a small threshold (UPDATES.md 7.1 items 2-6). A no-op if
    /// handle is null.
    void le_mouse_down(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Like le_mouse_down(), but begins a rectangle-*zoom* gesture
    /// instead of a drag-*select* one (UPDATES.md 9.3 - e.g. on a
    /// right-button pointer-down event) - le_mouse_up() is still the one
    /// call that ends either kind, deciding which behavior to run from
    /// which of le_mouse_down()/le_zoom_drag_down() started it (see its
    /// own doc comment). A no-op if handle is null.
    void le_zoom_drag_down(LeHandle *handle, int32_t x, int32_t y);

    /// @brief End a mouse gesture at the given pixel position, e.g. on a
    /// pointer-up event - the single shared endpoint for both
    /// le_mouse_down()'s drag-select gesture and
    /// le_zoom_drag_down()'s drag-zoom gesture (UPDATES.md 9.3); which
    /// one runs depends entirely on which of those two started the
    /// in-progress gesture, not on which mouse button this call itself
    /// corresponds to (a real up-event's own "which button" state isn't
    /// reliably available at release time on every platform, so the
    /// frontend doesn't need to track or pass it here - only the down
    /// side needs to pick correctly). A no-op if handle is null or there
    /// was no matching le_mouse_down()/le_zoom_drag_down() call first (a
    /// stray/duplicate mouse-up).
    ///
    /// **Started by le_mouse_down()** - two outcomes, both subject to
    /// Pipeline::hit_test_point/hit_test_rect's own topmost-layer-first
    /// (click) / all-layers (drag) and layer-selectability rules, and
    /// both consulting LE_KEY_SHIFT's current held state (see
    /// le_key_down) rather than taking it as a parameter here:
    ///
    /// - **Click** (down/up pixel distance below a small threshold): hit-
    ///   tests the single point. Without shift held, replaces the
    ///   current selection with the hit shape, or clears it if nothing
    ///   was hit; with shift held, adds the hit to the current selection
    ///   (a no-op if nothing was hit).
    /// - **Drag-select** (distance above the threshold): hit-tests every
    ///   selectable shape, on every layer, fully enclosed by the
    ///   rectangle between the down and up points. Without shift held,
    ///   replaces the current selection with the results; with shift
    ///   held, adds them to it.
    ///
    /// **Started by le_zoom_drag_down()** (UPDATES.md 9.3): a click-sized
    /// release (same threshold as above) is a no-op - fitting to a
    /// near-zero-size rectangle would produce an absurd scale; otherwise
    /// fits the viewport to the rectangle between the down and up points
    /// (Scene::fit_to_content, no padding), the same fitting math
    /// le_fit_scene() uses for a Design's own content bbox. Selection is
    /// untouched either way - this gesture is purely navigational.
    ///
    /// Either way, ends the in-progress drag gesture (Scene::end_drag) -
    /// see le_mouse_down's own doc comment for the live rubber-band
    /// rectangle this also stops showing.
    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y);

    /// @brief Instructional text describing which mouse gestures and
    /// keyboard modifiers are currently available, for display in the
    /// GUI's status bar below the texture (UPDATES.md item 7.3) - e.g.
    /// "Left click to select. Shift for multi-select. Left click and
    /// drag for rectangle multi-select." for the current (and, for now,
    /// only) interaction mode, Select - see UPDATES.md item 7's own
    /// intro for the other modes this will eventually distinguish
    /// between. Unlike every other `const char*`-returning function in
    /// this header, the returned pointer refers to static, process-
    /// lifetime storage - not owned by `handle`, never invalidated, safe
    /// to hold indefinitely. Null only if `handle` is null.
    const char *le_tooltip_message(LeHandle *handle);

    /// @brief Number of currently selected objects (Scene::selection()).
    /// Indexes the `selection_index` parameter of le_selected_object_kind()/
    /// le_selected_object_property_count()/le_selected_object_property_at()
    /// below - 0..le_selection_count()-1, in Scene::selection()'s own
    /// (insertion) order. Returns 0 if handle is null.
    int32_t le_selection_count(LeHandle *handle);

    /// @brief Monotonic counter (Scene::selection_version()) bumped on
    /// every actual selection change (a select()/deselect()/
    /// clear_selection() call that isn't a no-op) - a cheap way for a
    /// caller to tell whether anything selection-related has changed
    /// since it last checked, without re-fetching le_selection_count()/
    /// le_selected_object_kind()/le_selected_object_properties() for
    /// every selected object on every call (e.g. every mouse-move event -
    /// a real, measured cost that scales with selection size otherwise,
    /// see BENCHMARKS.md). Returns 0 if handle is null - a real handle's
    /// version is never observably 0 forever (any interaction eventually
    /// changes it), so a caller comparing against a sentinel it
    /// initialized to a negative value on its own side won't be confused
    /// by a null-handle 0 looking like "unchanged".
    int64_t le_selection_version(LeHandle *handle);

    /// @brief Which database class the selected object at `selection_index`
    /// is (Scene::SelectionRef's variant alternative) - lets a caller
    /// decide how to label/group a row before even looking at its
    /// properties. Returns -1 if handle is null or selection_index is out
    /// of range (see le_selection_count).
    typedef enum LeSelectionKind
    {
        LE_SELECTION_KIND_TERMINAL = 0,
        LE_SELECTION_KIND_OBSTRUCTION = 1,
    } LeSelectionKind;
    int32_t le_selected_object_kind(LeHandle *handle, int32_t selection_index);

    /// @brief Which field of LeProperty is meaningful for a given row -
    /// see LeProperty's own comment.
    typedef enum LePropertyType
    {
        LE_PROPERTY_TYPE_STRING = 0,
        LE_PROPERTY_TYPE_INT = 1,
        LE_PROPERTY_TYPE_DOUBLE = 2,
    } LePropertyType;

    /// @brief One name/value row of a selected object's property table
    /// (UPDATES.md 7.2) - a caller reads `type` to know which of
    /// string_value/int_value/double_value to use; the other two are
    /// unspecified. `name`/`string_value` point into memory owned by the
    /// LeHandle - valid only until the next le_selected_object_property_at()
    /// or le_selected_object_property_count() call *for the same
    /// selection_index* (a different selection_index, or a selection
    /// change, may invalidate them sooner) - same "valid until the next
    /// call" convention as LePixelBuffer, never owned by the caller and
    /// never to be freed by it. `name` is null (and every other field 0/
    /// unspecified) if this row is invalid (out-of-range selection_index/
    /// property_index or null handle).
    typedef struct LeProperty
    {
        const char *name;
        int32_t type; // LePropertyType
        const char *string_value;
        int64_t int_value;
        double double_value;
    } LeProperty;

    /// @brief Number of property rows the selected object at
    /// `selection_index` has (see LeProperty) - indexes
    /// le_selected_object_property_at()'s own `property_index` parameter,
    /// 0..this-1. Varies by LeSelectionKind and by whether the object has
    /// any shapes yet (an empty Terminal/Obstruction has no bounding box
    /// to report - see le_selected_object_property_at). Returns 0 if
    /// handle is null or selection_index is out of range.
    int32_t le_selected_object_property_count(LeHandle *handle, int32_t selection_index);

    /// @brief The property row at `property_index`
    /// (0..le_selected_object_property_count(selection_index)-1) for the
    /// selected object at `selection_index`. Per UPDATES.md 7.2: simple
    /// fields (name, direction, counts of list fields rather than the
    /// lists themselves) are sent as-is; any coordinate is always
    /// converted from dbu to microns first (via the same single shared/
    /// global Technology::database_units_microns as
    /// le_snapped_mouse_position - omitted if no Technology has been read
    /// yet). Every plain/enum field on the underlying database class
    /// itself (e.g. Terminal's "name"/"direction") comes from cmg's
    /// schema-generated `le::to_properties()` (see schema.py's Field
    /// reflection helpers and generated/property.hpp) - new plain fields
    /// added to schema.py appear here automatically, no api.cpp change
    /// needed. Only what's genuinely derived (not a stored field) is
    /// hand-appended: a schema-tracked list field's own size, named
    /// "<field>_count" (e.g. Obstruction's "shapes" list ->
    /// "shapes_count"), plus a child list that isn't a struct field at
    /// all in INDEXED_POOLS style (Terminal's "port_count", from Root's
    /// own port index - "ports" itself never appears), plus "bbox_um"
    /// (string, "ll_x ll_y ur_x ur_y" space-separated, mirroring how a
    /// LEF RECT statement itself lists four coordinates, since a bbox is
    /// computed from nested geometry, never a stored field). Each
    /// coordinate is formatted with trailing zeros trimmed in whole
    /// groups of three - never a partial group - down to whichever
    /// group first has a non-zero digit (or entirely, dropping the
    /// decimal point, if every digit is zero): 1.0 -> "1", 0.34 ->
    /// "0.340" (the next group, "340", isn't all zero, so it stays),
    /// 0.123456 unchanged (no trimmable trailing zero group at all). If
    /// the
    /// selection came from a click that hit one specific rect/polygon/
    /// path (as opposed to a drag-select, which selects whole objects -
    /// see le_mouse_up), "bbox_um" covers just that piece, not the whole
    /// object's union, and a "layer_name" (string) row is added for it -
    /// useful since a Terminal can have ports on different layers.
    /// Otherwise (drag-select, or no Technology available) "bbox_um"
    /// covers the whole object and there's no "layer_name" row. Current
    /// rows:
    ///
    /// - LE_SELECTION_KIND_TERMINAL: "name" (string), "direction" (string,
    ///   e.g. "INPUT"), "port_count" (int), then "bbox_um" (string) and,
    ///   if piece-scoped, "layer_name" (string), if the Terminal has any
    ///   shapes and a Technology is available.
    /// - LE_SELECTION_KIND_OBSTRUCTION: "shapes_count" (int), then the
    ///   same "bbox_um"/"layer_name" rows under the same conditions.
    ///
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null or either index is out of range.
    LeProperty le_selected_object_property_at(LeHandle *handle, int32_t selection_index, int32_t property_index);

    /// @brief Run the full pipeline+render chain (generate -> filter ->
    /// filter -> transform -> picture -> rasterize) for the currently
    /// selected Design and viewport, returning the resulting pixel
    /// buffer - including the grid-snap indicator box at the current
    /// mouse position, if one has been set (see le_set_mouse_position).
    /// Each stage is cached internally (see Pipeline/Renderer) - calling
    /// this again with nothing changed since the last call is close to
    /// free; only viewport/selection/mouse-position changes actually
    /// recompute, and a mouse-position-only change is itself cheap (see
    /// Renderer::compose_with_overlays), not proportional to design size.
    /// Returns an all-zero/null LePixelBuffer if handle is null. No
    /// Design selected (le_set_current_design was never called) degrades
    /// gracefully to an empty (but correctly-sized, non-null) buffer
    /// rather than crashing - Root/Pipeline's own lookups already degrade
    /// gracefully for an unset AbstractId (see pipeline.hpp).
    LePixelBuffer le_render_pixel_buffer(LeHandle *handle);

    // --- CRUD + filter-search (UPDATES.md item 15 / TCL_EXPLORATION.md
    // Phase 4) - Terminal only so far; TerminalPort/Obstruction/Abstract-
    // boundary follow the same shape in a later pass. Layered on top of
    // Root's Phase 1/2 primitives (create_x/delete_x/set_x_<field>,
    // get_field()/match_hop()/search_x, backend/src/database/filter.hpp's
    // parser+evaluator) - see TCL_EXPLORATION.md for the full design.
    // Every id here is addressed directly, not through the current GUI
    // selection (le_selected_object_property_at's own LeSelectionKind
    // path stays selection-scoped, unchanged) - the natural fit for a
    // TCL caller that isn't driving the GUI at all. ---

    /// @brief Mirrors the database's TerminalId handle - see LeLibraryId's
    /// comment for the general contract.
    typedef struct LeTerminalId
    {
        uint32_t index;
        uint32_t generation;
    } LeTerminalId;

    /// @brief Mirrors the database's TerminalPortId handle - see
    /// LeLibraryId's comment for the general contract.
    typedef struct LeTerminalPortId
    {
        uint32_t index;
        uint32_t generation;
    } LeTerminalPortId;

    /// @brief Mirrors the database's ObstructionId handle - see
    /// LeLibraryId's comment for the general contract.
    typedef struct LeObstructionId
    {
        uint32_t index;
        uint32_t generation;
    } LeObstructionId;

    /// @brief Mirrors le::SignalDirection (generated/signal_direction.hpp)
    /// field-for-field - kept as an explicit enum (not a bare int) so a
    /// future reordering of either fails to compile instead of silently
    /// mismatching, same reasoning as LePropertyType/LeSelectionKind.
    typedef enum LeSignalDirection
    {
        LE_SIGNAL_DIRECTION_INPUT = 0,
        LE_SIGNAL_DIRECTION_OUTPUT = 1,
        LE_SIGNAL_DIRECTION_INOUT = 2,
        LE_SIGNAL_DIRECTION_NONE = 3,
        LE_SIGNAL_DIRECTION_OUTPUT_TRISTATE = 4,
        LE_SIGNAL_DIRECTION_FEEDTHRU = 5,
    } LeSignalDirection;

    /// @brief Create a Terminal (a.k.a. pin) on the Abstract at
    /// `abstract_id` (UPDATES.md item 15's `create_terminal -name IN0
    /// -direction IN`). Only name/direction are settable here - every
    /// other Terminal field (LEF's optional use/shape/antenna/etc.
    /// fields) defaults to its zero value; extend this function's
    /// parameter list if a real caller needs to set one at creation time
    /// rather than via a later le_set_terminal_* call. `name` must be
    /// unique among Terminals already on `abstract_id` (a TCL script
    /// addresses a Terminal by name - UPDATES.md's Terminal-friendly-id
    /// item - so a collision would be genuinely ambiguous, not just
    /// unusual; enforced by a linear scan over
    /// Root::get_abstract_terminals, not a cmg index=True lookup, since
    /// real LEF libraries legitimately reuse pin names like VDD/IN0
    /// across different Abstracts and index=True's generated index is
    /// flat/global, not per-Abstract). Returns an invalid LeTerminalId
    /// (index == UINT32_MAX) if handle or name is null, abstract_id
    /// doesn't name an Abstract on this handle, or name collides with an
    /// existing Terminal on it (pushes an ERROR message the same way a
    /// filter-expression parse error does - see le_message_count/
    /// le_message_at).
    LeTerminalId le_create_terminal(LeHandle *handle, LeAbstractId abstract_id, const char *name, int32_t direction);

    /// @brief Find the Terminal named `name` (exact match) within the
    /// currently selected Design's Abstract (see le_set_current_design/
    /// le_set_current_design_by_id - same current-view scoping
    /// le_get_terminals already uses). A linear scan over
    /// Root::get_abstract_terminals, not a cmg index=True lookup - see
    /// le_create_terminal's own comment for why. Returns an invalid
    /// LeTerminalId (index == UINT32_MAX) if handle or name is null, no
    /// Design is currently selected, or no Terminal in the current
    /// Abstract has that name.
    LeTerminalId le_terminal_by_name(LeHandle *handle, const char *name);

    /// @brief The Terminal at `id`'s own name - a direct field accessor
    /// (unlike le_terminal_property_at's stringly-typed property table),
    /// for a caller that just wants the name itself. Returned pointer is
    /// owned by the handle's Root - valid until the next call that
    /// mutates this handle's Terminal pool (same convention as
    /// le_design_name) - copy out immediately, don't hold across another
    /// call. Returns nullptr if handle is null or id doesn't name a
    /// Terminal on this handle.
    const char *le_terminal_name(LeHandle *handle, LeTerminalId id);

    /// @brief Number of property rows for the Terminal at `id` - same
    /// name/value table shape as le_selected_object_property_at
    /// (LeProperty), but addressed directly by id rather than by current
    /// selection index, for a TCL `get_terminal`-style caller that isn't
    /// working off the current GUI selection. Rows: every plain Terminal
    /// field from cmg's generated to_properties() ("name", "direction",
    /// ...), plus "port_count" (int, from Root's own port index -
    /// "ports" itself isn't a stored field, same as
    /// le_selected_object_property_at's). No "bbox_um"/"layer_name" rows
    /// (those are piece-selection-scoped concepts, meaningless for an
    /// arbitrary id lookup). Returns 0 if handle is null or id doesn't
    /// name a Terminal on this handle.
    int32_t le_terminal_property_count(LeHandle *handle, LeTerminalId id);

    /// @brief The property row at `index`
    /// (0..le_terminal_property_count(id)-1) for the Terminal at `id`.
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, id doesn't name a Terminal on this handle, or
    /// index is out of range.
    LeProperty le_terminal_property_at(LeHandle *handle, LeTerminalId id, int32_t index);

    /// @brief Resolve a dotted property path against the Terminal at
    /// `id` (UPDATES.md item 19.2's `get_properties`/`report_properties`
    /// dot-notation, e.g. `.name`, or chained through a hop like
    /// `.ports.port_class`) - see `backend/src/database/filter.hpp`'s
    /// `parse_property_path`/`resolve_property_path` for the grammar and
    /// resolution semantics (same `match_hop`/`get_field` machinery
    /// `-filter` expressions already use, just resolving a value instead
    /// of evaluating a comparison). Every segment but the last must be a
    /// hop, the last must be a leaf field - both checked against the
    /// same allowlist `-filter` validation uses (`validate_filter_path`
    /// in api.cpp) before resolving, so an unrecognized field/hop name
    /// is a real error (pushed via `le_message_count`/`le_message_at`),
    /// not silent. Returns an all-null/zero row (LeProperty::name ==
    /// nullptr) if handle/path is null, `path` fails to parse or
    /// validate, id doesn't name a Terminal on this handle, or the path
    /// is structurally valid but resolves to nothing for this specific
    /// object (e.g. a list hop with zero elements) - the latter case
    /// pushes no message, unlike a parse/validation failure.
    LeProperty le_terminal_property_path(LeHandle *handle, LeTerminalId id, const char *path);

    // --- Library/Design/Abstract/Shape property rows (UPDATES.md item
    // 19.2's get_properties/report_properties) - same by-id shape as
    // le_terminal_property_count/_at above, one pair per type. Unlike
    // Terminal's own "port_count" (or TerminalPort/Obstruction's
    // "shapes_count", further below), none of these four add a derived
    // child-pool "_count" row - just cmg's generated to_properties() as-is
    // for each type; see le_get_designs/le_get_terminals/
    // le_get_obstructions if you need those counts instead. ---

    /// @brief Number of property rows for the Library at `id`. Rows:
    /// every plain Library field from cmg's generated to_properties()
    /// ("name"). Returns 0 if handle is null or id doesn't name a Library
    /// on this handle.
    int32_t le_library_property_count(LeHandle *handle, LeLibraryId id);

    /// @brief The property row at `index`
    /// (0..le_library_property_count(id)-1) for the Library at `id`.
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, id doesn't name a Library on this handle, or index
    /// is out of range.
    LeProperty le_library_property_at(LeHandle *handle, LeLibraryId id, int32_t index);

    /// @brief Resolve a dotted property path against the Library at
    /// `id` (e.g. `.name`, or chained through a hop like
    /// `.designs.name`) - see le_terminal_property_path's own comment
    /// for the full grammar/validation/error contract.
    LeProperty le_library_property_path(LeHandle *handle, LeLibraryId id, const char *path);

    /// @brief Number of property rows for the Design at `id`. Rows: every
    /// plain Design field from cmg's generated to_properties() ("name").
    /// Returns 0 if handle is null or id doesn't name a Design on this
    /// handle.
    int32_t le_design_property_count(LeHandle *handle, LeDesignId id);

    /// @brief The property row at `index`
    /// (0..le_design_property_count(id)-1) for the Design at `id`.
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, id doesn't name a Design on this handle, or index
    /// is out of range.
    LeProperty le_design_property_at(LeHandle *handle, LeDesignId id, int32_t index);

    /// @brief Resolve a dotted property path against the Design at
    /// `id` (e.g. `.name`, or chained through a hop like
    /// `.library.name`) - see le_terminal_property_path's own comment
    /// for the full grammar/validation/error contract.
    LeProperty le_design_property_path(LeHandle *handle, LeDesignId id, const char *path);

    /// @brief Number of property rows for the Abstract at `id`. Rows:
    /// every plain Abstract field from cmg's generated to_properties()
    /// ("type", "size", "origin", ..., plus its own list-field "_count"
    /// rows like "boundary_count"). Returns 0 if handle is null or id
    /// doesn't name an Abstract on this handle.
    int32_t le_abstract_property_count(LeHandle *handle, LeAbstractId id);

    /// @brief The property row at `index`
    /// (0..le_abstract_property_count(id)-1) for the Abstract at `id`.
    /// Returns an all-null/zero row (LeProperty::name == nullptr) if
    /// handle is null, id doesn't name an Abstract on this handle, or
    /// index is out of range.
    LeProperty le_abstract_property_at(LeHandle *handle, LeAbstractId id, int32_t index);

    /// @brief Resolve a dotted property path against the Abstract at
    /// `id` (e.g. `.type`, or chained through a hop like
    /// `.design.name`) - see le_terminal_property_path's own comment
    /// for the full grammar/validation/error contract.
    LeProperty le_abstract_property_path(LeHandle *handle, LeAbstractId id, const char *path);

    /// @brief Rename the Terminal at `id` (UPDATES.md item 15's
    /// `update_terminal_port -name ...` pattern, applied to Terminal
    /// itself) - `name` isn't index=True so this is a direct field
    /// mutation, not a generated Root::set_terminal_name (see
    /// TCL_EXPLORATION.md's "cmg codegen design" for why only
    /// indexed/parent fields get a generated setter). Same uniqueness
    /// enforcement as le_create_terminal, scoped to the Terminal's own
    /// Abstract - renaming to the Terminal's own current name is a no-op
    /// success, not a self-collision. Returns 0 on success, nonzero if
    /// handle or name is null, id doesn't name a Terminal on this handle,
    /// or name collides with a different Terminal already on the same
    /// Abstract (pushes an ERROR message, same convention as
    /// le_create_terminal).
    int le_set_terminal_name(LeHandle *handle, LeTerminalId id, const char *name);

    /// @brief Change the Terminal at `id`'s signal direction. Returns 0
    /// on success, nonzero if handle is null or id doesn't name a
    /// Terminal on this handle.
    int le_set_terminal_direction(LeHandle *handle, LeTerminalId id, int32_t direction);

    /// @brief Delete the Terminal at `id`, cascading to every
    /// TerminalPort it owns first - unlike Root::delete_terminal's own
    /// no-cascade default (see its doc comment), a orphaned port here
    /// would be permanently unreachable garbage (TerminalPorts are only
    /// ever enumerated through their parent Terminal's own port list, no
    /// top-level "every TerminalPort" API exists), not just a stale
    /// reference that degrades gracefully elsewhere - a deliberate,
    /// domain-specific exception at this API layer, not a generic Root
    /// behavior. Returns 0 on success, nonzero if handle is null or id
    /// doesn't name a Terminal on this handle.
    int le_delete_terminal(LeHandle *handle, LeTerminalId id);

    /// @brief Search every Terminal on this handle for
    /// `filter_expression` (UPDATES.md item 15's `-filter {...}`, e.g.
    /// ".name =~ IN*" - see backend/src/database/filter.hpp for the full
    /// grammar). Returns the number of matches (0 if handle or
    /// filter_expression is null, or if nothing matched), or -1 if
    /// filter_expression fails to parse (see le_message_count/
    /// le_message_at for the parse error, pushed the same way
    /// le_read_lef's own errors are). Results are cached on the handle
    /// until the next le_search_terminal call - read them via
    /// le_search_result_terminal_at, same "valid until the next call"
    /// convention as this API's other cached-result accessors
    /// (le_selected_object_property_at et al).
    int32_t le_search_terminal(LeHandle *handle, const char *filter_expression);

    /// @brief The LeTerminalId at `index` (0..le_search_terminal's last
    /// return value - 1) from the most recent le_search_terminal call on
    /// this handle. Returns an invalid id (index == UINT32_MAX) if
    /// handle is null or index is out of range.
    LeTerminalId le_search_result_terminal_at(LeHandle *handle, int32_t index);

    /// @brief Search Terminals (UPDATES.md item 19.1 - supersedes item
    /// 17's own single-filter-string `le_get_terminals`, this is the
    /// general contract every other le_get_* function in this API
    /// follows too). Three independent, each-optional axes:
    ///   - `of_abstract` scopes to one Abstract's own Terminals (see
    ///     le_get_abstracts) - pass an invalid id (e.g. a
    ///     default-constructed LeAbstractId) to use the default scope
    ///     instead: the currently selected Design's Abstract (item 17's
    ///     "current view" - le_set_current_design/
    ///     le_set_current_design_by_id), or none if no Design is
    ///     selected.
    ///   - `name_expression` glob-matches Terminal::name (Tcl `string
    ///     match` semantics, e.g. "IN*") - pass null or "" to skip this
    ///     axis (match every name).
    ///   - `filter_expression` (see backend/src/database/filter.hpp) -
    ///     pass null or "" to skip this axis. Only field/hop names this
    ///     API recognizes as valid for Terminal are accepted - an
    ///     unrecognized one is a validation error (item 19.1's own
    ///     error-checking requirement), not silent no-match.
    /// A Terminal matches iff it satisfies every given axis. Shares
    /// le_search_terminal's result buffer - read results back via
    /// le_search_result_terminal_at. Returns the match count (0 if handle
    /// is null or nothing matched), or -1 if filter_expression fails to
    /// parse or references an unknown field/hop (see le_message_count/
    /// le_message_at for either error).
    int32_t le_get_terminals(LeHandle *handle, LeAbstractId of_abstract, const char *name_expression, const char *filter_expression);

    // --- TerminalPort/Obstruction CRUD + filter-search, and Abstract
    // boundary update (Phase 4, continued) ---
    //
    // le_create_terminal_port/le_create_obstruction create an empty
    // parent only - no layer, no geometry. Shape creation
    // (le_create_terminal_port_shape/le_create_obstruction_shape) and all
    // shape geometry (le_add_shape_rect/_polygon/_path, further below)
    // are separate calls: rects aren't privileged over polygons/paths by
    // being foldable into one "create" call while the others aren't -
    // every shape member is added the same way, after the shape exists.

    /// @brief Create an empty TerminalPort owned by the Terminal at
    /// `terminal_id` - no shapes yet, see le_create_terminal_port_shape.
    /// Returns an invalid LeTerminalPortId (index == UINT32_MAX) if
    /// handle is null, or terminal_id doesn't name a Terminal on this
    /// handle.
    LeTerminalPortId le_create_terminal_port(LeHandle *handle, LeTerminalId terminal_id);

    /// @brief Number of property rows for the TerminalPort at `id` - same
    /// by-id (not selection-scoped) shape as le_terminal_property_count.
    /// Rows: "shapes_count" (int), "port_class" (string) - cmg's
    /// generated to_properties() output for TerminalPortData, unchanged.
    /// Returns 0 if handle is null or id doesn't name a TerminalPort on
    /// this handle.
    int32_t le_terminal_port_property_count(LeHandle *handle, LeTerminalPortId id);

    /// @brief The property row at `index`
    /// (0..le_terminal_port_property_count(id)-1) for the TerminalPort at
    /// `id`. Returns an all-null/zero row (LeProperty::name == nullptr)
    /// if handle is null, id doesn't name a TerminalPort on this handle,
    /// or index is out of range.
    LeProperty le_terminal_port_property_at(LeHandle *handle, LeTerminalPortId id, int32_t index);

    /// @brief Resolve a dotted property path against the TerminalPort at
    /// `id` (e.g. `.port_class`, or chained through a hop like
    /// `.terminal.name`) - see le_terminal_property_path's own comment
    /// for the full grammar/validation/error contract.
    LeProperty le_terminal_port_property_path(LeHandle *handle, LeTerminalPortId id, const char *path);

    /// @brief Delete the TerminalPort at `id`, cascading to every Shape
    /// it owns first - same reasoning as le_delete_terminal's own cascade
    /// to TerminalPorts (see its doc comment): a Shape is pooled
    /// (TCL_EXPLORATION.md Phase 3) and only ever reachable through its
    /// parent's shape list, so leaving it behind would be permanently
    /// unreachable garbage, not just a dangling reference. Returns 0 on
    /// success, nonzero if handle is null or id doesn't name a
    /// TerminalPort on this handle.
    int le_delete_terminal_port(LeHandle *handle, LeTerminalPortId id);

    /// @brief Search every TerminalPort on this handle for
    /// `filter_expression` - see le_search_terminal's own comment for the
    /// full contract (grammar, error/caching behavior); identical here,
    /// just scoped to TerminalPort (e.g. ".terminal.name =~ IN*" or
    /// ".shapes.layer_name == M4", both straight from UPDATES.md item
    /// 15's own example). Returns the match count, or -1 on a parse
    /// error.
    int32_t le_search_terminal_port(LeHandle *handle, const char *filter_expression);

    /// @brief The LeTerminalPortId at `index` from the most recent
    /// le_search_terminal_port call - see le_search_result_terminal_at's
    /// own comment for the general contract.
    LeTerminalPortId le_search_result_terminal_port_at(LeHandle *handle, int32_t index);

    /// @brief Search TerminalPorts - see le_get_terminals' own comment
    /// for the general contract. `of_terminal` scopes to one Terminal's
    /// own Ports (see le_terminal_by_name) - pass an invalid id to use
    /// the default scope: every TerminalPort under every Terminal of the
    /// currently selected Abstract (a 2-hop current-view default).
    /// TerminalPort has no name field, so there is no `name_expression`
    /// parameter, only `filter_expression`. Returns the match count, or
    /// -1 on a filter parse/validation error.
    int32_t le_get_terminal_ports(LeHandle *handle, LeTerminalId of_terminal, const char *filter_expression);

    /// @brief Create an empty Obstruction on the Abstract at
    /// `abstract_id` - no shapes yet, see le_create_obstruction_shape.
    /// Returns an invalid LeObstructionId (index == UINT32_MAX) if
    /// handle is null, or abstract_id doesn't name an Abstract on this
    /// handle.
    LeObstructionId le_create_obstruction(LeHandle *handle, LeAbstractId abstract_id);

    /// @brief Number of property rows for the Obstruction at `id` - same
    /// by-id shape as le_terminal_property_count. Rows: "shapes_count"
    /// (int) - cmg's generated to_properties() output for
    /// ObstructionData, unchanged. Returns 0 if handle is null or id
    /// doesn't name an Obstruction on this handle.
    int32_t le_obstruction_property_count(LeHandle *handle, LeObstructionId id);

    /// @brief The property row at `index`
    /// (0..le_obstruction_property_count(id)-1) for the Obstruction at
    /// `id`. Returns an all-null/zero row (LeProperty::name == nullptr)
    /// if handle is null, id doesn't name an Obstruction on this handle,
    /// or index is out of range.
    LeProperty le_obstruction_property_at(LeHandle *handle, LeObstructionId id, int32_t index);

    /// @brief Resolve a dotted property path against the Obstruction at
    /// `id` - Obstruction itself has no leaf scalar fields (see
    /// api.cpp's filter_field_tables), so every valid path here is
    /// chained through a hop, e.g. `.shapes.layer_name`. See
    /// le_terminal_property_path's own comment for the full grammar/
    /// validation/error contract.
    LeProperty le_obstruction_property_path(LeHandle *handle, LeObstructionId id, const char *path);

    /// @brief Delete the Obstruction at `id`, cascading to every Shape it
    /// owns first - same reasoning as le_delete_terminal_port. Returns 0
    /// on success, nonzero if handle is null or id doesn't name an
    /// Obstruction on this handle.
    int le_delete_obstruction(LeHandle *handle, LeObstructionId id);

    /// @brief Search every Obstruction on this handle for
    /// `filter_expression` - see le_search_terminal's own comment for the
    /// full contract. Returns the match count, or -1 on a parse error.
    int32_t le_search_obstruction(LeHandle *handle, const char *filter_expression);

    /// @brief The LeObstructionId at `index` from the most recent
    /// le_search_obstruction call - see le_search_result_terminal_at's
    /// own comment for the general contract.
    LeObstructionId le_search_result_obstruction_at(LeHandle *handle, int32_t index);

    /// @brief Search Obstructions - see le_get_terminals' own comment
    /// for the general contract. `of_abstract` scopes to one Abstract's
    /// own Obstructions - pass an invalid id to use the default scope:
    /// the currently selected Abstract. Obstruction has no name field,
    /// so there is no `name_expression` parameter, only
    /// `filter_expression`. Returns the match count, or -1 on a filter
    /// parse/validation error.
    int32_t le_get_obstructions(LeHandle *handle, LeAbstractId of_abstract, const char *filter_expression);

    /// @brief Replace the Abstract at `id`'s boundary outline wholesale
    /// with a single polygon built from `coords_um` (UPDATES.md item
    /// 15's `update_abstract -boundary {x y x y ...}`) - a flat array of
    /// microns, alternating x/y, `coord_count` must be a positive even
    /// number (at least 3 points, i.e. coord_count >= 6, to be a real
    /// polygon - a smaller count is rejected). No standalone Boundary
    /// object exists (see TCL_EXPLORATION.md's round-2 decision) -
    /// `Abstract.boundary` is a plain field, so this is a direct
    /// mutation through le::Root::get_abstract(), not a generated
    /// Root::set_abstract_boundary. Returns 0 on success, nonzero if
    /// handle or coords_um is null, id doesn't name an Abstract on this
    /// handle, coord_count is invalid, or no Technology has been read
    /// yet (needed for the micron-to-dbu conversion).
    int le_update_abstract_boundary(LeHandle *handle, LeAbstractId id, const double *coords_um, int32_t coord_count);

    // --- Shape CRUD, addressed by a stable id (TCL_EXPLORATION.md Phase 3:
    // `Shape` was pooled specifically so a single existing shape - attached
    // to either a TerminalPort or an Obstruction - could be read/updated/
    // deleted independently of its parent). A shape's own id doesn't say
    // which kind of parent it belongs to - le_terminal_port_shape_at/
    // le_obstruction_shape_at are how a caller discovers a LeShapeId in
    // the first place (enumerating a specific parent's shapes) or
    // le_create_terminal_port_shape/le_create_obstruction_shape return
    // one directly; after that, every le_shape_*/le_add_shape_*/
    // le_remove_shape_*/le_delete_shape call below only needs the
    // LeShapeId itself.
    //
    // Rects/polygons/paths are all treated the same way: none are
    // privileged, each has its own count/read/add/remove functions
    // following the same shape (a plain 0-based position within that
    // shape's own list of that member - not a further stable id; unlike
    // a Shape itself, a rect/polygon/path never needs to be addressed
    // independently of the Shape it's part of). Coordinates are always a
    // flat microns array (converted to/from dbu via the same shared/
    // global Technology::database_units_microns every other coordinate
    // in this API uses) - a real coordinate-list typemap, so a caller
    // doesn't have to pre-flatten into a raw double*, is Phase 5's job
    // (see TCL_EXPLORATION.md); this is the plain-C building block it
    // wraps. Texts aren't included - see TCL_EXPLORATION.md for why
    // (they're a Pipeline-computed render-time label, never LEF-authored
    // data, so there's nothing for a caller to create/read here). ---

    /// @brief Mirrors the database's ShapeId handle - see LeLibraryId's
    /// comment for the general contract.
    typedef struct LeShapeId
    {
        uint32_t index;
        uint32_t generation;
    } LeShapeId;

    /// @brief One point, in microns - `le_shape_polygon_point_at`'s and
    /// `le_shape_path_point_at`'s return type.
    typedef struct LePointUm
    {
        double x_um;
        double y_um;
    } LePointUm;

    /// @brief One rect's corners, in microns - `le_shape_rect_at`'s
    /// return type.
    typedef struct LeRectUm
    {
        double ll_x_um;
        double ll_y_um;
        double ur_x_um;
        double ur_y_um;
    } LeRectUm;

    /// @brief Create an empty Shape (just `layer_name`, no geometry yet)
    /// owned by the TerminalPort at `port_id`. Returns an invalid
    /// LeShapeId (index == UINT32_MAX) if handle or layer_name is null,
    /// or port_id doesn't name a TerminalPort on this handle.
    LeShapeId le_create_terminal_port_shape(LeHandle *handle, LeTerminalPortId port_id, const char *layer_name);

    /// @brief Create an empty Shape owned by the Obstruction at
    /// `obstruction_id` - see le_create_terminal_port_shape's own
    /// comment for the general contract. Returns an invalid LeShapeId if
    /// handle or layer_name is null, or obstruction_id doesn't name an
    /// Obstruction on this handle.
    LeShapeId le_create_obstruction_shape(LeHandle *handle, LeObstructionId obstruction_id, const char *layer_name);

    /// @brief Search Shapes (UPDATES.md item 19.1) - `of_terminal_port`/
    /// `of_obstruction` each independently scope to one TerminalPort's or
    /// one Obstruction's own Shapes (a Shape has exactly one real parent,
    /// never both - see ShapeData's own comment - but both parameters are
    /// accepted so a caller doesn't need to know which kind it has;
    /// pass an invalid id for whichever doesn't apply). If **both** are
    /// invalid, the default scope is every Shape reachable from the
    /// current view: every Shape under the currently selected Design's
    /// Abstract's Terminals' Ports, unioned with every Shape under that
    /// Abstract's own Obstructions (the same current-view concept
    /// le_get_terminal_ports already uses, extended one hop further).
    /// Shape has no name field, only `layer_name` (a property, not an
    /// identity), so there is no `name_expression` parameter - only
    /// `filter_expression`, see le_get_libraries' own comment. Returns
    /// the match count, or -1 on a filter parse/validation error.
    int32_t le_get_shapes(LeHandle *handle, LeTerminalPortId of_terminal_port, LeObstructionId of_obstruction, const char *filter_expression);

    /// @brief The LeShapeId at `index` from the most recent le_get_shapes
    /// call - see le_search_result_library_at's own contract.
    LeShapeId le_search_result_shape_at(LeHandle *handle, int32_t index);

    /// @brief Number of property rows for the Shape at `id` (UPDATES.md
    /// item 19.2 - same by-id shape as le_terminal_property_count/_at and
    /// its Library/Design/Abstract siblings above). Rows: every plain
    /// Shape field from cmg's generated to_properties() ("layer_name",
    /// "spacing", ..., plus its own list-field "_count" rows like
    /// "rects_count"). Shape has no children (the leaf of the
    /// Library->Design->Abstract->{Terminal->TerminalPort,Obstruction}->
    /// Shape hierarchy), so unlike TerminalPort/Obstruction there's no
    /// derived child-pool "_count" row this function could even add.
    /// Returns 0 if handle is null or id doesn't name a Shape on this
    /// handle.
    int32_t le_shape_property_count(LeHandle *handle, LeShapeId id);

    /// @brief The property row at `index`
    /// (0..le_shape_property_count(id)-1) for the Shape at `id`. Returns
    /// an all-null/zero row (LeProperty::name == nullptr) if handle is
    /// null, id doesn't name a Shape on this handle, or index is out of
    /// range.
    LeProperty le_shape_property_at(LeHandle *handle, LeShapeId id, int32_t index);

    /// @brief Resolve a dotted property path against the Shape at `id`
    /// (e.g. `.layer_name`, or chained through a hop like
    /// `.terminal_port.terminal.name`) - see le_terminal_property_path's
    /// own comment for the full grammar/validation/error contract.
    LeProperty le_shape_property_path(LeHandle *handle, LeShapeId id, const char *path);

    /// @brief Number of shapes owned by the TerminalPort at `id` -
    /// indexes `le_terminal_port_shape_at`'s own `index` parameter,
    /// 0..this-1. Returns 0 if handle is null or id doesn't name a
    /// TerminalPort on this handle.
    int32_t le_terminal_port_shape_count(LeHandle *handle, LeTerminalPortId id);

    /// @brief The LeShapeId at `index` (0..le_terminal_port_shape_count(id)-1)
    /// owned by the TerminalPort at `id`. Returns an invalid LeShapeId
    /// (index == UINT32_MAX) if handle is null, id doesn't name a
    /// TerminalPort on this handle, or index is out of range.
    LeShapeId le_terminal_port_shape_at(LeHandle *handle, LeTerminalPortId id, int32_t index);

    /// @brief Number of shapes owned by the Obstruction at `id` - see
    /// le_terminal_port_shape_count's own comment for the general
    /// contract. Returns 0 if handle is null or id doesn't name an
    /// Obstruction on this handle.
    int32_t le_obstruction_shape_count(LeHandle *handle, LeObstructionId id);

    /// @brief The LeShapeId at `index` owned by the Obstruction at `id` -
    /// see le_terminal_port_shape_at's own comment for the general
    /// contract. Returns an invalid LeShapeId if handle is null, id
    /// doesn't name an Obstruction on this handle, or index is out of
    /// range.
    LeShapeId le_obstruction_shape_at(LeHandle *handle, LeObstructionId id, int32_t index);

    /// @brief The layer name of the Shape at `id`. Owned by the handle's
    /// Root - valid until the handle is destroyed, this shape is
    /// deleted, or le_set_shape_layer_name changes it, never owned by
    /// the caller. Returns null if handle is null or id doesn't name a
    /// Shape on this handle.
    const char *le_shape_layer_name(LeHandle *handle, LeShapeId id);

    /// @brief Rename the Shape at `id`'s layer. Returns 0 on success,
    /// nonzero if handle or layer_name is null, or id doesn't name a
    /// Shape on this handle.
    int le_set_shape_layer_name(LeHandle *handle, LeShapeId id, const char *layer_name);

    /// @brief Delete the Shape at `id`. Its parent (TerminalPort or
    /// Obstruction) is untouched - le_terminal_port_shape_count/
    /// le_obstruction_shape_count on it simply reports one fewer
    /// afterward. Returns 0 on success, nonzero if handle is null or id
    /// doesn't name a Shape on this handle.
    int le_delete_shape(LeHandle *handle, LeShapeId id);

    /// @brief Number of rects on the Shape at `id` - indexes
    /// `le_shape_rect_at`'s own `index` parameter, 0..this-1. Returns 0
    /// if handle is null or id doesn't name a Shape on this handle.
    int32_t le_shape_rect_count(LeHandle *handle, LeShapeId id);

    /// @brief The rect at `index` (0..le_shape_rect_count(id)-1) on the
    /// Shape at `id`, in microns. Returns an all-zero LeRectUm if handle
    /// is null, id doesn't name a Shape on this handle, index is out of
    /// range, or no Technology has been read yet (needed for the
    /// dbu-to-micron conversion) - the same "can't distinguish a real
    /// zero-sized rect from a degrade-gracefully zero" tradeoff
    /// le_snapped_mouse_position's own comment already accepts for this
    /// API, not a new one.
    LeRectUm le_shape_rect_at(LeHandle *handle, LeShapeId id, int32_t index);

    /// @brief Append one rect to the Shape at `id`. Returns 0 on
    /// success, nonzero if handle is null, id doesn't name a Shape on
    /// this handle, or no Technology has been read yet.
    int le_add_shape_rect(LeHandle *handle, LeShapeId id, double ll_x_um, double ll_y_um, double ur_x_um, double ur_y_um);

    /// @brief Remove the rect at `index` (0..le_shape_rect_count(id)-1)
    /// from the Shape at `id`, shifting every later rect's index down by
    /// one (matching le_shape_rect_at's own 0-based-position addressing
    /// - not a stable id, see this section's own comment for why that's
    /// fine here). Returns 0 on success, nonzero if handle is null, id
    /// doesn't name a Shape on this handle, or index is out of range.
    int le_remove_shape_rect(LeHandle *handle, LeShapeId id, int32_t index);

    /// @brief Number of polygons on the Shape at `id` - indexes
    /// `le_shape_polygon_point_count`/`le_shape_polygon_point_at`'s own
    /// `polygon_index` parameter, 0..this-1. Returns 0 if handle is null
    /// or id doesn't name a Shape on this handle.
    int32_t le_shape_polygon_count(LeHandle *handle, LeShapeId id);

    /// @brief Number of points in the polygon at `polygon_index`
    /// (0..le_shape_polygon_count(id)-1) on the Shape at `id` - indexes
    /// `le_shape_polygon_point_at`'s own `point_index` parameter,
    /// 0..this-1. Returns 0 if handle is null, id doesn't name a Shape
    /// on this handle, or polygon_index is out of range.
    int32_t le_shape_polygon_point_count(LeHandle *handle, LeShapeId id, int32_t polygon_index);

    /// @brief The point at `point_index` in the polygon at
    /// `polygon_index` on the Shape at `id`, in microns. Returns an
    /// all-zero LePointUm if handle is null, id doesn't name a Shape on
    /// this handle, either index is out of range, or no Technology has
    /// been read yet.
    LePointUm le_shape_polygon_point_at(LeHandle *handle, LeShapeId id, int32_t polygon_index, int32_t point_index);

    /// @brief Append one polygon to the Shape at `id`, built from
    /// `points_um` (a flat array of microns, alternating x/y -
    /// `point_coord_count` must be a positive even number, at least 6 -
    /// i.e. at least 3 points - to be a real polygon). Returns 0 on
    /// success, nonzero if handle or points_um is null; id doesn't name
    /// a Shape on this handle; point_coord_count is invalid; or no
    /// Technology has been read yet.
    int le_add_shape_polygon(LeHandle *handle, LeShapeId id, const double *points_um, int32_t point_coord_count);

    /// @brief Remove the polygon at `polygon_index`
    /// (0..le_shape_polygon_count(id)-1) from the Shape at `id` - same
    /// position-shifts-down semantics as le_remove_shape_rect. Returns 0
    /// on success, nonzero if handle is null, id doesn't name a Shape on
    /// this handle, or polygon_index is out of range.
    int le_remove_shape_polygon(LeHandle *handle, LeShapeId id, int32_t polygon_index);

    /// @brief Number of paths on the Shape at `id` - indexes
    /// `le_shape_path_width_um`/`le_shape_path_point_count`/
    /// `le_shape_path_point_at`'s own `path_index` parameter, 0..this-1.
    /// Returns 0 if handle is null or id doesn't name a Shape on this
    /// handle.
    int32_t le_shape_path_count(LeHandle *handle, LeShapeId id);

    /// @brief The width, in microns, of the path at `path_index`
    /// (0..le_shape_path_count(id)-1) on the Shape at `id`. Returns 0 if
    /// handle is null, id doesn't name a Shape on this handle,
    /// path_index is out of range, or no Technology has been read yet.
    double le_shape_path_width_um(LeHandle *handle, LeShapeId id, int32_t path_index);

    /// @brief Number of points in the centerline of the path at
    /// `path_index` on the Shape at `id` - indexes
    /// `le_shape_path_point_at`'s own `point_index` parameter,
    /// 0..this-1. Returns 0 if handle is null, id doesn't name a Shape
    /// on this handle, or path_index is out of range.
    int32_t le_shape_path_point_count(LeHandle *handle, LeShapeId id, int32_t path_index);

    /// @brief The point at `point_index` on the centerline of the path
    /// at `path_index` on the Shape at `id`, in microns. Returns an
    /// all-zero LePointUm if handle is null, id doesn't name a Shape on
    /// this handle, either index is out of range, or no Technology has
    /// been read yet.
    LePointUm le_shape_path_point_at(LeHandle *handle, LeShapeId id, int32_t path_index, int32_t point_index);

    /// @brief Append one path to the Shape at `id`: `width_um` (the
    /// path's stroke width) and `points_um`/`point_coord_count` (its
    /// centerline, same flat-microns-array convention as
    /// le_add_shape_polygon, but only at least 2 points - i.e.
    /// point_coord_count >= 4 - are required, since a path's centerline
    /// doesn't need to close into a loop). Returns 0 on success, nonzero
    /// if handle or points_um is null; id doesn't name a Shape on this
    /// handle; point_coord_count is invalid; or no Technology has been
    /// read yet.
    int le_add_shape_path(LeHandle *handle, LeShapeId id, double width_um, const double *points_um, int32_t point_coord_count);

    /// @brief Remove the path at `path_index`
    /// (0..le_shape_path_count(id)-1) from the Shape at `id` - same
    /// position-shifts-down semantics as le_remove_shape_rect. Returns 0
    /// on success, nonzero if handle is null, id doesn't name a Shape on
    /// this handle, or path_index is out of range.
    int le_remove_shape_path(LeHandle *handle, LeShapeId id, int32_t path_index);

#ifdef __cplusplus
}
#endif
