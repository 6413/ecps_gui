// The system headers the client headers use, read before the client modules (ecps_core.ixx, ecps_app.ixx) so none
// of them lands inside a module. Under clang the sources read them too, before the import: clang reports
// redefinitions (_InlineInterlockedAdd in winnt.h) when a header the module already has is read after the import
#pragma once

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
