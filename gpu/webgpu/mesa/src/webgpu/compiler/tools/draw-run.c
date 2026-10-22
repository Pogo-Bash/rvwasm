/*
 * draw-run: render one vertex + fragment shader pair twice and compare the pixels:
 *   (a) as SPIR-V on Vulkan (lavapipe), the reference;
 *   (b) as the WGSL wgsl-corpus made from them (with -link), on Dawn native.
 *
 *   draw-run DIR/NAME...   reads DIR/NAME-vert.{spv,wgsl} and DIR/NAME-frag.{spv,wgsl}
 *
 * The convention (tests/draw): no vertex buffers; 6 vertices, 2 instances (the vertex shader makes its
 * positions from gl_VertexIndex / gl_InstanceIndex); a 64x64 rgba8unorm target cleared to 0;
 * set 0: binding 0 a 256-byte uniform buffer (word i = the float i / 64), visible to both stages;
 * binding 1 a combined image sampler: a 16x16 rgba8unorm texture (a pattern), linear filtering,
 * repeat. In WGSL the sampler half is at binding 501 (the default remap).
 * Pixels are compared exactly, then with a tolerance of 1 (reported separately).
 *
 * It does not link Mesa (see cs-run.c).
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <webgpu/webgpu.h>

#define W 64
#define H 64
#define TW 16

static uint32_t ubo_init[64];
static uint8_t tex_init[TW * TW * 4];

static void
fill(void)
{
   for (unsigned i = 0; i < 64; i++) {
      float f = i / 64.0f;
      memcpy(&ubo_init[i], &f, 4);
   }
   for (unsigned y = 0; y < TW; y++)
      for (unsigned x = 0; x < TW; x++) {
         uint8_t *p = &tex_init[(y * TW + x) * 4];
         p[0] = x * 16;
         p[1] = y * 16;
         p[2] = ((x ^ y) & 1) ? 255 : 0;
         p[3] = 255 - x * 8;
      }
}

static void *
slurp(const char *path, long *size)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *p = malloc(sz + 1);
   if (fread(p, 1, sz, f) != (size_t)sz)
      sz = 0;
   p[sz] = 0;
   fclose(f);
   if (size)
      *size = sz;
   return p;
}

/* ------------------------------------------------------------------ Vulkan */

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, r_); return -1; } } while (0)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static VkPhysicalDeviceMemoryProperties mp;

static uint32_t
memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   return 0;
}

static int
vk_init(void)
{
   VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   VK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n)
      return -1;
   float prio = 1;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                   .queueCount = 1, .pQueuePriorities = &prio };
   VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE };
   VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
   f2.features.shaderClipDistance = VK_TRUE;
   f2.features.sampleRateShading = VK_TRUE;
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci };
   VK(vkCreateDevice(pd, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   return 0;
}

static int
vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b, VkDeviceMemory *m, void **map)
{
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
   VK(vkCreateBuffer(dev, &bci, NULL, b));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, *b, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(mr.memoryTypeBits, 6) };
   VK(vkAllocateMemory(dev, &mai, NULL, m));
   VK(vkBindBufferMemory(dev, *b, *m, 0));
   VK(vkMapMemory(dev, *m, 0, VK_WHOLE_SIZE, 0, map));
   return 0;
}

static int
vk_image(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *m, VkImageView *view)
{
   VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
   VK(vkCreateImage(dev, &ic, NULL, img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, *img, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) };
   VK(vkAllocateMemory(dev, &mai, NULL, m));
   VK(vkBindImageMemory(dev, *img, *m, 0));
   VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VK(vkCreateImageView(dev, &vc, NULL, view));
   return 0;
}

static void
barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static int
vk_draw(const uint32_t *vs, long vs_size, const uint32_t *fs, long fs_size, uint8_t *out)
{
   VkBuffer ub, stage, rb;
   VkDeviceMemory ubm, stagem, rbm;
   void *ubp, *stagep, *rbp;
   if (vk_buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ub, &ubm, &ubp) ||
       vk_buffer(sizeof(tex_init), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stage, &stagem, &stagep) ||
       vk_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb, &rbm, &rbp))
      return -1;
   memcpy(ubp, ubo_init, 256);
   memcpy(stagep, tex_init, sizeof(tex_init));
   VkImage tex, rt;
   VkDeviceMemory texm, rtm;
   VkImageView texv, rtv;
   if (vk_image(TW, TW, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &tex, &texm, &texv) ||
       vk_image(W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &rt, &rtm, &rtv))
      return -1;
   VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT, .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = 0.25f };
   VkSampler smp;
   VK(vkCreateSampler(dev, &sci, NULL, &smp));
   VkDescriptorSetLayoutBinding lb[2] = {
      { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
      { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
   };
   VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = lb };
   VkDescriptorSetLayout dsl;
   VK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl));
   VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl };
   VkPipelineLayout layout;
   VK(vkCreatePipelineLayout(dev, &pl, NULL, &layout));
   VkShaderModule vsm, fsm;
   VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vs_size, .pCode = vs };
   VK(vkCreateShaderModule(dev, &smi, NULL, &vsm));
   smi.codeSize = fs_size;
   smi.pCode = fs;
   VK(vkCreateShaderModule(dev, &smi, NULL, &fsm));
   VkPipelineShaderStageCreateInfo stages[2] = {
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" },
   };
   VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, W, H, 0, 1 };
   VkRect2D sc = { { 0, 0 }, { W, H } };
   VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1,
                                             .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
   VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba };
   VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   VkPipelineRenderingCreateInfo pr = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt };
   VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pr, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pColorBlendState = &cb, .layout = layout };
   VkPipeline pipe;
   VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
   VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = ps };
   VkDescriptorPool pool;
   VK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool));
   VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                       .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet set;
   VK(vkAllocateDescriptorSets(dev, &dsa, &set));
   VkDescriptorBufferInfo bi = { ub, 0, 256 };
   VkDescriptorImageInfo ii = { smp, texv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
   VkWriteDescriptorSet w[2] = {
      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi },
      { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii },
   };
   vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

   VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
   VkCommandPool cp;
   VK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
   VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd;
   VK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   vkBeginCommandBuffer(cmd, &cbi);
   barrier(cmd, tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TW, TW, 1 } };
   vkCmdCopyBufferToImage(cmd, stage, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
   barrier(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
   barrier(cmd, rt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
   VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = rtv,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
   VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca };
   vkCmdBeginRendering(cmd, &ri);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
   vkCmdDraw(cmd, 6, 2, 0, 0);
   vkCmdEndRendering(cmd);
   barrier(cmd, rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
   VkBufferImageCopy rbc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
   vkCmdCopyImageToBuffer(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &rbc);
   vkEndCommandBuffer(cmd);
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
   VK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   vkQueueWaitIdle(q);
   memcpy(out, rbp, W * H * 4);
   return 0; /* (objects are leaked: one process per test) */
}

/* ------------------------------------------------------------------ Dawn */

static WGPUInstance winst;
static WGPUAdapter wadapter;
static WGPUDevice wdev;
static int wmapped, werror;
static char werrmsg[4096];

static WGPUStringView sv(const char *s) { return (WGPUStringView){ s, strlen(s) }; }
static void on_adapter(WGPURequestAdapterStatus st, WGPUAdapter a, WGPUStringView msg, void *u1, void *u2)
{ if (st == WGPURequestAdapterStatus_Success) wadapter = a; }
static void on_device(WGPURequestDeviceStatus st, WGPUDevice d, WGPUStringView msg, void *u1, void *u2)
{ if (st == WGPURequestDeviceStatus_Success) wdev = d; else fprintf(stderr, "device: %.*s\n", (int)msg.length, msg.data); }
static void on_map(WGPUMapAsyncStatus st, WGPUStringView msg, void *u1, void *u2) { wmapped = st == WGPUMapAsyncStatus_Success ? 1 : -1; }
static void on_error(const WGPUDevice *d, WGPUErrorType type, WGPUStringView msg, void *u1, void *u2)
{
   werror = 1;
   snprintf(werrmsg, sizeof(werrmsg), "%.*s", (int)msg.length, msg.data);
}

static int
wgpu_init(void)
{
   winst = wgpuCreateInstance(NULL);
   WGPURequestAdapterOptions ao = { .backendType = WGPUBackendType_Vulkan, .featureLevel = WGPUFeatureLevel_Core };
   for (int fb = 0; fb < 2 && !wadapter; fb++) {
      ao.forceFallbackAdapter = fb;
      wgpuInstanceRequestAdapter(winst, &ao, (WGPURequestAdapterCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_adapter });
      for (int i = 0; i < 100 && !wadapter; i++)
         wgpuInstanceProcessEvents(winst);
   }
   if (!wadapter)
      return -1;
   WGPUFeatureName want[] = { WGPUFeatureName_ShaderF16, WGPUFeatureName_Subgroups, WGPUFeatureName_ClipDistances,
                              WGPUFeatureName_PrimitiveIndex, WGPUFeatureName_DualSourceBlending };
   WGPUFeatureName have[5];
   unsigned n = 0;
   for (unsigned i = 0; i < 5; i++)
      if (wgpuAdapterHasFeature(wadapter, want[i]))
         have[n++] = want[i];
   WGPULimits lim = WGPU_LIMITS_INIT;
   wgpuAdapterGetLimits(wadapter, &lim);
   WGPUDeviceDescriptor dd = { .requiredFeatureCount = n, .requiredFeatures = have, .requiredLimits = &lim,
                               .uncapturedErrorCallbackInfo = { .callback = on_error } };
   wgpuAdapterRequestDevice(wadapter, &dd, (WGPURequestDeviceCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_device });
   for (int i = 0; i < 100 && !wdev; i++)
      wgpuInstanceProcessEvents(winst);
   return wdev ? 0 : -1;
}

static bool
uses(const char *wgsl, const char *binding)
{
   return strstr(wgsl, binding) != NULL;
}

static int
wgpu_draw(const char *vs, const char *fs, uint8_t *out)
{
   werror = 0;
   WGPUShaderSourceWGSL vsrc = { .chain = { .sType = WGPUSType_ShaderSourceWGSL }, .code = sv(vs) };
   WGPUShaderModuleDescriptor vmd = { .nextInChain = &vsrc.chain };
   WGPUShaderModule vsm = wgpuDeviceCreateShaderModule(wdev, &vmd);
   WGPUShaderSourceWGSL fsrc = { .chain = { .sType = WGPUSType_ShaderSourceWGSL }, .code = sv(fs) };
   WGPUShaderModuleDescriptor fmd = { .nextInChain = &fsrc.chain };
   WGPUShaderModule fsm = wgpuDeviceCreateShaderModule(wdev, &fmd);
   WGPUColorTargetState ct = { .format = WGPUTextureFormat_RGBA8Unorm, .writeMask = WGPUColorWriteMask_All };
   WGPUFragmentState fst = { .module = fsm, .entryPoint = sv("main"), .targetCount = 1, .targets = &ct };
   WGPURenderPipelineDescriptor rpd = {
      .vertex = { .module = vsm, .entryPoint = sv("main") },
      .primitive = { .topology = WGPUPrimitiveTopology_TriangleList, .cullMode = WGPUCullMode_None,
                     /* the same front face as Vulkan: with y negated in the vertex shader the framebuffer
                      * image, and so the winding WebGPU sees, is Vulkan's (measured: gl_FrontFacing agrees) */
                     .frontFace = WGPUFrontFace_CCW },
      .multisample = { .count = 1, .mask = ~0u },
      .fragment = &fst,
   };
   WGPURenderPipeline rp = wgpuDeviceCreateRenderPipeline(wdev, &rpd);
   wgpuInstanceProcessEvents(winst);
   if (werror) {
      fprintf(stderr, "dawn: %s\n", werrmsg);
      return -1;
   }
   WGPUQueue queue = wgpuDeviceGetQueue(wdev);
   WGPUBufferDescriptor ubd = { .size = 256, .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst };
   WGPUBuffer ub = wgpuDeviceCreateBuffer(wdev, &ubd);
   wgpuQueueWriteBuffer(queue, ub, 0, ubo_init, 256);
   WGPUTextureDescriptor td = { .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                .dimension = WGPUTextureDimension_2D, .size = { TW, TW, 1 },
                                .format = WGPUTextureFormat_RGBA8Unorm, .mipLevelCount = 1, .sampleCount = 1 };
   WGPUTexture tex = wgpuDeviceCreateTexture(wdev, &td);
   WGPUTexelCopyTextureInfo dst = { .texture = tex };
   WGPUTexelCopyBufferLayout lay = { .bytesPerRow = TW * 4, .rowsPerImage = TW };
   WGPUExtent3D ext = { TW, TW, 1 };
   wgpuQueueWriteTexture(queue, &dst, tex_init, sizeof(tex_init), &lay, &ext);
   WGPUSamplerDescriptor sd = { .addressModeU = WGPUAddressMode_Repeat, .addressModeV = WGPUAddressMode_Repeat,
                                .addressModeW = WGPUAddressMode_Repeat, .magFilter = WGPUFilterMode_Linear,
                                .minFilter = WGPUFilterMode_Linear, .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                                .lodMinClamp = 0, .lodMaxClamp = 0.25f, .maxAnisotropy = 1 };
   WGPUSampler smp = wgpuDeviceCreateSampler(wdev, &sd);
   WGPUBindGroupEntry be[3];
   unsigned nbe = 0;
   bool any_ub = uses(vs, "@binding(0)") || uses(fs, "@binding(0)");
   bool any_tex = uses(vs, "@binding(1) ") || uses(fs, "@binding(1) ");
   bool any_smp = uses(vs, "@binding(501)") || uses(fs, "@binding(501)");
   if (any_ub)
      be[nbe++] = (WGPUBindGroupEntry){ .binding = 0, .buffer = ub, .size = 256 };
   if (any_tex)
      be[nbe++] = (WGPUBindGroupEntry){ .binding = 1, .textureView = wgpuTextureCreateView(tex, NULL) };
   if (any_smp)
      be[nbe++] = (WGPUBindGroupEntry){ .binding = 501, .sampler = smp };
   WGPUTextureDescriptor rtd = { .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
                                 .dimension = WGPUTextureDimension_2D, .size = { W, H, 1 },
                                 .format = WGPUTextureFormat_RGBA8Unorm, .mipLevelCount = 1, .sampleCount = 1 };
   WGPUTexture rt = wgpuDeviceCreateTexture(wdev, &rtd);
   WGPUBufferDescriptor rbd = { .size = W * H * 4, .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst };
   WGPUBuffer rb = wgpuDeviceCreateBuffer(wdev, &rbd);
   WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(wdev, NULL);
   WGPURenderPassColorAttachment rca = { .view = wgpuTextureCreateView(rt, NULL), .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                                         .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store,
                                         .clearValue = { 0, 0, 0, 0 } };
   WGPURenderPassDescriptor rpass = { .colorAttachmentCount = 1, .colorAttachments = &rca };
   WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rpass);
   wgpuRenderPassEncoderSetPipeline(pass, rp);
   if (nbe) {
      WGPUBindGroupDescriptor bgd = { .layout = wgpuRenderPipelineGetBindGroupLayout(rp, 0), .entryCount = nbe, .entries = be };
      wgpuRenderPassEncoderSetBindGroup(pass, 0, wgpuDeviceCreateBindGroup(wdev, &bgd), 0, NULL);
   }
   wgpuRenderPassEncoderDraw(pass, 6, 2, 0, 0);
   wgpuRenderPassEncoderEnd(pass);
   WGPUTexelCopyTextureInfo src = { .texture = rt };
   WGPUTexelCopyBufferInfo dstb = { .layout = { .bytesPerRow = W * 4, .rowsPerImage = H }, .buffer = rb };
   WGPUExtent3D rext = { W, H, 1 };
   wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dstb, &rext);
   WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
   wgpuQueueSubmit(queue, 1, &cmd);
   wmapped = 0;
   wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, W * H * 4, (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map });
   for (time_t t0 = time(NULL); !wmapped && time(NULL) - t0 < 300;)
      wgpuInstanceProcessEvents(winst);
   if (wmapped != 1 || werror) {
      fprintf(stderr, "dawn: %s\n", werrmsg);
      return -1;
   }
   memcpy(out, wgpuBufferGetConstMappedRange(rb, 0, W * H * 4), W * H * 4);
   return 0;
}

int
main(int argc, char **argv)
{
   fill();
   if (vk_init() || wgpu_init()) {
      fprintf(stderr, "no Vulkan or Dawn device\n");
      return 1;
   }
   int pass = 0, fail_n = 0;
   static uint8_t ref[W * H * 4], got[W * H * 4];
   for (int a = 1; a < argc; a++) {
      const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
      char p[1100];
      long vss, fss;
      snprintf(p, sizeof(p), "%s-vert.spv", argv[a]);
      uint32_t *vs = slurp(p, &vss);
      snprintf(p, sizeof(p), "%s-frag.spv", argv[a]);
      uint32_t *fs = slurp(p, &fss);
      snprintf(p, sizeof(p), "%s-vert.wgsl", argv[a]);
      char *vw = slurp(p, NULL);
      snprintf(p, sizeof(p), "%s-frag.wgsl", argv[a]);
      char *fw = slurp(p, NULL);
      if (!vs || !fs || !vw || !fw) {
         printf("FAIL %s: missing spv or wgsl (see the .err files)\n", name);
         fail_n++;
         continue;
      }
      memset(ref, 0, sizeof(ref));
      memset(got, 0xcd, sizeof(got));
      if (vk_draw(vs, vss, fs, fss, ref)) {
         printf("FAIL %s: vulkan\n", name);
         fail_n++;
         continue;
      }
      if (wgpu_draw(vw, fw, got)) {
         printf("FAIL %s: dawn\n", name);
         fail_n++;
         continue;
      }
      unsigned exact = 0, off1 = 0, lit = 0;
      for (unsigned i = 0; i < W * H; i++) {
         bool e = !memcmp(&ref[i * 4], &got[i * 4], 4), close = true;
         for (unsigned k = 0; k < 4; k++)
            close = close && abs((int)ref[i * 4 + k] - (int)got[i * 4 + k]) <= 1;
         lit += ref[i * 4] | ref[i * 4 + 1] | ref[i * 4 + 2] | ref[i * 4 + 3] ? 1 : 0;
         if (!e && close)
            off1++;
         else if (!e) {
            if (exact < 6)
               printf("    pixel %u,%u: vulkan %02x%02x%02x%02x wgsl %02x%02x%02x%02x\n", i % W, i / W, ref[i * 4],
                      ref[i * 4 + 1], ref[i * 4 + 2], ref[i * 4 + 3], got[i * 4], got[i * 4 + 1], got[i * 4 + 2],
                      got[i * 4 + 3]);
            exact++;
         }
      }
      printf("%s %s (%u pixels differ, %u off by one, %u of %u drawn)\n", exact ? "FAIL" : "ok", name, exact, off1, lit, W * H);
      exact ? fail_n++ : pass++;
   }
   printf("draw-run: %d ok, %d failed\n", pass, fail_n);
   return fail_n != 0;
}
