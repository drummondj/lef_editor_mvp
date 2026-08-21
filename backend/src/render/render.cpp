#include "draw_helpers.hpp"
#include "include/core/SkFontMgr.h"
#include "include/core/SkFontStyle.h"

#include <spdlog/spdlog.h>

#include <string>

// See draw_helpers.hpp's comment on default_typeface() for why this is
// isolated to its own translation unit: ApplicationServices.h (pulled in by
// SkFontMgr_mac_ct.h) defines legacy Carbon Rect/Point/Polygon typedefs
// that collide with le::Rect/le::Point/le::Polygon wherever a file does
// `using namespace le`. SkFontMgr_directory.h doesn't have that problem, but
// it's included here anyway to keep both platforms' font-manager headers in
// the one .cpp that's allowed to know about them, rather than splitting the
// isolation rule across files.
#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(__linux__)
// A bundled font directory (LE_FONT_DIR, injected by backend/CMakeLists.txt
// - defaults to backend/assets/fonts, committed to the repo), not
// SkFontMgr_New_FontConfig - deliberately, for portability to a locked-down
// target machine we can't assume has any working system fontconfig config
// or installed fonts at all (a rootless-Rocky-8-build concern, not just a
// Docker-CI one). A missing/unmatched font under fontconfig fails silently
// (matchFamilyStyle just returns null, every label renders blank, no
// error) - shipping the font file removes that runtime dependency
// entirely, at the cost of only ever having this one bundled family
// available. SkFontMgr_New_Custom_Directory scans LE_FONT_DIR itself for
// .ttf/.ttc/.otf/.pfb files and uses FreeType for rasterization internally
// - no separate SkFontScanner argument needed, unlike the fontconfig path
// this replaced.
#include "include/ports/SkFontMgr_directory.h"
#include <limits.h>
#include <unistd.h>
#ifndef LE_FONT_DIR
#error "LE_FONT_DIR must be set by backend/CMakeLists.txt"
#endif
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
            // Deliberately *not* matchFamilyStyle(nullptr, ...) - unlike
            // CoreText/fontconfig, SkFontMgr_Custom's nullptr handling
            // (SkFontMgr_custom.cpp's onMatchFamilyStyle -> matchFamily ->
            // onMatchFamily) only matches by *name* (empty on no match), and
            // never falls back to the family it separately picks as
            // "default" in its own constructor - which only checks a
            // hardcoded name list ("Arial"/"Verdana"/.../"DejaVu Serif")
            // that doesn't include "DejaVu Sans", this project's own
            // bundled family. Confirmed via gdb: matchFamilyStyle(nullptr,
            // ...) returns null even though the font manager itself loaded
            // the bundled font successfully. createStyleSet(0) sidesteps
            // name-matching entirely - a candidate directory only ever has
            // the one bundled family, so "family at index 0" is unambiguous.
            auto try_font_dir = [](const char* dir) -> sk_sp<SkTypeface>
            {
                sk_sp<SkFontMgr> font_mgr = SkFontMgr_New_Custom_Directory(dir);
                if (font_mgr->countFamilies() > 0)
                {
                    sk_sp<SkFontStyleSet> style_set = font_mgr->createStyleSet(0);
                    if (style_set != nullptr)
                    {
                        return style_set->matchStyle(SkFontStyle());
                    }
                }
                return nullptr;
            };

            // LE_FONT_DIR is a compile-time path (this build machine's own
            // backend/assets/fonts) - correct for ctest/render_preview/local
            // dev, where that source tree genuinely exists, but never valid
            // on a machine a packaged release bundle gets copied to. Fall
            // back to the font bundled next to the running executable
            // instead (flutter_plugin/linux/CMakeLists.txt's own
            // LE_RUNTIME_BUNDLED_FILES bundles backend/assets/fonts/*.ttf
            // into the app's lib/ directory, same place le_tcl.so/
            // le_tcl_procs.tcl land - see lef_editor_plugin.cc's
            // ExecutableDir() for the equivalent Tcl-console fix this
            // mirrors) - confirmed necessary by a real report of every text
            // label rendering blank on a release build.
            sk_sp<SkTypeface> face = try_font_dir(LE_FONT_DIR);
            if (face == nullptr)
            {
                char buf[PATH_MAX];
                ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
                if (len > 0)
                {
                    buf[len] = '\0';
                    std::string exe_path(buf);
                    size_t slash = exe_path.find_last_of('/');
                    std::string exe_dir = (slash == std::string::npos) ? "." : exe_path.substr(0, slash);
                    face = try_font_dir((exe_dir + "/lib").c_str());
                }
            }
            if (face == nullptr)
            {
                // Not a hard failure (matches this function's existing
                // "degrade rather than throw" contract - see draw_helpers.hpp)
                // but every terminal/pin/ruler label silently renders blank
                // if this ever fires, which is otherwise very hard to
                // diagnose on a machine we can't reproduce against directly -
                // see backend/CLAUDE.md's Build section for LE_FONT_DIR.
                spdlog::error("default_typeface(): no usable font found in LE_FONT_DIR ({}) or next to the executable", LE_FONT_DIR);
            }
            return face;
#else
            return nullptr;
#endif
        }();
        return typeface;
    }
}
