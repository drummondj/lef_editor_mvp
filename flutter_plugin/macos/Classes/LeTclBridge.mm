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

namespace {
// Redefines `puts` to capture its text into `::le_console_output` instead
// of writing to the real stdout/stderr channels - deliberately injected
// here as a bootstrap Tcl_Eval, not added to le_tcl_procs.tcl itself:
// that file is also sourced by le_shell.cpp (a real interactive terminal
// program, backend/src/tcl/le_shell.cpp), where `puts` must keep printing
// to the real terminal. Scoping the override to this one embedded
// Tcl_Interp is what makes it safe to redefine a built-in this way at
// all.
//
// A pure-Tcl `rename`+`proc` override rather than a custom C
// Tcl_ChannelType (the more "complete" way to intercept stdout/stderr,
// registered via Tcl_RegisterChannel/Tcl_SetStdChannel) - every realistic
// source of console output in this project's own Tcl surface (user-typed
// commands, le_tcl_procs.tcl, Tcl core's own default `bgerror`) goes
// through the `puts` command, channel argument (stdout/stderr) and all
// (`puts stderr $msg` is still a call to `puts`), so this catches both
// without needing a lower-level channel driver. Extend to a real channel
// intercept only if something is ever found writing to a channel
// directly, bypassing `puts` - not done speculatively.
const char *const kCapturePutsBootstrap = R"tcl(
    set ::le_console_output {}
    rename ::puts ::le_tcl_real_puts
    proc ::puts {args} {
        set noNewline [expr {[lindex $args 0] eq "-nonewline"}]
        append ::le_console_output [lindex $args end]
        if {!$noNewline} {
            append ::le_console_output "\n"
        }
        return {}
    }
)tcl";
}  // namespace

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

    if (Tcl_Eval(_interp, kCapturePutsBootstrap) != TCL_OK) {
      NSLog(@"LeTclBridge: failed to install puts capture: %s", Tcl_GetStringResult(_interp));
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
  // Routed through le_repl_eval (le_tcl_procs.tcl), not a raw Tcl_Eval of
  // `command` itself (UPDATES.md item 21) - the single bracket point that
  // makes a typed console command exactly as undoable (Ctrl-Z/Ctrl-
  // Shift-Z) as a GUI edit, and the source of the command-recall log's
  // success-only filtering. `command` is passed through Tcl_SetVar/a
  // variable reference, not substituted directly into the eval'd string,
  // to avoid re-escaping arbitrary user-typed text - same pattern this
  // file already uses for `le_console_output` below.
  //
  // Tcl_Eval's return code (TCL_OK vs TCL_ERROR) isn't surfaced
  // separately here - le_repl_eval already catches the wrapped command's
  // own error internally and always returns TCL_OK itself, and the
  // interpreter's own string result holds the (successful or error)
  // result either way, exactly what a real interactive Tcl shell would
  // print (see this class's header comment).
  Tcl_SetVar(_interp, "le_pending_command", command.UTF8String, TCL_GLOBAL_ONLY);
  Tcl_Eval(_interp, "le_repl_eval $le_pending_command");
  NSString *result = [NSString stringWithUTF8String:Tcl_GetStringResult(_interp)];

  // Drain whatever `puts` captured during this command (see
  // kCapturePutsBootstrap above) and reset it for the next call - shown
  // ahead of the interpreter's own result, matching the order a real
  // terminal would print them in (output as the command runs, its return
  // value logged last).
  const char *captured = Tcl_GetVar(_interp, "le_console_output", TCL_GLOBAL_ONLY);
  NSString *capturedOutput = captured != nullptr ? [NSString stringWithUTF8String:captured] : @"";
  Tcl_SetVar(_interp, "le_console_output", "", TCL_GLOBAL_ONLY);

  if (capturedOutput.length == 0) {
    return result;
  }
  if (result.length == 0) {
    return capturedOutput;
  }
  return [capturedOutput stringByAppendingString:result];
}

@end
