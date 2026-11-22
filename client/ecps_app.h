// What a client source includes to use the shared client code: the compiled module ecps.app (ecps_app.ixx) instead
// of reading backend.h, gui.h and the rest again in every file. The macros and the using-directive don't come
// through an import, so they are here
#pragma once

#include "gui_std.h"

#if defined(__clang__)
#include "ecps_system.h"
#endif

import ecps.app;
using namespace fan::graphics;

#include "ecps_macros.h"
