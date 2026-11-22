// The client's shared code without the gui (core_state.h) as the module ecps.core, compiled once. Its declarations
// stay attached to the global module (extern "C++"), the same entities a plain #include gives. The system headers
// come first, in the global module fragment, so nothing of them lands inside the module
module;

#include "gui_std.h"

#include "ecps_system.h"

export module ecps.core;

export import fan;
export import fan.fmt;
export import fan.graphics.video.screen;
export import fan.graphics.stb;

using namespace fan::graphics;

// core_state.h leaves out its imports, a module has them above
#define ECPS_APP_MODULE 1

export extern "C++" {
#include "core_state.h"
}
