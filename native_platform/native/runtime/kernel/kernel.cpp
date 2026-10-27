#include <cstdlib>
#include <algorithm>
#include <vector>

#include "kernel.h"
#include "local_profile.h"
#include "stats_store.h"
#include "profile_store.h"
#include "audio/imports_audio.h"

#include <cstring>
#include <thread>
#include <unordered_map>
#include <shared_mutex>

#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "gfx/command_processor.h"
#include "gfx/renderer.h"

namespace xn {

Kernel& K() {
  static Kernel kernel;
  return kernel;
}

namespace {

// Kernel variables the title imports by ordinal (xboxkrnl.exe). Each gets a
// guest-side home the size the real kernel gives it.
struct VariableExport {
  uint16_t ordinal;
  const char* name;
  uint32_t size;
};
const VariableExport kVariables[] = {
    {0x000C, "ExConsoleGameRegion", 4},  {0x000E, "ExEventObjectType", 0x1C},
    {0x0012, "ExMutantObjectType", 0x1C}, {0x0017, "ExSemaphoreObjectType", 0x1C},
    {0x001B, "ExThreadObjectType", 0x1C}, {0x001C, "ExTimerObjectType", 0x1C},
    {0x0059, "KeDebugMonitorData", 4},    {0x00AD, "KeTimeStampBundle", 24},
    {0x0156, "XboxHardwareInfo", 16},     {0x0158, "XboxKrnlVersion", 8},
    {0x0193, "XexExecutableModuleHandle", 4}, {0x01AE, "ExLoadedCommandLine", 0x400},
    {0x01AF, "ExLoadedImageName", 0x100}, {0x01BE, "VdGlobalDevice", 4},
    {0x01BF, "VdGlobalXamDevice", 4},     {0x01C0, "VdGpuClockInMHz", 4},
    {0x01C1, "VdHSIOCalibrationLock", 0x1C}, {0x0266, "KeCertMonitorData", 4},
};

std::unordered_map<uint32_t, PPCFunc*> g_function_map;
std::shared_mutex g_function_mutex;
uint32_t g_dispatch_table_guest = 0;
std::thread g_timestamp_thread;

void FillDispatchTable(const rex::PPCImageInfo& info) {
  std::unique_lock lock(g_function_mutex);
  Kernel& k = K();
  // Codegen contract: host pointers at IMAGE_BASE + IMAGE_SIZE + (addr - CODE_BASE) * 2.
  const uint32_t span = info.code_size + 0x10000;  // + REX_THUNK_RESERVE_SIZE
  g_dispatch_table_guest = info.image_base + info.image_size;
  if (!k.arena.AllocVirtualFixed(g_dispatch_table_guest, span * 2)) {
    Fatal("dispatch table placement failed");
  }
  k.arena.Fill(g_dispatch_table_guest, span * 2, 0);
  uint32_t count = 0;
  for (const PPCFuncMapping* m = info.func_mappings; m->host; m++) {
    uint32_t addr = uint32_t(m->guest);
    g_function_map[addr] = m->host;
    if (addr >= info.code_base && addr - info.code_base < span) {
      uint64_t offset = uint64_t(addr - info.code_base) * 2;
      std::memcpy(G(g_dispatch_table_guest) + offset, &m->host, sizeof(PPCFunc*));
    }
    count++;
  }
  REXLOG_INFO("dispatch table: {} functions at guest {:#010x}", count, g_dispatch_table_guest);
}

uint32_t InitVariable(const VariableExport& v) {
  Kernel& k = K();
  uint32_t addr = k.arena.SystemAlloc(v.size, 32);
  switch (v.ordinal) {
    case 0x000C: Store32(addr, 0xFFFFFFFF); break;             // ExConsoleGameRegion
    case 0x00AD: k.ke_timestamp_bundle = addr; break;
    case 0x0156: Store32(addr, 0x20); Store8(addr + 4, 6); break;  // flags, cpu count
    case 0x0158: Store16(addr, 2); Store16(addr + 2, 0xFFFF); Store16(addr + 4, 0xFFFF);
                 Store8(addr + 6, 0x80); break;                 // XboxKrnlVersion 2.x
    case 0x0193: Store32(addr, k.ldr_entry_guest); break;       // XexExecutableModuleHandle
    case 0x01AF: std::strcpy(reinterpret_cast<char*>(G(addr)), "game:\\default.xex"); break;
    case 0x01AE: std::strcpy(reinterpret_cast<char*>(G(addr)), "default.xex"); break;
    case 0x01BE: k.vd_global_device = addr; break;
    case 0x01C0: Store32(addr, 500); break;                     // VdGpuClockInMHz
    case 0x01C1: {                                              // VdHSIOCalibrationLock
      auto* cs = reinterpret_cast<X_RTL_CRITICAL_SECTION*>(G(addr));
      cs->header.type = 1;
      cs->lock_count = -1;
      break;
    }
    case 0x001B: k.ex_thread_object_type = addr; break;
    default: break;
  }
  return addr;
}

void PatchVariableImports(const XexImage& image) {
  Kernel& k = K();
  static std::unordered_map<uint16_t, uint32_t> homes;
  for (auto& lib : image.imports) {
    if (lib.name != "xboxkrnl.exe") continue;
    // Function imports have a type-1 thunk record for the same ordinal.
    std::unordered_map<uint16_t, bool> has_thunk;
    for (auto& r : lib.records) if (r.type == 1) has_thunk[r.ordinal] = true;
    for (auto& r : lib.records) {
      if (r.type != 0 || has_thunk[r.ordinal]) continue;
      const VariableExport* v = nullptr;
      for (auto& cand : kVariables) if (cand.ordinal == r.ordinal) v = &cand;
      if (!v) {
        REXLOG_WARN("variable import ordinal {} unknown; slot {:#010x} left as is", r.ordinal,
                    r.slot_address);
        continue;
      }
      uint32_t& home = homes[r.ordinal];
      if (!home) home = InitVariable(*v);
      Store32(r.slot_address, home);
      REXLOG_DEBUG("variable import {} -> {:#010x} (slot {:#010x})", v->name, home, r.slot_address);
    }
  }
  // Variables the kernel needs even if the title does not import them.
  if (!k.ke_timestamp_bundle) k.ke_timestamp_bundle = InitVariable(kVariables[7]);
  if (!k.ex_thread_object_type) k.ex_thread_object_type = InitVariable(kVariables[4]);
  if (!k.vd_global_device) k.vd_global_device = InitVariable(kVariables[13]);
}

}  // namespace

PPCFunc* LookupFunction(uint32_t guest_address) {
  std::shared_lock lock(g_function_mutex);
  auto it = g_function_map.find(guest_address);
  return it == g_function_map.end() ? nullptr : it->second;
}

void RegisterImageFunctions(const rex::PPCImageInfo& info) { FillDispatchTable(info); }
void UnregisterImageFunctions(const rex::PPCImageInfo& info) {
  std::unique_lock lock(g_function_mutex);
  for (const PPCFuncMapping* m = info.func_mappings; m->host; ++m)
    g_function_map.erase(uint32_t(m->guest));
  K().arena.FreeVirtual(info.image_base + info.image_size, nullptr);
}
void PatchImageVariableImports(const XexImage& image) { PatchVariableImports(image); }

bool KernelBoot(const KernelConfig& config, const rex::PPCImageInfo& image_info,
                std::string& error) {
  InstallWaitDumpSignalHandler();  // SIGUSR1 dumps every blocked thread
  Kernel& k = K();
  k.config = config;
  k.image_info = &image_info;
  if (!k.arena.Map(error)) return false;
  if (config.track_physical_writes) k.arena.EnablePhysicalWriteWatch();
  if (!LoadXexFile(config.xex_path, k.image, error)) return false;
  if (k.image.base_address != image_info.image_base || k.image.image_size != image_info.image_size) {
    error = std::format("XEX layout {:#x}+{:#x} does not match the generated code {:#x}+{:#x}",
                        k.image.base_address, k.image.image_size, image_info.image_base,
                        image_info.image_size);
    return false;
  }
  if (!k.arena.AllocVirtualFixed(k.image.base_address, k.image.image_size)) {
    error = "image placement failed";
    return false;
  }
  std::memcpy(G(k.image.base_address), k.image.image.data(), k.image.image_size);
  REXLOG_INFO("image {:#010x}+{:#x} entry {:#010x} title {:08X} stack {:#x}",
              k.image.base_address, k.image.image_size, k.image.entry_point, k.image.title_id,
              k.image.default_stack_size);
  FillDispatchTable(image_info);

  {
    std::filesystem::path dir = k.config.save_dir;
    if (dir.empty()) {
      dir = ProfileRoot();
      char tid[16]; std::snprintf(tid, sizeof tid, "%08X", k.image.title_id); dir /= tid;
    }
    if (!LocalUser().Open(ProfileRoot(), error) || !LocalUser().RegisterTitle(k.image, dir, error)) return false;
    k.save_dir = dir;
    Profile().Open(dir / "profile.bin");
    Stats().Open(dir / "stats.bin");
  }

  // Guest copy of the XEX header + a loader table entry that points at it
  // (XexExecutableModuleHandle -> entry, entry+0x58 -> header) for
  // RtlImageXexHeaderField and XamGetExecutionId.
  k.xex_header_guest = k.arena.SystemAlloc(uint32_t(k.image.header.size()), 0x1000);
  std::memcpy(G(k.xex_header_guest), k.image.header.data(), k.image.header.size());
  k.ldr_entry_guest = k.arena.SystemAlloc(0x80, 16);
  Store32(k.ldr_entry_guest + 0x58, k.xex_header_guest);
  Store32(k.ldr_entry_guest + 0x1C, k.image.base_address);  // image base
  Store32(k.ldr_entry_guest + 0x20, k.image.image_size);

  // Title process block: KTHREAD.process points here; process type = title.
  k.title_process = k.arena.SystemAlloc(0x60, 16);
  Store8(k.title_process + 0x24, 1);  // process_type

  k.tls_bitmap.assign((k.image.tls.slot_count ? k.image.tls.slot_count : 64 + 31) / 32 + 1, 0);
  PatchVariableImports(k.image);

  // KeTimeStampBundle+16 is the console's uptime in ms; refresh it every ms.
  g_timestamp_thread = std::thread([] {
    Kernel& kk = K();
    while (kk.title_running.load()) {
      Store32(kk.ke_timestamp_bundle + 16, rex::chrono::Clock::QueryGuestUptimeMillis());
      PumpOverlapped();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  g_timestamp_thread.detach();
  return true;
}

std::shared_ptr<GuestThread> KernelLaunch() {
  Kernel& k = K();
  GuestThread::Params p;
  p.name = "main";
  p.stack_size = k.image.default_stack_size;
  p.start_address = k.image.entry_point;
  p.start_context = 0;
  k.main_thread = CreateGuestThread(std::move(p));
  return k.main_thread;
}

void KernelShutdown() {
  Kernel& k = K();
  k.title_running = false;
  // Title-terminate notifications, highest priority first, the way the console
  // runs them when a title unloads. D3D registers one at device create and the
  // audio library registers one per pool; they release physical memory and
  // stop their own threads, so running them here is what makes a native
  // shutdown look like a console one rather than a process kill.
  //
  // Only from a thread that can call guest code: the routines are guest
  // functions. Shutdown runs on the host main thread, which has no guest
  // context, so the call is skipped there and the registrations are reported
  // instead - better than calling a guest function with no r1/r13.
  RunTitleTerminateNotifications();
  xn::audio::AudioShutdown();
}

void RunTitleTerminateNotifications() {
  Kernel& k = K();
  auto& list = k.title_terminate_notifications;
  if (list.empty()) return;
  if (!CurrentContext()) {
    REXKRNL_INFO("shutdown: {} title-terminate notification(s) not run (no guest context on this thread)",
                 list.size());
    return;
  }
  // Highest priority first; the registration's priority is at +4.
  std::vector<uint32_t> ordered(list.begin(), list.end());
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](uint32_t a, uint32_t b) { return Load32(a + 4) > Load32(b + 4); });
  for (uint32_t reg : ordered) {
    uint32_t routine = Load32(reg + 0);
    if (!routine) continue;
    REXKRNL_DEBUG("shutdown: title-terminate routine {:#010x} (registration {:#010x})", routine, reg);
    CallGuest(routine, {0});
  }
  list.clear();
}

namespace {

// An overlapped XAM request finishes a few milliseconds later, never inside
// the call that started it. On the console these take real time, and titles
// are written for that: they set the result field to IO_PENDING, kick the
// request, and poll. Completing synchronously means the operation is already
// finished when the call returns, so it is never "in flight" - and a screen
// that only takes input while nothing is pending can spin forever. That is
// exactly what Geometry Wars' leaderboard screen does: with an instant
// answer it re-issues the read every frame and never sees the Back button.
struct PendingOverlapped {
  uint32_t ptr = 0;
  uint32_t result = 0;
  uint32_t length = 0;
  uint32_t routine = 0;
  uint64_t due_ms = 0;
};
std::mutex g_overlapped_mutex;
std::vector<PendingOverlapped> g_overlapped;
std::thread g_overlapped_thread;

// Long enough that a polling title goes round its loop at least once, short
// enough to be invisible. Two frames at 60 Hz.
constexpr uint64_t kOverlappedLatencyMs = 32;

void ApplyOverlapped(const PendingOverlapped& p) {
  auto* o = reinterpret_cast<XAM_OVERLAPPED*>(G(p.ptr));
  o->length = p.length;
  // XAM publishes both the Win32 status and its HRESULT form. In particular,
  // an exhausted content enumerator is ERROR_NO_MORE_FILES / 0x80070012, not
  // an error accompanied by S_OK. Preserve callers that already supply HRESULTs.
  o->extended_error = !p.result || (p.result & 0x80000000u)
      ? p.result : 0x80070000u | (p.result & 0xFFFFu);
  o->result = p.result;  // last: this is the field the title polls
  if (uint32_t ev = o->event) {
    if (auto e = K().objects.Lookup<Event>(ev)) e->Set();
  }
  if (p.routine) {
    // Completion routines want (result, length, overlapped) on a guest
    // context. This pump has none; no title we run uses them, and saying so
    // is better than calling one from the wrong thread.
    REXKRNL_WARN("overlapped {:#x}: completion routine {:#x} not called (no guest context)", p.ptr,
                 p.routine);
  }
}

}  // namespace

void CompleteOverlapped(uint32_t overlapped_ptr, uint32_t result, uint32_t length) {
  if (!overlapped_ptr) return;
  auto* o = reinterpret_cast<XAM_OVERLAPPED*>(G(overlapped_ptr));
  uint32_t routine = o->completion_routine;
  // Mark it in flight now; the answer lands on a later tick.
  o->result = X_ERROR_IO_PENDING;
  o->length = 0;
  o->extended_error = 0;
  PendingOverlapped p{overlapped_ptr, result, length, routine,
                      rex::chrono::Clock::QueryGuestUptimeMillis() + kOverlappedLatencyMs};
  std::lock_guard lock(g_overlapped_mutex);
  g_overlapped.push_back(p);
}

// Cancels a deferred overlapped operation, if one is still in flight for this
// pointer. Returns whether there was one.
//
// This exists because overlapped operations here complete about 32 ms late (see
// CompleteOverlapped), which is deliberate - a title that polls has to be able
// to see one in flight - and that window is exactly what XMsgCancelIORequest is
// for. Cancelling completes it immediately with ERROR_CANCELLED rather than
// dropping it, because a dropped operation leaves the guest polling a result
// field that will never change.
bool CancelOverlapped(uint32_t overlapped_ptr) {
  if (!overlapped_ptr) return false;
  PendingOverlapped cancelled{};
  {
    std::lock_guard lock(g_overlapped_mutex);
    auto it = std::find_if(g_overlapped.begin(), g_overlapped.end(),
                           [&](const PendingOverlapped& p) { return p.ptr == overlapped_ptr; });
    if (it == g_overlapped.end()) return false;
    cancelled = *it;
    g_overlapped.erase(it);
  }
  cancelled.result = X_ERROR_CANCELLED;
  cancelled.length = 0;
  ApplyOverlapped(cancelled);
  return true;
}

// Applies overlapped requests whose latency has elapsed. Runs on the kernel's
// own timer thread so every title gets it without changing its main loop.
void PumpOverlapped() {
  std::vector<PendingOverlapped> due;
  {
    std::lock_guard lock(g_overlapped_mutex);
    if (g_overlapped.empty()) return;
    uint64_t now = rex::chrono::Clock::QueryGuestUptimeMillis();
    auto it = std::stable_partition(g_overlapped.begin(), g_overlapped.end(),
                                    [&](const PendingOverlapped& p) { return p.due_ms > now; });
    due.assign(it, g_overlapped.end());
    g_overlapped.erase(it, g_overlapped.end());
  }
  for (const PendingOverlapped& p : due) ApplyOverlapped(p);
}

void BroadcastNotification(uint32_t id, uint32_t param) {
  // Listener handles are small; walk a bounded range rather than keep a list.
  Kernel& k = K();
  for (uint32_t h = 0x10; h < 0x1000; h += 4) {
    auto l = k.objects.Lookup<NotifyListener>(h);
    if (!l) continue;
    // Bit 31 marks an internal notification; bits 25..30 select the area.
    uint32_t area = (id >> 25) & 0x3F;
    if (l->mask() & (1ull << area)) l->Enqueue(id, param);
  }
}

}  // namespace xn
