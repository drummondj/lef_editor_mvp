import Cocoa
import FlutterMacOS

/// Method channel name - must match `_kChannelName` in
/// `lib/lef_editor_plugin.dart`.
private let kChannelName = "lef_editor_plugin"

/// Registers the `lef_editor_plugin` method channel used to bridge a
/// Dart-owned `LeHandle*` (see `lib/lef_editor_plugin.dart`'s `LeEditor`)
/// into a native `FlutterTexture` (see `LeTexture.swift`). Dart FFI can't
/// reach the texture registry itself - only platform embedder code can -
/// so this channel exists purely to hand a texture id back and forth; the
/// actual per-frame pixel pull never crosses it (see this plugin's
/// CLAUDE.md's Architecture section).
public class LefEditorPlugin: NSObject, FlutterPlugin {
  private let textureRegistry: FlutterTextureRegistry
  private var textures: [Int64: LeTexture] = [:]

  init(textureRegistry: FlutterTextureRegistry) {
    self.textureRegistry = textureRegistry
  }

  public static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(name: kChannelName, binaryMessenger: registrar.messenger)
    let instance = LefEditorPlugin(textureRegistry: registrar.textures)
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    guard let args = call.arguments as? [String: Any] else {
      result(FlutterError(code: "bad_args", message: "\(call.method) requires a map of arguments", details: nil))
      return
    }

    switch call.method {
    case "createTexture":
      guard let handleAddress = (args["handleAddress"] as? NSNumber)?.int64Value else {
        result(FlutterError(code: "bad_args", message: "createTexture requires handleAddress", details: nil))
        return
      }
      let texture = LeTexture(handleAddress: handleAddress)
      let textureId = textureRegistry.register(texture)
      textures[textureId] = texture
      result(textureId)

    case "markTextureFrameAvailable":
      guard let textureId = (args["textureId"] as? NSNumber)?.int64Value else {
        result(FlutterError(code: "bad_args", message: "markTextureFrameAvailable requires textureId", details: nil))
        return
      }
      textureRegistry.textureFrameAvailable(textureId)
      result(nil)

    case "disposeTexture":
      guard let textureId = (args["textureId"] as? NSNumber)?.int64Value else {
        result(FlutterError(code: "bad_args", message: "disposeTexture requires textureId", details: nil))
        return
      }
      textureRegistry.unregisterTexture(textureId)
      textures.removeValue(forKey: textureId)
      result(nil)

    default:
      result(FlutterMethodNotImplemented)
    }
  }
}
