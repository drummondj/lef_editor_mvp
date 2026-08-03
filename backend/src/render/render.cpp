#include "render.hpp"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"

// See render.hpp's comment on Renderer::default_typeface() for why this is
// isolated to its own translation unit: ApplicationServices.h (pulled in by
// SkFontMgr_mac_ct.h) defines legacy Carbon Rect/Point/Polygon typedefs
// that collide with le::Rect/le::Point/le::Polygon wherever a file does
// `using namespace le`.
#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#endif

namespace le
{
    sk_sp<SkTypeface> Renderer::default_typeface()
    {
        static const sk_sp<SkTypeface> typeface = []() -> sk_sp<SkTypeface>
        {
#if defined(__APPLE__)
            sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_CoreText(nullptr);
            return font_mgr->matchFamilyStyle(nullptr, SkFontStyle());
#else
            return nullptr;
#endif
        }();
        return typeface;
    }
}
