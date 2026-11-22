// The shared client code (gui_state.h: the backend, the gui struct, audio, voice, the updater...) as one module,
// compiled once. The sources import it (ecps_app.h) instead of parsing all those headers each, which made every
// edit to a gui_*.cpp slow.
//
// What it declares stays attached to the global module (extern "C++"), the same entities a plain #include would
// give. The system headers come first, in the global module fragment, so the client headers that include them
// again find them done and nothing of them lands inside the module
module;

#include "gui_std.h"

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <mmsystem.h>
  #include <timeapi.h>
  #include <shellapi.h>
  #include <io.h>
  #include <fcntl.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
  #include <dlfcn.h>
#endif

export module ecps.app;

export import fan;
export import fan.fmt;
export import fan.graphics.video.screen;
export import fan.graphics.stb;

using namespace fan::graphics;

// gui_state.h leaves out its imports, a module has them above
#define ECPS_APP_MODULE 1

export extern "C++" {
#include "gui_state.h"
}

extern "C++" {
#include "render_thread_ctor.h"
}
