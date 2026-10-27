// xenon-native: import/hook authoring macros.
//
// Every kernel import the title calls is a symbol named __imp__<Name> with the
// PPCFunc signature. XN_IMPORT marshals a plain C++ function; XN_IMPORT_RAW gets
// the raw context. XN_STUB logs once and returns 0. Definitions are strong: an
// import defined twice is a link error, which is what we want in a runtime that
// is supposed to know exactly which imports it answers.
#pragma once
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/types.h>

// ---------------------------------------------------------------------------
// Writing an import handler: two rules that are silent when you get them wrong.
//
// 1. ARGUMENT WIDTH. A 64-bit guest argument occupies ONE register, because a
//    guest register is 64 bits wide. Declaring it as two 32-bit parameters
//    shifts every later argument along by one register - so the last ones land
//    on the CALLER'S STACK FRAME, and writing through an out-pointer that
//    landed there corrupts the caller's locals. Declare it `u64`:
//
//      // XamUserCreateAchievementEnumerator: EIGHT arguments, xuid is r5 alone
//      u32 f(u32 title_id, u32 user_index, u64 xuid, u32 flags, u32 offset,
//            u32 count, mapped_u32 buffer_size_ptr, mapped_u32 handle_ptr);
//
//      // WRONG - splits the xuid, pushes handle_ptr onto the caller's stack
//      u32 f(u32 title_id, u32 user_index, u32 xuid_lo, u32 xuid_hi, ...);
//
//    This exact mistake gave Geometry Wars 2 a 1 GB NtAllocateVirtualMemory and
//    a fatal RtlRaiseException in about half of all boots. Arguments 9 and
//    beyond live at `r1 + 0x54 + (arg - 8) * 8` (see ArgTranslator in
//    rex/ppc/function.h); confirm an arity against the guest caller's register
//    setup before trusting a signature from anywhere else.
//
// 2. ANSWERING MEANS WRITING THE OUTPUT. Returning a status and leaving an
//    output buffer untouched hands the caller whatever was in that memory
//    before - usually its own stack - and many callers do not check the status
//    at all. Write every out-parameter on EVERY path, failure paths included;
//    zero is a defined "none", stale memory is not. If a buffer's layout is
//    unknown, zero what you can and log loudly that you could not.
// ---------------------------------------------------------------------------

#define XN_IMPORT(name, function)                       \
  extern "C" REX_FUNC(name) {                           \
    rex::ppc::HostToGuestFunction<function>(ctx, base); \
  }
#define XN_IMPORT_RAW(name) extern "C" REX_FUNC(name)
#define XN_STUB(name)                                                 \
  extern "C" REX_FUNC(name) {                                         \
    (void)base;                                                       \
    static bool warned = false;                                       \
    if (!warned) { warned = true; REXKRNL_WARN("{} STUB (returns 0)", #name); } \
    ctx.r3.u64 = 0;                                                   \
  }
#define XN_STUB_RETURN(name, value)                                   \
  extern "C" REX_FUNC(name) {                                         \
    (void)base;                                                       \
    static bool warned = false;                                       \
    if (!warned) { warned = true; REXKRNL_WARN("{} STUB (returns {:#x})", #name, (uint32_t)(value)); } \
    ctx.r3.u64 = (value);                                             \
  }
