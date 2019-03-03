/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM udp

#if !defined(_TRACE_UDP_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_UDP_H

#include <linux/udp.h>
#include <linux/tracepoint.h>

TRACE_EVENT(udp_fail_queue_rcv_skb,

	TP_PROTO(int rc, struct sock *sk),

	TP_ARGS(rc, sk),

	TP_STRUCT__entry(
		__field(int, rc)
		__field(__u16, lport)
	),

	TP_fast_assign(
		__entry->rc = rc;
		__entry->lport = inet_sk(sk)->inet_num;
	),

	TP_printk("rc=%d port=%hu", __entry->rc, __entry->lport)
);

DECLARE_EVENT_CLASS(udp_stream_length,

    TP_PROTO(struct sock *sk, int length, int error, int flags),

    TP_ARGS(sk, length, error, flags),

    TP_STRUCT__entry(
        __field(void *, sk)
        __field(int, length)
        __field(int, error)
        __field(int, flags)
    ),

    TP_fast_assign(
        __entry->sk = sk;
        __entry->length = length;
        __entry->error = error;
        __entry->flags = flags;
    ),

    TP_printk("sk address = %p, length = %d, error=%d, flags = %u ",
        __entry->sk, __entry->length, __entry->error, __entry->flags)
);

DEFINE_EVENT(udp_stream_length, udp_send_length,
    TP_PROTO(struct sock *sk, int length, int error, int flags),

    TP_ARGS(sk, length, error, flags)
);

DEFINE_EVENT(udp_stream_length, udp_recv_length,
    TP_PROTO(struct sock *sk, int length, int error, int flags),

    TP_ARGS(sk, length, error, flags)
);


#endif /* _TRACE_UDP_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
