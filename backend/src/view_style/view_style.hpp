#pragma once
#include "../database/database.hpp"
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

            for (LayerId layer_id : root.get_technology_layers(technology_id))
            {
                const LayerData *layer = root.get_layer(layer_id);
                set.add(layer->name + "/TERMINAL", ViewLayerPurpose::TERMINAL, layer_id);
                set.add(layer->name + "/OBSTRUCTION", ViewLayerPurpose::OBSTRUCTION, layer_id);
            }

            set.boundary_id_ = set.add("BOUNDARY", ViewLayerPurpose::BOUNDARY, LayerId{});

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

        ViewLayerId add(std::string name, ViewLayerPurpose purpose, LayerId layer)
        {
            ViewLayerId id = pool_.create(ViewLayerData{
                .name = std::move(name),
                .purpose = purpose,
                .layer = layer,
                .style = default_style(purpose),
            });
            lookup_.push_back(LookupEntry{.layer = layer, .purpose = purpose, .id = id});
            return id;
        }

        // Placeholder palette - real styling is a render/UI concern to
        // revisit once there's something on screen to look at.
        static ViewLayerStyle default_style(ViewLayerPurpose purpose)
        {
            switch (purpose)
            {
            case ViewLayerPurpose::TERMINAL:
                return ViewLayerStyle{.outline_color = {0, 200, 0, 255}, .fill_color = {0, 200, 0, 120}};
            case ViewLayerPurpose::OBSTRUCTION:
                return ViewLayerStyle{.outline_color = {200, 0, 0, 255}, .fill_color = {200, 0, 0, 60}};
            case ViewLayerPurpose::BOUNDARY:
                return ViewLayerStyle{.outline_color = {255, 255, 255, 255}, .fill_color = {0, 0, 0, 0}};
            }
        }

        Pool<ViewLayerData, ViewLayerId> pool_;
        std::vector<LookupEntry> lookup_;
        ViewLayerId boundary_id_;
    };
}
