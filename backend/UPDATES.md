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

# 3. Error message handling and display - DONE

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

## 7.3 Tooltip message - DONE

I would like a toopltip message to be generated, which is used in the GUIs status bar below the texture. This tooltip will contain instructions to the user about which mouse gestures and keyboard shortcuts/modifiers are available at that time. For example, when select mode is activated the tooltip should read something click: "Left click to select. Shift for multi-select. Left click and drag for rectangle multi-select."

# 8. Label placement update - DONE

I would like to try a different Geometry::get_label_location algorithm.

1. For polygons/paths I would like to fracture the polygon into rects. If the polygon's bbox is wider than it is tall, fracture vertically, otherwise fracture horizontally. The place a label in the center of the largest rectangle.

2. For rects, find the largest rect in the shape and label it.

3. Please draw a cross at the origin of the label because some larger labels can overlap other shapes, so it's not obvious what the label is labeling!

# 9. Mouse and keyboard event additions - DONE

1. Ctrl-A select all but stop at 10,000 objects
2. Mouse scroll wheel zoom in/out
3. Right mouse button rectangle zoom
4. 1, 2, 3 ,4 etc changes layer visibility - where 1 maps to the first routing layer, 2 to the second etc. If two adjacent routing layers are made visible via the keyboard (not when the user clicks on the layer manager), then the VIA layer between then is also made visible. Same logic in reverse for making vias invisible.
5. Ctrl-D deselect all
6. Ctrl-F fit selected

# 10. Default layer visibility - DONE

By default the only visible layers should be ROUTING, CUT and BOUNDARY, all other layers should not be visible when the user reads a LEF with LAYERs in to initialize a Technology.

# 11. Modes switching and edit mode

The default select mode (already implemented) can be switched to other modes, initially edit mode. During edit mode only the selected objects can be edited. To change the selection the user must switch back to select mode.

Modes are changed by 2 mechanisms:

1. Keyboard shortcuts: s - select mode, e - edit mode
2. Via events in the flutter UI which requires a mode switching method in the API

The current mode can be queried via the API.

Details of how objects are edited to follow.

# 12. LEF Syntax Completion (big task)

The LEF parser only supports a subset of the available syntax. I need it to support all LEF syntax.

There is an example LEF file that contains all LEF syntax that needs to be parsed: src/lefdef/lef/TEST/complete.5.8.lef

Here is an outline of a plan to follow (you may modify as long as the results are the same):

1. Create a LEFWriter class that creates a LEF file for the specified AbstractId. The LEFWriter needs an option to choose wether to include Technology layers or not, or just write out Technology layers.
2. Read the complete.5.8.lef and write it out, diff the results
3. Update the LEFReader and LEFWriter to fix the differences reported by lefdiff. You will also need to update schema.py and rebuild the database. Make sure any coordinates are converted from microns to dbu.
4. Repeat step 2 until there are no reported differences.

There will be ambiguous cases where you need to ask me what to do. For example, should you store other values as integers, or us the units in the LEF file, or use SI units. Please ask me and we will discuss each individual case.

Known issue: the current LEFReader splits ITERATE statements into separate shapes, we need to store the raw ITERATE as an object in the database then split during generate_shapes in the pipeline.

# 13. Ruler mode

The next mode to implement is Ruler mode, shortcut key r.

- Rulers can be drawn on the design by clicking on the layout view.
- Each time the user clicks a new point is added to the ruler.
- A "ghost" ruler should follow the users snapped mouse position, until they click, then the real ruler segment is drawn.
- Points are snapped to the grid by using the snapped mouse position.
- The ruler displays the distance between points and the total distance.
- Rulers are drawn orthogonally by default. The user can hold down shift to allow an non-orthogonal ruler.
- A tooltip displays the information about the shift function.

Ruler display:

- A line is drawn between the point with dynamic ticks depending on the scene's scale.
- There are major ticks with values and minor ticks without values.
- Major ticks should be in multiple of tens, i.e. every 1, 10, 100 etc and minor values should be one order of magnitude below. i.e. ten minor ticks for every major tick.
- The point-to-point distance should be displayed at the end of the line segment between the two points.
- The total distance should be displayed at the last point, prefixed with "total: "

# 14. Properties revisit

I notice that the properties contain the bbox of shapes and not the raw values. I would like to see the raw polygon, path and rect values as properties, in addition to the bbox.

Also, I would like the boundary layer selectable, which selects the abstract object and creates it's properties in the API.

# 15. TCL support exploration - DONE (see TCL_EXPLORATION.md; `show_gui` deliberately deferred)

I would like to explore how to enable TCL commands to be executed by the user.

For example:

```tcl
read_lef <filename>
create_library -name my_library
current_library my_library
create_design -name top_design
current_design top_design
create_view -type abstract
current_view abstract

create_terminal -name IN0 -direction IN
create_terminal_port -terminal IN0 -shapes {0.1 0.1 0.3 0.4} -layer M4

set terminals [get_terminal -filter {.name =~ IN*}]

set i 0
foreach terminal $terminals {
    create_terminal_port -terminal $terminal -shapes [list [expr $i * 10 - 10] 0 [expr $i * 10 + 10] 100]
}

set terminal_ports [get_terminal_ports -filter {.terminal.name =~ IN* && .layer_name == M4}]

foreach terminal_port $terminal_ports {
    update_terminal_port -name [get_prop $terminal_port .name]_SUFFIX
}

delete_terminal [get_terminal]

... etc ...
```

The main CRUD functions are, for each type of object:

- create
- get
- update
- delete

This is just a very rough example, not an exact specification, we would need to plan a concrete specification. Especially how to create different shapes, rect, poly, and path.

Ideally, I would like to support a batch mode where the user can run a terminal command to open an interactive TCL shell interface. Then use a TCL command, e.g. show_gui, to open the Flutter GUI. Please explore if this is possible. Possible problems include:

1. How to connect the GUI to the Root database already read in batch mode
2. How to refresh the GUI when TCL commands are entered in the shell

The reason I want to explore this now, is because the next step is to create API functions to create/read/update/delete terminal, obstruction and boundary objects and their shapes. I would like to develop the C API so it can be used by the Flutter GUI and a TCL shell at the same time.

There is a TCL interface generator called SWIG which may be useful to wrap the C API into a TCL API.

Also, Shape objects may need to be added to a pool to support this. The reason they are not in a pool right now, is that they can multiple parent types.

cmg could be used to generate the C API.

# 16. Pipeline and render module refactor for structure and clarity - PARTIALLY DONE (Pipeline done, see BENCHMARKS.md 2026-08-12; Renderer is a separate follow-up)

I would like more structure to help increase code clarity, understanding and readability.

Both pipeline and render perform similar data transformation functions using cached data. To make it easier to determine what feed each stage and waht is cached, I would like to refactor both modules to introduce some helper classes.

This MVP is only a small subset of the number of steps required for a full LEF/DEF editor, so it is only going to get more complex.

My idea is:

1. Each step in the data flow has it's own class
2. The output of each class contains a version number
3. Classes can be stitched together, so the inputs of one connect to the outputs of other.
4. Ideally some generic class can be created for easier future expansion.

This should help reduce the amount of caching bugs, like we had to fix in the previous commit.
