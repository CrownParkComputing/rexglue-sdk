// Files: GAME:\ and D:\ map onto the assets directory (case-insensitive lookup).
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/hook.h>

#include "kernel.h"
#include "loader/xctd.h"
#include "memory/physical_write_watch.h"

using namespace xn;
namespace fs = std::filesystem;

namespace {

// All file-query paths must agree. MCLA compares creation times when reopening
// its save; returning the query's wall-clock time makes it report device removal.
bool WriteFileTimes(const fs::path& path, uint32_t output) {
  struct statx info{};
  if (::statx(AT_FDCWD, path.c_str(), AT_STATX_SYNC_AS_STAT,
              STATX_BASIC_STATS | STATX_BTIME, &info)) return false;
  auto filetime = [](const struct statx_timestamp& time) -> uint64_t {
    return uint64_t(time.tv_sec + 11644473600ll) * 10000000ull + time.tv_nsec / 100;
  };
  // Zero means unavailable on filesystems without birth times; do not invent
  // a creation time that changes whenever the file is read or written.
  Store64(output, info.stx_mask & STATX_BTIME ? filetime(info.stx_btime) : 0);
  Store64(output + 8, filetime(info.stx_atime));
  Store64(output + 16, filetime(info.stx_mtime));
  Store64(output + 24, filetime(info.stx_ctime));
  return true;
}

class File : public KObject {
 public:
  File(fs::path path, std::fstream stream, uint64_t size)
      : KObject(Type::File), path_(std::move(path)), stream_(std::move(stream)), size_(size) {}
  ~File() override {
    stream_.close();
    if (delete_on_close_) { std::error_code error; fs::remove(path_, error); }
  }
  const char* name() const override { return "file"; }
  fs::path path_;
  std::fstream stream_;
  uint64_t size_;
  uint64_t position_ = 0;
  bool synchronous_ = true;
  bool writable_ = false, deletable_ = false, delete_on_close_ = false;
  std::mutex mutex_;
  // When the on-disk file is XCTD-compressed (magic 0x0FF512ED), the console's
  // file cache would transparently serve decompressed bytes; this holds them.
  std::shared_ptr<const std::vector<uint8_t>> xctd_data_;
  // Directory-enumeration cursor (NtQueryDirectoryFile).
  std::vector<fs::path> dir_entries_;
  uint32_t dir_index_ = 0;
  bool dir_initialized_ = false;
  std::string dir_pattern_;
};

// The caller owns the file mutex. Completing a save flush must publish the
// fstream buffer and sync it before the guest sees completion.
bool FlushFile(File& file) {
  if (!file.writable_) return true;
  if (file.stream_.bad()) return false;
  file.stream_.clear(); // a preceding read may have reached EOF
  file.stream_.flush();
  if (!file.stream_) return false;
  const int fd = ::open(file.path_.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  int result;
  do { result = ::fsync(fd); } while (result && errno == EINTR);
  ::close(fd);
  return result == 0;
}

// Decompresses XCTD files once per path; titles tend to open their paks twice
// (probe, then read), and decoding 56 MB twice would double that cost.
std::shared_ptr<const std::vector<uint8_t>> XctdDecodedContent(const fs::path& host) {
  static std::mutex cache_mutex;
  static std::unordered_map<std::string, std::weak_ptr<const std::vector<uint8_t>>> cache;
  std::string key = fs::absolute(host).string();
  {
    std::lock_guard lock(cache_mutex);
    auto it = cache.find(key);
    if (it != cache.end()) {
      if (auto cached = it->second.lock()) return cached;
    }
  }
  std::ifstream f(host, std::ios::binary);
  if (!f) return nullptr;
  uint8_t prefix[16];
  f.read(reinterpret_cast<char*>(prefix), sizeof(prefix));
  if (!xn::XctdIsCompressed(prefix, size_t(f.gcount()))) return nullptr;
  f.clear();
  f.seekg(0);
  std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (!xn::XctdIsCompressed(raw.data(), raw.size())) return nullptr;
  auto decoded = std::make_shared<std::vector<uint8_t>>();
  if (!xn::XctdDecompress(raw.data(), raw.size(), *decoded)) {
    REXKRNL_WARN("XCTD: {} failed to decode; serving raw bytes", host.string());
    return nullptr;
  }
  REXKRNL_INFO("XCTD: {} decompressed {} -> {} bytes", host.string(), raw.size(), decoded->size());
  std::lock_guard lock(cache_mutex);
  cache[key] = decoded;
  return decoded;
}

std::string GuestPath(uint32_t object_attrs) {
  auto* attrs = reinterpret_cast<X_OBJECT_ATTRIBUTES*>(G(object_attrs));
  auto* name = reinterpret_cast<X_ANSI_STRING*>(G(attrs->name_ptr));
  return std::string(reinterpret_cast<const char*>(G(name->pointer)), name->length);
}

}  // namespace

uint32_t xn::FlushContentFiles(const fs::path& root) {
  std::unordered_set<File*> flushed;
  for (const auto& object : K().objects.Snapshot(KObject::Type::File)) {
    auto file = std::static_pointer_cast<File>(object);
    if (!flushed.insert(file.get()).second) continue;
    std::lock_guard lock(file->mutex_);
    const auto relative = file->path_.lexically_normal().lexically_relative(root.lexically_normal());
    if (relative.empty() || *relative.begin() == "..") continue;
    if (!FlushFile(*file)) return 29; // ERROR_WRITE_FAULT
  }
  // Content saves may have created or renamed a file since the last flush.
  const int fd = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return 29;
  int result;
  do { result = ::fsync(fd); } while (result && errno == EINTR);
  ::close(fd);
  return result == 0 ? X_ERROR_SUCCESS : 29;
}

// "GAME:\dir\file.ext" -> assets/dir/file.ext, matching names case-insensitively.
bool xn::ResolveHostPath(std::string guest, fs::path& out) {
  // ObCreateSymbolicLink registrations first: the guest may have aliased a
  // device name onto another one (Geometry Wars 2's D3D screenshot helper
  // does). Substitution is on the device part only, and is not recursive.
  {
    size_t colon = guest.find(':');
    if (colon != std::string::npos) {
      std::string device = guest.substr(0, colon);
      std::transform(device.begin(), device.end(), device.begin(), ::toupper);
      for (auto& [from, to] : K().symbolic_links) {
        if (from == device) { guest = to + guest.substr(colon + 1); break; }
      }
    }
  }
  std::string lower = guest;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  size_t colon = lower.find(':');
  std::string rel = colon == std::string::npos ? lower : lower.substr(colon + 1);
  std::string device = colon == std::string::npos ? "" : lower.substr(0, colon);
  if (device.rfind("\\device\\cdrom", 0) == 0 || device.rfind("\\device\\harddisk", 0) == 0) {
    size_t s = rel.find('\\', 1);
    rel = s == std::string::npos ? "" : rel.substr(s);
  }
  std::replace(rel.begin(), rel.end(), '\\', '/');
  fs::path cur = K().config.game_root;
  {
    std::lock_guard lock(K().content_mutex);
    auto mount = K().content_mounts.find(device);
    if (mount != K().content_mounts.end()) cur = mount->second;
    else if (!device.empty() && device != "game" && device != "d" && device.rfind("\\device\\", 0) != 0) return false;
  }
  size_t pos = 0;
  while (pos < rel.size()) {
    size_t next = rel.find('/', pos);
    std::string part = rel.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
    pos = next == std::string::npos ? rel.size() : next + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") return false;
    bool found = false;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(cur, ec)) {
      std::string n = e.path().filename().string();
      std::transform(n.begin(), n.end(), n.begin(), ::tolower);
      if (n == part) { cur = e.path(); found = true; break; }
    }
    if (!found) { cur /= part; }
  }
  out = cur;
  return true;
}

u32 NtCreateFile_entry(mapped_u32 handle_out, u32 desired_access, u32 object_attrs,
                       ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_u64 allocation_size_ptr,
                       u32 file_attributes, u32 share_access, u32 creation_disposition,
                       u32 create_options) {
  if (!object_attrs || !handle_out || creation_disposition > 5) return X_STATUS_INVALID_PARAMETER;
  std::string guest = GuestPath(object_attrs);
  fs::path host;
  if (!ResolveHostPath(guest, host)) {
    if (handle_out) *handle_out = 0;
    if (io_status_block) { io_status_block->status = X_STATUS_NO_SUCH_FILE; io_status_block->information = 0; }
    return X_STATUS_NO_SUCH_FILE;
  }
  // Generic all/write are high-bit access masks; testing only GENERIC_WRITE and
  // FILE_WRITE_DATA misses callers such as Burnout's 0x10100080 GENERIC_ALL
  // save-file open, causing CREATE_ALWAYS to fail because no file is created.
  constexpr uint32_t kGenericAll = 0x10000000u;
  constexpr uint32_t kGenericWrite = 0x40000000u;
  constexpr uint32_t kFileWriteData = 0x00000002u;
  constexpr uint32_t kFileAppendData = 0x00000004u;
  const bool want_write = (desired_access & (kGenericAll | kGenericWrite |
                                             kFileWriteData | kFileAppendData)) != 0;
  const bool want_delete = (desired_access & (kGenericAll | 0x10000)) != 0 || (create_options & 0x1000);
  std::error_code exists_error;
  const bool mutating_open = want_write || want_delete || creation_disposition == 0 ||
      creation_disposition == 4 || creation_disposition == 5 ||
      ((creation_disposition == 2 || creation_disposition == 3) && !fs::exists(host, exists_error));
  if (K().config.game_root_read_only && mutating_open) {
    std::error_code path_error;
    auto game_root = fs::weakly_canonical(K().config.game_root, path_error);
    auto target = fs::weakly_canonical(host, path_error);
    auto relative = target.lexically_relative(game_root);
    if (!relative.empty() && *relative.begin() != "..") {
      constexpr uint32_t denied = 0xC0000022u;
      *handle_out = 0;
      if (io_status_block) { io_status_block->status = denied; io_status_block->information = 0; }
      return denied;
    }
  }
  std::error_code ec;
  bool exists = fs::exists(host, ec);
  bool is_directory = exists && fs::is_directory(host, ec);
  uint32_t status = X_STATUS_SUCCESS;
  uint32_t action = 1;  // FILE_OPENED
  if (!exists && (creation_disposition == 1 /*OPEN*/ || creation_disposition == 4 /*OVERWRITE*/)) {
    status = X_STATUS_NO_SUCH_FILE;
  }
  if (create_options & 1 /*FILE_DIRECTORY_FILE*/) {
    if (exists && !is_directory) status = 0xC0000103u; // STATUS_NOT_A_DIRECTORY
    if (!exists && status == X_STATUS_SUCCESS) {
      is_directory = fs::create_directory(host, ec);
      if (!is_directory) status = X_STATUS_NO_SUCH_FILE;
      else action = 2;
    }
  }
  if (exists && creation_disposition == 2) status = 0xC0000035u; // OBJECT_NAME_COLLISION
  if (is_directory && (create_options & 0x40)) status = 0xC00000BAu; // FILE_IS_A_DIRECTORY
  std::shared_ptr<File> file;
  if (status == X_STATUS_SUCCESS) {
    std::fstream stream;
    uint64_t size = 0;
    if (!is_directory) {
      auto mode = std::ios::binary | std::ios::in;
      if (want_write) mode |= std::ios::out;
      if (!exists || creation_disposition == 0 /*SUPERSEDE*/ || creation_disposition == 4 /*OVERWRITE*/ || creation_disposition == 5 /*OVERWRITE_IF*/) {
        if (want_write) { std::ofstream(host, std::ios::binary).close(); action = 2; }
      }
      stream.open(host, mode);
      if (!stream) status = X_STATUS_NO_SUCH_FILE;
      else size = fs::file_size(host, ec);
    }
    if (status == X_STATUS_SUCCESS) {
      file = std::make_shared<File>(host, std::move(stream), size);
      file->synchronous_ = (create_options & 0x30) != 0;
      file->writable_ = want_write;
      file->deletable_ = want_delete;
      file->delete_on_close_ = (create_options & 0x1000) != 0;
      if (!is_directory && !want_write) {
        if (auto decoded = XctdDecodedContent(host)) {
          file->xctd_data_ = std::move(decoded);
          file->size_ = file->xctd_data_->size();
        }
      }
      K().objects.Add(file);
    }
  }
  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = action;
  }
  *handle_out = file ? file->handle() : X_INVALID_HANDLE_VALUE;
  REXKRNL_INFO("NtCreateFile '{}' -> {} ({:#x}, {} bytes)", guest, host.string(), status,
               file ? file->size_ : 0);
  if (file && !is_directory && K().config.file_opened) K().config.file_opened(host);
  return status;
}
XN_IMPORT(__imp__NtCreateFile, NtCreateFile_entry)

u32 NtReadFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                     ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, u32 buffer, u32 buffer_length,
                     mapped_u64 byte_offset_ptr) {
  auto file = K().objects.Lookup<File>(file_handle);
  if (!file) return X_STATUS_INVALID_HANDLE;
  uint32_t status = X_STATUS_SUCCESS;
  uint32_t bytes = 0;
  uint64_t request_offset = 0;
  {
    std::lock_guard lock(file->mutex_);
    uint64_t offset = byte_offset_ptr ? uint64_t(*byte_offset_ptr) : file->position_;
    if (byte_offset_ptr && int64_t(offset) < 0) offset = file->position_;  // FILE_USE_FILE_POINTER_POSITION
    request_offset = offset;
    if (offset >= file->size_) {
      status = X_STATUS_END_OF_FILE;
    } else if (file->xctd_data_) {
      const auto& decoded = *file->xctd_data_;
      bytes = uint32_t(std::min<uint64_t>(buffer_length, decoded.size() - offset));
      std::memcpy(G(buffer), decoded.data() + offset, bytes);
      file->position_ = offset + bytes;
    } else {
      file->stream_.clear();
      file->stream_.seekg(std::streamoff(offset));
      GuestWriteScope writable(G(buffer),buffer_length);
      file->stream_.read(reinterpret_cast<char*>(G(buffer)), buffer_length);
      bytes = uint32_t(file->stream_.gcount());
      file->position_ = offset + bytes;
    }
  }
  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = bytes;
  }
  static const bool io_stats = std::getenv("XN_IO_STATS") != nullptr;
  if (io_stats) {
    static std::atomic<uint64_t> reads{0};
    const auto sequence = reads.fetch_add(1, std::memory_order_relaxed);
    if (sequence < 16 || sequence % 100000 == 0)
      REXKRNL_INFO("native IO read={} path='{}' offset={:#x} request={:#x} bytes={:#x} status={:#x}",
          sequence, file->path_.string(), request_offset, buffer_length, bytes, status);
  }
  if (std::getenv("XN_TRACE_PGR4_IO")) {
    const uint64_t logged_offset = byte_offset_ptr ? uint64_t(*byte_offset_ptr) : file->position_ - bytes;
    PPCContext* context = CurrentContext();
    GuestThread* thread = CurrentThread();
    REXKRNL_INFO("PGR4 NtReadFile '{}' h={:#x} off={:#x} requested={:#x} got={:#x} "
                 "status={:#010x} event={:#x} lr={:#010x} tid={:#x}",
                 file->path_.string(), file_handle, logged_offset, buffer_length, bytes, status,
                 event_handle, context ? uint32_t(context->lr) : 0,
                 thread ? thread->thread_id() : 0);
    if (context && (status != X_STATUS_SUCCESS || bytes != buffer_length)) {
      uint32_t frame_sp = context->r1.u32;
      for (uint32_t frame = 0; frame < 5 && frame_sp; ++frame) {
        uint32_t previous_sp = Load32(frame_sp);
        if (!previous_sp || previous_sp <= frame_sp || previous_sp - frame_sp > 0x10000) break;
        REXKRNL_INFO("PGR4 NtReadFile stack frame={} sp={:#010x} caller_lr={:#010x}",
                     frame, frame_sp, Load32(previous_sp - 8));
        frame_sp = previous_sp;
      }
    }
  }
  if (event_handle) {
    if (auto ev = K().objects.Lookup<Event>(event_handle)) ev->Set();
  }
  if (apc_routine & ~1u) REXKRNL_WARN("NtReadFile with APC routine {:#x} ignored", apc_routine);
  if (status != X_STATUS_SUCCESS && status != X_STATUS_END_OF_FILE) {
    PPCContext* context = CurrentContext();
    GuestThread* thread = CurrentThread();
    REXKRNL_WARN("NtReadFile failed '{}' h={:#x} off={:#x} requested={:#x} "
                 "got={:#x} status={:#x} lr={:#010x} tid={:#x}",
                 file->path_.string(), file_handle,
                 byte_offset_ptr ? uint64_t(*byte_offset_ptr) : file->position_, buffer_length,
                 bytes, status, context ? uint32_t(context->lr) : 0,
                 thread ? thread->thread_id() : 0);
  }
  REXKRNL_DEBUG("NtReadFile h={:#x} len={:#x} off={} -> {:#x} ({} bytes)", file_handle, buffer_length,
                byte_offset_ptr ? int64_t(uint64_t(*byte_offset_ptr)) : -1, status, bytes);
  // The IOSB and event describe completion; the return value describes the
  // asynchronous handle contract, even when the host read completed inline.
  // Forza advances its queued stream offset on ERROR_IO_PENDING. Reporting a
  // synchronous success here made it reread one texture block indefinitely.
  return !file->synchronous_ && status == X_STATUS_SUCCESS ? X_STATUS_PENDING : status;
}
XN_IMPORT(__imp__NtReadFile, NtReadFile_entry)

u32 NtWriteFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                      ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, u32 buffer, u32 buffer_length,
                      mapped_u64 byte_offset_ptr) {
  auto file = K().objects.Lookup<File>(file_handle);
  if (!file) return X_STATUS_INVALID_HANDLE;
  uint32_t bytes = 0;
  uint64_t offset = 0;
  uint64_t file_size = 0;
  uint32_t status = X_STATUS_SUCCESS;
  {
    std::lock_guard lock(file->mutex_);
    offset = byte_offset_ptr && int64_t(uint64_t(*byte_offset_ptr)) >= 0
                 ? uint64_t(*byte_offset_ptr) : file->position_;
    file->stream_.clear();
    file->stream_.seekp(std::streamoff(offset));
    file->stream_.write(reinterpret_cast<const char*>(G(buffer)), buffer_length);
    if (file->stream_) {
      bytes = buffer_length;
      file->position_ = offset + bytes;
      // std::fstream may buffer writes, so querying filesystem size here can
      // observe the old length. Track the guest-visible EOF from successful
      // writes; this also handles sparse writes and overwrites correctly.
      file->size_ = std::max(file->size_, file->position_);
    } else {
      status = X_STATUS_UNSUCCESSFUL;
    }
    file_size = file->size_;
  }
  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = bytes;
  }
  if (file->path_.string().find("/content/") != std::string::npos) {
    REXKRNL_INFO("Content file write '{}' offset={:#x} requested={} written={} size={} status={:#x}",
                 file->path_.string(), offset, buffer_length, bytes, file_size, status);
  }
  if (event_handle) if (auto ev = K().objects.Lookup<Event>(event_handle)) ev->Set();
  // Match NtReadFile: completion lives in IOSB/event even when the host write
  // finished inline. Asynchronous handles must still report STATUS_PENDING.
  return !file->synchronous_ && status == X_STATUS_SUCCESS ? X_STATUS_PENDING : status;
}
XN_IMPORT(__imp__NtWriteFile, NtWriteFile_entry)

u32 NtQueryInformationFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                                 u32 info_ptr, u32 info_length, u32 info_class) {
  auto file = K().objects.Lookup<File>(file_handle);
  if (!file) return X_STATUS_INVALID_HANDLE;
  uint32_t status = X_STATUS_SUCCESS;
  uint32_t out_length = 0;
  std::memset(G(info_ptr), 0, info_length);
  switch (info_class) {
    case 4:  // FileBasicInformation
      if (info_length < 40) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      if (!WriteFileTimes(file->path_, info_ptr)) { status = X_STATUS_NO_SUCH_FILE; break; }
      Store32(info_ptr + 0x20, 0x80);  // FILE_ATTRIBUTE_NORMAL
      out_length = 40;
      break;
    case 5:  // FileStandardInformation
      Store64(info_ptr + 0, (file->size_ + 0xFFF) & ~0xFFFull);
      Store64(info_ptr + 8, file->size_);
      Store32(info_ptr + 16, 1);
      out_length = 24;
      break;
    case 6:  // FileInternalInformation
      Store64(info_ptr, uint64_t(std::hash<std::string>{}(file->path_.string())));
      out_length = 8;
      break;
    case 14:  // FilePositionInformation
      Store64(info_ptr, file->position_);
      out_length = 8;
      break;
    case 17:  // FileAlignmentInformation
      Store32(info_ptr, 0);
      out_length = 4;
      break;
    case 26: {  // Xbox FileSectorInformation: an opaque per-file identity.
      if (info_length < 4) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      const auto relative = file->path_.lexically_relative(K().config.game_root).generic_string();
      uint32_t identity = 2166136261u;
      for (unsigned char c : relative) identity = (identity ^ uint8_t(std::tolower(c))) * 16777619u;
      Store32(info_ptr, identity ? identity : 1);
      out_length = 4;
      break;
    }
    case 27:  // FileXctdCompressionInformation
      // 1 when the file carries the XCTD magic (0x0FF512ED), else 0. Ion Assault
      // queries this right after opening each pak; reads on such files are
      // transparently decompressed (see XctdDecodedContent).
      if (info_length < 4) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      Store32(info_ptr, file->xctd_data_ ? 1 : 0);
      out_length = 4;
      break;
    case 34:  // FileNetworkOpenInformation
      if (info_length < 56) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      if (!WriteFileTimes(file->path_, info_ptr)) { status = X_STATUS_NO_SUCH_FILE; break; }
      Store64(info_ptr + 0x20, (file->size_ + 0xFFF) & ~0xFFFull);
      Store64(info_ptr + 0x28, file->size_);
      Store32(info_ptr + 0x30, 0x80);
      out_length = 0x38;
      break;
    default:
      REXKRNL_WARN("NtQueryInformationFile: class {} unimplemented", info_class);
      status = X_STATUS_INVALID_INFO_CLASS;
      break;
  }
  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = out_length;
  }
  return status;
}
XN_IMPORT(__imp__NtQueryInformationFile, NtQueryInformationFile_entry)

u32 NtSetInformationFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                               u32 info_ptr, u32 info_length, u32 info_class) {
  auto file = K().objects.Lookup<File>(file_handle);
  if (!file) return X_STATUS_INVALID_HANDLE;
  uint32_t status = X_STATUS_SUCCESS;
  std::lock_guard lock(file->mutex_);
  switch (info_class) {
    case 13:  // FileDispositionInformation: delete when the last handle closes.
      if (info_length < 1 || !info_ptr) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      if (!file->deletable_) { status = 0xC0000022; break; }
      file->delete_on_close_ = Load8(info_ptr) != 0;
      break;
    case 14:  // FilePositionInformation
      if (info_length < 8 || !info_ptr) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      file->position_ = Load64(info_ptr);
      break;
    case 20: {  // FileEndOfFileInformation: update actual storage as well as EOF.
      if (info_length < 8 || !info_ptr) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      if (!file->writable_) { status = 0xC0000022; break; }
      file->stream_.flush();
      std::error_code error;
      fs::resize_file(file->path_, Load64(info_ptr), error);
      if (error) status = X_STATUS_UNSUCCESSFUL;
      else file->size_ = Load64(info_ptr);
      break;
    }
    case 19:  // FileAllocationInformation is an allocation hint; EOF is unchanged.
      if (info_length < 8 || !info_ptr) status = X_STATUS_INFO_LENGTH_MISMATCH;
      break;
    default:
      REXKRNL_WARN("NtSetInformationFile: class {} unimplemented", info_class);
      status = X_STATUS_INVALID_INFO_CLASS;
      break;
  }
  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = 0;
  }
  return status;
}
XN_IMPORT(__imp__NtSetInformationFile, NtSetInformationFile_entry)

u32 NtFlushBuffersFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block) {
  auto file = K().objects.Lookup<File>(file_handle);
  uint32_t status = X_STATUS_INVALID_HANDLE;
  if (file) {
    std::lock_guard lock(file->mutex_);
    status = FlushFile(*file) ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
  }
  if (io_status_block) { io_status_block->status = status; io_status_block->information = 0; }
  return status;
}
XN_IMPORT(__imp__NtFlushBuffersFile, NtFlushBuffersFile_entry)

// ---- NtOpenFile ---------------------------------------------------------------
// The second title's audio library opens with NtOpenFile rather than
// NtCreateFile (xapilib CreateFile 0x82140430). Same resolver, same File
// object; "open an existing file" is NtCreateFile with disposition OPEN and no
// allocation size.
u32 NtOpenFile_entry(mapped_u32 handle_out, u32 desired_access, u32 object_attrs,
                     ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, u32 share_access,
                     u32 open_options) {
  return NtCreateFile_entry(handle_out, desired_access, object_attrs, io_status_block,
                            mapped_u64{}, /*file_attributes=*/0, share_access,
                            /*creation_disposition=*/1 /*FILE_OPEN*/, open_options);
}
XN_IMPORT(__imp__NtOpenFile, NtOpenFile_entry)

u32 NtQueryFullAttributesFile_entry(u32 attrs, u32 info) {
  if (!attrs || !info) return X_STATUS_INVALID_PARAMETER;
  // FILE_NETWORK_OPEN_INFORMATION: six u64s then attributes and padding.
  std::memset(G(info), 0, 56);
  fs::path path;
  std::error_code ec;
  if (!ResolveHostPath(GuestPath(attrs), path) || !fs::exists(path, ec)) return X_STATUS_NO_SUCH_FILE;
  bool directory = fs::is_directory(path, ec);
  uint64_t size = directory ? 0 : fs::file_size(path, ec);
  if (ec) return X_STATUS_NO_SUCH_FILE;
  if (!WriteFileTimes(path, info)) return X_STATUS_NO_SUCH_FILE;
  Store64(info + 32, (size + 4095) & ~4095ull);
  Store64(info + 40, size);
  Store32(info + 48, directory ? 0x10 : 0x80);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile_entry)

// ---- volume information ---------------------------------------------------------
// Asked at open time by the same wrapper. The shape a retail hard disc reports:
// 512-byte sectors, one sector per allocation unit.
u32 NtQueryVolumeInformationFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                                       u32 fs_info_ptr, u32 length, u32 info_class) {
  auto file = K().objects.Lookup<File>(file_handle);
  if (!file) return X_STATUS_INVALID_HANDLE;
  uint32_t status = X_STATUS_SUCCESS;
  uint32_t out_length = 0;
  std::error_code ec;
  auto space = fs::space(K().config.game_root, ec);
  std::memset(G(fs_info_ptr), 0, length);
  switch (info_class) {
    case 1: {  // FileFsVolumeInformation
      static const char kLabel[] = "GAME";
      if (length < 18 + sizeof(kLabel)) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      Store64(fs_info_ptr + 0, rex::chrono::Clock::QueryGuestSystemTime());
      Store32(fs_info_ptr + 8, 0x584E0001);  // volume serial
      Store32(fs_info_ptr + 12, uint32_t(sizeof(kLabel) - 1));
      Store8(fs_info_ptr + 16, 0);           // not supports objects
      std::memcpy(G(fs_info_ptr + 17), kLabel, sizeof(kLabel) - 1);
      out_length = 17 + uint32_t(sizeof(kLabel) - 1);
      break;
    }
    case 3: {  // FileFsSizeInformation
      if (length < 24) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      const uint64_t sector = 512, per_unit = 1;
      Store64(fs_info_ptr + 0, ec ? 0x100000ull : space.capacity / (sector * per_unit));
      Store64(fs_info_ptr + 8, ec ? 0x80000ull : space.available / (sector * per_unit));
      Store32(fs_info_ptr + 16, uint32_t(per_unit));
      Store32(fs_info_ptr + 20, uint32_t(sector));
      out_length = 24;
      break;
    }
    case 4:  // FileFsDeviceInformation
      if (length < 8) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      Store32(fs_info_ptr + 0, 0x00000007);  // FILE_DEVICE_DISK
      Store32(fs_info_ptr + 4, 0x00000010);  // FILE_DEVICE_IS_MOUNTED
      out_length = 8;
      break;
    case 5: {  // FileFsAttributeInformation
      static const char kName[] = "FATX";
      if (length < 12 + sizeof(kName)) { status = X_STATUS_INFO_LENGTH_MISMATCH; break; }
      Store32(fs_info_ptr + 0, 0x00000004);  // FILE_CASE_PRESERVED_NAMES
      Store32(fs_info_ptr + 4, 42);          // max component name length
      Store32(fs_info_ptr + 8, uint32_t(sizeof(kName) - 1));
      std::memcpy(G(fs_info_ptr + 12), kName, sizeof(kName) - 1);
      out_length = 12 + uint32_t(sizeof(kName) - 1);
      break;
    }
    default:
      REXKRNL_WARN("NtQueryVolumeInformationFile: class {} unimplemented", info_class);
      status = X_STATUS_INVALID_INFO_CLASS;
      break;
  }
  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = out_length;
  }
  return status;
}
XN_IMPORT(__imp__NtQueryVolumeInformationFile, NtQueryVolumeInformationFile_entry)

// ---- directory enumeration -----------------------------------------------------
// Geometry Wars 2 links this through the file-API pointer table at 0x8218E7F4
// but never calls it. Implemented rather than stubbed because a stub would have
// to claim "empty directory", which is a wrong answer that looks like a right
// one. Xbox uses nine arguments and always FILE_DIRECTORY_INFORMATION;
// the desktop NT information-class / single-entry arguments do not exist.
u32 NtQueryDirectoryFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                               ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, u32 file_info_ptr,
                               u32 length, u32 file_name_ptr, u32 restart_scan) {
  auto dir = K().objects.Lookup<File>(file_handle);
  if (!dir) return X_STATUS_INVALID_HANDLE;
  std::string pattern;
  if (file_name_ptr) {
    auto* name = reinterpret_cast<X_ANSI_STRING*>(G(file_name_ptr));
    if (name->pointer && name->length) {
      pattern.assign(reinterpret_cast<const char*>(G(name->pointer)), name->length);
    }
  }
  std::lock_guard lock(dir->mutex_);
  if (restart_scan || !dir->dir_initialized_) {
    dir->dir_entries_.clear();
    dir->dir_index_ = 0;
    dir->dir_initialized_ = true;
    dir->dir_pattern_ = pattern;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir->path_, ec)) dir->dir_entries_.push_back(e.path());
    std::sort(dir->dir_entries_.begin(), dir->dir_entries_.end());
  }
  pattern = dir->dir_pattern_;
  auto matches = [&](const fs::path& p) {
    if (pattern.empty() || pattern == "*" || pattern == "*.*") return true;
    std::string n = p.filename().string(), pat = pattern;
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    std::transform(pat.begin(), pat.end(), pat.begin(), ::tolower);
    if (pat.size() > 1 && pat[0] == '*') return n.size() >= pat.size() - 1 &&
        n.compare(n.size() - (pat.size() - 1), pat.size() - 1, pat.substr(1)) == 0;
    return n == pat;
  };
  while (dir->dir_index_ < dir->dir_entries_.size() && !matches(dir->dir_entries_[dir->dir_index_])) {
    dir->dir_index_++;
  }
  if (dir->dir_index_ >= dir->dir_entries_.size()) {
    if (io_status_block) { io_status_block->status = 0x80000006; io_status_block->information = 0; }
    return 0x80000006;  // STATUS_NO_MORE_FILES
  }
  const fs::path& entry = dir->dir_entries_[dir->dir_index_];
  std::string name = entry.filename().string();
  const uint32_t header = 0x40;  // FILE_DIRECTORY_INFORMATION up to FileName
  if (length < std::max<size_t>(72, header + name.size())) {
    if (io_status_block) { io_status_block->status = X_STATUS_INFO_LENGTH_MISMATCH; io_status_block->information = 0; }
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }
  ++dir->dir_index_;
  std::error_code ec;
  bool is_dir = fs::is_directory(entry, ec);
  uint64_t size = is_dir ? 0 : fs::file_size(entry, ec);
  std::memset(G(file_info_ptr), 0, header);
  if (!WriteFileTimes(entry, file_info_ptr + 8)) {
    if (io_status_block) { io_status_block->status = X_STATUS_NO_SUCH_FILE; io_status_block->information = 0; }
    return X_STATUS_NO_SUCH_FILE;
  }
  Store32(file_info_ptr + 0x00, 0);                    // NextEntryOffset (single entry)
  Store32(file_info_ptr + 0x04, dir->dir_index_);      // FileIndex
  Store64(file_info_ptr + 0x28, size);
  Store64(file_info_ptr + 0x30, (size + 0xFFF) & ~0xFFFull);
  Store32(file_info_ptr + 0x38, is_dir ? 0x10 : 0x80);
  Store32(file_info_ptr + 0x3C, uint32_t(name.size()));
  std::memcpy(G(file_info_ptr + 0x40), name.data(), name.size());
  if (io_status_block) {
    io_status_block->status = X_STATUS_SUCCESS;
    io_status_block->information = header + uint32_t(name.size());
  }
  if (event_handle) if (auto ev = K().objects.Lookup<Event>(event_handle)) ev->Set();
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__NtQueryDirectoryFile, NtQueryDirectoryFile_entry)

// ---- scatter read ---------------------------------------------------------------
// Also table-only for this title. The segment array is 64-bit entries whose low
// 32 bits are a page-aligned guest buffer; each segment takes one page.
u32 NtReadFileScatter_entry(u32 file_handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                            ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, u32 segment_array,
                            u32 length, mapped_u64 byte_offset_ptr) {
  auto file = K().objects.Lookup<File>(file_handle);
  if (!file) return X_STATUS_INVALID_HANDLE;
  constexpr uint32_t kPage = 0x1000;
  uint32_t total = 0;
  uint32_t status = X_STATUS_SUCCESS;
  {
    std::lock_guard lock(file->mutex_);
    uint64_t offset = byte_offset_ptr && int64_t(uint64_t(*byte_offset_ptr)) >= 0
                          ? uint64_t(*byte_offset_ptr) : file->position_;
    if (offset >= file->size_) {
      status = X_STATUS_END_OF_FILE;
    } else if (file->xctd_data_) {
      const auto& decoded = *file->xctd_data_;
      for (uint32_t done = 0, seg = 0; done < length; done += kPage, seg++) {
        uint32_t buffer = uint32_t(Load64(segment_array + seg * 8));
        if (!buffer) break;
        uint32_t want = std::min<uint64_t>(std::min(kPage, length - done), decoded.size() - offset - done);
        std::memcpy(G(buffer), decoded.data() + offset + done, want);
        total += want;
        if (want < std::min(kPage, length - done)) break;
      }
      file->position_ = offset + total;
    } else {
      for (uint32_t done = 0, seg = 0; done < length; done += kPage, seg++) {
        uint32_t buffer = uint32_t(Load64(segment_array + seg * 8));
        if (!buffer) break;
        uint32_t want = std::min(kPage, length - done);
        file->stream_.clear();
        file->stream_.seekg(std::streamoff(offset + done));
        GuestWriteScope writable(G(buffer),want);
        file->stream_.read(reinterpret_cast<char*>(G(buffer)), want);
        uint32_t got = uint32_t(file->stream_.gcount());
        total += got;
        if (got < want) break;
      }
      file->position_ = offset + total;
    }
  }
  if (io_status_block) {
    io_status_block->status = status;
    io_status_block->information = total;
  }
  if (std::getenv("XN_TRACE_PGR4_IO")) {
    const uint64_t logged_offset = byte_offset_ptr && int64_t(uint64_t(*byte_offset_ptr)) >= 0
                                      ? uint64_t(*byte_offset_ptr) : file->position_ - total;
    PPCContext* context = CurrentContext();
    GuestThread* thread = CurrentThread();
    REXKRNL_INFO("PGR4 NtReadFileScatter '{}' h={:#x} off={:#x} requested={:#x} got={:#x} "
                 "status={:#010x} event={:#x} lr={:#010x} tid={:#x}",
                 file->path_.string(), file_handle, logged_offset, length, total, status,
                 event_handle, context ? uint32_t(context->lr) : 0,
                 thread ? thread->thread_id() : 0);
    if (context && (status != X_STATUS_SUCCESS || total != length)) {
      uint32_t frame_sp = context->r1.u32;
      for (uint32_t frame = 0; frame < 5 && frame_sp; ++frame) {
        uint32_t previous_sp = Load32(frame_sp);
        if (!previous_sp || previous_sp <= frame_sp || previous_sp - frame_sp > 0x10000) break;
        REXKRNL_INFO("PGR4 NtReadFileScatter stack frame={} sp={:#010x} caller_lr={:#010x}",
                     frame, frame_sp, Load32(previous_sp - 8));
        frame_sp = previous_sp;
      }
    }
  }
  if (event_handle) if (auto ev = K().objects.Lookup<Event>(event_handle)) ev->Set();
  REXKRNL_DEBUG("NtReadFileScatter h={:#x} len={:#x} -> {} bytes", file_handle, length, total);
  return status;
}
XN_IMPORT(__imp__NtReadFileScatter, NtReadFileScatter_entry)

// ---- object-manager symbolic links -------------------------------------------------
// Geometry Wars 2's D3D screenshot/persist helper (0x820B4008) creates one and
// removes it again (0x820B32C0). Recorded rather than ignored: ignoring it
// would leave a later open of the aliased device resolving to the wrong place,
// which is exactly the kind of silent wrong answer a stub should not produce.
namespace {
std::string LinkKey(uint32_t ansi_string_ptr) {
  auto* s = reinterpret_cast<X_ANSI_STRING*>(G(ansi_string_ptr));
  if (!s->pointer || !s->length) return {};
  std::string v(reinterpret_cast<const char*>(G(s->pointer)), s->length);
  // "\\??\\GAME:" / "GAME:" / "GAME" all key on "GAME".
  size_t last = v.find_last_of('\\');
  if (last != std::string::npos) v = v.substr(last + 1);
  if (!v.empty() && v.back() == ':') v.pop_back();
  std::transform(v.begin(), v.end(), v.begin(), ::toupper);
  return v;
}
}  // namespace

u32 ObCreateSymbolicLink_entry(u32 link_name_ptr, u32 target_ptr) {
  std::string from = LinkKey(link_name_ptr);
  auto* t = reinterpret_cast<X_ANSI_STRING*>(G(target_ptr));
  std::string to = t->pointer && t->length
                       ? std::string(reinterpret_cast<const char*>(G(t->pointer)), t->length)
                       : std::string();
  if (from.empty()) return X_STATUS_INVALID_PARAMETER;
  auto& links = K().symbolic_links;
  links.erase(std::remove_if(links.begin(), links.end(),
                             [&](auto& e) { return e.first == from; }), links.end());
  links.emplace_back(from, to);
  REXKRNL_INFO("ObCreateSymbolicLink {}: -> {}", from, to);
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__ObCreateSymbolicLink, ObCreateSymbolicLink_entry)

u32 ObDeleteSymbolicLink_entry(u32 link_name_ptr) {
  std::string from = LinkKey(link_name_ptr);
  auto& links = K().symbolic_links;
  size_t before = links.size();
  links.erase(std::remove_if(links.begin(), links.end(),
                             [&](auto& e) { return e.first == from; }), links.end());
  REXKRNL_INFO("ObDeleteSymbolicLink {}: ({})", from, before == links.size() ? "absent" : "removed");
  return X_STATUS_SUCCESS;
}
XN_IMPORT(__imp__ObDeleteSymbolicLink, ObDeleteSymbolicLink_entry)

XN_IMPORT_RAW(__imp__IoInvalidDeviceRequest) {
  REXKRNL_WARN("IoInvalidDeviceRequest(device={:#010x}, irp={:#010x})", ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = 0xC0000010u;  // STATUS_INVALID_DEVICE_REQUEST
}

XN_IMPORT_RAW(__imp__IoCreateDevice) {
  REXKRNL_WARN("IoCreateDevice(driver={:#010x}, extension={}, name={:#010x}): device objects are not modelled",
               ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  if (ctx.r8.u32) Store32(ctx.r8.u32, 0);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

XN_IMPORT_RAW(__imp__IoDeleteDevice) {
  REXKRNL_WARN("IoDeleteDevice(device={:#010x}): ignored", ctx.r3.u32);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

XN_IMPORT_RAW(__imp__IoCompleteRequest) {
  REXKRNL_DEBUG("IoCompleteRequest(irp={:#010x}, priority={:#x})", ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

XN_IMPORT_RAW(__imp__ObIsTitleObject) {
  ctx.r3.u64 = 1;
}

XN_IMPORT_RAW(__imp__IoCheckShareAccess) {
  REXKRNL_DEBUG("IoCheckShareAccess(desired={:#x}, share={:#x}, file={:#010x}, share_access={:#010x}, update={})",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

XN_IMPORT_RAW(__imp__IoSetShareAccess) {
  REXKRNL_DEBUG("IoSetShareAccess(desired={:#x}, share={:#x}, file={:#010x}, share_access={:#010x})",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  if (ctx.r6.u32) std::memset(G(ctx.r6.u32), 0, 0x20);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

XN_IMPORT_RAW(__imp__IoRemoveShareAccess) {
  REXKRNL_DEBUG("IoRemoveShareAccess(file={:#010x}, share_access={:#010x})", ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

XN_IMPORT_RAW(__imp__StfsCreateDevice) {
  REXKRNL_WARN("StfsCreateDevice(root={:#010x}, name={:#010x}): STFS device mounting is not modelled",
               ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

XN_IMPORT_RAW(__imp__StfsControlDevice) {
  REXKRNL_WARN("StfsControlDevice(device={:#010x}, code={:#x}): ignored", ctx.r3.u32, ctx.r4.u32);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

// PGR4 can ask the kernel to dismount its game volume while shutting down or
// changing content. GAME:/D: are static host roots, so there is no mount to
// tear down; acknowledge the request without changing the asset root.
XN_IMPORT_RAW(__imp__IoDismountVolume) {
  REXKRNL_DEBUG("IoDismountVolume(device={:#010x}): static host mount retained", ctx.r3.u32);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

XN_IMPORT_RAW(__imp__IoDismountVolumeByFileHandle) {
  REXKRNL_DEBUG("IoDismountVolumeByFileHandle(handle={:#010x}): static host mount retained", ctx.r3.u32);
  ctx.r3.u64 = X_STATUS_SUCCESS;
}

// Xbox NtDeviceIoControlFile has ten register/stack arguments. Device-control
// dispatch is not modelled yet; zero the caller's output and publish a defined
// NOT_IMPLEMENTED status rather than leaking stale guest memory.
u32 NtDeviceIoControlFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine,
                                u32 apc_context, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                                u32 control_code, u32 input_buffer, u32 input_length,
                                u32 output_buffer, u32 output_length) {
  (void)apc_routine;
  (void)apc_context;
  if (output_buffer && output_length) std::memset(G(output_buffer), 0, output_length);
  if (io_status_block) {
    io_status_block->status = X_STATUS_NOT_IMPLEMENTED;
    io_status_block->information = 0;
  }
  if (event_handle) {
    if (auto event = K().objects.Lookup<Event>(event_handle)) event->Set();
  }
  REXKRNL_WARN("NtDeviceIoControlFile(handle={:#x}, code={:#x}, input={} bytes, output={} bytes): "
               "device controls are not modelled", file_handle, control_code, input_length,
               output_length);
  return X_STATUS_NOT_IMPLEMENTED;
}
XN_IMPORT(__imp__NtDeviceIoControlFile, NtDeviceIoControlFile_entry)
