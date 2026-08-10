#import "LeTclBridge.h"

#include <tcl.h>

#include <cstdint>
#include <string>

// LE_TCL_MODULE_PATH/LE_TCL_PROCS_PATH are injected as quoted C string
// literals via GCC_PREPROCESSOR_DEFINITIONS in ../lef_editor_plugin.podspec
// (mirroring how that podspec already hardcodes backend/build_release's
// absolute path elsewhere) - dev-machine functionality only, same
// explicitly-accepted scope limit as every other backend path this
// podspec hardcodes (see this plugin's CLAUDE.md "Open design
// questions" / Packaging).
#ifndef LE_TCL_MODULE_PATH
#error "LE_TCL_MODULE_PATH must be set by lef_editor_plugin.podspec"
#endif
#ifndef LE_TCL_PROCS_PATH
#error "LE_TCL_PROCS_PATH must be set by lef_editor_plugin.podspec"
#endif

@implementation LeTclBridge {
  Tcl_Interp *_interp;
}

- (instancetype)initWithHandleAddress:(int64_t)handleAddress {
  self = [super init];
  if (self) {
    Tcl_FindExecutable(nullptr);
    _interp = Tcl_CreateInterp();
    if (Tcl_Init(_interp) != TCL_OK) {
      NSLog(@"LeTclBridge: Tcl_Init failed: %s", Tcl_GetStringResult(_interp));
    }

    const std::string loadCommand = std::string("load {") + LE_TCL_MODULE_PATH + "} le_tcl";
    if (Tcl_Eval(_interp, loadCommand.c_str()) != TCL_OK) {
      NSLog(@"LeTclBridge: failed to load le_tcl module: %s", Tcl_GetStringResult(_interp));
    }

    const std::string injectCommand =
        "set_session_handle " + std::to_string(handleAddress);
    if (Tcl_Eval(_interp, injectCommand.c_str()) != TCL_OK) {
      NSLog(@"LeTclBridge: set_session_handle failed: %s", Tcl_GetStringResult(_interp));
    }

    if (Tcl_EvalFile(_interp, LE_TCL_PROCS_PATH) != TCL_OK) {
      NSLog(@"LeTclBridge: failed to source le_tcl_procs.tcl: %s", Tcl_GetStringResult(_interp));
    }
  }
  return self;
}

- (void)dealloc {
  if (_interp != nullptr) {
    Tcl_DeleteInterp(_interp);
    _interp = nullptr;
  }
}

- (NSString *)evalTcl:(NSString *)command {
  if (_interp == nullptr) {
    return @"error: Tcl interpreter unavailable";
  }
  // Tcl_Eval's return code (TCL_OK vs TCL_ERROR) isn't surfaced
  // separately here - the interpreter's own string result already holds
  // the error message on failure, exactly what a real interactive Tcl
  // shell would print either way (see this class's header comment).
  Tcl_Eval(_interp, command.UTF8String);
  return [NSString stringWithUTF8String:Tcl_GetStringResult(_interp)];
}

@end
