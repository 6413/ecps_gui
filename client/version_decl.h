// The client's version, declared only: the values are in version.h, which just ecps_version.cpp reads. A release
// changes version.h, and with it read here (core_state.h, the module ecps.core) every client source was compiled
// again; now only ecps_version.cpp is
#pragma once

extern const char* const ecps_version;
extern const char* const ecps_update_marker;
