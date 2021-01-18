#undef TRACE_SYSTEM
#define TRACE_SYSTEM nat

#if !defined(_NAT_PT_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _NAT_PT_TRACE_H

#include <linux/tracepoint.h>

TRACE_EVENT(nat_get_tuple,
	TP_PROTO(struct nf_conntrack_tuple *new, 
		 struct nf_conntrack_tuple *orig,
		 const struct nf_nat_range2 *range, 
		 struct nf_conn *ct, 
		 enum nf_nat_manip_type maniptype),
	TP_ARGS(new, orig, range, ct, maniptype),
	TP_STRUCT__entry(
		__field(struct nf_conntrack_tuple *, new)
		__field(struct nf_conntrack_tuple *, orig)
		__field(const struct nf_nat_range2 *, range)
		__field(struct nf_conn *, ct)
		__field(enum nf_nat_manip_type, maniptype)
	),

	TP_fast_assign(
		__entry->new = new;
		__entry->orig = orig;
		__entry->range = range;
		__entry->ct = ct;
		__entry->maniptype = maniptype;
	),

	TP_printk("maniptype:%d", __entry->maniptype)
);
#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE nf_nat_tp
#include <trace/define_trace.h>
