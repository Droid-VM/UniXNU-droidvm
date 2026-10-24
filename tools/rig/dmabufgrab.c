// Read a dma-buf another process holds (crosvm's udmabufs over guest memory: the scanout surfaces of a virtio-gpu
// guest): take the descriptor with pidfd_getfd, map it, clean and invalidate the lines (dc civac) so bytes the
// guest's GPU wrote are not shadowed by stale host cache lines, then copy it out. A dma-buf behind /proc/PID/fd/N
// cannot be opened by path, hence the pidfd.
//   dmabufgrab PID FD OFFSET LENGTH OUTPUT        static aarch64 build: aarch64-linux-gnu-gcc -static -O2
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc != 6) { fprintf(stderr, "usage: dmabufgrab PID FD OFFSET LENGTH OUTPUT\n"); return 2; }
	unsigned long long offset = strtoull(argv[3], 0, 0), length = strtoull(argv[4], 0, 0);
	unsigned long long page = 4096, start = offset & ~(page - 1), span = ((offset + length + page - 1) & ~(page - 1)) - start;
	int process = syscall(SYS_pidfd_open, atoi(argv[1]), 0);
	int fd = process < 0 ? -1 : syscall(SYS_pidfd_getfd, process, atoi(argv[2]), 0);
	if (fd < 0) { perror("pidfd_getfd"); return 1; }
	unsigned char *map = mmap(0, span, PROT_READ, MAP_SHARED, fd, start);
	if (map == MAP_FAILED) { perror("mmap"); return 1; }
	for (unsigned long long at = 0; at < span; at += 64)
		__asm__ volatile("dc civac, %0" :: "r"(map + at) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
	FILE *out = fopen(argv[5], "wb");
	if (!out) { perror("fopen"); return 1; }
	fwrite(map + (offset - start), 1, length, out);
	fclose(out);
	return 0;
}
