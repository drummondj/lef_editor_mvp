#pragma once
#include "../database/database.hpp"
#include <array>
#include <string>
#include <vector>

namespace le
{
    /// @brief What a ViewLayer represents: which kind of object drew the
    /// shapes on it. Purely an application/rendering concept, not a LEF
    /// vocabulary term - a closed enum we own, not an open string field.
    enum class ViewLayerPurpose
    {
        TERMINAL,
        OBSTRUCTION,
        BOUNDARY,
    };

    struct Color
    {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        uint8_t a = 255;
    };

    struct ViewLayerStyle
    {
        Color outline_color;
        Color fill_color;
    };

    struct ViewLayerTag
    {
    };
    using ViewLayerId = Id<ViewLayerTag>;

    /// @brief A named, styled, purpose-tagged rendering layer. `layer` is
    /// invalid for the one special BOUNDARY ViewLayer, which isn't tied to
    /// any physical LEF Layer.
    struct ViewLayerData
    {
        std::string name;
        ViewLayerPurpose purpose;
        LayerId layer;
        ViewLayerStyle style;
    };

    /// @brief The set of ViewLayers for a Technology: a TERMINAL and an
    /// OBSTRUCTION ViewLayer per physical Layer, plus one BOUNDARY
    /// ViewLayer not tied to any physical Layer. Shared/global for a
    /// Technology (not per-Scene) - which ViewLayers are toggled off is a
    /// per-Scene concern (Scene::set_layer_visible), but the set of
    /// ViewLayers that exist and how they're styled is not.
    class ViewLayerSet
    {
    public:
        static ViewLayerSet build_for_technology(const Root &root, TechnologyId technology_id)
        {
            ViewLayerSet set;

            size_t color_index = 0;
            for (LayerId layer_id : root.get_technology_layers(technology_id))
            {
                const LayerData *layer = root.get_layer(layer_id);
                // One color per physical Layer, shared by every purpose of
                // that layer (not per-purpose like before) - see
                // layer_color's own comment for where this palette/scheme
                // comes from. Purpose has no visual distinction yet beyond
                // that shared color; a future update adds fill patterns per
                // purpose instead of splitting the color further.
                const ViewLayerStyle style = layer_style(layer_color(color_index++));
                set.add(layer->name + "/TERMINAL", ViewLayerPurpose::TERMINAL, layer_id, style);
                set.add(layer->name + "/OBSTRUCTION", ViewLayerPurpose::OBSTRUCTION, layer_id, style);
            }

            set.boundary_id_ = set.add("BOUNDARY", ViewLayerPurpose::BOUNDARY, LayerId{}, boundary_style());

            return set;
        }

        /// @brief Resolve a physical Layer + purpose to its ViewLayerId.
        /// Invalid if no such ViewLayer was registered (e.g. `layer` itself
        /// is invalid/unknown - callers don't need to check that first).
        ViewLayerId find(LayerId layer, ViewLayerPurpose purpose) const
        {
            for (const auto &entry : lookup_)
                if (entry.layer == layer && entry.purpose == purpose)
                    return entry.id;
            return ViewLayerId{};
        }

        ViewLayerId boundary_view_layer() const { return boundary_id_; }

        const ViewLayerData *get(ViewLayerId id) const { return pool_.get(id); }

        std::vector<ViewLayerId> all() const { return pool_.ids(); }

    private:
        struct LookupEntry
        {
            LayerId layer;
            ViewLayerPurpose purpose;
            ViewLayerId id;
        };

        ViewLayerId add(std::string name, ViewLayerPurpose purpose, LayerId layer, ViewLayerStyle style)
        {
            ViewLayerId id = pool_.create(ViewLayerData{
                .name = std::move(name),
                .purpose = purpose,
                .layer = layer,
                .style = style,
            });
            lookup_.push_back(LookupEntry{.layer = layer, .purpose = purpose, .id = id});
            return id;
        }

        // Default per-layer palette - ported from the sibling project's
        // `layer_generator_node.hpp` (../../layout_engine/backend/pipeline/
        // nodes/layer_generator_node.hpp), which cycles through the same 30
        // colors one per physical Layer. Real styling is still a render/UI
        // concern to revisit further (this only replaces the old fixed
        // green-TERMINAL/red-OBSTRUCTION scheme with per-layer color, so
        // e.g. M1 and M2 are now visually distinguishable at all).
        static constexpr std::array<Color, 30> kDefaultColors = {{
            {255, 0, 0, 255},     // red
            {0, 255, 0, 255},     // green
            {0, 0, 255, 255},     // blue
            {255, 255, 0, 255},   // yellow
            {255, 0, 255, 255},   // magenta
            {0, 255, 255, 255},   // cyan
            {128, 0, 0, 255},     // maroon
            {0, 128, 0, 255},     // dark green
            {0, 0, 128, 255},     // navy
            {128, 128, 0, 255},   // olive
            {128, 0, 128, 255},   // purple
            {0, 128, 128, 255},   // teal
            {192, 192, 192, 255}, // silver
            {128, 128, 128, 255}, // gray
            {255, 165, 0, 255},   // orange
            {210, 105, 30, 255},  // chocolate
            {139, 69, 19, 255},   // saddle brown
            {255, 20, 147, 255},  // deep pink
            {50, 205, 50, 255},   // lime green
            {72, 209, 204, 255},  // medium turquoise
            {123, 104, 238, 255}, // medium slate blue
            {255, 215, 0, 255},   // gold
            {160, 82, 45, 255},   // sienna
            {32, 178, 170, 255},  // light sea green
            {218, 112, 214, 255}, // orchid
            {95, 158, 160, 255},  // cadet blue
            {255, 99, 71, 255},   // tomato
            {60, 179, 113, 255},  // medium sea green
            {106, 90, 205, 255},  // slate blue
            {238, 130, 238, 255}, // violet
        }};

        // Wraps (modulo), not clamps or overflows, once there are more
        // Layers than palette entries - unlike the sibling's own indexing
        // (`default_colors[++color_idx]`), which has no bounds check and
        // reads out of range past 30 layers.
        static Color layer_color(size_t index)
        {
            return kDefaultColors[index % kDefaultColors.size()];
        }

        static ViewLayerStyle layer_style(Color base)
        {
            Color fill = base;
            fill.a = 100;
            return ViewLayerStyle{.outline_color = base, .fill_color = fill};
        }

        static ViewLayerStyle boundary_style()
        {
            return ViewLayerStyle{.outline_color = {255, 255, 255, 255}, .fill_color = {0, 0, 0, 0}};
        }

        Pool<ViewLayerData, ViewLayerId> pool_;
        std::vector<LookupEntry> lookup_;
        ViewLayerId boundary_id_;
    };
}
