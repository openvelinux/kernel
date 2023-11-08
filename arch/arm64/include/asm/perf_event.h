/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */

#ifndef __ASM_PERF_EVENT_H
#define __ASM_PERF_EVENT_H

#include <asm/stack_pointer.h>
#include <asm/ptrace.h>

struct pmu_hw_events;
struct arm_pmu;
struct perf_event;

#ifdef CONFIG_PERF_EVENTS
struct pt_regs;
extern unsigned long perf_instruction_pointer(struct pt_regs *regs);
extern unsigned long perf_misc_flags(struct pt_regs *regs);
#define perf_misc_flags(regs)	perf_misc_flags(regs)
#define perf_arch_bpf_user_pt_regs(regs) &regs->user_regs
#endif

#define perf_arch_fetch_caller_regs(regs, __ip) { \
	(regs)->pc = (__ip);    \
	(regs)->regs[29] = (unsigned long) __builtin_frame_address(0); \
	(regs)->sp = current_stack_pointer; \
	(regs)->pstate = PSR_MODE_EL1h;	\
}

#ifdef CONFIG_ARM64_BRBE
void armv8pmu_branch_reset(void);
void armv8pmu_branch_probe(struct arm_pmu *arm_pmu);
bool armv8pmu_branch_attr_valid(struct perf_event *event);
void armv8pmu_branch_enable(struct perf_event *event);
void armv8pmu_branch_disable(struct perf_event *event);
void armv8pmu_branch_read(struct pmu_hw_events *cpuc,
			  struct perf_event *event);
void armv8pmu_branch_save(struct arm_pmu *arm_pmu, void *ctx);
int armv8pmu_task_ctx_cache_alloc(struct arm_pmu *arm_pmu);
void armv8pmu_task_ctx_cache_free(struct arm_pmu *arm_pmu);
#else  /* !CONFIG_ARM64_BRBE */
static inline void armv8pmu_branch_reset(void)
{
}

static inline void armv8pmu_branch_probe(struct arm_pmu *arm_pmu)
{
}

static inline bool armv8pmu_branch_attr_valid(struct perf_event *event)
{
	return false;
}

static inline void armv8pmu_branch_enable(struct perf_event *event)
{
}

static inline void armv8pmu_branch_disable(struct perf_event *event)
{
}

static inline void armv8pmu_branch_read(struct pmu_hw_events *cpuc,
					struct perf_event *event)
{
}

static inline void armv8pmu_branch_save(struct arm_pmu *arm_pmu, void *ctx)
{
}

static inline int armv8pmu_task_ctx_cache_alloc(struct arm_pmu *arm_pmu)
{
	return 0;
}

static inline void armv8pmu_task_ctx_cache_free(struct arm_pmu *arm_pmu)
{
}

#endif /* CONFIG_ARM64_BRBE */
#endif
