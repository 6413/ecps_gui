// The render thread and the gui (gui_state.h) as the module ecps.app, on top of ecps.core. A gui edit rebuilds this
// and what imports it; ecps.core and the sources that import only that stay as they are
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

export import ecps.core;

using namespace fan::graphics;

// macros don't come through an import
#include "ecps_macros.h"

// gui_state.h leaves out core_state.h, the import above has it
#define ECPS_APP_MODULE 1

export extern "C++" {
#include "gui_state.h"
}

extern "C++" {
#include "render_thread_ctor.h"
}
