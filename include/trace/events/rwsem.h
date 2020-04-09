/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM rwsem

#if !defined(_TRACE_RWSEM_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_RWSEM_H

#include <linux/types.h>
#include <linux/tracepoint.h>
#include <linux/rwsem.h>

DECLARE_EVENT_CLASS(rwsem,

	TP_PROTO(struct rw_semaphore *sem, unsigned long ip),

	TP_ARGS(sem, ip),

	TP_STRUCT__entry(
		__field(	struct rw_semaphore *,	sem	)
		__field(	unsigned long,		ip	)
	),

	TP_fast_assign(
		__entry->sem	= sem;
		__entry->ip	= ip;
	),

	TP_printk("sem=%px, ip=%lx", __entry->sem, __entry->ip)
);

DEFINE_EVENT(rwsem, rwsem_write_acquire,

	TP_PROTO(struct rw_semaphore *sem, unsigned long ip),

	TP_ARGS(sem, ip)
);

DEFINE_EVENT(rwsem, rwsem_write_acquired,

	TP_PROTO(struct rw_semaphore *sem, unsigned long ip),

	TP_ARGS(sem, ip)
);

DEFINE_EVENT(rwsem, rwsem_write_release,

	TP_PROTO(struct rw_semaphore *sem, unsigned long ip),

	TP_ARGS(sem, ip)
);

DEFINE_EVENT(rwsem, rwsem_write_downgrade,

	TP_PROTO(struct rw_semaphore *sem, unsigned long ip),

	TP_ARGS(sem, ip)
);

#endif /* _TRACE_RWSEM_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
