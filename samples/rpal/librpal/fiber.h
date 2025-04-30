#ifndef FIBER_H
#define FIBER_H

#include <stdlib.h>

typedef void *fcontext_t;
typedef struct {
	fcontext_t fctx;
	void *ud;
} transfer_t;

typedef struct fiber_stack {
	unsigned long padding;
	unsigned long r12;
	unsigned long r13;
	unsigned long r14;
	unsigned long r15;
	unsigned long rbx;
	unsigned long rbp;
	unsigned long rip;
} fiber_stack_t;

#define NR_PADDING 8
typedef struct fiber_ctx {
	void *sp;
	size_t size;
	void (*fn)(void *fc);
	void *ud;
	fcontext_t fctx;
	int padding[NR_PADDING];
} task_t;

task_t *fiber_ctx_alloc(void (*fn)(void *ud), void *ud, size_t size);
void fiber_ctx_free(task_t *fc);

/**
 * @brief Make a context for jump_fcontext.
 *
 * @param sp The stack top pointer of context.
 * @param size The size of stack, this argument is useless. But a second argument is neccessary.
 * @param fn The function pointer of the context function.
 *
 * @return The pointer of the newly made context.
 */
extern fcontext_t make_fcontext(void *sp, size_t size, void (*fn)(transfer_t));

/**
 * @brief jump to target context and execute fn with argument ud
 *
 * @param to The pointer of target context.
 * @param ud The data part of the argument of fn.
 *
 * @return the pointer of the prev transfer_t struct, where RAX store
 *  previous context, RDX store ud passed by previous caller.
 */
extern transfer_t jump_fcontext(fcontext_t const to, void *ud);

/**
 * @brief To be written.
 */
extern transfer_t ontop_fcontext(fcontext_t const to, void *ud,
				 transfer_t (*fn)(transfer_t));

#endif
