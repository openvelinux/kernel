// SPDX-License-Identifier: GPL-2.0
#include <linux/kallsyms.h>
#include <linux/cgroup.h>

#define LOOKUP_SYMS(name)						\
do {									\
	m_##name = (void *)kallsyms_lookup_name(#name);			\
	if (!m_##name) {						\
		pr_err("kallsyms loopup failed: %s\n", #name);		\
		return -ENXIO;						\
	}								\
} while (0)

static int (*m_cgroup_add_dfl_cftypes)(struct cgroup_subsys *ss, struct cftype *cfts);
static int (*m_cgroup_rm_cftypes)(struct cftype *cfts);
static struct cgroup_subsys *m_cpu_cgrp_subsys;

static inline void get_cgroup_percpu_subtree_bstat(struct cgroup *cgrp, int cpu,
						struct cgroup_base_stat *subtree_bstat)
{
	struct cgroup_rstat_cpu *rstatc;
	unsigned int seq;

	rstatc = per_cpu_ptr(cgrp->rstat_cpu, cpu);
	if (likely(rstatc)) {
		do {
			seq = __u64_stats_fetch_begin(&rstatc->bsync);
			*subtree_bstat = rstatc->subtree_bstat;
		} while (__u64_stats_fetch_retry(&rstatc->bsync, seq));
	}
}

#define DEF_CSTAT_PERCPU_SHOW_FN(name)						\
static int cstat_percpu_show_##name(struct seq_file *m, void *v)		\
{										\
	struct cgroup_base_stat subtree_bstat;					\
	struct cgroup *cgrp;							\
	int cpu;								\
	cgrp = seq_css(m)->cgroup;						\
	for_each_possible_cpu(cpu) {						\
		get_cgroup_percpu_subtree_bstat(cgrp, cpu, &subtree_bstat);	\
		seq_printf(m, "%llu ", subtree_bstat.cputime.name);		\
	}									\
	seq_putc(m, '\n');							\
	return 0;								\
}

DEF_CSTAT_PERCPU_SHOW_FN(stime)
DEF_CSTAT_PERCPU_SHOW_FN(utime)
DEF_CSTAT_PERCPU_SHOW_FN(sum_exec_runtime)
#undef DEF_CSTAT_PERCPU_SHOW_FN

static int cstat_percpu_show_all(struct seq_file *m, void *v)
{
	struct cgroup_base_stat subtree_bstat;
	struct cgroup *cgrp;
	int cpu;

	cgrp = seq_css(m)->cgroup;
	for_each_possible_cpu(cpu) {
		get_cgroup_percpu_subtree_bstat(cgrp, cpu, &subtree_bstat);
		seq_printf(m, "%llu %llu %llu\n", subtree_bstat.cputime.sum_exec_runtime,
				subtree_bstat.cputime.utime, subtree_bstat.cputime.stime);
	}
	return 0;
}

static struct cftype cstat_files[] = {
	{
		.name = "stat_percpu_usage",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cstat_percpu_show_sum_exec_runtime,
	},
	{
		.name = "stat_percpu_sys",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cstat_percpu_show_stime,
	},
	{
		.name = "stat_percpu_user",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cstat_percpu_show_utime,
	},
	{
		.name = "stat_percpu_all",
		.flags = CFTYPE_NOT_ON_ROOT,
		.seq_show = cstat_percpu_show_all,
	},
	{ } /* terminate */
};

static int __init cstat_percpu_init(void)
{
	LOOKUP_SYMS(cgroup_add_dfl_cftypes);
	LOOKUP_SYMS(cgroup_rm_cftypes);
	LOOKUP_SYMS(cpu_cgrp_subsys);

	return m_cgroup_add_dfl_cftypes(m_cpu_cgrp_subsys, cstat_files);
}

static void __exit cstat_percpu_exit(void)
{
	m_cgroup_rm_cftypes(cstat_files);
}

module_init(cstat_percpu_init);
module_exit(cstat_percpu_exit);
MODULE_LICENSE("GPL v2");
