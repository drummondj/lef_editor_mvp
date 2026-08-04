#include "include/lef_editor_plugin/lef_editor_plugin.h"

#include <flutter_linux/flutter_linux.h>

#include <cstring>

#include "lef_texture.h"

// Registers the "lef_editor_plugin" method channel used to bridge a
// Dart-owned LeHandle* (see ../lib/lef_editor_plugin.dart's LeEditor) into
// a native FlLeTexture (see lef_texture.h/.cc). Dart FFI can't reach the
// texture registrar itself - only platform embedder code can - so this
// channel exists purely to hand a texture id back and forth; the actual
// per-frame pixel pull never crosses it. Mirrors
// ../macos/Classes/LefEditorPlugin.swift's protocol exactly:
//   createTexture({handleAddress: int}) -> int textureId
//   markTextureFrameAvailable({textureId: int}) -> null
//   disposeTexture({textureId: int}) -> null

#define LEF_EDITOR_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), lef_editor_plugin_get_type(), LefEditorPlugin))

struct _LefEditorPlugin {
  GObject parent_instance;

  FlTextureRegistrar* texture_registrar;
  // Keyed by texture id (as returned by fl_texture_get_id) - keeps each
  // FlLeTexture alive between createTexture and disposeTexture, mirroring
  // LefEditorPlugin.swift's own `textures` dictionary.
  GHashTable* textures;
};

G_DEFINE_TYPE(LefEditorPlugin, lef_editor_plugin, g_object_get_type())

static FlMethodResponse* handle_create_texture(LefEditorPlugin* self, FlValue* args) {
  FlValue* handle_address_value = fl_value_lookup_string(args, "handleAddress");
  if (handle_address_value == nullptr ||
      fl_value_get_type(handle_address_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "bad_args", "createTexture requires handleAddress", nullptr));
  }

  FlLeTexture* texture = fl_le_texture_new(fl_value_get_int(handle_address_value));
  if (!fl_texture_registrar_register_texture(self->texture_registrar,
                                             FL_TEXTURE(texture))) {
    g_object_unref(texture);
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("register_failed", "register_texture failed", nullptr));
  }

  int64_t texture_id = fl_texture_get_id(FL_TEXTURE(texture));
  // Table owns the reference from here on; released in handle_dispose_texture.
  g_hash_table_insert(self->textures, GINT_TO_POINTER(texture_id), texture);

  g_autoptr(FlValue) result = fl_value_new_int(texture_id);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static FlMethodResponse* handle_mark_texture_frame_available(LefEditorPlugin* self,
                                                              FlValue* args) {
  FlValue* texture_id_value = fl_value_lookup_string(args, "textureId");
  if (texture_id_value == nullptr || fl_value_get_type(texture_id_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(fl_method_error_response_new(
        "bad_args", "markTextureFrameAvailable requires textureId", nullptr));
  }

  int64_t texture_id = fl_value_get_int(texture_id_value);
  FlLeTexture* texture =
      FL_LE_TEXTURE(g_hash_table_lookup(self->textures, GINT_TO_POINTER(texture_id)));
  if (texture == nullptr) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("unknown_texture", "unknown textureId", nullptr));
  }

  fl_texture_registrar_mark_texture_frame_available(self->texture_registrar, FL_TEXTURE(texture));
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static FlMethodResponse* handle_dispose_texture(LefEditorPlugin* self, FlValue* args) {
  FlValue* texture_id_value = fl_value_lookup_string(args, "textureId");
  if (texture_id_value == nullptr || fl_value_get_type(texture_id_value) != FL_VALUE_TYPE_INT) {
    return FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "disposeTexture requires textureId", nullptr));
  }

  int64_t texture_id = fl_value_get_int(texture_id_value);
  FlLeTexture* texture =
      FL_LE_TEXTURE(g_hash_table_lookup(self->textures, GINT_TO_POINTER(texture_id)));
  if (texture != nullptr) {
    fl_texture_registrar_unregister_texture(self->texture_registrar, FL_TEXTURE(texture));
    g_hash_table_remove(self->textures, GINT_TO_POINTER(texture_id));
  }
  return FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                           gpointer user_data) {
  LefEditorPlugin* self = LEF_EDITOR_PLUGIN(user_data);

  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  g_autoptr(FlMethodResponse) response = nullptr;
  if (args == nullptr || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
    response = FL_METHOD_RESPONSE(
        fl_method_error_response_new("bad_args", "expected a map of arguments", nullptr));
  } else if (strcmp(method, "createTexture") == 0) {
    response = handle_create_texture(self, args);
  } else if (strcmp(method, "markTextureFrameAvailable") == 0) {
    response = handle_mark_texture_frame_available(self, args);
  } else if (strcmp(method, "disposeTexture") == 0) {
    response = handle_dispose_texture(self, args);
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond(method_call, response, &error)) {
    g_warning("Failed to send lef_editor_plugin response: %s", error->message);
  }
}

static void lef_editor_plugin_dispose(GObject* object) {
  LefEditorPlugin* self = LEF_EDITOR_PLUGIN(object);
  g_clear_pointer(&self->textures, g_hash_table_unref);
  g_clear_object(&self->texture_registrar);
  G_OBJECT_CLASS(lef_editor_plugin_parent_class)->dispose(object);
}

static void lef_editor_plugin_class_init(LefEditorPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = lef_editor_plugin_dispose;
}

static void lef_editor_plugin_init(LefEditorPlugin* self) {
  self->textures = g_hash_table_new_full(g_direct_hash, g_direct_equal, nullptr, g_object_unref);
}

void lef_editor_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  LefEditorPlugin* plugin =
      LEF_EDITOR_PLUGIN(g_object_new(lef_editor_plugin_get_type(), nullptr));
  plugin->texture_registrar =
      FL_TEXTURE_REGISTRAR(g_object_ref(fl_plugin_registrar_get_texture_registrar(registrar)));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar), "lef_editor_plugin", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, method_call_cb, g_object_ref(plugin),
                                            g_object_unref);

  g_object_unref(plugin);
}
