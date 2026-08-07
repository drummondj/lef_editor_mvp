#include <fmt/ostream.h>
#include "lef_reader.hpp"
#include "../geometry/geometry.hpp"
#include <cmath>

namespace le
{

    int LEFReader::read_lef(std::string filename, Root &root, std::string library_name)
    {
        lefrInit();

        // Setup callbacks
        lefrSetUnitsCbk(lefrUnitsCbkFn);
        lefrSetLayerCbk(lefrLayerCbkFn);
        lefrSetMacroBeginCbk(lefrMacroBeginCbkFn);
        lefrSetMacroCbk(lefrMacroCbkFn);
        lefrSetPinCbk(lefrPinCbkFn);
        lefrSetObstructionCbk(lefrObstructionCbkFn);
        lefrSetRegisterUnusedCallbacks();

        // If there is no technology, create a new one. Otherwise get the first technology.
        TechnologyId technology_id = root.is_technology_empty() ? root.create_technology(TechnologyData{}) : root.get_technology_ids().front();

        // Initialize
        root_ = &root;
        technology_id_ = technology_id;
        technology_ = root.get_technology(technology_id);
        library_name_ = library_name;

        // Open file - RAII (fclose via unique_ptr's deleter) rather than a
        // manual fclose() at each return path below, so the handle is
        // still closed if an exception propagates out of lefrRead (which
        // invokes our own callbacks - e.g. a std::vector::reserve deep in
        // shapes_from_parser could throw on a malformed file) rather than
        // returning through here normally.
        std::unique_ptr<FILE, int (*)(FILE *)> file(fopen(filename.c_str(), "r"), &fclose);
        if (!file)
        {
            spdlog::error("Could not open LEF file {}.", filename);
            return 1;
        }

        // Read file
        int result = lefrRead(file.get(), filename.c_str(), (void *)this);
        if (result != 0)
        {
            spdlog::error("Could not parse LEF file {}.", filename);
            return 2;
        }
        lefrPrintUnusedCallbacks(stdout);

        return 0;
    }

    /// @brief Build boundary from OVERLAP or SIZE and ORIGIN
    void LEFReader::post_process(LEFReader *reader)
    {
        // Look for OVERLAP obstructions
        std::vector<const Shape *> overlap_shapes;

        for (auto const &obstruction_id : reader->root_->get_abstract_obstructions(reader->abstract_id_))
        {
            auto const obstruction = reader->root_->get_obstruction(obstruction_id);
            for (auto const &shape : obstruction->shapes)
            {
                if (shape.layer_name == "OVERLAP")
                {
                    overlap_shapes.push_back(&shape);
                }
            }
        }

        bool did_use_overlap = false;

        if (overlap_shapes.size() > 0)
        {
            // Union overlap_shapes
            auto boundary = Geometry::union_shapes(overlap_shapes);

            if (boundary)
            {
                reader->abstract_data_.boundary = *boundary;
                did_use_overlap = true;
            }
        }
        if (!did_use_overlap)
        {
            // Use size and origin to determine boundary
            Rect bbox;
            bbox.ll.x = reader->abstract_data_.origin.x;
            bbox.ll.y = reader->abstract_data_.origin.y;
            bbox.ur.x = reader->abstract_data_.origin.x + reader->abstract_data_.size.x;
            bbox.ur.y = reader->abstract_data_.origin.y + reader->abstract_data_.size.y;
            reader->abstract_data_.boundary.push_back(Geometry::rect_to_polygon(bbox));
        }

        for (auto const &polygon : reader->abstract_data_.boundary)
            spdlog::debug("boundary polygon {}", fmt::streamed(polygon));
    }

    int LEFReader::lefrLayerCbkFn(lefrCallbackType_e typ, lefiLayer *lef_layer, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        auto layer_name = lef_layer->name();

        if (reader->root_->get_layer_by_name(layer_name).valid())
        {
            spdlog::warn("Layer {} already exists. Ignoring new definition.", layer_name);
            return 0;
        }

        // The parser reuses one scratch lefiLayer across every LAYER statement and
        // never resets direction_ between them, so a layer that omits DIRECTION
        // would otherwise silently inherit whichever prior layer last set it.
        reader->root_->create_layer(
            LayerData{
                .technology = reader->technology_id_,
                .name = layer_name,
                .type = lef_layer->type(),
                .direction = lef_layer->hasDirection() ? routing_direction_from_parser(lef_layer->direction()) : RoutingDirection::NONE,
            });

        return 0;
    }

    int LEFReader::lefrUnitsCbkFn(lefrCallbackType_e typ, lefiUnits *lef_units, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);
        if (lef_units->hasDatabase())
        {
            auto lef_database_units = lef_units->databaseNumber();
            auto const technology = reader->technology_;
            if (technology->database_units_microns == 0)
            {
                technology->database_units_microns = lef_database_units;
            }
            else
            {
                if (technology->database_units_microns != lef_database_units)
                {
                    spdlog::warn("Database UNITS in LEF {} does not equal current technology units {}. Ignoring new definition.", lef_database_units, technology->database_units_microns);
                }
            }
        }
        return 0;
    }

    int LEFReader::lefrMacroBeginCbkFn(lefrCallbackType_e typ, const char *name, void *user_data)
    {
        spdlog::debug("processing macro {}", name);

        auto reader = static_cast<LEFReader *>(user_data);

        // If library has not been created, then create one
        if (!reader->library_id_.valid())
        {
            reader->library_id_ = reader->root_->create_library(LibraryData{.name = reader->library_name_});
        }

        // If design does not exist, then create it
        auto design_id = reader->root_->get_design_by_name(name);
        if (!design_id.valid())
        {
            design_id = reader->root_->create_design(
                DesignData{
                    .library = reader->library_id_,
                    .name = name,
                });
        }

        // If abstract view exists, then error
        auto abstract_id = reader->root_->get_design_abstract(design_id);
        if (abstract_id.valid())
        {
            spdlog::error("Abstract view for design {} already exists.", name);
            return 1;
        }

        reader->abstract_data_ = AbstractData{.design = design_id};
        reader->abstract_id_ = reader->root_->create_abstract(reader->abstract_data_);

        return 0;
    }

    int LEFReader::lefrMacroCbkFn(lefrCallbackType_e typ, lefiMacro *lef_macro, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);

        if (lef_macro->hasClass())
            reader->abstract_data_.type = lef_macro->macroClass();

        reader->abstract_data_.foreigns.reserve(lef_macro->numForeigns());

        for (int i = 0; i < lef_macro->numForeigns(); i++)
        {
            auto foreign = Foreign{.name = lef_macro->foreignName(i)};

            // Not lef_macro->hasForeignOrigin(i): the vendored parser's
            // hasForeignOrigin_ is actually populated from the orient code
            // (see lefiMacro::addForeign), not a real "has a point" flag -
            // hasForeignPoint(i) is the field that's genuinely wired to it.
            if (lef_macro->hasForeignPoint(i))
                foreign.origin = Point{
                    .x = reader->microns_to_dbu(lef_macro->foreignX(i)),
                    .y = reader->microns_to_dbu(lef_macro->foreignY(i)),
                };
            else
                foreign.origin = Point{0, 0};

            if (lef_macro->hasForeignOrient(i))
                foreign.orient = orientation_from_parser(lef_macro->foreignOrient(i));
            else
                foreign.orient = Orientation::N;

            reader->abstract_data_.foreigns.push_back(foreign);
        }

        if (lef_macro->hasSize())
            reader->abstract_data_.size = Point{
                .x = reader->microns_to_dbu(lef_macro->sizeX()),
                .y = reader->microns_to_dbu(lef_macro->sizeY()),
            };

        if (lef_macro->hasOrigin())
            reader->abstract_data_.origin = Point{
                .x = reader->microns_to_dbu(lef_macro->originX()),
                .y = reader->microns_to_dbu(lef_macro->originY()),
            };

        reader->abstract_data_.symmetry = Symmetry{
            .r90 = lef_macro->has90Symmetry() == 1,
            .x = lef_macro->hasXSymmetry() == 1,
            .y = lef_macro->hasYSymmetry() == 1,
        };

        if (lef_macro->hasSiteName())
        {
            reader->abstract_data_.site = lef_macro->siteName();
        }

        // Post process
        post_process(reader);

        // Update stored AbstractData in pool
        // TODO: change this to build AbstractData first, then add child objects by
        // iterating through lef_macro data twice.
        if (reader->abstract_id_.valid())
        {
            if (auto stored = reader->root_->get_abstract(reader->abstract_id_))
            {
                *stored = reader->abstract_data_;
            }
        }
        auto stored = reader->root_->get_abstract(reader->abstract_id_);
        spdlog::debug("stored boundary size = {}", stored ? stored->boundary.size() : -1);
        spdlog::debug("stored site = {}", stored ? stored->site : "NONE");

        return 0;
    }

    int LEFReader::lefrPinCbkFn(
        lefrCallbackType_e typ,
        lefiPin *lef_pin,
        void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);

        // TODO: Check that pin doesn't already exists with the same name
        // TODO: link to shematic depending if we are creating the schematic or not

        // The parser reuses one scratch lefiPin across every PIN statement
        // and never resets direction_ between them, so a pin that omits
        // DIRECTION would otherwise silently inherit whichever prior pin
        // last set it (same hazard as lefiLayer's direction_ above).
        auto terminal = TerminalData{
            .abstract = reader->abstract_id_,
            .name = lef_pin->name(),
            .direction = lef_pin->hasDirection() ? signal_direction_from_parser(lef_pin->direction()) : SignalDirection::NONE,
        };

        auto terminal_id = reader->root_->create_terminal(terminal);

        for (int i = 0; i < lef_pin->numPorts(); i++)
        {
            auto lef_port = lef_pin->port(i);
            auto shapes = shapes_from_parser(reader, lef_port);
            auto port = TerminalPortData{.terminal = terminal_id};
            port.shapes.insert(port.shapes.end(),
                               shapes.begin(),
                               shapes.end());
            reader->root_->create_terminal_port(port);
        }

        return 0;
    }

    int LEFReader::lefrObstructionCbkFn(lefrCallbackType_e typ, lefiObstruction *lef_obs, void *user_data)
    {
        auto reader = static_cast<LEFReader *>(user_data);

        auto shapes = shapes_from_parser(reader, lef_obs->geometries());
        spdlog::debug("shapes size = {}", shapes.size());

        reader->root_->create_obstruction(ObstructionData{.abstract = reader->abstract_id_, .shapes = shapes});

        return 0;
    }

    int64_t LEFReader::microns_to_dbu(const double microns)
    {
        return std::llround(microns * technology_->database_units_microns);
    }

    Orientation LEFReader::orientation_from_parser(int v)
    {
        // Matches lef.y's `orientation` rule (R0=N, R90=W, R180=S, R270=E,
        // MY=FN, MYR90=FW, MX=FS, MXR90=FE) - a 90 degree rotation turns a
        // North-facing shape to face West, not South, so this is NOT the
        // naive 0=N,1=S,2=E,3=W ordering it's easy to assume instead.
        switch (v)
        {
        case 0:
            return Orientation::N;
        case 1:
            return Orientation::W;
        case 2:
            return Orientation::S;
        case 3:
            return Orientation::E;
        case 4:
            return Orientation::FN;
        case 5:
            return Orientation::FW;
        case 6:
            return Orientation::FS;
        case 7:
            return Orientation::FE;
        default:
            return Orientation::N;
        }
    }

    RoutingDirection LEFReader::routing_direction_from_parser(const char *name)
    {
        if (name == nullptr)
            return RoutingDirection::NONE;

        if (strcmp(name, "HORIZONTAL") == 0)
            return RoutingDirection::H;

        if (strcmp(name, "VERTICAL") == 0)
            return RoutingDirection::V;

        return RoutingDirection::NONE;
    }

    SignalDirection LEFReader::signal_direction_from_parser(const char *name)
    {
        if (name == nullptr)
            return SignalDirection::NONE;

        if (strcmp(name, "INPUT") == 0)
            return SignalDirection::INPUT;

        if (strcmp(name, "OUTPUT") == 0)
            return SignalDirection::OUTPUT;

        if (strcmp(name, "INOUT") == 0 || strcmp(name, "FEEDTHRU") == 0 || strcmp(name, "OUTPUT TRISTATE") == 0)
            return SignalDirection::INOUT;

        return SignalDirection::NONE;
    }

    Polygon LEFReader::polygon_from_parser(LEFReader *reader, int count, double *x, double *y)
    {
        auto polygon = Polygon{};
        polygon.points.reserve(count);
        for (int k = 0; k < count; k++)
        {
            polygon.points.push_back(Point{
                .x = reader->microns_to_dbu(x[k]),
                .y = reader->microns_to_dbu(y[k]),
            });
        }
        return polygon;
    }

    Rect LEFReader::rect_from_parser(LEFReader *reader, double xl, double yl, double xh, double yh)
    {
        return Rect{
            .ll = Point{
                .x = reader->microns_to_dbu(xl),
                .y = reader->microns_to_dbu(yl),
            },
            .ur = Point{
                .x = reader->microns_to_dbu(xh),
                .y = reader->microns_to_dbu(yh),
            },
        };
    }

    size_t LEFReader::safe_iteration_count(double x_count, double y_count)
    {
        if (!std::isfinite(x_count) || !std::isfinite(y_count) || x_count < 0.0 || y_count < 0.0)
            return 0;

        // Generous but bounded - a real LEF ITERATE statement repeats a
        // handful of times (single/double digits), never anywhere close
        // to this. Checked against each factor individually, before
        // multiplying, so the product itself can't overflow double's
        // range either.
        constexpr double kMaxReasonableCount = 1'000'000.0;
        if (x_count > kMaxReasonableCount || y_count > kMaxReasonableCount)
            return 0;

        return static_cast<size_t>(x_count) * static_cast<size_t>(y_count);
    }

    // The `!shape.has_value()` guards below (RECT/RECT-ITERATE/PATH/
    // PATH-ITERATE/POLYGON/POLYGON-ITERATE without a preceding LAYER) are
    // unreachable in practice - the LEF grammar itself already rejects that
    // as a parse error before any such item is added to `geometries` - but
    // kept anyway as defense-in-depth at this file-parsing boundary rather
    // than removed as dead code.
    std::vector<Shape> LEFReader::shapes_from_parser(LEFReader *reader, lefiGeometries *geometries)
    {
        std::vector<Shape> shapes;
        uint64_t width = 0;
        uint64_t geo_count = 0;

        shapes.reserve(geometries->numItems());

        std::optional<Shape> shape;
        for (int j = 0; j < geometries->numItems(); j++)
        {
            auto item_type = geometries->itemType(j);
            switch (item_type)
            {
            case lefiGeomEnum::lefiGeomLayerE:
            {
                if (shape.has_value() && geo_count > 0)
                {
                    shapes.push_back(shape.value());
                    geo_count = 0;
                }

                shape = Shape{.layer_name = geometries->getLayer(j)};
                break;
            }
            case lefiGeomEnum::lefiGeomWidthE:
            {
                width = reader->microns_to_dbu(geometries->getWidth(j));
                break;
            }
            case lefiGeomEnum::lefiGeomRectE:
            {
                if (!shape.has_value())
                {
                    spdlog::error("RECT defined without previous LAYER definition.");
                    break;
                }
                auto lef_rect = geometries->getRect(j);
                auto rect = rect_from_parser(reader, lef_rect->xl, lef_rect->yl, lef_rect->xh, lef_rect->yh);
                shape.value().rects.push_back(rect);
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomRectIterE:
            {
                if (!shape.has_value())
                {
                    spdlog::error("RECT ITERATE defined without previous LAYER definition.");
                    break;
                }
                auto lef_rect_iter = geometries->getRectIter(j);
                shape.value().rects.reserve(safe_iteration_count(lef_rect_iter->xStart, lef_rect_iter->yStart));

                for (int ix = 0; ix < lef_rect_iter->xStart; ix++)
                {
                    for (int iy = 0; iy < lef_rect_iter->yStart; iy++)
                    {
                        auto rect = rect_from_parser(
                            reader,
                            lef_rect_iter->xl + ix * lef_rect_iter->xStep,
                            lef_rect_iter->yl + iy * lef_rect_iter->yStep,
                            lef_rect_iter->xh + ix * lef_rect_iter->xStep,
                            lef_rect_iter->yh + iy * lef_rect_iter->yStep);
                        shape.value().rects.push_back(rect);
                        geo_count++;
                    }
                }
                break;
            }
            case lefiGeomEnum::lefiGeomPathE:
            {
                if (!shape.has_value())
                {
                    spdlog::error("PATH defined without previous LAYER definition.");
                    break;
                }
                auto lef_path = geometries->getPath(j);
                auto polygon = polygon_from_parser(reader, lef_path->numPoints, lef_path->x, lef_path->y);
                auto path = Path{.width = width, .polygon = polygon};
                shape.value().paths.push_back(path);
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomPathIterE:
            {
                if (!shape.has_value())
                {
                    spdlog::error("PATH ITERATE defined without previous LAYER definition.");
                    break;
                }
                auto lef_path_iter = geometries->getPathIter(j);
                shape.value().paths.reserve(safe_iteration_count(lef_path_iter->xStart, lef_path_iter->yStart));
                for (int ix = 0; ix < lef_path_iter->xStart; ix++)
                {
                    for (int iy = 0; iy < lef_path_iter->yStart; iy++)
                    {
                        auto polygon = polygon_from_parser(reader, lef_path_iter->numPoints, lef_path_iter->x, lef_path_iter->y);
                        auto offset = Point{
                            .x = reader->microns_to_dbu(ix * lef_path_iter->xStep),
                            .y = reader->microns_to_dbu(iy * lef_path_iter->yStep),
                        };
                        auto transformed = Geometry::transform(polygon, offset);
                        auto path = Path{.width = width, .polygon = transformed};
                        shape.value().paths.push_back(path);
                        geo_count++;
                    }
                }
                break;
            }
            case lefiGeomEnum::lefiGeomPolygonE:
            {
                if (!shape.has_value())
                {
                    spdlog::error("POLYGON defined without previous LAYER definition.");
                    break;
                }
                auto lef_polygon = geometries->getPolygon(j);
                auto polygon = polygon_from_parser(reader, lef_polygon->numPoints, lef_polygon->x, lef_polygon->y);
                shape.value().polygons.push_back(polygon);
                geo_count++;
                break;
            }
            case lefiGeomEnum::lefiGeomPolygonIterE:
            {
                if (!shape.has_value())
                {
                    spdlog::error("POLYGON ITERATE defined without previous LAYER definition.");
                    break;
                }
                auto lef_polygon_iter = geometries->getPolygonIter(j);
                shape.value().polygons.reserve(safe_iteration_count(lef_polygon_iter->xStart, lef_polygon_iter->yStart));
                for (int ix = 0; ix < lef_polygon_iter->xStart; ix++)
                {
                    for (int iy = 0; iy < lef_polygon_iter->yStart; iy++)
                    {
                        auto polygon = polygon_from_parser(reader, lef_polygon_iter->numPoints, lef_polygon_iter->x, lef_polygon_iter->y);
                        auto offset = Point{
                            .x = reader->microns_to_dbu(ix * lef_polygon_iter->xStep),
                            .y = reader->microns_to_dbu(iy * lef_polygon_iter->yStep),
                        };
                        auto transformed = Geometry::transform(polygon, offset);
                        shape.value().polygons.push_back(transformed);
                        geo_count++;
                    }
                }
                break;
            }
            }
        }
        if (shape.has_value() && geo_count > 0)
            shapes.push_back(shape.value());
        return shapes;
    }
}