// xenon-native: host<->guest calling-convention marshalling.
// The ArgTranslator / HostToGuestFunction shape is ReXGlue's (BSD-3, based on
// XenonRecomp/UnleashedRecomp); the thread/base plumbing is xenon-native's own.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <tuple>
#include <type_traits>
#include <utility>

#include <rex/ppc/context.h>
#include <rex/types.h>

namespace rex::memory {
template <typename T>
inline T GuestPtr(uint8_t* base, uint32_t guest_addr) {
  return reinterpret_cast<T>(base + guest_addr);
}
}  // namespace rex::memory

namespace xn {
// Implemented by runtime/kernel/threads.cpp: the PPCContext of the calling host thread.
PPCContext* CurrentContext();
uint8_t* GuestBase();
}  // namespace xn

namespace rex::ppc {

template <typename R, typename... T>
constexpr std::tuple<T...> function_args(R (*)(T...)) noexcept {
  return std::tuple<T...>();
}

template <typename T>
static constexpr bool is_precise_v = std::is_same_v<T, float> || std::is_same_v<T, double>;

template <typename T>
concept BigEndianType = is_be_type_v<T>;
template <typename T>
concept MappedPtrType = is_mapped_ptr_v<T>;
template <typename T>
concept PlainType = !std::is_pointer_v<T> && !BigEndianType<T> && !MappedPtrType<T>;

struct ArgTranslator {
  static constexpr uint64_t GetIntegerArgumentValue(const PPCContext& ctx, uint8_t* base,
                                                    size_t arg) noexcept {
    switch (arg) {
      case 0: return ctx.r3.u64;
      case 1: return ctx.r4.u64;
      case 2: return ctx.r5.u64;
      case 3: return ctx.r6.u64;
      case 4: return ctx.r7.u64;
      case 5: return ctx.r8.u64;
      case 6: return ctx.r9.u64;
      case 7: return ctx.r10.u64;
      default: break;
    }
    // Stack arguments live at r1 + 0x54 + ((arg - 8) * 8).
    return __builtin_bswap32(
        *rex::memory::GuestPtr<uint32_t*>(base, ctx.r1.u32 + 0x54 + ((arg - 8) * 8)));
  }
  static double GetPrecisionArgumentValue(const PPCContext& ctx, size_t arg) noexcept {
    const PPCRegister* f[] = {&ctx.f1, &ctx.f2,  &ctx.f3,  &ctx.f4,  &ctx.f5,  &ctx.f6, &ctx.f7,
                              &ctx.f8, &ctx.f9,  &ctx.f10, &ctx.f11, &ctx.f12, &ctx.f13};
    return arg < 13 ? f[arg]->f64 : 0.0;
  }
  static constexpr void SetIntegerArgumentValue(PPCContext& ctx, uint8_t* base, size_t arg,
                                                uint64_t value) noexcept {
    switch (arg) {
      case 0: ctx.r3.u64 = value; return;
      case 1: ctx.r4.u64 = value; return;
      case 2: ctx.r5.u64 = value; return;
      case 3: ctx.r6.u64 = value; return;
      case 4: ctx.r7.u64 = value; return;
      case 5: ctx.r8.u64 = value; return;
      case 6: ctx.r9.u64 = value; return;
      case 7: ctx.r10.u64 = value; return;
      default: break;
    }
    *rex::memory::GuestPtr<uint32_t*>(base, ctx.r1.u32 + 0x54 + ((arg - 8) * 8)) =
        __builtin_bswap32(static_cast<uint32_t>(value));
  }
  static void SetPrecisionArgumentValue(PPCContext& ctx, size_t arg, double value) noexcept {
    PPCRegister* f[] = {&ctx.f1, &ctx.f2,  &ctx.f3,  &ctx.f4,  &ctx.f5,  &ctx.f6, &ctx.f7,
                        &ctx.f8, &ctx.f9,  &ctx.f10, &ctx.f11, &ctx.f12, &ctx.f13};
    if (arg < 13) f[arg]->f64 = value;
  }

  template <BigEndianType T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    T result;
    result.value = static_cast<decltype(result.value)>(GetIntegerArgumentValue(ctx, base, idx));
    return result;
  }
  template <MappedPtrType T>
  static T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    using inner_t = typename mapped_ptr_inner_type<T>::type;
    const auto v = uint32_t(GetIntegerArgumentValue(ctx, base, idx));
    if (!v) return T(nullptr);
    uint32_t guest_addr = static_cast<uint32_t>(v);
    return T(rex::memory::GuestPtr<inner_t*>(base, guest_addr), guest_addr);
  }
  template <PlainType T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    if constexpr (is_precise_v<T>) {
      return static_cast<T>(GetPrecisionArgumentValue(ctx, idx));
    } else {
      if constexpr (sizeof(T) == 8) {
        if (idx >= 8) return static_cast<T>(__builtin_bswap64(
            *rex::memory::GuestPtr<uint64_t*>(base, ctx.r1.u32 + 0x50 + (idx - 8) * 8)));
      }
      return static_cast<T>(GetIntegerArgumentValue(ctx, base, idx));
    }
  }
  template <typename T>
    requires std::is_pointer_v<T>
  static constexpr T GetValue(PPCContext& ctx, uint8_t* base, size_t idx) noexcept {
    const auto v = uint32_t(GetIntegerArgumentValue(ctx, base, idx));
    if (!v) return nullptr;
    return rex::memory::GuestPtr<T>(base, static_cast<uint32_t>(v));
  }
  template <typename T>
  static constexpr void SetValue(PPCContext& ctx, uint8_t* base, size_t idx, T value) noexcept {
    if constexpr (is_precise_v<T>) {
      SetPrecisionArgumentValue(ctx, idx, value);
    } else if constexpr (std::is_null_pointer_v<T>) {
      SetIntegerArgumentValue(ctx, base, idx, 0);
    } else if constexpr (std::is_pointer_v<T>) {
      SetIntegerArgumentValue(ctx, base, idx,
                              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value) -
                                                    reinterpret_cast<uintptr_t>(base)));
    } else {
      if constexpr (sizeof(T) == 8) {
        if (idx >= 8) {
          *rex::memory::GuestPtr<uint64_t*>(base, ctx.r1.u32 + 0x50 + (idx - 8) * 8) = __builtin_bswap64(static_cast<uint64_t>(value));
          return;
        }
      }
      SetIntegerArgumentValue(ctx, base, idx, static_cast<uint64_t>(value));
    }
  }
};

struct Argument {
  int type{};
  int ordinal{};
};

template <typename... Args>
constexpr std::array<Argument, sizeof...(Args)> GatherFunctionArgumentsFromTypes() {
  std::array<Argument, sizeof...(Args)> args{};
  if constexpr (sizeof...(Args) > 0) {
    int floatOrdinal{};
    int intOrdinal{};
    size_t i{};
    (
        [&]<typename T>() {
          if constexpr (is_precise_v<T>) {
            args[i] = {1, floatOrdinal++};
          } else {
            args[i] = {0, intOrdinal++};
          }
          i++;
        }.template operator()<Args>(),
        ...);
  }
  return args;
}
template <typename R, typename... Args>
constexpr auto GatherFromSignature(R (*)(Args...)) {
  return GatherFunctionArgumentsFromTypes<Args...>();
}
template <auto Func>
constexpr auto GatherFunctionArguments() {
  return GatherFromSignature(Func);
}
template <auto Func, size_t I>
struct arg_ordinal_t {
  static constexpr size_t value = GatherFunctionArguments<Func>()[I].ordinal;
};

template <auto Func, int I = 0, typename... TArgs>
  requires(I >= sizeof...(TArgs))
void _translate_args_to_host(PPCContext&, uint8_t*, std::tuple<TArgs...>&) noexcept {}

template <auto Func, int I = 0, typename... TArgs>
  requires(I < sizeof...(TArgs))
void _translate_args_to_host(PPCContext& ctx, uint8_t* base, std::tuple<TArgs...>& tpl) noexcept {
  using T = std::tuple_element_t<I, std::remove_reference_t<decltype(tpl)>>;
  std::get<I>(tpl) = ArgTranslator::GetValue<T>(ctx, base, arg_ordinal_t<Func, I>::value);
  _translate_args_to_host<Func, I + 1>(ctx, base, tpl);
}

template <int I = 0, typename... TArgs>
  requires(I >= sizeof...(TArgs))
void _translate_args_to_guest(PPCContext&, uint8_t*, std::tuple<TArgs...>&) noexcept {}

template <int I = 0, typename... TArgs>
  requires(I < sizeof...(TArgs))
void _translate_args_to_guest(PPCContext& ctx, uint8_t* base, std::tuple<TArgs...>& tpl) noexcept {
  using T = std::tuple_element_t<I, std::remove_reference_t<decltype(tpl)>>;
  ArgTranslator::SetValue<T>(ctx, base, GatherFunctionArgumentsFromTypes<TArgs...>()[I].ordinal,
                             std::get<I>(tpl));
  _translate_args_to_guest<I + 1>(ctx, base, tpl);
}

// Guest -> host: unpack PPC registers into a native call, pack the return into r3/f1.
template <auto Func>
__attribute__((noinline)) void HostToGuestFunction(PPCContext& ctx, uint8_t* base) {
  using ret_t = decltype(std::apply(Func, function_args(Func)));
  auto args = function_args(Func);
  _translate_args_to_host<Func>(ctx, base, args);
  if constexpr (std::is_same_v<ret_t, void>) {
    std::apply(Func, args);
  } else {
    auto v = std::apply(Func, args);
    asm volatile("" ::: "memory");
    if constexpr (std::is_pointer<ret_t>()) {
      ctx.r3.u64 = v ? static_cast<uint32_t>(reinterpret_cast<size_t>(v) -
                                             reinterpret_cast<size_t>(base))
                     : 0;
    } else if constexpr (is_precise_v<ret_t>) {
      ctx.f1.f64 = v;
    } else {
      ctx.r3.u64 = static_cast<uint64_t>(v);
    }
  }
}

// Host -> guest: call a recompiled function on the current guest thread with native args.
template <typename T, typename TFunction, typename... TArgs>
T GuestToHostFunction(const TFunction& func, TArgs&&... argv) {
  auto args = std::make_tuple(std::forward<TArgs>(argv)...);
  PPCContext* currentCtx = xn::CurrentContext();
  uint8_t* base = xn::GuestBase();
  PPCContext newCtx{};
  newCtx.r1 = currentCtx->r1;
  newCtx.r1.u32 -= 0x70;
  newCtx.r13 = currentCtx->r13;
  newCtx.fpscr = currentCtx->fpscr;
  _translate_args_to_guest(newCtx, base, args);
  func(newCtx, base);
  currentCtx->fpscr = newCtx.fpscr;
  if constexpr (std::is_void_v<T>) {
    return;
  } else if constexpr (std::is_pointer_v<T>) {
    uint32_t guest_addr = newCtx.r3.u32;
    return guest_addr ? rex::memory::GuestPtr<T>(base, guest_addr) : nullptr;
  } else if constexpr (is_precise_v<T>) {
    return static_cast<T>(newCtx.f1.f64);
  } else {
    return static_cast<T>(newCtx.r3.u64);
  }
}

}  // namespace rex::ppc
