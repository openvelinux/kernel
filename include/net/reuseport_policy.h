/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _REUSEPORT_POLICY_H
#define _REUSEPORT_POLICY_H

#include <linux/filter.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <net/sock.h>

struct reuseport_policy {
	struct sock *(*select_sock)(struct sock *sk,
				    struct sock_reuseport *reuse,
				    u32 hash, u16 num_socks);
};

extern struct reuseport_policy __rcu *reuseport_policy;

int register_reuseport_policy(struct reuseport_policy *policy);
int unregister_reuseport_policy(struct reuseport_policy *policy);

#endif  /* _REUSEPORT_POLICY_H */
