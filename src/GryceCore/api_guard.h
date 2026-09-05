#ifndef GRYCE_API_GUARD_H
#define GRYCE_API_GUARD_H

#include "core_api.h"

#ifdef __cplusplus
#include <mutex>

namespace gryce_core {

// One API-wide recursive mutex shared by every exported C API entry point
// across all Gryce DLLs. The editor runs a dedicated render thread (up to the
// monitor refresh rate) alongside the UI thread (60Hz tick, input, hierarchy
// edits), so scene data must not be mutated while the render pipeline
// iterates it. Recursive so API functions may call each other and so callbacks
// fired while holding the lock can re-enter on the same thread (the editor's
// Dispatcher callbacks run on the UI thread). Defined in core/api/core_api.cpp.
GRYCE_CORE_API std::recursive_mutex& api_mutex();

} // namespace gryce_core

// RAII guard for exported C API entry points.
#define GRYCE_API_GUARD() \
    std::lock_guard<std::recursive_mutex> _gryce_api_guard_(gryce_core::api_mutex())

#endif // __cplusplus

#endif // GRYCE_API_GUARD_H
