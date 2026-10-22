/*
 * cs-run: run one compute shader twice and compare the memory it leaves behind:
 *   (a) as SPIR-V on Vulkan (lavapipe), the reference, run three times so that words a racy shader leaves
 *       different from run to run are not counted;
 *   (b) as the WGSL wgsl-corpus made from it, on Dawn native (webgpu.h).
 *
 *   cs-run [-groups N] [-pattern] DIR/NAME.spv...   (reads DIR/NAME.wgsl and DIR/NAME.bind next to each .spv)
 *
 * Buffers come from the .bind table (only uniform and storage buffers; a shader with textures or images
 * is skipped). Two ways to fill them:
 *   default    the tests/cs convention, set 0: binding 0 a 256-byte uniform buffer with word i = i;
 *              binding 2 "interesting" values; every other storage buffer zero; 16 KiB each
 *   -pattern   every buffer (64 KiB storage, the declared size for uniform) filled with small integers,
 *              for shaders that were not written for this (Blender's compute shaders)
 * Every storage buffer is read back and compared word by word; NaNs compare equal.
 *
 * What T3b's legalisation turned into buffers (".bind" lines "lowered KIND format NAME"; wgsl-corpus
 * -legalize writes them) runs as the original on Vulkan: texel buffers as buffer views of that format,
 * images used atomically as R32 storage images (32x16, 4 layers when arrayed) copied in from and out to
 * the buffer's texels after its 16-byte header. Storage images (".bind" kind 4, followed by a line
 * "    storage_format NAME") are IMG_W x IMG_H images of that format on both sides, filled from and read
 * back into a buffer laid out the same way (header + texels), which is what is compared. Driver bindings (vk set 255) exist on the WebGPU side only:
 * 900 holds the push constants (128 bytes on Vulkan), 901 the driver uniforms (zero). Storage buffers
 * the WGSL reaches through the buffer heap (".bind" lines "heap slot K vk S.B[I]") are their own buffers on
 * Vulkan and HEAP_STRIDE-byte pieces of the heap arena (910) on WebGPU, described by the table at 902.
 *
 * It does not link Mesa: Mesa's static SPIRV-Tools would interpose the copy inside libwebgpu_dawn.so.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include <webgpu/webgpu.h>

#define MAX_RES 32

struct res {
   unsigned group, wbinding;           /* WebGPU */
   unsigned set, binding, index, count; /* Vulkan (count = the shader's array size) */
   bool ubo;
   unsigned size;                      /* bytes */
   uint32_t *init;
   unsigned kind;                      /* the .bind kind (0 uniform, 1 storage, 2 read-only storage) */
   bool driver;                        /* vk set 255: WebGPU only */
   unsigned lowered;                   /* 1 image buffer, 2 texel buffer */
   bool storage_texel;
   char fmt[64];                       /* PIPE_FORMAT_* of a lowered binding */
   int heap_slot;                      /* >= 0: a storage buffer the WGSL reaches through the buffer heap */
   int simg;                           /* >= 0: a storage image of format sfmt[simg] */
   unsigned dim, arrayed;
};

#define HEAP_STRIDE 16384
#define IMG_W 64   /* rows of 256 bytes at 4 bytes per texel: WebGPU copies want that */
#define IMG_H 16

static unsigned
img_layers(const struct res *r)
{
   return r->arrayed ? 4 : 1;
}

static const struct { const char *n; VkFormat vk; WGPUTextureFormat wg; unsigned bpp; bool fl; } sfmt[] = {
   { "PIPE_FORMAT_R32_UINT", VK_FORMAT_R32_UINT, WGPUTextureFormat_R32Uint, 4, false },
   { "PIPE_FORMAT_R32_SINT", VK_FORMAT_R32_SINT, WGPUTextureFormat_R32Sint, 4, false },
   { "PIPE_FORMAT_R32_FLOAT", VK_FORMAT_R32_SFLOAT, WGPUTextureFormat_R32Float, 4, true },
   { "PIPE_FORMAT_R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, WGPUTextureFormat_RGBA8Unorm, 4, false },
   { "PIPE_FORMAT_R8G8B8A8_UINT", VK_FORMAT_R8G8B8A8_UINT, WGPUTextureFormat_RGBA8Uint, 4, false },
   { "PIPE_FORMAT_R16G16B16A16_FLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, WGPUTextureFormat_RGBA16Float, 8, true },
   { "PIPE_FORMAT_R32G32B32A32_FLOAT", VK_FORMAT_R32G32B32A32_SFLOAT, WGPUTextureFormat_RGBA32Float, 16, true },
   { "PIPE_FORMAT_R32G32B32A32_UINT", VK_FORMAT_R32G32B32A32_UINT, WGPUTextureFormat_RGBA32Uint, 16, false },
};

static int
sfmt_index(const char *n)
{
   for (unsigned i = 0; i < sizeof(sfmt) / sizeof(sfmt[0]); i++)
      if (!strcmp(n, sfmt[i].n))
         return i;
   return -1;
}

/* the few formats the tests use (cs-run links no Mesa) */
static VkFormat
vk_format_of(const char *pipe)
{
   static const struct { const char *n; VkFormat f; } t[] = {
      { "PIPE_FORMAT_R32G32B32A32_FLOAT", VK_FORMAT_R32G32B32A32_SFLOAT },
      { "PIPE_FORMAT_R32G32B32A32_UINT", VK_FORMAT_R32G32B32A32_UINT },
      { "PIPE_FORMAT_R32G32B32A32_SINT", VK_FORMAT_R32G32B32A32_SINT },
      { "PIPE_FORMAT_R32_UINT", VK_FORMAT_R32_UINT },
      { "PIPE_FORMAT_R32_SINT", VK_FORMAT_R32_SINT },
      { "PIPE_FORMAT_R32_FLOAT", VK_FORMAT_R32_SFLOAT },
      { "PIPE_FORMAT_R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM },
      { "PIPE_FORMAT_R8G8B8A8_SNORM", VK_FORMAT_R8G8B8A8_SNORM },
      { "PIPE_FORMAT_R8G8B8A8_UINT", VK_FORMAT_R8G8B8A8_UINT },
      { "PIPE_FORMAT_R16G16_FLOAT", VK_FORMAT_R16G16_SFLOAT },
      { "PIPE_FORMAT_R16G16B16A16_FLOAT", VK_FORMAT_R16G16B16A16_SFLOAT },
      { "PIPE_FORMAT_R16G16B16A16_UNORM", VK_FORMAT_R16G16B16A16_UNORM },
      { "PIPE_FORMAT_R8_UNORM", VK_FORMAT_R8_UNORM },
      { "PIPE_FORMAT_R8_UINT", VK_FORMAT_R8_UINT },
      { "PIPE_FORMAT_R16_UINT", VK_FORMAT_R16_UINT },
      { "PIPE_FORMAT_R16_SINT", VK_FORMAT_R16_SINT },
      { "PIPE_FORMAT_R11G11B10_FLOAT", VK_FORMAT_B10G11R11_UFLOAT_PACK32 },
      { "PIPE_FORMAT_R10G10B10A2_UNORM", VK_FORMAT_A2B10G10R10_UNORM_PACK32 },
   };
   for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
      if (!strcmp(pipe, t[i].n))
         return t[i].f;
   return VK_FORMAT_UNDEFINED;
}

static bool pattern;
static uint32_t special_init[4096];

static void
fill_special(void)
{
   static const uint32_t special[] = {
      0, 1, 2, 3, 0xffffffff, 0x80000000, 0x7fffffff, 0xfffffffe, 31, 32, 33, 0xdeadbeef,
      0x3f800000 /* 1.0 */, 0xbf800000 /* -1.0 */, 0x40490fdb /* pi */, 0x00000000, 0x80000000 /* -0.0 */,
      0x3f000000 /* 0.5 */, 0x3fc00000 /* 1.5 */, 0x40200000 /* 2.5 */, 0xc0200000 /* -2.5 */,
      0x7f7fffff /* FLT_MAX */, 0x00800000 /* FLT_MIN */, 0x00000001 /* denormal */, 0x7f800000 /* inf */,
      0x447a0000 /* 1000.0 */, 0xc47a0000, 0x3dcccccd /* 0.1 */, 0x4b000000 /* 2^23 */, 0x4f000000 /* 2^31 */,
   };
   unsigned ns = sizeof(special) / sizeof(special[0]);
   uint32_t x = 12345;
   for (unsigned i = 0; i < 4096; i++) {
      x = x * 1664525u + 1013904223u;
      special_init[i] = i < 1024 ? special[i % ns] : x;
   }
}

static uint32_t pc_data[32];

static void
init_res(struct res *r)
{
   if (r->driver) {
      r->size = r->size < 256 ? 256 : r->size;
      r->init = calloc(r->size, 1);
      if (r->binding == 900)
         memcpy(r->init, pc_data, sizeof(pc_data));
      return;
   }
   if (r->simg >= 0) {
      unsigned n = IMG_W * IMG_H * img_layers(r), bpp = sfmt[r->simg].bpp;
      r->size = 16 + n * bpp;
      r->init = calloc(r->size, 1);
      r->init[0] = IMG_W;
      r->init[1] = IMG_H;
      r->init[2] = img_layers(r);
      for (unsigned i = 0; i < n * bpp / 4; i++) {
         uint32_t x = (i * 2654435761u) >> 8;
         if (sfmt[r->simg].fl && bpp != 8) {
            float f = (float)(x % 1000) / 250.0f - 2.0f;
            memcpy(&x, &f, 4);
         } else if (sfmt[r->simg].fl)
            x = 0x3c003800u + (x & 0x03ff03ffu);   /* two halves in [0.5, 2) */
         r->init[4 + i] = x;
      }
      return;
   }
   if (r->lowered == 1) {
      unsigned n = IMG_W * IMG_H * img_layers(r);
      r->size = 16 + n * 4;
      r->init = calloc(r->size, 1);
      r->init[0] = IMG_W;
      r->init[1] = IMG_H;
      r->init[2] = img_layers(r);
      for (unsigned i = 0; i < n; i++)
         r->init[4 + i] = (i * 2654435761u) >> 20;
      return;
   }
   if (r->lowered == 2) {
      /* values every format here takes without NaNs or overflow */
      r->size = 4096;
      r->init = calloc(r->size, 1);
      bool f32 = strstr(r->fmt, "32") && strstr(r->fmt, "FLOAT");
      bool f16 = strstr(r->fmt, "16") && strstr(r->fmt, "FLOAT");
      for (unsigned i = 0; i < r->size / 4; i++) {
         float f = (float)(i % 13) * 0.25f;
         if (f32)
            memcpy(&r->init[i], &f, 4);
         else if (f16)
            r->init[i] = (0x3c00u + (i % 7) * 0x100u) | (0x3800u + (i % 5) * 0x40u) << 16;
         else
            r->init[i] = (i * 0x01030507u) & 0x3f3f3f3fu;
      }
      return;
   }
   if (pattern) {
      r->size = r->ubo ? (r->size < 256 ? 256 : (r->size + 255) & ~255u) : 65536;
      r->init = calloc(r->size, 1);
      uint32_t x = r->set * 977 + r->binding * 131 + r->index * 17 + 1;
      for (unsigned i = 0; i < r->size / 4; i++) {
         x = x * 1664525u + 1013904223u;
         /* storage buffers: small integers (bounded loops and indices) and every fourth word a float in
          * [1, 2); uniform buffers, mostly float parameters: floats in [0.5, 2) only, so that no
          * denormals reach float math (Vulkan leaves their flushing to the implementation) */
         if (r->ubo)
            r->init[i] = ((x >> 31) ? 0x3f000000u : 0x3f800000u) + ((x >> 9) & 0x7fffff);
         else
            r->init[i] = (i & 3) == 3 ? 0x3f800000u + ((x >> 9) & 0x7fffff) : (x >> 24) & 63;
      }
      return;
   }
   r->size = r->ubo ? (r->size > 256 ? (r->size + 255) & ~255u : 256) : 16384;
   r->init = calloc(r->size, 1);
   if (r->ubo)
      for (unsigned i = 0; i < 64; i++)
         r->init[i] = i;
   else if (r->set == 0 && r->binding == 2)
      memcpy(r->init, special_init, 16384);
}

/* ------------------------------------------------------------------ Vulkan (reference) */

#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #x, r_); return -1; } } while (0)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue q;
static uint32_t mem_type = ~0u;

static int
vk_init(void)
{
   VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
   VK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 1;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n)
      return -1;
   float prio = 1;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                   .queueCount = 1, .pQueuePriorities = &prio };
   VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
   VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12 };
   vkGetPhysicalDeviceFeatures2(pd, &f2);
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci };
   VK(vkCreateDevice(pd, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((mp.memoryTypes[i].propertyFlags & 6) == 6) { /* HOST_VISIBLE | HOST_COHERENT */
         mem_type = i;
         break;
      }
   return 0;
}

static unsigned pc_size;        /* bytes of push constants / immediates the shader declares, 0 = none */
static unsigned vk_pc_size;     /* bytes pushed on Vulkan: 128 when push constants went to driver binding 900 */

static VkDescriptorType
vk_type(const struct res *r)
{
   if (r->lowered == 1 || r->simg >= 0)
      return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   if (r->lowered == 2)
      return r->storage_texel ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
   return r->ubo ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
}

static int
vk_run(const uint32_t *spv, size_t size, unsigned groups, struct res *res, unsigned nres, uint32_t **out)
{
   /* every (set, binding) the shader uses, with all its array elements */
   VkDescriptorSetLayout dsl[4] = { 0 };
   unsigned nsets = 0;
   for (unsigned i = 0; i < nres; i++)
      if (!res[i].driver && res[i].set + 1 > nsets)
         nsets = res[i].set + 1;
   VkImage img[MAX_RES * 4];
   VkImageView iview[MAX_RES * 4];
   VkBufferView bview[MAX_RES * 4];
   VkDeviceMemory imem[MAX_RES * 4];
   int img_of_buf[MAX_RES * 4];
   unsigned nimg = 0, nbview = 0;
   VkBuffer buf[MAX_RES * 4];
   VkDeviceMemory mem[MAX_RES * 4];
   void *map[MAX_RES * 4];
   unsigned nbuf = 0;
   VkDescriptorSetLayoutBinding lb[4][MAX_RES];
   unsigned nlb[4] = { 0 };
   for (unsigned i = 0; i < nres; i++) {
      if (res[i].driver)
         continue;
      bool seen = false;
      for (unsigned j = 0; j < nlb[res[i].set]; j++)
         seen = seen || lb[res[i].set][j].binding == res[i].binding;
      if (!seen)
         lb[res[i].set][nlb[res[i].set]++] = (VkDescriptorSetLayoutBinding){
            res[i].binding, vk_type(&res[i]), res[i].count, VK_SHADER_STAGE_COMPUTE_BIT };
   }
   for (unsigned s = 0; s < nsets; s++) {
      VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .bindingCount = nlb[s], .pBindings = lb[s] };
      VK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl[s]));
   }
   VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, vk_pc_size };
   VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nsets, .pSetLayouts = dsl,
                                     .pushConstantRangeCount = vk_pc_size ? 1 : 0, .pPushConstantRanges = &pcr };
   VkPipelineLayout layout;
   VK(vkCreatePipelineLayout(dev, &pl, NULL, &layout));
   VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = spv };
   VkShaderModule sm;
   VK(vkCreateShaderModule(dev, &smi, NULL, &sm));
   VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                 .module = sm, .pName = "main" }, .layout = layout };
   VkPipeline pipe;
   VK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
   VkDescriptorPoolSize ps[5] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 },
                                  { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 }, { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 64 },
                                  { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 64 } };
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 5, .pPoolSizes = ps };
   VkDescriptorPool pool;
   VK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool));
   VkDescriptorSet set[4] = { 0 };
   if (nsets) {
      VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                          .descriptorSetCount = nsets, .pSetLayouts = dsl };
      VK(vkAllocateDescriptorSets(dev, &dsa, set));
   }
   /* one buffer per element; elements the shader never touches get a scratch buffer */
   int res_buf[MAX_RES];
   for (unsigned s = 0; s < nsets; s++) {
      for (unsigned j = 0; j < nlb[s]; j++) {
         for (unsigned e = 0; e < lb[s][j].descriptorCount; e++) {
            struct res *r = NULL;
            for (unsigned i = 0; i < nres; i++)
               if (res[i].set == s && res[i].binding == lb[s][j].binding && res[i].index == e)
                  r = &res[i];
            unsigned sz = r ? r->size : 256;
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sz,
                                       .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                                VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
                                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
            VK(vkCreateBuffer(dev, &bci, NULL, &buf[nbuf]));
            VkMemoryRequirements mr;
            vkGetBufferMemoryRequirements(dev, buf[nbuf], &mr);
            VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                         .memoryTypeIndex = mem_type };
            VK(vkAllocateMemory(dev, &mai, NULL, &mem[nbuf]));
            VK(vkBindBufferMemory(dev, buf[nbuf], mem[nbuf], 0));
            VK(vkMapMemory(dev, mem[nbuf], 0, VK_WHOLE_SIZE, 0, &map[nbuf]));
            memset(map[nbuf], 0, sz);
            if (r) {
               memcpy(map[nbuf], r->init, sz);
               res_buf[r - res] = nbuf;
            }
            VkDescriptorBufferInfo bi = { buf[nbuf], 0, VK_WHOLE_SIZE };
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set[s],
                                       .dstBinding = lb[s][j].binding, .dstArrayElement = e,
                                       .descriptorCount = 1, .descriptorType = lb[s][j].descriptorType,
                                       .pBufferInfo = &bi };
            VkDescriptorImageInfo ii = { 0 };
            img_of_buf[nbuf] = -1;
            VkDescriptorType dt = lb[s][j].descriptorType;
            if (dt == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || dt == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
               VkBufferViewCreateInfo bv = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = buf[nbuf],
                                             .format = r ? vk_format_of(r->fmt) : VK_FORMAT_R32_UINT,
                                             .range = VK_WHOLE_SIZE };
               VK(vkCreateBufferView(dev, &bv, NULL, &bview[nbview]));
               w.pBufferInfo = NULL;
               w.pTexelBufferView = &bview[nbview++];
            } else if (dt == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
               /* the image; this buffer is its staging copy (header + texels) */
               unsigned layers = r ? img_layers(r) : 1;
               VkFormat f = r && r->simg >= 0 ? sfmt[r->simg].vk :
                            r && strstr(r->fmt, "SINT") ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
               VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                        .extent = { IMG_W, IMG_H, 1 }, .mipLevels = 1, .arrayLayers = layers,
                                        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT };
               VK(vkCreateImage(dev, &ic, NULL, &img[nimg]));
               VkMemoryRequirements ir;
               vkGetImageMemoryRequirements(dev, img[nimg], &ir);
               VkMemoryAllocateInfo ia = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = ir.size,
                                           .memoryTypeIndex = __builtin_ctz(ir.memoryTypeBits) };
               VK(vkAllocateMemory(dev, &ia, NULL, &imem[nimg]));
               VK(vkBindImageMemory(dev, img[nimg], imem[nimg], 0));
               VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img[nimg],
                                            .viewType = r && r->arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                                            .format = f, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
               VK(vkCreateImageView(dev, &vc, NULL, &iview[nimg]));
               ii = (VkDescriptorImageInfo){ .imageView = iview[nimg], .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
               w.pBufferInfo = NULL;
               w.pImageInfo = &ii;
               img_of_buf[nbuf] = nimg++;
            }
            vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
            nbuf++;
         }
      }
   }
   VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
   VkCommandPool cp;
   VK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
   VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cb;
   VK(vkAllocateCommandBuffers(dev, &cba, &cb));
   VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   vkBeginCommandBuffer(cb, &cbi);
   for (unsigned i = 0; i < nbuf; i++) {
      if (img_of_buf[i] < 0)
         continue;
      VkImage im = img[img_of_buf[i]];
      unsigned layers = 1;
      for (unsigned k = 0; k < nres; k++)
         if (!res[k].driver && res_buf[k] == (int)i)
            layers = img_layers(&res[k]);
      VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .image = im, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
      VkBufferImageCopy bic = { .bufferOffset = 16, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers },
                                .imageExtent = { IMG_W, IMG_H, 1 } };
      vkCmdCopyBufferToImage(cb, buf[i], im, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
   }
   VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   if (nsets)
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, nsets, set, 0, NULL);
   if (vk_pc_size)
      vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, vk_pc_size, pc_data);
   vkCmdDispatch(cb, groups, 1, 1);
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   for (unsigned i = 0; i < nbuf; i++) {
      if (img_of_buf[i] < 0)
         continue;
      unsigned layers = 1;
      for (unsigned k = 0; k < nres; k++)
         if (!res[k].driver && res_buf[k] == (int)i)
            layers = img_layers(&res[k]);
      VkBufferImageCopy bic = { .bufferOffset = 16, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers },
                                .imageExtent = { IMG_W, IMG_H, 1 } };
      vkCmdCopyImageToBuffer(cb, img[img_of_buf[i]], VK_IMAGE_LAYOUT_GENERAL, buf[i], 1, &bic);
   }
   vkEndCommandBuffer(cb);
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   VK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   vkQueueWaitIdle(q);
   for (unsigned i = 0; i < nres; i++)
      if (!res[i].driver)
         memcpy(out[i], map[res_buf[i]], res[i].size);
   for (unsigned i = 0; i < nimg; i++) {
      vkDestroyImageView(dev, iview[i], NULL);
      vkDestroyImage(dev, img[i], NULL);
      vkFreeMemory(dev, imem[i], NULL);
   }
   for (unsigned i = 0; i < nbview; i++)
      vkDestroyBufferView(dev, bview[i], NULL);
   vkDestroyCommandPool(dev, cp, NULL);
   vkDestroyDescriptorPool(dev, pool, NULL);
   vkDestroyPipeline(dev, pipe, NULL);
   vkDestroyShaderModule(dev, sm, NULL);
   vkDestroyPipelineLayout(dev, layout, NULL);
   for (unsigned s = 0; s < nsets; s++)
      vkDestroyDescriptorSetLayout(dev, dsl[s], NULL);
   for (unsigned i = 0; i < nbuf; i++) {
      vkDestroyBuffer(dev, buf[i], NULL);
      vkFreeMemory(dev, mem[i], NULL);
   }
   return 0;
}

/* ------------------------------------------------------------------ Dawn (WGSL) */

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
   WGPUFeatureName want[] = { WGPUFeatureName_ShaderF16, WGPUFeatureName_Subgroups };
   WGPUFeatureName have[2];
   unsigned n = 0;
   for (unsigned i = 0; i < 2; i++)
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

static int
wgpu_run(const char *code, unsigned groups, struct res *res, unsigned nres, uint32_t **out)
{
   werror = 0;
   WGPUShaderSourceWGSL wgsl = { .chain = { .sType = WGPUSType_ShaderSourceWGSL }, .code = sv(code) };
   WGPUShaderModuleDescriptor smd = { .nextInChain = &wgsl.chain };
   WGPUShaderModule sm = wgpuDeviceCreateShaderModule(wdev, &smd);
   WGPUComputePipelineDescriptor cpd = { .compute = { .module = sm, .entryPoint = sv("main") } };
   WGPUComputePipeline cp = wgpuDeviceCreateComputePipeline(wdev, &cpd);
   wgpuInstanceProcessEvents(winst);
   if (werror) {
      fprintf(stderr, "dawn: %s\n", werrmsg);
      return -1;
   }
   WGPUBuffer buf[MAX_RES], rb[MAX_RES];
   WGPUTexture tex[MAX_RES];
   WGPUQueue queue = wgpuDeviceGetQueue(wdev);
   for (unsigned i = 0; i < nres; i++) {
      buf[i] = rb[i] = NULL;
      tex[i] = NULL;
      if (res[i].heap_slot >= 0)
         continue;
      if (res[i].simg >= 0) {
         unsigned layers = img_layers(&res[i]), bpp = sfmt[res[i].simg].bpp;
         WGPUTextureDescriptor td = { .usage = WGPUTextureUsage_StorageBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc,
                                      .dimension = WGPUTextureDimension_2D, .size = { IMG_W, IMG_H, layers },
                                      .format = sfmt[res[i].simg].wg, .mipLevelCount = 1, .sampleCount = 1 };
         tex[i] = wgpuDeviceCreateTexture(wdev, &td);
         WGPUTexelCopyTextureInfo dst = { .texture = tex[i] };
         WGPUTexelCopyBufferLayout lay = { .bytesPerRow = IMG_W * bpp, .rowsPerImage = IMG_H };
         WGPUExtent3D ext = { IMG_W, IMG_H, layers };
         wgpuQueueWriteTexture(queue, &dst, res[i].init + 4, res[i].size - 16, &lay, &ext);
         continue;
      }
      WGPUBufferDescriptor bd = { .size = res[i].size,
                                  .usage = (res[i].ubo ? WGPUBufferUsage_Uniform : WGPUBufferUsage_Storage) |
                                           WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst };
      buf[i] = wgpuDeviceCreateBuffer(wdev, &bd);
      wgpuQueueWriteBuffer(queue, buf[i], 0, res[i].init, res[i].size);
   }
   WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(wdev, NULL);
   WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, NULL);
   wgpuComputePassEncoderSetPipeline(pass, cp);
   for (unsigned g = 0; g < 4; g++) {
      WGPUBindGroupEntry be[MAX_RES];
      unsigned nbe = 0;
      for (unsigned i = 0; i < nres; i++)
         if (res[i].group == g && res[i].heap_slot < 0) {
            if (tex[i]) {
               WGPUTextureViewDescriptor vd = { .format = sfmt[res[i].simg].wg,
                                                .dimension = res[i].arrayed ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D,
                                                .mipLevelCount = 1, .arrayLayerCount = img_layers(&res[i]),
                                                .aspect = WGPUTextureAspect_All, .usage = WGPUTextureUsage_StorageBinding };
               be[nbe++] = (WGPUBindGroupEntry){ .binding = res[i].wbinding, .textureView = wgpuTextureCreateView(tex[i], &vd) };
            } else
               be[nbe++] = (WGPUBindGroupEntry){ .binding = res[i].wbinding, .buffer = buf[i], .size = res[i].size };
         }
      if (!nbe)
         continue;
      WGPUBindGroupDescriptor bgd = { .layout = wgpuComputePipelineGetBindGroupLayout(cp, g), .entryCount = nbe, .entries = be };
      wgpuComputePassEncoderSetBindGroup(pass, g, wgpuDeviceCreateBindGroup(wdev, &bgd), 0, NULL);
   }
   if (pc_size)
      wgpuComputePassEncoderSetImmediates(pass, 0, pc_data, pc_size);
   wgpuComputePassEncoderDispatchWorkgroups(pass, groups, 1, 1);
   wgpuComputePassEncoderEnd(pass);
   for (unsigned i = 0; i < nres; i++) {
      if (res[i].heap_slot >= 0)
         continue;
      WGPUBufferDescriptor rd = { .size = res[i].size, .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst };
      rb[i] = wgpuDeviceCreateBuffer(wdev, &rd);
      if (tex[i]) {
         /* texels after the 16-byte header, as on the Vulkan side */
         unsigned bpp = sfmt[res[i].simg].bpp;
         WGPUTexelCopyTextureInfo src = { .texture = tex[i] };
         WGPUBufferDescriptor sd = { .size = res[i].size - 16, .usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst };
         WGPUBuffer stage = wgpuDeviceCreateBuffer(wdev, &sd);
         WGPUTexelCopyBufferInfo db = { .layout = { .bytesPerRow = IMG_W * bpp, .rowsPerImage = IMG_H }, .buffer = stage };
         WGPUExtent3D ext = { IMG_W, IMG_H, img_layers(&res[i]) };
         wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &db, &ext);
         WGPUBufferDescriptor hd = { .size = 16, .usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst };
         WGPUBuffer hb = wgpuDeviceCreateBuffer(wdev, &hd);
         wgpuQueueWriteBuffer(queue, hb, 0, res[i].init, 16);
         wgpuCommandEncoderCopyBufferToBuffer(enc, hb, 0, rb[i], 0, 16);
         wgpuCommandEncoderCopyBufferToBuffer(enc, stage, 0, rb[i], 16, res[i].size - 16);
         continue;
      }
      wgpuCommandEncoderCopyBufferToBuffer(enc, buf[i], 0, rb[i], 0, res[i].size);
   }
   WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
   wgpuQueueSubmit(queue, 1, &cmd);
   int rc = 0;
   for (unsigned i = 0; i < nres; i++) {
      if (res[i].heap_slot >= 0)
         continue;
      wmapped = 0;
      wgpuBufferMapAsync(rb[i], WGPUMapMode_Read, 0, res[i].size, (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map });
      for (time_t t0 = time(NULL); !wmapped && time(NULL) - t0 < 300;)
         wgpuInstanceProcessEvents(winst);
      if (wmapped != 1) {
         fprintf(stderr, "dawn: map failed %s\n", werrmsg);
         rc = -1;
         break;
      }
      memcpy(out[i], wgpuBufferGetConstMappedRange(rb[i], 0, res[i].size), res[i].size);
      wgpuBufferUnmap(rb[i]);
   }
   for (unsigned i = 0; i < nres; i++) {
      if (res[i].heap_slot >= 0)
         continue;
      wgpuBufferRelease(rb[i]);
      if (buf[i])
         wgpuBufferRelease(buf[i]);
   }
   if (werror) {
      fprintf(stderr, "dawn: %s\n", werrmsg);
      return -1;
   }
   return rc;
}

/* ------------------------------------------------------------------ main */

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

static bool
same(uint32_t a, uint32_t b)
{
   if (a == b)
      return true;
   float fa, fb;
   memcpy(&fa, &a, 4);
   memcpy(&fb, &b, 4);
   return isnan(fa) && isnan(fb);
}

int
main(int argc, char **argv)
{
   unsigned groups = 4;
   int a = 1;
   for (; a < argc && argv[a][0] == '-'; a++) {
      if (!strcmp(argv[a], "-groups") && a + 1 < argc)
         groups = atoi(argv[++a]);
      else if (!strcmp(argv[a], "-pattern"))
         pattern = true;
   }
   fill_special();
   if (vk_init() || wgpu_init()) {
      fprintf(stderr, "no Vulkan or Dawn device\n");
      return 1;
   }
   int pass = 0, fail_n = 0, skip = 0;
   for (; a < argc; a++) {
      const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
      char stem[1024], path[1100];
      snprintf(stem, sizeof(stem), "%.*s", (int)(strlen(argv[a]) - 4), argv[a]);
      long sz;
      uint32_t *w = slurp(argv[a], &sz);
      snprintf(path, sizeof(path), "%s.wgsl", stem);
      char *wgsl = slurp(path, NULL);
      snprintf(path, sizeof(path), "%s.bind", stem);
      char *bindtxt = slurp(path, NULL);
      if (!w || !wgsl) {
         snprintf(path, sizeof(path), "%s.err", stem);
         char *err = slurp(path, NULL);
         printf("FAIL %s: %s", name, err ? err : "no spv/wgsl\n");
         fail_n++;
         continue;
      }
      if (!strstr(wgsl, "@compute")) {
         printf("skip %s: not compute\n", name);
         skip++;
         continue;
      }
      /* immediates: struct PushConstants { m0: vec4<u32>, ... } */
      pc_size = 0;
      const char *pcs = strstr(wgsl, "struct PushConstants {"), *pce = pcs ? strstr(pcs, "};") : NULL;
      for (const char *m = pcs; m && (m = strstr(m + 1, ": vec4<u32>,")) && m < pce;)
         pc_size += 16;
      for (unsigned i = 0; i < 32; i++)
         pc_data[i] = 100 + i * 7;
      struct res res[MAX_RES];
      unsigned nres = 0;
      bool other = false;
      for (char *l = bindtxt; l && *l;) {
         unsigned g, bi, k, vs, vb, vi, vn, ms;
         char *mp = strstr(l, "min_size ");
         unsigned lk, st, ar;
         char fmt[64];
         if (sscanf(l, "group %u binding %u kind %u vk %u.%u[%u/%u]", &g, &bi, &k, &vs, &vb, &vi, &vn) == 7 &&
             nres < MAX_RES && (vs < 4 || vs == 255)) {
            if (k > 2 && k != 4)
               other = true;
            ms = mp ? strtoul(mp + 9, NULL, 10) : 256;
            char *dp = strstr(l, " dim "), *ap = strstr(l, " array ");
            res[nres++] = (struct res){ .group = g, .wbinding = bi, .set = vs, .binding = vb, .index = vi,
                                        .count = vn ? vn : 1, .ubo = k == 0, .size = ms, .kind = k,
                                        .driver = vs == 255, .dim = dp ? atoi(dp + 5) : 0,
                                        .arrayed = ap ? atoi(ap + 7) : 0, .heap_slot = -1, .simg = -1 };
         } else if (sscanf(l, "heap slot %u vk %u.%u[%u]", &lk, &vs, &vb, &vi) == 4 && nres < MAX_RES) {
            res[nres++] = (struct res){ .set = vs, .binding = vb, .index = vi, .count = 1, .kind = 1,
                                        .size = 4, .heap_slot = lk, .simg = -1 };
         } else if (nres && sscanf(l, " storage_format %63s", fmt) == 1) {
            res[nres - 1].simg = sfmt_index(fmt);
            if (res[nres - 1].simg < 0)
               other = true;
         } else if (nres && sscanf(l, " lowered %u format %63s storage %u arrayed %u", &lk, fmt, &st, &ar) == 4) {
            res[nres - 1].lowered = lk;
            res[nres - 1].storage_texel = st;
            res[nres - 1].arrayed = ar;
            snprintf(res[nres - 1].fmt, sizeof(res[nres - 1].fmt), "%s", fmt);
         }
         l = strchr(l, '\n');
         if (l)
            l++;
      }
      if (other) {
         printf("skip %s: textures or images\n", name);
         skip++;
         continue;
      }
      for (unsigned i = 0; i < nres; i++)
         for (unsigned j = 0; j < nres; j++)
            if (res[j].set == res[i].set && res[j].binding == res[i].binding && !res[i].driver && res[i].index + 1 > res[j].count)
               res[j].count = res[i].index + 1;
      vk_pc_size = pc_size;
      for (unsigned i = 0; i < nres; i++) {
         if (res[i].driver && res[i].binding == 900)
            vk_pc_size = 128;
         init_res(&res[i]);
      }
      if (pc_size && vk_pc_size < 64)
         vk_pc_size = 64;
      /* the heap: arena (910) = the heaped buffers one after another, table (902) = where each is */
      for (unsigned i = 0; i < nres; i++) {
         if (!res[i].driver || (res[i].binding != 910 && res[i].binding != 902))
            continue;
         unsigned nh = 0;
         for (unsigned j = 0; j < nres; j++)
            if (res[j].heap_slot >= 0 && (unsigned)res[j].heap_slot + 1 > nh)
               nh = res[j].heap_slot + 1;
         if (res[i].binding == 910) {
            free(res[i].init);
            res[i].size = nh * HEAP_STRIDE;
            res[i].init = calloc(res[i].size, 1);
            for (unsigned j = 0; j < nres; j++)
               if (res[j].heap_slot >= 0)
                  memcpy((char *)res[i].init + res[j].heap_slot * HEAP_STRIDE, res[j].init,
                         res[j].size < HEAP_STRIDE ? res[j].size : HEAP_STRIDE);
         } else {
            for (unsigned j = 0; j < nres; j++)
               if (res[j].heap_slot >= 0) {
                  uint32_t *e = &res[i].init[4 * res[j].heap_slot];
                  e[0] = 0;
                  e[1] = res[j].heap_slot * HEAP_STRIDE;
                  e[2] = res[j].size < HEAP_STRIDE ? res[j].size : HEAP_STRIDE;
               }
         }
      }
      uint32_t *ref[MAX_RES], *ref2[MAX_RES], *ref3[MAX_RES], *got[MAX_RES];
      for (unsigned i = 0; i < nres; i++) {
         ref[i] = calloc(res[i].size, 1);
         ref2[i] = calloc(res[i].size, 1);
         ref3[i] = calloc(res[i].size, 1);
         got[i] = malloc(res[i].size);
         memset(got[i], 0xcd, res[i].size);
      }
      /* three reference runs: a word that differs between any two is racy */
      if (vk_run(w, sz, groups, res, nres, ref) || vk_run(w, sz, groups, res, nres, ref2)) {
         printf("FAIL %s: vulkan\n", name);
         fail_n++;
         continue;
      }
      if (vk_run(w, sz, groups, res, nres, ref3)) {
         printf("FAIL %s: vulkan\n", name);
         fail_n++;
         continue;
      }
      if (wgpu_run(wgsl, groups, res, nres, got)) {
         printf("FAIL %s: dawn\n", name);
         fail_n++;
         continue;
      }
      for (unsigned i = 0; i < nres; i++) {
         if (res[i].heap_slot < 0)
            continue;
         for (unsigned j = 0; j < nres; j++)
            if (res[j].set == 255 && res[j].binding == 910)
               memcpy(got[i], (char *)got[j] + res[i].heap_slot * HEAP_STRIDE,
                      res[i].size < HEAP_STRIDE ? res[i].size : HEAP_STRIDE);
      }
      unsigned diff = 0, nonzero = 0, changed = 0, racy = 0;
      for (unsigned b = 0; b < nres; b++) {
         if (res[b].ubo || res[b].driver)
            continue;
         for (unsigned i = 0; i < res[b].size / 4; i++) {
            if (!same(ref[b][i], ref2[b][i]) || !same(ref[b][i], ref3[b][i])) {
               racy++;
               continue;
            }
            if (ref[b][i])
               nonzero++;
            if (ref[b][i] != res[b].init[i])
               changed++;
            if (!same(ref[b][i], got[b][i])) {
               if (diff < 8)
                  printf("    set %u binding %u[%u] word %u: vulkan 0x%08x wgsl 0x%08x (was 0x%08x)\n", res[b].set,
                         res[b].binding, res[b].index, i, ref[b][i], got[b][i], res[b].init[i]);
               diff++;
            }
         }
      }
      printf("%s %s (%u words differ, %u written by the shader, %u nonzero, %u racy)\n", diff ? "FAIL" : "ok", name,
             diff, changed, nonzero, racy);
      diff ? fail_n++ : pass++;
      for (unsigned i = 0; i < nres; i++) {
         free(ref[i]);
         free(ref2[i]);
         free(ref3[i]);
         free(got[i]);
         free(res[i].init);
      }
      free(w);
      free(wgsl);
      free(bindtxt);
   }
   printf("cs-run: %d ok, %d failed, %d skipped\n", pass, fail_n, skip);
   return fail_n != 0;
}
