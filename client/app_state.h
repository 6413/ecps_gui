#pragma once

// a source that doesn't use the gui defines ECPS_CORE_ONLY first and imports only ecps.core
#ifdef ECPS_CORE_ONLY
#include "ecps_core.h"
#else
#include "ecps_app.h"
#endif

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
