/* virglrenderer's config.h for vkr built inside Mesa's tree (T5; gpu/webgpu/vkr). Only venus: no vrend, no DRM
 * contexts, no render server, no tracing; Vulkan comes from the vkGetInstanceProcAddr rvgpu hands vkr. */
#ifndef VKR_RVGPU_CONFIG_H
#define VKR_RVGPU_CONFIG_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define VERSION "1.2.0-rvgpu"
#define VIRGL_RENDERER_UNSTABLE_APIS 1
#define ENABLE_VENUS 1
#define HAVE_DLFCN_H 1
#define HAVE_PTHREAD 1
#define HAVE_SYS_UIO_H 1

/* virglrenderer's copy of Mesa's util/list.h has it; Mesa's own no longer does */
#include <stddef.h>
#define LIST_ENTRY(__type, __item, __field) ((__type *)(((char *)(__item)) - offsetof(__type, __field)))
#endif
