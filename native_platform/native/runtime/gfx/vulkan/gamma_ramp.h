/**
 ******************************************************************************
 * xenon-native - the display gamma ramp, applied at scan-out
 ******************************************************************************
 * WHAT THE HARDWARE DOES
 *
 * The console keeps a 128-entry piecewise-linear display LUT that the guest
 * programs through the DC_LUT registers (what D3D's SetGammaRamp and
 * SetPWLGamma drive) and that the display controller applies to the finished
 * frame on its way to the screen. It is a per-channel curve over the whole
 * picture, after everything else.
 *
 * Geometry Wars 1 programs the 256-entry, 10-bit table. Its D3D initialization
 * only does this for process type X_PROCTYPE_USER (1); reporting type 2 caused
 * it to skip the table and made the bloom appear too bright at scan-out.
 * Table values are expanded to UNORM16 before uploading the LUT image.
 *
 * Geometry Wars 2 rewrites the whole ramp continuously: 1,115 writes by frame
 * 600, 1,715 by 1,200, 2,315 by 1,800. Decoded from its own writes, the curve
 * it asks for (normalised, 128 entries, peak 65472) is:
 *
 *      input   output   ratio
 *      0.008    0.003   x0.37
 *      0.032    0.011   x0.34
 *      0.126    0.066   x0.52
 *      0.252    0.191   x0.76
 *      0.504    0.455   x0.90
 *      0.756    0.729   x0.97
 *
 * It DARKENS, hardest at the bottom and barely at all near white - roughly
 * pow(x, 1.15..1.3). That is the inverse of the defect measured on this title:
 * a floor lifted most in the darks with highlights slightly compressed. The
 * title is asking the display to pull its blacks down and we never did it.
 *
 * THE DECODE, since it is not obvious and cost a wrong reading first time
 *
 * DC_LUT_PWL_DATA carries the segment BASE in the low 16 bits and the DELTA in
 * the high 16. The guest's own writes satisfy base[n+1] == base[n] + delta[n]
 * exactly, so the base values ARE the curve sampled once per entry and the
 * deltas are redundant. The table is 128 entries, not 256: entries 128..255 are
 * never written. Sampling the LUT with LINEAR filtering reproduces the
 * piecewise-linear evaluation the hardware does, for these continuous ramps (PWL delta quantisation and endpoints remain
 * approximate).
 *
 * WHAT IS APPROXIMATE
 *
 * The console applies the ramp at scan-out, to the signal. We apply it in a
 * pass at present time, to the frame we are about to show. For a title that
 * only reads back its own rendering (neither title reads the front buffer)
 * those are the same thing. A title that captured the screen after scan-out
 * would see a difference.
 */
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vulkan/vulkan.h>

#include "gamma_ramp_shaders.h"
#include "resolve_bias_shaders.h"  // kResolveBiasVert: same fullscreen triangle

namespace xn::gfx {

// Full-screen pass: sample `src` (scaled to the destination), run each channel
// through `lut`, write to `dest`. Owns its pipeline, layouts and samplers;
// borrows a fence-owned descriptor pool. The renderer owns the LUT image.
struct GammaRampPass {
  VkDevice device = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkSampler src_sampler = VK_NULL_HANDLE;  // LINEAR, the present blit's filter
  VkSampler lut_sampler = VK_NULL_HANDLE;  // LINEAR, the PWL interpolation
  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  std::unordered_map<uint32_t, VkPipeline> pipelines;                 // by dest VkFormat

  struct Push {
    float inv_size[2];
    float lut_scale;
    float lut_offset;
  };
  static_assert(sizeof(Push) == 16, "push constant layout");

  bool Init(VkDevice d) {
    if (device) return true;
    device = d;
    VkDescriptorSetLayoutBinding b[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      b[i].binding = i;
      b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 2;
    lci.pBindings = b;
    if (vkCreateDescriptorSetLayout(device, &lci, nullptr, &set_layout) != VK_SUCCESS) {
      return Fail();
    }
    VkPushConstantRange pcr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &set_layout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device, &plci, nullptr, &pipeline_layout) != VK_SUCCESS) {
      return Fail();
    }
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device, &sci, nullptr, &src_sampler) != VK_SUCCESS) return Fail();
    if (vkCreateSampler(device, &sci, nullptr, &lut_sampler) != VK_SUCCESS) return Fail();

    vert = MakeModule(kResolveBiasVert, sizeof(kResolveBiasVert));
    frag = MakeModule(kGammaRampFrag, sizeof(kGammaRampFrag));
    if (!vert || !frag) return Fail();
    return true;
  }

  void Destroy() {
    if (!device) return;
    for (auto& p : pipelines) vkDestroyPipeline(device, p.second, nullptr);
    pipelines.clear();
    if (vert) vkDestroyShaderModule(device, vert, nullptr);
    if (frag) vkDestroyShaderModule(device, frag, nullptr);
    if (src_sampler) vkDestroySampler(device, src_sampler, nullptr);
    if (lut_sampler) vkDestroySampler(device, lut_sampler, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    vert = frag = VK_NULL_HANDLE;
    src_sampler = lut_sampler = VK_NULL_HANDLE;
    pipeline_layout = VK_NULL_HANDLE;
    set_layout = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
  }

  // `src` and `lut` must be in SHADER_READ_ONLY_OPTIMAL, `dest` in
  // COLOR_ATTACHMENT_OPTIMAL, and no render pass may be open. `entries` is the
  // number of LUT texels actually written by the guest (128 here).
  bool Apply(VkCommandBuffer cb, VkDescriptorPool pool, VkImageView src, VkImageView lut, VkImageView dest,
             VkFormat dest_format, uint32_t width, uint32_t height, uint32_t entries) {
    if (!device || !width || !height || !entries) return false;
    VkPipeline pipeline = GetPipeline(dest_format);
    VkDescriptorSet set = GetSet(pool, src, lut);
    if (!pipeline || !set) return false;

    VkRenderingAttachmentInfo att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = dest;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every texel is written
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {width, height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cb, &ri);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport vp = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, {width, height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &set, 0,
                            nullptr);
    Push push = {};
    push.inv_size[0] = 1.0f / float(width);
    push.inv_size[1] = 1.0f / float(height);
    // A value v addresses entry v*(entries-1); texel centres are (i+0.5)/entries.
    push.lut_scale = float(entries - 1) / float(entries);
    push.lut_offset = 0.5f / float(entries);
    vkCmdPushConstants(cb, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    return true;
  }

 private:

  bool Fail() {
    Destroy();
    return false;
  }

  VkShaderModule MakeModule(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
  }

  VkDescriptorSet GetSet(VkDescriptorPool pool, VkImageView src, VkImageView lut) {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo ii[2] = {
        {src_sampler, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {lut_sampler, lut, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet wr[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wr[i].dstSet = set;
      wr[i].dstBinding = i;
      wr[i].descriptorCount = 1;
      wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(device, 2, wr, 0, nullptr);
    return set;
  }

  VkPipeline GetPipeline(VkFormat dest_format) {
    auto it = pipelines.find(uint32_t(dest_format));
    if (it != pipelines.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = {
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vs = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vs.viewportCount = 1;
    vs.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba = {};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfo prci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount = 1;
    prci.pColorAttachmentFormats = &dest_format;

    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &prci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vs;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = pipeline_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline) !=
        VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    pipelines[uint32_t(dest_format)] = pipeline;
    return pipeline;
  }
};

}  // namespace xn::gfx
