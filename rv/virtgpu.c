/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * rv-virtgpu: a virtio-gpu render node, /dev/dri/renderD128, made by rv.
 *
 * The machine has no GPU driver in its kernel. Programs that want one (Mesa's
 * virtio_gpu_dri.so with its virgl DRM winsys, GBM, EGL's device, surfaceless
 * and Wayland platforms, Chromium's GPU process) find a device anyway: rv
 * answers for it. What they ask of it is the virtio-gpu DRM interface
 * (Linux's include/uapi/drm/virtgpu_drm.h and drm.h), and rv carries it out
 * by talking vtest (virglrenderer's vtest/vtest_protocol.h, version 3) to the
 * GPU server whose socket RV_VIRTGPU names: gpuserver in the machine
 * (gpu/PROTOCOL.md), which puts the work into the ring for the page, or
 * virglrenderer's own virgl_test_server on a build machine.
 *
 * Only while RV_VIRTGPU is set (a socket's path, or 1 for
 * $XDG_RUNTIME_DIR/rvgpu-0). Then:
 *
 * - The device is in the file system for the programs that look: a small
 *   tree of real files (vg_tree()) stands in for /dev/dri and for the parts
 *   of /sys that libdrm reads (/sys/dev/char/226:128, /sys/class/drm, the
 *   PCI device a virtio-gpu is on, as QEMU's virtio-gpu-pci has it). Paths
 *   under those are taken there (rv_virtgpu_path()). The device file itself
 *   is a plain file there; whatever stat says of it says a character device
 *   226:128 instead (rv_virtgpu_fix_stat()).
 * - Opening it connects to the server: the descriptor the program gets is
 *   that socket (one connection is one context, as one open of a render node
 *   is one DRM file). Its ioctls are answered here (rv_virtgpu_ioctl()), its
 *   fstat says the device, and an mmap() of it maps the backing file of the
 *   buffer the offset stands for (rv_virtgpu_mmap()).
 * - A buffer (GEM object) is a server resource: its id is the virgl resource
 *   handle the command streams name, and its backing is the file in memory
 *   the server sent with it. PRIME export hands out a copy of that file's
 *   descriptor, which is what the window server's zwp_linux_dmabuf_v1 looks
 *   up in gpuserver's export table (slot format 2: zero copy).
 *
 * - A buffer another open made (another process's, passed as a dmabuf, or
 *   another open in this one: libva's and Mesa's EGL each open the device)
 *   is imported through the server (VCMD_RV_RESOURCE_IMPORT, when its ready
 *   file says "import"): it finds the resource by the backing file's
 *   identity and attaches it to this context too.
 *
 * Not (yet): blobs, syncobjs, venus' capset.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <unistd.h>

#include "rv.h"
#include "virtgpu_formats.h"

/* ---- vtest (virglrenderer vtest/vtest_protocol.h) */
#define VCMD_GET_CAPS			1
#define VCMD_RESOURCE_UNREF		3
#define VCMD_SUBMIT_CMD			6
#define VCMD_RESOURCE_BUSY_WAIT		7
#define VCMD_CREATE_RENDERER		8
#define VCMD_GET_CAPS2			9
#define VCMD_PING_PROTOCOL_VERSION	10
#define VCMD_PROTOCOL_VERSION		11
#define VCMD_RESOURCE_CREATE2		12
#define VCMD_TRANSFER_GET2		13
#define VCMD_TRANSFER_PUT2		14
#define VCMD_BUSY_WAIT_FLAG_WAIT	1
#define VCMD_RV_FENCE			0x10001	/* gpuserver's own: an eventfd written once the work so far is done */
#define VCMD_RV_RESOURCE_IMPORT		0x10002	/* gpuserver's own: another connection's resource, by its file */
#define VTEST_PROTOCOL			3

/* ---- DRM (drm.h, virtgpu_drm.h): request numbers are _IOC(dir, 'd', nr, size); only nr is looked at */
#define DRM_NR_VERSION			0x00
#define DRM_NR_GET_UNIQUE		0x01
#define DRM_NR_GET_MAGIC		0x02
#define DRM_NR_GEM_CLOSE		0x09
#define DRM_NR_GEM_FLINK		0x0a
#define DRM_NR_GEM_OPEN			0x0b
#define DRM_NR_GET_CAP			0x0c
#define DRM_NR_SET_CLIENT_CAP		0x0d
#define DRM_NR_PRIME_HANDLE_TO_FD	0x2d
#define DRM_NR_PRIME_FD_TO_HANDLE	0x2e
#define DRM_NR_SYNCOBJ_FIRST		0xbf
#define DRM_NR_SYNCOBJ_LAST		0xcd
#define DRM_COMMAND_BASE		0x40
#define VIRTGPU_MAP			0x01
#define VIRTGPU_EXECBUFFER		0x02
#define VIRTGPU_GETPARAM		0x03
#define VIRTGPU_RESOURCE_CREATE		0x04
#define VIRTGPU_RESOURCE_INFO		0x05
#define VIRTGPU_TRANSFER_FROM_HOST	0x06
#define VIRTGPU_TRANSFER_TO_HOST	0x07
#define VIRTGPU_WAIT			0x08
#define VIRTGPU_GET_CAPS		0x09
#define VIRTGPU_RESOURCE_CREATE_BLOB	0x0a
#define VIRTGPU_CONTEXT_INIT		0x0b

#define DRM_CAP_TIMESTAMP_MONOTONIC	0x6
#define DRM_CAP_PRIME			0x5
#define DRM_CAP_SYNCOBJ			0x13
#define DRM_CAP_SYNCOBJ_TIMELINE	0x14

#define VIRTGPU_PARAM_3D_FEATURES	1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX	2
#define VIRTGPU_PARAM_RESOURCE_BLOB	3
#define VIRTGPU_PARAM_HOST_VISIBLE	4
#define VIRTGPU_PARAM_CROSS_DEVICE	5
#define VIRTGPU_PARAM_CONTEXT_INIT	6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7
#define VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME 8

#define VIRTGPU_EXECBUF_FENCE_FD_IN	0x01
#define VIRTGPU_EXECBUF_FENCE_FD_OUT	0x02
#define VIRTGPU_EXECBUF_RING_IDX	0x04
#define VIRTGPU_WAIT_NOWAIT		1
#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID	0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS	0x0002
#define VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK 0x0003
#define VIRTGPU_CONTEXT_PARAM_DEBUG_NAME 0x0004
#define CAPSET_VIRGL			1
#define CAPSET_VIRGL2			2

#define DRM_MAJOR			226
#define RENDER_MINOR			128

/* The layouts, as the kernel's (fixed-size fields only, the same on every 64-bit machine). */
struct vg_version {
	int32_t major, minor, patchlevel, pad;
	uint64_t name_len, name, date_len, date, desc_len, desc;
};
struct vg_get_cap { uint64_t capability, value; };
struct vg_gem_close { uint32_t handle, pad; };
struct vg_prime { uint32_t handle, flags; int32_t fd; };
struct vg_map { uint64_t offset; uint32_t handle, pad; };
struct vg_execbuffer {
	uint32_t flags, size;
	uint64_t command, bo_handles;
	uint32_t num_bo_handles;
	int32_t fence_fd;
	uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs;
	uint64_t in_syncobjs, out_syncobjs;
};
struct vg_getparam { uint64_t param, value; };
struct vg_resource_create {
	uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags;
	uint32_t bo_handle, res_handle, size, stride;
};
struct vg_resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct vg_transfer {
	uint32_t bo_handle;
	uint32_t x, y, z, w, h, d;
	uint32_t level, offset, stride, layer_stride;
};
struct vg_wait { uint32_t handle, flags; };
struct vg_get_caps { uint32_t cap_set_id, cap_set_ver; uint64_t addr; uint32_t size, pad; };
struct vg_context_init { uint32_t num_params, pad; uint64_t ctx_set_params; };
struct vg_context_param { uint64_t param, value; };

/* ---- state */

/* A buffer of a device: GEM handle `handle` is the server's resource `res`. */
struct vg_bo {
	uint32_t res, size;
	int fd;				/* the backing file (ours: above the guest's descriptors), or -1 */
	uint64_t dev, ino;		/* its identity, for PRIME imports */
	uint32_t target, format, width, height, depth, array_size, last_level;
};

/* One open of the device: one connection, one context. Found by the socket's identity. */
struct vg_dev {
	struct vg_dev *next;
	uint64_t dev, ino;
	pthread_mutex_t lock;		/* one request and its answer at a time */
	struct vg_bo **bos;		/* by handle - 1 */
	uint32_t nbos;
	int context_init;		/* CONTEXT_INIT done (or a context made without it) */
	int fences;			/* the server makes fences as files (VCMD_RV_FENCE): version 0.1 */
	int imports;			/* the server imports other connections' buffers (VCMD_RV_RESOURCE_IMPORT) */
	uint64_t fence_seq;		/* of the last one */
	int has_caps;
	uint32_t caps_len;
	uint8_t caps[4096];
};

static int vg_on = -1;
static char vg_socket[108];
static char vg_root[96];		/* the tree that stands in for /dev/dri and parts of /sys */
static uint64_t vg_node_dev, vg_node_ino;	/* the stand-in device file */
static pthread_mutex_t vg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct vg_dev *vg_devs;
static int vg_verbose;

int rv_virtgpu_enabled(void)
{
	if (vg_on < 0) {
		const char *s = getenv("RV_VIRTGPU");

		vg_on = 0;
		vg_verbose = getenv("RV_VIRTGPU_DEBUG") != NULL;
		if (s && *s && strcmp(s, "0")) {
			if (!strcmp(s, "1")) {
				const char *dir = getenv("XDG_RUNTIME_DIR");

				snprintf(vg_socket, sizeof(vg_socket), "%s/rvgpu-0", dir ? dir : "/tmp");
			} else {
				snprintf(vg_socket, sizeof(vg_socket), "%s", s);
			}
			snprintf(vg_root, sizeof(vg_root), "/tmp/.rv-virtgpu-1-%u", (unsigned)getuid());
			vg_on = 1;
		}
	}
	return vg_on;
}

#define say(...) do { if (vg_verbose) fprintf(stderr, "rv-virtgpu: " __VA_ARGS__); } while (0)

/* ---- the tree */

/* What is in it: directories end in '/', links are "name -> target", files "name = contents". */
#define PCI "sys/devices/pci0000:00/0000:00:02.0"
static const char *const tree[] = {
	"dev/",
	"dev/dri/",
	"dev/dri/renderD128 = ",
	"sys/",
	"sys/devices/",
	"sys/devices/pci0000:00/",
	PCI "/",
	PCI "/uevent = DRIVER=virtio-pci\nPCI_CLASS=30000\nPCI_ID=1AF4:1050\nPCI_SUBSYS_ID=1AF4:1100\n"
		"PCI_SLOT_NAME=0000:00:02.0\nMODALIAS=pci:v00001AF4d00001050sv00001AF4sd00001100bc03sc00i00\n",
	PCI "/vendor = 0x1af4\n",
	PCI "/device = 0x1050\n",
	PCI "/subsystem_vendor = 0x1af4\n",
	PCI "/subsystem_device = 0x1100\n",
	PCI "/revision = 0x01\n",
	PCI "/class = 0x030000\n",
	PCI "/subsystem -> ../../../bus/pci",
	PCI "/driver -> ../../../bus/pci/drivers/virtio-pci",
	PCI "/virtio0/",
	PCI "/virtio0/uevent = DRIVER=virtio_gpu\nMODALIAS=virtio:d00000010v00001AF4\n",
	PCI "/virtio0/vendor = 0x1af4\n",
	PCI "/virtio0/device = 0x0010\n",
	PCI "/virtio0/subsystem -> ../../../../bus/virtio",
	PCI "/virtio0/driver -> ../../../../bus/virtio/drivers/virtio_gpu",
	PCI "/virtio0/drm/",
	PCI "/virtio0/drm/renderD128/",
	PCI "/virtio0/drm/renderD128/dev = 226:128\n",
	PCI "/virtio0/drm/renderD128/uevent = MAJOR=226\nMINOR=128\nDEVNAME=dri/renderD128\nDEVTYPE=drm_minor\n",
	PCI "/virtio0/drm/renderD128/device -> ../../../virtio0",
	PCI "/virtio0/drm/renderD128/subsystem -> ../../../../../../class/drm",
	"sys/dev/",
	"sys/dev/char/",
	"sys/dev/char/226:128 -> ../../devices/pci0000:00/0000:00:02.0/virtio0/drm/renderD128",
	"sys/class/",
	"sys/class/drm/",
	"sys/class/drm/version = drm 1.1.0 20060810\n",
	"sys/class/drm/renderD128 -> ../../devices/pci0000:00/0000:00:02.0/virtio0/drm/renderD128",
	"sys/bus/",
	"sys/bus/pci/",
	"sys/bus/pci/drivers/",
	"sys/bus/pci/drivers/virtio-pci/",
	"sys/bus/virtio/",
	"sys/bus/virtio/drivers/",
	"sys/bus/virtio/drivers/virtio_gpu/",
};

/* The paths taken to the tree: these, and what is under them. */
static const char *const taken[] = {
	"/dev/dri", "/sys/devices/pci0000:00", "/sys/dev/char/226:128", "/sys/class/drm", "/sys/bus/virtio", "/sys/bus/pci",
};

static int tree_write(const char *root)
{
	char path[256];

	for (unsigned i = 0; i < sizeof(tree) / sizeof(tree[0]); i++) {
		const char *e = tree[i], *arrow = strstr(e, " -> "), *eq = strstr(e, " = ");
		size_t n = strlen(e);

		if (e[n - 1] == '/') {
			snprintf(path, sizeof(path), "%s/%.*s", root, (int)(n - 1), e);
			if (mkdir(path, 0755) && errno != EEXIST)
				return -1;
		} else if (arrow) {
			snprintf(path, sizeof(path), "%s/%.*s", root, (int)(arrow - e), e);
			if (symlink(arrow + 4, path))
				return -1;
		} else if (eq) {
			int fd;
			ssize_t len = (ssize_t)strlen(eq + 3);

			snprintf(path, sizeof(path), "%s/%.*s", root, (int)(eq - e), e);
			fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
			if (fd < 0)
				return -1;
			if (len && write(fd, eq + 3, (size_t)len) != len) {
				close(fd);
				return -1;
			}
			fchmod(fd, 0666);
			close(fd);
		}
	}
	return 0;
}

static void tree_remove(const char *root)
{
	char path[256];

	for (unsigned i = sizeof(tree) / sizeof(tree[0]); i-- > 0;) {
		const char *e = tree[i], *arrow = strstr(e, " -> "), *eq = strstr(e, " = ");
		size_t n = strlen(e);

		if (e[n - 1] == '/') {
			snprintf(path, sizeof(path), "%s/%.*s", root, (int)(n - 1), e);
			rmdir(path);
		} else {
			snprintf(path, sizeof(path), "%s/%.*s", root, (int)((arrow ? arrow : eq) - e), e);
			unlink(path);
		}
	}
	rmdir(root);
}

/* Made once on the machine (by whichever process needs it first: built aside, then renamed into place). */
static int vg_tree(void)
{
	static int made;
	char tmp[128], node[160];
	struct stat st;

	if (made)
		return made > 0 ? 0 : -1;
	snprintf(node, sizeof(node), "%s/dev/dri/renderD128", vg_root);
	if (lstat(node, &st)) {
		snprintf(tmp, sizeof(tmp), "%s.%d", vg_root, (int)getpid());
		tree_remove(tmp);
		if (mkdir(tmp, 0755) || tree_write(tmp)) {
			say("cannot make %s: %s\n", tmp, strerror(errno));
			tree_remove(tmp);
			made = -1;
			return -1;
		}
		chmod(tmp, 0755);
		if (rename(tmp, vg_root))
			tree_remove(tmp);	/* (someone else's is there now) */
		if (lstat(node, &st)) {
			made = -1;
			return -1;
		}
	}
	vg_node_dev = (uint64_t)st.st_dev;
	vg_node_ino = (uint64_t)st.st_ino;
	made = 1;
	return 0;
}

const char *rv_virtgpu_path(const char *path)
{
	static __thread char buf[4096 + 128];

	if (!rv_virtgpu_enabled() || path[0] != '/' || (path[1] != 'd' && path[1] != 's'))
		return NULL;
	for (unsigned i = 0; i < sizeof(taken) / sizeof(taken[0]); i++) {
		size_t n = strlen(taken[i]);

		if (!strncmp(path, taken[i], n) && (path[n] == 0 || path[n] == '/')) {
			if (vg_tree())
				return NULL;
			snprintf(buf, sizeof(buf), "%s%s", vg_root, path);
			return buf;
		}
	}
	return NULL;
}

/* ---- devices */

/* Whether the server is gpuserver with a feature (fences as files, imports): its ready file (next to the socket) says so. */
static int server_feature(const char *name)
{
	char path[128], buf[512];
	int fd;
	ssize_t n;

	snprintf(path, sizeof(path), "%s.ready", vg_socket);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	{
		char quoted[64];

		snprintf(quoted, sizeof(quoted), "\"%s\"", name);
		return strstr(buf, quoted) != NULL;
	}
}

static struct vg_dev *dev_new(uint64_t dev, uint64_t ino)
{
	struct vg_dev *d = calloc(1, sizeof(*d));

	if (!d)
		return NULL;
	d->dev = dev;
	d->ino = ino;
	d->fences = getenv("RV_VIRTGPU_FENCES") ? atoi(getenv("RV_VIRTGPU_FENCES")) : server_feature("fence");
	d->imports = getenv("RV_VIRTGPU_IMPORT") ? atoi(getenv("RV_VIRTGPU_IMPORT")) : server_feature("import");
	pthread_mutex_init(&d->lock, NULL);
	pthread_mutex_lock(&vg_lock);
	d->next = vg_devs;
	vg_devs = d;
	pthread_mutex_unlock(&vg_lock);
	return d;
}

/*
 * The device a descriptor is, or NULL. One this process did not open (it
 * came from another process, over a socket: Chromium's GPU process gets the
 * render node from its sandbox's broker, which opened it) is taken for one
 * when it is a connection to the GPU server: the process that opened it made
 * the context, and nothing else was done with it there.
 */
static struct vg_dev *dev_find(int fd, int adopt)
{
	struct sockaddr_un un;
	socklen_t len = sizeof(un);
	struct stat st;
	struct vg_dev *d;

	if (fstat(fd, &st) || !S_ISSOCK(st.st_mode))
		return NULL;
	pthread_mutex_lock(&vg_lock);
	for (d = vg_devs; d; d = d->next)
		if (d->dev == (uint64_t)st.st_dev && d->ino == (uint64_t)st.st_ino)
			break;
	pthread_mutex_unlock(&vg_lock);
	if (d || !adopt)
		return d;
	memset(&un, 0, sizeof(un));
	if (getpeername(fd, (struct sockaddr *)&un, &len) || un.sun_family != AF_UNIX ||
	    strncmp(un.sun_path, vg_socket, sizeof(un.sun_path)))
		return NULL;
	say("descriptor %d: a connection to the GPU server made elsewhere, taken as the device\n", fd);
	return dev_new((uint64_t)st.st_dev, (uint64_t)st.st_ino);
}

static struct vg_dev *dev_of(int fd)
{
	return dev_find(fd, 1);
}

void rv_virtgpu_fd(int fd)
{
	if (vg_on > 0)
		dev_of(fd);
}

void rv_virtgpu_fix_stat(struct stat *st)
{
	struct vg_dev *d;

	if (vg_on <= 0)
		return;
	if (vg_node_ino && (uint64_t)st->st_ino == vg_node_ino && (uint64_t)st->st_dev == vg_node_dev) {
		st->st_mode = S_IFCHR | 0666;
		st->st_rdev = makedev(DRM_MAJOR, RENDER_MINOR);
		st->st_size = 0;
		return;
	}
	if (!S_ISSOCK(st->st_mode) || !vg_devs)
		return;
	pthread_mutex_lock(&vg_lock);
	for (d = vg_devs; d; d = d->next)
		if (d->dev == (uint64_t)st->st_dev && d->ino == (uint64_t)st->st_ino)
			break;
	pthread_mutex_unlock(&vg_lock);
	if (d) {
		st->st_mode = S_IFCHR | 0666;
		st->st_rdev = makedev(DRM_MAJOR, RENDER_MINOR);
	}
}

/* The kernel's struct statx, as the guest has it: mode at 0x1c, inode at 0x20, device numbers from 0x80. */
void rv_virtgpu_fix_statx(void *p)
{
	uint8_t *s = p;
	uint32_t dev_major, dev_minor;
	uint64_t ino, dev;
	uint16_t mode;
	struct stat st;

	if (vg_on <= 0)
		return;
	memcpy(&mode, s + 0x1c, 2);
	memcpy(&ino, s + 0x20, 8);
	memcpy(&dev_major, s + 0x88, 4);
	memcpy(&dev_minor, s + 0x8c, 4);
	dev = makedev(dev_major, dev_minor);
	memset(&st, 0, sizeof(st));
	st.st_mode = mode;
	st.st_ino = ino;
	st.st_dev = dev;
	rv_virtgpu_fix_stat(&st);
	if (st.st_mode != mode) {
		uint32_t maj = DRM_MAJOR, min = RENDER_MINOR;

		mode = (uint16_t)st.st_mode;
		memcpy(s + 0x1c, &mode, 2);
		memcpy(s + 0x80, &maj, 4);
		memcpy(s + 0x84, &min, 4);
	}
}

/* Whole writes and reads on the connection (a signal does not cut a request in two). */
static int put(int fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN) {
			struct pollfd pfd = { fd, POLLOUT, 0 };

			poll(&pfd, 1, -1);
			continue;
		}
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int get(int fd, void *buf, size_t len)
{
	uint8_t *p = buf;

	while (len) {
		ssize_t n = read(fd, p, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN) {
			struct pollfd pfd = { fd, POLLIN, 0 };

			poll(&pfd, 1, -1);
			continue;
		}
		if (n <= 0)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int cmd(int fd, uint32_t id, const uint32_t *args, uint32_t nargs)
{
	uint32_t hdr[2] = { nargs, id };

	if (put(fd, hdr, sizeof(hdr)))
		return -1;
	return nargs ? put(fd, args, 4 * nargs) : 0;
}

/* An answer: its header (which must be `id`) and `n` dwords. */
static int answer(int fd, uint32_t id, uint32_t *out, uint32_t n)
{
	uint32_t hdr[2];

	if (get(fd, hdr, sizeof(hdr)))
		return -1;
	if (hdr[1] != id) {
		say("answer %u to command %u\n", hdr[1], id);
		return -1;
	}
	return n ? get(fd, out, 4 * n) : 0;
}

/* A descriptor sent with SCM_RIGHTS (and the one byte beside it), put above the guest's own. */
static int receive_fd(int sock, int high_up)
{
	char byte, ctl[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { &byte, 1 };
	struct msghdr msg;
	struct cmsghdr *c;
	ssize_t n;
	int fd, high;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctl;
	msg.msg_controllen = sizeof(ctl);
	do
		n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
	while (n < 0 && errno == EINTR);
	if (n <= 0 || !(c = CMSG_FIRSTHDR(&msg)) || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
		return -1;
	memcpy(&fd, CMSG_DATA(c), sizeof(fd));
	high = high_up ? fcntl(fd, F_DUPFD_CLOEXEC, 512) : -1;
	if (high >= 0) {
		close(fd);
		fd = high;
	}
	return fd;
}

/* The program's name, as Mesa's vtest winsys sends it (the server logs it). */
static void program_name(char *out, size_t size)
{
	char buf[256];
	int fd = open("/proc/self/comm", O_RDONLY | O_CLOEXEC);
	ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;

	if (fd >= 0)
		close(fd);
	if (n <= 0) {
		snprintf(out, size, "rv-virtgpu");
		return;
	}
	buf[n] = 0;
	buf[strcspn(buf, "\n")] = 0;
	snprintf(out, size, "%.63s", buf);
}

/* Connect, make the context (CREATE_RENDERER) and agree on version 3, as Mesa's vtest winsys does. */
static long dev_connect(int cloexec)
{
	struct sockaddr_un un;
	uint32_t hdr[2], word, args[2];
	char name[64];
	long fd;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;
	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	snprintf(un.sun_path, sizeof(un.sun_path), "%s", vg_socket);
	while (connect((int)fd, (struct sockaddr *)&un, sizeof(un)) < 0) {
		if (errno == EINTR)
			continue;
		say("no GPU server at %s: %s\n", vg_socket, strerror(errno));
		close((int)fd);
		return -ENXIO;
	}
	program_name(name, sizeof(name));
	hdr[0] = (uint32_t)strlen(name) + 1;
	hdr[1] = VCMD_CREATE_RENDERER;
	if (put((int)fd, hdr, sizeof(hdr)) || put((int)fd, name, strlen(name) + 1))
		goto fail;
	args[0] = args[1] = 0;
	if (cmd((int)fd, VCMD_PING_PROTOCOL_VERSION, NULL, 0) || cmd((int)fd, VCMD_RESOURCE_BUSY_WAIT, args, 2) ||
	    get((int)fd, hdr, sizeof(hdr)) || hdr[1] != VCMD_PING_PROTOCOL_VERSION ||
	    answer((int)fd, VCMD_RESOURCE_BUSY_WAIT, &word, 1))
		goto fail;
	word = VTEST_PROTOCOL;
	if (cmd((int)fd, VCMD_PROTOCOL_VERSION, &word, 1) || answer((int)fd, VCMD_PROTOCOL_VERSION, &word, 1) ||
	    word < VTEST_PROTOCOL) {
		say("the server speaks vtest %u (%u is needed)\n", word, VTEST_PROTOCOL);
		goto fail;
	}
	if (!cloexec)
		fcntl((int)fd, F_SETFD, 0);
	return fd;
fail:
	close((int)fd);
	return -ENXIO;
}

long rv_virtgpu_open(long fd, int cloexec)
{
	struct stat st;
	struct vg_dev *d;
	long sock;

	if (vg_on <= 0 || fd < 0 || !vg_node_ino || fstat((int)fd, &st) ||
	    (uint64_t)st.st_ino != vg_node_ino || (uint64_t)st.st_dev != vg_node_dev)
		return fd;
	close((int)fd);
	sock = dev_connect(cloexec);
	if (sock < 0)
		return sock;
	if (fstat((int)sock, &st) || !(d = dev_new((uint64_t)st.st_dev, (uint64_t)st.st_ino))) {
		close((int)sock);
		return -ENOMEM;
	}
	say("opened (descriptor %ld, %s)\n", sock, vg_socket);
	return sock;
}

static void dev_free(struct vg_dev *d)
{
	for (uint32_t i = 0; i < d->nbos; i++) {
		if (d->bos[i]) {
			if (d->bos[i]->fd >= 0)
				close(d->bos[i]->fd);
			free(d->bos[i]);
		}
	}
	free(d->bos);
	pthread_mutex_destroy(&d->lock);
	free(d);
}

/*
 * About to close `fd`. If it is the last descriptor of a device here, the
 * device's state goes too (its buffers' backing files were ours): the
 * server sees the connection close and lets the context go. Another socket
 * may get the same identity later, and must not be taken for the device.
 */
void rv_virtgpu_close(int fd)
{
	struct vg_dev *d, **pp;
	DIR *dir;
	struct dirent *e;
	int others = 0;

	if (vg_on <= 0 || !vg_devs || !(d = dev_find(fd, 0)))
		return;
	dir = opendir("/proc/self/fd");
	if (!dir)
		return;
	while ((e = readdir(dir))) {
		struct stat st;
		int n = atoi(e->d_name);

		if (e->d_name[0] < '0' || e->d_name[0] > '9' || n == fd || n == dirfd(dir))
			continue;
		if (!fstat(n, &st) && (uint64_t)st.st_ino == d->ino && (uint64_t)st.st_dev == d->dev) {
			others = 1;
			break;
		}
	}
	closedir(dir);
	if (others)
		return;
	pthread_mutex_lock(&vg_lock);
	for (pp = &vg_devs; *pp; pp = &(*pp)->next) {
		if (*pp == d) {
			*pp = d->next;
			break;
		}
	}
	pthread_mutex_unlock(&vg_lock);
	say("closed (descriptor %d)\n", fd);
	dev_free(d);
}

/* ---- buffers */

static struct vg_bo *bo_of(struct vg_dev *d, uint32_t handle)
{
	return handle && handle <= d->nbos ? d->bos[handle - 1] : NULL;
}

static uint32_t bo_add(struct vg_dev *d, struct vg_bo *bo)
{
	uint32_t i;

	for (i = 0; i < d->nbos && d->bos[i]; i++)
		;
	if (i == d->nbos) {
		uint32_t cap = d->nbos ? d->nbos * 2 : 64;
		struct vg_bo **n = realloc(d->bos, cap * sizeof(*n));

		if (!n)
			return 0;
		memset(n + d->nbos, 0, (cap - d->nbos) * sizeof(*n));
		d->bos = n;
		d->nbos = cap;
	}
	d->bos[i] = bo;
	return i + 1;
}

/*
 * How many bytes of the backing a transfer of `box` at `level` moves, as
 * Mesa's vtest winsys counts them (vtest_get_transfer_size()): rows of the
 * level's natural stride, the last one only as wide as the box. Formats not
 * sized here: the rest of the backing.
 */
static uint32_t transfer_size(const struct vg_bo *bo, const struct vg_transfer *t)
{
	uint32_t bytes = 0, bw = 1, bh = 1, lw, lh, stride, rows, layer;
	uint64_t size;

	if (bo->format < sizeof(virgl_format_blocks) / sizeof(virgl_format_blocks[0])) {
		bytes = virgl_format_blocks[bo->format].bytes;
		bw = virgl_format_blocks[bo->format].bw;
		bh = virgl_format_blocks[bo->format].bh;
	}
	if (!bytes || t->offset >= bo->size)
		return t->offset < bo->size ? bo->size - t->offset : 0;
	lw = bo->width >> t->level;
	lh = bo->height >> t->level;
	if (!lw)
		lw = 1;
	if (!lh)
		lh = 1;
	/* (A buffer's "width" is its bytes; its rows are one.) */
	stride = (t->h > 1 ? (lw + bw - 1) / bw : (t->w + bw - 1) / bw) * bytes;
	rows = (t->h + bh - 1) / bh;
	layer = stride * rows;
	if (t->d > 1)
		layer = stride * ((lh + bh - 1) / bh);
	size = (uint64_t)layer * (t->d ? t->d : 1);
	if (size > bo->size - t->offset)
		size = bo->size - t->offset;
	return (uint32_t)size;
}

/* ---- fences as files */

/*
 * A fence is an eventfd the server writes when the work submitted before it
 * is done: readable then, as a sync_file is (poll() is all Mesa and libsync
 * do with one). The last ones made are remembered with their device and
 * order, so that merging two of the same device is the later one.
 */
#define FENCES_KEPT 64
static struct vg_fence {
	uint64_t dev, ino, seq;
	struct vg_dev *d;
} vg_fences[FENCES_KEPT];
static unsigned vg_fence_next;

static void fence_add(struct vg_dev *d, int fd)
{
	struct stat st;

	if (fstat(fd, &st))
		return;
	pthread_mutex_lock(&vg_lock);
	vg_fences[vg_fence_next % FENCES_KEPT] = (struct vg_fence){ (uint64_t)st.st_dev, (uint64_t)st.st_ino, ++d->fence_seq, d };
	vg_fence_next++;
	pthread_mutex_unlock(&vg_lock);
}

static int fence_of(int fd, struct vg_fence *out)
{
	struct stat st;
	int found = 0;

	if (fstat(fd, &st))
		return 0;
	pthread_mutex_lock(&vg_lock);
	for (unsigned i = 0; i < FENCES_KEPT && !found; i++) {
		if (vg_fences[i].d && vg_fences[i].ino == (uint64_t)st.st_ino && vg_fences[i].dev == (uint64_t)st.st_dev) {
			*out = vg_fences[i];
			found = 1;
		}
	}
	pthread_mutex_unlock(&vg_lock);
	return found;
}

static int signalled(int fd)
{
	struct pollfd pfd = { fd, POLLIN, 0 };

	return poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN);
}

/* SYNC_IOC_MERGE and SYNC_IOC_FILE_INFO (linux/sync_file.h) on fences of ours. */
#define SYNC_NR_MERGE		3
#define SYNC_NR_FILE_INFO	4
struct vg_sync_merge { char name[32]; int32_t fd2, fence; uint32_t flags, pad; };
struct vg_sync_file_info { char name[32]; int32_t status; uint32_t flags, num_fences, pad; uint64_t sync_fence_info; };
struct vg_sync_fence_info { char obj_name[32], driver_name[32]; int32_t status; uint32_t flags; uint64_t timestamp_ns; };

static int sync_ioctl(struct rv_cpu *cpu, int fd, uint32_t nr, uint64_t arg, uint64_t *ret)
{
	struct vg_fence a, b;
	int ours = fence_of(fd, &a);

	if (!ours)
		return 0;
	if (nr == SYNC_NR_MERGE) {
		struct vg_sync_merge *m = rv_user_ptr(cpu, arg, sizeof(*m), RV_PROT_READ | RV_PROT_WRITE);
		int keep, out;

		if (!m) {
			*ret = (uint64_t)-EFAULT;
			return 1;
		}
		/* Both of one device: the later one. One already done: the other. Else: when the first is done, the second. */
		if (fence_of(m->fd2, &b) && b.d == a.d)
			keep = b.seq > a.seq ? m->fd2 : fd;
		else if (signalled(fd))
			keep = m->fd2;
		else if (signalled(m->fd2))
			keep = fd;
		else {
			struct pollfd pfd = { fd, POLLIN, 0 };

			while (poll(&pfd, 1, -1) < 0 && errno == EINTR)
				;
			keep = m->fd2;
		}
		out = fcntl(keep, F_DUPFD_CLOEXEC, 0);
		*ret = out < 0 ? (uint64_t)-errno : 0;
		if (out >= 0)
			m->fence = out;
		return 1;
	}
	if (nr == SYNC_NR_FILE_INFO) {
		struct vg_sync_file_info *info = rv_user_ptr(cpu, arg, sizeof(*info), RV_PROT_READ | RV_PROT_WRITE);
		int status = signalled(fd);

		if (!info) {
			*ret = (uint64_t)-EFAULT;
			return 1;
		}
		snprintf(info->name, sizeof(info->name), "virtio_gpu");
		info->status = status;
		if (info->num_fences && info->sync_fence_info) {
			struct vg_sync_fence_info *fi = rv_user_ptr(cpu, info->sync_fence_info, sizeof(*fi), RV_PROT_WRITE);

			if (!fi) {
				*ret = (uint64_t)-EFAULT;
				return 1;
			}
			memset(fi, 0, sizeof(*fi));
			snprintf(fi->obj_name, sizeof(fi->obj_name), "virtio_gpu");
			snprintf(fi->driver_name, sizeof(fi->driver_name), "virtio_gpu");
			fi->status = status;
		}
		info->num_fences = 1;
		*ret = 0;
		return 1;
	}
	return 0;
}

/* ---- the ioctls */

static long copy_field(struct rv_cpu *cpu, uint64_t addr, uint64_t *len, const char *value)
{
	uint64_t n = strlen(value), want = *len;

	*len = n;
	if (want && addr) {
		void *p = rv_user_ptr(cpu, addr, want < n ? want : n, RV_PROT_WRITE);

		if (!p)
			return -EFAULT;
		memcpy(p, value, want < n ? want : n);
	}
	return 0;
}

/*
 * Caps that Mesa's virgl cannot be given together. The page's vrend takes away VIRGL_CAP_TRANSFER and
 * VIRGL_CAP_COPY_TRANSFER (gpu/vrend/rvgpu.c: transfers go as vtest records), and without them Mesa sets up no
 * staging buffer (virgl_context.c: virgl_staging_init only with both). But VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS
 * was still there, and with it Mesa marks textures use_staging (virgl_resource.c: virgl_can_use_staging) and
 * reads them back through that staging buffer that was never made: a NULL winsys at virgl_staging_alloc
 * (libgallium 25.0.7, virgl_staging_mgr.c:42, "address 0x38"), the first time a program such as mpv wrote part
 * of a texture. So the second goes with the first. (Offsets in struct virgl_caps_v2: virglrenderer's virgl_hw.h.)
 */
#define CAPS_V2_CAPABILITY_BITS		392
#define CAPS_V2_CAPABILITY_BITS_V2	688
#define VIRGL_CAP_COPY_TRANSFER		(1u << 26)
#define VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS (1u << 7)
static void caps_fix(uint8_t *caps, uint32_t size)
{
	uint32_t bits, bits2;

	if (size < CAPS_V2_CAPABILITY_BITS_V2 + 4)
		return;
	memcpy(&bits, caps + CAPS_V2_CAPABILITY_BITS, 4);
	memcpy(&bits2, caps + CAPS_V2_CAPABILITY_BITS_V2, 4);
	if (!(bits & VIRGL_CAP_COPY_TRANSFER) && (bits2 & VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS)) {
		bits2 &= ~VIRGL_CAP_V2_COPY_TRANSFER_BOTH_DIRECTIONS;
		memcpy(caps + CAPS_V2_CAPABILITY_BITS_V2, &bits2, 4);
	}
}

static long caps_fetch(struct vg_dev *d, int fd)
{
	uint32_t hdr[2];

	if (d->has_caps)
		return 0;
	/* (The answer says 2, the caps' version, where a command number would be; its length is in bytes, plus one.) */
	if (cmd(fd, VCMD_GET_CAPS2, NULL, 0) || get(fd, hdr, sizeof(hdr)) || hdr[1] != 2 || !hdr[0])
		return -EIO;
	d->caps_len = hdr[0] - 1;
	if (d->caps_len > sizeof(d->caps)) {
		uint8_t skip[256];
		uint32_t left = d->caps_len - (uint32_t)sizeof(d->caps);

		if (get(fd, d->caps, sizeof(d->caps)))
			return -EIO;
		while (left) {
			uint32_t n = left < sizeof(skip) ? left : (uint32_t)sizeof(skip);

			if (get(fd, skip, n))
				return -EIO;
			left -= n;
		}
		d->caps_len = sizeof(d->caps);
	} else if (get(fd, d->caps, d->caps_len)) {
		return -EIO;
	}
	d->has_caps = 1;
	return 0;
}

static long busy_wait(struct vg_dev *d, int fd, uint32_t res, int wait, int *busy)
{
	uint32_t args[2] = { res, wait ? VCMD_BUSY_WAIT_FLAG_WAIT : 0 }, word;

	(void)d;
	if (cmd(fd, VCMD_RESOURCE_BUSY_WAIT, args, 2) || answer(fd, VCMD_RESOURCE_BUSY_WAIT, &word, 1))
		return -EIO;
	*busy = word != 0;
	return 0;
}

/*
 * A buffer this open did not make: the server knows it by its backing file (VCMD_RV_RESOURCE_IMPORT) and
 * attaches it to this connection's context; it answers with what the resource is. The buffer keeps a copy of
 * the descriptor (for its identity and mmap).
 */
static long prime_import(struct vg_dev *d, int fd, struct vg_prime *p, const struct stat *st)
{
	uint32_t args[3] = { (uint32_t)st->st_dev, (uint32_t)st->st_ino, (uint32_t)((uint64_t)st->st_ino >> 32) };
	uint32_t a[10];
	struct vg_bo *bo;

	if (!d->imports) {
		say("PRIME import of a buffer that is not this device's: the server does not import\n");
		return -EINVAL;
	}
	if (cmd(fd, VCMD_RV_RESOURCE_IMPORT, args, 3) || answer(fd, VCMD_RV_RESOURCE_IMPORT, a, 10))
		return -EIO;
	if (!a[0]) {
		say("PRIME import: no GPU buffer has that file\n");
		return -EINVAL;
	}
	if (!(bo = calloc(1, sizeof(*bo))))
		return -ENOMEM;
	bo->fd = fcntl(p->fd, F_DUPFD_CLOEXEC, 512);
	if (bo->fd < 0)
		bo->fd = fcntl(p->fd, F_DUPFD_CLOEXEC, 0);
	bo->res = a[0];
	bo->target = a[1];
	bo->format = a[2];
	bo->width = a[4];
	bo->height = a[5];
	bo->depth = a[6];
	bo->array_size = a[7];
	bo->last_level = a[8];
	bo->size = a[9];
	bo->dev = (uint64_t)st->st_dev;
	bo->ino = (uint64_t)st->st_ino;
	p->handle = bo_add(d, bo);
	if (!p->handle) {
		if (bo->fd >= 0)
			close(bo->fd);
		free(bo);
		cmd(fd, VCMD_RESOURCE_UNREF, &a[0], 1);
		return -ENOMEM;
	}
	say("imported resource %u (handle %u): format %u %ux%u\n", bo->res, p->handle, bo->format, bo->width, bo->height);
	return 0;
}

#define ARG(type, name) \
	type *name = rv_user_ptr(cpu, arg, sizeof(type), RV_PROT_READ | RV_PROT_WRITE); \
	if (!name) return -EFAULT

static long drm_ioctl(struct rv_cpu *cpu, struct vg_dev *d, int fd, uint32_t nr, uint64_t arg)
{
	switch (nr) {
	case DRM_NR_VERSION: {
		ARG(struct vg_version, v);
		struct vg_version k;
		long r;

		memcpy(&k, v, sizeof(k));
		k.major = 0;
		k.minor = d->fences ? 1 : 0;	/* 1: fences as files (VIRTGPU_EXECBUF_FENCE_FD_*), as the kernel's */
		k.patchlevel = 0;
		if ((r = copy_field(cpu, k.name, &k.name_len, "virtio_gpu")) ||
		    (r = copy_field(cpu, k.date, &k.date_len, "0")) ||
		    (r = copy_field(cpu, k.desc, &k.desc_len, "virtio GPU")))
			return r;
		v = rv_user_ptr(cpu, arg, sizeof(*v), RV_PROT_WRITE);
		if (!v)
			return -EFAULT;
		memcpy(v, &k, sizeof(k));
		return 0;
	}
	case DRM_NR_GET_CAP: {
		ARG(struct vg_get_cap, c);

		switch (c->capability) {
		case DRM_CAP_TIMESTAMP_MONOTONIC: c->value = 1; return 0;
		case DRM_CAP_PRIME: c->value = 3; return 0;	/* import and export */
		case DRM_CAP_SYNCOBJ:
		case DRM_CAP_SYNCOBJ_TIMELINE: c->value = 0; return 0;
		}
		c->value = 0;
		return -EOPNOTSUPP;	/* the rest are KMS's, and this device has none */
	}
	case DRM_NR_GEM_CLOSE: {
		ARG(struct vg_gem_close, c);
		struct vg_bo *bo = bo_of(d, c->handle);
		uint32_t res;

		if (!bo)
			return -EINVAL;
		d->bos[c->handle - 1] = NULL;
		res = bo->res;
		if (bo->fd >= 0)
			close(bo->fd);
		free(bo);
		return cmd(fd, VCMD_RESOURCE_UNREF, &res, 1) ? -EIO : 0;
	}
	case DRM_NR_PRIME_HANDLE_TO_FD: {
		ARG(struct vg_prime, p);
		struct vg_bo *bo = bo_of(d, p->handle);
		int out;

		if (!bo)
			return -ENOENT;
		if (bo->fd < 0)
			return -EINVAL;
		out = fcntl(bo->fd, (p->flags & O_CLOEXEC) ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
		if (out < 0)
			return -errno;
		p->fd = out;
		return 0;
	}
	case DRM_NR_PRIME_FD_TO_HANDLE: {
		ARG(struct vg_prime, p);
		struct stat st;

		if (fstat(p->fd, &st))
			return -EBADF;
		for (uint32_t i = 0; i < d->nbos; i++) {
			struct vg_bo *bo = d->bos[i];

			if (bo && bo->fd >= 0 && bo->ino == (uint64_t)st.st_ino && bo->dev == (uint64_t)st.st_dev) {
				p->handle = i + 1;
				return 0;
			}
		}
		return prime_import(d, fd, p, &st);
	}
	}
	if (nr >= DRM_NR_SYNCOBJ_FIRST && nr <= DRM_NR_SYNCOBJ_LAST)
		return -EOPNOTSUPP;
	/* Everything else of the DRM core is not for render nodes. */
	return -EACCES;
}

static long virtgpu_ioctl(struct rv_cpu *cpu, struct vg_dev *d, int fd, uint32_t nr, uint64_t arg)
{
	switch (nr) {
	case VIRTGPU_GETPARAM: {
		ARG(struct vg_getparam, g);
		uint32_t value, *out;

		switch (g->param) {
		case VIRTGPU_PARAM_3D_FEATURES: value = 1; break;
		case VIRTGPU_PARAM_CAPSET_QUERY_FIX: value = 1; break;
		case VIRTGPU_PARAM_RESOURCE_BLOB: value = 0; break;
		case VIRTGPU_PARAM_HOST_VISIBLE: value = 0; break;
		case VIRTGPU_PARAM_CROSS_DEVICE: value = 0; break;
		case VIRTGPU_PARAM_CONTEXT_INIT: value = 1; break;
		case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs: value = (1u << CAPSET_VIRGL) | (1u << CAPSET_VIRGL2); break;
		case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME: value = 1; break;
		default: return -EINVAL;
		}
		/* (The kernel writes an int there.) */
		out = rv_user_ptr(cpu, g->value, 4, RV_PROT_WRITE);
		if (!out)
			return -EFAULT;
		memcpy(out, &value, 4);
		return 0;
	}
	case VIRTGPU_GET_CAPS: {
		ARG(struct vg_get_caps, c);
		uint32_t size = c->size, have;
		long r;
		void *p;

		if (!size)
			return -EINVAL;
		if (c->cap_set_id != CAPSET_VIRGL && c->cap_set_id != CAPSET_VIRGL2)
			return -EINVAL;	/* (venus, DRM native contexts: not here) */
		if ((c->cap_set_id == CAPSET_VIRGL && c->cap_set_ver > 1) || c->cap_set_ver > 2)
			return -EINVAL;
		if ((r = caps_fetch(d, fd)))
			return r;
		have = c->cap_set_id == CAPSET_VIRGL ? 308 : d->caps_len;	/* sizeof(struct virgl_caps_v1) */
		if (have > d->caps_len)
			have = d->caps_len;
		if (size > have)
			size = have;
		p = rv_user_ptr(cpu, c->addr, size, RV_PROT_WRITE);
		if (!p)
			return -EFAULT;
		memcpy(p, d->caps, size);
		caps_fix(p, size);
		return 0;
	}
	case VIRTGPU_CONTEXT_INIT: {
		ARG(struct vg_context_init, ci);
		struct vg_context_param *params;

		if (d->context_init)
			return -EEXIST;
		if (ci->num_params > 4)
			return -EINVAL;
		params = rv_user_ptr(cpu, ci->ctx_set_params, ci->num_params * sizeof(*params), RV_PROT_READ);
		if (ci->num_params && !params)
			return -EFAULT;
		for (uint32_t i = 0; i < ci->num_params; i++) {
			switch (params[i].param) {
			case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
				if (params[i].value != CAPSET_VIRGL && params[i].value != CAPSET_VIRGL2)
					return -EINVAL;
				break;
			case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
				if (params[i].value > 64)
					return -EINVAL;
				break;
			case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
			case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
				break;
			default:
				return -EINVAL;
			}
		}
		d->context_init = 1;
		return 0;
	}
	case VIRTGPU_RESOURCE_CREATE: {
		ARG(struct vg_resource_create, rc);
		struct vg_bo *bo;
		uint32_t args[11], res;
		struct stat st;

		args[0] = 0;	/* (version 3: the server names it) */
		args[1] = rc->target;
		args[2] = rc->format;
		args[3] = rc->bind;
		args[4] = rc->width;
		args[5] = rc->height;
		args[6] = rc->depth;
		args[7] = rc->array_size;
		args[8] = rc->last_level;
		args[9] = rc->nr_samples;
		args[10] = rc->size;
		if (!(bo = calloc(1, sizeof(*bo))))
			return -ENOMEM;
		bo->fd = -1;
		if (cmd(fd, VCMD_RESOURCE_CREATE2, args, 11) || answer(fd, VCMD_RESOURCE_CREATE2, &res, 1) ||
		    (rc->size && (bo->fd = receive_fd(fd, 1)) < 0)) {
			free(bo);
			return -EIO;
		}
		d->context_init = 1;
		bo->res = res;
		bo->size = rc->size;
		bo->target = rc->target;
		bo->format = rc->format;
		bo->width = rc->width;
		bo->height = rc->height;
		bo->depth = rc->depth;
		bo->array_size = rc->array_size;
		bo->last_level = rc->last_level;
		if (bo->fd >= 0 && !fstat(bo->fd, &st)) {
			bo->dev = (uint64_t)st.st_dev;
			bo->ino = (uint64_t)st.st_ino;
		}
		rc->res_handle = res;
		rc->bo_handle = bo_add(d, bo);
		if (!rc->bo_handle) {
			if (bo->fd >= 0)
				close(bo->fd);
			free(bo);
			cmd(fd, VCMD_RESOURCE_UNREF, &res, 1);
			return -ENOMEM;
		}
		say("resource %u (handle %u): target %u format %u bind 0x%x %ux%ux%u, %u bytes\n", res, rc->bo_handle,
		    rc->target, rc->format, rc->bind, rc->width, rc->height, rc->depth, rc->size);
		return 0;
	}
	case VIRTGPU_RESOURCE_INFO: {
		ARG(struct vg_resource_info, ri);
		struct vg_bo *bo = bo_of(d, ri->bo_handle);

		if (!bo)
			return -ENOENT;
		ri->size = bo->size ? bo->size : 4096;
		ri->res_handle = bo->res;
		ri->blob_mem = 0;
		return 0;
	}
	case VIRTGPU_MAP: {
		ARG(struct vg_map, m);
		struct vg_bo *bo = bo_of(d, m->handle);

		if (!bo)
			return -ENOENT;
		if (bo->fd < 0)
			return -EINVAL;
		/* (An offset that names the buffer: rv_virtgpu_mmap() takes it back.) */
		m->offset = (uint64_t)m->handle << 32;
		say("map resource %u\n", bo->res);
		return 0;
	}
	case VIRTGPU_TRANSFER_TO_HOST:
	case VIRTGPU_TRANSFER_FROM_HOST: {
		ARG(struct vg_transfer, t);
		struct vg_bo *bo = bo_of(d, t->bo_handle);
		uint32_t args[10];

		if (!bo)
			return -ENOENT;
		if (t->stride || t->layer_stride)
			return -EINVAL;	/* (only blobs take strides) */
		if (!bo->size || t->offset >= bo->size)
			return -EINVAL;
		args[0] = bo->res;
		args[1] = t->level;
		args[2] = t->x;
		args[3] = t->y;
		args[4] = t->z;
		args[5] = t->w;
		args[6] = t->h;
		args[7] = t->d;
		args[8] = transfer_size(bo, t);
		args[9] = t->offset;
		say("%s resource %u level %u box %u,%u,%u %ux%ux%u offset %u: %u bytes\n",
		    nr == VIRTGPU_TRANSFER_TO_HOST ? "put" : "get", bo->res, t->level, t->x, t->y, t->z, t->w, t->h, t->d,
		    t->offset, args[8]);
		return cmd(fd, nr == VIRTGPU_TRANSFER_TO_HOST ? VCMD_TRANSFER_PUT2 : VCMD_TRANSFER_GET2, args, 10) ? -EIO : 0;
	}
	case VIRTGPU_WAIT: {
		ARG(struct vg_wait, w);
		struct vg_bo *bo = bo_of(d, w->handle);
		int busy;
		long r;

		if (!bo)
			return -ENOENT;
		if ((r = busy_wait(d, fd, bo->res, !(w->flags & VIRTGPU_WAIT_NOWAIT), &busy)))
			return r;
		say("wait%s resource %u: %s\n", (w->flags & VIRTGPU_WAIT_NOWAIT) ? " (no wait)" : "", bo->res, busy ? "busy" : "idle");
		return busy ? -EBUSY : 0;
	}
	case VIRTGPU_EXECBUFFER: {
		ARG(struct vg_execbuffer, eb);
		const uint32_t *words;
		uint32_t *handles;

		if (eb->flags & ~(uint32_t)(VIRTGPU_EXECBUF_FENCE_FD_IN | VIRTGPU_EXECBUF_FENCE_FD_OUT | VIRTGPU_EXECBUF_RING_IDX))
			return -EINVAL;
		if (eb->num_in_syncobjs || eb->num_out_syncobjs)
			return -EINVAL;
		if ((eb->flags & VIRTGPU_EXECBUF_RING_IDX) && eb->ring_idx)
			return -EINVAL;
		if (eb->size & 3)
			return -EINVAL;
		if (eb->num_bo_handles) {
			handles = rv_user_ptr(cpu, eb->bo_handles, 4ULL * eb->num_bo_handles, RV_PROT_READ);
			if (!handles)
				return -EFAULT;
			for (uint32_t i = 0; i < eb->num_bo_handles; i++)
				if (!bo_of(d, handles[i]))
					return -ENOENT;
		}
		/* A fence to wait for first (from another context: this one's own work is in order anyway). */
		if (eb->flags & VIRTGPU_EXECBUF_FENCE_FD_IN) {
			struct pollfd pfd = { eb->fence_fd, POLLIN, 0 };

			while (poll(&pfd, 1, -1) < 0 && errno == EINTR)
				;
		}
		if (eb->size) {
			words = rv_user_ptr(cpu, eb->command, eb->size, RV_PROT_READ);
			if (!words)
				return -EFAULT;
			if (cmd(fd, VCMD_SUBMIT_CMD, words, eb->size / 4))
				return -EIO;
		}
		d->context_init = 1;
		if ((eb->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) && d->fences) {
			int out;

			if (cmd(fd, VCMD_RV_FENCE, NULL, 0) || answer(fd, VCMD_RV_FENCE, NULL, 0) || (out = receive_fd(fd, 0)) < 0)
				return -EIO;
			fence_add(d, out);
			eb->fence_fd = out;
			say("fence %llu: descriptor %d%s\n", (unsigned long long)d->fence_seq, out, signalled(out) ? " (done)" : "");
		} else if (eb->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT) {
			/* Not said to be possible (version 0.0); for whoever asks anyway: a fence that is done, once it is. */
			int busy;
			long r = busy_wait(d, fd, 0, 1, &busy);
			int out;

			if (r)
				return r;
			out = eventfd(1, EFD_CLOEXEC);
			if (out < 0)
				return -errno;
			eb->fence_fd = out;
		}
		return 0;
	}
	case VIRTGPU_RESOURCE_CREATE_BLOB:
		return -EINVAL;	/* (VIRTGPU_PARAM_RESOURCE_BLOB is 0) */
	}
	return -EINVAL;
}

int rv_virtgpu_ioctl(struct rv_cpu *cpu, int fd, uint64_t req, uint64_t arg, uint64_t *ret)
{
	uint32_t nr = (uint32_t)req & 0xff;
	struct vg_dev *d;
	long r;

	if (vg_on <= 0)
		return 0;
	if (((req >> 8) & 0xff) == '>')
		return sync_ioctl(cpu, fd, nr, arg, ret);
	if (((req >> 8) & 0xff) != 'd' || !(d = dev_of(fd)))
		return 0;
	pthread_mutex_lock(&d->lock);
	if (nr >= DRM_COMMAND_BASE && nr < 0xa0)
		r = virtgpu_ioctl(cpu, d, fd, nr - DRM_COMMAND_BASE, arg);
	else
		r = drm_ioctl(cpu, d, fd, nr, arg);
	pthread_mutex_unlock(&d->lock);
	if (r < 0)
		say("ioctl 0x%llx: %s\n", (unsigned long long)req, strerror((int)-r));
	*ret = (uint64_t)r;
	return 1;
}

int rv_virtgpu_mmap(int *fd, uint64_t *offset)
{
	struct vg_dev *d;
	struct vg_bo *bo;

	if (vg_on <= 0 || *fd < 0 || !(d = dev_of(*fd)))
		return 0;
	pthread_mutex_lock(&d->lock);
	bo = bo_of(d, (uint32_t)(*offset >> 32));
	if (bo && bo->fd >= 0) {
		*fd = bo->fd;
		*offset &= 0xffffffffu;
	} else {
		*fd = -1;
	}
	pthread_mutex_unlock(&d->lock);
	return 1;
}
