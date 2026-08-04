//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <lef_editor_plugin/lef_editor_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) lef_editor_plugin_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "LefEditorPlugin");
  lef_editor_plugin_register_with_registrar(lef_editor_plugin_registrar);
}
