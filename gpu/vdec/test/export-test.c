/* SPDX-License-Identifier: MIT */
/*
 * Zero copy's plumbing without a player: libva on one open of the render node, our driver exports a surface
 * (vaExportSurfaceHandle: two GPU resources, R8 and GR88, as PRIME descriptors), and a second open of the device
 * (as Mesa's EGL is, in a player) imports them (PRIME_FD_TO_HANDLE: rv/virtgpu.c asks the server, which attaches
 * the resource to that connection's context) and finds the same resources (RESOURCE_INFO).
 *   export-test [DEVICE]       (under rv, RV_VIRTGPU set; LIBVA_DRIVER_NAME=rvwasm)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

struct vg_prime { uint32_t handle, flags; int32_t fd; };
struct vg_resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct vg_gem_close { uint32_t handle, pad; };
#define DRM_IOCTL_PRIME_FD_TO_HANDLE _IOWR('d', 0x2e, struct vg_prime)
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO _IOWR('d', 0x40 + 0x05, struct vg_resource_info)
#define DRM_IOCTL_GEM_CLOSE _IOW('d', 0x09, struct vg_gem_close)

static int fails;
#define CHECK(what, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); fails += !ok_; } while (0)

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	int fd1 = open(dev, O_RDWR), fd2 = open(dev, O_RDWR), major, minor, n = 0;
	VADisplay dpy;
	VAProfile profiles[16];
	VAConfigID cfg;
	VASurfaceID surf;
	VAContextID ctx;
	VADRMPRIMESurfaceDescriptor d;
	VAStatus st;

	CHECK("two opens of the render node", fd1 >= 0 && fd2 >= 0);
	dpy = vaGetDisplayDRM(fd1);
	CHECK("vaGetDisplayDRM", dpy != NULL);
	st = vaInitialize(dpy, &major, &minor);
	CHECK("vaInitialize (our driver, the page's decoder there)", st == VA_STATUS_SUCCESS);
	if (st)
		return 1;
	printf("     vendor: %s\n", vaQueryVendorString(dpy));
	vaQueryConfigProfiles(dpy, profiles, &n);
	CHECK("profiles offered", n > 0);
	CHECK("vaCreateConfig H.264 High", vaCreateConfig(dpy, VAProfileH264High, VAEntrypointVLD, NULL, 0, &cfg) == 0);
	CHECK("vaCreateSurfaces 1920x1088", vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1088, &surf, 1, NULL, 0) == 0);
	CHECK("vaCreateContext", vaCreateContext(dpy, cfg, 1920, 1088, VA_PROGRESSIVE, &surf, 1, &ctx) == 0);
	memset(&d, 0, sizeof(d));
	st = vaExportSurfaceHandle(dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
				   VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &d);
	CHECK("vaExportSurfaceHandle (separate layers)", st == VA_STATUS_SUCCESS);
	if (st)
		return 1;
	CHECK("NV12 in 2 objects, 2 layers (R8, GR88)", d.fourcc == VA_FOURCC_NV12 && d.num_objects == 2 && d.num_layers == 2 &&
	      d.layers[0].drm_format == 0x20203852 && d.layers[1].drm_format == 0x38385247 &&
	      d.layers[0].pitch[0] == 1920 && d.layers[1].pitch[0] == 1920);
	CHECK("vaSyncSurface after export (nothing decoded: at once)", vaSyncSurface(dpy, surf) == VA_STATUS_SUCCESS);
	{
		VADRMPRIMESurfaceDescriptor c;

		memset(&c, 0, sizeof(c));
		CHECK("composed layers refused", vaExportSurfaceHandle(dpy, surf, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
			VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS, &c) != VA_STATUS_SUCCESS);
	}
	for (int i = 0; i < 2; i++) {
		struct vg_prime p = { 0, 0, d.objects[i].fd };
		struct vg_resource_info info = { 0 };
		struct vg_gem_close cl;
		char what[96];
		int r = ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &p);

		snprintf(what, sizeof(what), "plane %d imported by the other open (PRIME_FD_TO_HANDLE: %s)", i, r ? strerror(errno) : "ok");
		CHECK(what, r == 0 && p.handle);
		info.bo_handle = p.handle;
		r = ioctl(fd2, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info);
		snprintf(what, sizeof(what), "plane %d: resource %u", i, info.res_handle);
		CHECK(what, r == 0 && info.res_handle);
		{
			struct vg_prime again = { 0, 0, d.objects[i].fd };

			CHECK("imported again: the same handle", ioctl(fd2, DRM_IOCTL_PRIME_FD_TO_HANDLE, &again) == 0 &&
			      again.handle == p.handle);
		}
		cl.handle = p.handle;
		cl.pad = 0;
		CHECK("GEM_CLOSE of the import", ioctl(fd2, DRM_IOCTL_GEM_CLOSE, &cl) == 0);
		close(d.objects[i].fd);
	}
	CHECK("vaDestroySurfaces", vaDestroySurfaces(dpy, &surf, 1) == 0);
	vaDestroyContext(dpy, ctx);
	vaDestroyConfig(dpy, cfg);
	vaTerminate(dpy);
	printf(fails ? "%d FAILED\n" : "all ok\n", fails);
	return fails != 0;
}
