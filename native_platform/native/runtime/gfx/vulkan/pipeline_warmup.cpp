#include "vulkan_renderer_impl.h"

#include <chrono>
#include <cstdlib>

namespace xn::gfx {

void VulkanRenderer::Impl::RememberPipeline(const PipelineKey& key) {
  if (!warmup_enabled || warming_pipelines || pipeline_cache_file.empty() ||
      pipeline_warmup.size() >= kMaxWarmupRecords) return;
  const auto vertex = module_sources.find(key.vs);
  const auto pixel = module_sources.find(key.ps);
  // Debug shader modules have no guest microcode to replay.
  if (vertex == module_sources.end() || (key.ps && pixel == module_sources.end())) return;
  WarmupRecord record;
  record.state = key;
  record.state.vs = record.state.ps = VK_NULL_HANDLE;
  auto save = [](const auto& source, uint64_t& hash, uint64_t& modification,
                 std::vector<uint32_t>& code) {
    hash = source.first->shader->ucode_data_hash();
    modification = source.second;
    const auto words = source.first->shader->ucode_data();
    code.assign(words.begin(), words.end());
  };
  save(vertex->second, record.vertex_hash, record.vertex_modification, record.vertex_code);
  if (key.ps) save(pixel->second, record.pixel_hash, record.pixel_modification, record.pixel_code);
  pipeline_warmup.push_back(std::move(record));
}

bool VulkanRenderer::Impl::WarmPipelines() {
  const char* enabled = std::getenv("XN_VK_PREWARM");
  warmup_enabled = !enabled || std::strcmp(enabled, "0");
  if (!warmup_enabled || pipeline_cache_file.empty()) return true;
  auto records = ReadPipelineWarmup<PipelineKey>(pipeline_cache_file.string() + ".warmup");
  // A local playtest may contribute states while another game instance is
  // running. Keep that seed separate so the older instance's shutdown cannot
  // overwrite it. Successful unique states join the normal saved catalogue.
  auto seed = ReadPipelineWarmup<PipelineKey>(pipeline_cache_file.string() + ".warmup.seed");
  for (auto& record : seed) records.push_back(std::move(record));
  if (records.empty()) return true;
  const auto start = std::chrono::steady_clock::now();
  const std::string caption = window ? SDL_GetWindowTitle(window) : "";
  REXLOG_INFO("xn-vk: preparing {} previously used graphics pipelines", records.size());
  warming_pipelines = true;
  auto restore = [&] {
    warming_pipelines = false;
    if (window) SDL_SetWindowTitle(window, caption.c_str());
  };
  for (size_t index = 0; index < records.size(); ++index) {
    if (pipeline_warmup.size() >= kMaxWarmupRecords) break;
    if (window) {
      if (!(index % 8)) SDL_SetWindowTitle(window,
          fmt::format("Preparing graphics ({}/{})", index + 1, records.size()).c_str());
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT ||
            (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
             event.window.windowID == SDL_GetWindowID(window))) {
          restore();
          return false;
        }
      }
    }
    auto& record = records[index];
    // Serialized states contain no process-local handles.
    if (record.state.vs || record.state.ps) continue;
    auto prepare = [&](const std::vector<uint32_t>& code, uint64_t hash, uint64_t modification,
                       xenos::ShaderType type, VkShaderModule& module) -> SpirvShader* {
      ShaderRef ref{code.data(), uint32_t(code.size()), false, hash};
      auto* entry = GetShader(ref, type);
      auto* translation = entry->shader->GetOrCreateTranslation(modification);
      if ((!translation->is_translated() && !translator->TranslateAnalyzedShader(*translation)) ||
          !translation->is_valid()) return nullptr;
      module = GetShaderModule(*entry, modification, translation);
      return module ? entry->shader.get() : nullptr;
    };
    PipelineKey key = record.state;
    auto* vertex = prepare(record.vertex_code, record.vertex_hash, record.vertex_modification,
                           xenos::ShaderType::kVertex, key.vs);
    if (!vertex) continue;
    SpirvShader* pixel = nullptr;
    if (!record.pixel_code.empty()) {
      pixel = prepare(record.pixel_code, record.pixel_hash, record.pixel_modification,
                      xenos::ShaderType::kPixel, key.ps);
      if (!pixel) continue;
    }
    if (pipelines.contains(key)) continue;
    auto layout = GetPipelineLayout(uint32_t(vertex->GetTextureBindingsAfterTranslation().size()),
        uint32_t(vertex->GetSamplerBindingsAfterTranslation().size()),
        pixel ? uint32_t(pixel->GetTextureBindingsAfterTranslation().size()) : 0,
        pixel ? uint32_t(pixel->GetSamplerBindingsAfterTranslation().size()) : 0);
    if (!GetPipeline(key, layout)) continue;
    ++stats.pipelines_prewarmed;
    pipeline_warmup.push_back(std::move(record));
  }
  restore();
  const double seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - start).count();
  REXLOG_INFO("xn-vk: prepared {} graphics pipelines in {:.2f}s", stats.pipelines_prewarmed, seconds);
  return true;
}
}  // namespace xn::gfx
