/**
 * @file        system/tee.cpp
 * @brief       Per-frame digest channel for comparing two runs of a title
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

// Like snapshot.cpp this depends only on the C++ library and xxHash so the
// host tests can compile it in. One global registry of channels (so EndFrame
// can publish all of them), one mutex per channel for the digest states, one
// mutex for the output file.

#include <rex/system/tee.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include <xxhash.h>

namespace rex::tee {

namespace {

struct Log {
  std::mutex mutex;
  FILE* file = nullptr;
  bool enabled = false;
  std::atomic<uint64_t> frame{0};
  std::vector<Channel*> channels;  // guarded by mutex
};

Log& GetLog() {
  static Log* log = [] {
    auto* l = new Log();
    const char* path = std::getenv("REX_TEE_LOG");
    if (path && *path) {
      l->file = std::fopen(path, "wb");
      l->enabled = l->file != nullptr;
      if (l->enabled) std::atexit([] { Flush(); });
    }
    return l;
  }();
  return *log;
}

struct KeyState {
  XXH3_state_t* state;
  uint64_t n;
};

}  // namespace

struct Channel::Impl {
  std::mutex mutex;
  uint64_t frame = 0;
  std::map<std::string, KeyState> keys;  // sorted: publish order is stable

  ~Impl() {
    for (auto& [k, ks] : keys) XXH3_freeState(ks.state);
  }

  // Caller holds mutex.
  void PublishLocked(Log& log, const char* name) {
    if (keys.empty()) return;
    std::string out;
    char line[512];
    for (auto& [key, ks] : keys) {
      uint64_t h = XXH3_64bits_digest(ks.state);
      std::snprintf(line, sizeof(line),
                    "{\"frame\":%llu,\"key\":\"%s/%s\",\"hash\":\"%016llx\",\"n\":%llu}\n",
                    static_cast<unsigned long long>(frame), name, key.c_str(),
                    static_cast<unsigned long long>(h), static_cast<unsigned long long>(ks.n));
      out += line;
      XXH3_freeState(ks.state);
    }
    keys.clear();
    std::lock_guard<std::mutex> lock(log.mutex);
    if (log.file) std::fwrite(out.data(), 1, out.size(), log.file);
  }
};

bool Enabled() { return GetLog().enabled; }

uint64_t CurrentFrame() { return GetLog().frame.load(std::memory_order_relaxed); }

void EndFrame() {
  Log& log = GetLog();
  if (!log.enabled) return;
  std::vector<Channel*> channels;
  {
    std::lock_guard<std::mutex> lock(log.mutex);
    channels = log.channels;
  }
  for (Channel* c : channels) c->Publish();
  log.frame.fetch_add(1, std::memory_order_relaxed);
  Flush();
}

void Flush() {
  Log& log = GetLog();
  std::lock_guard<std::mutex> lock(log.mutex);
  if (log.file) std::fflush(log.file);
}

Channel::Channel(const char* name) : name_(name), impl_(new Impl()) {
  Log& log = GetLog();
  std::lock_guard<std::mutex> lock(log.mutex);
  log.channels.push_back(this);
}

Channel::~Channel() {
  Publish();
  Log& log = GetLog();
  {
    std::lock_guard<std::mutex> lock(log.mutex);
    log.channels.erase(std::remove(log.channels.begin(), log.channels.end(), this),
                       log.channels.end());
  }
  delete impl_;
}

void Channel::Digest(uint64_t frame, std::string_view key, const void* bytes, size_t len) {
  Log& log = GetLog();
  if (!log.enabled) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (frame > impl_->frame) {
    impl_->PublishLocked(log, name_);
    impl_->frame = frame;
  }
  auto it = impl_->keys.find(std::string(key));
  if (it == impl_->keys.end()) {
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    it = impl_->keys.emplace(std::string(key), KeyState{st, 0}).first;
  }
  XXH3_64bits_update(it->second.state, bytes, len);
  it->second.n++;
}

void Channel::Publish() {
  Log& log = GetLog();
  if (!log.enabled) return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->PublishLocked(log, name_);
  // The next digest with no explicit frame lands in the shared frame counter's
  // current value; an explicit lower frame folds into whatever is current.
  uint64_t now = CurrentFrame();
  if (now > impl_->frame) impl_->frame = now;
}

}  // namespace rex::tee
