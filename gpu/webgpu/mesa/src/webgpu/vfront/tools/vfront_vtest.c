/*
 * vfront-vtest: a vtest server (the protocol of virglrenderer's virgl_test_server, version 3) around vfront,
 * for testing natively in the build VM: stock Mesa's virgl driver with the vtest winsys
 * (GALLIUM_DRIVER=virpipe VTEST_SOCKET_NAME=...) draws through vfront on zink on lavapipe. It does what
 * gpuserver + the page executor do together (gpu/PROTOCOL.md), with the ring's semantics: resource ids made
 * here, backings in shared files the client maps, a flush per SUBMIT and per TRANSFER_GET, busy waits per
 * context.
 *
 *   vfront-vtest [--socket PATH] [--capture FILE] [--once]
 *
 * --capture writes everything vfront is given, with the backing bytes it reads and writes, as a capture
 * (tools/vf_capture.h) that vfront-replay and vrend-replay replay. --once exits when the first client has
 * gone and no other is connected.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "vfront.h"
#include "vf_capture.h"
#include "virgl_hw.h"

/* vtest_protocol.h (virglrenderer) */
#define VCMD_GET_CAPS 1
#define VCMD_RESOURCE_CREATE 2
#define VCMD_RESOURCE_UNREF 3
#define VCMD_TRANSFER_GET 4
#define VCMD_TRANSFER_PUT 5
#define VCMD_SUBMIT_CMD 6
#define VCMD_RESOURCE_BUSY_WAIT 7
#define VCMD_CREATE_RENDERER 8
#define VCMD_GET_CAPS2 9
#define VCMD_PING_PROTOCOL_VERSION 10
#define VCMD_PROTOCOL_VERSION 11
#define VCMD_RESOURCE_CREATE2 12
#define VCMD_TRANSFER_GET2 13
#define VCMD_TRANSFER_PUT2 14
#define VCMD_BUSY_WAIT_FLAG_WAIT 1

#define MAX_CLIENTS 32

struct res {
	uint32_t id, ctx;
	uint8_t *map;
	uint64_t size;
};

struct client {
	int fd;
	uint32_t ctx;
	uint32_t version;
	uint32_t flushed;		/* the serial of the last flush */
	char name[64];
	char cmdline[256];		/* the client process's command line (SO_PEERCRED, /proc), for crash reports */
};

static struct client clients[MAX_CLIENTS];
static struct client *current;	/* the client whose command is being handled */

/* A crash in the decoder or the driver takes every client down, so say whose command it was (the test
 * runner restarts the server; this tells it which test to blame). */
static void crash_handler(int sig)
{
	char msg[400];
	int n = snprintf(msg, sizeof(msg), "vfront-vtest: signal %d while handling %s (context %u): %s\n", sig,
			 current ? current->name : "nothing", current ? current->ctx : 0,
			 current ? current->cmdline : "");
	if (n > 0)
		(void)!write(2, msg, n < (int)sizeof(msg) ? n : (int)sizeof(msg) - 1);
	signal(sig, SIG_DFL);
	raise(sig);
}

static void client_cmdline(struct client *c)
{
	struct ucred cred;
	socklen_t len = sizeof(cred);
	c->cmdline[0] = 0;
	if (getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &cred, &len))
		return;
	char path[64];
	snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)cred.pid);
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	size_t n = fread(c->cmdline, 1, sizeof(c->cmdline) - 1, f);
	fclose(f);
	for (size_t i = 0; i < n; i++)
		if (!c->cmdline[i])
			c->cmdline[i] = ' ';
	c->cmdline[n] = 0;
}
static struct res *resources;
static unsigned num_resources, cap_resources;
static uint32_t next_res = 1, next_serial;
static FILE *capture;
static int verbose;	/* -v: contexts and failures; -v -v: every command */

/* ---- capture ---- */

static void cap_record(uint32_t op, uint32_t ctx, uint32_t serial, const void *a, uint32_t na, const void *b,
		       uint32_t nb)
{
	if (!capture)
		return;
	struct vfcap_hdr h = { op, ctx, serial, na + nb };
	fwrite(&h, sizeof(h), 1, capture);
	if (na)
		fwrite(a, 1, na, capture);
	if (nb)
		fwrite(b, 1, nb, capture);
}

static struct res *res_by_addr(uint64_t addr, uint64_t *offset)
{
	for (unsigned i = 0; i < num_resources; i++) {
		struct res *r = &resources[i];
		if (r->map && addr >= (uintptr_t)r->map && addr < (uintptr_t)r->map + r->size) {
			*offset = addr - (uintptr_t)r->map;
			return r;
		}
	}
	return NULL;
}

static void cap_mem(uint32_t op, uint64_t addr, const void *bytes, size_t size)
{
	uint64_t off = 0;
	struct res *r = res_by_addr(addr, &off);
	if (op == VFCAP_RESULT) {
		uint64_t hash = vfcap_hash(bytes, size);
		uint32_t h[5] = { r ? r->id : 0, (uint32_t)off, (uint32_t)size, (uint32_t)hash, (uint32_t)(hash >> 32) };
		cap_record(op, r ? r->ctx : 0, 0, h, sizeof(h), NULL, 0);
		return;
	}
	uint32_t h[3] = { r ? r->id : 0, (uint32_t)off, (uint32_t)size };
	cap_record(op, r ? r->ctx : 0, 0, h, sizeof(h), bytes, size);
}

static void cap_read(void *cookie, uint64_t addr, void *dst, size_t size)
{
	(void)cookie;
	memcpy(dst, (const void *)(uintptr_t)addr, size);
	cap_mem(VFCAP_DATA, addr, dst, size);
}

static void cap_write(void *cookie, uint64_t addr, const void *src, size_t size)
{
	(void)cookie;
	memcpy((void *)(uintptr_t)addr, src, size);
	cap_mem(VFCAP_RESULT, addr, src, size);
}

/* ---- io ---- */

static bool read_full(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	while (n) {
		ssize_t r = read(fd, p, n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		p += r;
		n -= r;
	}
	return true;
}

static bool write_full(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	while (n) {
		ssize_t r = write(fd, p, n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return false;
		p += r;
		n -= r;
	}
	return true;
}

static bool reply(int fd, uint32_t cmd, const uint32_t *data, uint32_t ndw)
{
	uint32_t h[2] = { ndw, cmd };
	return write_full(fd, h, sizeof(h)) && (!ndw || write_full(fd, data, ndw * 4));
}

static bool send_fd(int sock, int fd)
{
	char c = 0;
	struct iovec iov = { &c, 1 };
	char buf[CMSG_SPACE(sizeof(int))];
	memset(buf, 0, sizeof(buf));
	struct msghdr m = { 0 };
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = buf;
	m.msg_controllen = sizeof(buf);
	struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
	cm->cmsg_level = SOL_SOCKET;
	cm->cmsg_type = SCM_RIGHTS;
	cm->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cm), &fd, sizeof(int));
	return sendmsg(sock, &m, 0) == 1;
}

/* ---- resources ---- */

static struct res *res_find(uint32_t id)
{
	for (unsigned i = 0; i < num_resources; i++)
		if (resources[i].id == id)
			return &resources[i];
	return NULL;
}

static void res_remove(uint32_t ctx, uint32_t id)
{
	for (unsigned i = 0; i < num_resources; i++) {
		if (resources[i].id != id)
			continue;
		vf_res_unref(ctx, id);
		cap_record(VFCAP_RES_UNREF, ctx, 0, &id, 4, NULL, 0);
		if (resources[i].map)
			munmap(resources[i].map, resources[i].size);
		resources[i] = resources[--num_resources];
		return;
	}
}

/* ---- commands ---- */

static void flush(struct client *c)
{
	c->flushed = ++next_serial;
	vf_ctx_flush(c->ctx, c->flushed);
	cap_record(VFCAP_FLUSH, c->ctx, c->flushed, NULL, 0, NULL, 0);
}

static bool cmd_resource_create2(struct client *c, uint32_t len)
{
	uint32_t a[11];
	if (len != 11 || !read_full(c->fd, a, sizeof(a)))
		return false;
	/* a: handle (0), target, format, bind, width, height, depth, array_size, last_level, nr_samples, size */
	struct vf_res_create_args args = {
		.res = next_res++, .target = a[1], .format = a[2], .bind = a[3], .width = a[4], .height = a[5],
		.depth = a[6], .array_size = a[7], .last_level = a[8], .nr_samples = a[9], .flags = 0,
	};
	uint64_t size = a[10];
	int fd = -1;
	uint8_t *map = NULL;
	/* All clients' backings together stay under VFRONT_VTEST_MAX_MB (default 3072): a test allocating its
	 * way through the machine's memory should fail its own allocation, not get the server OOM-killed (and
	 * every other client with it). */
	static uint64_t limit;
	if (!limit) {
		const char *e = getenv("VFRONT_VTEST_MAX_MB");
		limit = (uint64_t)(e ? atoi(e) : 3072) << 20;
	}
	uint64_t used = 0;
	for (unsigned i = 0; i < num_resources; i++)
		used += resources[i].size;
	if (used + size > limit) {
		fprintf(stderr, "vfront-vtest: %s: resource of %llu MB refused (%llu MB in use)\n", c->name,
			(unsigned long long)(size >> 20), (unsigned long long)(used >> 20));
		return false;
	}
	if (size) {
		fd = memfd_create("vfront-res", MFD_CLOEXEC);
		if (fd < 0 || ftruncate(fd, size) < 0)
			return false;
		map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (map == MAP_FAILED)
			return false;
	}
	int ret = vf_res_create(c->ctx, &args, (uintptr_t)map, size);
	if (ret) {
		fprintf(stderr, "vfront-vtest: %s: resource %u failed (%d)\n", c->name, args.res, ret);
		/* the guest has no way to hear of it but a closed connection */
		if (map)
			munmap(map, size);
		if (fd >= 0)
			close(fd);
		return false;
	}
	if (num_resources == cap_resources) {
		cap_resources = cap_resources ? cap_resources * 2 : 256;
		resources = realloc(resources, cap_resources * sizeof(*resources));
	}
	resources[num_resources++] = (struct res){ args.res, c->ctx, map, size };
	uint32_t rec[14] = { args.res, args.target, args.format, args.bind, args.width, args.height, args.depth,
			     args.array_size, args.last_level, args.nr_samples, 0, (uint32_t)size, 0, 0 };
	cap_record(VFCAP_RES_CREATE, c->ctx, 0, rec, sizeof(rec), NULL, 0);
	uint32_t id = args.res;
	bool ok = reply(c->fd, VCMD_RESOURCE_CREATE2, &id, 1);
	if (ok && fd >= 0)
		ok = send_fd(c->fd, fd);
	if (fd >= 0)
		close(fd);
	return ok;
}

static bool cmd_transfer2(struct client *c, uint32_t len, bool get)
{
	uint32_t a[10];
	if (len < 10 || (get && len != 10) || !read_full(c->fd, a, sizeof(a)))
		return false;
	/* a: handle, level, x, y, z, w, h, d, data_size, offset */
	if (!get && a[8]) {
		/* A PUT2's len counts its data whether or not it comes inline: the guest sends data_size bytes
		 * after the header only for a resource without shared memory (virglrenderer reads them then
		 * too), and they go nowhere here (a multisampled texture: nothing to upload into). */
		bool shm = false;
		for (unsigned i = 0; i < num_resources; i++)
			if (resources[i].id == a[0] && resources[i].map)
				shm = true;
		if (!shm) {
			uint8_t *data = malloc(a[8]);
			bool ok = data && read_full(c->fd, data, a[8]);
			free(data);
			if (!ok)
				return false;
			return true;
		}
	}
	struct vf_box box = { a[2], a[3], a[4], a[5], a[6], a[7] };
	int ret = get ? vf_transfer_get(c->ctx, a[0], a[1], &box, a[9]) : vf_transfer_put(c->ctx, a[0], a[1], &box, a[9]);
	if (ret)
		fprintf(stderr, "vfront-vtest: %s: transfer %s of resource %u failed (%d)\n", c->name,
			get ? "get" : "put", a[0], ret);
	uint32_t rec[10] = { a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[9], a[8] };
	cap_record(get ? VFCAP_TRANSFER_GET : VFCAP_TRANSFER_PUT, c->ctx, 0, rec, sizeof(rec), NULL, 0);
	if (get)
		flush(c);
	return true;
}

static bool cmd_submit(struct client *c, uint32_t len)
{
	uint32_t *buf = malloc((size_t)len * 4 + 4);
	if (!buf || !read_full(c->fd, buf, (size_t)len * 4)) {
		free(buf);
		return false;
	}
	int ret = vf_submit(c->ctx, buf, len);
	if (ret && verbose)
		fprintf(stderr, "vfront-vtest: %s: submit of %u dwords failed (%d)\n", c->name, len, ret);
	cap_record(VFCAP_SUBMIT, c->ctx, 0, buf, len * 4, NULL, 0);
	free(buf);
	flush(c);
	return true;
}

static bool cmd_busy_wait(struct client *c, uint32_t len)
{
	uint32_t a[2];
	if (len != 2 || !read_full(c->fd, a, sizeof(a)))
		return false;
	uint32_t busy = 0;
	if (c->ctx) {
		if (a[1] & VCMD_BUSY_WAIT_FLAG_WAIT) {
			vf_ctx_poll(c->ctx, UINT64_MAX);
			cap_record(VFCAP_WAIT, c->ctx, c->flushed, NULL, 0, NULL, 0);
		} else {
			busy = vf_ctx_poll(c->ctx, 0) != c->flushed;
		}
	}
	return reply(c->fd, VCMD_RESOURCE_BUSY_WAIT, &busy, 1);
}

static bool cmd_caps(struct client *c, bool v2)
{
	static uint8_t caps[sizeof(union virgl_caps) + 64];
	uint32_t n = vf_caps(caps, sizeof(caps));
	if (!v2)
		n = sizeof(struct virgl_caps_v1);
	uint32_t h[2] = { n + 1, v2 ? 2 : 1 };
	return write_full(c->fd, h, sizeof(h)) && write_full(c->fd, caps, n);
}

static uint32_t free_ctx(void)
{
	for (uint32_t id = 1; id < 256; id++) {
		bool used = false;
		for (unsigned i = 0; i < MAX_CLIENTS; i++)
			used |= clients[i].fd > 0 && clients[i].ctx == id;
		if (!used)
			return id;
	}
	return 0;
}

static void client_gone(struct client *c)
{
	if (c->ctx) {
		for (unsigned i = 0; i < num_resources;) {
			if (resources[i].ctx == c->ctx)
				res_remove(c->ctx, resources[i].id);
			else
				i++;
		}
		vf_ctx_destroy(c->ctx);
		cap_record(VFCAP_CTX_DESTROY, c->ctx, 0, NULL, 0, NULL, 0);
	}
	if (verbose)
		fprintf(stderr, "vfront-vtest: %s (context %u) gone\n", c->name, c->ctx);
	close(c->fd);
	memset(c, 0, sizeof(*c));
}

static bool client_command_body(struct client *c, uint32_t cmd, uint32_t len);

/* One command; false when the client is to be dropped. */
static bool client_command(struct client *c)
{
	uint32_t h[2];
	if (!read_full(c->fd, h, sizeof(h)))
		return false;
	uint32_t len = h[0], cmd = h[1];
	if (verbose > 1)
		fprintf(stderr, "vfront-vtest: %s: command %u, %u dwords\n", c->name, cmd, len);
	bool ok = client_command_body(c, cmd, len);
	if (!ok)
		fprintf(stderr, "vfront-vtest: %s: dropped at command %u (%u dwords)\n", c->name, cmd, len);
	return ok;
}

static bool client_command_body(struct client *c, uint32_t cmd, uint32_t len)
{
	switch (cmd) {
	case VCMD_CREATE_RENDERER: {
		/* (the length is in bytes here) */
		char name[256];
		memset(name, 0, sizeof(name));
		if (len > sizeof(name) - 1) {
			char *tmp = malloc(len);
			bool ok = read_full(c->fd, tmp, len);
			memcpy(name, tmp, sizeof(name) - 1);
			free(tmp);
			if (!ok)
				return false;
		} else if (!read_full(c->fd, name, len)) {
			return false;
		}
		snprintf(c->name, sizeof(c->name), "%s", name);
		c->ctx = free_ctx();
		if (!c->ctx || vf_ctx_create(c->ctx, c->name))
			return false;
		cap_record(VFCAP_CTX_CREATE, c->ctx, 0, c->name, strlen(c->name) + 1, NULL, 0);
		if (verbose)
			fprintf(stderr, "vfront-vtest: %s is context %u\n", c->name, c->ctx);
		return true;
	}
	case VCMD_PING_PROTOCOL_VERSION:
		return reply(c->fd, VCMD_PING_PROTOCOL_VERSION, NULL, 0);
	case VCMD_PROTOCOL_VERSION: {
		uint32_t v;
		if (len != 1 || !read_full(c->fd, &v, 4))
			return false;
		/* versions 0-2 have client-made resource ids, which collide between clients */
		c->version = v >= 3 ? 3 : v;
		if (c->version < 3)
			fprintf(stderr, "vfront-vtest: %s asks for protocol %u; only 3 works\n", c->name, v);
		return reply(c->fd, VCMD_PROTOCOL_VERSION, &c->version, 1);
	}
	case VCMD_GET_CAPS2:
		return cmd_caps(c, true);
	case VCMD_GET_CAPS:
		return cmd_caps(c, false);
	case VCMD_RESOURCE_CREATE2:
		return cmd_resource_create2(c, len);
	case VCMD_RESOURCE_UNREF: {
		uint32_t id;
		if (len != 1 || !read_full(c->fd, &id, 4))
			return false;
		struct res *r = res_find(id);
		if (r && r->ctx == c->ctx)
			res_remove(c->ctx, id);
		return true;
	}
	case VCMD_TRANSFER_GET2:
		return cmd_transfer2(c, len, true);
	case VCMD_TRANSFER_PUT2:
		return cmd_transfer2(c, len, false);
	case VCMD_SUBMIT_CMD:
		return cmd_submit(c, len);
	case VCMD_RESOURCE_BUSY_WAIT:
		return cmd_busy_wait(c, len);
	}
	fprintf(stderr, "vfront-vtest: %s: command %u is not supported\n", c->name, cmd);
	return false;
}

int main(int argc, char **argv)
{
	const char *path = "/tmp/.virgl_test";
	const char *cap_path = NULL;
	bool once = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--socket") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "--capture") && i + 1 < argc)
			cap_path = argv[++i];
		else if (!strcmp(argv[i], "--once"))
			once = true;
		else if (!strcmp(argv[i], "-v"))
			verbose++;
		else {
			fprintf(stderr, "usage: vfront-vtest [--socket PATH] [--capture FILE] [--once] [-v]\n");
			return 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	signal(SIGSEGV, crash_handler);
	signal(SIGABRT, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGFPE, crash_handler);

	struct vf_mem_ops cap_ops = { NULL, NULL, cap_read, cap_write };
	struct vf_init_args ia = { 0 };
	if (cap_path) {
		capture = fopen(cap_path, "wb");
		if (!capture) {
			perror(cap_path);
			return 1;
		}
		fwrite(VFCAP_MAGIC, 1, 8, capture);
		ia.mem = &cap_ops;
	}
	if (vf_init(&ia)) {
		fprintf(stderr, "vfront-vtest: vfront did not start\n");
		return 1;
	}
	if (capture) {
		static uint8_t caps[sizeof(union virgl_caps) + 64];
		uint32_t n = vf_caps(caps, sizeof(caps));
		cap_record(VFCAP_CAPS, 0, 0, caps, n, NULL, 0);
	}
	fprintf(stderr, "vfront-vtest: %s on %s\n", vf_renderer_name(), path);

	int ls = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un un = { .sun_family = AF_UNIX };
	snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
	unlink(path);
	if (bind(ls, (struct sockaddr *)&un, sizeof(un)) < 0 || listen(ls, 8) < 0) {
		perror(path);
		return 1;
	}
	bool had_client = false;
	for (;;) {
		struct pollfd pf[MAX_CLIENTS + 1];
		int map[MAX_CLIENTS + 1];
		unsigned n = 0;
		pf[n].fd = ls;
		pf[n].events = POLLIN;
		map[n++] = -1;
		unsigned live = 0;
		for (unsigned i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd <= 0)
				continue;
			live++;
			pf[n].fd = clients[i].fd;
			pf[n].events = POLLIN;
			map[n++] = i;
		}
		if (once && had_client && !live)
			break;
		if (poll(pf, n, -1) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (pf[0].revents & POLLIN) {
			int fd = accept(ls, NULL, NULL);
			unsigned i;
			for (i = 0; i < MAX_CLIENTS && clients[i].fd > 0; i++)
				;
			if (fd >= 0 && i < MAX_CLIENTS) {
				clients[i].fd = fd;
				snprintf(clients[i].name, sizeof(clients[i].name), "client");
				client_cmdline(&clients[i]);
				had_client = true;
			} else if (fd >= 0) {
				close(fd);
			}
		}
		for (unsigned k = 1; k < n; k++) {
			if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			struct client *c = &clients[map[k]];
			current = c;
			if (!client_command(c))
				client_gone(c);
			current = NULL;
		}
		if (capture)
			fflush(capture);
	}
	struct vf_stats st;
	vf_get_stats(&st);
	fprintf(stderr, "vfront-vtest: %llu submits, %llu commands (%llu failed), %llu draws, %llu shaders "
		"(%llu failed, %.1f ms), %llu puts (%llu bytes), %llu gets (%llu bytes), %llu flushes\n",
		(unsigned long long)st.submits, (unsigned long long)st.commands, (unsigned long long)st.failed,
		(unsigned long long)st.draws, (unsigned long long)st.shaders, (unsigned long long)st.shader_failed,
		st.shader_ns / 1e6, (unsigned long long)st.puts, (unsigned long long)st.put_bytes,
		(unsigned long long)st.gets, (unsigned long long)st.get_bytes, (unsigned long long)st.flushes);
	if (capture)
		fclose(capture);
	vf_fini();
	unlink(path);
	return 0;
}
