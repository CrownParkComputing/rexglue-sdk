#include "gpu_work_profile.h"

#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <type_traits>
#include <unistd.h>

namespace xn::gfx {
namespace {
constexpr uint32_t kNone = UINT32_MAX;
constexpr uint32_t kPipelineCounters = 6;
constexpr VkQueryPipelineStatisticFlags kPipelineStatistics =
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
constexpr std::array<const char*, kPipelineCounters> kPipelineNames = {
    "ia_vertices", "ia_primitives", "vs_invocations", "clip_invocations",
    "clip_primitives", "fs_invocations"};
std::atomic<uint64_t> next_session{0};

void JsonString(FILE* file, const char* text) {
  std::fputc('"', file);
  for (const auto* at = reinterpret_cast<const unsigned char*>(text); *at; ++at) {
    switch (*at) {
      case '"': std::fputs("\\\"", file); break;
      case '\\': std::fputs("\\\\", file); break;
      case '\n': std::fputs("\\n", file); break;
      case '\r': std::fputs("\\r", file); break;
      case '\t': std::fputs("\\t", file); break;
      default:
        // Category labels are bytes. Escaping non-ASCII also keeps a label
        // truncated in the middle of UTF-8 a valid JSON diagnostic record.
        if (*at < 0x20 || *at >= 0x7F) std::fprintf(file, "\\u%04x", unsigned(*at));
        else std::fputc(*at, file);
    }
  }
  std::fputc('"', file);
}
void U64(FILE* file, uint64_t value) {
  std::fprintf(file, "%llu", static_cast<unsigned long long>(value));
}
bool MonotonicNs(uint64_t& value) {
  timespec stamp{};
  if (clock_gettime(CLOCK_MONOTONIC, &stamp) != 0) return false;
  value = uint64_t(stamp.tv_sec) * 1000000000ull + uint64_t(stamp.tv_nsec);
  return true;
}
}  // namespace

struct GpuWorkProfile::Impl {
  struct Scope {
    char category[64]{};
    uint64_t frame = 0, bytes = 0, draws = 0, vertices = 0;
    uint32_t resource = 0, width = 0, height = 0, samples = 0;
    uint32_t begin_query = kNone, end_query = kNone;
    uint32_t stats_query = kNone;
    bool stats_requested = false, stats_ended = false;
    const char* stats_error = nullptr;
  };
  struct Slot {
    std::array<Scope, kScopesPerSlot> scopes;
    std::array<uint64_t, kQueriesPerSlot> ticks{};
    std::array<std::array<uint64_t, kPipelineCounters>, kScopesPerSlot> pipeline_stats{};
    VkCommandBuffer cb = VK_NULL_HANDLE;
    uint64_t serial = 0, batch = 0, guest_frame = 0;
    uint64_t cpu_begin_ns = 0, cpu_end_ns = 0;
    bool cpu_begin_valid = false, cpu_end_valid = false;
    uint32_t count = 0, queries = 0, root_begin = kNone, root_end = kNone;
    uint32_t dropped = 0, invalid_tokens = 0, cross_batch = 0, category_truncated = 0;
    uint32_t stats_queries = 0, stats_collected = 0, active_stats_scope = kNone;
    uint32_t stats_invalid = 0, stats_dropped = 0;
    const char* stats_error = nullptr;
    bool stats_available = false;
    bool pending = false, recording = false;
  };
  VkDevice device = VK_NULL_HANDLE;
  VkQueryPool pool = VK_NULL_HANDLE;
  VkQueryPool stats_pool = VK_NULL_HANDLE;
  FILE* output = nullptr;
  std::array<Slot, kSlots> slots;
  uint32_t active_slot = kNone, valid_bits = 0;
  float period = 0;
  uint64_t serial = 0;
  char session[96]{};
  char period_text[32]{};
  bool io_reported = false;

  void CheckOutput() {
    if (output && std::ferror(output) && !io_reported) {
      std::fputs("gpu-work-profile: diagnostic output write failed\n", stderr);
      io_reported = true;
    }
  }
  void Error(const char* operation, const char* reason, Token token = {}) {
    Slot* owner = active_slot < kSlots ? &slots[active_slot] : nullptr;
    if (!owner && token.slot < kSlots && slots[token.slot].pending &&
        slots[token.slot].serial == token.serial) owner = &slots[token.slot];
    if (owner) {
      ++owner->invalid_tokens;
      if (token.serial && (token.serial != owner->serial || token.slot != active_slot))
        ++owner->cross_batch;
    }
    std::fputs("{\"type\":\"gpu_work_error\",\"session\":", output);
    JsonString(output, session);
    if (owner) {
      std::fputs(",\"batch\":", output); U64(output, owner->batch);
      std::fputs(",\"guest_frame_start\":", output); U64(output, owner->guest_frame);
    }
    std::fputs(",\"operation\":", output); JsonString(output, operation);
    std::fputs(",\"error\":", output); JsonString(output, reason);
    std::fputs(",\"token_serial\":", output); U64(output, token.serial);
    std::fprintf(output, ",\"token_slot\":%u,\"token_index\":%u}\n", token.slot, token.index);
    CheckOutput();
  }
  bool Validate(Token token, const char* operation, VkCommandBuffer cb,
                Scope*& scope) {
    if (!token.serial) return false;  // normal default/capacity-drop token
    if (!token || active_slot != token.slot) {
      Error(operation, "inactive_or_cross_batch_token", token); return false;
    }
    auto& slot = slots[token.slot];
    if (!slot.pending || !slot.recording || slot.serial != token.serial ||
        token.index >= slot.count || slot.cb != cb) {
      Error(operation, "command_buffer_or_batch_mismatch", token); return false;
    }
    scope = &slot.scopes[token.index];
    if (scope->end_query != kNone) {
      Error(operation, "scope_already_ended", token); return false;
    }
    return true;
  }
  void Tick(FILE* file, const Slot& slot, uint32_t query, bool available) {
    if (!available || query == kNone || query >= slot.queries) std::fputs("null", file);
    else U64(file, slot.ticks[query]);
  }
  void Write(uint32_t index, uint64_t retired_frame, bool available, const char* error) {
    const auto& slot = slots[index];
    uint32_t incomplete = 0;
    uint32_t stats_incomplete = 0;
    for (uint32_t i = 0; i < slot.count; ++i)
      incomplete += slot.scopes[i].end_query == kNone;
    for (uint32_t i = 0; i < slot.count; ++i)
      stats_incomplete += slot.scopes[i].stats_requested &&
          (slot.scopes[i].stats_query == kNone || !slot.scopes[i].stats_ended);
    std::fputs("{\"type\":\"gpu_work_batch\",\"session\":", output);
    JsonString(output, session);
    std::fputs(",\"batch\":", output); U64(output, slot.batch);
    std::fputs(",\"guest_frame_start\":", output); U64(output, slot.guest_frame);
    std::fputs(",\"retired_guest_frame\":", output); U64(output, retired_frame);
    std::fputs(",\"cpu_record_begin_ns\":", output);
    if (slot.cpu_begin_valid) U64(output, slot.cpu_begin_ns);
    else std::fputs("null", output);
    std::fputs(",\"cpu_record_end_ns\":", output);
    if (slot.cpu_end_valid) U64(output, slot.cpu_end_ns);
    else std::fputs("null", output);
    if (!slot.cpu_begin_valid || (slot.root_end != kNone && !slot.cpu_end_valid))
      std::fputs(",\"cpu_clock_error\":\"clock_gettime_CLOCK_MONOTONIC_failed\"", output);
    std::fprintf(output, ",\"slot\":%u,\"query_base\":%u,\"valid_bits\":%u,\"period_ns\":%s,"
        "\"queries\":%u,\"dropped\":%u,\"incomplete\":%u,\"invalid_tokens\":%u,"
        "\"cross_batch\":%u,\"category_truncated\":%u,\"root_begin_query\":%u,\"root_end_query\":",
        index, index * kQueriesPerSlot, valid_bits, period_text, slot.queries,
        slot.dropped, incomplete, slot.invalid_tokens, slot.cross_batch,
        slot.category_truncated, slot.root_begin);
    if (slot.root_end == kNone) std::fputs("null", output);
    else std::fprintf(output, "%u", slot.root_end);
    std::fputs(",\"root_begin_tick\":", output); Tick(output, slot, slot.root_begin, available);
    std::fputs(",\"root_end_tick\":", output); Tick(output, slot, slot.root_end, available);
    std::fprintf(output, ",\"pipeline_statistics\":%s,\"pipeline_stats_query_base\":%u,\"pipeline_stats_queries\":%u,"
        "\"pipeline_stats_collected\":%u,\"pipeline_stats_incomplete\":%u,"
        "\"pipeline_stats_invalid\":%u,\"pipeline_stats_dropped\":%u",
        stats_pool ? "true" : "false", index * kScopesPerSlot, slot.stats_queries, slot.stats_collected,
        stats_incomplete, slot.stats_invalid, slot.stats_dropped);
    if (slot.stats_error) {
      std::fputs(",\"pipeline_stats_error\":", output); JsonString(output, slot.stats_error);
    }
    if (error || slot.invalid_tokens || incomplete || slot.dropped || slot.category_truncated) {
      std::fputs(",\"error\":", output);
      JsonString(output, error ? error : "incomplete_dropped_or_invalid_scope_data");
    }
    std::fputs(",\"scopes\":[", output);
    for (uint32_t i = 0; i < slot.count; ++i) {
      const auto& scope = slot.scopes[i];
      if (i) std::fputc(',', output);
      std::fputs("{\"category\":", output); JsonString(output, scope.category);
      std::fputs(",\"frame\":", output); U64(output, scope.frame);
      std::fprintf(output, ",\"resource\":%u,\"width\":%u,\"height\":%u,\"samples\":%u,\"bytes\":",
          scope.resource, scope.width, scope.height, scope.samples);
      U64(output, scope.bytes);
      std::fputs(",\"draws\":", output); U64(output, scope.draws);
      std::fputs(",\"vertices\":", output); U64(output, scope.vertices);
      std::fprintf(output, ",\"begin_query\":%u,\"end_query\":", scope.begin_query);
      if (scope.end_query == kNone) std::fputs("null", output);
      else std::fprintf(output, "%u", scope.end_query);
      std::fputs(",\"begin_tick\":", output); Tick(output, slot, scope.begin_query, available);
      std::fputs(",\"end_tick\":", output); Tick(output, slot, scope.end_query, available);
      if (scope.stats_requested) {
        std::fputs(",\"pipeline_stats_query\":", output);
        if (scope.stats_query == kNone) std::fputs("null", output);
        else std::fprintf(output, "%u", scope.stats_query);
        const char* stats_error = scope.stats_error;
        if (!stats_error && scope.stats_query == kNone) stats_error = "missing_pipeline_statistics_query";
        if (!stats_error && !scope.stats_ended) stats_error = "pipeline_statistics_query_unended";
        if (!stats_error && (!slot.stats_available || scope.stats_query >= slot.stats_collected))
          stats_error = slot.stats_error ? slot.stats_error : "pipeline_statistics_results_not_collected";
        std::fputs(",\"pipeline_stats\":", output);
        if (stats_error) {
          std::fputs("null,\"pipeline_stats_error\":", output); JsonString(output, stats_error);
        } else {
          std::fputc('{', output);
          for (uint32_t counter = 0; counter < kPipelineCounters; ++counter) {
            if (counter) std::fputc(',', output);
            JsonString(output, kPipelineNames[counter]); std::fputc(':', output);
            U64(output, slot.pipeline_stats[scope.stats_query][counter]);
          }
          std::fputc('}', output);
        }
      }
      std::fputc('}', output);
    }
    std::fputs("]}\n", output);
    CheckOutput();
  }
};

static_assert(std::is_trivially_copyable_v<GpuWorkProfile::Token>);
static_assert(sizeof(GpuWorkProfile::Token) == 16);

GpuWorkProfile::GpuWorkProfile() = default;
GpuWorkProfile::~GpuWorkProfile() { Destroy(); }

bool GpuWorkProfile::Init(VkDevice device, uint32_t valid_bits, float timestamp_period,
                          const char* output_path, bool pipeline_statistics) {
  Destroy();
  if (!device || !valid_bits || valid_bits > 64 || !std::isfinite(timestamp_period) ||
      timestamp_period <= 0 || !output_path || !*output_path) return false;
  impl_ = std::make_unique<Impl>();
  auto& im = *impl_;
  im.device = device; im.valid_bits = valid_bits; im.period = timestamp_period;
  const auto converted = std::to_chars(im.period_text, im.period_text + sizeof(im.period_text) - 1,
      timestamp_period, std::chars_format::general, 9);
  if (converted.ec != std::errc{}) { impl_.reset(); return false; }
  *converted.ptr = '\0';
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  std::snprintf(im.session, sizeof(im.session), "%ld-%llu-%llu", long(getpid()),
      static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()),
      static_cast<unsigned long long>(next_session.fetch_add(1, std::memory_order_relaxed)));
  im.output = std::fopen(output_path, "ab");
  if (!im.output) { impl_.reset(); return false; }
  std::setvbuf(im.output, nullptr, _IOFBF, 256 * 1024);
  VkQueryPoolCreateInfo create{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  create.queryType = VK_QUERY_TYPE_TIMESTAMP;
  create.queryCount = kSlots * kQueriesPerSlot;
  const auto result = vkCreateQueryPool(device, &create, nullptr, &im.pool);
  if (result != VK_SUCCESS) {
    im.Error("Init", "vkCreateQueryPool_failed"); Destroy(); return false;
  }
  if (pipeline_statistics) {
    create.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
    create.queryCount = kSlots * kScopesPerSlot;
    create.pipelineStatistics = kPipelineStatistics;
    if (vkCreateQueryPool(device, &create, nullptr, &im.stats_pool) != VK_SUCCESS) {
      im.Error("Init", "pipeline_statistics_vkCreateQueryPool_failed"); Destroy(); return false;
    }
  }
  std::fputs("{\"type\":\"gpu_work_session\",\"session\":", im.output);
  JsonString(im.output, im.session);
  std::fprintf(im.output, ",\"valid_bits\":%u,\"period_ns\":%s,\"slots\":%u,"
      "\"scope_capacity\":%u,\"queries_per_slot\":%u,\"scope_timestamp_stages\":\"top-bottom\","
      "\"pipeline_statistics\":%s,\"pipeline_statistics_mask\":%u,"
      "\"pipeline_statistics_queries_per_slot\":%u,\"cpu_record_clock\":\"CLOCK_MONOTONIC\"}\n",
      valid_bits, im.period_text, kSlots, kScopesPerSlot, kQueriesPerSlot,
      im.stats_pool ? "true" : "false", im.stats_pool ? kPipelineStatistics : 0,
      im.stats_pool ? kScopesPerSlot : 0);
  im.CheckOutput();
  return true;
}

void GpuWorkProfile::Calibrate(PFN_vkGetCalibratedTimestampsEXT fn,
                               VkTimeDomainEXT host_domain) {
  if (!impl_) return;
  auto& im = *impl_;
  if (!fn) { im.Error("Calibrate", "vkGetCalibratedTimestampsEXT_function_missing"); return; }
  if (host_domain != VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT &&
      host_domain != VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT) {
    // Other domains may use ticks rather than nanoseconds. Never label those
    // values host_ns without a valid domain-specific conversion.
    im.Error("Calibrate", "host_time_domain_not_nanoseconds"); return;
  }
  std::array<VkCalibratedTimestampInfoEXT, 2> info{};
  for (auto& entry : info) entry.sType = VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT;
  info[0].timeDomain = VK_TIME_DOMAIN_DEVICE_EXT;
  info[1].timeDomain = host_domain;
  std::array<uint64_t, 2> ticks{};
  uint64_t max_deviation = 0;
  const auto result = fn(im.device, uint32_t(info.size()), info.data(), ticks.data(), &max_deviation);
  if (result != VK_SUCCESS) {
    char reason[96];
    std::snprintf(reason, sizeof(reason), "vkGetCalibratedTimestampsEXT_failed_%d", int(result));
    im.Error("Calibrate", reason); return;
  }
  std::fputs("{\"type\":\"gpu_work_clock_calibration\",\"session\":", im.output);
  JsonString(im.output, im.session);
  std::fputs(",\"device_tick\":", im.output); U64(im.output, ticks[0]);
  std::fputs(",\"host_ns\":", im.output); U64(im.output, ticks[1]);
  std::fprintf(im.output, ",\"host_domain\":%u,\"max_deviation_ns\":", uint32_t(host_domain));
  U64(im.output, max_deviation);
  std::fputs("}\n", im.output);
  im.CheckOutput();
}

void GpuWorkProfile::Destroy() {
  if (!impl_) return;
  auto& im = *impl_;
  for (uint32_t slot = 0; slot < kSlots; ++slot)
    if (im.slots[slot].pending && im.output)
      im.Write(slot, im.slots[slot].guest_frame, false, "destroy_before_collect");
  if (im.pool) vkDestroyQueryPool(im.device, im.pool, nullptr);
  if (im.stats_pool) vkDestroyQueryPool(im.device, im.stats_pool, nullptr);
  if (im.output) {
    if (std::fflush(im.output) != 0) {
      std::fputs("gpu-work-profile: diagnostic output flush failed\n", stderr);
    }
    std::fclose(im.output);
  }
  impl_.reset();
}

void GpuWorkProfile::BeginBatch(VkCommandBuffer cb, uint32_t index, uint64_t batch,
                               uint64_t guest_frame) {
  if (!impl_) return;
  auto& im = *impl_;
  if (!cb || index >= kSlots) { im.Error("BeginBatch", "invalid_slot_or_command_buffer"); return; }
  if (im.active_slot < kSlots && im.slots[im.active_slot].recording) {
    im.Error("BeginBatch", "previous_batch_still_recording");
    im.slots[im.active_slot].recording = false;
  }
  auto& slot = im.slots[index];
  if (slot.pending) im.Write(index, guest_frame, false, "slot_reused_without_collect");
  slot.cb = cb; slot.serial = ++im.serial; slot.batch = batch; slot.guest_frame = guest_frame;
  slot.count = slot.queries = slot.dropped = slot.invalid_tokens = slot.cross_batch = slot.category_truncated = 0;
  slot.root_begin = slot.root_end = kNone;
  slot.stats_queries = slot.stats_collected = slot.stats_invalid = slot.stats_dropped = 0;
  slot.active_stats_scope = kNone;
  slot.stats_available = false; slot.stats_error = nullptr;
  slot.cpu_begin_ns = slot.cpu_end_ns = 0;
  slot.cpu_begin_valid = MonotonicNs(slot.cpu_begin_ns);
  slot.cpu_end_valid = false;
  slot.pending = slot.recording = true;
  im.active_slot = index;
  vkCmdResetQueryPool(cb, im.pool, index * kQueriesPerSlot, kQueriesPerSlot);
  if (im.stats_pool) vkCmdResetQueryPool(cb, im.stats_pool, index * kScopesPerSlot, kScopesPerSlot);
  slot.root_begin = slot.queries++;
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, im.pool,
                       index * kQueriesPerSlot + slot.root_begin);
}

void GpuWorkProfile::EndBatch(VkCommandBuffer cb) {
  if (!impl_) return;
  auto& im = *impl_;
  if (im.active_slot >= kSlots) { im.Error("EndBatch", "no_recording_batch"); return; }
  auto& slot = im.slots[im.active_slot];
  if (!slot.recording || slot.cb != cb) { im.Error("EndBatch", "command_buffer_mismatch"); return; }
  if (slot.active_stats_scope != kNone) {
    ++slot.stats_invalid;
    im.Error("EndBatch", "pipeline_statistics_query_still_active");
  }
  slot.root_end = slot.queries++;
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, im.pool,
                       im.active_slot * kQueriesPerSlot + slot.root_end);
  slot.cpu_end_valid = MonotonicNs(slot.cpu_end_ns);
  slot.recording = false;
  im.active_slot = kNone;
}

void GpuWorkProfile::Collect(uint32_t index, uint64_t retired_guest_frame) {
  if (!impl_) return;
  auto& im = *impl_;
  if (index >= kSlots) { im.Error("Collect", "invalid_slot"); return; }
  auto& slot = im.slots[index];
  if (!slot.pending) return;
  const char* error = nullptr;
  bool available = false;
  if (slot.recording || slot.root_end == kNone) error = "collect_before_end_batch";
  else {
    const auto result = vkGetQueryPoolResults(im.device, im.pool, index * kQueriesPerSlot,
        slot.queries, slot.queries * sizeof(uint64_t), slot.ticks.data(), sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    available = result == VK_SUCCESS;
    if (!available) error = result == VK_NOT_READY ? "query_results_not_ready" : "vkGetQueryPoolResults_failed";
  }
  if (im.stats_pool) {
    slot.stats_collected = slot.stats_queries;
    for (uint32_t i = 0; i < slot.count; ++i) {
      const auto& scope = slot.scopes[i];
      if (scope.stats_query != kNone && !scope.stats_ended && scope.stats_query < slot.stats_collected)
        slot.stats_collected = scope.stats_query;
    }
    if (slot.recording || slot.root_end == kNone) {
      slot.stats_collected = 0; slot.stats_error = "collect_before_end_batch";
    } else if (slot.stats_collected) {
      const auto result = vkGetQueryPoolResults(im.device, im.stats_pool, index * kScopesPerSlot,
          slot.stats_collected, size_t(slot.stats_collected) * sizeof(slot.pipeline_stats[0]),
          slot.pipeline_stats.data(), sizeof(slot.pipeline_stats[0]), VK_QUERY_RESULT_64_BIT);
      slot.stats_available = result == VK_SUCCESS;
      if (!slot.stats_available) slot.stats_error = result == VK_NOT_READY
          ? "pipeline_statistics_results_not_ready" : "pipeline_statistics_vkGetQueryPoolResults_failed";
    }
  }
  im.Write(index, retired_guest_frame, available, error);
  slot.pending = slot.recording = false;
  if (im.active_slot == index) im.active_slot = kNone;
}

GpuWorkProfile::Token GpuWorkProfile::Begin(VkCommandBuffer cb, const char* category,
    uint64_t guest_frame, uint32_t resource, uint32_t width, uint32_t height,
    uint32_t samples, uint64_t bytes) {
  if (!impl_) return {};
  auto& im = *impl_;
  if (im.active_slot >= kSlots || im.slots[im.active_slot].cb != cb ||
      !im.slots[im.active_slot].recording) {
    im.Error("Begin", "no_matching_recording_batch"); return {};
  }
  auto& slot = im.slots[im.active_slot];
  if (slot.count >= kScopesPerSlot || slot.queries >= kQueriesPerSlot - 1) {
    ++slot.dropped; return {};
  }
  const uint32_t index = slot.count++;
  auto& scope = slot.scopes[index];
  scope = {};
  if (!category) category = "unspecified";
  const size_t length = std::strlen(category);
  if (length >= sizeof(scope.category)) ++slot.category_truncated;
  std::snprintf(scope.category, sizeof(scope.category), "%s", category);
  scope.frame = guest_frame; scope.resource = resource; scope.width = width;
  scope.height = height; scope.samples = samples; scope.bytes = bytes;
  scope.begin_query = slot.queries++;
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, im.pool,
                       im.active_slot * kQueriesPerSlot + scope.begin_query);
  if (im.stats_pool && std::strcmp(scope.category, "draw_pass") == 0) {
    scope.stats_requested = true;
    if (slot.active_stats_scope != kNone) {
      ++slot.stats_invalid; scope.stats_error = "pipeline_statistics_query_already_active";
      im.Error("Begin", scope.stats_error);
    } else if (slot.stats_queries >= kScopesPerSlot) {
      ++slot.stats_dropped; scope.stats_error = "pipeline_statistics_capacity_exceeded";
    } else {
      scope.stats_query = slot.stats_queries++;
      slot.active_stats_scope = index;
      vkCmdBeginQuery(cb, im.stats_pool, im.active_slot * kScopesPerSlot + scope.stats_query, 0);
    }
  }
  return {slot.serial, im.active_slot, index};
}

void GpuWorkProfile::End(VkCommandBuffer cb, Token token) {
  if (!impl_) return;
  auto& im = *impl_;
  Impl::Scope* scope = nullptr;
  if (!im.Validate(token, "End", cb, scope)) return;
  auto& slot = im.slots[token.slot];
  if (scope->stats_query != kNone && !scope->stats_ended) {
    if (slot.active_stats_scope != token.index) {
      ++slot.stats_invalid; scope->stats_error = "pipeline_statistics_active_scope_mismatch";
      im.Error("End", scope->stats_error, token);
    } else {
      vkCmdEndQuery(cb, im.stats_pool, token.slot * kScopesPerSlot + scope->stats_query);
      scope->stats_ended = true;
      slot.active_stats_scope = kNone;
    }
  }
  if (slot.queries >= kQueriesPerSlot - 1) {
    ++slot.dropped; return;
  }
  scope->end_query = slot.queries++;
  vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, im.pool,
                       token.slot * kQueriesPerSlot + scope->end_query);
}

void GpuWorkProfile::AddDraw(Token token, uint32_t count) {
  if (!impl_) return;
  auto& im = *impl_;
  Impl::Scope* scope = nullptr;
  const auto cb = im.active_slot < kSlots ? im.slots[im.active_slot].cb : VK_NULL_HANDLE;
  if (!im.Validate(token, "AddDraw", cb, scope)) return;
  ++scope->draws;
  scope->vertices += count;
}

}  // namespace xn::gfx
