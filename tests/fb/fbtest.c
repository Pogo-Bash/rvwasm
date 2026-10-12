/*
 * Draw on the frame buffer the way a program does: ask the device for its
 * geometry, map it, write pixels. fbtest [SECONDS] keeps the picture up for
 * that long (default 3) and prints what it found.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int fd = open("/dev/fb0", O_RDWR), w, h;
	uint32_t *px;

	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
		perror("/dev/fb0");
		return 1;
	}
	w = (int)var.xres;
	h = (int)var.yres;
	printf("%s: %dx%d, %d bits per pixel, red at bit %d, green %d, blue %d, %d bytes per line\n",
	       fix.id, w, h, var.bits_per_pixel, var.red.offset, var.green.offset, var.blue.offset, fix.line_length);
	px = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	/* A gradient, three colour bars, a disc and a frame. */
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			px[y * w + x] = (uint32_t)(x * 255 / w) << var.red.offset |
					(uint32_t)(y * 255 / h) << var.green.offset |
					(uint32_t)96 << var.blue.offset;
	for (int bar = 0; bar < 3; bar++)
		for (int y = 40; y < 140; y++)
			for (int x = 40 + bar * 120; x < 140 + bar * 120; x++)
				px[y * w + x] = 255u << (bar == 0 ? var.red.offset : bar == 1 ? var.green.offset : var.blue.offset);
	for (int y = -120; y <= 120; y++)
		for (int x = -120; x <= 120; x++)
			if (x * x + y * y <= 120 * 120)
				px[(h / 2 + y) * w + w / 2 + x] = 0x00ffffff;
	for (int x = 0; x < w; x++)
		px[x] = px[(h - 1) * w + x] = 0x00ffffff;
	for (int y = 0; y < h; y++)
		px[y * w] = px[y * w + w - 1] = 0x00ffffff;

	fflush(stdout);
	sleep(argc > 1 ? (unsigned)atoi(argv[1]) : 3);
	munmap(px, fix.smem_len);
	close(fd);
	return 0;
}
