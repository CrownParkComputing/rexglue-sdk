// xenon-native shim for <rex/cvar.h>: the translator reads a handful of knobs
// through REXCVAR_GET(); there is no registry, just static storage.
#pragma once
#include <cstdint>
#include <string>
#define REXCVAR_DECLARE(type, name) type& FLAGS_##name##_storage_()
#define REXCVAR_GET(name) (FLAGS_##name##_storage_())
#define REXCVAR_SET(name, value) (FLAGS_##name##_storage_() = (value))
#define XN_CVAR_DEFINE_(type, name, default_val) \
  type& FLAGS_##name##_storage_() {             \
    static type storage = (default_val);        \
    return storage;                             \
  }
#define REXCVAR_DEFINE_BOOL(name, default_val, category, desc) XN_CVAR_DEFINE_(bool, name, default_val)
#define REXCVAR_DEFINE_INT32(name, default_val, category, desc) XN_CVAR_DEFINE_(int32_t, name, default_val)
#define REXCVAR_DEFINE_UINT32(name, default_val, category, desc) XN_CVAR_DEFINE_(uint32_t, name, default_val)
#define REXCVAR_DEFINE_STRING(name, default_val, category, desc) XN_CVAR_DEFINE_(std::string, name, default_val)
