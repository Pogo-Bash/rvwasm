/*
 * /proc/cpuinfo as rv makes it (rv/identity.c), for lists of the processors
 * online like the kernel's /sys/devices/system/cpu/online: one entry per
 * processor, under its own number, in order, nothing else numbered. On
 * the machine in the page the list has gaps by design (0-4,57-63: the
 * kernel's pool for programs that share CPUs takes them from the top),
 * and programs that read both, hwloc for one, must find the same numbers.
 * A native program, built with rv/identity.c (tools/cpuinfo-check.sh).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "rv.h"

static int failed;

/* The numbers after "processor" and "hart", which must be the same, as one list "a,b,c". */
static void check(const char *online, const char *expected)
{
	char text[65536], list[4096] = "", *line, *save;
	int fd = memfd_create("cpuinfo", 0), entries = 0, harts = 0, isas = 0;
	ssize_t n;

	if (fd < 0 || rv_cpuinfo_for(fd, online) || lseek(fd, 0, SEEK_SET) < 0 ||
	    (n = read(fd, text, sizeof(text) - 1)) < 0) {
		printf("%-12.*s cannot be made\n", (int)strcspn(online, "\n"), online);
		failed = 1;
		return;
	}
	close(fd);
	text[n] = '\0';
	for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		int cpu;

		if (sscanf(line, "processor\t: %d", &cpu) == 1) {
			snprintf(list + strlen(list), sizeof(list) - strlen(list), "%s%d", entries ? "," : "", cpu);
			entries++;
		} else if (sscanf(line, "hart\t\t: %d", &cpu) == 1) {
			int last = atoi(strrchr(list, ',') ? strrchr(list, ',') + 1 : list);

			harts += cpu == last;
		} else if (!strcmp(line, "isa\t\t: " RV_ISA) || !strcmp(line, "hart isa\t: " RV_ISA)) {
			isas++;
		}
	}
	if (strcmp(list, expected) || harts != entries || isas != 2 * entries) {
		printf("%-12.*s FAILED: processors %s (expected %s), %d harts and %d isa lines that agree\n", (int)strcspn(online, "\n"),
		       online, list, expected, harts, isas);
		failed = 1;
	} else {
		printf("%-12.*s ok: %s\n", (int)strcspn(online, "\n"), online, list);
	}
}

int main(void)
{
	check("0-4,57-63\n", "0,1,2,3,4,57,58,59,60,61,62,63");
	check("0\n", "0");
	check("0-11\n", "0,1,2,3,4,5,6,7,8,9,10,11");
	check("1,3,5-6\n", "1,3,5,6");
	check("63\n", "63");
	printf("cpuinfo: %s\n", failed ? "FAILED" : "all ok");
	return failed;
}
