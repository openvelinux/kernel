// SPDX-License-Identifier: GPL-2.0-only
#include "cgroup-internal.h"

#include <linux/sched/cputime.h>

#include <trace/events/cgroup.h>

static DEFINE_SPINLOCK(cgroup_rstat_lock);
static DEFINE_PER_CPU(raw_spinlock_t, cgroup_rstat_cpu_lock);

static void cgroup_base_stat_flush(struct cgroup *cgrp, int cpu);

static struct cgroup_rstat_cpu *cgroup_rstat_cpu(struct cgroup *cgrp, int cpu)
{
	return per_cpu_ptr(cgrp->rstat_cpu, cpu);
}

/**
 * cgroup_rstat_updated - keep track of updated rstat_cpu
 * @cgrp: target cgroup
 * @cpu: cpu on which rstat_cpu was updated
 *
 * @cgrp's rstat_cpu on @cpu was updated.  Put it on the parent's matching
 * rstat_cpu->updated_children list.  See the comment on top of
 * cgroup_rstat_cpu definition for details.
 */
void cgroup_rstat_updated(struct cgroup *cgrp, int cpu)
{
	raw_spinlock_t *cpu_lock = per_cpu_ptr(&cgroup_rstat_cpu_lock, cpu);
	unsigned long flags;

	/*
	 * Speculative already-on-list test. This may race leading to
	 * temporary inaccuracies, which is fine.
	 *
	 * Because @parent's updated_children is terminated with @parent
	 * instead of NULL, we can tell whether @cgrp is on the list by
	 * testing the next pointer for NULL.
	 */
	if (cgroup_rstat_cpu(cgrp, cpu)->updated_next)
		return;

	raw_spin_lock_irqsave(cpu_lock, flags);

	/* put @cgrp and all ancestors on the corresponding updated lists */
	while (true) {
		struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(cgrp, cpu);
		struct cgroup *parent = cgroup_parent(cgrp);
		struct cgroup_rstat_cpu *prstatc;

		/*
		 * Both additions and removals are bottom-up.  If a cgroup
		 * is already in the tree, all ancestors are.
		 */
		if (rstatc->updated_next)
			break;

		/* Root has no parent to link it to, but mark it busy */
		if (!parent) {
			rstatc->updated_next = cgrp;
			break;
		}

		prstatc = cgroup_rstat_cpu(parent, cpu);
		rstatc->updated_next = prstatc->updated_children;
		prstatc->updated_children = cgrp;

		cgrp = parent;
	}

	raw_spin_unlock_irqrestore(cpu_lock, flags);
}

/**
 * cgroup_rstat_push_children - push children cgroups into the given list
 * @head: current head of the list (= subtree root)
 * @child: first child of the root
 * @cpu: target cpu
 * Return: A new singly linked list of cgroups to be flush
 *
 * Iteratively traverse down the cgroup_rstat_cpu updated tree level by
 * level and push all the parents first before their next level children
 * into a singly linked list built from the tail backward like "pushing"
 * cgroups into a stack. The root is pushed by the caller.
 */
static struct cgroup *cgroup_rstat_push_children(struct cgroup *head,
						 struct cgroup *child, int cpu)
{
	struct cgroup *chead = child;	/* Head of child cgroup level */
	struct cgroup *ghead = NULL;	/* Head of grandchild cgroup level */
	struct cgroup *parent, *grandchild;
	struct cgroup_rstat_cpu *crstatc;

	child->rstat_flush_next = NULL;

next_level:
	while (chead) {
		child = chead;
		chead = child->rstat_flush_next;
		parent = cgroup_parent(child);

		/* updated_next is parent cgroup terminated */
		while (child != parent) {
			child->rstat_flush_next = head;
			head = child;
			crstatc = cgroup_rstat_cpu(child, cpu);
			grandchild = crstatc->updated_children;
			if (grandchild != child) {
				/* Push the grand child to the next level */
				crstatc->updated_children = child;
				grandchild->rstat_flush_next = ghead;
				ghead = grandchild;
			}
			child = crstatc->updated_next;
			crstatc->updated_next = NULL;
		}
	}

	if (ghead) {
		chead = ghead;
		ghead = NULL;
		goto next_level;
	}
	return head;
}

/**
 * cgroup_rstat_updated_list - return a list of updated cgroups to be flushed
 * @root: root of the cgroup subtree to traverse
 * @cpu: target cpu
 * Return: A singly linked list of cgroups to be flushed
 *
 * Walks the updated rstat_cpu tree on @cpu from @root.  During traversal,
 * each returned cgroup is unlinked from the updated tree.
 *
 * The only ordering guarantee is that, for a parent and a child pair
 * covered by a given traversal, the child is before its parent in
 * the list.
 *
 * Note that updated_children is self terminated and points to a list of
 * child cgroups if not empty. Whereas updated_next is like a sibling link
 * within the children list and terminated by the parent cgroup. An exception
 * here is the cgroup root whose updated_next can be self terminated.
 */
static struct cgroup *cgroup_rstat_updated_list(struct cgroup *root, int cpu)
{
	raw_spinlock_t *cpu_lock = per_cpu_ptr(&cgroup_rstat_cpu_lock, cpu);
	struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(root, cpu);
	struct cgroup *head = NULL, *parent, *child;
	unsigned long flags;

	/*
	 * The _irqsave() is needed because cgroup_rstat_lock is
	 * spinlock_t which is a sleeping lock on PREEMPT_RT. Acquiring
	 * this lock with the _irq() suffix only disables interrupts on
	 * a non-PREEMPT_RT kernel. The raw_spinlock_t below disables
	 * interrupts on both configurations. The _irqsave() ensures
	 * that interrupts are always disabled and later restored.
	 */
	raw_spin_lock_irqsave(cpu_lock, flags);

	/* Return NULL if this subtree is not on-list */
	if (!rstatc->updated_next)
		goto unlock_ret;

	/*
	 * Unlink @root from its parent. As the updated_children list is
	 * singly linked, we have to walk it to find the removal point.
	 */
	parent = cgroup_parent(root);
	if (parent) {
		struct cgroup_rstat_cpu *prstatc;
		struct cgroup **nextp;

		prstatc = cgroup_rstat_cpu(parent, cpu);
		nextp = &prstatc->updated_children;
		while (*nextp != root) {
			struct cgroup_rstat_cpu *nrstatc;

			nrstatc = cgroup_rstat_cpu(*nextp, cpu);
			WARN_ON_ONCE(*nextp == parent);
			nextp = &nrstatc->updated_next;
		}
		*nextp = rstatc->updated_next;
	}

	rstatc->updated_next = NULL;

	/* Push @root to the list first before pushing the children */
	head = root;
	root->rstat_flush_next = NULL;
	child = rstatc->updated_children;
	rstatc->updated_children = root;
	if (child != root)
		head = cgroup_rstat_push_children(head, child, cpu);
unlock_ret:
	raw_spin_unlock_irqrestore(cpu_lock, flags);
	return head;
}

/*
 * Helper functions for locking cgroup_rstat_lock.
 *
 * This makes it easier to diagnose locking issues and contention in
 * production environments.  The parameter @cpu_in_loop indicate lock
 * was released and re-taken when collection data from the CPUs. The
 * value -1 is used when obtaining the main lock else this is the CPU
 * number processed last.
 */
static inline void __cgroup_rstat_lock(struct cgroup *cgrp, int cpu_in_loop)
	__acquires(&cgroup_rstat_lock)
{
	bool contended;

	contended = !spin_trylock_irq(&cgroup_rstat_lock);
	if (contended) {
		trace_cgroup_rstat_lock_contended(cgrp, cpu_in_loop, contended);
		spin_lock_irq(&cgroup_rstat_lock);
	}
	trace_cgroup_rstat_locked(cgrp, cpu_in_loop, contended);
}

static inline void __cgroup_rstat_unlock(struct cgroup *cgrp, int cpu_in_loop)
	__releases(&cgroup_rstat_lock)
{
	trace_cgroup_rstat_unlock(cgrp, cpu_in_loop, false);
	spin_unlock_irq(&cgroup_rstat_lock);
}

/**
 * cgroup_rstat_flush - flush stats in @cgrp's subtree
 * @cgrp: target cgroup
 *
 * Collect all per-cpu stats in @cgrp's subtree into the global counters
 * and propagate them upwards.  After this function returns, all cgroups in
 * the subtree have up-to-date ->stat.
 *
 * This also gets all cgroups in the subtree including @cgrp off the
 * ->updated_children lists.
 *
 * This function may block.
 */
void cgroup_rstat_flush(struct cgroup *cgrp)
{
	int cpu;

	might_sleep();
	for_each_possible_cpu(cpu) {
		struct cgroup *pos;

		/* Reacquire for each CPU to avoid disabling IRQs too long */
		__cgroup_rstat_lock(cgrp, cpu);
		pos = cgroup_rstat_updated_list(cgrp, cpu);
		for (; pos; pos = pos->rstat_flush_next) {
			struct cgroup_subsys_state *css;

			cgroup_base_stat_flush(pos, cpu);

			rcu_read_lock();
			list_for_each_entry_rcu(css, &pos->rstat_css_list,
						rstat_css_node)
				css->ss->css_rstat_flush(css, cpu);
			rcu_read_unlock();
		}
		__cgroup_rstat_unlock(cgrp, cpu);
		if (!cond_resched())
			cpu_relax();
	}
}

int cgroup_rstat_init(struct cgroup *cgrp)
{
	int cpu;

	/* the root cgrp has rstat_cpu preallocated */
	if (!cgrp->rstat_cpu) {
		cgrp->rstat_cpu = alloc_percpu(struct cgroup_rstat_cpu);
		if (!cgrp->rstat_cpu)
			return -ENOMEM;
	}

	/* ->updated_children list is self terminated */
	for_each_possible_cpu(cpu) {
		struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(cgrp, cpu);

		rstatc->updated_children = cgrp;
		u64_stats_init(&rstatc->bsync);
	}

	return 0;
}

void cgroup_rstat_exit(struct cgroup *cgrp)
{
	int cpu;

	cgroup_rstat_flush(cgrp);

	/* sanity check */
	for_each_possible_cpu(cpu) {
		struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(cgrp, cpu);

		if (WARN_ON_ONCE(rstatc->updated_children != cgrp) ||
		    WARN_ON_ONCE(rstatc->updated_next))
			return;
	}

	free_percpu(cgrp->rstat_cpu);
	cgrp->rstat_cpu = NULL;
}

void __init cgroup_rstat_boot(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		raw_spin_lock_init(per_cpu_ptr(&cgroup_rstat_cpu_lock, cpu));
}

/*
 * Functions for cgroup basic resource statistics implemented on top of
 * rstat.
 */
static void cgroup_base_stat_add(struct cgroup_base_stat *dst_bstat,
				 struct cgroup_base_stat *src_bstat)
{
	dst_bstat->cputime.utime += src_bstat->cputime.utime;
	dst_bstat->cputime.stime += src_bstat->cputime.stime;
	dst_bstat->cputime.sum_exec_runtime += src_bstat->cputime.sum_exec_runtime;
#ifdef CONFIG_SCHED_CORE
	dst_bstat->forceidle_sum += src_bstat->forceidle_sum;
#endif
	dst_bstat->ntime += src_bstat->ntime;
#ifdef CONFIG_SCHED_INFO
	dst_bstat->run_delay += src_bstat->run_delay;
#endif
}

static void cgroup_base_stat_sub(struct cgroup_base_stat *dst_bstat,
				 struct cgroup_base_stat *src_bstat)
{
	dst_bstat->cputime.utime -= src_bstat->cputime.utime;
	dst_bstat->cputime.stime -= src_bstat->cputime.stime;
	dst_bstat->cputime.sum_exec_runtime -= src_bstat->cputime.sum_exec_runtime;
#ifdef CONFIG_SCHED_CORE
	dst_bstat->forceidle_sum -= src_bstat->forceidle_sum;
#endif
	dst_bstat->ntime -= src_bstat->ntime;
#ifdef CONFIG_SCHED_INFO
	dst_bstat->run_delay -= src_bstat->run_delay;
#endif
}

static void cgroup_base_stat_flush(struct cgroup *cgrp, int cpu)
{
	struct cgroup_rstat_cpu *rstatc = cgroup_rstat_cpu(cgrp, cpu);
	struct cgroup *parent = cgroup_parent(cgrp);
	struct cgroup_rstat_cpu *prstatc;
	struct cgroup_base_stat delta;
	unsigned seq;

	/* Root-level stats are sourced from system-wide CPU stats */
	if (!parent)
		return;

	/* fetch the current per-cpu values */
	do {
		seq = __u64_stats_fetch_begin(&rstatc->bsync);
		delta = rstatc->bstat;
	} while (__u64_stats_fetch_retry(&rstatc->bsync, seq));

	/* propagate per-cpu delta to cgroup and per-cpu global statistics */
	cgroup_base_stat_sub(&delta, &rstatc->last_bstat);
	cgroup_base_stat_add(&cgrp->bstat, &delta);
	cgroup_base_stat_add(&rstatc->last_bstat, &delta);
	cgroup_base_stat_add(&rstatc->subtree_bstat, &delta);

	/* propagate cgroup and per-cpu global delta to parent (unless that's root) */
	if (cgroup_parent(parent)) {
		delta = cgrp->bstat;
		cgroup_base_stat_sub(&delta, &cgrp->last_bstat);
		cgroup_base_stat_add(&parent->bstat, &delta);
		cgroup_base_stat_add(&cgrp->last_bstat, &delta);

		delta = rstatc->subtree_bstat;
		prstatc = cgroup_rstat_cpu(parent, cpu);
		cgroup_base_stat_sub(&delta, &rstatc->last_subtree_bstat);
		cgroup_base_stat_add(&prstatc->subtree_bstat, &delta);
		cgroup_base_stat_add(&rstatc->last_subtree_bstat, &delta);
	}
}

static struct cgroup_rstat_cpu *
cgroup_base_stat_cputime_account_begin(struct cgroup *cgrp, unsigned long *flags)
{
	struct cgroup_rstat_cpu *rstatc;

	rstatc = get_cpu_ptr(cgrp->rstat_cpu);
	*flags = u64_stats_update_begin_irqsave(&rstatc->bsync);
	return rstatc;
}

static void cgroup_base_stat_cputime_account_end(struct cgroup *cgrp,
						 struct cgroup_rstat_cpu *rstatc,
						 unsigned long flags)
{
	u64_stats_update_end_irqrestore(&rstatc->bsync, flags);
	cgroup_rstat_updated(cgrp, smp_processor_id());
	put_cpu_ptr(rstatc);
}

void __cgroup_account_cputime(struct cgroup *cgrp, u64 delta_exec)
{
	struct cgroup_rstat_cpu *rstatc;
	unsigned long flags;

	rstatc = cgroup_base_stat_cputime_account_begin(cgrp, &flags);
	rstatc->bstat.cputime.sum_exec_runtime += delta_exec;
	cgroup_base_stat_cputime_account_end(cgrp, rstatc, flags);
}

void __cgroup_account_cputime_field(struct cgroup *cgrp,
				    enum cpu_usage_stat index, u64 delta_exec)
{
	struct cgroup_rstat_cpu *rstatc;
	unsigned long flags;

	rstatc = cgroup_base_stat_cputime_account_begin(cgrp, &flags);

	switch (index) {
	case CPUTIME_NICE:
		rstatc->bstat.ntime += delta_exec;
		fallthrough;
	case CPUTIME_USER:
		rstatc->bstat.cputime.utime += delta_exec;
		break;
	case CPUTIME_SYSTEM:
	case CPUTIME_IRQ:
	case CPUTIME_SOFTIRQ:
		rstatc->bstat.cputime.stime += delta_exec;
		break;
#ifdef CONFIG_SCHED_CORE
	case CPUTIME_FORCEIDLE:
		rstatc->bstat.forceidle_sum += delta_exec;
		break;
#endif
#ifdef CONFIG_SCHED_INFO
	case CPUTIME_RUN_DELAY:
		rstatc->bstat.run_delay += delta_exec;
		break;
#endif
	default:
		break;
	}

	cgroup_base_stat_cputime_account_end(cgrp, rstatc, flags);
}

static void root_cgroup_cputime_cpu(struct cgroup_base_stat *bstatc, int cpu)
{
	struct task_cputime *cputime = &bstatc->cputime;
	struct kernel_cpustat kcpustat;
	u64 *cpustat = kcpustat.cpustat;

	kcpustat_cpu_fetch(&kcpustat, cpu);

	cputime->utime  = cpustat[CPUTIME_USER];
	cputime->utime += cpustat[CPUTIME_NICE];

	cputime->stime  = cpustat[CPUTIME_SYSTEM];
	cputime->stime += cpustat[CPUTIME_IRQ];
	cputime->stime += cpustat[CPUTIME_SOFTIRQ];

	cputime->sum_exec_runtime = cputime->utime + cputime->stime;
	cputime->sum_exec_runtime += cpustat[CPUTIME_STEAL];

	bstatc->ntime = cpustat[CPUTIME_NICE];
#ifdef CONFIG_SCHED_CORE
	bstatc->forceidle_sum = cpustat[CPUTIME_FORCEIDLE];
#endif
#ifdef CONFIG_SCHED_INFO
	bstatc->run_delay = cpustat[CPUTIME_RUN_DELAY];
#endif
}

/*
 * compute the cputime for the root cgroup by getting the per cpu data
 * at a global level, then categorizing the fields in a manner consistent
 * with how it is done by __cgroup_account_cputime_field for each bit of
 * cpu time attributed to a cgroup.
 */
static void root_cgroup_cputime(struct cgroup_base_stat *bstat)
{
	struct cgroup_base_stat bstatc;
	int i;

	memset(bstat, 0, sizeof(*bstat));
	for_each_possible_cpu(i) {
		root_cgroup_cputime_cpu(&bstatc, i);
		cgroup_base_stat_add(bstat, &bstatc);
	}
}


static void cgroup_force_idle_show(struct seq_file *seq, struct cgroup_base_stat *bstat)
{
#ifdef CONFIG_SCHED_CORE
	u64 forceidle_time = bstat->forceidle_sum;

	do_div(forceidle_time, NSEC_PER_USEC);
	seq_printf(seq, "core_sched.force_idle_usec %llu\n", forceidle_time);
#endif
}

static void cgroup_run_delay_show(struct seq_file *seq, struct cgroup_base_stat *bstat)
{
#ifdef CONFIG_SCHED_INFO
	u64 run_delay = bstat->run_delay;

	do_div(run_delay, NSEC_PER_USEC);
	seq_printf(seq, "run_delay_usec %llu\n", run_delay);
#endif
}

void cgroup_base_stat_cputime_show(struct seq_file *seq)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct cgroup_base_stat bstat;

	if (cgroup_parent(cgrp)) {
		cgroup_rstat_flush(cgrp);
		__cgroup_rstat_lock(cgrp, -1);
		bstat = cgrp->bstat;
		cputime_adjust(&cgrp->bstat.cputime, &cgrp->prev_cputime,
			       &bstat.cputime.utime, &bstat.cputime.stime);
		__cgroup_rstat_unlock(cgrp, -1);
	} else {
		root_cgroup_cputime(&bstat);
	}

	do_div(bstat.cputime.sum_exec_runtime, NSEC_PER_USEC);
	do_div(bstat.cputime.utime, NSEC_PER_USEC);
	do_div(bstat.cputime.stime, NSEC_PER_USEC);
	do_div(bstat.ntime, NSEC_PER_USEC);

	seq_printf(seq, "usage_usec %llu\n"
			"user_usec %llu\n"
			"system_usec %llu\n"
			"nice_usec %llu\n",
			bstat.cputime.sum_exec_runtime,
			bstat.cputime.utime,
			bstat.cputime.stime,
			bstat.ntime);

	cgroup_force_idle_show(seq, &bstat);
	cgroup_run_delay_show(seq, &bstat);
}

#define bstat_offset(field)	offsetof(struct cgroup_base_stat, field)

static const struct bstat_entry {
	const char	*name;
	int		offset;
} bstats[] = {
	{ "usage_usec",		bstat_offset(cputime.sum_exec_runtime)	},
	{ "user_usec",		bstat_offset(cputime.utime)		},
	{ "system_usec",	bstat_offset(cputime.stime)		},
	{ "nice_usec",		bstat_offset(ntime)			},
#ifdef CONFIG_SCHED_CORE
	{ "core_sched.force_idle_usec",	bstat_offset(forceidle_sum)	},
#endif
#ifdef CONFIG_SCHED_INFO
	{ "run_delay_usec",	bstat_offset(run_delay)			},
#endif
};

static void cgroup_bstat_cpu_read(struct cgroup_base_stat *bstatc,
				  struct cgroup *cgrp, int cpu)
{
	if (cgroup_parent(cgrp))
		/*
		 * In theory cputime_adjust() is needed for tick-based
		 * accounting of [u|s]time against scheduler-based rtime.
		 * But for this percpu stats, although it's really easy
		 * to scale [u|s]time to meet 'utime + stime = rtime',
		 * there is no holder for prev_cputime which is required
		 * to preserve monotonicity across reads which might be
		 * unacceptable. So keep things simple to just provide
		 * the raw cputimes and let userspace play tricks if
		 * they want.
		 */
		*bstatc = cgroup_rstat_cpu(cgrp, cpu)->subtree_bstat;
	else
		root_cgroup_cputime_cpu(bstatc, cpu);
}

int cgroup_base_stat_percpu_show(struct seq_file *seq)
{
	struct cgroup *cgrp = seq_css(seq)->cgroup;
	struct cgroup_base_stat __percpu *pcbstat;
	struct cgroup_base_stat *bstat;
	const struct bstat_entry *e;
	int i;

	pcbstat = alloc_percpu(struct cgroup_base_stat);
	if (!pcbstat)
		return -ENOMEM;

	if (cgroup_parent(cgrp))
		__cgroup_rstat_lock(cgrp, -1);

	for_each_possible_cpu(i) {
		bstat = per_cpu_ptr(pcbstat, i);
		cgroup_bstat_cpu_read(bstat, cgrp, i);
	}

	if (cgroup_parent(cgrp))
		__cgroup_rstat_unlock(cgrp, -1);

	for (e = bstats; e < bstats + ARRAY_SIZE(bstats); e++) {
		seq_puts(seq, e->name);
		for_each_possible_cpu(i) {
			u64 *val;

			bstat = per_cpu_ptr(pcbstat, i);
			val = (void *)bstat + e->offset;
			do_div(*val, NSEC_PER_USEC);
			seq_printf(seq, " C%d=%llu", i, *val);
		}
		seq_putc(seq, '\n');
	}

	free_percpu(pcbstat);
	return 0;
}
