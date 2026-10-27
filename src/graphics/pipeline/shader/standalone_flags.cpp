/**
 ******************************************************************************
 * ReXGlue - rex::shader-translator standalone cvar storage
 ******************************************************************************
 *
 * The Xenos microcode translator reads three cvars that the GPU plugins define
 * in flags.cpp / spirv_translator_fetch.cpp. A port that links
 * rex::shader-translator into its executable also loads a GPU plugin at
 * runtime; both register cvars by name into the one registry in rexruntime,
 * and the second registration of a name is dropped. Defining the same names
 * here would therefore silently detach the plugin's `--dump_shaders` and
 * friends from the command line.
 *
 * So the standalone copy registers its knobs under a `shader_translator_`
 * prefix and forwards the translator's storage accessors to them.
 */

#include <string>

#include <rex/cvar.h>

REXCVAR_DEFINE_STRING(shader_translator_dump_shaders, "", "GPU/ShaderTranslator",
                      "rex::shader-translator (native device in the port): directory to dump "
                      "analyzed microcode into, same layout as the plugin's --dump_shaders.");
REXCVAR_DEFINE_BOOL(shader_translator_vertex_nan_kill_debug, false, "GPU/ShaderTranslator",
                    "rex::shader-translator: mirror of gpu_vertex_nan_kill_debug for the "
                    "standalone translator copy.");
REXCVAR_DEFINE_BOOL(shader_translator_texture_exp_bias_ignore, false, "GPU/ShaderTranslator",
                    "rex::shader-translator: mirror of texture_exp_bias_ignore for the "
                    "standalone translator copy.");

// Storage accessors the translator sources call through REXCVAR_GET(). The
// names must match REXCVAR_DECLARE(...) in <rex/graphics/flags.h> and
// spirv_translator_fetch.cpp.
std::string& FLAGS_dump_shaders_storage_() {
  return FLAGS_shader_translator_dump_shaders_storage_();
}
bool& FLAGS_gpu_vertex_nan_kill_debug_storage_() {
  return FLAGS_shader_translator_vertex_nan_kill_debug_storage_();
}
bool& FLAGS_texture_exp_bias_ignore_storage_() {
  return FLAGS_shader_translator_texture_exp_bias_ignore_storage_();
}
