/*
 * vkr-vtest: the native test bed of the venus front (T5). One process plays both halves of the machine's GPU
 * path: gpuserver's vtest server for venus clients (gpu/server/server.js does the same in the machine) and
 * the page's executor (rvgpu.c's ring consumer + rvgpu_vkr.c + vkr + wgvk on Dawn native), with the ring v3
 * records between them exactly as in the browser (gpu/webgpu/INTERFACES.md 7.1). Guest memory is a
 * reserved range of this process: every blob is a memfd mapped into it (and by the client), the ring and the
 * sync words too, and a "guest address" is an offset in it, as in the machine's memory.
 *
 *   vkr-vtest [--socket PATH] [--quiet]        (default socket: /tmp/.virgl_test, as virgl_test_server)
 *   client: VN_DEBUG=vtest VTEST_SOCKET_NAME=PATH VK_DRIVER_FILES=.../virtio_icd.json vulkaninfo
 *
 * Environment: WGVK_DAWN_DRIVER_FILES (the Vulkan driver under Dawn: lavapipe), WGVK_BACKEND=null,
 * VKR_VTEST_STATS=1 (a line of counts every 5 s), VKR_VTEST_EXIT_IDLE=N (exit N s after the last client went).
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "rvgpu.h"
#include "rvgpu_vkr.h"
#include "wgvk.h"

extern struct vkr_ring_stats { uint64_t cmds, ns, bytes, nested; } vkr_ring_stats;	/* (vkr_guest.h) */

/* T2's (wgvk_cmd.c): retire what WebGPU finished; a build without T2 has none */
unsigned wgvk_device_poll(VkDevice device) __attribute__((weak));

/* vtest (virglrenderer vtest/vtest_protocol.h) */
enum {
	VCMD_RESOURCE_UNREF = 3, VCMD_RESOURCE_BUSY_WAIT = 7, VCMD_CREATE_RENDERER = 8, VCMD_PING_PROTOCOL_VERSION = 10,
	VCMD_PROTOCOL_VERSION = 11, VCMD_GET_PARAM = 15, VCMD_GET_CAPSET = 16, VCMD_CONTEXT_INIT = 17,
	VCMD_RESOURCE_CREATE_BLOB = 18, VCMD_SYNC_CREATE = 19, VCMD_SYNC_UNREF = 20, VCMD_SYNC_READ = 21,
	VCMD_SYNC_WRITE = 22, VCMD_SYNC_WAIT = 23, VCMD_SUBMIT_CMD2 = 24,
};
#define VCMD_PARAM_MAX_TIMELINE_COUNT 1
#define VCMD_SYNC_WAIT_FLAG_ANY 1
#define CAPSET_VENUS 4
#define MAX_TIMELINES 64

/* the ring (PROTOCOL.md v2 + INTERFACES.md 7.1) */
#define RING_HEADER 2048
#define RING_DATA (4u << 20)
#define CAPS_SIZE 4096
#define MAX_SYNCS 4096

#define GUEST_SPAN (64ull << 30)

static uint8_t *G;		/* guest memory: [G, G + GUEST_SPAN) */
static uint64_t g_next = 1 << 20;	/* bump allocator (addresses are not reused: tests are short) */
static uint64_t ring, data, caps, syncs;
static uint32_t head, serial;
static bool quiet;
static struct { unsigned clients, cmds, submits, blobs, blob_mb, waits; double submit_us; } st;

static uint64_t
gpage(uint64_t size)
{
	uint64_t at = g_next;
	g_next += (size + 0xffff) & ~0xffffull;
	if (g_next > GUEST_SPAN) {
		fprintf(stderr, "vkr-vtest: guest memory used up\n");
		exit(1);
	}
	return at;
}

static double
now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---- ring records (gpuserver's record()) */

static void
put32(uint64_t at, uint32_t v)
{
	memcpy(G + at, &v, 4);
}

static uint32_t
record_begin(uint32_t op, uint32_t ctx, uint32_t bytes, uint64_t *payload)
{
	const uint32_t total = 16 + ((bytes + 15) & ~15u);
	uint32_t at = head % RING_DATA;
	if (at + total > RING_DATA) {
		const uint32_t pad = RING_DATA - at;
		put32(data + at, RVGPU_OP_PAD);
		put32(data + at + 4, 0);
		put32(data + at + 8, 0);
		put32(data + at + 12, pad - 16);
		head += pad;
		at = 0;
	}
	/* the executor is this process and drains after every command: the ring never fills */
	serial++;
	put32(data + at, op);
	put32(data + at + 4, ctx);
	put32(data + at + 8, serial);
	put32(data + at + 12, bytes);
	*payload = data + at + 16;
	return total;
}

static void
record_end(uint32_t total, bool completion)
{
	head += total;
	if (completion)
		rvgpu_guest_store32(ring + RVGPU_RING_SUBMITTED, serial);
	rvgpu_guest_store32(ring + RVGPU_RING_HEAD, head);
}

static void
record32(uint32_t op, uint32_t ctx, const uint32_t *w, unsigned n, bool completion)
{
	uint64_t p;
	uint32_t t = record_begin(op, ctx, 4 * n, &p);
	memcpy(G + p, w, 4 * n);
	record_end(t, completion);
}

/* ---- syncs: u64 words in guest memory, as gpuserver keeps them */

static struct { bool used; } sync_slot[MAX_SYNCS];

static uint64_t
sync_get(uint32_t id)
{
	uint64_t v;
	memcpy(&v, G + syncs + 8 * id, 8);
	return v;
}

static void
sync_set(uint32_t id, uint64_t v)
{
	memcpy(G + syncs + 8 * id, &v, 8);
}

/* ---- clients */

struct blob {
	uint32_t res;
	uint64_t addr, size;
	struct blob *next;
};

struct waiter {
	int efd;
	bool any;
	double deadline;
	unsigned n;
	struct { uint32_t id; uint64_t value; } *w;
	struct waiter *next;
};

struct client {
	int fd;
	uint32_t ctx;
	char name[64];
	uint8_t *in;
	size_t len, cap;
	bool venus;
	struct blob *blobs;
};

#define MAX_CLIENTS 64
static struct client *clients[MAX_CLIENTS];
static struct waiter *waiters;
static uint32_t next_res = 1;
static bool ctx_used[256];

static struct blob *
blob_find(uint32_t res)
{
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i])
			for (struct blob *b = clients[i]->blobs; b; b = b->next)
				if (b->res == res)
					return b;
	return NULL;
}

/* ---- present (T7, INTERFACES.md 9). There is no page here to draw a window's texture: a window server in this
 * test (weston) reads the venus WSI image's dma-buf, which is the blob's memfd. So the texels go there: vkr tells us
 * which blobs are VkDeviceMemory of a device (rvgpu_present_register_memory, at VN_BLOB_CREATE), and before a
 * fence's syncs reach the guest (rvgpu_vkr_retire_hook), each registered blob whose memory has an image bound
 * (wgvk_memory_image) is read back into the blob's pages in the image's linear layout, the pitch the guest's
 * WSI got from vkGetImageSubresourceLayout. */

static struct present_blob {
	uint32_t res;
	VkDevice device;
	VkDeviceMemory memory;
	uint8_t *dst;		/* the blob's pages in G; NULL until found */
	uint64_t size;
} pblob[256];
static struct { unsigned regs, calls, noimage, reads, failed; uint64_t bytes; double ms; } pst;


static void present_all(void *data);

void
rvgpu_present_register_memory(uint32_t res, uint64_t device, uint64_t memory)
{
	unsigned slot = 256;
	pst.regs++;
	for (unsigned i = 0; i < 256; i++) {
		if (pblob[i].res == res) {
			slot = i;
			break;
		}
		if (!pblob[i].res && slot == 256)
			slot = i;
	}
	if (slot == 256)
		return;
	pblob[slot] = (struct present_blob){ .res = res, .device = (VkDevice)(uintptr_t)device };
	/* (every time: a device made after the last one went may sit at the same address, with no hook) */
	wgvk_device_set_retire_hook(pblob[slot].device, present_all, NULL);
	memcpy(&pblob[slot].memory, &memory, sizeof(pblob[slot].memory));
}

static void
present_forget(uint32_t res)
{
	for (unsigned i = 0; i < 256; i++)
		if (pblob[i].res == res)
			pblob[i] = (struct present_blob){ 0 };
}

static void
present_all(void *data)
{
	(void)data;
	pst.calls++;
	for (unsigned i = 0; i < 256; i++) {
		struct present_blob *p = &pblob[i];
		if (!p->res)
			continue;
		VkImage image = wgvk_memory_image(p->device, p->memory);
		if (image == VK_NULL_HANDLE)
		{
			pst.noimage++;
			continue;	/* (a buffer's memory, or an image not bound yet) */
		}
		if (!p->dst) {
			struct blob *b = blob_find(p->res);
			if (!b)
				continue;
			p->dst = G + b->addr;
			p->size = b->size;
		}
		const double t0 = now_ms();
		uint32_t w, h, pitch;
		VkResult r = wgvk_image_read_present(image, p->dst, p->size, &w, &h, &pitch);
		pst.ms += now_ms() - t0;
		if (r != VK_SUCCESS) {
			if (!pst.failed++)
				fprintf(stderr, "vkr-vtest: present: resource %u: read back failed (%d)\n", p->res, r);
			continue;
		}
		pst.reads++;
		pst.bytes += (uint64_t)pitch * h;
	}
}


static bool
write_all(int fd, const void *p, size_t n)
{
	const uint8_t *b = p;
	while (n) {
		ssize_t r = write(fd, b, n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		b += r;
		n -= r;
	}
	return true;
}

static bool
reply(struct client *c, uint32_t cmd, const uint32_t *w, unsigned n)
{
	uint32_t hdr[2] = { n, cmd };
	return write_all(c->fd, hdr, 8) && (!n || write_all(c->fd, w, 4 * n));
}

static bool
send_fd(struct client *c, int fd)
{
	char byte = 0;
	char ctl[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { &byte, 1 };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof(ctl) };
	struct cmsghdr *cm = CMSG_FIRSTHDR(&msg);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	return sendmsg(c->fd, &msg, MSG_NOSIGNAL) == 1;
}

static void present_forget(uint32_t res);

static void
blob_free(struct blob *b)
{
	present_forget(b->res);
	/* back to unused address space (gpuserver unmaps after `done` passed the RES_UNREF; here the
	 * executor already ran it) */
	mmap(G + b->addr, b->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
	free(b);
}

static void
client_gone(int i, const char *why)
{
	struct client *c = clients[i];
	if (!quiet)
		fprintf(stderr, "vkr-vtest: client %d (%s, context %u) gone: %s\n", c->fd, c->name, c->ctx, why);
	for (struct blob *b = c->blobs, *n; b; b = n) {
		n = b->next;
		record32(RVGPU_OP_RES_UNREF, c->ctx, &b->res, 1, false);
		rvgpu_ring_drain();
		blob_free(b);
	}
	record32(RVGPU_OP_CTX_DESTROY, c->ctx, NULL, 0, false);
	rvgpu_ring_drain();
	ctx_used[c->ctx] = false;
	close(c->fd);
	free(c->in);
	free(c);
	clients[i] = NULL;
}

/* Arguments in dwords of a command (-1: the header's length in dwords; -2: in bytes); -3 unknown. */
static int
arg_dwords(uint32_t cmd)
{
	switch (cmd) {
	case VCMD_CREATE_RENDERER: return -2;
	case VCMD_PING_PROTOCOL_VERSION: return 0;
	case VCMD_PROTOCOL_VERSION: case VCMD_GET_PARAM: case VCMD_CONTEXT_INIT: case VCMD_RESOURCE_UNREF:
	case VCMD_SYNC_UNREF: case VCMD_SYNC_READ: return 1;
	case VCMD_GET_CAPSET: case VCMD_RESOURCE_BUSY_WAIT: case VCMD_SYNC_CREATE: return 2;
	case VCMD_SYNC_WRITE: return 3;
	case VCMD_RESOURCE_CREATE_BLOB: return 6;
	case VCMD_SYNC_WAIT: case VCMD_SUBMIT_CMD2: return -1;
	default: return -3;
	}
}

/* SUBMIT_CMD2 -> VN_SUBMIT: the same batches with sync ids turned into the addresses of their words */
static bool
submit_cmd2(struct client *c, const uint32_t *w, uint32_t n_dw)
{
	if (!n_dw)
		return true;
	const uint32_t n = w[0];
	/* a batch is 6 dwords from Mesa 25.0 (8 in virglrenderer's newer header); the client says which by where
	 * the first batch's commands start (1 + stride * n) */
	uint32_t stride = 6;
	if (n && n_dw > 2 && w[2] == 1 + 8 * n)
		stride = 8;
	if (n > (n_dw - 1) / stride)
		return false;
	uint32_t cmd_dw = 0, sync_n = 0;
	for (uint32_t b = 0; b < n; b++) {
		const uint32_t *h = w + 1 + stride * b;
		if (h[1] > n_dw || h[2] > n_dw - h[1] || h[3] > n_dw || h[4] > (n_dw - h[3]) / 3)
			return false;
		cmd_dw += h[2];
		sync_n += h[4];
	}
	const uint32_t out_dw = 1 + 6 * n + cmd_dw + 4 * sync_n;
	uint64_t p;
	const uint32_t total = record_begin(RVGPU_OP_VN_SUBMIT, c->ctx, 4 * out_dw, &p);
	uint32_t *o = (uint32_t *)(G + p);
	o[0] = n;
	uint32_t cmd_at = 1 + 6 * n, sync_at = cmd_at + cmd_dw;
	for (uint32_t b = 0; b < n; b++) {
		const uint32_t *h = w + 1 + stride * b;
		const uint32_t flags = h[0], cmd_off = h[1], cmd_size = h[2], sync_off = h[3], sync_count = h[4];
		const uint32_t ring_idx = (flags & 1) ? h[5] : 0;
		uint32_t *oh = o + 1 + 6 * b;
		oh[0] = cmd_at;
		oh[1] = cmd_size;
		oh[2] = sync_at;
		oh[3] = sync_count;
		oh[4] = ring_idx;
		oh[5] = flags;
		memcpy(o + cmd_at, w + cmd_off, 4 * cmd_size);
		cmd_at += cmd_size;
		for (uint32_t k = 0; k < sync_count; k++) {
			const uint32_t *s = w + sync_off + 3 * k;
			const uint32_t id = s[0] < MAX_SYNCS ? s[0] : 0;
			const uint64_t addr = syncs + 8 * id;
			o[sync_at++] = (uint32_t)addr;
			o[sync_at++] = (uint32_t)(addr >> 32);
			o[sync_at++] = s[1];
			o[sync_at++] = s[2];
		}
	}
	record_end(total, true);
	st.submits++;
	return true;
}

/* one whole command: returns false to drop the client */
static bool
command(struct client *c, uint32_t cmd, uint32_t len, const uint32_t *a)
{
	st.cmds++;
	switch (cmd) {
	case VCMD_CREATE_RENDERER: {
		size_t n = len < sizeof(c->name) - 1 ? len : sizeof(c->name) - 1;
		memcpy(c->name, a, n);
		c->name[n] = 0;
		return true;
	}
	case VCMD_PING_PROTOCOL_VERSION:
		return reply(c, VCMD_PING_PROTOCOL_VERSION, NULL, 0);
	case VCMD_RESOURCE_BUSY_WAIT: {
		const uint32_t busy = 0;	/* (only the ping's dummy: venus has no busy waits) */
		return reply(c, VCMD_RESOURCE_BUSY_WAIT, &busy, 1);
	}
	case VCMD_PROTOCOL_VERSION: {
		uint32_t v = a[0] < 3 ? a[0] : 3;
		return reply(c, VCMD_PROTOCOL_VERSION, &v, 1);
	}
	case VCMD_GET_PARAM: {
		uint32_t r[2] = { 0, 0 };
		if (a[0] == VCMD_PARAM_MAX_TIMELINE_COUNT)
			r[0] = 1, r[1] = MAX_TIMELINES;
		return reply(c, VCMD_GET_PARAM, r, 2);
	}
	case VCMD_GET_CAPSET: {
		const uint32_t vlen = rvgpu_guest_load32(ring + RVGPU_RING_VCAPS_LEN);
		if (a[0] != CAPSET_VENUS || a[1] > 0 || !vlen) {
			const uint32_t no = 0;
			return reply(c, VCMD_GET_CAPSET, &no, 1);
		}
		uint32_t r[1 + 1024];
		r[0] = 1;
		memcpy(r + 1, G + caps + 2048, vlen);
		return reply(c, VCMD_GET_CAPSET, r, 1 + vlen / 4);
	}
	case VCMD_CONTEXT_INIT: {
		uint32_t w[1 + 16] = { a[0] };
		memcpy(w + 1, c->name, sizeof(c->name) - 1);
		record32(RVGPU_OP_VN_CTX_INIT, c->ctx, w, 1 + (strlen(c->name) + 4) / 4, false);
		c->venus = true;
		return true;
	}
	case VCMD_RESOURCE_CREATE_BLOB: {
		const uint64_t size = a[2] | (uint64_t)a[3] << 32;
		const uint64_t psize = (size + 4095) & ~4095ull;
		int fd = memfd_create("vkr-vtest-blob", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, psize) < 0) {
			fprintf(stderr, "vkr-vtest: no blob of %llu bytes: %s\n", (unsigned long long)size, strerror(errno));
			return false;
		}
		const uint64_t addr = gpage(psize);
		if (mmap(G + addr, psize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
			fprintf(stderr, "vkr-vtest: mmap blob: %s\n", strerror(errno));
			close(fd);
			return false;
		}
		struct blob *b = calloc(1, sizeof(*b));
		b->res = next_res++;
		b->addr = addr;
		b->size = psize;
		b->next = c->blobs;
		c->blobs = b;
		const uint32_t w[9] = { b->res, a[0], a[1], (uint32_t)size, (uint32_t)(size >> 32), a[4], a[5],
					(uint32_t)addr, (uint32_t)(addr >> 32) };
		record32(RVGPU_OP_VN_BLOB_CREATE, c->ctx, w, 9, false);
		/* (drained before the reply: the import copies the memory's bytes into the blob's pages, so it
		 * must come before the client can write them; gpuserver waits for the executor's tail, VK-25) */
		rvgpu_ring_drain();
		st.blobs++;
		st.blob_mb += psize >> 20;
		bool ok = reply(c, VCMD_RESOURCE_CREATE_BLOB, &b->res, 1) && send_fd(c, fd);
		close(fd);
		return ok;
	}
	case VCMD_RESOURCE_UNREF: {
		for (struct blob **p = &c->blobs; *p; p = &(*p)->next) {
			if ((*p)->res == a[0]) {
				struct blob *b = *p;
				*p = b->next;
				record32(RVGPU_OP_RES_UNREF, c->ctx, &b->res, 1, false);
				rvgpu_ring_drain();
				blob_free(b);
				break;
			}
		}
		return true;
	}
	case VCMD_SYNC_CREATE: {
		uint32_t id = 1;
		while (id < MAX_SYNCS && sync_slot[id].used)
			id++;
		if (id == MAX_SYNCS)
			return false;
		sync_slot[id].used = true;
		sync_set(id, a[0] | (uint64_t)a[1] << 32);
		return reply(c, VCMD_SYNC_CREATE, &id, 1);
	}
	case VCMD_SYNC_UNREF:
		if (a[0] < MAX_SYNCS)
			sync_slot[a[0]].used = false;
		return true;
	case VCMD_SYNC_READ: {
		const uint64_t v = a[0] < MAX_SYNCS ? sync_get(a[0]) : 0;
		const uint32_t r[2] = { (uint32_t)v, (uint32_t)(v >> 32) };
		return reply(c, VCMD_SYNC_READ, r, 2);
	}
	case VCMD_SYNC_WRITE:
		if (a[0] < MAX_SYNCS)
			sync_set(a[0], a[1] | (uint64_t)a[2] << 32);
		return true;
	case VCMD_SYNC_WAIT: {
		if (len < 2)
			return false;
		const uint32_t n = (len - 2) / 3;
		struct waiter *wt = calloc(1, sizeof(*wt));
		wt->efd = eventfd(0, EFD_CLOEXEC);
		wt->any = a[0] & VCMD_SYNC_WAIT_FLAG_ANY;
		wt->deadline = a[1] == UINT32_MAX ? 1e300 : now_ms() + a[1];
		wt->n = n;
		wt->w = calloc(n ? n : 1, sizeof(*wt->w));
		for (uint32_t i = 0; i < n; i++) {
			wt->w[i].id = a[2 + 3 * i] < MAX_SYNCS ? a[2 + 3 * i] : 0;
			wt->w[i].value = a[3 + 3 * i] | (uint64_t)a[4 + 3 * i] << 32;
		}
		wt->next = waiters;
		waiters = wt;
		st.waits++;
		bool ok = reply(c, VCMD_SYNC_WAIT, NULL, 0) && send_fd(c, wt->efd);
		return ok;
	}
	case VCMD_SUBMIT_CMD2: {
		const double t0 = now_ms();
		if (!submit_cmd2(c, a, len)) {
			fprintf(stderr, "vkr-vtest: %s: a SUBMIT_CMD2 that does not add up\n", c->name);
			return false;
		}
		rvgpu_ring_drain();
		st.submit_us += (now_ms() - t0) * 1e3;
		return true;
	}
	default:
		fprintf(stderr, "vkr-vtest: %s sent command %u, which is not done here\n", c->name, cmd);
		return false;
	}
}

static bool
client_input(struct client *c)
{
	for (;;) {
		if (c->cap - c->len < 65536) {
			c->cap = c->cap ? 2 * c->cap : 1 << 20;
			c->in = realloc(c->in, c->cap);
		}
		ssize_t n = read(c->fd, c->in + c->len, c->cap - c->len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		if (n <= 0)
			return false;
		c->len += n;
	}
	size_t at = 0;
	while (c->len - at >= 8) {
		uint32_t hdr[2];
		memcpy(hdr, c->in + at, 8);
		const int size = arg_dwords(hdr[1]);
		if (size == -3) {
			fprintf(stderr, "vkr-vtest: %s sent unknown command %u\n", c->name, hdr[1]);
			return false;
		}
		const size_t bytes = size == -1 ? 4 * (size_t)hdr[0] : size == -2 ? hdr[0] : 4 * (size_t)size;
		if (c->len - at < 8 + bytes)
			break;
		/* (aligned copy for the dword view) */
		uint32_t *args = malloc(bytes + 4);
		memcpy(args, c->in + at + 8, bytes);
		bool ok = command(c, hdr[1], hdr[0], args);
		free(args);
		if (!ok)
			return false;
		at += 8 + bytes;
	}
	memmove(c->in, c->in + at, c->len - at);
	c->len -= at;
	return true;
}

static void
check_waiters(void)
{
	const double t = now_ms();
	for (struct waiter **p = &waiters; *p;) {
		struct waiter *w = *p;
		unsigned met = 0;
		for (unsigned i = 0; i < w->n; i++)
			met += sync_get(w->w[i].id) >= w->w[i].value;
		const bool done = w->any ? met > 0 : met == w->n;
		if (done || t > w->deadline) {
			if (done) {
				uint64_t one = 1;
				if (write(w->efd, &one, 8) != 8)
					perror("vkr-vtest: eventfd");
			}
			close(w->efd);
			*p = w->next;
			free(w->w);
			free(w);
		} else {
			p = &w->next;
		}
	}
}

int
main(int argc, char **argv)
{
	const char *sock = "/tmp/.virgl_test";
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			sock = argv[++i];
		else if (!strcmp(argv[i], "--quiet"))
			quiet = true;
		else {
			fprintf(stderr, "usage: vkr-vtest [--socket PATH] [--quiet]\n");
			return 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stderr, NULL, _IOLBF, 0);

	G = mmap(NULL, GUEST_SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (G == MAP_FAILED) {
		perror("vkr-vtest: reserve guest memory");
		return 1;
	}
	rvgpu_guest_set_host(G, GUEST_SPAN);
	const uint64_t block = RING_HEADER + RING_DATA + CAPS_SIZE + 8 * MAX_SYNCS;
	ring = gpage(block);
	if (mmap(G + ring, block, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
		perror("vkr-vtest: ring");
		return 1;
	}
	data = ring + RING_HEADER;
	caps = data + RING_DATA;
	syncs = caps + CAPS_SIZE;
	put32(ring + RVGPU_RING_MAGIC, RVGPU_RING_MAGIC_VALUE);
	put32(ring + RVGPU_RING_VERSION, 3);
	put32(ring + RVGPU_RING_PID, getpid());
	put32(ring + RVGPU_RING_DATA_SIZE, RING_DATA);
	put32(ring + RVGPU_RING_DATA_OFF, RING_HEADER);
	if (rvgpu_ring_attach(ring, RING_HEADER + RING_DATA, caps, CAPS_SIZE) != 0)
		return 1;
	const double t0 = now_ms();
	if (rvgpu_vkr_start((PFN_vkGetInstanceProcAddr)wgvk_GetInstanceProcAddr, wgvk_memory_set_guest_window,
			    wgvk_device_poll) != 0)
		return 1;
	fprintf(stderr, "vkr-vtest: vkr started in %.1f ms; venus capset %u bytes, exec_state %u\n", now_ms() - t0,
		rvgpu_guest_load32(ring + RVGPU_RING_VCAPS_LEN), rvgpu_guest_load32(ring + RVGPU_RING_EXEC_STATE));

	int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	struct sockaddr_un un = { .sun_family = AF_UNIX };
	snprintf(un.sun_path, sizeof(un.sun_path), "%s", sock);
	unlink(sock);
	if (bind(lfd, (struct sockaddr *)&un, sizeof(un)) < 0 || listen(lfd, 16) < 0) {
		fprintf(stderr, "vkr-vtest: %s: %s\n", sock, strerror(errno));
		return 1;
	}
	fprintf(stderr, "vkr-vtest: listening on %s\n", sock);

	const bool stats = getenv("VKR_VTEST_STATS") && atoi(getenv("VKR_VTEST_STATS"));
	const double exit_idle = getenv("VKR_VTEST_EXIT_IDLE") ? atof(getenv("VKR_VTEST_EXIT_IDLE")) * 1e3 : 0;
	double last_stats = now_ms(), last_client = now_ms();
	for (;;) {
		struct pollfd pf[MAX_CLIENTS + 1];
		int map[MAX_CLIENTS + 1], n = 0, live = 0;
		pf[n] = (struct pollfd){ lfd, POLLIN, 0 };
		map[n++] = -1;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i]) {
				pf[n] = (struct pollfd){ clients[i]->fd, POLLIN, 0 };
				map[n++] = i;
				live++;
			}
		}
		const int timeout = waiters ? 1 : rvgpu_poll_ms();
		int r = poll(pf, n, timeout);
		if (r < 0 && errno != EINTR) {
			perror("vkr-vtest: poll");
			return 1;
		}
		for (int k = 0; r > 0 && k < n; k++) {
			if (!pf[k].revents)
				continue;
			if (map[k] < 0) {
				int fd;
				while ((fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK)) >= 0) {
					int i = 0, ctx = 1;
					while (i < MAX_CLIENTS && clients[i])
						i++;
					while (ctx < 256 && ctx_used[ctx])
						ctx++;
					if (i == MAX_CLIENTS || ctx == 256) {
						close(fd);
						continue;
					}
					ctx_used[ctx] = true;
					clients[i] = calloc(1, sizeof(struct client));
					clients[i]->fd = fd;
					clients[i]->ctx = ctx;
					strcpy(clients[i]->name, "?");
					st.clients++;
				}
				continue;
			}
			if (!client_input(clients[map[k]]))
				client_gone(map[k], "closed");
		}
		/* the page's loop: drain (and vkr's poll hook) every turn */
		rvgpu_ring_drain();
		check_waiters();
		if (live)
			last_client = now_ms();
		const double t = now_ms();
		if (stats && t - last_stats > 5000) {
			fprintf(stderr, "vkr-vtest: %u clients, %u commands, %u submits (%.1f us each in the executor), %u blobs "
				"(%u MiB), %u sync waits\n", st.clients, st.cmds, st.submits,
				st.submits ? st.submit_us / st.submits : 0, st.blobs, st.blob_mb, st.waits);
			last_stats = t;
		}
		if (exit_idle && st.clients && !live && t - last_client > exit_idle) {
			fprintf(stderr, "vkr-vtest: %u clients, %u commands, %u submits (%.1f us each in the executor), %u blobs "
				"(%u MiB), %u sync waits; idle: done\n", st.clients, st.cmds, st.submits,
				st.submits ? st.submit_us / st.submits : 0, st.blobs, st.blob_mb, st.waits);
			if (pst.regs)
				fprintf(stderr, "vkr-vtest: present: %u memories registered, %u fence retires, %u skipped without an image, %u read-backs of venus window images (%.1f MiB, %.2f ms each), %u failed\n",
					pst.regs, pst.calls, pst.noimage, pst.reads, pst.bytes / 1048576.0, pst.reads ? pst.ms / pst.reads : 0, pst.failed);
			const uint64_t all = vkr_ring_stats.cmds + vkr_ring_stats.nested;
			fprintf(stderr, "vkr-vtest: rings ran %llu venus commands + %llu in command streams (%.0f KiB) in %.1f ms: "
				"%.2f us each\n", (unsigned long long)vkr_ring_stats.cmds, (unsigned long long)vkr_ring_stats.nested,
				vkr_ring_stats.bytes / 1024.0, vkr_ring_stats.ns / 1e6, all ? vkr_ring_stats.ns / 1e3 / all : 0);
			unlink(sock);
			return 0;
		}
	}
}
