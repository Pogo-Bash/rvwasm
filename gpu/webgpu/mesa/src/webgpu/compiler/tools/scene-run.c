/*
 * scene-run: T3b's graphics runtime check. One vertex + fragment pair and its draw (tools/scene.h) rendered
 *   (a) as SPIR-V on Vulkan (lavapipe), the reference: the real vertex input (divisors, any format), point
 *       lists with gl_PointSize, indexed draws with a base vertex and first instance, push constants;
 *   (b) as the WGSL that wgsl-corpus -layout -legalize -link -scene made, on Dawn native, with what the
 *       passes asked for: pulled vertex buffers as storage (driver bindings 920 + b), the index buffer of an
 *       expanded point draw (936), draw parameters in the immediates (or driver uniforms 901), points drawn
 *       as 6 vertices each, push constants as immediates (or driver binding 900).
 * and compares the 64x64 rgba8unorm images.
 *
 *   scene-run DIR/NAME...   reads DIR/NAME.scene, DIR/NAME-{vert,frag}.{spv,wgsl,bind}
 *
 * Resources come from the two .bind tables (vk set < 4): uniform buffers (256 bytes or the declared size,
 * word i = the float i / 64), storage buffers (4 KiB of small integers), textures 16x16 with 2 levels (3
 * layers when arrayed; rgba8unorm for float, rgba32ui/si for integer; element e of an array has its own
 * pattern), samplers (clamp to edge; nearest unless the scene says linear). A texture with a
 * "sampler-of-combined" line is a combined image sampler on Vulkan.
 *
 * With a geometry shader (DIR/NAME-geom.{spv,wgsl,gs} and the libpoly parts NAME-geom.gs-PART.wgsl from
 * wgsl-corpus), the WebGPU side does what T2 will: libpoly's params in a poly buffer (poly/geometry.h),
 * the vertex shader as compute writing its outputs there, the GS main part writing the index buffer, the
 * pre part, then the rast part drawn indirectly from the params' draw command.
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
#include "scene.h"
#include "util/u_math.h"
#include "poly/prim.h"
#include "poly/geometry.h"
#include "wgvk_poly_args.h"
#include "poly/tessellator.h"

#define W 64
#define H 64
#define TW 16
#define MAX_RES 48
#define BUF_WORDS 16384     /* scene buffers: 64 KiB */
#define ATLAS_W 64
#define ATLAS_H 64
#define ATLAS_LAYERS 4

struct res {
   unsigned group, wbinding, kind;        /* WebGPU side; kind: wgsl_binding_kind */
   unsigned set, binding, index, count;   /* Vulkan side */
   bool combined, sampler_of_combined, arrayed;
   unsigned sample;                       /* 0 float, 1 sint, 2 uint */
   unsigned size;                         /* buffers */
   unsigned lowered;                      /* wgvk_lowered_kind (1: an image used atomically) */
};

struct prog {
   struct res res[MAX_RES];
   unsigned nres;
   unsigned draw_params_offset, dp_in_ubo, pc_in_ubo, pulled, points_expanded, imm_bytes;
};

static struct scene sc;
static void *xfb_vkmap[4];
static uint64_t vk_query[2];          /* transform feedback query of stream 0: written, needed */
static bool query_done;
static WGPUBuffer xfb_wg[4];
static uint32_t *gs_spv;
static long gs_size;
static uint32_t *tcs_spv, *tes_spv;
static long tcs_size, tes_size;
struct gsinfo { unsigned shape, mode, max_indices, input, count_words, prefix_sum, parts[4]; uint32_t topology[64]; };
static struct gsinfo gsi;
struct prog;
static struct prog *pgs;      /* the GS rast part's bindings (its app bindings are the GS's) */
static uint32_t ubo_init[1024];
static uint32_t ssbo_init[1024];
static uint8_t vb_data[8][SCENE_VB_SIZE];
static uint32_t idx_data[256];
static uint32_t push_data[32];

/* a uniform buffer the scene gives words for */
static void
scene_ubo_fill(bool ubo, unsigned set, unsigned binding, unsigned elem, uint32_t *data)
{
   (void)ubo;
   const struct scene_ubo *u = scene_ubo(&sc, set, binding, elem);
   if (!u)
      return;
   memset(data, 0, BUF_WORDS * 4);
   memcpy(data, u->w, u->n * 4);
}

/* the draw command of an indirect draw: VkDraw[Indexed]IndirectCommand */
static void
scene_indirect_args(uint32_t *a)
{
   if (sc.indexed) {
      uint32_t c[5] = { sc.count, sc.instances, sc.first, sc.base_vertex, sc.first_instance };
      memcpy(a, c, 20);
   } else {
      uint32_t c[5] = { sc.count, sc.instances, sc.first, sc.first_instance, 0 };
      memcpy(a, c, 20);
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

static void
fill(void)
{
   for (unsigned i = 0; i < 1024; i++) {
      float f = (i % 64) / 64.0f;
      memcpy(&ubo_init[i], &f, 4);
      ssbo_init[i] = (i * 2654435761u) >> 26;
   }
   for (unsigned b = 0; b < 8; b++)
      scene_vb_data(&sc, b, vb_data[b]);
   for (unsigned i = 0; i < 256; i++)
      idx_data[i] = scene_index(i);
   for (unsigned i = 0; i < 32; i++)
      push_data[i] = sc.push_set & (1u << i) ? sc.push_w[i] : scene_push_word(i);
}

/* texel (x, y) of level l, layer z, element e: rgba8 for float textures, 4 x u32 for integer ones */
static void
texel(const struct res *r, unsigned e, unsigned l, unsigned z, unsigned x, unsigned y, uint32_t *out)
{
   if (r->sample == 0) {
      uint8_t p[4] = { (uint8_t)(x * 16 + e * 40), (uint8_t)(y * 16 + l * 100), (uint8_t)(((x ^ y) & 1) ? 255 - z * 60 : z * 60),
                       (uint8_t)(255 - x * 8 - e * 20) };
      memcpy(out, p, 4);
   } else {
      for (unsigned c = 0; c < 4; c++)
         out[c] = x + 16 * y + 256 * z + 1024 * e + 4096 * l + 16384 * c;
   }
}

static bool
parse_bind(const char *txt, struct prog *p)
{
   memset(p, 0, sizeof(*p));
   p->draw_params_offset = 0xffff;
   for (const char *l = txt; l && *l;) {
      unsigned g, bi, k, vs, vb, vi, vn, arr = 0, smp = 0, ms;
      if (sscanf(l, "group %u binding %u kind %u vk %u.%u[%u/%u]", &g, &bi, &k, &vs, &vb, &vi, &vn) == 7 &&
          p->nres < MAX_RES) {
         const char *a = strstr(l, " array "), *s = strstr(l, " sample "), *m = strstr(l, "min_size ");
         if (a)
            arr = atoi(a + 7);
         if (s)
            smp = atoi(s + 8);
         ms = m ? atoi(m + 9) : 256;
         p->res[p->nres++] = (struct res){ .group = g, .wbinding = bi, .kind = k, .set = vs, .binding = vb,
                                           .index = vi, .count = vn ? vn : 1, .arrayed = arr,
                                           .sampler_of_combined = strstr(l, "sampler-of-combined") && strstr(l, "sampler-of-combined") < strchr(l, '\n'),
                                           .sample = smp == 1 ? 1 : smp == 2 ? 2 : 0, .size = ms };
      } else if (p->nres && strncmp(l, "    lowered ", 12) == 0) {
         p->res[p->nres - 1].lowered = atoi(l + 12);
      } else if (!strncmp(l, "legal ", 6)) {
         sscanf(strstr(l, "draw_params_offset"), "draw_params_offset %u draw_params_in_ubo %u push_constants_in_ubo %u",
                &p->draw_params_offset, &p->dp_in_ubo, &p->pc_in_ubo);
         const char *q = strstr(l, "pulled ");
         if (q)
            p->pulled = strtoul(q + 7, NULL, 0);
         q = strstr(l, "points_expanded ");
         if (q)
            p->points_expanded = atoi(q + 16);
      }
      l = strchr(l, '\n');
      if (l)
         l++;
   }
   return true;
}

/* textures that are the image half of a combined image sampler */
static void
mark_combined(struct prog *p)
{
   for (unsigned i = 0; i < p->nres; i++)
      for (unsigned j = 0; j < p->nres; j++)
         if (p->res[j].sampler_of_combined && p->res[i].kind == 3 && p->res[j].set == p->res[i].set &&
             p->res[j].binding == p->res[i].binding)
            p->res[i].combined = true;
}

/* ------------------------------------------------------------------ format tables */

static const struct { const char *name; VkFormat vk; WGPUVertexFormat wg; } formats[] = {
   { "R32G32B32A32_FLOAT", VK_FORMAT_R32G32B32A32_SFLOAT, WGPUVertexFormat_Float32x4 },
   { "R32G32B32_FLOAT", VK_FORMAT_R32G32B32_SFLOAT, WGPUVertexFormat_Float32x3 },
   { "R32G32_FLOAT", VK_FORMAT_R32G32_SFLOAT, WGPUVertexFormat_Float32x2 },
   { "R32_FLOAT", VK_FORMAT_R32_SFLOAT, WGPUVertexFormat_Float32 },
   { "R32_UINT", VK_FORMAT_R32_UINT, WGPUVertexFormat_Uint32 },
   { "R32G32B32A32_UINT", VK_FORMAT_R32G32B32A32_UINT, WGPUVertexFormat_Uint32x4 },
   { "R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, WGPUVertexFormat_Unorm8x4 },
   { "R8G8B8A8_UINT", VK_FORMAT_R8G8B8A8_UINT, WGPUVertexFormat_Uint8x4 },
   { "R8G8B8A8_SNORM", VK_FORMAT_R8G8B8A8_SNORM, WGPUVertexFormat_Snorm8x4 },
   { "R16G16_SINT", VK_FORMAT_R16G16_SINT, WGPUVertexFormat_Sint16x2 },
   { "R16G16B16A16_UNORM", VK_FORMAT_R16G16B16A16_UNORM, WGPUVertexFormat_Unorm16x4 },
   { "R10G10B10A2_UNORM", VK_FORMAT_A2B10G10R10_UNORM_PACK32, WGPUVertexFormat_Unorm10_10_10_2 },
   { "B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM, WGPUVertexFormat_Unorm8x4BGRA },
   /* none of these exist as WebGPU vertex formats */
   { "R8G8B8_UNORM", VK_FORMAT_R8G8B8_UNORM, 0 },
   { "R8G8B8_SNORM", VK_FORMAT_R8G8B8_SNORM, 0 },
   { "R8G8B8_UINT", VK_FORMAT_R8G8B8_UINT, 0 },
   { "R16G16B16_SNORM", VK_FORMAT_R16G16B16_SNORM, 0 },
   { "R16G16B16_UNORM", VK_FORMAT_R16G16B16_UNORM, 0 },
   { "R16G16B16_SINT", VK_FORMAT_R16G16B16_SINT, 0 },
   { "R8G8B8A8_USCALED", VK_FORMAT_R8G8B8A8_USCALED, 0 },
   { "R16G16_SSCALED", VK_FORMAT_R16G16_SSCALED, 0 },
   { "R10G10B10A2_SNORM", VK_FORMAT_A2B10G10R10_SNORM_PACK32, 0 },
   { "B8G8R8_UNORM", VK_FORMAT_B8G8R8_UNORM, 0 },
};

static int
format_index(const char *name)
{
   for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
      if (!strcmp(name, formats[i].name))
         return i;
   fprintf(stderr, "scene-run: no format %s\n", name);
   return -1;
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
   VkPhysicalDeviceTransformFeedbackFeaturesEXT tf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,
                                                       .transformFeedback = VK_TRUE, .geometryStreams = VK_TRUE };
   VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .pNext = &tf,
      .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_TRUE };
   VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &div,
                                            .shaderDrawParameters = VK_TRUE, .multiview = VK_TRUE };
   VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f11,
                                            .shaderOutputViewportIndex = VK_TRUE, .shaderOutputLayer = VK_TRUE };
   VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &f12,
                                            .dynamicRendering = VK_TRUE };
   VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
   f2.features.largePoints = VK_TRUE;
   f2.features.multiViewport = VK_TRUE;
   f2.features.geometryShader = VK_TRUE;
   f2.features.tessellationShader = VK_TRUE;
   f2.features.vertexPipelineStoresAndAtomics = VK_TRUE;
   f2.features.shaderClipDistance = VK_TRUE;
   const char *ext[] = { "VK_EXT_vertex_attribute_divisor", "VK_EXT_transform_feedback" };
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 2, .ppEnabledExtensionNames = ext };
   VK(vkCreateDevice(pd, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   return 0;
}

static int
vk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void *init, VkBuffer *b, void **map)
{
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
   VK(vkCreateBuffer(dev, &bci, NULL, b));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, *b, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(mr.memoryTypeBits, 6) };
   VkDeviceMemory m;
   VK(vkAllocateMemory(dev, &mai, NULL, &m));
   VK(vkBindBufferMemory(dev, *b, m, 0));
   void *p;
   VK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, &p));
   if (init)
      memcpy(p, init, size);
   else
      memset(p, 0, size);
   if (map)
      *map = p;
   return 0;
}

static void
barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, unsigned levels, unsigned layers)
{
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers } };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* An image used atomically (EEVEE's shadow atlas): ATLAS_W x ATLAS_H x ATLAS_LAYERS r32ui, every texel
 * 0xffffffff to start with (atomicMin shows), compared after the draw. Vulkan: a storage image; WebGPU:
 * the lowered buffer (16-byte header + texels). */
static VkImage atlas_img;
static VkBuffer atlas_vkbuf;
static void *atlas_vkmap;
static WGPUBuffer atlas_wg;
static bool atlas_used;
static unsigned atlas_bytes(void) { return 16 + ATLAS_W * ATLAS_H * ATLAS_LAYERS * 4; }
static void
atlas_init(uint32_t *w)
{
   w[0] = ATLAS_W;
   w[1] = ATLAS_H;
   w[2] = ATLAS_LAYERS;
   w[3] = 0;
   for (unsigned i = 0; i < ATLAS_W * ATLAS_H * ATLAS_LAYERS; i++)
      w[4 + i] = 0xffffffffu;
}
static WGPUBuffer atlas_wbuf(WGPUQueue q);

static int
vk_atlas(VkCommandBuffer cmd, VkImageView *view)
{
   VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32_UINT,
                            .extent = { ATLAS_W, ATLAS_H, 1 }, .mipLevels = 1, .arrayLayers = ATLAS_LAYERS,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
   VK(vkCreateImage(dev, &ic, NULL, &atlas_img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, atlas_img, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) };
   VkDeviceMemory m;
   VK(vkAllocateMemory(dev, &mai, NULL, &m));
   VK(vkBindImageMemory(dev, atlas_img, m, 0));
   static uint32_t init[16 / 4 + ATLAS_W * ATLAS_H * ATLAS_LAYERS];
   atlas_init(init);
   if (vk_buffer(atlas_bytes(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, init, &atlas_vkbuf, &atlas_vkmap))
      return -1;
   barrier(cmd, atlas_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 1, ATLAS_LAYERS);
   VkBufferImageCopy c = { .bufferOffset = 16, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, ATLAS_LAYERS },
                           .imageExtent = { ATLAS_W, ATLAS_H, 1 } };
   vkCmdCopyBufferToImage(cmd, atlas_vkbuf, atlas_img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
   barrier(cmd, atlas_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 1, ATLAS_LAYERS);
   VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = atlas_img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, .format = VK_FORMAT_R32_UINT,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ATLAS_LAYERS } };
   VK(vkCreateImageView(dev, &vc, NULL, view));
   atlas_used = true;
   return 0;
}

/* a texture for res r, element e, filled on cmd; returns its view */
static int
vk_texture(VkCommandBuffer cmd, const struct res *r, unsigned e, VkImageView *view)
{
   unsigned layers = r->arrayed ? 3 : 1;
   VkFormat f = r->sample == 0 ? VK_FORMAT_R8G8B8A8_UNORM : r->sample == 1 ? VK_FORMAT_R32G32B32A32_SINT :
                VK_FORMAT_R32G32B32A32_UINT;
   unsigned bpp = r->sample == 0 ? 4 : 16;
   VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = { TW, TW, 1 }, .mipLevels = 2, .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
   VkImage img;
   VK(vkCreateImage(dev, &ic, NULL, &img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) };
   VkDeviceMemory m;
   VK(vkAllocateMemory(dev, &mai, NULL, &m));
   VK(vkBindImageMemory(dev, img, m, 0));
   /* staging: level 0 then level 1, each all layers */
   unsigned sz0 = TW * TW * bpp * layers, sz1 = (TW / 2) * (TW / 2) * bpp * layers;
   uint8_t *data = malloc(sz0 + sz1);
   for (unsigned l = 0; l < 2; l++) {
      unsigned w = TW >> l;
      uint8_t *base = data + (l ? sz0 : 0);
      for (unsigned z = 0; z < layers; z++)
         for (unsigned y = 0; y < w; y++)
            for (unsigned x = 0; x < w; x++) {
               uint32_t t[4];
               texel(r, e, l, z, x, y, t);
               memcpy(base + ((z * w + y) * w + x) * bpp, t, bpp);
            }
   }
   VkBuffer st;
   if (vk_buffer(sz0 + sz1, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, data, &st, NULL))
      return -1;
   free(data);
   barrier(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, layers);
   VkBufferImageCopy c[2] = {
      { .bufferOffset = 0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers }, .imageExtent = { TW, TW, 1 } },
      { .bufferOffset = sz0, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, layers }, .imageExtent = { TW / 2, TW / 2, 1 } },
   };
   vkCmdCopyBufferToImage(cmd, st, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, c);
   barrier(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 2, layers);
   VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                .viewType = r->arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 2, 0, layers } };
   VK(vkCreateImageView(dev, &vc, NULL, view));
   return 0;
}

static VkDescriptorType
vk_desc_type(const struct res *r)
{
   if (r->lowered == 1)
      return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
   switch (r->kind) {
   case 0: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
   case 1: case 2: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   case 3: return r->combined ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
   default: return VK_DESCRIPTOR_TYPE_SAMPLER;
   }
}

static int
vk_draw(const uint32_t *vs, long vs_size, const uint32_t *fs, long fs_size, struct prog *pv, struct prog *pf,
        uint8_t *out)
{
   VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
   VkCommandPool cp;
   VK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
   VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
   VkCommandBuffer cmd;
   VK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
   VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   vkBeginCommandBuffer(cmd, &cbi);

   /* the descriptor set layouts: the union of both stages' bindings (Vulkan sets only) */
   struct res all[3 * MAX_RES];
   unsigned nall = 0;
   for (unsigned s = 0; s < 3; s++) {
      struct prog *p = s == 2 ? pgs : s ? pf : pv;
      for (unsigned i = 0; p && i < p->nres; i++)
         if (p->res[i].set < 4 && !p->res[i].sampler_of_combined)
            all[nall++] = p->res[i];
   }
   VkDescriptorSetLayoutBinding lb[4][MAX_RES];
   unsigned nlb[4] = { 0 }, nsets = 0;
   for (unsigned i = 0; i < nall; i++) {
      struct res *r = &all[i];
      bool seen = false;
      for (unsigned j = 0; j < nlb[r->set]; j++)
         seen |= lb[r->set][j].binding == r->binding;
      if (!seen)
         lb[r->set][nlb[r->set]++] = (VkDescriptorSetLayoutBinding){ r->binding, vk_desc_type(r), r->count,
                                                                     VK_SHADER_STAGE_ALL_GRAPHICS };
      nsets = r->set + 1 > nsets ? r->set + 1 : nsets;
   }
   VkDescriptorSetLayout dsl[4];
   for (unsigned s = 0; s < nsets; s++) {
      VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                             .bindingCount = nlb[s], .pBindings = lb[s] };
      VK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl[s]));
   }
   VkPushConstantRange pcr = { VK_SHADER_STAGE_ALL_GRAPHICS, 0, sc.push_bytes };
   VkPipelineLayoutCreateInfo pl = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nsets,
                                     .pSetLayouts = dsl, .pushConstantRangeCount = sc.push_bytes ? 1 : 0,
                                     .pPushConstantRanges = &pcr };
   VkPipelineLayout layout;
   VK(vkCreatePipelineLayout(dev, &pl, NULL, &layout));
   VkShaderModule vsm, fsm;
   VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vs_size, .pCode = vs };
   VK(vkCreateShaderModule(dev, &smi, NULL, &vsm));
   smi.codeSize = fs_size;
   smi.pCode = fs;
   VK(vkCreateShaderModule(dev, &smi, NULL, &fsm));
   VkPipelineShaderStageCreateInfo stages[5] = {
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vsm, .pName = "main" },
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fsm, .pName = "main" },
      { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .pName = "main" },
   };
   unsigned nstages = 2;
   if (gs_spv) {
      smi.codeSize = gs_size;
      smi.pCode = gs_spv;
      VK(vkCreateShaderModule(dev, &smi, NULL, &stages[2].module));
      nstages = 3;
   }
   if (tcs_spv) {
      for (unsigned t = 0; t < 2; t++) {
         VkPipelineShaderStageCreateInfo *st = &stages[nstages++];
         *st = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = t ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT :
                                                               VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                  .pName = "main" };
         smi.codeSize = t ? tes_size : tcs_size;
         smi.pCode = t ? tes_spv : tcs_spv;
         VK(vkCreateShaderModule(dev, &smi, NULL, &st->module));
      }
   }
   VkPipelineTessellationStateCreateInfo ts = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                                                .patchControlPoints = sc.patch_size ? sc.patch_size : 3 };
   VkVertexInputBindingDescription vbd[8];
   VkVertexInputBindingDivisorDescriptionEXT vdd[8];
   VkVertexInputAttributeDescription vad[16];
   unsigned nvb = 0, nvd = 0, nva = 0;
   for (unsigned b = 0; b < 8; b++) {
      if (!sc.vb[b].used)
         continue;
      vbd[nvb++] = (VkVertexInputBindingDescription){ b, sc.vb[b].stride,
                                                      sc.vb[b].per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX };
      if (sc.vb[b].per_instance && sc.vb[b].divisor != 1)
         vdd[nvd++] = (VkVertexInputBindingDivisorDescriptionEXT){ b, sc.vb[b].divisor };
   }
   for (unsigned l = 0; l < 16; l++) {
      if (!sc.attr[l].used)
         continue;
      int fi = format_index(sc.attr[l].format);
      if (fi < 0)
         return -1;
      vad[nva++] = (VkVertexInputAttributeDescription){ l, sc.attr[l].binding, formats[fi].vk, sc.attr[l].offset };
   }
   VkPipelineVertexInputDivisorStateCreateInfoEXT vdiv = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                           .vertexBindingDivisorCount = nvd, .pVertexBindingDivisors = vdd };
   VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                               .pNext = nvd ? &vdiv : NULL,
                                               .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = vbd,
                                               .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = vad };
   VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = (VkPrimitiveTopology)sc.topology };
   VkViewport vp = { 0, 0, W, H, 0, 1 };
   VkRect2D scr = { { 0, 0 }, { W, H } };
   VkViewport vps_v[16];
   VkRect2D vps_s[16];
   for (unsigned i = 0; i < sc.nviewports; i++) {
      vps_v[i] = (VkViewport){ sc.viewport[i][0], sc.viewport[i][1], sc.viewport[i][2], sc.viewport[i][3],
                               sc.viewport[i][4], sc.viewport[i][5] };
      vps_s[i] = (VkRect2D){ { sc.scissor[i][0], sc.scissor[i][1] }, { sc.scissor[i][2], sc.scissor[i][3] } };
   }
   VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = sc.nviewports ? sc.nviewports : 1,
                                             .pViewports = sc.nviewports ? vps_v : &vp,
                                             .scissorCount = sc.nviewports ? sc.nviewports : 1,
                                             .pScissors = sc.nviewports ? vps_s : &scr };
   VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
   VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
   VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &cba };
   VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   VkFormat dfmt = sc.depth_bits == 16 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT;
   VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                 .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_FALSE,
                                                 .depthCompareOp = (VkCompareOp)sc.depth_op };
   VkPipelineRenderingCreateInfo pr = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .viewMask = sc.view ? 1u << sc.view : 0,
                                        .depthAttachmentFormat = sc.depth_bits ? dfmt : VK_FORMAT_UNDEFINED,
                                        .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt };
   VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &pr, .stageCount = nstages,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
                                       .pTessellationState = tcs_spv ? &ts : NULL,
                                       .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
                                       .pDepthStencilState = sc.depth_bits ? &dss : NULL,
                                       .pColorBlendState = &cb, .layout = layout };
   VkPipeline pipe;
   VK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));

   VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .magFilter = sc.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                               .minFilter = sc.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                               .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 16 };
   VkSampler smp, ismp;
   VK(vkCreateSampler(dev, &sci, NULL, &smp));
   sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
   VK(vkCreateSampler(dev, &sci, NULL, &ismp));
   VkDescriptorPoolSize ps[6] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64 },
                                  { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64 },
                                  { VK_DESCRIPTOR_TYPE_SAMPLER, 64 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8 } };
   VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 4, .poolSizeCount = 6, .pPoolSizes = ps };
   VkDescriptorPool pool;
   VK(vkCreateDescriptorPool(dev, &dpi, NULL, &pool));
   VkDescriptorSet set[4];
   if (nsets) {
      VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                          .descriptorSetCount = nsets, .pSetLayouts = dsl };
      VK(vkAllocateDescriptorSets(dev, &dsa, set));
   }
   /* every element of every binding gets a resource, used or not */
   for (unsigned s = 0; s < nsets; s++)
      for (unsigned j = 0; j < nlb[s]; j++) {
         struct res *r = NULL;
         for (unsigned i = 0; i < nall; i++)
            if (all[i].set == s && all[i].binding == lb[s][j].binding)
               r = &all[i];
         for (unsigned e = 0; e < lb[s][j].descriptorCount; e++) {
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set[s],
                                       .dstBinding = lb[s][j].binding, .dstArrayElement = e,
                                       .descriptorCount = 1, .descriptorType = lb[s][j].descriptorType };
            VkDescriptorBufferInfo bi;
            VkDescriptorImageInfo ii = { .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkBuffer buf;
            switch (w.descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
               bool u = w.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
               static uint32_t data[BUF_WORDS];
               for (unsigned k = 0; k < BUF_WORDS; k++)
                  data[k] = u ? ubo_init[(k + e * 5) % 1024] : ssbo_init[(k + e * 5) % 1024];
               scene_ubo_fill(u, s, lb[s][j].binding, e, data);
               if (vk_buffer(BUF_WORDS * 4, u ? VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, data, &buf, NULL))
                  return -1;
               bi = (VkDescriptorBufferInfo){ buf, 0, BUF_WORDS * 4 };
               w.pBufferInfo = &bi;
               break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLER:
               ii.sampler = smp;
               w.pImageInfo = &ii;
               break;
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
               if (vk_atlas(cmd, &ii.imageView))
                  return -1;
               ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
               w.pImageInfo = &ii;
               break;
            default:
               if (vk_texture(cmd, r, e, &ii.imageView))
                  return -1;
               ii.sampler = r->sample ? ismp : smp;
               w.pImageInfo = &ii;
               break;
            }
            vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
         }
      }
   VkBuffer vbuf[8], ibuf, rb;
   void *rbp;
   for (unsigned b = 0; b < 8; b++)
      if (sc.vb[b].used && vk_buffer(SCENE_VB_SIZE, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vb_data[b], &vbuf[b], NULL))
         return -1;
   uint16_t idx16[256];
   for (unsigned i = 0; i < 256; i++)
      idx16[i] = idx_data[i];
   if (vk_buffer(1024, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, sc.index_bits == 16 ? (void *)idx16 : (void *)idx_data, &ibuf, NULL) ||
       vk_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, NULL, &rb, &rbp))
      return -1;
   VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                            .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = sc.view + sc.layer + 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
   VkImage rt;
   VK(vkCreateImage(dev, &ic, NULL, &rt));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, rt, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) };
   VkDeviceMemory rtm;
   VK(vkAllocateMemory(dev, &mai, NULL, &rtm));
   VK(vkBindImageMemory(dev, rt, rtm, 0));
   VkImageViewCreateInfo vc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = rt,
                                .viewType = sc.view || sc.layer ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, sc.view + sc.layer + 1 } };
   VkImageView rtv;
   VK(vkCreateImageView(dev, &vc, NULL, &rtv));

   barrier(cmd, rt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, sc.view + sc.layer + 1);
   /* a depth attachment: d16 filled with the pattern, d32 cleared to 0.5 */
   VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
   if (sc.depth_bits) {
      VkImageCreateInfo dic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = dfmt,
                                .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
      VkImage dimg;
      VK(vkCreateImage(dev, &dic, NULL, &dimg));
      VkMemoryRequirements dmr;
      vkGetImageMemoryRequirements(dev, dimg, &dmr);
      VkMemoryAllocateInfo dmai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = dmr.size,
                                    .memoryTypeIndex = memtype(dmr.memoryTypeBits, 0) };
      VkDeviceMemory dm;
      VK(vkAllocateMemory(dev, &dmai, NULL, &dm));
      VK(vkBindImageMemory(dev, dimg, dm, 0));
      VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = dimg, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
      if (sc.depth_bits == 16) {
         static uint16_t dpat[W * H];
         for (unsigned y = 0; y < H; y++)
            for (unsigned x = 0; x < W; x++)
               dpat[y * W + x] = scene_depth16(x, y);
         VkBuffer dbuf;
         if (vk_buffer(sizeof(dpat), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, dpat, &dbuf, NULL))
            return -1;
         VkBufferImageCopy dc = { .imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
         vkCmdCopyBufferToImage(cmd, dbuf, dimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dc);
      } else {
         VkClearDepthStencilValue cv = { 0.5f, 0 };
         VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
         vkCmdClearDepthStencilImage(cmd, dimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr);
      }
      db.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      db.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
      db.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      db.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
      VkImageViewCreateInfo dvc = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dimg, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = dfmt, .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
      VkImageView dv;
      VK(vkCreateImageView(dev, &dvc, NULL, &dv));
      da.imageView = dv;
      da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
   }
   VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = rtv,
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
   /* (multiview: the view's layer only, as wgvk renders each view in a pass of its own) */
   VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = scr, .layerCount = sc.layer + 1,
                          .viewMask = sc.view ? 1u << sc.view : 0,
                          /* (layered: every layer; layer N read back, as wgvk renders one pass per layer) */
                          .colorAttachmentCount = 1, .pColorAttachments = &ca,
                          .pDepthAttachment = sc.depth_bits ? &da : NULL };
   vkCmdBeginRendering(cmd, &ri);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
   if (nsets)
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, nsets, set, 0, NULL);
   if (sc.push_bytes)
      vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sc.push_bytes, push_data);
   for (unsigned b = 0; b < 8; b++)
      if (sc.vb[b].used) {
         VkDeviceSize off = 0;
         vkCmdBindVertexBuffers(cmd, b, 1, &vbuf[b], &off);
      }
   /* transform feedback: captured into host-visible buffers, compared after both draws */
   PFN_vkCmdBindTransformFeedbackBuffersEXT bind_tf =
      (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(dev, "vkCmdBindTransformFeedbackBuffersEXT");
   PFN_vkCmdBeginTransformFeedbackEXT begin_tf =
      (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(dev, "vkCmdBeginTransformFeedbackEXT");
   PFN_vkCmdEndTransformFeedbackEXT end_tf =
      (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(dev, "vkCmdEndTransformFeedbackEXT");
   bool any_xfb = false;
   for (unsigned b = 0; b < 4; b++) {
      if (!sc.xfb_size[b])
         continue;
      VkBuffer xb;
      if (vk_buffer(sc.xfb_size[b], VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT, NULL, &xb, &xfb_vkmap[b]))
         return -1;
      VkDeviceSize off = 0, size = sc.xfb_size[b];
      bind_tf(cmd, b, 1, &xb, &off, &size);
      any_xfb = true;
   }
   /* VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT on stream 0: what libpoly's counters must match */
   VkQueryPool qp = VK_NULL_HANDLE;
   PFN_vkCmdBeginQueryIndexedEXT begin_q =
      (PFN_vkCmdBeginQueryIndexedEXT)vkGetDeviceProcAddr(dev, "vkCmdBeginQueryIndexedEXT");
   PFN_vkCmdEndQueryIndexedEXT end_q = (PFN_vkCmdEndQueryIndexedEXT)vkGetDeviceProcAddr(dev, "vkCmdEndQueryIndexedEXT");
   if (any_xfb) {
      VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT, .queryCount = 1 };
      VK(vkCreateQueryPool(dev, &qci, NULL, &qp));
      vkCmdEndRendering(cmd);
      vkCmdResetQueryPool(cmd, qp, 0, 1);
      vkCmdBeginRendering(cmd, &ri);
      begin_q(cmd, qp, 0, 0, 0);
   }
   if (any_xfb)
      begin_tf(cmd, 0, 0, NULL, NULL);
   if (sc.indirect) {
      uint32_t args[5];
      scene_indirect_args(args);
      VkBuffer ib;
      if (vk_buffer(64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, args, &ib, NULL))
         return -1;
      if (sc.indexed) {
         vkCmdBindIndexBuffer(cmd, ibuf, 0, sc.index_bits == 16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
         vkCmdDrawIndexedIndirect(cmd, ib, 0, 1, 20);
      } else {
         vkCmdDrawIndirect(cmd, ib, 0, 1, 16);
      }
   } else if (sc.indexed) {
      vkCmdBindIndexBuffer(cmd, ibuf, 0, sc.index_bits == 16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cmd, sc.count, sc.instances, sc.first, (int32_t)sc.base_vertex, sc.first_instance);
   } else {
      vkCmdDraw(cmd, sc.count, sc.instances, sc.first, sc.first_instance);
   }
   if (any_xfb) {
      end_tf(cmd, 0, 0, NULL, NULL);
      end_q(cmd, qp, 0, 0);
   }
   vkCmdEndRendering(cmd);
   barrier(cmd, rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, sc.view + sc.layer + 1);
   VkBufferImageCopy rbc = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, sc.view + sc.layer, 1 }, .imageExtent = { W, H, 1 } };
   vkCmdCopyImageToBuffer(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &rbc);
   if (atlas_used) {
      barrier(cmd, atlas_img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 1, ATLAS_LAYERS);
      VkBufferImageCopy ac = { .bufferOffset = 16, .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, ATLAS_LAYERS },
                               .imageExtent = { ATLAS_W, ATLAS_H, 1 } };
      vkCmdCopyImageToBuffer(cmd, atlas_img, VK_IMAGE_LAYOUT_GENERAL, atlas_vkbuf, 1, &ac);
   }
   vkEndCommandBuffer(cmd);
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
   VK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   vkQueueWaitIdle(q);
   query_done = false;
   if (qp) {
      VK(vkGetQueryPoolResults(dev, qp, 0, 1, sizeof(vk_query), vk_query, sizeof(vk_query),
                               VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
      query_done = true;
   }
   memcpy(out, rbp, W * H * 4);
   return 0; /* (objects are leaked: one process per run) */
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
   if (!werror)
      snprintf(werrmsg, sizeof(werrmsg), "%.*s", (int)msg.length, msg.data);   /* the first error */
   werror = 1;
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

static WGPUBuffer
wbuf(WGPUQueue queue, const void *data, unsigned size, WGPUBufferUsage usage)
{
   WGPUBufferDescriptor bd = { .size = (size + 3) & ~3u, .usage = usage | WGPUBufferUsage_CopyDst };
   WGPUBuffer b = wgpuDeviceCreateBuffer(wdev, &bd);
   if (data)
      wgpuQueueWriteBuffer(queue, b, 0, data, (size + 3) & ~3u);
   return b;
}

static WGPUBuffer
atlas_wbuf(WGPUQueue q)
{
   static uint32_t init[16 / 4 + ATLAS_W * ATLAS_H * ATLAS_LAYERS];
   atlas_init(init);
   WGPUBufferDescriptor bd = { .size = atlas_bytes(), .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc |
                               WGPUBufferUsage_CopyDst };
   atlas_wg = wgpuDeviceCreateBuffer(wdev, &bd);
   wgpuQueueWriteBuffer(q, atlas_wg, 0, init, atlas_bytes());
   return atlas_wg;
}

static WGPUBuffer wg_poly_buf;
static int buffer_readback_at(WGPUBuffer b, unsigned offset, unsigned size, uint32_t *out);

static int
buffer_readback(WGPUBuffer b, unsigned size, uint32_t *out)
{
   return buffer_readback_at(b, 0, size, out);
}

static int
buffer_readback_at(WGPUBuffer b, unsigned offset, unsigned size, uint32_t *out)
{
   WGPUBufferDescriptor rd = { .size = size, .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst };
   WGPUBuffer rb = wgpuDeviceCreateBuffer(wdev, &rd);
   WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(wdev, NULL);
   wgpuCommandEncoderCopyBufferToBuffer(enc, b, offset, rb, 0, size);
   WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
   wgpuQueueSubmit(wgpuDeviceGetQueue(wdev), 1, &cmd);
   wmapped = 0;
   wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, size, (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map });
   for (time_t t0 = time(NULL); !wmapped && time(NULL) - t0 < 60;)
      wgpuInstanceProcessEvents(winst);
   if (wmapped != 1)
      return -1;
   memcpy(out, wgpuBufferGetConstMappedRange(rb, 0, size), size);
   return 0;
}

static int
atlas_readback(uint32_t *out)
{
   WGPUBufferDescriptor rd = { .size = atlas_bytes(), .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst };
   WGPUBuffer rb = wgpuDeviceCreateBuffer(wdev, &rd);
   WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(wdev, NULL);
   wgpuCommandEncoderCopyBufferToBuffer(enc, atlas_wg, 0, rb, 0, atlas_bytes());
   WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
   wgpuQueueSubmit(wgpuDeviceGetQueue(wdev), 1, &cmd);
   wmapped = 0;
   wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, atlas_bytes(), (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map });
   for (time_t t0 = time(NULL); !wmapped && time(NULL) - t0 < 60;)
      wgpuInstanceProcessEvents(winst);
   if (wmapped != 1)
      return -1;
   memcpy(out, wgpuBufferGetConstMappedRange(rb, 0, atlas_bytes()), atlas_bytes());
   return 0;
}

static WGPUTextureView
wtexture(WGPUQueue queue, const struct res *r, unsigned e)
{
   unsigned layers = r->arrayed ? 3 : 1, bpp = r->sample == 0 ? 4 : 16;
   WGPUTextureDescriptor td = { .usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst,
                                .dimension = WGPUTextureDimension_2D, .size = { TW, TW, layers },
                                .format = r->sample == 0 ? WGPUTextureFormat_RGBA8Unorm :
                                          r->sample == 1 ? WGPUTextureFormat_RGBA32Sint : WGPUTextureFormat_RGBA32Uint,
                                .mipLevelCount = 2, .sampleCount = 1 };
   WGPUTexture tex = wgpuDeviceCreateTexture(wdev, &td);
   for (unsigned l = 0; l < 2; l++) {
      unsigned w = TW >> l;
      uint8_t *data = malloc(w * w * bpp * layers);
      for (unsigned z = 0; z < layers; z++)
         for (unsigned y = 0; y < w; y++)
            for (unsigned x = 0; x < w; x++) {
               uint32_t t[4];
               texel(r, e, l, z, x, y, t);
               memcpy(data + ((z * w + y) * w + x) * bpp, t, bpp);
            }
      WGPUTexelCopyTextureInfo dst = { .texture = tex, .mipLevel = l };
      WGPUTexelCopyBufferLayout lay = { .bytesPerRow = w * bpp, .rowsPerImage = w };
      WGPUExtent3D ext = { w, w, layers };
      wgpuQueueWriteTexture(queue, &dst, data, w * w * bpp * layers, &lay, &ext);
      free(data);
   }
   WGPUTextureViewDescriptor vd = { .format = td.format,
                                    .dimension = r->arrayed ? WGPUTextureViewDimension_2DArray : WGPUTextureViewDimension_2D,
                                    .mipLevelCount = 2, .arrayLayerCount = layers, .aspect = WGPUTextureAspect_All,
                                    .usage = WGPUTextureUsage_TextureBinding };
   return wgpuTextureCreateView(tex, &vd);
}

/* driver uniforms (wgvk_compile.h: struct wgvk_driver_uniforms) */
static void
driver_uniforms(uint32_t *u)
{
   memset(u, 0, 1024);
   u[0] = sc.indexed ? sc.base_vertex : sc.first;
   u[1] = sc.first_instance;
   u[2] = 0;
   u[3] = sc.first;
   float f[4] = { 1.0f / W, 1.0f / H, 1.0f, 64.0f };
   memcpy(&u[4], f, 16);
   u[34] = sc.view;   /* view_index */
   u[36] = sc.layer;  /* layer */
   /* early fragment tests: the depth test's op and format (depth_compare at 804, depth_unorm_max at 808) */
   u[804 / 4] = sc.depth_bits ? sc.depth_op : 7;
   float dmax = sc.depth_bits == 16 ? 65535.0f : 0.0f;
   memcpy(&u[808 / 4], &dmax, 4);
   /* multiple viewports: Vulkan's viewports and scissors, the WebGPU viewport the whole target
    * (struct wgvk_driver_uniforms: render_area at 148, viewports at 164, scissors at 548) */
   float ra[4] = { 0, 0, W, H };
   memcpy(&u[148 / 4], ra, 16);
   memcpy(&u[164 / 4], sc.viewport, sizeof(sc.viewport));
   memcpy(&u[548 / 4], sc.scissor, sizeof(sc.scissor));
}

static int
wgpu_draw(const char *vs, const char *fs, struct prog *pv, struct prog *pf, uint8_t *out)
{
   werror = 0;
   WGPUShaderSourceWGSL vsrc = { .chain = { .sType = WGPUSType_ShaderSourceWGSL }, .code = sv(vs) };
   WGPUShaderModuleDescriptor vmd = { .nextInChain = &vsrc.chain };
   WGPUShaderModule vsm = wgpuDeviceCreateShaderModule(wdev, &vmd);
   WGPUShaderSourceWGSL fsrc = { .chain = { .sType = WGPUSType_ShaderSourceWGSL }, .code = sv(fs) };
   WGPUShaderModuleDescriptor fmd = { .nextInChain = &fsrc.chain };
   WGPUShaderModule fsm = wgpuDeviceCreateShaderModule(wdev, &fmd);
   /* WebGPU's vertex state: the bindings and attributes that were not pulled */
   bool expanded = pv->points_expanded;
   WGPUVertexAttribute va[8][16];
   WGPUVertexBufferLayout vbl[8];
   unsigned slot_of[8], nslots = 0;
   for (unsigned b = 0; b < 8; b++) {
      slot_of[b] = ~0u;
      if (!sc.vb[b].used)
         continue;
      unsigned na = 0;
      for (unsigned l = 0; l < 16; l++) {
         if (!sc.attr[l].used || sc.attr[l].binding != b || (pv->pulled & (1u << l)))
            continue;
         int fi = format_index(sc.attr[l].format);
         if (fi < 0 || !formats[fi].wg) {
            fprintf(stderr, "scene-run: attribute %u (%s) not pulled and not a WebGPU vertex format\n", l, sc.attr[l].format);
            return -1;
         }
         va[nslots][na++] = (WGPUVertexAttribute){ .format = formats[fi].wg, .offset = sc.attr[l].offset, .shaderLocation = l };
      }
      if (!na)
         continue;
      slot_of[b] = nslots;
      vbl[nslots] = (WGPUVertexBufferLayout){ .stepMode = sc.vb[b].per_instance ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex,
                                              .arrayStride = sc.vb[b].stride, .attributeCount = na, .attributes = va[nslots] };
      nslots++;
   }
   /* WebGPU wants writeMask 0 for a target the fragment shader does not write (Vulkan ignores it): T2 */
   WGPUColorTargetState ct = { .format = WGPUTextureFormat_RGBA8Unorm,
                               .writeMask = strstr(fs, "@location(0) c0") ? WGPUColorWriteMask_All : WGPUColorWriteMask_None };
   WGPUFragmentState fst = { .module = fsm, .entryPoint = sv("main"), .targetCount = 1, .targets = &ct };
   WGPUDepthStencilState dst = { .format = sc.depth_bits == 16 ? WGPUTextureFormat_Depth16Unorm : WGPUTextureFormat_Depth32Float,
                                 .depthWriteEnabled = WGPUOptionalBool_False,
                                 .depthCompare = (WGPUCompareFunction)(WGPUCompareFunction_Never + sc.depth_op),
                                 .stencilFront = { .compare = WGPUCompareFunction_Always },
                                 .stencilBack = { .compare = WGPUCompareFunction_Always },
                                 .stencilReadMask = 0xff, .stencilWriteMask = 0 };
   WGPURenderPipelineDescriptor rpd = {
      .vertex = { .module = vsm, .entryPoint = sv("main"), .bufferCount = nslots, .buffers = vbl },
      .depthStencil = sc.depth_bits ? &dst : NULL,
      .primitive = { .topology = sc.points && !expanded ? WGPUPrimitiveTopology_PointList : WGPUPrimitiveTopology_TriangleList,
                     .cullMode = WGPUCullMode_None, .frontFace = WGPUFrontFace_CCW },
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
   WGPUSamplerDescriptor sd = { .addressModeU = WGPUAddressMode_ClampToEdge, .addressModeV = WGPUAddressMode_ClampToEdge,
                                .addressModeW = WGPUAddressMode_ClampToEdge,
                                .magFilter = sc.linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest,
                                .minFilter = sc.linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest,
                                .mipmapFilter = WGPUMipmapFilterMode_Nearest, .lodMinClamp = 0, .lodMaxClamp = 16,
                                .maxAnisotropy = 1 };
   WGPUSampler smp = wgpuDeviceCreateSampler(wdev, &sd);
   WGPUBuffer vbuf[8];
   for (unsigned b = 0; b < 8; b++)
      vbuf[b] = sc.vb[b].used ? wbuf(queue, vb_data[b], SCENE_VB_SIZE, WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage) : NULL;
   uint16_t idx16[256];
   for (unsigned i = 0; i < 256; i++)
      idx16[i] = idx_data[i];
   WGPUBuffer ibuf = wbuf(queue, sc.index_bits == 16 ? (void *)idx16 : (void *)idx_data, 1024,
                          WGPUBufferUsage_Index | WGPUBufferUsage_Storage);
   uint32_t du[256];
   driver_uniforms(du);
   uint32_t imm[16] = { 0 };
   memcpy(imm, push_data, sc.push_bytes < 64 ? sc.push_bytes : 64);
   unsigned dpo = pv->draw_params_offset != 0xffff ? pv->draw_params_offset : pf->draw_params_offset;
   if (dpo != 0xffff && dpo + 12 <= 64)
      memcpy((uint8_t *)imm + dpo, du, 12);

   /* the depth attachment (read-only in the pass; the fragment shader's early test reads it at 938) */
   WGPUTextureView dview = NULL;
   if (sc.depth_bits) {
      WGPUTextureDescriptor dtd = { .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding |
                                             WGPUTextureUsage_CopyDst,
                                    .dimension = WGPUTextureDimension_2D, .size = { W, H, 1 },
                                    .format = sc.depth_bits == 16 ? WGPUTextureFormat_Depth16Unorm : WGPUTextureFormat_Depth32Float,
                                    .mipLevelCount = 1, .sampleCount = 1 };
      WGPUTexture dt = wgpuDeviceCreateTexture(wdev, &dtd);
      dview = wgpuTextureCreateView(dt, NULL);
      if (sc.depth_bits == 16) {
         static uint16_t dpat[W * H];
         for (unsigned y = 0; y < H; y++)
            for (unsigned x = 0; x < W; x++)
               dpat[y * W + x] = scene_depth16(x, y);
         WGPUTexelCopyTextureInfo dtc = { .texture = dt, .aspect = WGPUTextureAspect_DepthOnly };
         WGPUTexelCopyBufferLayout dl = { .bytesPerRow = W * 2, .rowsPerImage = H };
         WGPUExtent3D dext = { W, H, 1 };
         wgpuQueueWriteTexture(queue, &dtc, dpat, sizeof(dpat), &dl, &dext);
      } else {
         /* cleared to 0.5 by a pass of its own (a read-only depth attachment cannot be cleared) */
         WGPUCommandEncoder ce = wgpuDeviceCreateCommandEncoder(wdev, NULL);
         WGPURenderPassDepthStencilAttachment cda = { .view = dview, .depthLoadOp = WGPULoadOp_Clear,
                                                      .depthStoreOp = WGPUStoreOp_Store, .depthClearValue = 0.5f };
         WGPURenderPassDescriptor crp = { .depthStencilAttachment = &cda };
         wgpuRenderPassEncoderEnd(wgpuCommandEncoderBeginRenderPass(ce, &crp));
         WGPUCommandBuffer cb2 = wgpuCommandEncoderFinish(ce, NULL);
         wgpuQueueSubmit(queue, 1, &cb2);
      }
   }
   /* bind groups: every binding either stage declares (set 255 = driver bindings) */
   WGPUBindGroupEntry be[4][2 * MAX_RES];
   unsigned nbe[4] = { 0 };
   for (unsigned s = 0; s < 2; s++) {
      struct prog *p = s ? pf : pv;
      for (unsigned i = 0; i < p->nres; i++) {
         struct res *r = &p->res[i];
         bool seen = false;
         for (unsigned j = 0; j < nbe[r->group]; j++)
            seen |= be[r->group][j].binding == r->wbinding;
         if (seen)
            continue;
         WGPUBindGroupEntry e = { .binding = r->wbinding };
         if (r->set == 255) {
            if (r->binding == 900) {
               e.buffer = wbuf(queue, push_data, 128, WGPUBufferUsage_Uniform);
               e.size = 128;
            } else if (r->binding == 901) {
               e.buffer = wbuf(queue, du, 1024, WGPUBufferUsage_Uniform);
               e.size = 1024;
            } else if (r->binding >= 920 && r->binding < 928) {
               e.buffer = vbuf[r->binding - 920];
               e.size = SCENE_VB_SIZE;
            } else if (r->binding == 936) {
               e.buffer = ibuf;
               e.size = 1024;
            } else if (r->binding == 938 && dview) {
               e.textureView = dview;
            } else {
               fprintf(stderr, "scene-run: driver binding %u\n", r->binding);
               return -1;
            }
         } else if (r->kind <= 2) {
            static uint32_t data[BUF_WORDS];
            for (unsigned k = 0; k < BUF_WORDS; k++)
               data[k] = r->kind == 0 ? ubo_init[(k + r->index * 5) % 1024] : ssbo_init[(k + r->index * 5) % 1024];
            scene_ubo_fill(r->kind == 0, r->set, r->binding, r->index, data);
            if (r->lowered == 1) {
               /* an image used atomically: header + texels (the Vulkan side has the image) */
               e.buffer = atlas_wbuf(queue);
               e.size = atlas_bytes();
            } else {
               e.buffer = wbuf(queue, data, BUF_WORDS * 4, r->kind == 0 ? WGPUBufferUsage_Uniform : WGPUBufferUsage_Storage);
               e.size = BUF_WORDS * 4;
            }
         } else if (r->kind == 3) {
            e.textureView = wtexture(queue, r, r->index);
         } else if (r->kind == 5) {
            e.sampler = smp;
         } else {
            fprintf(stderr, "scene-run: binding kind %u\n", r->kind);
            return -1;
         }
         be[r->group][nbe[r->group]++] = e;
      }
   }
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
   WGPURenderPassDepthStencilAttachment rda = { .view = dview, .depthReadOnly = true, .stencilReadOnly = true };
   WGPURenderPassDescriptor rpass = { .colorAttachmentCount = 1, .colorAttachments = &rca,
                                      .depthStencilAttachment = dview ? &rda : NULL };
   WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rpass);
   wgpuRenderPassEncoderSetPipeline(pass, rp);
   for (unsigned g = 0; g < 4; g++) {
      if (!nbe[g])
         continue;
      WGPUBindGroupDescriptor bgd = { .layout = wgpuRenderPipelineGetBindGroupLayout(rp, g), .entryCount = nbe[g], .entries = be[g] };
      wgpuRenderPassEncoderSetBindGroup(pass, g, wgpuDeviceCreateBindGroup(wdev, &bgd), 0, NULL);
   }
   if (strstr(vs, "var<immediate>") || strstr(fs, "var<immediate>"))
      wgpuRenderPassEncoderSetImmediates(pass, 0, imm, 64);
   for (unsigned b = 0; b < 8; b++)
      if (slot_of[b] != ~0u)
         wgpuRenderPassEncoderSetVertexBuffer(pass, slot_of[b], vbuf[b], 0, SCENE_VB_SIZE);
   if (expanded)
      wgpuRenderPassEncoderDraw(pass, 6 * sc.count, sc.instances, 6 * sc.first, sc.first_instance);
   else if (sc.indexed) {
      wgpuRenderPassEncoderSetIndexBuffer(pass, ibuf, sc.index_bits == 16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32, 0, 1024);
      wgpuRenderPassEncoderDrawIndexed(pass, sc.count, sc.instances, sc.first, (int32_t)sc.base_vertex, sc.first_instance);
   } else
      wgpuRenderPassEncoderDraw(pass, sc.count, sc.instances, sc.first, sc.first_instance);
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


/* ------------------------------------------------------------------ Dawn, with a geometry shader */

#define POLY_SIZE (4u << 20)
#define POLY_VP 0
#define POLY_GP 128
#define POLY_SINK 512
#define POLY_VS_OUT 4096
#define POLY_INDEX (512u << 10)
#define POLY_STATIC_INDEX (896u << 10)
#define POLY_COUNT 1024
#define POLY_XFB_OFFS 960
#define POLY_QUERY 992
#define POLY_SETUP 640
#define POLY_HEAP 704
#define POLY_DRAW_PARAMS 752
#define POLY_HEAP_BASE (64u << 10)
#define IND_DISPATCH 32

struct gs_ctx {
   WGPUQueue queue;
   WGPUBuffer poly, du, vbuf[8], ibuf, ind;
   WGPUSampler smp;
};

/* the bind group entries of one or more parts' .bind tables */
static int
gs_entries(struct gs_ctx *c, struct prog **progs, unsigned np, WGPUBindGroupEntry be[4][2 * MAX_RES], unsigned nbe[4])
{
   memset(nbe, 0, 4 * sizeof(unsigned));
   for (unsigned s = 0; s < np; s++) {
      for (unsigned i = 0; i < progs[s]->nres; i++) {
         struct res *r = &progs[s]->res[i];
         bool seen = false;
         for (unsigned j = 0; j < nbe[r->group]; j++)
            seen |= be[r->group][j].binding == r->wbinding;
         if (seen)
            continue;
         WGPUBindGroupEntry e = { .binding = r->wbinding };
         if (r->set == 255) {
            if (r->binding == 900) {
               e.buffer = wbuf(c->queue, push_data, 128, WGPUBufferUsage_Uniform);
               e.size = 128;
            } else if (r->binding == 901) {
               e.buffer = c->du;
               e.size = 1024;
            } else if (r->binding >= 920 && r->binding < 928) {
               e.buffer = c->vbuf[r->binding - 920];
               e.size = SCENE_VB_SIZE;
            } else if (r->binding == 936) {
               e.buffer = c->ibuf;
               e.size = 1024;
            } else if (r->binding == 940) {
               e.buffer = c->poly;
               e.size = POLY_SIZE;
            } else if (r->binding == 937) {
               e.buffer = c->ind;
               e.size = 256;
            } else if (r->binding >= 941 && r->binding < 945 && xfb_wg[r->binding - 941]) {
               e.buffer = xfb_wg[r->binding - 941];
               e.size = sc.xfb_size[r->binding - 941];
            } else {
               fprintf(stderr, "scene-run: driver binding %u\n", r->binding);
               return -1;
            }
         } else if (r->kind <= 2) {
            static uint32_t data[BUF_WORDS];
            for (unsigned k = 0; k < BUF_WORDS; k++)
               data[k] = r->kind == 0 ? ubo_init[(k + r->index * 5) % 1024] : ssbo_init[(k + r->index * 5) % 1024];
            scene_ubo_fill(r->kind == 0, r->set, r->binding, r->index, data);
            if (r->lowered == 1) {
               /* an image used atomically: header + texels (the Vulkan side has the image) */
               e.buffer = atlas_wbuf(c->queue);
               e.size = atlas_bytes();
            } else {
               e.buffer = wbuf(c->queue, data, BUF_WORDS * 4, r->kind == 0 ? WGPUBufferUsage_Uniform : WGPUBufferUsage_Storage);
               e.size = BUF_WORDS * 4;
            }
         } else if (r->kind == 3) {
            e.textureView = wtexture(c->queue, r, r->index);
         } else if (r->kind == 5) {
            e.sampler = c->smp;
         } else {
            fprintf(stderr, "scene-run: binding kind %u\n", r->kind);
            return -1;
         }
         be[r->group][nbe[r->group]++] = e;
      }
   }
   return 0;
}

static WGPUShaderModule
wmodule(const char *code)
{
   WGPUShaderSourceWGSL src = { .chain = { .sType = WGPUSType_ShaderSourceWGSL }, .code = sv(code) };
   WGPUShaderModuleDescriptor md = { .nextInChain = &src.chain };
   return wgpuDeviceCreateShaderModule(wdev, &md);
}

/* one compute part: (groups_x, groups_y) workgroups */
static int
gs_dispatch(struct gs_ctx *c, WGPUCommandEncoder enc, const char *code, struct prog *p, unsigned gx, unsigned gy,
            const uint32_t *imm)
{
   WGPUComputePipelineDescriptor cpd = { .compute = { .module = wmodule(code), .entryPoint = sv("main") } };
   WGPUComputePipeline cp = wgpuDeviceCreateComputePipeline(wdev, &cpd);
   wgpuInstanceProcessEvents(winst);
   if (werror)
      return -1;
   WGPUBindGroupEntry be[4][2 * MAX_RES];
   unsigned nbe[4];
   if (gs_entries(c, &p, 1, be, nbe))
      return -1;
   WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(enc, NULL);
   wgpuComputePassEncoderSetPipeline(pass, cp);
   for (unsigned g = 0; g < 4; g++) {
      if (!nbe[g])
         continue;
      WGPUBindGroupDescriptor bgd = { .layout = wgpuComputePipelineGetBindGroupLayout(cp, g), .entryCount = nbe[g], .entries = be[g] };
      wgpuComputePassEncoderSetBindGroup(pass, g, wgpuDeviceCreateBindGroup(wdev, &bgd), 0, NULL);
   }
   if (strstr(code, "var<immediate>"))
      wgpuComputePassEncoderSetImmediates(pass, 0, imm, 64);
   if (gx == ~0u)
      wgpuComputePassEncoderDispatchWorkgroupsIndirect(pass, c->ind, gy);
   else
      wgpuComputePassEncoderDispatchWorkgroups(pass, gx, gy, 1);
   wgpuComputePassEncoderEnd(pass);
   return 0;
}

static const char *gs_setup;      /* NAME-geom.gs-setup.wgsl */
static struct prog *gs_setup_prog;
static const char *vsc_rast;      /* vertex shader run as compute: the passthrough */
static struct prog *vsc_rast_prog;

static int
wgpu_draw_gs(const char *vs, const char *fs, const char *gs_parts[6], struct prog *pv, struct prog *pf,
             struct prog *pg[6], uint8_t *out)
{
   werror = 0;
   struct gs_ctx c = { .queue = wgpuDeviceGetQueue(wdev) };
   bool vsc = gs_parts[0] == NULL;   /* a vertex shader run as compute, no GS: vs is its compute part */
   enum mesa_prim in_prim = sc.topology == 1 ? MESA_PRIM_LINES : sc.topology == 2 ? MESA_PRIM_LINE_STRIP :
                            sc.topology == 4 ? MESA_PRIM_TRIANGLE_STRIP : sc.topology == 0 ? MESA_PRIM_POINTS :
                            MESA_PRIM_TRIANGLES;
   /* libpoly's state, as the driver writes it for a direct draw (poly/geometry.h) */
   static uint8_t poly[POLY_SIZE];
   memset(poly, 0, sizeof(poly));
   struct poly_vertex_params *vp = (void *)(poly + POLY_VP);
   struct poly_geometry_params *gp = (void *)(poly + POLY_GP);
   static const uint32_t wg[3] = { 64, 1, 1 };
   uint64_t vs_outputs = 0;
   {
      /* the vertex shader's outputs, from its .bind legal line */
      extern uint64_t scene_vs_outputs;
      vs_outputs = scene_vs_outputs;
   }
   poly_vertex_params_init(vp, vs_outputs, wg);
   bool ind = sc.indirect && !vsc;
   if (!ind)
      poly_vertex_params_set_draw(vp, sc.count, sc.instances);
   vp->output_buffer = POLY_VS_OUT;
   if (sc.indexed && ind)
      vp->index_size_B = sc.index_bits / 8;   /* (the setup sets the buffer and its range, not the size) */
   if (sc.indexed && !ind) {
      vp->index_buffer = (1ull << 32) | (sc.first * (sc.index_bits / 8));
      vp->index_size_B = sc.index_bits / 8;
      vp->index_buffer_range_el = 256 - sc.first;
   }
   if (vsc)
      goto params_done;
   poly_geometry_params_init(gp, in_prim, wg);
   if (!ind)
      poly_geometry_params_set_draw(gp, in_prim, gsi.shape, gsi.max_indices, sc.count, sc.instances);
   for (unsigned i = 0; i < 4; i++) {
      gp->prims_generated_counter[i] = i ? POLY_SINK : POLY_QUERY + 4;      /* "needed" */
      gp->xfb_prims_generated_counter[i] = i ? POLY_SINK : POLY_QUERY;     /* "written" */
      gp->xfb_overflow[i] = POLY_SINK;
   }
   gp->xfb_any_overflow = POLY_SINK;
   if (ind) {
      /* the setup part (wgvk_compile_gs_setup) fills the rest from the draw command on the GPU */
      struct wgvk_gs_indirect_args *ia = (void *)(poly + POLY_SETUP);
      *ia = (struct wgvk_gs_indirect_args){ .vs_outputs = vs_outputs, .index_size_B = sc.indexed ? sc.index_bits / 8 : 0,
                                            .index_buffer_range_el = 256, .prim = in_prim, .prefix_sum = 0,
                                            .max_indices = gsi.max_indices, .shape = gsi.shape };
      struct poly_heap *heap = (void *)(poly + POLY_HEAP);
      /* base 0: libpoly gives the GS index buffer's first index relative to the heap's base, and the draw
       * binds the whole poly buffer as its index buffer; allocations start past the fixed structs */
      heap->base = 0;
      heap->bottom = POLY_HEAP_BASE;
      heap->size = POLY_STATIC_INDEX;
   }
   if (gsi.shape == POLY_GS_SHAPE_DYNAMIC_INDEXED && !ind) {
      gp->output_index_buffer = POLY_INDEX;
      gp->draw.first_index = POLY_INDEX / 4;
   } else if (gsi.shape == POLY_GS_SHAPE_STATIC_INDEXED) {
      uint32_t *ix = (void *)(poly + POLY_STATIC_INDEX);
      for (unsigned i = 0; i < gsi.max_indices; i++)
         ix[i] = gsi.topology[i] == 0xff ? ~0u : gsi.topology[i];
      gp->draw.first_index = POLY_STATIC_INDEX / 4;
   }
   if (gsi.count_words) {
      gp->count_buffer_stride = gsi.count_words * 4;
      /* transform feedback: per input primitive (prefix summed); otherwise one set added to atomically */
      gp->count_buffer = gsi.prefix_sum ? POLY_STATIC_INDEX + 4096 : POLY_COUNT;
   }
   for (unsigned b = 0; b < 4; b++) {
      xfb_wg[b] = NULL;
      if (!sc.xfb_size[b])
         continue;
      WGPUBufferDescriptor xd = { .size = sc.xfb_size[b], .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc };
      xfb_wg[b] = wgpuDeviceCreateBuffer(wdev, &xd);
      gp->xfb_base_original[b] = (uint64_t)(3 + b) << 32;   /* segment 3 + b, offset 0 */
      gp->xfb_size[b] = sc.xfb_size[b];
      gp->xfb_offs_ptrs[b] = POLY_XFB_OFFS + 4 * b;          /* where the buffer's write offset lives */
   }
params_done:;
   unsigned prims = gp->grid[0];
   uint32_t indbuf[64] = { 0 };
   scene_indirect_args(indbuf);
   c.ind = wbuf(c.queue, indbuf, 256, WGPUBufferUsage_Storage | WGPUBufferUsage_Indirect | WGPUBufferUsage_CopySrc);
   if (getenv("SCENE_DUMP"))
      fprintf(stderr, "poly: shape %u mode %u max_indices %u; grid %u %u; draw %u %u %u; input_primitives %u; vs_outputs 0x%llx\n",
              gsi.shape, gsi.mode, gsi.max_indices, gp->grid[0], gp->grid[1], gp->draw.vertex_count,
              gp->draw.instance_count, gp->draw.first_vertex, gp->input_primitives, (unsigned long long)vs_outputs);
   WGPUBufferDescriptor pbd = { .size = POLY_SIZE, .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Index | WGPUBufferUsage_CopySrc |
                                WGPUBufferUsage_Indirect | WGPUBufferUsage_CopyDst };
   c.poly = wgpuDeviceCreateBuffer(wdev, &pbd);
   wg_poly_buf = c.poly;
   wgpuQueueWriteBuffer(c.queue, c.poly, 0, poly, POLY_SIZE);
   uint32_t du[256];
   driver_uniforms(du);
   du[25] = POLY_VP;
   du[26] = POLY_GP;
   du[27] = POLY_SINK;
   du[28] = POLY_SETUP;
   du[29] = POLY_HEAP;
   du[30] = 0;
   du[31] = IND_DISPATCH;
   if (ind) {
      /* draw parameters from the draw command on the GPU (the setup writes them): none from the CPU */
      du[35] = POLY_DRAW_PARAMS;
      du[0] = du[1] = du[2] = 0xdead;
   }
   c.du = wbuf(c.queue, du, 1024, WGPUBufferUsage_Uniform);
   for (unsigned b = 0; b < 8; b++)
      c.vbuf[b] = sc.vb[b].used ? wbuf(c.queue, vb_data[b], SCENE_VB_SIZE, WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage) : NULL;
   uint16_t idx16[256];
   for (unsigned i = 0; i < 256; i++)
      idx16[i] = idx_data[i];
   c.ibuf = wbuf(c.queue, sc.index_bits == 16 ? (void *)idx16 : (void *)idx_data, 1024, WGPUBufferUsage_Storage);
   WGPUSamplerDescriptor sd = { .addressModeU = WGPUAddressMode_ClampToEdge, .addressModeV = WGPUAddressMode_ClampToEdge,
                                .addressModeW = WGPUAddressMode_ClampToEdge, .magFilter = WGPUFilterMode_Nearest,
                                .minFilter = WGPUFilterMode_Nearest, .mipmapFilter = WGPUMipmapFilterMode_Nearest,
                                .lodMaxClamp = 16, .maxAnisotropy = 1 };
   c.smp = wgpuDeviceCreateSampler(wdev, &sd);
   uint32_t imm[16] = { 0 };
   memcpy(imm, push_data, sc.push_bytes < 64 ? sc.push_bytes : 64);
   if (pv->draw_params_offset != 0xffff && pv->draw_params_offset + 12 <= 64)
      memcpy((uint8_t *)imm + pv->draw_params_offset, du, 12);

   WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(wdev, NULL);
   /* an indirect draw: the setup part first, then every part dispatched from the counts it wrote */
   if (ind && gs_dispatch(&c, enc, gs_setup, gs_setup_prog, 1, 1, imm))
      goto fail;
   unsigned gsx = ind ? ~0u : (prims + 63) / 64, gsy = ind ? IND_DISPATCH + 12 : sc.instances;
   /* the vertex shader as compute, then the GS parts */
   if (gs_dispatch(&c, enc, vs, pv, ind ? ~0u : (sc.count + 63) / 64, ind ? IND_DISPATCH : sc.instances, imm))
      goto fail;
   if (vsc)
      goto rast;
   if (gs_parts[2] && gs_dispatch(&c, enc, gs_parts[2], pg[2], gsx, gsy, imm))
      goto fail;
   if (gs_parts[5] && gs_dispatch(&c, enc, gs_parts[5], pg[5], 1, 1, imm))
      goto fail;
   if (gs_parts[3] && gs_dispatch(&c, enc, gs_parts[3], pg[3], 1, 1, imm))
      goto fail;
   if (gs_parts[1] && gs_dispatch(&c, enc, gs_parts[1], pg[1], gsx, gsy, imm))
      goto fail;
   /* transform feedback: one invocation per output vertex of the rasterization draw */
   if (gs_parts[4] && gs_dispatch(&c, enc, gs_parts[4], pg[4], ind ? ~0u : (gp->draw.index_count + 63) / 64,
                                  ind ? IND_DISPATCH + 24 : gp->draw.instance_count, imm))
      goto fail;

   /* the rast part */
rast:;
   /* WebGPU wants writeMask 0 for a target the fragment shader does not write (Vulkan ignores it): T2 */
   WGPUColorTargetState ct = { .format = WGPUTextureFormat_RGBA8Unorm,
                               .writeMask = strstr(fs, "@location(0) c0") ? WGPUColorWriteMask_All : WGPUColorWriteMask_None };
   WGPUFragmentState fst = { .module = wmodule(fs), .entryPoint = sv("main"), .targetCount = 1, .targets = &ct };
   bool indexed = !vsc && poly_gs_indexed(gsi.shape);
   unsigned mode = vsc ? (sc.topology == 0 ? MESA_PRIM_POINTS : sc.topology == 1 ? MESA_PRIM_LINES :
                          sc.topology == 2 ? MESA_PRIM_LINE_STRIP : sc.topology == 4 ? MESA_PRIM_TRIANGLE_STRIP :
                          MESA_PRIM_TRIANGLES) : gsi.mode;
   WGPUPrimitiveTopology topo = mode == MESA_PRIM_POINTS ? WGPUPrimitiveTopology_PointList :
                                mode == MESA_PRIM_LINES ? WGPUPrimitiveTopology_LineList :
                                mode == MESA_PRIM_LINE_STRIP ? WGPUPrimitiveTopology_LineStrip :
                                mode == MESA_PRIM_TRIANGLES ? WGPUPrimitiveTopology_TriangleList :
                                WGPUPrimitiveTopology_TriangleStrip;
   bool strip = topo == WGPUPrimitiveTopology_LineStrip || topo == WGPUPrimitiveTopology_TriangleStrip;
   WGPURenderPipelineDescriptor rpd = {
      .vertex = { .module = wmodule(vsc ? vsc_rast : gs_parts[0]), .entryPoint = sv("main") },
      .primitive = { .topology = topo, .stripIndexFormat = indexed && strip ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Undefined,
                     .cullMode = WGPUCullMode_None, .frontFace = WGPUFrontFace_CCW },
      .multisample = { .count = 1, .mask = ~0u },
      .fragment = &fst,
   };
   WGPURenderPipeline rp = wgpuDeviceCreateRenderPipeline(wdev, &rpd);
   wgpuInstanceProcessEvents(winst);
   if (werror)
      goto fail;
   WGPUBindGroupEntry be[4][2 * MAX_RES];
   unsigned nbe[4];
   struct prog *rp_progs[2] = { vsc ? vsc_rast_prog : pg[0], pf };
   if (gs_entries(&c, rp_progs, 2, be, nbe))
      goto fail;
   WGPUTextureDescriptor rtd = { .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
                                 .dimension = WGPUTextureDimension_2D, .size = { W, H, 1 },
                                 .format = WGPUTextureFormat_RGBA8Unorm, .mipLevelCount = 1, .sampleCount = 1 };
   WGPUTexture rt = wgpuDeviceCreateTexture(wdev, &rtd);
   WGPUBufferDescriptor rbd = { .size = W * H * 4, .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst };
   WGPUBuffer rb = wgpuDeviceCreateBuffer(wdev, &rbd);
   WGPURenderPassColorAttachment rca = { .view = wgpuTextureCreateView(rt, NULL), .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                                         .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store,
                                         .clearValue = { 0, 0, 0, 0 } };
   WGPURenderPassDescriptor rpass = { .colorAttachmentCount = 1, .colorAttachments = &rca };
   WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rpass);
   wgpuRenderPassEncoderSetPipeline(pass, rp);
   for (unsigned g = 0; g < 4; g++) {
      if (!nbe[g])
         continue;
      WGPUBindGroupDescriptor bgd = { .layout = wgpuRenderPipelineGetBindGroupLayout(rp, g), .entryCount = nbe[g], .entries = be[g] };
      wgpuRenderPassEncoderSetBindGroup(pass, g, wgpuDeviceCreateBindGroup(wdev, &bgd), 0, NULL);
   }
   if (strstr(vsc ? vsc_rast : gs_parts[0], "var<immediate>") || strstr(fs, "var<immediate>"))
      wgpuRenderPassEncoderSetImmediates(pass, 0, imm, 64);
   /* the draw command libpoly keeps in the geometry params (vertex/index count, instances, first) */
   uint64_t draw_off = POLY_GP + offsetof(struct poly_geometry_params, draw);
   if (vsc) {
      /* the passthrough reads outputs[instance * count + vertex] */
      wgpuRenderPassEncoderDraw(pass, sc.count, sc.instances, 0, 0);
   } else if (indexed) {
      wgpuRenderPassEncoderSetIndexBuffer(pass, c.poly, WGPUIndexFormat_Uint32, 0, POLY_SIZE);
      wgpuRenderPassEncoderDrawIndexedIndirect(pass, c.poly, draw_off);
   } else {
      wgpuRenderPassEncoderDrawIndirect(pass, c.poly, draw_off);
   }
   wgpuRenderPassEncoderEnd(pass);
   WGPUTexelCopyTextureInfo src = { .texture = rt };
   WGPUTexelCopyBufferInfo dstb = { .layout = { .bytesPerRow = W * 4, .rowsPerImage = H }, .buffer = rb };
   WGPUExtent3D rext = { W, H, 1 };
   wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dstb, &rext);
   WGPUBuffer dbg = NULL;
   if (getenv("SCENE_DUMP")) {
      WGPUBufferDescriptor dd = { .size = 4096, .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst };
      dbg = wgpuDeviceCreateBuffer(wdev, &dd);
      wgpuCommandEncoderCopyBufferToBuffer(enc, c.poly, 0, dbg, 0, 2048);
      wgpuCommandEncoderCopyBufferToBuffer(enc, c.ind, 0, dbg, 2048, 256);
   }
   WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
   wgpuQueueSubmit(c.queue, 1, &cmd);
   if (dbg) {
      wmapped = 0;
      wgpuBufferMapAsync(dbg, WGPUMapMode_Read, 0, 4096, (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map });
      for (time_t t0 = time(NULL); !wmapped && time(NULL) - t0 < 30;)
         wgpuInstanceProcessEvents(winst);
      const uint8_t *m = wgpuBufferGetConstMappedRange(dbg, 0, 4096);
      if (m) {
         const struct poly_vertex_params *dvp = (const void *)(m + POLY_VP);
         const struct poly_geometry_params *dgp = (const void *)(m + POLY_GP);
         const uint32_t *di = (const void *)(m + 2048);
         const struct poly_heap *dh = (const void *)(m + POLY_HEAP);
         fprintf(stderr, "after: vp grid %u %u verts %u out %llx ib %llx size %u; gp grid %u %u draw %u %u %u prims %u log2 %u oib %llx; "
                 "dispatch %u %u %u / %u %u %u; heap bottom %u\n", dvp->grid[0], dvp->grid[1], dvp->verts_per_instance,
                 (unsigned long long)dvp->output_buffer, (unsigned long long)dvp->index_buffer, dvp->index_size_B,
                 dgp->grid[0], dgp->grid[1], dgp->draw.index_count, dgp->draw.instance_count, dgp->draw.first_index,
                 dgp->input_primitives, dgp->primitives_log2, (unsigned long long)dgp->output_index_buffer,
                 di[8], di[9], di[10], di[11], di[12], di[13], dh->bottom);
      }
   }
   wmapped = 0;
   wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, W * H * 4, (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map });
   for (time_t t0 = time(NULL); !wmapped && time(NULL) - t0 < 300;)
      wgpuInstanceProcessEvents(winst);
   if (wmapped != 1 || werror)
      goto fail;
   memcpy(out, wgpuBufferGetConstMappedRange(rb, 0, W * H * 4), W * H * 4);
   return 0;
fail:
   fprintf(stderr, "dawn: %s\n", werrmsg);
   return -1;
}


/* ------------------------------------------------------------------ Dawn, with tessellation */

#define POLY_TESS 1536
#define TESS_TCS (1u << 20)
#define TESS_ALLOCS (TESS_TCS + (512u << 10))
#define TESS_COUNTS (TESS_ALLOCS + (64u << 10))
#define TESS_DRAW (TESS_COUNTS + (64u << 10))
#define TESS_SCRATCH (2u << 20)

struct tess_files {
   const char *tcs, *tes, *k[3];          /* WGSL: TCS (compute), TES (rast), kernels count/prefix/tess */
   struct prog *ptcs, *ptes, *pk[3];
   unsigned tcs_vertices_out, nr_patch_outputs, output_stride, domain, spacing, ccw, point_mode;
   unsigned long long per_vertex_outputs;
   unsigned scratch;                      /* bytes per tessellator invocation */
};

static int
wgpu_draw_tess(const char *vs, const char *fs, struct prog *pv, struct prog *pf, struct tess_files *t, uint8_t *out)
{
   werror = 0;
   struct gs_ctx c = { .queue = wgpuDeviceGetQueue(wdev) };
   static uint8_t poly[POLY_SIZE];
   memset(poly, 0, sizeof(poly));
   struct poly_vertex_params *vp = (void *)(poly + POLY_VP);
   static const uint32_t wg[3] = { 64, 1, 1 };
   extern uint64_t scene_vs_outputs;
   poly_vertex_params_init(vp, scene_vs_outputs, wg);
   poly_vertex_params_set_draw(vp, sc.count, sc.instances);
   vp->output_buffer = POLY_VS_OUT;
   unsigned in_patches = sc.count / sc.patch_size, nr = in_patches * sc.instances;
   struct poly_heap *heap = (void *)(poly + POLY_HEAP);
   heap->base = 0;
   heap->bottom = POLY_HEAP_BASE;
   heap->size = TESS_TCS;
   struct poly_tess_params *tp = (void *)(poly + POLY_TESS);
   *tp = (struct poly_tess_params){
      .heap = POLY_HEAP,
      .patch_coord_buffer = 0,       /* the heap's base: coordinates are allocated in the heap */
      .coord_allocs = TESS_ALLOCS,
      .out_draws = TESS_DRAW,
      .tcs_buffer = TESS_TCS,
      .counts = TESS_COUNTS,
      .tcs_per_vertex_outputs = t->per_vertex_outputs,
      .input_patch_size = sc.patch_size,
      .output_patch_size = t->tcs_vertices_out,
      .tcs_patch_constants = t->nr_patch_outputs,
      .patches_per_instance = in_patches,
      .tcs_stride_el = t->output_stride / 4,
      .nr_patches = nr,
      .partitioning = t->spacing == TESS_SPACING_EQUAL ? POLY_TESS_PARTITIONING_INTEGER :
                      t->spacing == TESS_SPACING_FRACTIONAL_ODD ? POLY_TESS_PARTITIONING_FRACTIONAL_ODD :
                      POLY_TESS_PARTITIONING_FRACTIONAL_EVEN,
      .points_mode = t->point_mode,
      .isolines = t->domain == TESS_PRIMITIVE_ISOLINES,
      .ccw = !t->point_mode && t->domain != TESS_PRIMITIVE_ISOLINES ? t->ccw : 0,
   };
   WGPUBufferDescriptor pbd = { .size = POLY_SIZE, .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Index |
                                WGPUBufferUsage_CopySrc | WGPUBufferUsage_Indirect | WGPUBufferUsage_CopyDst };
   c.poly = wgpuDeviceCreateBuffer(wdev, &pbd);
   wg_poly_buf = c.poly;
   wgpuQueueWriteBuffer(c.queue, c.poly, 0, poly, POLY_SIZE);
   uint32_t du[256];
   driver_uniforms(du);
   du[25] = POLY_VP;
   du[27] = POLY_SINK;
   du[32] = POLY_TESS;
   du[33] = TESS_SCRATCH;
   if (t->scratch * ((nr + 63) / 64 * 64) > POLY_SIZE - TESS_SCRATCH) {
      fprintf(stderr, "scene-run: tessellator scratch too large\n");
      return -1;
   }
   c.du = wbuf(c.queue, du, 1024, WGPUBufferUsage_Uniform);
   for (unsigned b = 0; b < 8; b++)
      c.vbuf[b] = sc.vb[b].used ? wbuf(c.queue, vb_data[b], SCENE_VB_SIZE, WGPUBufferUsage_Vertex | WGPUBufferUsage_Storage) : NULL;
   uint16_t idx16[256];
   for (unsigned i = 0; i < 256; i++)
      idx16[i] = idx_data[i];
   c.ibuf = wbuf(c.queue, sc.index_bits == 16 ? (void *)idx16 : (void *)idx_data, 1024, WGPUBufferUsage_Storage);
   uint32_t imm[16] = { 0 };
   memcpy(imm, push_data, sc.push_bytes < 64 ? sc.push_bytes : 64);
   if (pv->draw_params_offset != 0xffff && pv->draw_params_offset + 12 <= 64)
      memcpy((uint8_t *)imm + pv->draw_params_offset, du, 12);

   WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(wdev, NULL);
   /* the vertex shader, the TCS (a workgroup per patch), the tessellator: counts, prefix sum, tessellate */
   if (gs_dispatch(&c, enc, vs, pv, (sc.count + 63) / 64, sc.instances, imm) ||
       gs_dispatch(&c, enc, t->tcs, t->ptcs, in_patches, sc.instances, imm) ||
       gs_dispatch(&c, enc, t->k[0], t->pk[0], (nr + 63) / 64, 1, imm) ||
       gs_dispatch(&c, enc, t->k[1], t->pk[1], 1, 1, imm) ||
       gs_dispatch(&c, enc, t->k[2], t->pk[2], (nr + 63) / 64, 1, imm))
      goto fail;

   /* the TES draws, from the index buffer and draw command the prefix sum made */
   WGPUColorTargetState ct = { .format = WGPUTextureFormat_RGBA8Unorm,
                               .writeMask = strstr(fs, "@location(0) c0") ? WGPUColorWriteMask_All : WGPUColorWriteMask_None };
   WGPUFragmentState fst = { .module = wmodule(fs), .entryPoint = sv("main"), .targetCount = 1, .targets = &ct };
   WGPURenderPipelineDescriptor rpd = {
      .vertex = { .module = wmodule(t->tes), .entryPoint = sv("main") },
      .primitive = { .topology = t->point_mode ? WGPUPrimitiveTopology_PointList :
                                 t->domain == TESS_PRIMITIVE_ISOLINES ? WGPUPrimitiveTopology_LineList :
                                 WGPUPrimitiveTopology_TriangleList,
                     .cullMode = WGPUCullMode_None, .frontFace = WGPUFrontFace_CCW },
      .multisample = { .count = 1, .mask = ~0u },
      .fragment = &fst,
   };
   WGPURenderPipeline rp = wgpuDeviceCreateRenderPipeline(wdev, &rpd);
   wgpuInstanceProcessEvents(winst);
   if (werror)
      goto fail;
   WGPUBindGroupEntry be[4][2 * MAX_RES];
   unsigned nbe[4];
   struct prog *rp_progs[2] = { t->ptes, pf };
   if (gs_entries(&c, rp_progs, 2, be, nbe))
      goto fail;
   WGPUTextureDescriptor rtd = { .usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc,
                                 .dimension = WGPUTextureDimension_2D, .size = { W, H, 1 },
                                 .format = WGPUTextureFormat_RGBA8Unorm, .mipLevelCount = 1, .sampleCount = 1 };
   WGPUTexture rt = wgpuDeviceCreateTexture(wdev, &rtd);
   WGPUBufferDescriptor rbd = { .size = W * H * 4, .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst };
   WGPUBuffer rb = wgpuDeviceCreateBuffer(wdev, &rbd);
   WGPURenderPassColorAttachment rca = { .view = wgpuTextureCreateView(rt, NULL), .depthSlice = WGPU_DEPTH_SLICE_UNDEFINED,
                                         .loadOp = WGPULoadOp_Clear, .storeOp = WGPUStoreOp_Store,
                                         .clearValue = { 0, 0, 0, 0 } };
   WGPURenderPassDescriptor rpass = { .colorAttachmentCount = 1, .colorAttachments = &rca };
   WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rpass);
   wgpuRenderPassEncoderSetPipeline(pass, rp);
   for (unsigned g = 0; g < 4; g++) {
      if (!nbe[g])
         continue;
      WGPUBindGroupDescriptor bgd = { .layout = wgpuRenderPipelineGetBindGroupLayout(rp, g), .entryCount = nbe[g], .entries = be[g] };
      wgpuRenderPassEncoderSetBindGroup(pass, g, wgpuDeviceCreateBindGroup(wdev, &bgd), 0, NULL);
   }
   if (strstr(t->tes, "var<immediate>") || strstr(fs, "var<immediate>"))
      wgpuRenderPassEncoderSetImmediates(pass, 0, imm, 64);
   wgpuRenderPassEncoderSetIndexBuffer(pass, c.poly, WGPUIndexFormat_Uint32, 0, POLY_SIZE);
   wgpuRenderPassEncoderDrawIndexedIndirect(pass, c.poly, TESS_DRAW);
   wgpuRenderPassEncoderEnd(pass);
   WGPUTexelCopyTextureInfo src = { .texture = rt };
   WGPUTexelCopyBufferInfo dstb = { .layout = { .bytesPerRow = W * 4, .rowsPerImage = H }, .buffer = rb };
   WGPUExtent3D rext = { W, H, 1 };
   wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dstb, &rext);
   WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
   wgpuQueueSubmit(c.queue, 1, &cmd);
   wmapped = 0;
   wgpuBufferMapAsync(rb, WGPUMapMode_Read, 0, W * H * 4, (WGPUBufferMapCallbackInfo){ .mode = WGPUCallbackMode_AllowProcessEvents, .callback = on_map });
   for (time_t t0 = time(NULL); !wmapped && time(NULL) - t0 < 300;)
      wgpuInstanceProcessEvents(winst);
   if (wmapped != 1 || werror)
      goto fail;
   memcpy(out, wgpuBufferGetConstMappedRange(rb, 0, W * H * 4), W * H * 4);
   if (getenv("SCENE_DUMP")) {
      uint32_t d[5];
      if (!buffer_readback_at(c.poly, TESS_DRAW, 20, d))
         fprintf(stderr, "tess draw: %u indices, %u instances, first %u\n", d[0], d[1], d[2]);
   }
   return 0;
fail:
   fprintf(stderr, "dawn: %s\n", werrmsg);
   return -1;
}

uint64_t scene_vs_outputs;

int
main(int argc, char **argv)
{
   if (vk_init() || wgpu_init()) {
      fprintf(stderr, "no Vulkan or Dawn device\n");
      return 1;
   }
   int pass = 0, fail_n = 0;
   static uint8_t ref[W * H * 4], got[W * H * 4];
   for (int a = 1; a < argc; a++) {
      const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
      char p[1100];
      snprintf(p, sizeof(p), "%s.scene", argv[a]);
      if (!scene_load(p, &sc)) {
         printf("FAIL %s: no scene\n", name);
         fail_n++;
         continue;
      }
      fill();
      long vss, fss;
      snprintf(p, sizeof(p), "%s-vert.spv", argv[a]);
      uint32_t *vs = slurp(p, &vss);
      snprintf(p, sizeof(p), "%s-frag.spv", argv[a]);
      uint32_t *fs = slurp(p, &fss);
      snprintf(p, sizeof(p), "%s-vert.wgsl", argv[a]);
      char *vw = slurp(p, NULL);
      snprintf(p, sizeof(p), "%s-frag.wgsl", argv[a]);
      char *fw = slurp(p, NULL);
      snprintf(p, sizeof(p), "%s-vert.bind", argv[a]);
      char *vb = slurp(p, NULL);
      snprintf(p, sizeof(p), "%s-frag.bind", argv[a]);
      char *fb = slurp(p, NULL);
      if (!vs || !fs || !vw || !fw) {
         printf("FAIL %s: missing spv or wgsl (see the .err files)\n", name);
         fail_n++;
         continue;
      }
      static struct prog pv, pf, pg[6];
      parse_bind(vb ? vb : "", &pv);
      parse_bind(fb ? fb : "", &pf);
      mark_combined(&pv);
      mark_combined(&pf);
      /* a geometry shader, through libpoly on the WebGPU side */
      snprintf(p, sizeof(p), "%s-geom.spv", argv[a]);
      gs_spv = slurp(p, &gs_size);   /* none: transform feedback's passthrough GS (WebGPU side only) */
      const char *gs_parts[6] = { NULL };
      pgs = NULL;
      snprintf(p, sizeof(p), "%s-geom.gs", argv[a]);
      FILE *gsf = fopen(p, "r");
      bool gs_mode = gsf != NULL;
      if (gsf)
         fclose(gsf);
      if (gs_mode) {
         static const char *pn[] = { "", ".gs-main", ".gs-count", ".gs-pre", ".gs-xfb", ".gs-prefix" };
         for (unsigned i = 0; i < 6; i++) {
            snprintf(p, sizeof(p), "%s-geom%s.wgsl", argv[a], pn[i]);
            gs_parts[i] = slurp(p, NULL);
            snprintf(p, sizeof(p), "%s-geom%s.bind", argv[a], pn[i]);
            char *bt = slurp(p, NULL);
            parse_bind(bt ? bt : "", &pg[i]);
            mark_combined(&pg[i]);
         }
         pgs = &pg[0];
         static struct prog psetup;
         snprintf(p, sizeof(p), "%s-geom.gs-setup.wgsl", argv[a]);
         gs_setup = slurp(p, NULL);
         snprintf(p, sizeof(p), "%s-geom.gs-setup.bind", argv[a]);
         char *sbt = slurp(p, NULL);
         parse_bind(sbt ? sbt : "", &psetup);
         gs_setup_prog = &psetup;
         snprintf(p, sizeof(p), "%s-geom.gs", argv[a]);
         char *gt = slurp(p, NULL);
         memset(&gsi, 0, sizeof(gsi));
         if (!gt || !gs_parts[0] ||
             sscanf(gt, "shape %u mode %u max_indices %u input %u count_words %u prefix_sum %u", &gsi.shape, &gsi.mode,
                    &gsi.max_indices, &gsi.input, &gsi.count_words, &gsi.prefix_sum) != 6) {
            printf("FAIL %s: no geometry shader parts\n", name);
            fail_n++;
            continue;
         }
         const char *tp = strstr(gt, "topology");
         for (unsigned i = 0; tp && i < 64; i++) {
            tp = strchr(tp, ' ');
            if (!tp)
               break;
            gsi.topology[i] = strtoul(++tp, NULL, 10);
         }
         const char *vo = vb ? strstr(vb, "vs_outputs ") : NULL;
         scene_vs_outputs = vo ? strtoull(vo + 11, NULL, 0) : 0;
      }
      /* tessellation */
      static struct tess_files tf;
      static struct prog ptcs, ptes, pk[3];
      bool tess_mode = false;
      snprintf(p, sizeof(p), "%s-tesc.spv", argv[a]);
      tcs_spv = slurp(p, &tcs_size);
      snprintf(p, sizeof(p), "%s-tese.spv", argv[a]);
      tes_spv = slurp(p, &tes_size);
      if (tcs_spv && tes_spv) {
         tess_mode = true;
         memset(&tf, 0, sizeof(tf));
         snprintf(p, sizeof(p), "%s-tesc.wgsl", argv[a]);
         tf.tcs = slurp(p, NULL);
         snprintf(p, sizeof(p), "%s-tese.wgsl", argv[a]);
         tf.tes = slurp(p, NULL);
         static const char *kn[] = { "count", "prefix", "tess" };
         struct prog *pks[3] = { &pk[0], &pk[1], &pk[2] };
         for (unsigned i = 0; i < 3; i++) {
            snprintf(p, sizeof(p), "%s-tese.tess-%s.wgsl", argv[a], kn[i]);
            tf.k[i] = slurp(p, NULL);
            snprintf(p, sizeof(p), "%s-tese.tess-%s.bind", argv[a], kn[i]);
            char *kb = slurp(p, NULL);
            parse_bind(kb ? kb : "", pks[i]);
            tf.pk[i] = pks[i];
            const char *sp = kb ? strstr(kb, " scratch ") : NULL;
            if (sp && i == 2)
               tf.scratch = atoi(sp + 9);
            if (sp && i == 0 && atoi(sp + 9) > (int)tf.scratch)
               tf.scratch = atoi(sp + 9);
         }
         snprintf(p, sizeof(p), "%s-tesc.bind", argv[a]);
         char *tb = slurp(p, NULL);
         parse_bind(tb ? tb : "", &ptcs);
         snprintf(p, sizeof(p), "%s-tese.bind", argv[a]);
         tb = slurp(p, NULL);
         parse_bind(tb ? tb : "", &ptes);
         mark_combined(&ptes);
         tf.ptcs = &ptcs;
         tf.ptes = &ptes;
         snprintf(p, sizeof(p), "%s-tesc.tess", argv[a]);
         char *tt = slurp(p, NULL);
         if (tt)
            sscanf(tt, "tcs_vertices_out %u nr_patch_outputs %u output_stride %u per_vertex_outputs %llx",
                   &tf.tcs_vertices_out, &tf.nr_patch_outputs, &tf.output_stride, &tf.per_vertex_outputs);
         snprintf(p, sizeof(p), "%s-tese.tess", argv[a]);
         tt = slurp(p, NULL);
         const char *dp = tt ? strstr(tt, "domain ") : NULL;
         if (dp)
            sscanf(dp, "domain %u spacing %u ccw %u point_mode %u", &tf.domain, &tf.spacing, &tf.ccw, &tf.point_mode);
         const char *vo = vb ? strstr(vb, "vs_outputs ") : NULL;
         scene_vs_outputs = vo ? strtoull(vo + 11, NULL, 0) : 0;
         if (!tf.tcs || !tf.tes || !tf.k[0] || !tf.k[1] || !tf.k[2]) {
            printf("FAIL %s: tessellation parts missing\n", name);
            fail_n++;
            continue;
         }
      }
      snprintf(p, sizeof(p), "%s-vert.vs-compute.wgsl", argv[a]);
      char *vcw = slurp(p, NULL);
      static struct prog pvc;
      vsc_rast = NULL;
      struct prog *pv_vk = &pv;
      if (vcw && !gs_mode) {
         snprintf(p, sizeof(p), "%s-vert.vs-compute.bind", argv[a]);
         char *vcb = slurp(p, NULL);
         parse_bind(vcb ? vcb : "", &pvc);
         const char *vo = vcb ? strstr(vcb, "vs_outputs ") : NULL;
         scene_vs_outputs = vo ? strtoull(vo + 11, NULL, 0) : 0;
         vsc_rast = vw;
         vsc_rast_prog = &pv;
         pv_vk = &pvc;   /* Vulkan's descriptor sets: the compute part has the app bindings */
         /* the Vulkan side's descriptor sets: the compute part's app bindings (the passthrough has none) */
      }
      memset(ref, 0, sizeof(ref));
      memset(got, 0xcd, sizeof(got));
      if (vk_draw(vs, vss, fs, fss, pv_vk, &pf, ref)) {
         printf("FAIL %s: vulkan\n", name);
         fail_n++;
         continue;
      }
      if (tess_mode ? wgpu_draw_tess(vw, fw, &pv, &pf, &tf, got) :
          vsc_rast ? wgpu_draw_gs(vcw, fw, (const char *[6]){ NULL }, &pvc, &pf, (struct prog *[6]){ NULL }, got) :
          gs_mode ? wgpu_draw_gs(vw, fw, gs_parts, &pv, &pf, (struct prog *[6]){ &pg[0], &pg[1], &pg[2], &pg[3], &pg[4], &pg[5] }, got) :
                   wgpu_draw(vw, fw, &pv, &pf, got)) {
         printf("FAIL %s: dawn\n", name);
         fail_n++;
         continue;
      }
      if (getenv("SCENE_DUMP")) {
         /* NAME-vk.ppm and NAME-wg.ppm (rgb) next to the inputs */
         for (unsigned k = 0; k < 2; k++) {
            snprintf(p, sizeof(p), "%s-%s.ppm", argv[a], k ? "wg" : "vk");
            FILE *pf2 = fopen(p, "wb");
            if (!pf2)
               continue;
            fprintf(pf2, "P6\n%u %u\n255\n", W, H);
            for (unsigned i = 0; i < W * H; i++)
               fwrite(&(k ? got : ref)[i * 4], 1, 3, pf2);
            fclose(pf2);
         }
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
      /* an image used atomically: compared texel for texel */
      unsigned adiff = 0, awritten = 0;
      char atext[256] = "";
      if (atlas_used && atlas_wg) {
         static uint32_t wa[16 / 4 + ATLAS_W * ATLAS_H * ATLAS_LAYERS];
         const uint32_t *va = atlas_vkmap;
         if (atlas_readback(wa)) {
            adiff = ~0u;
         } else {
            for (unsigned i = 4; i < 4 + ATLAS_W * ATLAS_H * ATLAS_LAYERS; i++) {
               awritten += va[i] != 0xffffffffu;
               if (va[i] != wa[i] && adiff++ < 4)
                  printf("    atlas word %u: vulkan 0x%08x wgsl 0x%08x\n", i - 4, va[i], wa[i]);
            }
         }
         snprintf(atext, sizeof(atext), "; atomic image: %u texels differ, %u written", adiff, awritten);
      }
      /* transform feedback buffers: word for word */
      unsigned xdiff = 0, xwritten = 0;
      for (unsigned b = 0; b < 4; b++) {
         if (!sc.xfb_size[b] || !xfb_wg[b])
            continue;
         static uint32_t wx[1 << 16];
         unsigned words = sc.xfb_size[b] / 4;
         if (buffer_readback(xfb_wg[b], sc.xfb_size[b], wx)) {
            xdiff = ~0u;
            continue;
         }
         const uint32_t *vx = xfb_vkmap[b];
         for (unsigned i = 0; i < words; i++) {
            xwritten += vx[i] != 0;
            if (vx[i] != wx[i] && xdiff++ < 6)
               printf("    xfb %u word %u: vulkan 0x%08x wgsl 0x%08x\n", b, i, vx[i], wx[i]);
         }
      }
      /* libpoly's counters against the Vulkan query (stream 0) */
      if (query_done && wg_poly_buf) {
         uint32_t qw[2];
         if (buffer_readback_at(wg_poly_buf, POLY_QUERY, 8, qw) == 0) {
            bool same = qw[0] == vk_query[0] && qw[1] == vk_query[1];
            snprintf(atext + strlen(atext), sizeof(atext) - strlen(atext), "; query written/needed: vulkan %llu/%llu wgsl %u/%u",
                     (unsigned long long)vk_query[0], (unsigned long long)vk_query[1], qw[0], qw[1]);
            if (!same)
               xdiff++;
         }
      }
      if (xwritten || xdiff)
         snprintf(atext + strlen(atext), sizeof(atext) - strlen(atext), "; xfb: %u words differ, %u written", xdiff, xwritten);
      bool bad = exact || adiff || xdiff || (!lit && !awritten && !xwritten);
      printf("%s %s (%u pixels differ, %u off by one, %u of %u drawn%s)\n", bad ? "FAIL" : "ok", name, exact,
             off1, lit, W * H, atext);
      bad ? fail_n++ : pass++;
      atlas_used = false;
      atlas_wg = NULL;
   }
   printf("scene-run: %d ok, %d failed\n", pass, fail_n);
   return fail_n != 0;
}
