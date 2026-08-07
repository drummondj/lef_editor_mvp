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
    /// - LE_KEY_FIT: le_fit_scene() with a fixed padding.
    /// - LE_KEY_PAN_LEFT/RIGHT/UP/DOWN: le_pan() by a fixed
    ///   viewport-fraction step in the corresponding direction.
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

    /// @brief End a mouse gesture at the given pixel position, e.g. on a
    /// pointer-up event. A no-op if handle is null or there was no
    /// matching le_mouse_down() call first (a stray/duplicate mouse-up).
    /// Two outcomes, both subject to Pipeline::hit_test_point/
    /// hit_test_rect's own topmost-layer-first (click) / all-layers
    /// (drag) and layer-selectability rules, and both consulting
    /// LE_KEY_SHIFT's current held state (see le_key_down) rather than
    /// taking it as a parameter here:
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
    /// Either way, ends the in-progress drag gesture (Scene::end_drag) -
    /// see le_mouse_down's own doc comment for the live rubber-band
    /// rectangle this also stops showing.
    void le_mouse_up(LeHandle *handle, int32_t x, int32_t y);

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

#ifdef __cplusplus
}
#endif
