/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <bit>
#include <chrono>
#include <mutex>
#include <set>
#include <cstring>
#include <utility>

#include <rex/assert.h>
#include <rex/bit.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/graphics/shared_memory.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>

namespace rex::graphics {

SharedMemory::SharedMemory(memory::Memory& memory) : memory_(memory) {
  page_size_log2_ = rex::log2_ceil(uint32_t(rex::memory::page_size()));
}

SharedMemory::~SharedMemory() {
  ShutdownCommon();
}

void SharedMemory::InitializeCommon() {
  num_system_page_flags_ = ((kBufferSize >> page_size_log2_) + 63) / 64;
  system_page_flags_valid_ = std::vector<std::atomic<uint64_t>>(num_system_page_flags_);
  system_page_flags_valid_and_gpu_written_ =
      std::vector<std::atomic<uint64_t>>(num_system_page_flags_);
  for (uint32_t i = 0; i < num_system_page_flags_; ++i) {
    system_page_flags_valid_[i].store(0, std::memory_order_relaxed);
    system_page_flags_valid_and_gpu_written_[i].store(0, std::memory_order_relaxed);
  }

  memory_invalidation_callback_handle_ =
      memory_.RegisterPhysicalMemoryInvalidationCallback(MemoryInvalidationCallbackThunk, this);
}

void SharedMemory::InitializeSparseHostGpuMemory(uint32_t granularity_log2) {
  assert_true(granularity_log2 <= kBufferSizeLog2);
  assert_true(host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX);
  host_gpu_memory_sparse_granularity_log2_ = granularity_log2;
  host_gpu_memory_sparse_allocated_.resize(
      size_t(1) << (std::max(kBufferSizeLog2 - granularity_log2, uint32_t(6)) - 6));
}

void SharedMemory::ShutdownCommon() {
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, false);
  assert_true(global_watches_.empty());
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();

  if (memory_invalidation_callback_handle_ != nullptr) {
    memory_.UnregisterPhysicalMemoryInvalidationCallback(memory_invalidation_callback_handle_);
    memory_invalidation_callback_handle_ = nullptr;
  }

  if (host_gpu_memory_sparse_used_bytes_) {
    host_gpu_memory_sparse_used_bytes_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb", 0);
  }
  if (host_gpu_memory_sparse_allocations_) {
    host_gpu_memory_sparse_allocations_ = 0;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations", 0);
  }
  host_gpu_memory_sparse_allocated_.clear();
  host_gpu_memory_sparse_allocated_.shrink_to_fit();
  host_gpu_memory_sparse_granularity_log2_ = UINT32_MAX;

  invalidation_version_.fetch_add(1, std::memory_order_release);
  system_page_flags_valid_.clear();
  system_page_flags_valid_.shrink_to_fit();
  system_page_flags_valid_and_gpu_written_.clear();
  system_page_flags_valid_and_gpu_written_.shrink_to_fit();
  num_system_page_flags_ = 0;
}

void SharedMemory::InvalidateAllPages() {
  auto global_lock = global_critical_region_.Acquire();

  invalidation_version_.fetch_add(1, std::memory_order_release);
  for (size_t i = 0; i < system_page_flags_valid_.size(); ++i) {
    system_page_flags_valid_[i].store(0, std::memory_order_release);
    system_page_flags_valid_and_gpu_written_[i].store(0, std::memory_order_release);
  }
}

void SharedMemory::SetSystemPageBlocksValidWithGpuDataWritten() {
  auto global_lock = global_critical_region_.Acquire();

  // Pages that are valid only because the CPU uploaded them lose their valid
  // bit here, so the next frame re-reads them from guest memory.
  for (size_t i = 0; i < system_page_flags_valid_.size(); ++i) {
    system_page_flags_valid_[i].store(
        system_page_flags_valid_and_gpu_written_[i].load(std::memory_order_relaxed),
        std::memory_order_release);
  }
}

void SharedMemory::ClearCache() {
  // Keeping GPU-written data, so "invalidated by GPU".
  FireWatches(0, (kBufferSize - 1) >> page_size_log2_, true);
  // No watches now, so no references to the pools accessible by guest threads -
  // safe not to enter the global critical region.
  watch_node_first_free_ = nullptr;
  watch_node_current_pool_allocated_ = 0;
  for (WatchNode* pool : watch_node_pools_) {
    delete[] pool;
  }
  watch_node_pools_.clear();
  watch_range_first_free_ = nullptr;
  watch_range_current_pool_allocated_ = 0;
  for (WatchRange* pool : watch_range_pools_) {
    delete[] pool;
  }
  watch_range_pools_.clear();
  SetSystemPageBlocksValidWithGpuDataWritten();
}

SharedMemory::GlobalWatchHandle SharedMemory::RegisterGlobalWatch(GlobalWatchCallback callback,
                                                                  void* callback_context) {
  GlobalWatch* watch = new GlobalWatch;
  watch->callback = callback;
  watch->callback_context = callback_context;

  auto global_lock = global_critical_region_.Acquire();
  global_watches_.push_back(watch);

  return reinterpret_cast<GlobalWatchHandle>(watch);
}

void SharedMemory::UnregisterGlobalWatch(GlobalWatchHandle handle) {
  auto watch = reinterpret_cast<GlobalWatch*>(handle);

  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::find(global_watches_.begin(), global_watches_.end(), watch);
    assert_false(it == global_watches_.end());
    if (it != global_watches_.end()) {
      global_watches_.erase(it);
    }
  }

  delete watch;
}

SharedMemory::WatchHandle SharedMemory::WatchMemoryRange(uint32_t start, uint32_t length,
                                                         WatchCallback callback,
                                                         void* callback_context,
                                                         void* callback_data,
                                                         uint64_t callback_argument) {
  if (length == 0 || start >= kBufferSize) {
    return nullptr;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t watch_page_first = start >> page_size_log2_;
  uint32_t watch_page_last = (start + length - 1) >> page_size_log2_;
  uint32_t bucket_first = watch_page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  uint32_t bucket_last = watch_page_last << page_size_log2_ >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Allocate the range.
  WatchRange* range = watch_range_first_free_;
  if (range != nullptr) {
    watch_range_first_free_ = range->next_free;
  } else {
    if (watch_range_pools_.empty() || watch_range_current_pool_allocated_ >= kWatchRangePoolSize) {
      watch_range_pools_.push_back(new WatchRange[kWatchRangePoolSize]);
      watch_range_current_pool_allocated_ = 0;
    }
    range = &(watch_range_pools_.back()[watch_range_current_pool_allocated_++]);
  }
  range->callback = callback;
  range->callback_context = callback_context;
  range->callback_data = callback_data;
  range->callback_argument = callback_argument;
  range->page_first = watch_page_first;
  range->page_last = watch_page_last;

  // Allocate and link the nodes.
  WatchNode* node_previous = nullptr;
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_node_first_free_;
    if (node != nullptr) {
      watch_node_first_free_ = node->next_free;
    } else {
      if (watch_node_pools_.empty() || watch_node_current_pool_allocated_ >= kWatchNodePoolSize) {
        watch_node_pools_.push_back(new WatchNode[kWatchNodePoolSize]);
        watch_node_current_pool_allocated_ = 0;
      }
      node = &(watch_node_pools_.back()[watch_node_current_pool_allocated_++]);
    }
    node->range = range;
    node->range_node_next = nullptr;
    if (node_previous != nullptr) {
      node_previous->range_node_next = node;
    } else {
      range->node_first = node;
    }
    node_previous = node;
    node->bucket_node_previous = nullptr;
    node->bucket_node_next = watch_buckets_[i];
    if (watch_buckets_[i] != nullptr) {
      watch_buckets_[i]->bucket_node_previous = node;
    }
    watch_buckets_[i] = node;
  }

  return reinterpret_cast<WatchHandle>(range);
}

void SharedMemory::UnwatchMemoryRange(WatchHandle handle) {
  auto global_lock = global_critical_region_.Acquire();
  UnlinkWatchRange(reinterpret_cast<WatchRange*>(handle));
}

void SharedMemory::FireWatches(uint32_t page_first, uint32_t page_last, bool invalidated_by_gpu) {
  uint32_t address_first = page_first << page_size_log2_;
  uint32_t address_last = (page_last << page_size_log2_) + ((1 << page_size_log2_) - 1);
  uint32_t bucket_first = address_first >> kWatchBucketSizeLog2;
  uint32_t bucket_last = address_last >> kWatchBucketSizeLog2;

  auto global_lock = global_critical_region_.Acquire();

  // Fire global watches.
  for (const auto global_watch : global_watches_) {
    global_watch->callback(global_lock, global_watch->callback_context, address_first, address_last,
                           invalidated_by_gpu);
  }

  // Fire per-range watches.
  for (uint32_t i = bucket_first; i <= bucket_last; ++i) {
    WatchNode* node = watch_buckets_[i];
    while (node != nullptr) {
      WatchRange* range = node->range;
      // Store the next node now since when the callback is triggered, the links
      // will be broken.
      node = node->bucket_node_next;
      if (page_first <= range->page_last && page_last >= range->page_first) {
        range->callback(global_lock, range->callback_context, range->callback_data,
                        range->callback_argument, invalidated_by_gpu);
        UnlinkWatchRange(range);
      }
    }
  }
}

void SharedMemory::RangeWrittenByGpu(uint32_t start, uint32_t length) {
  if (length == 0 || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t end = start + length - 1;
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = end >> page_size_log2_;

  // Trigger modification callbacks so, for instance, resolved data is loaded to
  // the texture.
  FireWatches(page_first, page_last, true);

  // Mark the range as valid (so pages are not reuploaded until modified by the
  // CPU) and watch it so the CPU can reuse it and this will be caught.
  MakeRangeValid(start, length, true);
}

bool SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                    uint32_t length_allocations) {
  assert_always(
      "Sparse host GPU memory allocation has been initialized, but the "
      "implementation doesn't provide AllocateSparseHostGpuMemoryRange");
  return false;
}

bool SharedMemory::MakeRangeValid(uint32_t start, uint32_t length, bool written_by_gpu,
                                  bool arm_watches) {
  if (length == 0 || start >= kBufferSize) {
    return false;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t last = start + length - 1;
  uint32_t valid_page_first = start >> page_size_log2_;
  uint32_t valid_page_last = last >> page_size_log2_;
  uint32_t valid_block_first = valid_page_first >> 6;
  uint32_t valid_block_last = valid_page_last >> 6;
  bool enable_callbacks = false;
  uint32_t made_valid = 0;

  // No lock: every bit change here is a single atomic read-modify-write, and
  // this runs once per upload on the draw path, where taking the global
  // critical region - which every guest thread also wants - was the dominant
  // cost. The window against a concurrent invalidation is the same one the
  // lock already left open, because the copy into the host buffer happens
  // outside it either way.
  for (uint32_t i = valid_block_first; i <= valid_block_last; ++i) {
    uint64_t valid_bits = UINT64_MAX;
    if (i == valid_block_first) {
      valid_bits &= ~((uint64_t(1) << (valid_page_first & 63)) - 1);
    }
    if (i == valid_block_last && (valid_page_last & 63) != 63) {
      valid_bits &= (uint64_t(1) << ((valid_page_last & 63) + 1)) - 1;
    }
    // Already-valid pages already have CPU write callbacks armed. Repeated
    // memexports (for example one AABB per draw into the same pool) need not
    // walk all three physical heaps again just to re-arm the same pages.
    const uint64_t previous_valid =
        system_page_flags_valid_[i].fetch_or(valid_bits, std::memory_order_acq_rel);
    enable_callbacks |= (previous_valid & valid_bits) != valid_bits;
    made_valid += uint32_t(std::popcount(~previous_valid & valid_bits));
    if (written_by_gpu) {
      system_page_flags_valid_and_gpu_written_[i].fetch_or(valid_bits, std::memory_order_release);
    } else {
      system_page_flags_valid_and_gpu_written_[i].fetch_and(~valid_bits, std::memory_order_release);
    }
  }

  {
    static int stats = -1;
    if (stats < 0) {
      const char* value = getenv("REX_SHMEM_STATS");
      stats = (value && *value == '1') ? 1 : 0;
    }
    if (stats) {
      static std::atomic<uint64_t> calls{0}, pages{0};
      static std::atomic<uint64_t> next_report{0};
      calls.fetch_add(1, std::memory_order_relaxed);
      pages.fetch_add(made_valid, std::memory_order_relaxed);
      const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
      uint64_t due = next_report.load(std::memory_order_relaxed);
      if (now >= due && next_report.compare_exchange_strong(due, now + 2)) {
        REXLOG_INFO("[SHMEM] MakeRangeValid: {} calls, {} pages invalid->valid",
                    calls.exchange(0), pages.exchange(0));
      }
    }
  }

  if (enable_callbacks && arm_watches) {
    ArmWriteWatches(valid_page_first << page_size_log2_,
                    (valid_page_last - valid_page_first + 1) << page_size_log2_);
  }
  return enable_callbacks;
}

void SharedMemory::ArmWriteWatches(uint32_t start, uint32_t length) {
  const uint32_t valid_page_first = start >> page_size_log2_;
  const uint32_t valid_page_last = (start + length - 1) >> page_size_log2_;
  if (memory_invalidation_callback_handle_) {
    // Arming the guest write watch is an mprotect across the physical heaps;
    // measure it, because it lands on the draw path once per page that becomes
    // valid. Off unless REX_SHMEM_STATS=1.
    static int arm_stats = -1;
    if (arm_stats < 0) {
      const char* value = getenv("REX_SHMEM_STATS");
      arm_stats = (value && *value == '1') ? 1 : 0;
    }
    static std::atomic<uint64_t> arm_ns{0}, arms{0}, next_arm_report{0};
    const auto arm_start = arm_stats ? std::chrono::steady_clock::now()
                                     : std::chrono::steady_clock::time_point{};
    if (arm_stats) {
      arms.fetch_add(1, std::memory_order_relaxed);
    }
    memory().EnablePhysicalMemoryAccessCallbacks(
        valid_page_first << page_size_log2_,
        (valid_page_last - valid_page_first + 1) << page_size_log2_, true, false);
    if (arm_stats) {
      arm_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - arm_start)
                                    .count()),
                       std::memory_order_relaxed);
      const uint64_t now_s = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
      uint64_t due = next_arm_report.load(std::memory_order_relaxed);
      if (now_s >= due && next_arm_report.compare_exchange_strong(due, now_s + 2)) {
        const uint64_t n = std::max<uint64_t>(arms.exchange(0), 1);
        REXLOG_INFO("[SHMEM] arming write watches: {} calls, {:.1f} us each", n,
                    double(arm_ns.exchange(0)) / 1000.0 / double(n));
      }
    }
  }
}

void SharedMemory::UnlinkWatchRange(WatchRange* range) {
  uint32_t bucket = range->page_first << page_size_log2_ >> kWatchBucketSizeLog2;
  WatchNode* node = range->node_first;
  while (node != nullptr) {
    WatchNode* node_next = node->range_node_next;
    if (node->bucket_node_previous != nullptr) {
      node->bucket_node_previous->bucket_node_next = node->bucket_node_next;
    } else {
      watch_buckets_[bucket] = node->bucket_node_next;
    }
    if (node->bucket_node_next != nullptr) {
      node->bucket_node_next->bucket_node_previous = node->bucket_node_previous;
    }
    node->next_free = watch_node_first_free_;
    watch_node_first_free_ = node;
    node = node_next;
    ++bucket;
  }
  range->next_free = watch_range_first_free_;
  watch_range_first_free_ = range;
}

bool SharedMemory::RequestRanges(const std::pair<uint32_t, uint32_t>* ranges, size_t count) {
  if (ranges == nullptr || !count) {
    return true;
  }

  // Some texture or buffer is empty, for example - safe to draw in this case.
  std::vector<std::pair<uint32_t, uint32_t>> merged_ranges;
  merged_ranges.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    uint32_t start = ranges[i].first;
    uint32_t length = ranges[i].second;
    if (!length) {
      continue;
    }
    if (start > kBufferSize || (kBufferSize - start) < length) {
      return false;
    }
    merged_ranges.emplace_back(start, length);
  }
  if (merged_ranges.empty()) {
    return true;
  }

  SCOPE_profile_cpu_f("gpu");

  std::sort(merged_ranges.begin(), merged_ranges.end(),
            [](const std::pair<uint32_t, uint32_t>& a, const std::pair<uint32_t, uint32_t>& b) {
              return a.first < b.first;
            });
  size_t merged_write = 0;
  for (size_t i = 1; i < merged_ranges.size(); ++i) {
    std::pair<uint32_t, uint32_t>& range_previous = merged_ranges[merged_write];
    const std::pair<uint32_t, uint32_t>& range_current = merged_ranges[i];
    uint64_t previous_end = uint64_t(range_previous.first) + uint64_t(range_previous.second);
    uint64_t current_start = uint64_t(range_current.first);
    if (current_start <= previous_end) {
      uint64_t current_end = current_start + uint64_t(range_current.second);
      if (current_end > previous_end) {
        range_previous.second = uint32_t(current_end - uint64_t(range_previous.first));
      }
    } else {
      merged_ranges[++merged_write] = range_current;
    }
  }
  merged_ranges.resize(merged_write + 1);

  for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
    if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
      return false;
    }
  }

  upload_ranges_.clear();
  auto append_upload_range = [this](uint32_t page_start, uint32_t page_count) {
    if (!page_count) {
      return;
    }
    if (!upload_ranges_.empty()) {
      std::pair<uint32_t, uint32_t>& last_upload_range = upload_ranges_.back();
      if (last_upload_range.first + last_upload_range.second == page_start) {
        last_upload_range.second += page_count;
        return;
      }
    }
    upload_ranges_.emplace_back(page_start, page_count);
  };
  {
    // No lock while working out what to upload: the bitmap is atomic, only this
    // thread uploads, and a page invalidated concurrently is either seen here
    // (uploaded now) or on the next request - the same guarantee holding the
    // global critical region gave, since it was released before the upload
    // anyway. Holding it here cost ~40 us per draw on a title issuing thousands
    // of draws, because every guest thread wants the same lock.
    for (const std::pair<uint32_t, uint32_t>& range : merged_ranges) {
      uint32_t page_first = range.first >> page_size_log2_;
      uint32_t page_last = (range.first + range.second - 1) >> page_size_log2_;
      uint32_t block_first = page_first >> 6;
      uint32_t block_last = page_last >> 6;
      uint32_t range_start = UINT32_MAX;
      for (uint32_t i = block_first; i <= block_last; ++i) {
        uint64_t block_valid = system_page_flags_valid_[i].load(std::memory_order_relaxed);
        // Consider pages in the block outside the requested range valid.
        if (i == block_first) {
          uint64_t block_before = (uint64_t(1) << (page_first & 63)) - 1;
          block_valid |= block_before;
        }
        if (i == block_last && (page_last & 63) != 63) {
          uint64_t block_inside = (uint64_t(1) << ((page_last & 63) + 1)) - 1;
          block_valid |= ~block_inside;
        }

        while (true) {
          uint32_t block_page;
          if (range_start == UINT32_MAX) {
            // Check if need to open a new range.
            if (!rex::bit_scan_forward(~block_valid, &block_page)) {
              break;
            }
            range_start = (i << 6) + block_page;
          } else {
            // Check if need to close the range.
            // Ignore the valid pages before the beginning of the range.
            uint64_t block_valid_from_start = block_valid;
            if (i == (range_start >> 6)) {
              block_valid_from_start &= ~((uint64_t(1) << (range_start & 63)) - 1);
            }
            if (!rex::bit_scan_forward(block_valid_from_start, &block_page)) {
              break;
            }
            append_upload_range(range_start, (i << 6) + block_page - range_start);
            // In the next iteration within this block, consider this range
            // valid since it has been queued for upload.
            block_valid |= (uint64_t(1) << block_page) - 1;
            range_start = UINT32_MAX;
          }
        }
      }
      if (range_start != UINT32_MAX) {
        append_upload_range(range_start, page_last + 1 - range_start);
      }
    }
  }

  COUNT_profile_set("gpu/shared_memory/request_ranges_count", uint32_t(count));
  COUNT_profile_set("gpu/shared_memory/request_ranges_merged_count",
                    uint32_t(merged_ranges.size()));
  COUNT_profile_set("gpu/shared_memory/request_ranges_upload_count",
                    uint32_t(upload_ranges_.size()));

  if (upload_ranges_.empty()) {
    return true;
  }

  {
    static int stats = -1;
    if (stats < 0) {
      const char* value = getenv("REX_SHMEM_STATS");
      stats = (value && *value == '1') ? 1 : 0;
    }
    if (stats) {
      static std::atomic<uint64_t> uploads{0}, upload_pages{0};
      static std::atomic<uint64_t> next_report{0};
      uint64_t pages = 0;
      static std::mutex unique_lock;
      static std::set<uint32_t> unique_pages;
      for (const auto& range : upload_ranges_) {
        pages += range.second;
        std::lock_guard<std::mutex> guard(unique_lock);
        for (uint32_t page = range.first; page < range.first + range.second; ++page) {
          unique_pages.insert(page);
        }
      }
      uploads.fetch_add(upload_ranges_.size(), std::memory_order_relaxed);
      upload_pages.fetch_add(pages, std::memory_order_relaxed);
      const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
      uint64_t due = next_report.load(std::memory_order_relaxed);
      if (now >= due && next_report.compare_exchange_strong(due, now + 2)) {
        const uint64_t page_count = upload_pages.exchange(0);
        size_t unique_count;
        {
          std::lock_guard<std::mutex> guard(unique_lock);
          unique_count = unique_pages.size();
          unique_pages.clear();
        }
        REXLOG_INFO("[SHMEM] uploads: {} regions, {} pages ({} MB), {} distinct pages",
                    uploads.exchange(0), page_count, (page_count << page_size_log2_) >> 20,
                    unique_count);
      }
    }
  }

  uint64_t uploaded_pages = 0;
  for (const auto& range : upload_ranges_) {
    uploaded_pages += range.second;
  }
  const auto upload_start = std::chrono::steady_clock::now();
  const bool uploaded = UploadRanges(upload_ranges_);
  upload_events_.fetch_add(1, std::memory_order_relaxed);
  upload_pages_.fetch_add(uploaded_pages, std::memory_order_relaxed);
  upload_ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - upload_start)
                                    .count()),
                       std::memory_order_relaxed);
  return uploaded;
}

bool SharedMemory::FlushDeferredRanges() {
  if (deferred_ranges_.empty()) {
    return true;
  }
  // Ranges that are already resident cost only the lock-free bitmap check and
  // the sparse-allocation check; only the rest go through the sorting,
  // merging, locking upload path - and they go through it together, so their
  // uploads share one barrier.
  bool result = true;
  size_t needs_upload = 0;
  for (const std::pair<uint32_t, uint32_t>& range : deferred_ranges_) {
    if (RangeResident(range.first, range.second)) {
      if (!EnsureHostGpuMemoryAllocated(range.first, range.second)) {
        result = false;
      }
      continue;
    }
    deferred_ranges_[needs_upload++] = range;
  }
  if (needs_upload) {
    result = RequestRanges(deferred_ranges_.data(), needs_upload) && result;
  }
  deferred_ranges_.clear();
  return result;
}

void SharedMemory::TakeUploadStats(uint64_t& events_out, uint64_t& pages_out,
                                   double& milliseconds_out) {
  events_out = upload_events_.exchange(0, std::memory_order_relaxed);
  pages_out = upload_pages_.exchange(0, std::memory_order_relaxed);
  milliseconds_out = double(upload_ns_.exchange(0, std::memory_order_relaxed)) / 1000000.0;
}

bool SharedMemory::RangeResident(uint32_t start, uint32_t length) const {
  if (!length) {
    return true;
  }
  if (start >= kBufferSize || length > kBufferSize - start) {
    return false;
  }
  const uint32_t first_page = start >> page_size_log2_;
  const uint32_t last_page = (start + length - 1) >> page_size_log2_;
  // No lock: the bitmap is atomic, and this check is taken thousands of times
  // per frame by the draw path while guest threads hold the global critical
  // region for their own memory work. Taking it here made residency checking
  // the single most expensive thing in the draw path.
  for (uint32_t block = first_page >> 6; block <= (last_page >> 6); ++block) {
    uint64_t mask = UINT64_MAX;
    if (block == (first_page >> 6)) mask &= UINT64_MAX << (first_page & 63);
    if (block == (last_page >> 6)) mask &= UINT64_MAX >> (63 - (last_page & 63));
    if ((system_page_flags_valid_[block].load(std::memory_order_acquire) & mask) != mask) {
      return false;
    }
  }
  return true;
}

bool SharedMemory::RequestRange(uint32_t start, uint32_t length) {
  // Most vertex-buffer requests are for a single already-resident range.
  // Avoid allocating/sorting a temporary range vector and assembling an upload
  // list for this overwhelmingly common case. Keep the same bounds and sparse
  // allocation contract as RequestRanges.
  if (!length) return true;
  if (start >= kBufferSize || length > kBufferSize - start) return false;
  uint32_t first_page = start >> page_size_log2_;
  uint32_t last_page = (start + length - 1) >> page_size_log2_;
  // No lock: the bitmap is atomic, and this check is taken thousands of times
  // per frame by the draw path while guest threads hold the global critical
  // region for their own memory work. Taking it here made residency checking
  // the single most expensive thing in the draw path.
  bool valid = true;
  for (uint32_t block = first_page >> 6; block <= (last_page >> 6); ++block) {
    uint64_t mask = UINT64_MAX;
    if (block == (first_page >> 6)) mask &= UINT64_MAX << (first_page & 63);
    if (block == (last_page >> 6)) mask &= UINT64_MAX >> (63 - (last_page & 63));
    if ((system_page_flags_valid_[block].load(std::memory_order_acquire) & mask) != mask) {
      valid = false;
      break;
    }
  }
  // Which half of this costs what: a resident range that only needs the
  // allocation check is cheap; a range that has to go through RequestRanges
  // uploads. Off unless REX_SHMEM_STATS=1.
  {
    static int stats = -1;
    if (stats < 0) {
      const char* value = getenv("REX_SHMEM_STATS");
      stats = (value && *value == '1') ? 1 : 0;
    }
    if (stats) {
      static std::atomic<uint64_t> fast{0}, slow{0}, pages{0};
      static std::atomic<uint64_t> next_report{0};
      (valid ? fast : slow).fetch_add(1, std::memory_order_relaxed);
      pages.fetch_add(last_page - first_page + 1, std::memory_order_relaxed);
      const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
      uint64_t due = next_report.load(std::memory_order_relaxed);
      if (now >= due && next_report.compare_exchange_strong(due, now + 2)) {
        REXLOG_INFO("[SHMEM] RequestRange: {} resident, {} needing upload, {} pages scanned",
                    fast.exchange(0), slow.exchange(0), pages.exchange(0));
      }
    }
  }
  if (valid) return EnsureHostGpuMemoryAllocated(start, length);
  std::pair<uint32_t, uint32_t> range(start, length);
  return RequestRanges(&range, 1);
}

REXCVAR_DEFINE_UINT32(
    gpu_invalidation_widening_pages, 64, "GPU",
    "How many extra shared-memory pages either side of a guest write may be invalidated with it. "
    "Larger means fewer guest access-violation traps but more re-uploading; 64 (the block) is the "
    "legacy behaviour, 0 invalidates only what the guest actually wrote.");

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallbackThunk(
    void* context_ptr, uint32_t physical_address_start, uint32_t length, bool exact_range) {
  return reinterpret_cast<SharedMemory*>(context_ptr)
      ->MemoryInvalidationCallback(physical_address_start, length, exact_range);
}

std::pair<uint32_t, uint32_t> SharedMemory::MemoryInvalidationCallback(
    uint32_t physical_address_start, uint32_t length, bool exact_range) {
  if (length == 0 || physical_address_start >= kBufferSize) {
    return std::make_pair(uint32_t(0), UINT32_MAX);
  }
  length = std::min(length, kBufferSize - physical_address_start);
  uint32_t physical_address_last = physical_address_start + (length - 1);

  uint32_t page_first = physical_address_start >> page_size_log2_;
  uint32_t page_last = physical_address_last >> page_size_log2_;
  uint32_t block_first = page_first >> 6;
  uint32_t block_last = page_last >> 6;

  auto global_lock = global_critical_region_.Acquire();

  if (!exact_range) {
    // Check if a somewhat wider range (up to 256 KB with 4 KB pages) can be
    // invalidated - if no GPU-written data nearby that was not intended to be
    // invalidated since it's not in sync with CPU memory and can't be
    // reuploaded. It's a lot cheaper to upload some excess data than to catch
    // access violations - with 4 KB callbacks, 58410824 (being a
    // software-rendered game) runs at 4 FPS on Intel Core i7-3770, with 64 KB,
    // the CPU game code takes 3 ms to run per frame, but with 256 KB, it's
    // 0.7 ms.
    if (page_first & 63) {
      uint64_t gpu_written_start =
          system_page_flags_valid_and_gpu_written_[block_first].load(std::memory_order_relaxed);
      gpu_written_start &= (uint64_t(1) << (page_first & 63)) - 1;
      page_first = (page_first & ~uint32_t(63)) + (64 - rex::lzcnt(gpu_written_start));
    }
    if ((page_last & 63) != 63) {
      uint64_t gpu_written_end =
          system_page_flags_valid_and_gpu_written_[block_last].load(std::memory_order_relaxed);
      gpu_written_end &= ~((uint64_t(1) << ((page_last & 63) + 1)) - 1);
      page_last =
          (page_last & ~uint32_t(63)) + (std::max(rex::tzcnt(gpu_written_end), uint8_t(1)) - 1);
    }
    // Widening trades upload bandwidth for guest access-violation traps, and
    // the default trade (out to the 64-page block, i.e. 256 KB) is only right
    // when the invalidated data is small or rarely re-read. A title that
    // streams geometry pays it as pure amplification: Midnight Club LA dirties
    // ~1 page per callback and re-uploads 64, turning 1.7 MB/s of guest writes
    // into 110 MB/s of shared-memory uploads. Clamp how far the range may grow.
    const uint32_t widening_limit = REXCVAR_GET(gpu_invalidation_widening_pages);
    if (widening_limit < 64) {
      const uint32_t requested_first = physical_address_start >> page_size_log2_;
      const uint32_t requested_last = physical_address_last >> page_size_log2_;
      page_first = std::max(page_first, requested_first > widening_limit
                                            ? requested_first - widening_limit
                                            : uint32_t(0));
      page_last = std::min(page_last, requested_last + widening_limit);
    }
  }

  uint32_t transitions = 0;
  for (uint32_t i = block_first; i <= block_last; ++i) {
    uint64_t invalidate_bits = UINT64_MAX;
    if (i == block_first) {
      invalidate_bits &= ~((uint64_t(1) << (page_first & 63)) - 1);
    }
    if (i == block_last && (page_last & 63) != 63) {
      invalidate_bits &= (uint64_t(1) << ((page_last & 63) + 1)) - 1;
    }
    const uint64_t previous_valid = system_page_flags_valid_[i].load(std::memory_order_relaxed);
    transitions += uint32_t(std::popcount(previous_valid & invalidate_bits));
    system_page_flags_valid_[i].store(previous_valid & ~invalidate_bits, std::memory_order_release);
    system_page_flags_valid_and_gpu_written_[i].store(
        system_page_flags_valid_and_gpu_written_[i].load(std::memory_order_relaxed) &
            ~invalidate_bits,
        std::memory_order_release);
  }
  invalidation_version_.fetch_add(1, std::memory_order_release);

  {
    static int stats = -1;
    if (stats < 0) {
      const char* value = getenv("REX_SHMEM_STATS");
      stats = (value && *value == '1') ? 1 : 0;
    }
    if (stats) {
      static std::atomic<uint64_t> calls{0}, asked{0}, widened{0}, flips{0};
      static std::atomic<uint64_t> next_report{0};
      calls.fetch_add(1, std::memory_order_relaxed);
      asked.fetch_add((length + (uint32_t(1) << page_size_log2_) - 1) >> page_size_log2_,
                      std::memory_order_relaxed);
      widened.fetch_add(page_last - page_first + 1, std::memory_order_relaxed);
      flips.fetch_add(transitions, std::memory_order_relaxed);
      const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
      uint64_t due = next_report.load(std::memory_order_relaxed);
      if (now >= due && next_report.compare_exchange_strong(due, now + 2)) {
        REXLOG_INFO(
            "[SHMEM] invalidations: {} calls, {} pages asked, {} pages in range, {} valid->invalid",
            calls.exchange(0), asked.exchange(0), widened.exchange(0), flips.exchange(0));
      }
    }
  }

  FireWatches(page_first, page_last, false);

  return std::make_pair(page_first << page_size_log2_, (page_last - page_first + 1)
                                                           << page_size_log2_);
}

bool SharedMemory::EnsureHostGpuMemoryAllocated(uint32_t start, uint32_t length) {
  if (host_gpu_memory_sparse_granularity_log2_ == UINT32_MAX) {
    return true;
  }
  if (!length) {
    return true;
  }
  if (start > kBufferSize || (kBufferSize - start) < length) {
    return false;
  }
  uint32_t page_first = start >> page_size_log2_;
  uint32_t page_last = (start + length - 1) >> page_size_log2_;
  uint32_t allocation_first =
      page_first << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  uint32_t allocation_last =
      page_last << page_size_log2_ >> host_gpu_memory_sparse_granularity_log2_;
  while (true) {
    std::pair<size_t, size_t> allocation_range =
        rex::bit::GetNextRangeUnset(host_gpu_memory_sparse_allocated_.data(), allocation_first,
                                    allocation_last - allocation_first + 1);
    if (!allocation_range.second) {
      break;
    }
    if (!AllocateSparseHostGpuMemoryRange(uint32_t(allocation_range.first),
                                          uint32_t(allocation_range.second))) {
      return false;
    }
    rex::bit::SetRange(host_gpu_memory_sparse_allocated_.data(), allocation_range.first,
                       allocation_range.second);
    ++host_gpu_memory_sparse_allocations_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_allocations",
                      host_gpu_memory_sparse_allocations_);
    host_gpu_memory_sparse_used_bytes_ += uint32_t(allocation_range.second)
                                          << host_gpu_memory_sparse_granularity_log2_;
    COUNT_profile_set("gpu/shared_memory/host_gpu_memory_sparse_used_mb",
                      (host_gpu_memory_sparse_used_bytes_ + ((1 << 20) - 1)) >> 20);
    allocation_first = uint32_t(allocation_range.first + allocation_range.second);
  }
  return true;
}

}  // namespace rex::graphics
