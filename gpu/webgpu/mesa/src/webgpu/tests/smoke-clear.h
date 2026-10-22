/* The clear smoke test (smoke-clear.c), callable from the page host too. */
#ifndef SMOKE_CLEAR_H
#define SMOKE_CLEAR_H
#include <vulkan/vulkan.h>
struct smoke {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	VkImage image;		/* the 64x64 image, kept */
	const void *pixels;	/* smoke_draw: its pixels, mapped */
	char device_name[256];
	char msg[600];
};
/* 0 = every pixel right, 1 = wrong pixels, -1 = a Vulkan call failed (s->msg says which). */
int smoke_clear(struct smoke *s, PFN_vkGetInstanceProcAddr gipa, const float rgba[4], const char *device_prefix);
#endif
