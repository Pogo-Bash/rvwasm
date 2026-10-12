/*
 * Floating point, timed: the kinds of loops that numeric and graphics code
 * is made of. Prints a checksum of each result (the bits, so that emulators
 * can be compared exactly) and the time it took.
 *
 *   fpbench [SCALE]   (default 1)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static uint64_t bits(double d)
{
	uint64_t u;

	memcpy(&u, &d, 8);
	return u;
}

/* Bodies attracting each other: add, subtract, multiply, divide, square root. */
static double nbody(int steps)
{
	enum { N = 5 };
	double x[N], y[N], z[N], vx[N], vy[N], vz[N], m[N], e = 0;

	for (int i = 0; i < N; i++) {
		x[i] = i * 1.5 + 0.1;
		y[i] = i * -0.7 + 0.3;
		z[i] = i * 0.2 - 0.4;
		vx[i] = 0.01 * i;
		vy[i] = 0.02 * (N - i);
		vz[i] = -0.005 * i;
		m[i] = 1.0 + 0.3 * i;
	}
	for (int s = 0; s < steps; s++) {
		for (int i = 0; i < N; i++) {
			for (int j = i + 1; j < N; j++) {
				double dx = x[i] - x[j], dy = y[i] - y[j], dz = z[i] - z[j];
				double d2 = dx * dx + dy * dy + dz * dz + 0.01;
				double mag = 0.001 / (d2 * sqrt(d2));

				vx[i] -= dx * m[j] * mag;
				vy[i] -= dy * m[j] * mag;
				vz[i] -= dz * m[j] * mag;
				vx[j] += dx * m[i] * mag;
				vy[j] += dy * m[i] * mag;
				vz[j] += dz * m[i] * mag;
			}
		}
		for (int i = 0; i < N; i++) {
			x[i] += 0.001 * vx[i];
			y[i] += 0.001 * vy[i];
			z[i] += 0.001 * vz[i];
		}
	}
	for (int i = 0; i < N; i++)
		e += m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i]) + x[i] + y[i] + z[i];
	return e;
}

/* The Mandelbrot set: multiply, add, compare. */
static double mandel(int size)
{
	long inside = 0;
	double sum = 0;

	for (int py = 0; py < size; py++) {
		for (int px = 0; px < size; px++) {
			double cr = -2.0 + 3.0 * px / size, ci = -1.5 + 3.0 * py / size;
			double zr = 0, zi = 0;
			int k;

			for (k = 0; k < 100 && zr * zr + zi * zi < 4.0; k++) {
				double t = zr * zr - zi * zi + cr;

				zi = 2 * zr * zi + ci;
				zr = t;
			}
			inside += k == 100;
			sum += zr;
		}
	}
	return sum + (double)inside;
}

/* Pixels: single precision, conversions to and from integers, as in blending and scaling. */
static double pixels(int count)
{
	uint32_t seed = 12345, acc = 0;
	float gain = 0.75f, total = 0;

	for (int i = 0; i < count; i++) {
		float r, g, b, l;
		uint32_t out;

		seed = seed * 1664525u + 1013904223u;
		r = (float)((seed >> 16) & 255) / 255.0f;
		g = (float)((seed >> 8) & 255) / 255.0f;
		b = (float)(seed & 255) / 255.0f;
		l = 0.299f * r + 0.587f * g + 0.114f * b;
		r = r * gain + l * (1.0f - gain);
		out = (uint32_t)(r * 255.0f + 0.5f) << 16 | (uint32_t)(g * 255.0f) << 8 | (uint32_t)(b * 255.0f);
		acc += out;
		total += l;
	}
	return (double)acc + (double)total;
}

/* The library's functions, which are loops of polynomial arithmetic. */
static double library(int count)
{
	double sum = 0;

	for (int i = 1; i <= count; i++) {
		double v = i * 0.001;

		sum += sin(v) * cos(v * 0.5) + exp(-v * 0.01) + log(v + 1.0) + atan2(v, 2.0) + pow(v, 0.3);
	}
	return sum;
}

int main(int argc, char **argv)
{
	int scale = argc > 1 ? atoi(argv[1]) : 1;
	long start, total = 0, ms;
	double r;

#define RUN(name, call) \
	start = now_ms(); r = call; ms = now_ms() - start; total += ms; \
	printf("%-8s %016llx %6ld ms\n", name, (unsigned long long)bits(r), ms)

	RUN("nbody", nbody(200000 * scale));
	RUN("mandel", mandel(300 * scale));
	RUN("pixels", pixels(3000000 * scale));
	RUN("library", library(300000 * scale));
	printf("%-8s %16s %6ld ms\n", "total", "", total);
	return 0;
}
