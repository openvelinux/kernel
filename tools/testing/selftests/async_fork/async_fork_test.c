// SPDX-License-Identifier: GPL-2.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <sys/prctl.h>

#define swap(a, b) do {	\
	typeof(a) temp = a; \
	a = b; \
	b = temp; \
} while (0)

#ifndef PR_SET_ASYNC_FORK_ENABLE
#define PR_SET_ASYNC_FORK_ENABLE        0x4000
#endif

#ifndef MADV_COLD
#define MADV_COLD        20
#endif

#ifndef MADV_PAGEOUT
#define MADV_PAGEOUT        21
#endif

#define PAGE_SIZE 4096
#define STEP 64
unsigned long memory_size_bytes;
unsigned long per_vma_size_bytes;
unsigned long vma_count;
void **vmas;

void creat_vmas()
{
	vmas = malloc(vma_count * sizeof(void*));
	for (int i = 0; i < vma_count; i++) {
		vmas[i] = mmap(NULL, per_vma_size_bytes,
			       PROT_READ | PROT_WRITE | ((i & 1) ? PROT_EXEC : 0),
			       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (vmas[i] == MAP_FAILED) {
			perror("mmap failed");
			exit(EXIT_FAILURE);
		}
	}
}

#define PROC_MAPS_FILE "/proc/%d/maps"
void print_maps(void)
{
	pid_t pid = getpid();
	char maps_file[64];
	FILE *maps_fp;
	sprintf(maps_file, PROC_MAPS_FILE, pid);

	printf("Memory mappings:\n");

	maps_fp = fopen(maps_file, "r");
	if (maps_fp != NULL) {
		char line[1024];
		while (fgets(line, sizeof(line), maps_fp)) {
			printf("%s", line);
		}
		fclose(maps_fp);
	} else {
		perror("Failed to open /proc/pid/maps");
		exit(EXIT_FAILURE);
	}
}

void mm_init()
{
	for (int i = 0; i < vma_count; i++) {
		volatile unsigned char *p = vmas[i];

		for (int j = 0; j < per_vma_size_bytes; j += STEP, p += STEP) {
			if (j / PAGE_SIZE % 2 == 0) {
				*p = 0x7c;
			}
		}
	}
}

void do_child()
{
	for (int i = 0; i < vma_count; i++) {
		volatile unsigned char *p = vmas[i];

		for (int j = 0; j < per_vma_size_bytes; j += STEP, p += STEP) {
			if (j / PAGE_SIZE % 2 == 0) {
				assert(*p == 0x7c);
			} else {
				assert(*p == 0);
			}
		}
	}
}

void rand_wr()
{
	int cnt = 1000;

	for (int i = 0; i < cnt; i++) {
		int number = rand();
		int op = number % 2;
		int vma_index = number % vma_count;
		int vma_offset = number % per_vma_size_bytes;

		volatile unsigned char tmp;
		volatile unsigned char *p = vmas[vma_index] + vma_offset;

		if (op)
			tmp = *p; /* read */
		else
			*p = 0xcf; /* write */
	}
}

void rand_madvise()
{
	int cnt = 100;

	for (int i = 0; i < cnt; i++) {
		int number = rand();
		int op = number % 5;
		int vma_index = number % vma_count;
		int nr_pages = per_vma_size_bytes / PAGE_SIZE;
		unsigned long start = rand() % nr_pages;
		unsigned long end = rand() % nr_pages;

		if (start > end)
			swap(start, end);

		void *addr = vmas[vma_index] + start * PAGE_SIZE;
		unsigned long len = (end - start + 1) * PAGE_SIZE;

		int res = 0;
		switch (op) {
			case 0:
				res = madvise(addr, len, MADV_DONTNEED);
				break;
			case 1:
				res = madvise(addr, len, MADV_FREE);
				break;
			case 2:
				res = madvise(addr, len, MADV_WILLNEED);
				break;
			case 3:
				res = madvise(addr, len, MADV_COLD);
				break;
			case 4:
				res = madvise(addr, len, MADV_PAGEOUT);
				break;
		}

		if(res)
			exit(EXIT_FAILURE);
	}
}

void do_parent()
{
	rand_wr();
	rand_madvise();
}

double get_time_ms(struct timespec start, struct timespec end)
{
	return (end.tv_sec - start.tv_sec) * 1000.0 +
	       (end.tv_nsec - start.tv_nsec) / 1000000.0;
}

int main(int argc, char *argv[])
{
	int err, status, exitcode, bysignal = 0;
	struct timespec start, end, end2;
	pid_t pid;

	if (argc != 3) {
		printf("Usage: %s <memory_size_in_gb> <vma_count>\n", argv[0]);
		return 1;
	}

	memory_size_bytes = atoi(argv[1]) * 1024UL * 1024 * 1024;
	vma_count = atoi(argv[2]);
	per_vma_size_bytes = memory_size_bytes / vma_count;

	creat_vmas();
	/* print_maps(); */
	mm_init();

	/* Enable async-fork.*/
	err = prctl(PR_SET_ASYNC_FORK_ENABLE, 1);
	if (err)
                perror("Failed to enable async-fork.\n");

	clock_gettime(CLOCK_MONOTONIC, &start);
	pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	if (pid == 0) {
		printf("child: %.7fms\n", get_time_ms(start, end));
		do_child();
	} else {
		clock_gettime(CLOCK_MONOTONIC, &end);
		do_parent();
		clock_gettime(CLOCK_MONOTONIC, &end2);
		printf("parent: %.7fms %.7fms\n", get_time_ms(start, end),
			get_time_ms(start, end2));
		wait(&status);
		exitcode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		if (WIFSIGNALED(status))
			bysignal = WTERMSIG(status);
		printf("child status: %d, exitcode: %d, bysignal: %d\n", status,
			exitcode, bysignal);
	}

	return 0;
}
