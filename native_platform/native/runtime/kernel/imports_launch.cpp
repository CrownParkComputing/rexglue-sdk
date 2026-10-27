// Process-local XAM launch data. A cold start has no launch payload.
#include <cstring>
#include <mutex>
#include <vector>
#include <rex/hook.h>
#include "kernel.h"

using namespace xn;
namespace {
std::mutex launch_mutex;
std::vector<uint8_t> launch_data;
}

u32 XamLoaderSetLaunchData_entry(u32 data, u32 size) {
  if (size && !data) return X_ERROR_INVALID_PARAMETER;
  std::lock_guard lock(launch_mutex);
  if (size) launch_data.assign(G(data), G(data) + size);
  else launch_data.clear();
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamLoaderSetLaunchData, XamLoaderSetLaunchData_entry)

u32 XamLoaderGetLaunchDataSize_entry(mapped_u32 size) {
  if (!size) return X_ERROR_INVALID_PARAMETER;
  std::lock_guard lock(launch_mutex);
  *size = uint32_t(launch_data.size());
  return launch_data.empty() ? X_ERROR_NOT_FOUND : X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamLoaderGetLaunchDataSize, XamLoaderGetLaunchDataSize_entry)

u32 XamLoaderGetLaunchData_entry(u32 buffer, u32 size) {
  if (size && !buffer) return X_ERROR_INVALID_PARAMETER;
  std::lock_guard lock(launch_mutex);
  if (launch_data.empty()) return X_ERROR_NOT_FOUND;
  const size_t count = std::min<size_t>(size, launch_data.size());
  if (count) std::memcpy(G(buffer), launch_data.data(), count);
  return X_ERROR_SUCCESS;
}
XN_IMPORT(__imp__XamLoaderGetLaunchData, XamLoaderGetLaunchData_entry)
