#pragma once

#include "ecps_app.h"

#include <WITCH/WITCH.h>

#if __has_include("cuda.h")
#include <cuda.h>
#endif

#include <fan/event/types.h>
#include <fan/graphics/types.h>
#include <fan/nativefiledialog/nfd.h>

#if defined(_WIN32)
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#else
#include <dlfcn.h>
#endif
