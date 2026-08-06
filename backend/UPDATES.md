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

## 2.2 Layer visibility and selection

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
