// xenon-native: the kernel state shared by every import.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <vector>

#include <rex/image_info.h>
#include <rex/ppc/func.h>

#include "loader/xex.h"
#include "memory/arena.h"
#include "objects.h"
#include "threads.h"

namespace xn {

class IRenderer;
class CommandProcessor;

struct KernelConfig {
  std::string xex_path;
  std::filesystem::path game_root;  // GAME:\ maps here
  uint32_t display_width = 1280;
  uint32_t display_height = 720;
  bool headless = false;
  bool track_physical_writes = false; // replace repeated GPU memory scans with CPU write tracking
  bool game_root_read_only = false;
  std::function<void(const std::filesystem::path&)> file_opened;
  std::filesystem::path save_dir;   // profile settings (high score); default $XDG_DATA_HOME/xenon-native/<title_id>
};

struct Kernel {
  KernelConfig config;
  Arena arena;
  XexImage image;
  const rex::PPCImageInfo* image_info = nullptr;
  ObjectTable objects;
  IRenderer* renderer = nullptr;
  CommandProcessor* gpu = nullptr;

  // Guest-side globals the title imports as variables (guest addresses).
  uint32_t xex_header_guest = 0;      // copy of the raw XEX header
  uint32_t ldr_entry_guest = 0;       // X_LDR_DATA_TABLE_ENTRY (+0x58 = header)
  uint32_t ke_timestamp_bundle = 0;   // KeTimeStampBundle (uptime ms at +16)
  uint32_t title_process = 0;         // X_KPROCESS
  uint32_t ex_thread_object_type = 0;
  uint32_t vd_global_device = 0;
  std::vector<uint32_t> tls_bitmap;   // per-process TLS slot allocation
  // Where profile.bin / stats.bin live, after KernelConfig::save_dir has been
  // defaulted. Titles that keep their own save file put it here too.
  std::filesystem::path save_dir;
  // ExRegisterTitleTerminateNotification registrations (guest addresses), in
  // registration order. Nothing runs them yet - the runtime never unloads a
  // title - but dropping them on the floor would lose the D3D and audio
  // shutdown hooks a clean exit will need.
  std::vector<uint32_t> title_terminate_notifications;
  // ObCreateSymbolicLink: guest device name (upper case, no trailing colon or
  // backslash) -> target, consulted by the file-path resolver.
  std::vector<std::pair<std::string, std::string>> symbolic_links;
  // Content roots registered by XamContentCreate/Ex, mapped to local save data.
  std::mutex content_mutex;
  std::map<std::string, std::filesystem::path> content_mounts;

  // Video / interrupt state (Vd* imports, consumed by the command processor).
  uint32_t gfx_interrupt_callback = 0;
  uint32_t gfx_interrupt_user_data = 0;
  std::atomic<uint32_t> nui_tilt_callback{0};

  std::atomic<bool> title_running{true};
  std::atomic<int> title_exit_code{0};
  std::shared_ptr<GuestThread> main_thread;
};

Kernel& K();  // the singleton

// Boot: map arena, load XEX into it, fill the dispatch table, create kernel globals.
bool KernelBoot(const KernelConfig& config, const rex::PPCImageInfo& image_info,
                std::string& error);
// Runs the title's entry point on a new guest thread and returns it.
std::shared_ptr<GuestThread> KernelLaunch();
void KernelShutdown();
// Runs (and clears) the ExRegisterTitleTerminateNotification list, highest
// priority first. Called by KernelShutdown; only does anything on a thread
// with a guest context, because the routines are guest functions.
void RunTitleTerminateNotifications();

PPCFunc* LookupFunction(uint32_t guest_address);
void RegisterImageFunctions(const rex::PPCImageInfo& info);
void UnregisterImageFunctions(const rex::PPCImageInfo& info);
void PatchImageVariableImports(const XexImage& image);

// Guest memory helpers.
inline uint8_t* G(uint32_t addr) { return K().arena.Translate(addr); }
inline uint32_t Load32(uint32_t addr) { return __builtin_bswap32(*reinterpret_cast<uint32_t*>(G(addr))); }
inline void Store32(uint32_t addr, uint32_t v) { *reinterpret_cast<uint32_t*>(G(addr)) = __builtin_bswap32(v); }
inline uint16_t Load16(uint32_t addr) { return __builtin_bswap16(*reinterpret_cast<uint16_t*>(G(addr))); }
inline void Store16(uint32_t addr, uint16_t v) { *reinterpret_cast<uint16_t*>(G(addr)) = __builtin_bswap16(v); }
inline uint8_t Load8(uint32_t addr) { return *G(addr); }
inline void Store8(uint32_t addr, uint8_t v) { *G(addr) = v; }
inline void Store64(uint32_t addr, uint64_t v) { *reinterpret_cast<uint64_t*>(G(addr)) = __builtin_bswap64(v); }
inline uint64_t Load64(uint32_t addr) { return __builtin_bswap64(*reinterpret_cast<uint64_t*>(G(addr))); }

// Overlapped completion shared by XAM imports.
void CompleteOverlapped(uint32_t overlapped_ptr, uint32_t result, uint32_t length = 0);
// Completes a still-deferred overlapped operation as ERROR_CANCELLED. Returns
// false when nothing was in flight for that pointer.
bool CancelOverlapped(uint32_t overlapped_ptr);
// Applies overlapped answers whose latency has elapsed (kernel timer thread).
void PumpOverlapped();
// Notification broadcast to every listener whose mask matches.
void BroadcastNotification(uint32_t id, uint32_t param);
// Shared case-insensitive guest file lookup (GAME:, D:, content mounts).
bool ResolveHostPath(std::string guest, std::filesystem::path& out);
// Flush writable handles under a mounted content root; returns a Win32 error.
uint32_t FlushContentFiles(const std::filesystem::path& root);

}  // namespace xn
