#pragma once
/**
 * @file        rex/main_android.h
 * @brief       Android runtime facts the POSIX backends need
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <android/api-level.h>

namespace rex {

// The API level of the device this is RUNNING on, not the one it was built
// against. memory_posix.cpp and threading_posix.cpp both branch on it because
// the calls they want (memfd_create, pthread_getname_np) only exist from 26 on,
// and an APK built against a newer platform still has to start on an older one.
inline int GetAndroidApiLevel() {
  static const int level = android_get_device_api_level();
  return level;
}

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
