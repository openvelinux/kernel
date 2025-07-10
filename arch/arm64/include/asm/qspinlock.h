/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_ARM64_QSPINLOCK_H
#define _ASM_ARM64_QSPINLOCK_H

#ifdef CONFIG_NUMA_AWARE_SPINLOCKS
#include <asm-generic/qspinlock_types.h>

extern void cna_configure_spin_lock_slowpath(void);

extern void (*cna_queued_spin_lock_slowpath)(struct qspinlock *lock, u32 val);
extern void native_queued_spin_lock_slowpath(struct qspinlock *lock, u32 val);

#define	queued_spin_unlock queued_spin_unlock
/**
 * queued_spin_unlock - release a queued spinlock
 * @lock : Pointer to queued spinlock structure
 *
 * A smp_store_release() on the least-significant byte.
 */
static inline void native_queued_spin_unlock(struct qspinlock *lock)
{
	smp_store_release(&lock->locked, 0);
}

static inline void queued_spin_lock_slowpath(struct qspinlock *lock, u32 val)
{
	cna_queued_spin_lock_slowpath(lock, val);
}

static inline void queued_spin_unlock(struct qspinlock *lock)
{
	native_queued_spin_unlock(lock);
}
#endif

#include <asm-generic/qspinlock.h>

#endif /* _ASM_ARM64_QSPINLOCK_H */
