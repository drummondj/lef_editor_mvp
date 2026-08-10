#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Objective-C++ wrapper around one embedded Tcl interpreter, sharing the
/// same `LeHandle*` (see backend/src/api/api.hpp) the Dart-owned `LeEditor`
/// already created - see TCL_EXPLORATION.md's show_gui section for the
/// full design rationale.
///
/// Unlike `backend/src/tcl/le_shell.cpp` (a `Tcl_Main`-based standalone
/// binary, owning the whole process and its own blocking event loop),
/// this class embeds Tcl as a plain library: one `Tcl_Interp*`, evaluated
/// synchronously one command at a time via `-evalTcl:`, called from
/// whatever thread the owning method channel handler already runs on
/// (the platform thread - see `LefEditorPlugin.swift`). There is no
/// second event loop here to reconcile with Cocoa's own run loop, which
/// is exactly why this embedding direction was chosen over embedding a
/// FlutterEngine inside a Tcl process instead.
@interface LeTclBridge : NSObject

/// Creates a fresh `Tcl_Interp`, loads the SWIG-built `le_tcl` module,
/// points its session at `handleAddress` via `set_session_handle` (see
/// le_tcl_shim.hpp) so every CRUD/search command mutates the exact same
/// database the GUI is already rendering, then sources
/// `le_tcl_procs.tcl` for the `-flag value` ergonomic command layer.
/// Never destroys `handleAddress` - ownership stays with whoever created
/// it (the Dart-owned `LeEditor`), same as `le_tcl_shim.cpp`'s own
/// injected-handle contract.
- (instancetype)initWithHandleAddress:(int64_t)handleAddress NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

/// Evaluates one Tcl command synchronously against this bridge's
/// interpreter, returning the interpreter's string result whether the
/// command succeeded or failed (Tcl already puts the error message in
/// the same place on `TCL_ERROR` - a caller doesn't need a different
/// path for errors, matching what a real interactive shell prints
/// either way).
- (NSString *)evalTcl:(NSString *)command;

@end

NS_ASSUME_NONNULL_END
