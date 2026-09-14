/**
 * @file        rex/core/filesystem_android.cpp
 * @brief       Android content-URI entry points for rex::filesystem
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <rex/filesystem.h>
#include <rex/logging.h>

namespace rex {
namespace filesystem {

// Android's Storage Access Framework hands an app a `content://` URI rather
// than a path, and the only way to read one is ContentResolver.openFileDescriptor
// over JNI. Ports here are given their game tree in the app's own external
// files directory, which is an ordinary path, so nothing reaches for a content
// URI - mapped_memory_posix only calls this after IsAndroidContentUri says yes.
//
// Recognising the scheme is worth doing properly even so: it is what stops a
// content URI being silently treated as a relative path and failing as "file
// not found", which is a much worse thing to debug than the message below.

bool IsAndroidContentUri(const std::string_view source) {
  return source.starts_with("content://");
}

int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  (void)mode;
  REXLOG_ERROR(
      "Cannot open the content URI {}: reading one needs a JNI call to "
      "ContentResolver.openFileDescriptor, which is not wired up. Give the "
      "title its game tree as a filesystem path instead.",
      uri);
  return -1;
}

void AndroidInitialize() {}
void AndroidShutdown() {}

}  // namespace filesystem
}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
