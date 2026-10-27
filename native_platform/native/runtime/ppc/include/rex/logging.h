// xenon-native: logging surface referenced by the codegen contract
// (geometrywars_pch.h uses REXCPU_DEBUG / REXCPU_WARN) and by the runtime.
#pragma once
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace xn {
enum class LogLevel : int { Trace = 0, Debug, Info, Warn, Error, Fatal };
void LogWrite(LogLevel level, const char* category, std::string_view message);
bool LogEnabled(LogLevel level);
template <class... A>
inline void LogF(LogLevel level, const char* cat, std::format_string<A...> f, A&&... a) {
  if (LogEnabled(level)) LogWrite(level, cat, std::format(f, std::forward<A>(a)...));
}
[[noreturn]] void Fatal(std::string_view message);
}  // namespace xn

#define XN_LOG(level, cat, ...) ::xn::LogF(::xn::LogLevel::level, cat, __VA_ARGS__)
#define REXCPU_TRACE(...) XN_LOG(Trace, "cpu", __VA_ARGS__)
#define REXCPU_DEBUG(...) XN_LOG(Debug, "cpu", __VA_ARGS__)
#define REXCPU_INFO(...) XN_LOG(Info, "cpu", __VA_ARGS__)
#define REXCPU_WARN(...) XN_LOG(Warn, "cpu", __VA_ARGS__)
#define REXCPU_ERROR(...) XN_LOG(Error, "cpu", __VA_ARGS__)
#define REXKRNL_TRACE(...) XN_LOG(Trace, "krnl", __VA_ARGS__)
#define REXKRNL_DEBUG(...) XN_LOG(Debug, "krnl", __VA_ARGS__)
#define REXKRNL_INFO(...) XN_LOG(Info, "krnl", __VA_ARGS__)
#define REXKRNL_WARN(...) XN_LOG(Warn, "krnl", __VA_ARGS__)
#define REXKRNL_ERROR(...) XN_LOG(Error, "krnl", __VA_ARGS__)
#define REXGPU_DEBUG(...) XN_LOG(Debug, "gpu", __VA_ARGS__)
#define REXGPU_INFO(...) XN_LOG(Info, "gpu", __VA_ARGS__)
#define REXGPU_WARN(...) XN_LOG(Warn, "gpu", __VA_ARGS__)
#define REXLOG_DEBUG(...) XN_LOG(Debug, "sys", __VA_ARGS__)
#define REXLOG_INFO(...) XN_LOG(Info, "sys", __VA_ARGS__)
#define REXLOG_WARN(...) XN_LOG(Warn, "sys", __VA_ARGS__)
#define REXLOG_ERROR(...) XN_LOG(Error, "sys", __VA_ARGS__)
#define REX_FATAL(...) ::xn::Fatal(std::format(__VA_ARGS__))
