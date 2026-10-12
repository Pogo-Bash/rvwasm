/* SPDX-License-Identifier: LicenseRef-rvwasm-NC-SA-1.0 */
/*
 * The driver's side of gpuserver's socket (gpu/server/vdec.js): vtest's framing ({length in dwords,
 * command} and the dwords), commands of our own (VDEC_*). A connection whose first command is VDEC_HELLO is
 * a video client. Only HELLO and ALLOC are answered (with a descriptor: the status file, a memory file); the
 * rest is said and not waited for: what the driver waits for (a frame decoded, drawn, copied; room in the
 * chunk area) it reads in the status file, which the page writes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "vdec.h"

static int put(int fd, const void *p, size_t n)
{
	const uint8_t *b = p;

	while (n) {
		ssize_t r = send(fd, b, n, MSG_NOSIGNAL);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		b += r;
		n -= (size_t)r;
	}
	return 0;
}

static int get(int fd, void *p, size_t n)
{
	uint8_t *b = p;

	while (n) {
		ssize_t r = recv(fd, b, n, 0);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		b += r;
		n -= (size_t)r;
	}
	return 0;
}

/* An answer's dwords (at most `max`; the header says how many). */
static int answer(int sock, uint32_t cmd, uint32_t *out, uint32_t max, uint32_t *n)
{
	uint32_t hdr[2];

	if (get(sock, hdr, sizeof(hdr)) || hdr[1] != cmd || hdr[0] > max)
		return -1;
	if (hdr[0] && get(sock, out, 4 * hdr[0]))
		return -1;
	*n = hdr[0];
	return 0;
}

/* A descriptor sent after an answer (SCM_RIGHTS, with one byte beside it, as vtest sends backing files). */
static int receive_fd(int sock)
{
	char byte, ctl[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { &byte, 1 };
	struct msghdr msg;
	struct cmsghdr *c;
	ssize_t r;
	int fd;

	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctl;
	msg.msg_controllen = sizeof(ctl);
	do
		r = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
	while (r < 0 && errno == EINTR);
	if (r <= 0 || !(c = CMSG_FIRSTHDR(&msg)) || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
		return -1;
	memcpy(&fd, CMSG_DATA(c), sizeof(int));
	return fd;
}

int conn_cmd(struct driver *d, uint32_t cmd, const uint32_t *args, uint32_t n)
{
	uint32_t hdr[2] = { n, cmd };
	int r;

	pthread_mutex_lock(&d->sock_lock);
	r = put(d->sock, hdr, sizeof(hdr)) || (n && put(d->sock, args, 4 * n)) ? -1 : 0;
	pthread_mutex_unlock(&d->sock_lock);
	return r;
}

static const char *socket_path(char *buf, size_t size)
{
	const char *s = getenv("RVVDEC_SOCKET");
	const char *dir = getenv("XDG_RUNTIME_DIR");

	if (!s || !*s) {
		/* (rv's render node names gpuserver's socket the same way: RV_VIRTGPU=PATH, or 1) */
		s = getenv("RV_VIRTGPU");
		if (s && (!strcmp(s, "1") || !strcmp(s, "0") || !*s))
			s = NULL;
	}
	if (s && *s)
		snprintf(buf, size, "%s", s);
	else
		snprintf(buf, size, "%s/rvgpu-0", dir && *dir ? dir : "/tmp");
	return buf;
}

static void msleep(int ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
		;
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * Connect and say hello; the answer is {status, n, n x {codec, profile, max_w, max_h, flags}} and the status
 * file. Status 1 (no page decoder yet: the page's Worker had not written its caps) is asked again, for up to
 * RVVDEC_WAIT seconds (default 5).
 */
int conn_open(struct driver *d)
{
	char path[108];
	struct sockaddr_un un;
	long until = now_ms() + 1000L * (getenv("RVVDEC_WAIT") ? atoi(getenv("RVVDEC_WAIT")) : 5);

	socket_path(path, sizeof(path));
	for (;;) {
		uint32_t version = VDEC_VERSION, a[2 + 5 * 32], n = 0;
		int fd = -1;

		d->sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (d->sock < 0)
			return -1;
		memset(&un, 0, sizeof(un));
		un.sun_family = AF_UNIX;
		snprintf(un.sun_path, sizeof(un.sun_path), "%s", path);
		if (connect(d->sock, (struct sockaddr *)&un, sizeof(un)) < 0) {
			vlog(d, "no GPU server at %s (%s): is the desktop started with RVGPU=1?\n", path, strerror(errno));
			close(d->sock);
			d->sock = -1;
			return -1;
		}
		{
			/* the program's name first, as vtest clients say it (gpuserver's log names clients by it) */
			char name[64] = "?";
			FILE *f = fopen("/proc/self/comm", "r");
			uint32_t hdr[2];

			if (f) {
				if (fgets(name, sizeof(name), f))
					name[strcspn(name, "\n")] = 0;
				fclose(f);
			}
			hdr[0] = (uint32_t)strlen(name) + 1;
			hdr[1] = 8;	/* VCMD_CREATE_RENDERER (its length is in bytes) */
			if (put(d->sock, hdr, sizeof(hdr)) || put(d->sock, name, hdr[0])) {
				close(d->sock);
				d->sock = -1;
				return -1;
			}
		}
		if (conn_cmd(d, VDEC_HELLO, &version, 1) || answer(d->sock, VDEC_HELLO, a, sizeof(a) / 4, &n) || n < 2 ||
		    n < 2 + 5 * a[1]) {
			vlog(d, "%s did not answer VDEC_HELLO: an older gpuserver?\n", path);
			close(d->sock);
			d->sock = -1;
			return -1;
		}
		if (a[0] == 0) {
			fd = receive_fd(d->sock);
			if (fd < 0) {
				vlog(d, "no status file from gpuserver\n");
				close(d->sock);
				d->sock = -1;
				return -1;
			}
			d->status = mmap(NULL, ST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
			if (d->status == MAP_FAILED) {
				vlog(d, "status file: mmap: %s\n", strerror(errno));
				close(fd);
				close(d->sock);
				d->sock = -1;
				d->status = NULL;
				return -1;
			}
			d->status_fd = fd;
			d->ncaps = 0;
			for (uint32_t i = 0; i < a[1] && i < 32; i++) {
				d->caps[i].codec = a[2 + 5 * i];
				d->caps[i].profile = a[3 + 5 * i];
				d->caps[i].max_w = a[4 + 5 * i];
				d->caps[i].max_h = a[5 + 5 * i];
				d->caps[i].flags = a[6 + 5 * i];
				d->ncaps++;
			}
			return 0;
		}
		close(d->sock);
		d->sock = -1;
		if (a[0] != 1 || now_ms() > until) {
			vlog(d, "gpuserver: %s\n", a[0] == 1 ? "no video decoder on the page (it said nothing in time)"
			     : a[0] == 4 ? "the page has no WebCodecs VideoDecoder" : "the page's video decoder is gone");
			return -1;
		}
		msleep(100);
	}
}

void conn_close(struct driver *d)
{
	if (d->status) {
		munmap(d->status, ST_SIZE);
		d->status = NULL;
	}
	if (d->status_fd >= 0) {
		close(d->status_fd);
		d->status_fd = -1;
	}
	if (d->sock >= 0) {
		close(d->sock);
		d->sock = -1;
	}
}

/* A memory file of `size` bytes from gpuserver (contiguous pages the page can reach), mapped here. */
int conn_alloc(struct driver *d, uint32_t size, struct mem *m)
{
	uint32_t a[2], n = 0;
	int fd = -1, r;

	pthread_mutex_lock(&d->sock_lock);
	{
		uint32_t hdr[3] = { 1, VDEC_ALLOC, size };

		r = put(d->sock, hdr, sizeof(hdr)) || answer(d->sock, VDEC_ALLOC, a, 2, &n) || n != 2 ? -1 : 0;
		if (!r && a[1] == 0)
			fd = receive_fd(d->sock);
	}
	pthread_mutex_unlock(&d->sock_lock);
	if (r || a[1] != 0 || fd < 0) {
		vlog(d, "no memory file of %u bytes from gpuserver\n", size);
		if (fd >= 0)
			close(fd);
		return -1;
	}
	m->p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m->p == MAP_FAILED) {
		vlog(d, "memory file of %u bytes: mmap: %s\n", size, strerror(errno));
		close(fd);
		conn_cmd(d, VDEC_FREE, &a[0], 1);
		m->p = NULL;
		return -1;
	}
	m->id = (int)a[0];
	m->fd = fd;
	m->size = size;
	return 0;
}

void conn_free(struct driver *d, struct mem *m)
{
	uint32_t id = (uint32_t)m->id;

	if (!m->p)
		return;
	munmap(m->p, m->size);
	close(m->fd);
	conn_cmd(d, VDEC_FREE, &id, 1);
	memset(m, 0, sizeof(*m));
	m->fd = -1;
}

uint32_t st_word(struct driver *d, uint32_t offset)
{
	return __atomic_load_n((uint32_t *)(d->status + offset), __ATOMIC_ACQUIRE);
}

uint32_t *st_slot(struct driver *d, uint32_t slot)
{
	return (uint32_t *)(d->status + ST_SLOTS + ST_SLOT_SIZE * slot);
}

int seq_reached(uint32_t done, uint32_t target)
{
	return (int32_t)(done - target) >= 0;
}

/*
 * Poll: first at once, then sleeping 0.1 ms and up to 2 ms between looks. The page is gone when gpuserver
 * says so or its beat has not moved for 3 s.
 */
int st_wait(struct driver *d, int (*cond)(struct driver *, void *), void *arg, int timeout_ms)
{
	long start = now_ms(), beat_at = start;
	uint32_t beat = st_word(d, ST_BEAT);
	int sleep_us = 100;

	for (;;) {
		long t;

		if (cond(d, arg))
			return 0;
		if (st_word(d, ST_STATE) == 2)
			return -1;
		t = now_ms();
		if (st_word(d, ST_BEAT) != beat) {
			beat = st_word(d, ST_BEAT);
			beat_at = t;
		} else if (t - beat_at > 3000) {
			vlog(d, "the page's video decoder has not moved for 3 s\n");
			return -1;
		}
		if (t - start > timeout_ms)
			return -1;
		{
			struct timespec ts = { 0, sleep_us * 1000L };

			nanosleep(&ts, NULL);
		}
		if (sleep_us < 2000)
			sleep_us *= 2;
	}
}
