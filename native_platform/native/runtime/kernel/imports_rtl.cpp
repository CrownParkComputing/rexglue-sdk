// Rtl*, Xex*, Ke* trivia.
#include <cstring>
#include <chrono>
#include <cctype>

#include <rex/chrono/clock.h>
#include <rex/hook.h>

#include "kernel.h"

using namespace xn;

u32 RtlImageXexHeaderField_entry(u32 xex_header, u32 field) {
  Kernel& k = K();
  const uint8_t* header = k.image.header.data();
  size_t size = k.image.header.size();
  if (xex_header && xex_header != k.xex_header_guest) {
    header = G(xex_header);
    size = 0x2000;
  }
  uint32_t value = 0;
  if (!XexGetOptHeader(header, size, field, &value)) return 0;
  // Keys with a low byte of 0xFF (or >1 dword) are offsets into the header: the
  // caller expects a guest pointer.
  if ((field & 0xFF) > 1) return (xex_header ? xex_header : k.xex_header_guest) + value;
  return value;
}
XN_IMPORT(__imp__RtlImageXexHeaderField, RtlImageXexHeaderField_entry)

u32 XexCheckExecutablePrivilege_entry(u32 privilege) {
  return (K().image.system_flags >> privilege) & 1;
}
XN_IMPORT(__imp__XexCheckExecutablePrivilege, XexCheckExecutablePrivilege_entry)

void RtlInitAnsiString_entry(ppc_ptr_t<X_ANSI_STRING> dest, mapped_string source) {
  if (source) {
    uint16_t len = uint16_t(std::strlen(source));
    dest->length = len;
    dest->maximum_length = len + 1;
  } else {
    dest->length = 0;
    dest->maximum_length = 0;
  }
  dest->pointer = source.guest_address();
}
XN_IMPORT(__imp__RtlInitAnsiString, RtlInitAnsiString_entry)

void RtlFreeAnsiString_entry(ppc_ptr_t<X_ANSI_STRING> str) {
  if (str->pointer) K().arena.SystemFree(str->pointer);
  str->length = 0; str->maximum_length = 0; str->pointer = 0;
}
XN_IMPORT(__imp__RtlFreeAnsiString, RtlFreeAnsiString_entry)

void RtlInitUnicodeString_entry(ppc_ptr_t<X_ANSI_STRING> dest, u32 source) {
  // ANSI_STRING and UNICODE_STRING have the same layout; lengths are bytes.
  uint32_t bytes = 0;
  if (source) while (bytes < 65532 && Load16(source + bytes)) bytes += 2;
  dest->length = bytes;
  dest->maximum_length = source ? bytes + 2 : 0;
  dest->pointer = source;
}
XN_IMPORT(__imp__RtlInitUnicodeString, RtlInitUnicodeString_entry)

u32 RtlUnicodeToMultiByteN_entry(u32 dest, u32 capacity, mapped_u32 written, u32 source, u32 bytes) {
  uint32_t count = std::min(uint32_t(capacity), bytes >> 1);
  for (uint32_t i = 0; i < count; ++i) {
    uint16_t c = Load16(source + i * 2);
    Store8(dest + i, c < 256 ? uint8_t(c) : '?');
  }
  if (written) *written = count;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__RtlUnicodeToMultiByteN, RtlUnicodeToMultiByteN_entry)

u32 RtlUnicodeStringToAnsiString_entry(ppc_ptr_t<X_ANSI_STRING> dest,
                                      ppc_ptr_t<X_ANSI_STRING> source, u32 allocate) {
  uint32_t size = source->length / 2;
  if (allocate) {
    uint32_t ptr = K().arena.SystemAlloc(size + 1);
    if (!ptr) { dest->length = 0; dest->maximum_length = 0; dest->pointer = 0; return X_STATUS_NO_MEMORY; }
    dest->pointer = ptr;
    dest->maximum_length = size + 1;
  }
  uint32_t capacity = dest->maximum_length;
  uint32_t count = capacity ? std::min(size, capacity - 1) : 0;
  RtlUnicodeToMultiByteN_entry(dest->pointer, count, mapped_u32{}, source->pointer, source->length);
  if (capacity) Store8(dest->pointer + count, 0);
  dest->length = count;
  return capacity > size ? X_STATUS_SUCCESS : 0x80000005u; // STATUS_BUFFER_OVERFLOW
}
XN_IMPORT(__imp__RtlUnicodeStringToAnsiString, RtlUnicodeStringToAnsiString_entry)

void RtlTimeToTimeFields_entry(mapped_u64 time, u32 fields) {
  using namespace std::chrono;
  using FileDuration = duration<int64_t, std::ratio<1, 10000000>>;
  const sys_time<FileDuration> tp{FileDuration{int64_t(*time) - 116444736000000000ll}};
  auto day = floor<days>(tp);
  year_month_day date{day};
  hh_mm_ss clock{floor<milliseconds>(tp - day)};
  const uint16_t values[] = {uint16_t(int(date.year())), uint16_t(unsigned(date.month())),
    uint16_t(unsigned(date.day())), uint16_t(clock.hours().count()), uint16_t(clock.minutes().count()),
    uint16_t(clock.seconds().count()), uint16_t(clock.subseconds().count()), uint16_t(weekday{day}.c_encoding())};
  for (unsigned i = 0; i < 8; ++i) Store16(fields + i * 2, values[i]);
}
XN_IMPORT(__imp__RtlTimeToTimeFields, RtlTimeToTimeFields_entry)

u32 RtlTimeFieldsToTime_entry(u32 fields, mapped_u64 time) {
  using namespace std::chrono;
  uint16_t v[7];
  for (unsigned i = 0; i < 7; ++i) v[i] = Load16(fields + i * 2);
  year_month_day date{year{v[0]}, month{v[1]}, day{v[2]}};
  if (v[0] < 1601 || !date.ok() || v[3] > 23 || v[4] > 59 || v[5] > 59 || v[6] > 999) return 0;
  auto ms = (sys_days{date}.time_since_epoch() + hours{v[3]} + minutes{v[4]} + seconds{v[5]} + milliseconds{v[6]});
  *time = duration_cast<milliseconds>(ms).count() * 10000ll + 116444736000000000ll;
  return 1;
}
XN_IMPORT(__imp__RtlTimeFieldsToTime, RtlTimeFieldsToTime_entry)

u32 XexGetModuleSection_entry(u32 module, mapped_string name, mapped_u32 data, mapped_u32 size) {
  if (data) *data = 0;
  if (size) *size = 0;
  if (module != K().ldr_entry_guest) return X_STATUS_INVALID_HANDLE;
  if (!name || !data || !size) return X_STATUS_INVALID_PARAMETER;
  const auto& h = K().image.header;
  uint32_t offset = 0;
  if (XexGetOptHeader(h.data(), h.size(), 0x000002FF, &offset) && offset <= h.size() && h.size() - offset >= 4) {
    auto be32 = [&](size_t p) { return uint32_t(h[p]) << 24 | uint32_t(h[p+1]) << 16 | uint32_t(h[p+2]) << 8 | h[p+3]; };
    uint32_t length = be32(offset);
    if (length >= 4 && length <= h.size() - offset) {
      for (size_t p = offset + 4; p + 16 <= offset + length; p += 16) {
        std::string resource(reinterpret_cast<const char*>(h.data() + p), strnlen(reinterpret_cast<const char*>(h.data() + p), 8));
        if (resource == name.value()) { *data = be32(p + 8); *size = be32(p + 12); return X_STATUS_SUCCESS; }
      }
    }
  }
  return 0xC0000225u; // STATUS_NOT_FOUND
}
XN_IMPORT(__imp__XexGetModuleSection, XexGetModuleSection_entry)

// PGR4 checks the still-mounted image sections during startup. The loader has
// already validated and copied the complete decoded image into guest memory;
// this import is therefore a lightweight success for the running executable.
u32 XexLoadImageHeaders_entry(u32 image_base, u32 image_size, u32 flags) {
  (void)flags;
  if (image_base != K().image.base_address || image_size > K().image.image_size)
    return X_STATUS_INVALID_PARAMETER;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__XexLoadImageHeaders, XexLoadImageHeaders_entry)

// XeKeys signing/verifying is meaningful only on a console with its private
// key. The local port never authenticates to Xbox Live, so fail verification
// and signing explicitly while returning a fully defined signature length.
u32 XeKeysConsoleSignatureVerification_entry(u32 signature, u32 signature_size,
                                               u32 digest, u32 digest_size) {
  (void)signature; (void)signature_size; (void)digest; (void)digest_size;
  return 0;
}
XN_IMPORT(__imp__XeKeysConsoleSignatureVerification, XeKeysConsoleSignatureVerification_entry)

u32 XeKeysConsolePrivateKeySign_entry(u32 digest, u32 digest_size, u32 signature,
                                      mapped_u32 signature_size) {
  (void)digest; (void)digest_size;
  if (signature && signature_size && *signature_size) std::memset(G(signature), 0, *signature_size);
  if (signature_size) *signature_size = 0;
  return X_STATUS_NOT_IMPLEMENTED;
}
XN_IMPORT(__imp__XeKeysConsolePrivateKeySign, XeKeysConsolePrivateKeySign_entry)

u32 RtlMultiByteToUnicodeN_entry(mapped_u16 dest, u32 dest_len, mapped_u32 written, u32 src_ptr, u32 src_len) {
  uint32_t n = std::min(dest_len >> 1, src_len);
  for (uint32_t i = 0; i < n; i++) dest[i] = uint16_t(G(src_ptr)[i]);
  if (written) *written = n << 1;
  return 0;
}
XN_IMPORT(__imp__RtlMultiByteToUnicodeN, RtlMultiByteToUnicodeN_entry)

u32 RtlCompareMemoryUlong_entry(u32 source, u32 length, u32 pattern) {
  if ((source & 3) || (length & 3)) return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < length; i += 4) if (Load32(source + i) == pattern) n++;
  return n;
}
XN_IMPORT(__imp__RtlCompareMemoryUlong, RtlCompareMemoryUlong_entry)

void RtlFillMemoryUlong_entry(u32 dest, u32 length, u32 pattern) {
  for (uint32_t i = 0; i + 4 <= length; i += 4) Store32(dest + i, pattern);
}
XN_IMPORT(__imp__RtlFillMemoryUlong, RtlFillMemoryUlong_entry)

u32 RtlNtStatusToDosError_entry(u32 status) {
  switch (status) {
    case X_STATUS_SUCCESS: return 0;
    case X_STATUS_PENDING: return X_ERROR_IO_PENDING;
    case X_STATUS_TIMEOUT: return 258;
    case X_STATUS_NO_SUCH_FILE:
    case X_STATUS_OBJECT_NAME_NOT_FOUND: return 2;
    case X_STATUS_END_OF_FILE: return 38;
    case X_STATUS_NO_MEMORY: return 8;
    case X_STATUS_INVALID_HANDLE: return 6;
    case X_STATUS_INVALID_PARAMETER: return 87;
    default: return 317;  // ERROR_MR_MID_NOT_FOUND
  }
}
XN_IMPORT(__imp__RtlNtStatusToDosError, RtlNtStatusToDosError_entry)

XN_IMPORT_RAW(__imp__RtlRaiseException) {
  const uint32_t record = ctx.r3.u32;
  uint32_t code = record ? Load32(record + 0) : 0;
  uint32_t flags = record ? Load32(record + 4) : 0;
  uint32_t address = record ? Load32(record + 12) : 0;
  uint32_t count = record ? Load32(record + 16) : 0;
  uint32_t p0 = count > 0 ? Load32(record + 20) : 0;
  uint32_t p1 = count > 1 ? Load32(record + 24) : 0;
  uint32_t p2 = count > 2 ? Load32(record + 28) : 0;
  uint32_t p3 = count > 3 ? Load32(record + 32) : 0;
  REXKRNL_ERROR("RtlRaiseException record={:#010x} code={:#010x} flags={:#010x} address={:#010x} params={} [{:#010x}, {:#010x}, {:#010x}, {:#010x}] (lr={:#010x}, saved-lr={:#010x}, caller-r3={:#010x}, caller-r4={:#010x})",
                record, code, flags, address, count, p0, p1, p2, p3, uint32_t(ctx.lr),
                ctx.r1.u32 ? Load32(ctx.r1.u32 + 168) : 0,
                ctx.r1.u32 ? Load32(ctx.r1.u32 + 80) : 0,
                ctx.r1.u32 ? Load32(ctx.r1.u32 + 84) : 0);
  if (code == 0xE06D7363u && count >= 3 && p2) {
    uint32_t ti0 = Load32(p2 + 0), ti1 = Load32(p2 + 4), ti2 = Load32(p2 + 8), ti3 = Load32(p2 + 12);
    REXKRNL_ERROR("C++ throw info {:#010x}: [{:#010x}, {:#010x}, {:#010x}, {:#010x}], object {:#010x}: [{:#010x}, {:#010x}, {:#010x}, {:#010x}]",
                  p2, ti0, ti1, ti2, ti3, p1,
                  p1 ? Load32(p1 + 0) : 0, p1 ? Load32(p1 + 4) : 0,
                  p1 ? Load32(p1 + 8) : 0, p1 ? Load32(p1 + 12) : 0);
    if (ti3) {
      uint32_t cta_count = Load32(ti3 + 0);
      uint32_t ct0 = Load32(ti3 + 4);
      uint32_t props = ct0 ? Load32(ct0 + 0) : 0;
      uint32_t type = ct0 ? Load32(ct0 + 4) : 0;
      REXKRNL_ERROR("C++ catchable types {:#010x}: count={} first={:#010x} props={:#010x} type={:#010x}",
                    ti3, cta_count, ct0, props, type);
      if (type) {
        char name[96]{};
        for (uint32_t i = 0; i + 1 < sizeof(name); ++i) {
          char c = char(Load8(type + 8 + i));
          if (!c) break;
          name[i] = (c >= 32 && c < 127) ? c : '?';
        }
        REXKRNL_ERROR("C++ throw type name: {}", name);
      }
    }
  }
  Fatal(std::format("RtlRaiseException record={:#010x} (lr={:#010x})", record, uint32_t(ctx.lr)));
}
XN_IMPORT_RAW(__imp__RtlUnwind) { Fatal("RtlUnwind reached (SEH unwind is not supported)"); }
XN_IMPORT_RAW(__imp____C_specific_handler) { Fatal("__C_specific_handler reached"); }

// PPC guest floating-point state lives in the saved PPCContext, not in the
// host thread's x87/SSE register file. The title uses these as guards around
// host-side math; no guest-visible save buffer is consumed by its callers.
u32 KeSaveFloatingPointState_entry(u32 save_area) {
  (void)save_area;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__KeSaveFloatingPointState, KeSaveFloatingPointState_entry)

u32 KeRestoreFloatingPointState_entry(u32 save_area) {
  (void)save_area;
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__KeRestoreFloatingPointState, KeRestoreFloatingPointState_entry)

void KeQuerySystemTime_entry(mapped_u64 time_ptr) {
  if (time_ptr) *time_ptr = rex::chrono::Clock::QueryGuestSystemTime();
}
XN_IMPORT(__imp__KeQuerySystemTime, KeQuerySystemTime_entry)

u32 KeQueryPerformanceFrequency_entry() { return uint32_t(rex::chrono::Clock::kGuestTickFrequency); }
XN_IMPORT(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency_entry)

XN_IMPORT_RAW(__imp__KeBugCheck) {
  if (GuestThread* t = CurrentThread()) {
    Fatal(std::format("KeBugCheck({:#x}) on tid {:#x} lr={:#x} start={:#x}", ctx.r3.u32,
                      t->thread_id(), uint32_t(ctx.lr), t->params().start_address));
  }
  Fatal(std::format("KeBugCheck({:#x}) (no guest thread context)", ctx.r3.u32));
}
XN_IMPORT_RAW(__imp__KeBugCheckEx) {
  Fatal(std::format("KeBugCheckEx({:#x}, {:#x}, {:#x}, {:#x}, {:#x})", ctx.r3.u32, ctx.r4.u32,
                    ctx.r5.u32, ctx.r6.u32, ctx.r7.u32));
}
XN_IMPORT_RAW(__imp__HalReturnToFirmware) {
  REXKRNL_INFO("HalReturnToFirmware({}): title requested shutdown", ctx.r3.u32);
  K().title_running = false;
  while (true) std::this_thread::sleep_for(std::chrono::hours(1));
}

// RtlCaptureContext(context*). Boom Boom Rocket's only caller is 0x8217B230,
// the CRT's unwind entry: it captures a context, stores a handler address into
// context+0x08, and then calls RtlUnwind - which this runtime treats as fatal
// (above), because SEH unwinding is not implemented. So reaching this import is
// already a terminal condition and nothing downstream can observe a partial
// answer.
//
// That is why this does not fabricate a full PPC X_CONTEXT. The structure is
// several hundred bytes of register state we do not have at an import boundary,
// its exact layout is not established for this tree, and zeroing a length we
// guessed would write over the caller's own stack frame if the guess were long.
// It zeroes the leading words - enough that the field the caller overwrites
// next is defined rather than stale - and says loudly what happened.
XN_IMPORT_RAW(__imp__RtlCaptureContext) {
  const uint32_t context = ctx.r3.u32;
  REXKRNL_ERROR("RtlCaptureContext(context={:#010x}) from lr={:#010x}: SEH is not implemented in "
                "this runtime, and this title's only caller continues into RtlUnwind. Leading "
                "words zeroed; expect the RtlUnwind fatal next.",
                context, uint32_t(ctx.lr));
  if (context) {
    for (uint32_t o = 0; o < 0x20; o += 4) Store32(context + o, 0);
  }
}

u32 RtlCompareStringN_entry(u32 left, u32 left_len, u32 right, u32 right_len, u32 case_insensitive) {
  const uint32_t n = std::min(left_len, right_len);
  for (uint32_t i = 0; i < n; ++i) {
    unsigned char a = Load8(left + i);
    unsigned char b = Load8(right + i);
    if (case_insensitive) {
      if (a >= 'a' && a <= 'z') a -= 32;
      if (b >= 'a' && b <= 'z') b -= 32;
    }
    if (a != b) return a < b ? 0xFFFFFFFFu : 1u;
  }
  if (left_len == right_len) return 0;
  return left_len < right_len ? 0xFFFFFFFFu : 1u;
}
XN_IMPORT(__imp__RtlCompareStringN, RtlCompareStringN_entry)

u32 RtlUpcaseUnicodeChar_entry(u32 ch) {
  if (ch >= 'a' && ch <= 'z') return ch - 32;
  return ch;
}
XN_IMPORT(__imp__RtlUpcaseUnicodeChar, RtlUpcaseUnicodeChar_entry)
