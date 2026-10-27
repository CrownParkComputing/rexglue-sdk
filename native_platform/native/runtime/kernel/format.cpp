// Guest printf family (sprintf / _vsnprintf are kernel exports on the 360).
// Arguments follow the PPC ABI: integers in r3.., overflow on the stack at
// r1+0x54; doubles in f1.. (each also consuming an integer slot).
#include <cmath>
#include <cstdio>
#include <string>

#include <rex/hook.h>

#include "kernel.h"

namespace xn {

struct GuestArgs {
  PPCContext& ctx;
  uint8_t* base;
  uint32_t next_int;   // absolute argument index
  uint32_t next_float = 0;
  uint64_t Int() { return rex::ppc::ArgTranslator::GetIntegerArgumentValue(ctx, base, next_int++); }
  double Float() { next_int++; return rex::ppc::ArgTranslator::GetPrecisionArgumentValue(ctx, next_float++); }
};

// Formats fmt (guest string) into out using guest args. Returns the length.
static std::string GuestFormat(const char* fmt, GuestArgs& args) {
  std::string out;
  for (const char* p = fmt; *p; p++) {
    if (*p != '%') { out.push_back(*p); continue; }
    std::string spec = "%";
    p++;
    if (*p == '%') { out.push_back('%'); continue; }
    while (*p && strchr("-+ #0", *p)) spec.push_back(*p++);
    if (*p == '*') { spec += std::to_string(int(args.Int())); p++; }
    while (*p >= '0' && *p <= '9') spec.push_back(*p++);
    if (*p == '.') {
      spec.push_back(*p++);
      if (*p == '*') { spec += std::to_string(int(args.Int())); p++; }
      while (*p >= '0' && *p <= '9') spec.push_back(*p++);
    }
    bool is_long = false, is_i64 = false;
    while (*p == 'l' || *p == 'h' || *p == 'I' || *p == 'w') {
      if (*p == 'l') is_long = true;
      if (*p == 'I' && p[1] == '6' && p[2] == '4') { is_i64 = true; p += 2; }
      p++;
    }
    char buf[512];
    char c = *p;
    switch (c) {
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
        uint64_t v = args.Int();
        if (is_i64) { spec += "ll"; spec.push_back(c); std::snprintf(buf, sizeof buf, spec.c_str(), (long long)v); }
        else { spec.push_back(c); std::snprintf(buf, sizeof buf, spec.c_str(), (int)v); }
        out += buf;
        break;
      }
      case 'p': {
        uint64_t v = args.Int();
        std::snprintf(buf, sizeof buf, "%08X", uint32_t(v));
        out += buf;
        break;
      }
      case 'f': case 'g': case 'e': case 'G': case 'E': {
        double v = args.Float();
        spec.push_back(c);
        std::snprintf(buf, sizeof buf, spec.c_str(), v);
        out += buf;
        break;
      }
      case 's': {
        uint32_t ptr = uint32_t(args.Int());
        const char* s = ptr ? reinterpret_cast<const char*>(args.base + ptr) : "(null)";
        if (is_long) {  // wide guest string (UTF-16BE): narrow it
          std::string n;
          for (const uint16_t* w = reinterpret_cast<const uint16_t*>(s); ptr && __builtin_bswap16(*w); w++)
            n.push_back(char(__builtin_bswap16(*w)));
          spec.push_back('s');
          std::snprintf(buf, sizeof buf, spec.c_str(), n.c_str());
        } else {
          spec.push_back('s');
          std::snprintf(buf, sizeof buf, spec.c_str(), s);
        }
        out += buf;
        break;
      }
      default:
        out += spec;
        out.push_back(c);
        break;
    }
  }
  return out;
}

}  // namespace xn

using namespace xn;

// int sprintf(char* buffer, const char* format, ...)
XN_IMPORT_RAW(__imp__sprintf) {
  uint32_t buffer = ctx.r3.u32;
  const char* fmt = reinterpret_cast<const char*>(base + ctx.r4.u32);
  GuestArgs args{ctx, base, 2};
  std::string s = GuestFormat(fmt, args);
  std::memcpy(base + buffer, s.c_str(), s.size() + 1);
  ctx.r3.u64 = s.size();
}

// Xbox/MSVC _snprintf: a full buffer need not have a terminator; truncation
// returns -1. Arguments after format use the ordinary variadic PPC ABI.
XN_IMPORT_RAW(__imp___snprintf) {
  const uint32_t buffer = ctx.r3.u32, count = ctx.r4.u32;
  GuestArgs args{ctx, base, 3};
  const std::string s = GuestFormat(reinterpret_cast<const char*>(base + ctx.r5.u32), args);
  if (buffer && count) {
    const size_t n = std::min<size_t>(s.size(), count);
    std::memcpy(base + buffer, s.data(), n);
    if (n < count) base[buffer + n] = 0;
  }
  ctx.r3.s64 = s.size() > count ? -1 : int64_t(s.size());
}

// int _vsnprintf(char* buffer, size_t count, const char* format, va_list)
// The va_list is a guest pointer to the caller's register save area. We cannot
// re-marshal it precisely; treat it as a pointer to packed 32-bit args.
XN_IMPORT_RAW(__imp___vsnprintf) {
  uint32_t buffer = ctx.r3.u32;
  uint32_t count = ctx.r4.u32;
  const char* fmt = reinterpret_cast<const char*>(base + ctx.r5.u32);
  uint32_t va = ctx.r6.u32;
  // Build a context whose "stack args" start at the va_list.
  PPCContext tmp = ctx;
  tmp.r1.u32 = va - 0x54 - 8 * 8 + 8 * 8;  // arg index 8 lands at va
  GuestArgs args{tmp, base, 8};
  std::string s = GuestFormat(fmt, args);
  if (count) {
    size_t n = std::min<size_t>(s.size(), count - 1);
    std::memcpy(base + buffer, s.data(), n);
    base[buffer + n] = 0;
  }
  ctx.r3.u64 = s.size();
}

// void DbgPrint(const char* format, ...)
XN_IMPORT_RAW(__imp__DbgPrint) {
  const char* fmt = reinterpret_cast<const char*>(base + ctx.r3.u32);
  GuestArgs args{ctx, base, 1};
  std::string s = GuestFormat(fmt, args);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  REXKRNL_INFO("DbgPrint: {}", s);
  ctx.r3.u64 = 0;
}
