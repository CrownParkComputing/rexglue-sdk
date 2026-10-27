#pragma once

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace xn::gfx {

// Diagnostic command ranges, not additive GPU busy-time measurements. The
// caller owns synchronization: BeginBatch follows an existing slot fence wait,
// Collect follows an existing fence/device-idle wait, and Destroy follows idle.
// This collector never waits, submits, or adds a memory/execution barrier.
class GpuWorkProfile {
 public:
  static constexpr uint32_t kSlots = 2;
  static constexpr uint32_t kScopesPerSlot = 4096;
  static constexpr uint32_t kQueriesPerSlot = kScopesPerSlot * 2 + 2;
  struct Token {
    uint64_t serial = 0;
    uint32_t slot = UINT32_MAX, index = UINT32_MAX;
    explicit constexpr operator bool() const {
      return serial && slot < kSlots && index < kScopesPerSlot;
    }
  };

  GpuWorkProfile();
  ~GpuWorkProfile();
  GpuWorkProfile(const GpuWorkProfile&) = delete;
  GpuWorkProfile& operator=(const GpuWorkProfile&) = delete;

  // pipeline_statistics requires the device's pipelineStatisticsQuery feature.
  // Only exact "draw_pass" scopes collect it. Begin/End those scopes outside
  // dynamic rendering, so the query spans its BeginRendering/EndRendering.
  bool Init(VkDevice device, uint32_t valid_bits, float timestamp_period,
            const char* output_path, bool pipeline_statistics = false);
  // The caller enables the extension and checks that host_domain is supported.
  // CLOCK_MONOTONIC calibration shares the batch CPU timestamps' clock/epoch.
  void Calibrate(PFN_vkGetCalibratedTimestampsEXT fn, VkTimeDomainEXT host_domain);
  void Destroy();
  void BeginBatch(VkCommandBuffer cb, uint32_t slot, uint64_t batch,
                  uint64_t guest_frame);
  void EndBatch(VkCommandBuffer cb);
  void Collect(uint32_t slot, uint64_t retired_guest_frame);
  Token Begin(VkCommandBuffer cb, const char* category, uint64_t guest_frame,
              uint32_t resource = 0, uint32_t width = 0, uint32_t height = 0,
              uint32_t samples = 0, uint64_t bytes = 0);
  void End(VkCommandBuffer cb, Token token);
  // count is submitted vertex/index references, not unique shader invocations.
  void AddDraw(Token token, uint32_t count);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class GpuWorkScope {
 public:
  GpuWorkScope(GpuWorkProfile* profile, VkCommandBuffer cb, const char* category,
               uint64_t guest_frame, uint32_t resource = 0, uint32_t width = 0,
               uint32_t height = 0, uint32_t samples = 0, uint64_t bytes = 0)
      : profile_(profile), cb_(cb) {
    if (profile_) token_ = profile_->Begin(cb, category, guest_frame, resource,
                                          width, height, samples, bytes);
  }
  ~GpuWorkScope() { End(); }
  GpuWorkScope(const GpuWorkScope&) = delete;
  GpuWorkScope& operator=(const GpuWorkScope&) = delete;
  GpuWorkProfile::Token token() const { return token_; }
  void AddDraw(uint32_t count) {
    if (profile_) profile_->AddDraw(token_, count);
  }
  void End() {
    if (profile_ && token_) profile_->End(cb_, token_);
    token_ = {};
  }

 private:
  GpuWorkProfile* profile_ = nullptr;
  VkCommandBuffer cb_ = VK_NULL_HANDLE;
  GpuWorkProfile::Token token_;
};

}  // namespace xn::gfx
