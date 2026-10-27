// xenon-native: storage for the knobs the vendored translator reads, plus the
// logging sink behind the <rex/logging.h> shim.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <rex/cvar.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(dump_shaders, "", "GPU", "directory to dump analyzed microcode into");
REXCVAR_DEFINE_BOOL(gpu_vertex_nan_kill_debug, false, "GPU", "log NaN vertex kill decisions");
REXCVAR_DEFINE_BOOL(texture_exp_bias_ignore, false, "GPU", "ignore texture fetch exponent bias");

namespace xn::log {
namespace {
Level MinLevel() {
  static Level min_level = [] {
    const char* v = std::getenv("XN_LOG_LEVEL");
    if (!v) return Level::kInfo;
    switch (v[0]) {
      case 't': return Level::kTrace;
      case 'd': return Level::kDebug;
      case 'w': return Level::kWarn;
      case 'e': return Level::kError;
      default: return Level::kInfo;
    }
  }();
  return min_level;
}
void DefaultSink(Level level, const std::string& message) {
  static const char* const kNames[] = {"trace", "debug", "info", "warn", "error"};
  if (level < MinLevel()) return;
  std::fprintf(stderr, "[xn %s] %s\n", kNames[int(level)], message.c_str());
}
Sink g_sink = DefaultSink;
}  // namespace
void SetSink(Sink sink) { g_sink = sink ? sink : DefaultSink; }
void Emit(Level level, const std::string& message) { g_sink(level, message); }
}  // namespace xn::log
