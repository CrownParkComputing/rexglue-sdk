/**
 * @file        hook.h
 * @brief       Hook authoring API: macros, typed imports, RAII helpers
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstring>
#include <tuple>
#include <type_traits>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/types.h>

//=============================================================================
// Hook Macros
//=============================================================================

namespace rex {
// Defined in system/import_trace.cpp. Records an import the first time it runs
// when REX_TRACE_IMPORTS names an output file; the per-call cost with tracing
// off is one load of a function-local static and a predictable branch.
bool ImportTraceEnabled();
void NoteImportUsed(const char* name);
}  // namespace rex

// A title's import table says what it might call; this says what it did. That
// shorter list is the work list for giving one title a native implementation
// of the kernel surface it actually uses.
#define REX_TRACE_IMPORT(subroutine_name)      \
  do {                                         \
    static bool rex_import_seen_ = false;      \
    if (!rex_import_seen_) {                   \
      rex_import_seen_ = true;                 \
      ::rex::NoteImportUsed(subroutine_name);  \
    }                                          \
  } while (0)

// Hook a recompiled function with an auto-marshaled native C++ function.
// The native function uses plain types (u32, mapped_u32, etc.) and
// HostToGuestFunction handles register translation automatically.
#ifdef REXGLUE_ENABLE_PROFILING
#include <tracy/Tracy.hpp>
#define REX_HOOK(subroutine, function)                           \
  extern "C" REX_FUNC(subroutine) {                              \
    ZoneNamedN(___tracy_hook_zone, #subroutine, TracyIsStarted); \
    REX_TRACE_IMPORT(#subroutine);                               \
    rex::ppc::HostToGuestFunction<function>(ctx, base);          \
  }
#else
#define REX_HOOK(subroutine, function)                  \
  extern "C" REX_FUNC(subroutine) {                     \
    REX_TRACE_IMPORT(#subroutine);                      \
    rex::ppc::HostToGuestFunction<function>(ctx, base); \
  }
#endif

// Define a raw hook with direct ctx/base access.
#define REX_HOOK_RAW(name) extern "C" REX_FUNC(name)

// Stub: logs a warning when called.
#define REX_STUB(subroutine)              \
  extern "C" REX_FUNC(subroutine) {       \
    (void)base;                           \
    REX_TRACE_IMPORT(#subroutine);        \
    REXKRNL_WARN("{} STUB", #subroutine); \
  }

#define REX_STUB_LOG(subroutine, msg)               \
  extern "C" REX_FUNC(subroutine) {                 \
    (void)base;                                     \
    REXKRNL_WARN("{} STUB - {}", #subroutine, msg); \
  }

#define REX_STUB_RETURN(subroutine, value)                                                \
  extern "C" REX_FUNC(subroutine) {                                                       \
    (void)base;                                                                           \
    REXKRNL_WARN("{} STUB - returning {:#x}", #subroutine, static_cast<uint32_t>(value)); \
    ctx.r3.u64 = (value);                                                                 \
  }

// Kernel exports are WEAK definitions. A port that wants to answer one import
// with its own native code defines the same symbol in its own sources and the
// strong definition wins, with no SDK edit and no duplicate-symbol error - in a
// static link as well as a shared one, which is what makes a per-title native
// kernel possible at all. REX_NATIVE_HOOK in the port is the other half.
#if defined(_WIN32)
// MSVC has no weak definitions; a port overriding an import there needs
// /FORCE:MULTIPLE or an SDK edit instead. MinGW does spell them, but PE has
// no weak symbol: GNU ld models one as a weak external with a default alias,
// and refuses to dllexport that ("symbol wrong type (2 vs 3)") - so a weak
// hook would cost the runtime DLL every one of its ~2,900 exports. On Windows
// the hooks are strong, and a title's REX_NATIVE_HOOK still wins for the
// title's own calls because the linker takes the exe's object definition
// before it searches the import library.
#define REX_WEAK_EXPORT
#else
#define REX_WEAK_EXPORT __attribute__((weak))
#endif

// Export: hook + register in global registry for kernel ordinal lookup.
#define REX_EXPORT(name, function)                                             \
  extern "C" REX_WEAK_EXPORT REX_FUNC(name) {                                  \
    REX_TRACE_IMPORT(#name);                                                   \
    rex::ppc::HostToGuestFunction<function>(ctx, base);                        \
  }                                                                            \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

#define REX_EXPORT_STUB(name)                                                  \
  extern "C" REX_WEAK_EXPORT REX_FUNC(name) {                                  \
    (void)base;                                                                \
    REX_TRACE_IMPORT(#name);                                                   \
    REXKRNL_WARN("{} STUB", #name);                                            \
  }                                                                            \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

#define REX_EXPORT_STUB_RETURN(name, retval)                                   \
  extern "C" REX_WEAK_EXPORT REX_FUNC(name) {                                  \
    (void)base;                                                                \
    REX_TRACE_IMPORT(#name);                                                   \
    REXKRNL_WARN("{} STUB - returning {:#x}", #name,                           \
                 static_cast<uint32_t>(retval));                               \
    ctx.r3.u64 = (retval);                                                     \
  }                                                                            \
  static rex::ppc::detail::PPCFuncRegistrar _ppc_reg_##name(#name, &name);

// Author a native replacement for a kernel import in a port. Same signature as
// REX_HOOK_RAW; the name is the import, e.g. REX_NATIVE_HOOK(__imp__XGetLanguage).
#define REX_NATIVE_HOOK(name) extern "C" REX_FUNC(name)

namespace rex {

//=============================================================================
// CallFrame - Lightweight isolated calling context
//=============================================================================
// For side calls that must not disturb the outer hook's register state.
// Stack-allocated, NOT zero-initialized. Only copies the registers a callee
// actually needs from the parent context.

struct CallFrame {
  PPCContext ctx;
  PPCContext& parent_;

  explicit CallFrame(PPCContext& parent) : parent_(parent) {
    ctx.r1 = parent.r1;
    ctx.r13 = parent.r13;
    ctx.fpscr = parent.fpscr;
  }

  ~CallFrame() { parent_.fpscr = ctx.fpscr; }

  operator PPCContext&() { return ctx; }

  CallFrame(const CallFrame&) = delete;
  CallFrame& operator=(const CallFrame&) = delete;
};

}  // namespace rex

namespace rex::ppc {

//=============================================================================
// ImportFunction - Zero-overhead typed callable for recompiled functions
//=============================================================================

template <typename S>
struct ImportFunction;

template <typename R, typename... Args>
struct ImportFunction<R(Args...)> {
  PPCFunc& fn;

  R operator()(PPCContext& ctx, uint8_t* base, Args... args) const {
    auto tpl = std::make_tuple(args...);
    _translate_args_to_guest(ctx, base, tpl);
    fn(ctx, base);
    if constexpr (std::is_void_v<R>) {
      return;
    } else if constexpr (is_precise_v<R>) {
      return static_cast<R>(ctx.f1.f64);
    } else {
      return static_cast<R>(ctx.r3.u64);
    }
  }

  R operator()(rex::CallFrame& frame, uint8_t* base, Args... args) const {
    return (*this)(frame.ctx, base, args...);
  }

  /// Auto-isolating call: retrieves ctx/base internally, creates isolated
  /// context with 0x70 frame, calls function, returns result.
  R operator()(Args... args) const {
    auto* ts = rex::runtime::ThreadState::Get();
    PPCContext* parentCtx = ts->context();

    auto* ks = rex::system::kernel_state();
    uint8_t* base = ks->memory()->virtual_membase();

    PPCContext ctx{};
    ctx.r1 = parentCtx->r1;
    ctx.r1.u32 -= 0x70;
    ctx.r13 = parentCtx->r13;
    ctx.fpscr = parentCtx->fpscr;

    auto tpl = std::make_tuple(args...);
    _translate_args_to_guest(ctx, base, tpl);
    fn(ctx, base);

    parentCtx->fpscr = ctx.fpscr;

    if constexpr (std::is_void_v<R>) {
      return;
    } else if constexpr (is_precise_v<R>) {
      return static_cast<R>(ctx.f1.f64);
    } else {
      return static_cast<R>(ctx.r3.u64);
    }
  }
};

}  // namespace rex::ppc

namespace rex {

//=============================================================================
// StackFrame - RAII guest stack allocation
//=============================================================================

class [[deprecated("Use rex::ppc::stack_push / stack_guard instead")]] StackFrame {
  PPCContext& ctx_;
  u32 size_;

 public:
  explicit StackFrame(PPCContext& ctx, u32 size) : ctx_(ctx), size_(size) { ctx_.r1.u32 -= size; }

  ~StackFrame() { ctx_.r1.u32 += size_; }

  u32 addr() const { return ctx_.r1.u32; }
  u32 addr(u32 offset) const { return ctx_.r1.u32 + offset; }

  void write_string(u32 offset, const char* str, u8* base) const {
    std::memcpy(rex::memory::GuestPtr(base, addr(offset)), str, std::strlen(str) + 1);
  }

  void write(u32 offset, const void* data, size_t len, u8* base) const {
    std::memcpy(rex::memory::GuestPtr(base, addr(offset)), data, len);
  }

  template <typename... A>
  size_t write_fmt(u32 offset, u8* base, fmt::format_string<A...> fmtstr, A&&... args) const {
    char* dst = rex::memory::GuestPtr<char*>(base, addr(offset));
    auto result = fmt::format_to_n(dst, size_ - offset - 1, fmtstr, std::forward<A>(args)...);
    *result.out = '\0';
    return result.size;
  }

  StackFrame(const StackFrame&) = delete;
  StackFrame& operator=(const StackFrame&) = delete;
};

}  // namespace rex

//=============================================================================
// REX_IMPORT - Typed callable import of a recompiled function
//=============================================================================
// Three explicit arguments, no hidden prefix transformations:
//   symbol:   the exact linker symbol to reference
//   callable: the name of the typed callable variable
//   sig:      the function signature (e.g. u32(u32, u32))
//
// The callable is internal-linkage on purpose: on ELF, a namespace-scope C++
// variable is not name-mangled, so an external-linkage callable named after a
// guest function would collide with the generated weak function alias and
// hijack its function table entry (MSVC decorates variables, hiding the bug).

#define REX_IMPORT(symbol, callable, sig)                          \
  REX_EXTERN(symbol);                                              \
  [[maybe_unused]] static rex::ppc::ImportFunction<sig> callable { \
    symbol                                                         \
  }

//=============================================================================
// Legacy Compat Aliases
//=============================================================================

#define XBOXKRNL_EXPORT(n, f) REX_EXPORT(n, f)
#define XBOXKRNL_EXPORT_STUB(n) REX_EXPORT_STUB(n)
#define XAM_EXPORT(n, f) REX_EXPORT(n, f)
#define XAM_EXPORT_STUB(n) REX_EXPORT_STUB(n)
#define REXCRT_EXPORT(n, f) REX_HOOK(n, f)
#define REXCRT_EXPORT_STUB(n) REX_STUB(n)
