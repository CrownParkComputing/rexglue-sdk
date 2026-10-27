#include "frame_recorder.h"

#include <xxhash.h>

#include <algorithm>
#include <memory>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include <rex/logging.h>

#include "command_processor.h"

namespace xn {
namespace {

struct DrawRecord {
  uint32_t opcode, prim, count, source;
  uint64_t vs, ps, state_hash;
  uint32_t edram_mode;  // RB_MODECONTROL.edram_mode: 4 = colour+depth draw, 6 = copy (resolve helper draw)
};

struct FrameDigest {
  uint64_t frame = 0;
  std::vector<DrawRecord> draws;
  uint32_t reg_writes = 0, const_loads = 0, shader_loads = 0, events = 0;
  uint64_t regs_hash = 0, state_hash = 0, const_hash = 0;
  uint32_t swap_address = 0, width = 0, height = 0;
};

class FrameRecorder;
FrameRecorder* g_recorder = nullptr;

class FrameRecorder : public IRenderer {
 public:
  FrameRecorder(IRenderer* inner, const FrameRecorderConfig& config) : inner_(inner), config_(config) {
    if (!config_.digest_prefix.empty()) {
      frames_csv_ = Open(config_.digest_prefix + "_frames.csv",
                         "frame,draws,vertices,prims,vs,ps,draw_seq_hash,state_hash,const_hash,regs_hash,"
                         "reg_writes,const_loads,shader_loads,events,swap_addr,width,height\n");
      draws_csv_ = Open(config_.digest_prefix + "_draws.csv",
                        "frame,draw,opcode,prim,count,source,vs,ps,state_hash,edram_mode\n");
      events_csv_ = Open(config_.digest_prefix + "_events.csv", "name,seconds,frame\n");
    }
    g_recorder = this;
  }
  ~FrameRecorder() override {
    g_recorder = nullptr;
    for (FILE* f : {frames_csv_, draws_csv_, events_csv_}) if (f) std::fclose(f);
  }

  bool Initialize(SDL_Window* w, uint8_t* base, uint8_t* phys) override { return inner_->Initialize(w, base, phys); }
  void Shutdown() override { inner_->Shutdown(); }
  void SetFpsOverlay(std::string_view text) override { inner_->SetFpsOverlay(text); }
  void SetFrameRateTarget(uint32_t fps) override { inner_->SetFrameRateTarget(fps); }
  void FlushCaches() override { inner_->FlushCaches(); }

  void OnRegisterWrite(uint32_t index, uint32_t value) override {
    cur_.reg_writes++;
    inner_->OnRegisterWrite(index, value);
  }
  void OnShaderLoad(uint32_t type, uint32_t address, uint32_t size, const uint32_t* dwords) override {
    cur_.shader_loads++;
    uint64_t h = ShaderUcodeHash(dwords, size);
    if (type == 0) vs_ = h; else ps_ = h;
    if (seen_shaders_.emplace(h, type).second)
      REXGPU_INFO("recorder: {} shader {:016X} ({} dwords at {:#010x})", type == 0 ? "vertex" : "pixel", h, size, address);
    inner_->OnShaderLoad(type, address, size, dwords);
  }
  bool RequiresRegistersForShaderLoad() const override {
    return inner_->RequiresRegistersForShaderLoad();
  }
  void OnConstants(uint32_t index, const uint32_t* dwords, uint32_t count) override {
    cur_.const_loads++;
    inner_->OnConstants(index, dwords, count);
  }
  void OnRegisterBatch(const GpuRegisterFile& registers,
                       std::span<const RegisterRange> ranges) override {
    cur_.const_loads++;
    inner_->OnRegisterBatch(registers, ranges);
  }
  void OnDraw(const DrawPacket& draw, const GpuRegisterFile& regs) override {
    DrawRecord r{draw.opcode, draw.prim_type, draw.index_count, draw.source, vs_, ps_,
                 XXH3_64bits(&regs.values[0x2000], 0x1000 * sizeof(uint32_t)),
                 regs[0x2208] & 7 /*RB_MODECONTROL*/};
    cur_.draws.push_back(r);
    last_regs_ = &regs;
    cur_.regs_hash = XXH3_64bits(regs.values, sizeof(regs.values));
    cur_.const_hash = XXH3_64bits(&regs.values[0x4000], 0x800 * sizeof(uint32_t));
    if (draws_csv_)
      std::fprintf(draws_csv_, "%llu,%zu,%#x,%u,%u,%u,%016llX,%016llX,%016llX,%u\n",
                   (unsigned long long)cur_.frame, cur_.draws.size() - 1, r.opcode, r.prim, r.count,
                   r.source, (unsigned long long)r.vs, (unsigned long long)r.ps,
                   (unsigned long long)r.state_hash, r.edram_mode);
    inner_->OnDraw(draw, regs);
  }
  void OnEvent(uint32_t initiator, const GpuRegisterFile& regs) override {
    cur_.events++;
    inner_->OnEvent(initiator, regs);
  }
  void OnSwap(uint32_t address, uint32_t width, uint32_t height) override {
    cur_.swap_address = address; cur_.width = width; cur_.height = height;
    FinishFrame();
    inner_->OnSwap(address, width, height);
  }
  void Present() override {
    inner_->Present();
    std::unique_ptr<FrameDigest> pending;
    {
      std::lock_guard lock(mutex_);
      pending = std::move(pending_dump_);
    }
    if (!pending) return;
    char path[1024];
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (inner_->CaptureFrame(rgba, w, h) && w && h && rgba.size() >= size_t(w) * h * 4) {
      std::snprintf(path, sizeof path, "%s_%04llu.ppm", config_.dump_prefix.c_str(), (unsigned long long)pending->frame);
      if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        std::vector<uint8_t> row(size_t(w) * 3);
        for (uint32_t y = 0; y < h; y++) {
          const uint8_t* src = rgba.data() + size_t(y) * w * 4;
          for (uint32_t x = 0; x < w; x++) { row[x * 3] = src[x * 4]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4 + 2]; }
          std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        REXGPU_INFO("recorder: dumped frame {} to {} ({}x{})", pending->frame, path, w, h);
      }
    } else {
      std::snprintf(path, sizeof path, "%s_%04llu.draws.txt", config_.dump_prefix.c_str(), (unsigned long long)pending->frame);
      if (FILE* f = std::fopen(path, "w")) {
        std::fprintf(f, "# frame %llu: %zu draws, swap %#010x %ux%u (renderer has no readback)\n",
                     (unsigned long long)pending->frame, pending->draws.size(), pending->swap_address,
                     pending->width, pending->height);
        for (size_t i = 0; i < pending->draws.size(); i++) {
          const DrawRecord& r = pending->draws[i];
          std::fprintf(f, "%zu op=%#x prim=%u count=%u src=%u vs=%016llX ps=%016llX state=%016llX mode=%u\n", i,
                       r.opcode, r.prim, r.count, r.source, (unsigned long long)r.vs, (unsigned long long)r.ps,
                       (unsigned long long)r.state_hash, r.edram_mode);
        }
        std::fclose(f);
        REXGPU_INFO("recorder: no readback; wrote draw log for frame {} to {}", pending->frame, path);
      }
    }
  }
  bool OnD3DCall(const D3DCall& call) override { return inner_->OnD3DCall(call); }
  bool SynchronizeTextureToGuest(uint32_t address, uint32_t width,
                                 uint32_t height, uint32_t format) override {
    return inner_->SynchronizeTextureToGuest(address, width, height, format);
  }
  bool CaptureFrame(std::vector<uint8_t>& rgba8, uint32_t& w, uint32_t& h) override {
    return inner_->CaptureFrame(rgba8, w, h);
  }

  void NoteEvent(std::string_view name, double seconds, uint64_t frame) {
    if (!events_csv_) return;
    std::fprintf(events_csv_, "%.*s,%.3f,%llu\n", int(name.size()), name.data(), seconds, (unsigned long long)frame);
    std::fflush(events_csv_);
  }

 private:
  static FILE* Open(const std::string& path, const char* header) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) { REXGPU_WARN("recorder: cannot open {}", path); return nullptr; }
    std::fputs(header, f);
    return f;
  }

  void FinishFrame() {
    FrameDigest& d = cur_;
    if (frames_csv_) {
      uint64_t vertices = 0;
      std::map<uint32_t, uint32_t> prims;
      std::map<uint64_t, int> vs, ps;
      std::vector<uint64_t> seq;
      uint64_t state = 0;
      for (const DrawRecord& r : d.draws) {
        vertices += r.count;
        prims[r.prim]++;
        vs[r.vs]++; ps[r.ps]++;
        seq.insert(seq.end(), {uint64_t(r.prim) | (uint64_t(r.count) << 8), r.vs, r.ps});
        state = XXH3_64bits_withSeed(&r.state_hash, sizeof r.state_hash, state);
      }
      auto join = [](auto& m, auto fmt) { std::string s; for (auto& [k, v] : m) { if (!s.empty()) s += ';'; s += fmt(k, v); } return s; };
      std::string prim_s = join(prims, [](uint32_t k, uint32_t v) { char b[32]; std::snprintf(b, sizeof b, "%u:%u", k, v); return std::string(b); });
      auto hex = [](uint64_t k, int) { char b[24]; std::snprintf(b, sizeof b, "%016llX", (unsigned long long)k); return std::string(b); };
      std::fprintf(frames_csv_, "%llu,%zu,%llu,%s,%s,%s,%016llX,%016llX,%016llX,%016llX,%u,%u,%u,%u,%#x,%u,%u\n",
                   (unsigned long long)d.frame, d.draws.size(), (unsigned long long)vertices, prim_s.c_str(),
                   join(vs, hex).c_str(), join(ps, hex).c_str(),
                   (unsigned long long)(seq.empty() ? 0 : XXH3_64bits(seq.data(), seq.size() * 8)),
                   (unsigned long long)state, (unsigned long long)d.const_hash, (unsigned long long)d.regs_hash,
                   d.reg_writes, d.const_loads, d.shader_loads, d.events, d.swap_address, d.width, d.height);
      std::fflush(frames_csv_);
      if (draws_csv_) std::fflush(draws_csv_);
    }
    if (!config_.dump_prefix.empty() && d.frame >= config_.dump_start &&
        (d.frame - config_.dump_start) % std::max(1u, config_.dump_every) == 0 &&
        (config_.dump_max == 0 || dumped_ < config_.dump_max)) {
      std::lock_guard lock(mutex_);
      if (!pending_dump_) { pending_dump_ = std::make_unique<FrameDigest>(d); dumped_++; }
    }
    uint64_t next = d.frame + 1;
    cur_ = FrameDigest{};
    cur_.frame = next;
  }

  IRenderer* inner_;
  FrameRecorderConfig config_;
  FILE* frames_csv_ = nullptr;
  FILE* draws_csv_ = nullptr;
  FILE* events_csv_ = nullptr;
  FrameDigest cur_;
  uint64_t vs_ = 0, ps_ = 0;
  const GpuRegisterFile* last_regs_ = nullptr;
  std::map<uint64_t, uint32_t> seen_shaders_;
  std::mutex mutex_;
  std::unique_ptr<FrameDigest> pending_dump_;
  uint64_t dumped_ = 0;  // queued for dumping (CP thread, under mutex_)
};

}  // namespace

uint64_t ShaderUcodeHash(const uint32_t* dwords, uint32_t count) {
  return dwords && count ? XXH3_64bits(dwords, size_t(count) * 4) : 0;
}

IRenderer* CreateFrameRecorder(IRenderer* inner, const FrameRecorderConfig& config) {
  return new FrameRecorder(inner, config);
}

void FrameRecorderNoteEvent(std::string_view name, double seconds, uint64_t frame) {
  if (g_recorder) g_recorder->NoteEvent(name, seconds, frame);
}

}  // namespace xn
