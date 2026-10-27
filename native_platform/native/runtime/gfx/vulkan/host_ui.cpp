#include "vulkan_renderer_impl.h"
#include "presentation_mode.h"
#include "host_ui_vert.h"
#include "host_ui_frag.h"
#include "host_sharpen_frag.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>

namespace xn::gfx {
void VulkanRenderer::SetHostOverlay(std::shared_ptr<const HostUiFrame> frame) {
  std::lock_guard lock(impl_->mutex);
  impl_->host_ui_frame = std::move(frame);
}
void VulkanRenderer::SetPresentationSettings(const PresentationSettings& value) {
  std::lock_guard lock(impl_->mutex);
  impl_->presentation_settings = value;
  impl_->presentation_settings.sharpness = std::clamp(value.sharpness,0.f,1.f);
  if (impl_->config.vsync != value.vsync) {
    impl_->config.vsync = value.vsync;
    impl_->swapchain_dirty = true;
  }
}
std::string VulkanRenderer::DeviceName() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->properties.deviceName;
}
uint64_t VulkanRenderer::PresentedFrames() const {
  return impl_->presented_frames.load(std::memory_order_relaxed);
}
void VulkanRenderer::RedrawHostOverlay() {
  auto& im = *impl_;
  std::unique_lock lock(im.mutex,std::try_to_lock);
  if (!lock || !im.device || !im.swapchain || !im.host_ui_frame) return;
  const uint64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (now-im.last_host_present_ns < 16000000) return;
  im.PresentImpl(im.last_host_present);
}
bool VulkanRenderer::Impl::InitHostUi() {
  if (host_ui_pipeline_layout) return true;
  auto shader = [&](const uint32_t* code,size_t size,VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize=size; info.pCode=code;
    return vkCreateShaderModule(device,&info,nullptr,&module)==VK_SUCCESS;
  };
  if (!shader(xn_host_ui_vert,sizeof(xn_host_ui_vert),host_ui_vs) ||
      !shader(xn_host_ui_frag,sizeof(xn_host_ui_frag),host_ui_ps) ||
      !shader(xn_host_sharpen_frag,sizeof(xn_host_sharpen_frag),host_sharpen_ps)) return false;
  VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,
                                       VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
  VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout.bindingCount=1; layout.pBindings=&binding;
  XN_VK_CHECK(vkCreateDescriptorSetLayout(device,&layout,nullptr,&host_ui_texture_layout));
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,20};
  VkPipelineLayoutCreateInfo pipeline_layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout.setLayoutCount=1; pipeline_layout.pSetLayouts=&host_ui_texture_layout;
  pipeline_layout.pushConstantRangeCount=1; pipeline_layout.pPushConstantRanges=&push;
  XN_VK_CHECK(vkCreatePipelineLayout(device,&pipeline_layout,nullptr,&host_ui_pipeline_layout));
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
  sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  XN_VK_CHECK(vkCreateSampler(device,&sampler,nullptr,&host_ui_sampler));
  return true;
}
VkPipeline VulkanRenderer::Impl::HostUiPipeline(bool sharpen) {
  auto& cache = sharpen ? host_sharpen_pipelines : host_ui_pipelines;
  if (auto it=cache.find(uint32_t(swapchain_format));it!=cache.end()) return it->second;
  VkPipelineShaderStageCreateInfo stages[2]{};
  for (auto& stage:stages) { stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stage.pName="main"; }
  stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=host_ui_vs;
  stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=sharpen?host_sharpen_ps:host_ui_ps;
  VkVertexInputBindingDescription binding{0,sizeof(HostUiVertex),VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attributes[]={{0,0,VK_FORMAT_R32G32_SFLOAT,0},
      {1,0,VK_FORMAT_R32G32_SFLOAT,8},{2,0,VK_FORMAT_R8G8B8A8_UNORM,16}};
  VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex.vertexBindingDescriptionCount=1; vertex.pVertexBindingDescriptions=&binding;
  vertex.vertexAttributeDescriptionCount=3; vertex.pVertexAttributeDescriptions=attributes;
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount=viewport.scissorCount=1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask=15; blend.blendEnable=!sharpen;
  blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA; blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp=blend.alphaBlendOp=VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE; blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blending.attachmentCount=1; blending.pAttachments=&blend;
  VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount=2; dynamic.pDynamicStates=states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount=1; rendering.pColorAttachmentFormats=&swapchain_format;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext=&rendering; info.stageCount=2; info.pStages=stages;
  info.pVertexInputState=&vertex; info.pInputAssemblyState=&assembly; info.pViewportState=&viewport;
  info.pRasterizationState=&raster; info.pMultisampleState=&samples;
  info.pColorBlendState=&blending; info.pDynamicState=&dynamic; info.layout=host_ui_pipeline_layout;
  VkPipeline pipeline=VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&pipeline)!=VK_SUCCESS) return VK_NULL_HANDLE;
  cache.emplace(uint32_t(swapchain_format),pipeline);
  return pipeline;
}
bool VulkanRenderer::Impl::DrawHostUi(VkImage output,uint32_t width,uint32_t height,GpuImage* source) {
  InvalidateDrawState();
  auto ui=host_ui_frame;
  const bool has_ui=ui && ui->font && !ui->vertices.empty() && !ui->indices.empty();
  if ((!has_ui && !source) || !width || !height || !InitHostUi()) return false;
  auto& frame=frames[frame_index];
  if (has_ui && host_ui_font_source!=ui->font) {
    if (!ui->font->width || !ui->font->height ||
        ui->font->rgba.size()!=uint64_t(ui->font->width)*ui->font->height*4) return false;
    if (host_ui_font.image) { frame.garbage_images.push_back(host_ui_font); host_ui_font={}; }
    if (!CreateImage(ui->font->width,ui->font->height,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,VK_IMAGE_ASPECT_COLOR_BIT,host_ui_font)) return false;
    HostBuffer staging;
    if (!CreateHostBuffer(ui->font->rgba.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,staging)) return false;
    std::memcpy(staging.mapped,ui->font->rgba.data(),ui->font->rgba.size());
    Transition(host_ui_font,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={ui->font->width,ui->font->height,1};
    vkCmdCopyBufferToImage(cb,staging.buffer,host_ui_font.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    Transition(host_ui_font,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.garbage.push_back(staging);
    host_ui_font_source=ui->font;
  }
  const uint32_t prefix=source?4:0;
  const size_t vertex_count=prefix+(has_ui?ui->vertices.size():0);
  const size_t index_count=(source?6:0)+(has_ui?ui->indices.size():0);
  const VkDeviceSize index_offset=(vertex_count*sizeof(HostUiVertex)+3)&~VkDeviceSize(3);
  const VkDeviceSize needed=index_offset+index_count*sizeof(uint32_t);
  if (needed>frame.host_ui_geometry.size) {
    DestroyHostBuffer(frame.host_ui_geometry);
    if (!CreateHostBuffer(std::bit_ceil(std::max<VkDeviceSize>(needed,65536)),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT,frame.host_ui_geometry)) return false;
  }
  auto* vertices=reinterpret_cast<HostUiVertex*>(frame.host_ui_geometry.mapped);
  auto* indices=reinterpret_cast<uint32_t*>(frame.host_ui_geometry.mapped+index_offset);
  if (source) {
    const auto rect=FitPresentationRect({source->width,source->height},{width,height});
    const float left=rect.offset.x, top=rect.offset.y;
    const float right=left+rect.extent.width,bottom=top+rect.extent.height;
    const HostUiVertex quad[]={{{left,top},{0,0},{255,255,255,255}},{{right,top},{1,0},{255,255,255,255}},
        {{right,bottom},{1,1},{255,255,255,255}},{{left,bottom},{0,1},{255,255,255,255}}};
    std::memcpy(vertices,quad,sizeof(quad));
    const uint32_t triangles[]={0,1,2,0,2,3}; std::memcpy(indices,triangles,sizeof(triangles));
    Transition(*source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
  if (has_ui) {
    std::memcpy(vertices+prefix,ui->vertices.data(),ui->vertices.size()*sizeof(HostUiVertex));
    std::memcpy(indices+(source?6:0),ui->indices.data(),ui->indices.size()*sizeof(uint32_t));
  }
  auto descriptor=[&](VkImageView view) -> VkDescriptorSet {
    VkDescriptorSet set=VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocation.descriptorPool=frame.texture_pool; allocation.descriptorSetCount=1;
    allocation.pSetLayouts=&host_ui_texture_layout;
    if (vkAllocateDescriptorSets(device,&allocation,&set)!=VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo image{host_ui_sampler,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=set; write.dstBinding=0; write.descriptorCount=1;
    write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo=&image;
    vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    return set;
  };
  VkDescriptorSet image_set=source?descriptor(source->view):VK_NULL_HANDLE;
  VkDescriptorSet font_set=has_ui?descriptor(host_ui_font.view):VK_NULL_HANDLE;
  auto found=std::find(swapchain_images.begin(),swapchain_images.end(),output);
  if (found==swapchain_images.end()) return false;
  VkPipeline ui_pipeline=has_ui?HostUiPipeline(false):VK_NULL_HANDLE;
  VkPipeline sharpen_pipeline=source?HostUiPipeline(true):VK_NULL_HANDLE;
  if ((has_ui && (!font_set || !ui_pipeline)) || (source && (!image_set || !sharpen_pipeline))) return false;
  TransitionRaw(output,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  attachment.imageView=swapchain_image_views[size_t(found-swapchain_images.begin())];
  attachment.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent={width,height}; rendering.layerCount=1;
  rendering.colorAttachmentCount=1; rendering.pColorAttachments=&attachment;
  vkCmdBeginRendering(cb,&rendering);
  VkViewport viewport{0,0,float(width),float(height),0,1}; vkCmdSetViewport(cb,0,1,&viewport);
  VkRect2D scissor{{0,0},{width,height}}; vkCmdSetScissor(cb,0,1,&scissor);
  const VkDeviceSize zero=0;
  vkCmdBindVertexBuffers(cb,0,1,&frame.host_ui_geometry.buffer,&zero);
  vkCmdBindIndexBuffer(cb,frame.host_ui_geometry.buffer,index_offset,VK_INDEX_TYPE_UINT32);
  const float push[]={2.f/width,2.f/height,-1,-1,presentation_settings.sharpness};
  vkCmdPushConstants(cb,host_ui_pipeline_layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),push);
  if (source) {
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,sharpen_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,host_ui_pipeline_layout,0,1,&image_set,0,nullptr);
    vkCmdDrawIndexed(cb,6,1,0,0,0);
  }
  if (has_ui) {
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,ui_pipeline);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,host_ui_pipeline_layout,0,1,&font_set,0,nullptr);
    for (const auto& command:ui->commands) {
      const int32_t x=int32_t(std::clamp(command.clip[0],0.f,float(width)));
      const int32_t y=int32_t(std::clamp(command.clip[1],0.f,float(height)));
      const int32_t right=int32_t(std::clamp(command.clip[0]+command.clip[2],0.f,float(width)));
      const int32_t bottom=int32_t(std::clamp(command.clip[1]+command.clip[3],0.f,float(height)));
      if (!command.index_count || right<=x || bottom<=y ||
          uint64_t(command.index_offset)+command.index_count>ui->indices.size()) continue;
      scissor={{x,y},{uint32_t(right-x),uint32_t(bottom-y)}};
      vkCmdSetScissor(cb,0,1,&scissor);
      vkCmdDrawIndexed(cb,command.index_count,1,command.index_offset+(source?6:0),prefix,0);
    }
  }
  vkCmdEndRendering(cb);
  return true;
}
void VulkanRenderer::Impl::DestroyHostUi() {
  for (auto& [format,pipeline]:host_ui_pipelines) vkDestroyPipeline(device,pipeline,nullptr);
  for (auto& [format,pipeline]:host_sharpen_pipelines) vkDestroyPipeline(device,pipeline,nullptr);
  host_ui_pipelines.clear(); host_sharpen_pipelines.clear();
  if (host_ui_vs) vkDestroyShaderModule(device,host_ui_vs,nullptr);
  if (host_ui_ps) vkDestroyShaderModule(device,host_ui_ps,nullptr);
  if (host_sharpen_ps) vkDestroyShaderModule(device,host_sharpen_ps,nullptr);
  if (host_ui_pipeline_layout) vkDestroyPipelineLayout(device,host_ui_pipeline_layout,nullptr);
  if (host_ui_texture_layout) vkDestroyDescriptorSetLayout(device,host_ui_texture_layout,nullptr);
  if (host_ui_sampler) vkDestroySampler(device,host_ui_sampler,nullptr);
  DestroyImage(host_ui_font);
  for (auto& frame:frames) DestroyHostBuffer(frame.host_ui_geometry);
  host_ui_vs=host_ui_ps=host_sharpen_ps=VK_NULL_HANDLE;
  host_ui_pipeline_layout=VK_NULL_HANDLE; host_ui_texture_layout=VK_NULL_HANDLE; host_ui_sampler=VK_NULL_HANDLE;
}
}  // namespace xn::gfx
