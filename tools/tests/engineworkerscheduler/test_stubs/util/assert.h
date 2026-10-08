#pragma once

#include <cassert>

// Isolate assertion support from the product logging/GUI dependencies.
#define DEBUG_ASSERT(condition) assert(condition)
#define VERIFY_OR_DEBUG_ASSERT(condition) if (!(condition))
