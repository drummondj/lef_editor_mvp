# 1. API Updates

## 1.1. Enable library browser. - DONE

Currently the API only exposes a list of flat designs and their count. To build a hierarchical library browser widget, I need a hierarchical representation of the library -> design -> view structure. Including the LibraryId, DesignId and AbstractId for each one.

Can a struct be passed via the C API? Or maybe use Google Flat Buffers / Protocol Buffers to transfer complex data? Please explore possible options.

## 1.2 Zoom/Pan control. - DONE

The current API requires the frontend to store scale and pan information and send that to the API. But, when le_fit_scene is called, that information is updated internally to the backend. I would like the backend to own the scale and pan numbers, and replace the le_set_pan and le_set_pan with:

- le_zoom(double factor, int x, int y) - where factor can be positive for zoom in and negative for zoom out. x and y are the pixel coordinate for the center of the zoom.

- le_pan(double x_factor, y_factor) - same concept as le_zoom, using a factor to pan

# 2. Colors and layer selection

## 2.1 The default colors should be split into two lists. - DONE

1. Default ROUTING and CUT layer colors - bright high contrast colors
2. Other layer colors - more muted colors

Each CUT layer above a ROUTING layer should be the same color as the ROUTING layer below it.

## 2.2 Layer visibility and selection - DONE

Each view layer can be set as visible/invisible and selectable/un-selectable.

This is achieved by a widget that contains one row for each layer and columns for each purpose.

The API needs to be update to get a list of layers for the widget, plus provide methods to change visibility and selectability.

## 2.3 Layer fill patterns - DONE

I would like to implement the following fill patterns. Maybe using a shader?

1. ROUTING layers - diagonal stripes with different directions depending on the layers routing direction
2. CUT layers - a cross shape as most cut layers should be rectangles
3. OBSTRUCTIONS purpose - a brick pattern
4. TERMINAL purpose - a tighter diagonal stripe
5. Other layers - small dots.

All patterns must have a transparent background so layers below are visible between.

# 3. Error message handling and display

When certain operations happen in the backend, such as reading a LEF file. I would like to return any error messages to the flutter_plugin so they can be displayed in the GUI.

# 4. Automatic text sizing - DONE

Try and size text based on the shape it's labeling, with a minimum text size. Should be simple for rectangle shapes, but polygons and paths need to use the width of the shape to determine the text size and not the bbox.

# 5. Grid and snapping

## 5.1 Grid display - DONE

I would like to display a grid of major and minor dots. The major dots should be bolder than minor dots. The grid spacing should be configurable via the API but a good default value is minor 5nm and major 50nm. When zoomed out the grid should not be shown if too dense.

A solid line should be used for the x and y axis.

## 5.2 Mouse snapping - DONE

I would like to store the current mouse position and a snapped to grid mouse position. Then display a red box around the current grid position that follows the mouse. The mouse position, in pixels, should be sent by flutter via the API. If this causes performance problems then an alternative solution may be required. For example, rendering the mouse position in a separate SkPicture and thread, then just merging it with the abstract layout picture.

## 5.3 Coordinate display - DONE

The API should return the snapped mouse coordinates so they can be displayed in the Flutter UI.

## 5.4 Origin marker - DONE

Please add a cross shape at the origin of the abstract, which is not necessarily at 0,0 - it depends on the origin property of the abstract view.

# 6. Small shape display - DONE

When zoomed out of a large design, very small shapes disappear which makes it look like there is nothing in the design. I would like to see the performance impact of rendering a single pixel instead of nothing for small shapes.

Single pixel representations can not be selected, they are there just to inform the user that something exists and they need to zoom in.

# 7. Shape selection

We will have different modes which cause mouse gestures to perform different tasks.

The first of these modes is "Select" mode.

## 7.1 Mouse gestures and keyboard modifiers - DONE

Shape selection requires the following features:

1. When the mouse hovers over a selectable shape, the outline of the shape turns yellow.
2. When the mouse is clicked the shape is added to the selection.
3. Shapes are selected by top-most layer first.
4. To select shapes under the current selection, the user must press shift.
5. Mouse down, move and release selects all selectable shapes on all layers completely enclosed by the selection rectangle.
6. Multiple shapes are selected with shift-click, without shift the current selection is replaced with the new selection.

## 7.2 Selection results - DONE

Each selected object and it's properties are sent to the frontend via the API, so the flutter GUI can display the data in a table.

Simple properties such as int, double, string etc are just sent as is. Coordinates are always converted to um. Lists of other objects or values are just sent as the size of the list, not the list contents.

## 7.3 Tooltip message

I would like a toopltip message to be generated, which is used in the GUIs status bar below the texture. This tooltip will contain instructions to the user about which mouse gestures and keyboard shortcuts/modifiers are available at that time. For example, when select mode is activated the tooltip should read something click: "Left click to select. Shift for multi-select. Left click and drag for rectangle multi-select."

# 8. Label placement update

I would like to try a different Geometry::get_label_location algorithm.

1. For polygons/paths I would like to fracture the polygon into rects. If the polygon's bbox is wider than it is tall, fracture vertically, otherwise fracture horizontally. The place a label in the center of the largest rectangle.

2. For rects, find the largest rect in the shape and label it.

3. Please draw a cross at the origin of the label because some larger labels can overlap other shapes, so it's not obvious what the label is labeling!
