#pragma once
#include <string>
#include <memory>
#include <expected>
#include "../lefdef/lef/include/lefrReader.hpp"
#include "../database/database.hpp"
#include "spdlog/spdlog.h"

namespace le
{
    class LEFReader
    {
    public:
        int read_lef(std::string filename, Root &root, std::string library_name);

        // Pure LEF-enum -> database-enum conversions. Public (and static, with
        // no dependency on parser/instance state) so they can be unit tested
        // directly rather than only incidentally through a full LEF parse.
        static Orientation orientation_from_parser(int v);
        static RoutingDirection routing_direction_from_parser(const char *name);
        static SignalDirection signal_direction_from_parser(const char *name);

    private:
        // Callbacks
        static int lefrLayerCbkFn(lefrCallbackType_e typ, lefiLayer *lef_layer, void *user_data);
        static int lefrMacroBeginCbkFn(lefrCallbackType_e typ, const char *name, void *user_data);
        static int lefrUnitsCbkFn(lefrCallbackType_e typ, lefiUnits *lef_units, void *user_data);
        static int lefrMacroCbkFn(lefrCallbackType_e typ, lefiMacro *lef_macro, void *user_data);
        static int lefrPinCbkFn(lefrCallbackType_e typ, lefiPin *lef_pin, void *user_data);
        static int lefrObstructionCbkFn(lefrCallbackType_e typ, lefiObstruction *lef_obs, void *user_data);

        // Helper functions
        int64_t microns_to_dbu(const double microns);
        static Polygon polygon_from_parser(LEFReader *reader, int count, double *x, double *y);
        static Rect rect_from_parser(LEFReader *reader, double xl, double yl, double xh, double yh);
        static std::vector<Shape> shapes_from_parser(LEFReader *reader, lefiGeometries *geometries);
        static void post_process(LEFReader *reader);

        // Private variables
        Root *root_;
        TechnologyId technology_id_;
        TechnologyData *technology_;
        std::string library_name_;
        LibraryId library_id_;
        AbstractData abstract_data_;
        AbstractId abstract_id_;
    };
}
