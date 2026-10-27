/**
 ******************************************************************************
 * ReXGlue - rex::shader-translator host test
 ******************************************************************************
 *
 * Loads every shader_<hash>.ucode.bin.{vert,frag} written by --dump_shaders
 * (Shader::DumpUcode: raw host-endian ucode dwords; the sibling
 * shader_<hash>.ucode.{vert,frag} is the disassembly text) and translates it to
 * SPIR-V with the default modification, exactly as a native device layer in a
 * port would, using only rex::shader-translator.
 *
 * Checks per shader:
 *   - the file name hash matches XXH3 of the guest (big-endian) ucode bytes,
 *     which is how the command processor keys shaders;
 *   - AnalyzeUcode() reproduces the dumped disassembly byte for byte;
 *   - TranslateAnalyzedShader() succeeds, the translation is valid and the
 *     binary starts with the SPIR-V magic and has a whole number of words.
 */

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <xxhash.h>

#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/pipeline/shader/spirv_translator_features.h>
#include <rex/string/buffer.h>

using namespace rex::graphics;

namespace {

struct Options {
  std::filesystem::path dir = "/home/jon/pgr4dump/shaders";
  bool features_all = true;
  bool features_min = false;
  bool verbose = false;
  std::vector<std::string> show;
  std::filesystem::path dump;
};

struct Counters {
  int files = 0;
  int hash_mismatch = 0;
  int disasm_mismatch = 0;
  int translate_fail = 0;
  int translate_ok = 0;
};

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  auto size = f.tellg();
  f.seekg(0);
  out.resize(size_t(size));
  return bool(f.read(reinterpret_cast<char*>(out.data()), size));
}

bool ParseHash(const std::string& name, uint64_t& hash) {
  // shader_<16 hex>.ucode.bin.<vert|frag>
  if (name.rfind("shader_", 0) != 0 || name.size() < 7 + 16) return false;
  try {
    hash = std::stoull(name.substr(7, 16), nullptr, 16);
  } catch (...) {
    return false;
  }
  return true;
}

// One translation of one shader with one feature set. Returns true on success.
bool TranslateOne(const std::filesystem::path& path, xenos::ShaderType type, uint64_t hash,
                  const std::vector<uint32_t>& dwords, const SpirvShaderTranslator::Features& features,
                  const char* features_name, const Options& opt, Counters& c, bool show,
                  std::string& status) {
  // The dump is host-endian (Shader keeps ucode in host order), so no swap.
  SpirvShader shader(type, hash, dwords.data(), dwords.size(), std::endian::native);

  rex::string::StringBuffer disasm;
  shader.AnalyzeUcode(disasm);

  // Disassembly check against the sibling text file.
  {
    std::string disasm_name = path.filename().string();
    auto pos = disasm_name.find(".ucode.bin.");
    if (pos != std::string::npos) {
      disasm_name.replace(pos, strlen(".ucode.bin."), ".ucode.");
      std::vector<uint8_t> expected;
      if (ReadFile(path.parent_path() / disasm_name, expected)) {
        const std::string& got = shader.ucode_disassembly();
        if (got.size() != expected.size() ||
            std::memcmp(got.data(), expected.data(), got.size()) != 0) {
          ++c.disasm_mismatch;
          status += " disasm-mismatch";
        }
      }
    }
  }

  // Same construction a native device uses: no EDRAM interlock, native 2x
  // MSAA with attachments, resolution scale 1.
  SpirvShaderTranslator translator(features, /*native_2x_msaa_with_attachments=*/true,
                                   /*native_2x_msaa_no_attachments=*/false,
                                   /*edram_fragment_shader_interlock=*/false);

  uint64_t modification_value;
  if (type == xenos::ShaderType::kVertex) {
    SpirvShaderTranslator::Modification mod(translator.GetDefaultVertexShaderModification(
        shader.GetDynamicAddressableRegisterCount(0)));
    mod.vertex.interpolator_mask = (uint32_t(1) << xenos::kMaxInterpolators) - 1;
    modification_value = mod.value;
  } else {
    SpirvShaderTranslator::Modification mod(translator.GetDefaultPixelShaderModification(
        shader.GetDynamicAddressableRegisterCount(0)));
    mod.pixel.interpolator_mask = (uint32_t(1) << xenos::kMaxInterpolators) - 1;
    mod.pixel.depth_stencil_mode =
        SpirvShaderTranslator::Modification::DepthStencilMode::kNoModifiers;
    modification_value = mod.value;
  }

  Shader::Translation* translation = shader.GetOrCreateTranslation(modification_value);
  bool ok = translator.TranslateAnalyzedShader(*translation);
  const std::vector<uint8_t>& spirv = translation->translated_binary();
  if (ok && !translation->is_valid()) {
    ok = false;
    status += " invalid";
  }
  if (ok && (spirv.size() < 20 || (spirv.size() % 4) != 0)) {
    ok = false;
    status += " bad-size";
  }
  if (ok) {
    uint32_t magic;
    std::memcpy(&magic, spirv.data(), 4);
    if (magic != 0x07230203u) {
      ok = false;
      status += " bad-magic";
    }
  }
  if (!ok) {
    ++c.translate_fail;
    status += " TRANSLATE-FAIL";
  } else {
    ++c.translate_ok;
    if (!opt.dump.empty()) {
      char name[64];
      snprintf(name, sizeof(name), "shader_%016llX.%s.%s.spv", (unsigned long long)hash,
               type == xenos::ShaderType::kVertex ? "vert" : "frag", features_name);
      std::ofstream out(opt.dump / name, std::ios::binary);
      out.write(reinterpret_cast<const char*>(spirv.data()), spirv.size());
    }
  }
  if (opt.verbose || show || !ok) {
    printf("  [%s] %s %s bytes=%zu%s\n", features_name, ok ? "ok  " : "FAIL",
           path.filename().string().c_str(), spirv.size(), status.c_str());
  }
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-v" || a == "--verbose") {
      opt.verbose = true;
    } else if (a.rfind("--features=", 0) == 0) {
      std::string v = a.substr(11);
      opt.features_all = (v == "all" || v == "both");
      opt.features_min = (v == "min" || v == "both");
    } else if (a.rfind("--show=", 0) == 0) {
      std::string v = a.substr(7);
      std::transform(v.begin(), v.end(), v.begin(), ::toupper);
      opt.show.push_back(v);
    } else if (a.rfind("--dump=", 0) == 0) {
      opt.dump = a.substr(7);
      std::filesystem::create_directories(opt.dump);
    } else if (!a.empty() && a[0] != '-') {
      opt.dir = a;
    } else {
      fprintf(stderr, "unknown option %s\n", a.c_str());
      return 2;
    }
  }
  if (!std::filesystem::is_directory(opt.dir)) {
    fprintf(stderr, "not a directory: %s\n", opt.dir.string().c_str());
    return 2;
  }

  std::vector<std::filesystem::path> files;
  for (auto& entry : std::filesystem::directory_iterator(opt.dir)) {
    if (!entry.is_regular_file()) continue;
    std::string name = entry.path().filename().string();
    if (name.find(".ucode.bin.vert") != std::string::npos ||
        name.find(".ucode.bin.frag") != std::string::npos) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());

  struct Pass {
    const char* name;
    SpirvShaderTranslator::Features features;
  };
  std::vector<Pass> passes;
  if (opt.features_all) {
    // Everything on, SPIR-V 1.5 - what a Vulkan 1.2+ desktop device gives.
    passes.push_back({"all", SpirvShaderTranslator::Features(true)});
  }
  if (opt.features_min) {
    // A bare Vulkan 1.0 device as MakeSpirvTranslatorFeatures() would describe
    // it with nothing enabled: SPIR-V 1.0, no clip/cull distance, 128 MB SSBO.
    passes.push_back({"min", MakeSpirvTranslatorFeatures(SpirvTranslatorDeviceInfo{})});
  }

  Counters c;
  int vert = 0, frag = 0;
  for (const auto& path : files) {
    ++c.files;
    std::string name = path.filename().string();
    xenos::ShaderType type = name.find(".vert") != std::string::npos ? xenos::ShaderType::kVertex
                                                                      : xenos::ShaderType::kPixel;
    (type == xenos::ShaderType::kVertex ? vert : frag)++;

    uint64_t hash = 0;
    if (!ParseHash(name, hash)) {
      printf("  skip (no hash in name): %s\n", name.c_str());
      continue;
    }
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes) || bytes.empty() || (bytes.size() % 4) != 0) {
      printf("  FAIL read: %s\n", name.c_str());
      ++c.translate_fail;
      continue;
    }
    std::vector<uint32_t> dwords(bytes.size() / 4);
    std::memcpy(dwords.data(), bytes.data(), bytes.size());

    // The command processor hashes the guest bytes (big-endian) with XXH3.
    std::vector<uint32_t> guest(dwords.size());
    for (size_t i = 0; i < dwords.size(); ++i) {
      guest[i] = std::byteswap(dwords[i]);
    }
    std::string status;
    if (XXH3_64bits(guest.data(), guest.size() * 4) != hash) {
      ++c.hash_mismatch;
      status += " hash-mismatch";
    }

    char hash_text[17];
    snprintf(hash_text, sizeof(hash_text), "%016llX", (unsigned long long)hash);
    bool show = std::find(opt.show.begin(), opt.show.end(), hash_text) != opt.show.end();

    for (const auto& pass : passes) {
      std::string pass_status = status;
      TranslateOne(path, type, hash, dwords, pass.features, pass.name, opt, c, show, pass_status);
    }
  }

  printf("shader_translator_test: %d files (%d vert, %d frag) x %zu feature set(s): "
         "%d translated, %d failed, %d hash mismatches, %d disassembly mismatches\n",
         c.files, vert, frag, passes.size(), c.translate_ok, c.translate_fail, c.hash_mismatch,
         c.disasm_mismatch);
  return (c.translate_fail == 0 && c.files > 0) ? 0 : 1;
}
