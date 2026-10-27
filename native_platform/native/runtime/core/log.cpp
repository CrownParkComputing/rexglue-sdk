#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace xn {
namespace {
std::atomic<int> g_min_level{int(LogLevel::Info)};
std::mutex g_mutex;
FILE* g_file = nullptr;
auto g_start = std::chrono::steady_clock::now();
const char* Name(LogLevel l) {
  switch (l) {
    case LogLevel::Trace: return "T";
    case LogLevel::Debug: return "D";
    case LogLevel::Info: return "I";
    case LogLevel::Warn: return "W";
    case LogLevel::Error: return "E";
    case LogLevel::Fatal: return "F";
  }
  return "?";
}
struct Init {
  Init() {
    if (const char* v = std::getenv("XN_LOG_LEVEL")) {
      switch (v[0]) {
        case 't': g_min_level = 0; break;
        case 'd': g_min_level = 1; break;
        case 'i': g_min_level = 2; break;
        case 'w': g_min_level = 3; break;
        case 'e': g_min_level = 4; break;
      }
    }
    if (const char* p = std::getenv("XN_LOG_FILE")) g_file = std::fopen(p, "w");
  }
} g_init;
}  // namespace

bool LogEnabled(LogLevel level) { return int(level) >= g_min_level.load(std::memory_order_relaxed); }

void LogWrite(LogLevel level, const char* category, std::string_view message) {
  auto now = std::chrono::steady_clock::now();
  double t = std::chrono::duration<double>(now - g_start).count();
  std::lock_guard lock(g_mutex);
  std::fprintf(stderr, "[%9.4f %s %-4s] %.*s\n", t, Name(level), category, int(message.size()),
               message.data());
  if (g_file) {
    std::fprintf(g_file, "[%9.4f %s %-4s] %.*s\n", t, Name(level), category, int(message.size()),
                 message.data());
    std::fflush(g_file);
  }
}

[[noreturn]] void Fatal(std::string_view message) {
  LogWrite(LogLevel::Fatal, "sys", message);
  std::fflush(stderr);
  std::abort();
}
}  // namespace xn
