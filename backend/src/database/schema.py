from cmg.schema import Schema, Klass, Field

schema = Schema(
    name="layout_engine",
    description="Layout Engine Database Schema",
    namespace="le",
    version="0.4.0",
    classes=[
        Klass(
            name="Technology",
            description="Technology information such as layers and site definitions",
            fields=[
                Field(
                    name="layers",
                    description="Routing layers",
                    type="Layer",
                    is_list=True,
                    is_child=True,
                ),
                Field(
                    name="database_units_microns",
                    description="Database units in microns",
                    type="double",
                    example=2000.0,
                ),
            ],
        ),
        Klass(
            name="Layer",
            description="A routing layer",
            fields=[
                Field(
                    name="technology",
                    description="Parent technology",
                    type="Technology",
                    parent="layers",
                ),
                Field(
                    name="name",
                    description="The name of the layer",
                    type="str",
                    example="M1",
                    index=True,
                ),
                Field(
                    name="type",
                    description="The type of the layer: ROUTING, CUT, IMPLANT, MASTERSLICE etc",
                    type="str",
                    example="ROUTING",
                ),
                Field(
                    name="direction",
                    description="The layer routing direction",
                    type="RoutingDirection",
                ),
            ],
        ),
        Klass(
            name="RoutingDirection",
            description="A routing direction: H, V or NONE",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(
                    name="H",
                    description="Horizontal routing",
                    type="int",
                    value=0,
                ),
                Field(
                    name="V",
                    description="Vertical routing",
                    type="int",
                    value=1,
                ),
                Field(
                    name="NONE",
                    description="No routing direction",
                    type="int",
                    value=2,
                ),
            ],
        ),
        Klass(
            name="Point",
            description="A coordinate",
            has_pool=False,
            fields=[
                Field(
                    name="x",
                    description="The x coordinate in database units",
                    type="int64_t",
                    example=100,
                ),
                Field(
                    name="y",
                    description="The y coordinate in database units",
                    type="int64_t",
                    example=200,
                ),
            ],
        ),
        Klass(
            name="Rect",
            description="A rectangle comprising of two points",
            has_pool=False,
            fields=[
                Field(
                    name="ll",
                    description="Lower-left point",
                    type="Point",
                ),
                Field(
                    name="ur",
                    description="Upper-right point",
                    type="Point",
                ),
            ],
        ),
        Klass(
            name="Polygon",
            description="A polygon",
            has_pool=False,
            fields=[
                Field(
                    name="points",
                    description="Points in the polygon",
                    type="Point",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="Path",
            description="A path",
            has_pool=False,
            fields=[
                Field(
                    name="polygon",
                    description="Polygon representing the center line of the path",
                    type="Polygon",
                ),
                Field(
                    name="width",
                    description="Width of the path in database units",
                    type="uint64_t",
                    example=10,
                ),
            ],
        ),
        Klass(
            name="Text",
            description="A text object used to label items in a layout",
            has_pool=False,
            fields=[
                Field(
                    name="label",
                    description="The text value",
                    type="str",
                    example="VDD",
                ),
                Field(
                    name="location",
                    description="The text location",
                    type="Point",
                ),
                Field(
                    name="size",
                    description="Local width of the shape geometry at the label's location, in database units, used to size the rendered text",
                    type="double",
                    example=200.0,
                ),
            ],
        ),
        Klass(
            name="Shape",
            description="A shape on a layer",
            has_pool=False,
            fields=[
                Field(
                    name="layer_name",
                    description="The name of the layer",
                    type="str",
                    example="M1",
                ),
                Field(
                    name="paths",
                    description="A list of paths",
                    type="Path",
                    is_list=True,
                ),
                Field(
                    name="polygons",
                    description="A list of polygons",
                    type="Polygon",
                    is_list=True,
                ),
                Field(
                    name="rects",
                    description="A list of rects",
                    type="Rect",
                    is_list=True,
                ),
                Field(
                    name="texts",
                    description="A list of texts",
                    type="Text",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="Orientation",
            description="Placement orientations, e.g. N, S, E, W, FN, FS, FE, FW",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(
                    name="N",
                    description="North orientation",
                    type="int",
                    value=0,
                ),
                Field(
                    name="S",
                    description="South orientation",
                    type="int",
                    value=2,
                ),
                Field(
                    name="E",
                    description="East orientation",
                    type="int",
                    value=3,
                ),
                Field(
                    name="W",
                    description="West orientation",
                    type="int",
                    value=1,
                ),
                Field(
                    name="FN",
                    description="Flip-North orientation",
                    type="int",
                    value=4,
                ),
                Field(
                    name="FS",
                    description="Flip-South orientation",
                    type="int",
                    value=6,
                ),
                Field(
                    name="FE",
                    description="Flip-East orientation",
                    type="int",
                    value=7,
                ),
                Field(
                    name="FW",
                    description="Flip-West orientation",
                    type="int",
                    value=5,
                ),
            ],
        ),
        Klass(
            name="Library",
            description="A library containing designs",
            fields=[
                Field(
                    name="name",
                    description="The name of the library",
                    type="str",
                    example="my_library",
                    index=True,
                ),
                Field(
                    name="designs",
                    description="Designs contained in this library",
                    type="Design",
                    is_list=True,
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="Design",
            description="A library cell or logical module",
            fields=[
                Field(
                    name="library",
                    description="Parent library",
                    type="Library",
                    parent="designs",
                ),
                Field(
                    name="name",
                    description="The name of the design",
                    type="str",
                    example="top_level",
                    index=True,
                ),
                Field(
                    name="abstract",
                    description="The abstract view",
                    type="Abstract",
                    is_child=True,
                ),
                Field(
                    name="schematic",
                    description="The schematic view",
                    type="Schematic",
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="Abstract",
            description="A physical abstract view (LEF)",
            fields=[
                Field(
                    name="design",
                    description="Parent design",
                    type="Design",
                    parent="abstract",
                ),
                Field(
                    name="type",
                    description="Type or class of abstract, e.g. CORE, PAD, SPACER, ENDCAP, COVER etc",
                    type="str",
                    example="CORE",
                ),
                Field(
                    name="foreigns",
                    description="Information used to determine the abstract location and orientation wrt to the layout",
                    type="Foreign",
                    is_list=True,
                ),
                Field(
                    name="size",
                    description="The width and height of the block",
                    type="Point",
                ),
                Field(
                    name="origin",
                    description="The original of the block",
                    type="Point",
                ),
                Field(
                    name="bbox",
                    description="The bbox of the boundary",
                    type="Rect",
                ),
                Field(
                    name="boundary",
                    description="The boundary of the abstract. If the block is rectilinear, then this matches the OVERLAP layer, otherwise it is the same as bbox.",
                    type="Polygon",
                    is_list=True,
                ),
                Field(
                    name="symmetry",
                    description="The symmetry of the abstract",
                    type="Symmetry",
                ),
                Field(
                    name="site",
                    description="The name of the SITE",
                    type="str",
                    example="MYSITE",
                ),
                Field(
                    name="terminals",
                    description="A list of physical terminals",
                    type="Terminal",
                    is_child=True,
                    is_list=True,
                ),
                Field(
                    name="obstructions",
                    description="A list of obstructions",
                    type="Obstruction",
                    is_child=True,
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="Foreign",
            description="A design abstract view foreign definition",
            has_pool=False,
            fields=[
                Field(
                    name="name",
                    description="The foreign cell name (usually the same as the design name)",
                    type="str",
                    example="BUFX1",
                ),
                Field(
                    name="origin",
                    description="The foreign origin",
                    type="Point",
                ),
                Field(
                    name="orient",
                    description="The foreign orientation",
                    type="Orientation",
                ),
            ],
        ),
        Klass(
            name="Symmetry",
            description="Specifies if this design can be flipped and/or rotated",
            has_pool=False,
            fields=[
                Field(
                    name="r90",
                    description="The design can be rotated by 90 degrees",
                    type="bool",
                    example=True,
                ),
                Field(
                    name="x",
                    description="The design can be flipped horizontally",
                    type="bool",
                    example=True,
                ),
                Field(
                    name="y",
                    description="The design can be flipped vertically",
                    type="bool",
                    example=True,
                ),
            ],
        ),
        Klass(
            name="Terminal",
            description="Top-level pin name of an abstract",
            fields=[
                Field(
                    name="abstract",
                    description="Parent abstract",
                    type="Abstract",
                    parent="terminals",
                ),
                Field(
                    name="name",
                    description="Name of the terminal",
                    type="str",
                    example="INPUT",
                ),
                Field(
                    name="direction",
                    description="The direction of the terminal",
                    type="SignalDirection",
                ),
                Field(
                    name="ports",
                    description="Physical ports",
                    type="TerminalPort",
                    is_child=True,
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="TerminalPort",
            description="Physical connection for a Terminal",
            fields=[
                Field(
                    name="terminal",
                    description="Parent terminal",
                    type="Terminal",
                    parent="ports",
                ),
                Field(
                    name="shapes",
                    description="The terminal port shapes",
                    type="Shape",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="Obstruction",
            description="An abstract routing blockage",
            fields=[
                Field(
                    name="abstract",
                    description="Parent abstract",
                    parent="obstructions",
                    type="Abstract",
                ),
                Field(
                    name="shapes",
                    description="The obstruction shapes",
                    type="Shape",
                    is_list=True,
                ),
            ],
        ),
        Klass(
            name="Schematic",
            description="A logical connectivity view (netlist)",
            fields=[
                Field(
                    name="design",
                    description="Parent design",
                    type="Design",
                    parent="schematic",
                ),
                Field(
                    name="instances",
                    description="Instances contained in this design",
                    type="Instance",
                    is_list=True,
                    is_child=True,
                ),
            ],
        ),
        Klass(
            name="Instance",
            description="An instance of another design",
            fields=[
                Field(
                    name="schematic",
                    description="Parent schematic",
                    type="Schematic",
                    parent="instances",
                ),
                Field(
                    name="name",
                    description="The name of the instance",
                    type="str",
                    example="U1",
                ),
                Field(
                    name="reference_name",
                    description="The name of the reference design",
                    type="str",
                    example="BUFX1",
                ),
                Field(
                    name="reference_design",
                    description="The ID of the reference design after linking",
                    type="Design",
                ),
                Field(
                    name="location",
                    description="The location of the lower-left corner of this instance",
                    type="Point",
                    is_optional=True,
                ),
            ],
        ),
        Klass(
            name="SignalDirection",
            description="The direction of a signal such as a port or terminal",
            is_enum=True,
            has_pool=False,
            fields=[
                Field(
                    name="INPUT",
                    description="Input signal",
                    type="int",
                    value=0,
                ),
                Field(
                    name="OUTPUT",
                    description="Output signal",
                    type="int",
                    value=1,
                ),
                Field(
                    name="INOUT",
                    description="In/out signal",
                    type="int",
                    value=2,
                ),
                Field(
                    name="NONE",
                    description="No direction specified",
                    type="int",
                    value=3,
                ),
            ],
        ),
    ],
)
