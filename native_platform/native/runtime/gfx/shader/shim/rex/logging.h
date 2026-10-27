// xenon-native shim for <rex/logging.h>: REXLOG_* / REXGPU_* go to a single
// sink (stderr by default; xn::log::SetSink installs another).
#pragma once
#include <string>
#include <fmt/format.h>
namespace xn::log {
enum class Level { kTrace, kDebug, kInfo, kWarn, kError };
using Sink = void (*)(Level, const std::string&);
void SetSink(Sink sink);
void Emit(Level level, const std::string& message);
}  // namespace xn::log
#define XN_LOG_(level, ...) ::xn::log::Emit(::xn::log::Level::level, ::fmt::format(__VA_ARGS__))
#define REXLOG_TRACE(...) XN_LOG_(kTrace, __VA_ARGS__)
#define REXLOG_DEBUG(...) XN_LOG_(kDebug, __VA_ARGS__)
#define REXLOG_INFO(...) XN_LOG_(kInfo, __VA_ARGS__)
#define REXLOG_WARN(...) XN_LOG_(kWarn, __VA_ARGS__)
#define REXLOG_ERROR(...) XN_LOG_(kError, __VA_ARGS__)
#define REXGPU_TRACE(...) XN_LOG_(kTrace, __VA_ARGS__)
#define REXGPU_DEBUG(...) XN_LOG_(kDebug, __VA_ARGS__)
#define REXGPU_INFO(...) XN_LOG_(kInfo, __VA_ARGS__)
#define REXGPU_WARN(...) XN_LOG_(kWarn, __VA_ARGS__)
#define REXGPU_ERROR(...) XN_LOG_(kError, __VA_ARGS__)
