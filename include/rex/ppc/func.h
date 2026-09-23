/**
 * @file        ppc/func.h
 * @brief       Recompiled PPC function signature, mapping table, and definition macros
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Split out of rex/ppc/context.h so callers that only need the
 *              PPCFunc type do not pull in PPCContext and its SIMD headers.
 */

#pragma once

#include <cstddef>
#include <cstdint>

//=============================================================================
// PPCFunc Type Definition
//=============================================================================
// Function signature for recompiled PPC functions.
// All recompiled functions take a context reference and memory base pointer.

// Forward declaration of the PPC execution context
struct PPCContext;

// Function signature for recompiled PPC functions
using PPCFunc = void(PPCContext& ctx, uint8_t* base);

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t guest_address);

// Logs how often indirect calls missed their module's dispatch table. Called
// once at shutdown; see the counters in system/function_dispatcher.cpp.
void ReportIndirectDispatchStats();
}  // namespace rex::runtime

//=============================================================================
// PPC Function Macros
//=============================================================================

#define REX_JOIN(x, y) x##y
#define REX_XSTRINGIFY(x) #x
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
// GNU ld's DLL auto-export deliberately skips every symbol whose name starts
// with "__imp_", because on PE that prefix names an import thunk. ReXGlue's
// kernel hooks are all called __imp__<Export>, so on a MinGW build the runtime
// DLL would export none of them and every title would fail to link. An
// explicit dllexport is honoured regardless of that filter, so mark the
// definitions while the runtime itself is being compiled. Inert everywhere
// else, including in titles that use the same macro for their own functions.
#if defined(_WIN32) && defined(rexruntime_EXPORTS)
#define REX_FUNC_LINKAGE __declspec(dllexport)
#else
#define REX_FUNC_LINKAGE
#endif

// REX_FUNC is the bare signature; the declaring site supplies linkage, either
// by writing extern "C" itself or by using REX_EXTERN.
#define REX_FUNC(x) \
  REX_FUNC_LINKAGE void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)

//=============================================================================
// Function Mapping
//=============================================================================

struct PPCFuncMapping {
  size_t guest;
  PPCFunc* host;
};

extern PPCFuncMapping PPCFuncMappings[];
