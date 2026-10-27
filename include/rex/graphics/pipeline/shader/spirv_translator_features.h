#pragma once
/**
 ******************************************************************************
 * ReXGlue - SpirvShaderTranslator::Features without a ui::vulkan::VulkanDevice
 ******************************************************************************
 *
 * A native device inside a port owns its own VkPhysicalDevice/VkDevice and has
 * no rex::ui::vulkan::VulkanDevice (that class belongs to the presenter). This
 * header fills SpirvShaderTranslator::Features from plain Vulkan queries so the
 * translator can be constructed without the UI layer.
 *
 * Header-only; the only link dependency is what the port already has (the
 * Vulkan function pointers it passes in).
 */

#include <cstdint>
#include <cstring>

#include <rex/graphics/pipeline/shader/spirv_translator.h>

namespace rex::graphics {

// What the translator needs to know about the device. Fill it from your own
// device creation code (the values must describe features you actually ENABLED
// on the VkDevice, not merely ones the physical device supports), or let
// QuerySpirvTranslatorDeviceInfo() fill it from the physical device and then
// clear whatever you do not enable.
struct SpirvTranslatorDeviceInfo {
  // VkPhysicalDeviceProperties::apiVersion clamped to the instance version.
  uint32_t api_version = VK_API_VERSION_1_0;
  // VkPhysicalDeviceLimits::maxStorageBufferRange.
  uint32_t max_storage_buffer_range = uint32_t(1) << 27;

  // Core 1.0 features (VkPhysicalDeviceFeatures).
  bool full_draw_index_uint32 = false;
  bool vertex_pipeline_stores_and_atomics = false;
  bool fragment_stores_and_atomics = false;
  bool shader_clip_distance = false;
  bool shader_cull_distance = false;
  bool sample_rate_shading = false;

  // VK_KHR_spirv_1_4 enabled (implied by api_version >= 1.2).
  bool spirv_1_4 = false;
  // VK_KHR_shader_float_controls / Vulkan 1.2 float controls properties.
  bool shader_signed_zero_inf_nan_preserve_float32 = false;
  bool shader_denorm_flush_to_zero_float32 = false;
  bool shader_rounding_mode_rte_float32 = false;
  // VK_EXT_fragment_shader_interlock feature, enabled.
  bool fragment_shader_sample_interlock = false;
  // Vulkan 1.3 / VK_EXT_shader_demote_to_helper_invocation feature, enabled.
  bool shader_demote_to_helper_invocation = false;
  // VK_KHR_portability_subset::imageViewFormatSwizzle; true on every
  // non-portability device.
  bool image_view_format_swizzle = true;
};

inline SpirvShaderTranslator::Features MakeSpirvTranslatorFeatures(
    const SpirvTranslatorDeviceInfo& info) {
  SpirvShaderTranslator::Features features(false);
  if (info.api_version >= VK_MAKE_API_VERSION(0, 1, 2, 0)) {
    features.spirv_version = spv::Spv_1_5;
  } else if (info.spirv_1_4) {
    features.spirv_version = spv::Spv_1_4;
  } else if (info.api_version >= VK_MAKE_API_VERSION(0, 1, 1, 0)) {
    features.spirv_version = spv::Spv_1_3;
  } else {
    features.spirv_version = spv::Spv_1_0;
  }
  features.max_storage_buffer_range = info.max_storage_buffer_range;
  features.full_draw_index_uint32 = info.full_draw_index_uint32;
  features.vertex_pipeline_stores_and_atomics = info.vertex_pipeline_stores_and_atomics;
  features.fragment_stores_and_atomics = info.fragment_stores_and_atomics;
  features.clip_distance = info.shader_clip_distance;
  features.cull_distance = info.shader_cull_distance;
  features.image_view_format_swizzle = info.image_view_format_swizzle;
  features.signed_zero_inf_nan_preserve_float32 = info.shader_signed_zero_inf_nan_preserve_float32;
  features.denorm_flush_to_zero_float32 = info.shader_denorm_flush_to_zero_float32;
  features.rounding_mode_rte_float32 = info.shader_rounding_mode_rte_float32;
  features.fragment_shader_sample_interlock = info.fragment_shader_sample_interlock;
  features.demote_to_helper_invocation = info.shader_demote_to_helper_invocation;
  features.sample_rate_shading = info.sample_rate_shading;
  return features;
}

// Queries what the physical device SUPPORTS. `api_version` is the version the
// device will be used with (physical device apiVersion clamped to the instance
// version). The ext_* flags say which extensions the caller is going to enable
// on the device; a chained query is only issued for those. Properties2 /
// Features2 entry points may be null on a 1.0 instance without
// VK_KHR_get_physical_device_properties2, in which case only core 1.0 state is
// filled.
inline SpirvTranslatorDeviceInfo QuerySpirvTranslatorDeviceInfo(
    VkPhysicalDevice physical_device, PFN_vkGetPhysicalDeviceProperties get_properties,
    PFN_vkGetPhysicalDeviceFeatures get_features,
    PFN_vkGetPhysicalDeviceProperties2 get_properties2,
    PFN_vkGetPhysicalDeviceFeatures2 get_features2, uint32_t api_version,
    bool ext_khr_spirv_1_4, bool ext_khr_shader_float_controls,
    bool ext_ext_fragment_shader_interlock, bool ext_ext_shader_demote_to_helper_invocation,
    bool image_view_format_swizzle = true) {
  SpirvTranslatorDeviceInfo info;
  info.api_version = api_version;
  info.image_view_format_swizzle = image_view_format_swizzle;

  VkPhysicalDeviceProperties properties;
  std::memset(&properties, 0, sizeof(properties));
  get_properties(physical_device, &properties);
  info.max_storage_buffer_range = properties.limits.maxStorageBufferRange;

  VkPhysicalDeviceFeatures features;
  std::memset(&features, 0, sizeof(features));
  get_features(physical_device, &features);
  info.full_draw_index_uint32 = features.fullDrawIndexUint32 != VK_FALSE;
  info.vertex_pipeline_stores_and_atomics = features.vertexPipelineStoresAndAtomics != VK_FALSE;
  info.fragment_stores_and_atomics = features.fragmentStoresAndAtomics != VK_FALSE;
  info.shader_clip_distance = features.shaderClipDistance != VK_FALSE;
  info.shader_cull_distance = features.shaderCullDistance != VK_FALSE;
  info.sample_rate_shading = features.sampleRateShading != VK_FALSE;

  const bool has_1_2 = api_version >= VK_MAKE_API_VERSION(0, 1, 2, 0);
  const bool has_1_3 = api_version >= VK_MAKE_API_VERSION(0, 1, 3, 0);
  info.spirv_1_4 = has_1_2 || ext_khr_spirv_1_4;

  if (get_properties2 && (has_1_2 || ext_khr_shader_float_controls)) {
    VkPhysicalDeviceFloatControlsProperties float_controls;
    std::memset(&float_controls, 0, sizeof(float_controls));
    float_controls.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;
    VkPhysicalDeviceProperties2 properties2;
    std::memset(&properties2, 0, sizeof(properties2));
    properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties2.pNext = &float_controls;
    get_properties2(physical_device, &properties2);
    info.shader_signed_zero_inf_nan_preserve_float32 =
        float_controls.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE;
    info.shader_denorm_flush_to_zero_float32 =
        float_controls.shaderDenormFlushToZeroFloat32 != VK_FALSE;
    info.shader_rounding_mode_rte_float32 =
        float_controls.shaderRoundingModeRTEFloat32 != VK_FALSE;
  }

  if (get_features2) {
    VkPhysicalDeviceFeatures2 features2;
    std::memset(&features2, 0, sizeof(features2));
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    void** next = &features2.pNext;

    VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures demote;
    std::memset(&demote, 0, sizeof(demote));
    demote.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES;
    const bool query_demote = has_1_3 || ext_ext_shader_demote_to_helper_invocation;
    if (query_demote) {
      *next = &demote;
      next = &demote.pNext;
    }

    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock;
    std::memset(&interlock, 0, sizeof(interlock));
    interlock.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT;
    if (ext_ext_fragment_shader_interlock) {
      *next = &interlock;
      next = &interlock.pNext;
    }

    if (features2.pNext) {
      get_features2(physical_device, &features2);
      info.shader_demote_to_helper_invocation =
          query_demote && demote.shaderDemoteToHelperInvocation != VK_FALSE;
      info.fragment_shader_sample_interlock =
          ext_ext_fragment_shader_interlock && interlock.fragmentShaderSampleInterlock != VK_FALSE;
    }
  }
  return info;
}

}  // namespace rex::graphics
