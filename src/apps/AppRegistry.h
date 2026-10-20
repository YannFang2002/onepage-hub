#pragma once

#include <cstddef>

#include "AppDescriptor.h"

// Returns the desktop app table in display order (first app = first tile).
// `count` receives the number of entries.
const AppDescriptor* appList(size_t& count);

// Returns the descriptor for `id`, or nullptr when it is not registered.
const AppDescriptor* findApp(AppId id);
