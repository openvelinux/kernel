/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM exceptions

#if !defined(_TRACE_PAGE_FAULT_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_PAGE_FAULT_H

#include <linux/tracepoint.h>
#include <asm/trace/common.h>

extern int trace_pagefault_reg(void);
extern void trace_pagefault_unreg(void);

DECLARE_EVENT_CLASS(x86_exceptions,

	TP_PROTO(unsigned long address, struct pt_regs *regs,
		 unsigned long error_code),

	TP_ARGS(address, regs, error_code),

	TP_STRUCT__entry(
		__field(		unsigned long, address	)
		__field(		unsigned long, ip	)
		__field(		unsigned long, error_code )
	),

	TP_fast_assign(
		__entry->address = address;
		__entry->ip = regs->ip;
		__entry->error_code = error_code;
	),

	TP_printk("address=%ps ip=%ps error_code=0x%lx",
		  (void *)__entry->address, (void *)__entry->ip,
		  __entry->error_code) );

#define DEFINE_PAGE_FAULT_EVENT(name)				\
DEFINE_EVENT_FN(x86_exceptions, name,				\
	TP_PROTO(unsigned long address,	struct pt_regs *regs,	\
		 unsigned long error_code),			\
	TP_ARGS(address, regs, error_code),			\
	trace_pagefault_reg, trace_pagefault_unreg);

DEFINE_PAGE_FAULT_EVENT(page_fault_user);
DEFINE_PAGE_FAULT_EVENT(page_fault_kernel);

TRACE_EVENT(segfault,

	TP_PROTO(unsigned long address, struct pt_regs *regs,
		 unsigned long error_code, struct task_struct *task),

	TP_ARGS(address, regs, error_code, task),

	TP_STRUCT__entry(
		__array(	char,	comm,	TASK_COMM_LEN	)
		__field(	pid_t,		pid		)
		__field(	unsigned long,	error_code	)
		__field(	unsigned long,	address		)
#ifndef __i386__
		__field(	unsigned long,	r15		)
		__field(	unsigned long,	r14		)
		__field(	unsigned long,	r13		)
		__field(	unsigned long,	r12		)
		__field(	unsigned long,	r11		)
		__field(	unsigned long,	r10		)
		__field(	unsigned long,	r9		)
		__field(	unsigned long,	r8		)
#endif
		__field(	unsigned long,	bp		)
		__field(	unsigned long,	bx		)
		__field(	unsigned long,	ax		)
		__field(	unsigned long,	cx		)
		__field(	unsigned long,	dx		)
		__field(	unsigned long,	si		)
		__field(	unsigned long,	di		)
		__field(	unsigned long,	ip		)
		__field(	unsigned long,	cs		)
		__field(	unsigned long,	flags		)
		__field(	unsigned long,	sp		)
		__field(	unsigned long,	ss		)
	),

	TP_fast_assign(
		memcpy(__entry->comm, task->comm, TASK_COMM_LEN);
		__entry->pid		= task_pid_nr(task);
		__entry->error_code	= error_code;
		__entry->address	= address;
#ifndef __i386__
		__entry->r15		= regs->r15;
		__entry->r14		= regs->r14;
		__entry->r13		= regs->r13;
		__entry->r12		= regs->r12;
		__entry->r11		= regs->r11;
		__entry->r10		= regs->r10;
		__entry->r9		= regs->r9;
		__entry->r8		= regs->r8;
#endif
		__entry->bp		= regs->bp;
		__entry->bx		= regs->bx;
		__entry->ax		= regs->ax;
		__entry->cx		= regs->cx;
		__entry->dx		= regs->dx;
		__entry->si		= regs->si;
		__entry->di		= regs->di;
		__entry->ip		= regs->ip;
		__entry->cs		= regs->cs;
		__entry->flags		= regs->flags;
		__entry->sp		= regs->sp;
		__entry->ss		= regs->ss;
	),

	TP_printk("%s[%d]: segfault at %lx ip %px sp %px error %lx",
		  __entry->comm, __entry->pid, __entry->address,
		  (void *)__entry->ip, (void *)__entry->sp, __entry->error_code)
);

#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE exceptions
#endif /*  _TRACE_PAGE_FAULT_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
