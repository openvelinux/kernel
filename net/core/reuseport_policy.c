// SPDX-License-Identifier: GPL-2.0

#include "linux/compiler.h"
#include <net/ip.h>
#include <net/sock_reuseport.h>
#include <net/reuseport_policy.h>
#include <linux/bpf.h>
#include <linux/idr.h>
#include <linux/filter.h>
#include <linux/rcupdate.h>

struct reuseport_policy __rcu *reuseport_policy __read_mostly;

int register_reuseport_policy(struct reuseport_policy *policy)
{
	return !cmpxchg((const struct reuseport_policy **)&reuseport_policy,
			NULL, policy) ? 0 : -1;
}
EXPORT_SYMBOL_GPL(register_reuseport_policy);

int unregister_reuseport_policy(struct reuseport_policy *policy)
{
	int ret = (cmpxchg((const struct reuseport_policy **)&reuseport_policy,
		       policy, NULL) == policy) ? 0 : -1;

	synchronize_rcu();

	return ret;
}
EXPORT_SYMBOL_GPL(unregister_reuseport_policy);
