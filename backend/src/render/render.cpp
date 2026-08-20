#include "draw_helpers.hpp"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"

// See draw_helpers.hpp's comment on default_typeface() for why this is
// isolated to its own translation unit: ApplicationServices.h (pulled in by
// SkFontMgr_mac_ct.h) defines legacy Carbon Rect/Point/Polygon typedefs
// that collide with le::Rect/le::Point/le::Polygon wherever a file does
// `using namespace le`. SkFontMgr_fontconfig.h doesn't have that problem
// (fontconfig's own headers don't define anything under those names), but
// it's included here anyway to keep both platforms' font-manager headers
// in the one .cpp that's allowed to know about them, rather than splitting
// the isolation rule across files.
#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(__linux__)
// SkFontScanner.h needed for the nullptr passed as
// SkFontMgr_New_FontConfig's second (unique_ptr<SkFontScanner>) arg below -
// without the full type, unique_ptr's default_delete destructor fails to
// instantiate (`sizeof` an incomplete type) even for a null pointer.
#include "include/core/SkFontScanner.h"
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

namespace le
{
    sk_sp<SkTypeface> default_typeface()
    {
        static const sk_sp<SkTypeface> typeface = []() -> sk_sp<SkTypeface>
        {
#if defined(__APPLE__)
            sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_CoreText(nullptr);
            return font_mgr->matchFamilyStyle(nullptr, SkFontStyle());
#elif defined(__linux__)
            // Second param is a required SkFontScanner - unlike the fc
            // config pointer, nullptr here does NOT mean "use a default":
            // SkFontMgr_fontconfig stores it as-is, and FontAccessible()
            // unconditionally calls fScanner->scanFile() on every
            // candidate font it hasn't already rejected - a null scanner
            // segfaults there on the first real matchFamilyStyle() call
            // (confirmed via gdb backtrace into FontAccessible on Linux
            // CI). SkFontScanner_Make_FreeType() is Skia's own concrete
            // scanner, matching skia_use_freetype=true in this project's
            // GN build args.
            sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
            return font_mgr->matchFamilyStyle(nullptr, SkFontStyle());
#else
            return nullptr;
#endif
        }();
        return typeface;
    }
}
