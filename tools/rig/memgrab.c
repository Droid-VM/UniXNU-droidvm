// Read guest memory of a running crosvm coherently: map its guest memfd (via /proc/PID/fd/N), clean and
// invalidate the lines (dc civac) so data the guest wrote through a non-cacheable mapping is not shadowed by
// stale host cache lines, then copy it out.
//   memgrab /proc/PID/fd/N FILE_OFFSET LENGTH OUTPUT
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 5) { fprintf(stderr, "usage: memgrab FD_PATH OFFSET LENGTH OUTPUT\n"); return 2; }
	unsigned long long offset = strtoull(argv[2], 0, 0), length = strtoull(argv[3], 0, 0);
	unsigned long long page = 4096, start = offset & ~(page - 1), span = ((offset + length + page - 1) & ~(page - 1)) - start;
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	unsigned char *map = mmap(0, span, PROT_READ, MAP_SHARED, fd, start);
	if (map == MAP_FAILED) { perror("mmap"); return 1; }
	for (unsigned long long at = 0; at < span; at += 64)
		__asm__ volatile("dc civac, %0" :: "r"(map + at) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	FILE *out = fopen(argv[4], "wb");
	if (!out) { perror("fopen"); return 1; }
	fwrite(map + (offset - start), 1, length, out);
	fclose(out);
	return 0;
}
