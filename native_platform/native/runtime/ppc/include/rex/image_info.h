// xenon-native: image layout descriptor emitted by codegen (geometrywars_init.cpp).
#pragma once
#include <rex/types.h>
struct PPCFuncMapping;
namespace rex {
struct PPCCodegenFlags {
  bool skip_lr = false;
  bool ctr_as_local = false;
  bool xer_as_local = false;
  bool reserved_as_local = false;
  bool skip_msr = false;
  bool cr_as_local = false;
  bool non_argument_as_local = false;
  bool non_volatile_as_local = false;
};
struct PPCImageInfo {
  u32 code_base;
  u32 code_size;
  u32 image_base;
  u32 image_size;
  const PPCFuncMapping* func_mappings;
  bool rexcrt_heap = false;
  void* register_modules = nullptr;  // unused here; keeps the designated-init order of codegen
  PPCCodegenFlags codegen_flags{};
};
}  // namespace rex
