// The only source that reads version.h (see version_decl.h). version.h keeps its own form: the updater and the
// release script read "ecps_version = ..." from it as text
#include "version_decl.h"

namespace version_file {
#include "version.h"
}

extern const char* const ecps_version = version_file::ecps_version;
extern const char* const ecps_update_marker = version_file::ecps_update_marker;
