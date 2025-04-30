#ifdef __x86_64__
#include "debug.h"
#include "fiber.h"
#include "private.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>

#define RPAL_CHECK_FAIL -1
#define STACK_DEBUG 1

static task_t *make_fiber_ctx(task_t *fc)
{
	fc->fctx = make_fcontext(fc->sp, 0, NULL);
	return fc;
}

static task_t *fiber_ctx_create(void (*fn)(void *ud), void *ud, void *stack,
				size_t size)
{
	task_t *fc;
	int i;

	if (stack == NULL)
		return NULL;

	fc = (task_t *)stack;
	fc->fn = fn;
	fc->ud = ud;
	fc->size = size;
	fc->sp = stack + size;
	for (i = 0; i < NR_PADDING; ++i) {
		fc->padding[i] = 0xdeadbeef;
	}

	return make_fiber_ctx(fc);
}

task_t *fiber_ctx_alloc(void (*fn)(void *ud), void *ud, size_t size)
{
	void *stack;
	size_t stack_size;
	size_t total_size;
	void *lower_guard;
	void *upper_guard;

	if (PAGE_SIZE == 4096 || STACK_DEBUG) {
		stack_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

		dbprint(RPAL_DEBUG_FIBER,
			"fiber_ctx_alloc: stack size adjusted from %lu to %lu\n",
			size, stack_size);

		// Allocate a stack using mmap with 2 extra pages, 1 at each end
		// which will be PROT_NONE to act as guard pages to catch overflow
		// and underflow. This will result in a SIGSEGV but should make it
		// easier to catch a stack that is too small (or underflows).
		//
		// Notes:
		//
		// 1. On ARM64 with 64K pages this would be quite wasteful of memory
		//    so it is behind a DEBUG flag to enable/disable on that platform.
		//
		// 2. If the requested stack size is not a multiple of a page size
		//    then stack underflow wont always be caught as there is some
		//    extra space up until the next page boundary with the guard page.
		//
		// 3. The task_t is placed at the top of the stack so can be overwritten
		//    just before the stack overflows and hits the guard page.
		//

		total_size = stack_size + (PAGE_SIZE * 2);
		lower_guard = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANON, -1, 0);
		if (lower_guard == MAP_FAILED) {
			errprint("mmap of %lu bytes failed: %s\n", total_size,
				 strerror(errno));
			return NULL;
		}

		stack = lower_guard + PAGE_SIZE;
		upper_guard = stack + stack_size;
		mprotect(lower_guard, PAGE_SIZE, PROT_NONE);
		mprotect(upper_guard, PAGE_SIZE, PROT_NONE);

		dbprint(RPAL_DEBUG_FIBER,
			"Total stack of size %lu bytes allocated @ %p\n",
			total_size, stack);
		dbprint(RPAL_DEBUG_FIBER,
			"Underflow guard page %p - %p overflow guard page %p - %p\n",
			lower_guard, lower_guard + PAGE_SIZE - 1, upper_guard,
			upper_guard + PAGE_SIZE - 1);
	} else {
		stack = malloc(size);
	}
	return fiber_ctx_create(fn, ud, stack, size);
}

void fiber_ctx_free(task_t *fc)
{
	size_t stack_size;
	size_t total_size;
	void *addr;

	if (STACK_DEBUG) {
		stack_size = (fc->size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
		total_size = stack_size + (PAGE_SIZE * 2);
		addr = fc;
		addr -= PAGE_SIZE;
		if (munmap(addr, total_size) != 0) {
			errprint("munmap of %lu bytes @ %p failed: %s\n",
				 total_size, addr, strerror(errno));
		}
	} else {
		free(fc);
	}
}
#endif
