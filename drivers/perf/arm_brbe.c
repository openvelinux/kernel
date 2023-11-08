// SPDX-License-Identifier: GPL-2.0-only
/*
 * Branch Record Buffer Extension Driver.
 *
 * Copyright (C) 2022-2023 ARM Limited
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
#include "arm_brbe.h"

void armv8pmu_branch_reset(void)
{
	asm volatile(BRB_IALL_INSN);
	isb();
}

static bool valid_brbe_nr(int brbe_nr)
{
	return brbe_nr == BRBIDR0_EL1_NUMREC_8 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_16 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_32 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_64;
}

static bool valid_brbe_cc(int brbe_cc)
{
	return brbe_cc == BRBIDR0_EL1_CC_20_BIT;
}

static bool valid_brbe_format(int brbe_format)
{
	return brbe_format == BRBIDR0_EL1_FORMAT_0;
}

static bool valid_brbe_version(int brbe_version)
{
	return brbe_version == ID_AA64DFR0_EL1_BRBE_IMP ||
	       brbe_version == ID_AA64DFR0_EL1_BRBE_BRBE_V1P1;
}

static void select_brbe_bank(int bank)
{
	u64 brbfcr;

	WARN_ON(bank > BRBE_BANK_IDX_1);
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbfcr &= ~BRBFCR_EL1_BANK_MASK;
	brbfcr |= SYS_FIELD_PREP(BRBFCR_EL1, BANK, bank);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();
}

static bool __read_brbe_regset(struct brbe_regset *entry, int idx)
{
	entry->brbinf = get_brbinf_reg(idx);

	if (brbe_invalid(entry->brbinf))
		return false;

	entry->brbsrc = get_brbsrc_reg(idx);
	entry->brbtgt = get_brbtgt_reg(idx);
	return true;
}

/*
 * Read all BRBE entries in HW until the first invalid entry.
 *
 * The caller must ensure that the BRBE is not concurrently modifying these
 * branch entries.
 */
static int capture_brbe_regset(struct brbe_regset *buf, int nr_hw_entries)
{
	int idx = 0;

	select_brbe_bank(BRBE_BANK_IDX_0);
	while (idx < nr_hw_entries && idx <= BRBE_BANK0_IDX_MAX) {
		if (!__read_brbe_regset(&buf[idx], idx))
			return idx;
		idx++;
	}

	select_brbe_bank(BRBE_BANK_IDX_1);
	while (idx < nr_hw_entries && idx <= BRBE_BANK1_IDX_MAX) {
		if (!__read_brbe_regset(&buf[idx], idx))
			return idx;
		idx++;
	}
	return idx;
}

/*
 * This function concatenates branch records from stored and live buffer
 * up to maximum nr_max records and the stored buffer holds the resultant
 * buffer. The concatenated buffer contains all the branch records from
 * the live buffer but might contain some from stored buffer considering
 * the maximum combined length does not exceed 'nr_max'.
 *
 *	Stored records	Live records
 *	------------------------------------------------^
 *	|	S0	|	L0	|	Newest	|
 *	---------------------------------		|
 *	|	S1	|	L1	|		|
 *	---------------------------------		|
 *	|	S2	|	L2	|		|
 *	---------------------------------		|
 *	|	S3	|	L3	|		|
 *	---------------------------------		|
 *	|	S4	|	L4	|		nr_max
 *	---------------------------------		|
 *	|		|	L5	|		|
 *	---------------------------------		|
 *	|		|	L6	|		|
 *	---------------------------------		|
 *	|		|	L7	|		|
 *	---------------------------------		|
 *	|		|		|		|
 *	---------------------------------		|
 *	|		|		|	Oldest	|
 *	------------------------------------------------V
 *
 *
 * S0 is the newest in the stored records, where as L7 is the oldest in
 * the live records. Unless the live buffer is detected as being full
 * thus potentially dropping off some older records, L7 and S0 records
 * are contiguous in time for a user task context. The stitched buffer
 * here represents maximum possible branch records, contiguous in time.
 *
 *	Stored records  Live records
 *	------------------------------------------------^
 *	|	L0	|	L0	|	Newest	|
 *	---------------------------------		|
 *	|	L0	|	L1	|		|
 *	---------------------------------		|
 *	|	L2	|	L2	|		|
 *	---------------------------------		|
 *	|	L3	|	L3	|		|
 *	---------------------------------		|
 *	|	L4	|	L4	|	      nr_max
 *	---------------------------------		|
 *	|	L5	|	L5	|		|
 *	---------------------------------		|
 *	|	L6	|	L6	|		|
 *	---------------------------------		|
 *	|	L7	|	L7	|		|
 *	---------------------------------		|
 *	|	S0	|		|		|
 *	---------------------------------		|
 *	|	S1	|		|    Oldest	|
 *	------------------------------------------------V
 *	|	S2	| <----|
 *	-----------------      |
 *	|	S3	| <----| Dropped off after nr_max
 *	-----------------      |
 *	|	S4	| <----|
 *	-----------------
 */
static int stitch_stored_live_entries(struct brbe_regset *stored,
				      struct brbe_regset *live,
				      int nr_stored, int nr_live,
				      int nr_max)
{
	int nr_move = min(nr_stored, nr_max - nr_live);

	/* Move the tail of the buffer to make room for the new entries */
	memmove(&stored[nr_live], &stored[0], nr_move * sizeof(*stored));

	/* Copy the new entries into the head of the buffer */
	memcpy(&stored[0], &live[0], nr_live * sizeof(*stored));

	/* Return the number of entries in the stitched buffer */
	return min(nr_live + nr_stored, nr_max);
}

static int brbe_branch_save(struct brbe_regset *live, int nr_hw_entries)
{
	u64 brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	int nr_live;

	write_sysreg_s(brbfcr | BRBFCR_EL1_PAUSED, SYS_BRBFCR_EL1);
	isb();

	nr_live = capture_brbe_regset(live, nr_hw_entries);

	write_sysreg_s(brbfcr & ~BRBFCR_EL1_PAUSED, SYS_BRBFCR_EL1);
	isb();

	return nr_live;
}

void armv8pmu_branch_save(struct arm_pmu *arm_pmu, void *ctx)
{
	struct arm64_perf_task_context *task_ctx = ctx;
	struct brbe_regset live[BRBE_MAX_ENTRIES];
	int nr_live, nr_store, nr_hw_entries;

	nr_hw_entries = brbe_get_numrec(arm_pmu->reg_brbidr);
	nr_live = brbe_branch_save(live, nr_hw_entries);
	nr_store = task_ctx->nr_brbe_records;
	nr_store = stitch_stored_live_entries(task_ctx->store, live, nr_store,
					      nr_live, nr_hw_entries);
	task_ctx->nr_brbe_records = nr_store;
}

/*
 * Generic perf branch filters supported on BRBE
 *
 * New branch filters need to be evaluated whether they could be supported on
 * BRBE. This ensures that such branch filters would not just be accepted, to
 * fail silently. PERF_SAMPLE_BRANCH_HV is a special case that is selectively
 * supported only on platforms where kernel is in hyp mode.
 */
#define BRBE_EXCLUDE_BRANCH_FILTERS (PERF_SAMPLE_BRANCH_ABORT_TX	| \
				     PERF_SAMPLE_BRANCH_IN_TX		| \
				     PERF_SAMPLE_BRANCH_NO_TX		| \
				     PERF_SAMPLE_BRANCH_CALL_STACK)

#define BRBE_ALLOWED_BRANCH_FILTERS (PERF_SAMPLE_BRANCH_USER		| \
				     PERF_SAMPLE_BRANCH_KERNEL		| \
				     PERF_SAMPLE_BRANCH_HV		| \
				     PERF_SAMPLE_BRANCH_ANY		| \
				     PERF_SAMPLE_BRANCH_ANY_CALL	| \
				     PERF_SAMPLE_BRANCH_ANY_RETURN	| \
				     PERF_SAMPLE_BRANCH_IND_CALL	| \
				     PERF_SAMPLE_BRANCH_COND		| \
				     PERF_SAMPLE_BRANCH_IND_JUMP	| \
				     PERF_SAMPLE_BRANCH_CALL		| \
				     PERF_SAMPLE_BRANCH_NO_FLAGS	| \
				     PERF_SAMPLE_BRANCH_NO_CYCLES	| \
				     PERF_SAMPLE_BRANCH_TYPE_SAVE	| \
				     PERF_SAMPLE_BRANCH_HW_INDEX	| \
				     PERF_SAMPLE_BRANCH_PRIV_SAVE)

#define BRBE_PERF_BRANCH_FILTERS    (BRBE_ALLOWED_BRANCH_FILTERS	| \
				     BRBE_EXCLUDE_BRANCH_FILTERS)

bool armv8pmu_branch_attr_valid(struct perf_event *event)
{
	u64 branch_type = event->attr.branch_sample_type;

	/*
	 * Ensure both perf branch filter allowed and exclude
	 * masks are always in sync with the generic perf ABI.
	 */
	BUILD_BUG_ON(BRBE_PERF_BRANCH_FILTERS != (PERF_SAMPLE_BRANCH_MAX - 1));

	if (branch_type & ~BRBE_ALLOWED_BRANCH_FILTERS) {
		pr_debug_once("requested branch filter not supported 0x%llx\n", branch_type);
		return false;
	}

	/*
	 * If the event does not have at least one of the privilege
	 * branch filters as in PERF_SAMPLE_BRANCH_PLM_ALL, the core
	 * perf will adjust its value based on perf event's existing
	 * privilege level via attr.exclude_[user|kernel|hv].
	 *
	 * As event->attr.branch_sample_type might have been changed
	 * when the event reaches here, it is not possible to figure
	 * out whether the event originally had HV privilege request
	 * or got added via the core perf. Just report this situation
	 * once and continue ignoring if there are other instances.
	 */
	if ((branch_type & PERF_SAMPLE_BRANCH_HV) && !is_kernel_in_hyp_mode())
		pr_debug_once("hypervisor privilege filter not supported 0x%llx\n", branch_type);

	return true;
}

int armv8pmu_task_ctx_cache_alloc(struct arm_pmu *arm_pmu)
{
	size_t size = sizeof(struct arm64_perf_task_context);

	arm_pmu->pmu.task_ctx_cache = kmem_cache_create("arm64_brbe_task_ctx", size, 0, 0, NULL);
	if (!arm_pmu->pmu.task_ctx_cache)
		return -ENOMEM;
	return 0;
}

void armv8pmu_task_ctx_cache_free(struct arm_pmu *arm_pmu)
{
	kmem_cache_destroy(arm_pmu->pmu.task_ctx_cache);
}

static int brbe_attributes_probe(struct arm_pmu *armpmu, u32 brbe)
{
	u64 brbidr = read_sysreg_s(SYS_BRBIDR0_EL1);
	int brbe_version, brbe_format, brbe_cc, brbe_nr;

	brbe_version = brbe;
	brbe_format = brbe_get_format(brbidr);
	brbe_cc = brbe_get_cc_bits(brbidr);
	brbe_nr = brbe_get_numrec(brbidr);
	armpmu->reg_brbidr = brbidr;

	if (!valid_brbe_version(brbe_version) ||
	   !valid_brbe_format(brbe_format) ||
	   !valid_brbe_cc(brbe_cc) ||
	   !valid_brbe_nr(brbe_nr))
		return -EOPNOTSUPP;
	return 0;
}

void armv8pmu_branch_probe(struct arm_pmu *armpmu)
{
	u64 aa64dfr0 = read_sysreg_s(SYS_ID_AA64DFR0_EL1);
	u32 brbe;

	/*
	 * BRBE implementation's branch entries cannot exceed maximum
	 * branch records supported at the ARM PMU level abstraction.
	 * Otherwise there is always a possibility of array overflow,
	 * while processing BRBE branch records.
	 */
	BUILD_BUG_ON(BRBE_BANK_MAX_ENTRIES > MAX_BRANCH_RECORDS);

	brbe = cpuid_feature_extract_unsigned_field(aa64dfr0, ID_AA64DFR0_EL1_BRBE_SHIFT);
	if (!brbe)
		return;

	if (brbe_attributes_probe(armpmu, brbe))
		return;

	armpmu->has_branch_stack = 1;
}

/*
 * BRBE supports the following functional branch type filters while
 * generating branch records. These branch filters can be enabled,
 * either individually or as a group i.e ORing multiple filters
 * with each other.
 *
 * BRBFCR_EL1_CONDDIR  - Conditional direct branch
 * BRBFCR_EL1_DIRCALL  - Direct call
 * BRBFCR_EL1_INDCALL  - Indirect call
 * BRBFCR_EL1_INDIRECT - Indirect branch
 * BRBFCR_EL1_DIRECT   - Direct branch
 * BRBFCR_EL1_RTN      - Subroutine return
 */
static u64 branch_type_to_brbfcr(int branch_type)
{
	u64 brbfcr = 0;

	if (branch_type & PERF_SAMPLE_BRANCH_ANY) {
		brbfcr |= BRBFCR_EL1_BRANCH_FILTERS;
		return brbfcr;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_CALL) {
		brbfcr |= BRBFCR_EL1_INDCALL;
		brbfcr |= BRBFCR_EL1_DIRCALL;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_RETURN)
		brbfcr |= BRBFCR_EL1_RTN;

	if (branch_type & PERF_SAMPLE_BRANCH_IND_CALL)
		brbfcr |= BRBFCR_EL1_INDCALL;

	if (branch_type & PERF_SAMPLE_BRANCH_COND)
		brbfcr |= BRBFCR_EL1_CONDDIR;

	if (branch_type & PERF_SAMPLE_BRANCH_IND_JUMP)
		brbfcr |= BRBFCR_EL1_INDIRECT;

	if (branch_type & PERF_SAMPLE_BRANCH_CALL)
		brbfcr |= BRBFCR_EL1_DIRCALL;

	return brbfcr;
}

/*
 * BRBE supports the following privilege mode filters while generating
 * branch records.
 *
 * BRBCR_ELx_E0BRE - EL0 branch records
 * BRBCR_ELx_ExBRE - EL1/EL2 branch records
 *
 * BRBE also supports the following additional functional branch type
 * filters while generating branch records.
 *
 * BRBCR_ELx_EXCEPTION - Exception
 * BRBCR_ELx_ERTN     -  Exception return
 */
static u64 branch_type_to_brbcr(int branch_type)
{
	u64 brbcr = BRBCR_ELx_DEFAULT_TS;

	/*
	 * BRBE should be paused on PMU interrupt while tracing kernel
	 * space to stop capturing further branch records. Otherwise
	 * interrupt handler branch records might get into the samples
	 * which is not desired.
	 *
	 * BRBE need not be paused on PMU interrupt while tracing only
	 * the user space, because it will automatically be inside the
	 * prohibited region. But even after PMU overflow occurs, the
	 * interrupt could still take much more cycles, before it can
	 * be taken and by that time BRBE will have been overwritten.
	 * Hence enable pause on PMU interrupt mechanism even for user
	 * only traces as well.
	 */
	brbcr |= BRBCR_ELx_FZP;

	if (branch_type & PERF_SAMPLE_BRANCH_USER)
		brbcr |= BRBCR_ELx_E0BRE;

	/*
	 * When running in the hyp mode, writing into BRBCR_EL1
	 * actually writes into BRBCR_EL2 instead. Field E2BRE
	 * is also at the same position as E1BRE.
	 */
	if (branch_type & PERF_SAMPLE_BRANCH_KERNEL)
		brbcr |= BRBCR_ELx_ExBRE;

	if (branch_type & PERF_SAMPLE_BRANCH_HV) {
		if (is_kernel_in_hyp_mode())
			brbcr |= BRBCR_ELx_ExBRE;
	}

	if (!(branch_type & PERF_SAMPLE_BRANCH_NO_CYCLES))
		brbcr |= BRBCR_ELx_CC;

	if (!(branch_type & PERF_SAMPLE_BRANCH_NO_FLAGS))
		brbcr |= BRBCR_ELx_MPRED;

	/*
	 * The exception and exception return branches could be
	 * captured, irrespective of the perf event's privilege.
	 * If the perf event does not have enough privilege for
	 * a given exception level, then addresses which falls
	 * under that exception level will be reported as zero
	 * for the captured branch record, creating source only
	 * or target only records.
	 */
	if (branch_type & PERF_SAMPLE_BRANCH_ANY) {
		brbcr |= BRBCR_ELx_EXCEPTION;
		brbcr |= BRBCR_ELx_ERTN;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_CALL)
		brbcr |= BRBCR_ELx_EXCEPTION;

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_RETURN)
		brbcr |= BRBCR_ELx_ERTN;

	return brbcr & BRBCR_ELx_CONFIG_MASK;
}

void armv8pmu_branch_enable(struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct pmu_hw_events *cpuc = this_cpu_ptr(cpu_pmu->hw_events);
	u64 brbfcr, brbcr;

	if (!cpuc->brbe_users)
		return;

	/*
	 * Skip enabling BRBE again with same filters and configs
	 */
	if (cpuc->brbe_sample_type == event->attr.branch_sample_type)
		return;

	/*
	 * BRBE gets configured with a new mismatched branch sample
	 * type request, overriding any previous branch filters.
	 */
	cpuc->brbe_sample_type = event->attr.branch_sample_type;
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbfcr &= ~BRBFCR_EL1_DEFAULT_CONFIG;
	brbfcr |= branch_type_to_brbfcr(cpuc->brbe_sample_type);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();

	brbcr = read_sysreg_s(SYS_BRBCR_EL1);
	brbcr &= ~BRBCR_ELx_CONFIG_MASK;
	brbcr |= branch_type_to_brbcr(cpuc->brbe_sample_type);
	write_sysreg_s(brbcr, SYS_BRBCR_EL1);
	isb();
}

void armv8pmu_branch_disable(struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	struct pmu_hw_events *cpuc = this_cpu_ptr(cpu_pmu->hw_events);
	u64 brbfcr, brbcr;

	if (cpuc->brbe_users)
		return;

	cpuc->brbe_sample_type = 0;
	brbcr = read_sysreg_s(SYS_BRBCR_EL1);
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbcr &= ~(BRBCR_ELx_E0BRE | BRBCR_ELx_ExBRE);
	brbfcr |= BRBFCR_EL1_PAUSED;
	write_sysreg_s(brbcr, SYS_BRBCR_EL1);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();
}

static void brbe_set_perf_entry_type(struct perf_branch_entry *entry, u64 brbinf)
{
	int brbe_type = brbe_get_type(brbinf);

	switch (brbe_type) {
	case BRBINFx_EL1_TYPE_UNCOND_DIRECT:
		entry->type = PERF_BR_UNCOND;
		break;
	case BRBINFx_EL1_TYPE_INDIRECT:
		entry->type = PERF_BR_IND;
		break;
	case BRBINFx_EL1_TYPE_DIRECT_LINK:
		entry->type = PERF_BR_CALL;
		break;
	case BRBINFx_EL1_TYPE_INDIRECT_LINK:
		entry->type = PERF_BR_IND_CALL;
		break;
	case BRBINFx_EL1_TYPE_RET:
		entry->type = PERF_BR_RET;
		break;
	case BRBINFx_EL1_TYPE_COND_DIRECT:
		entry->type = PERF_BR_COND;
		break;
	case BRBINFx_EL1_TYPE_CALL:
		entry->type = PERF_BR_CALL;
		break;
	case BRBINFx_EL1_TYPE_TRAP:
		entry->type = PERF_BR_SYSCALL;
		break;
	case BRBINFx_EL1_TYPE_ERET:
		entry->type = PERF_BR_ERET;
		break;
	case BRBINFx_EL1_TYPE_IRQ:
		entry->type = PERF_BR_IRQ;
		break;
	case BRBINFx_EL1_TYPE_DEBUG_HALT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_DEBUG_HALT;
		break;
	case BRBINFx_EL1_TYPE_SERROR:
		entry->type = PERF_BR_SERROR;
		break;
	case BRBINFx_EL1_TYPE_INSN_DEBUG:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_DEBUG_INST;
		break;
	case BRBINFx_EL1_TYPE_DATA_DEBUG:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_DEBUG_DATA;
		break;
	case BRBINFx_EL1_TYPE_ALIGN_FAULT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_NEW_FAULT_ALGN;
		break;
	case BRBINFx_EL1_TYPE_INSN_FAULT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_NEW_FAULT_INST;
		break;
	case BRBINFx_EL1_TYPE_DATA_FAULT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_NEW_FAULT_DATA;
		break;
	case BRBINFx_EL1_TYPE_FIQ:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_FIQ;
		break;
	case BRBINFx_EL1_TYPE_DEBUG_EXIT:
		entry->type = PERF_BR_EXTEND_ABI;
		entry->new_type = PERF_BR_ARM64_DEBUG_EXIT;
		break;
	default:
		pr_warn_once("%d - unknown branch type captured\n", brbe_type);
		entry->type = PERF_BR_UNKNOWN;
		break;
	}
}

static int brbe_get_perf_priv(u64 brbinf)
{
	int brbe_el = brbe_get_el(brbinf);

	switch (brbe_el) {
	case BRBINFx_EL1_EL_EL0:
		return PERF_BR_PRIV_USER;
	case BRBINFx_EL1_EL_EL1:
		return PERF_BR_PRIV_KERNEL;
	case BRBINFx_EL1_EL_EL2:
		if (is_kernel_in_hyp_mode())
			return PERF_BR_PRIV_KERNEL;
		return PERF_BR_PRIV_HV;
	default:
		pr_warn_once("%d - unknown branch privilege captured\n", brbe_el);
		return PERF_BR_PRIV_UNKNOWN;
	}
}

static void capture_brbe_flags(struct perf_branch_entry *entry, struct perf_event *event,
			       u64 brbinf)
{
	if (branch_sample_type(event))
		brbe_set_perf_entry_type(entry, brbinf);

	if (!branch_sample_no_cycles(event))
		entry->cycles = brbe_get_cycles(brbinf);

	if (!branch_sample_no_flags(event)) {
		/*
		 * BRBINFx_EL1.LASTFAILED indicates that a TME transaction failed (or
		 * was cancelled) prior to this record, and some number of records
		 * prior to this one, may have been generated during an attempt to
		 * execute the transaction.
		 *
		 * We will remove such entries later in process_branch_aborts().
		 */
		entry->abort = brbe_get_lastfailed(brbinf);

		/*
		 * All these information (i.e transaction state and mispredicts)
		 * are available for source only and complete branch records.
		 */
		if (brbe_record_is_complete(brbinf) ||
		    brbe_record_is_source_only(brbinf)) {
			entry->mispred = brbe_get_mispredict(brbinf);
			entry->predicted = !entry->mispred;
			entry->in_tx = brbe_get_in_tx(brbinf);
		}
	}

	if (branch_sample_priv(event)) {
		/*
		 * All these information (i.e branch privilege level) are
		 * available for target only and complete branch records.
		 */
		if (brbe_record_is_complete(brbinf) ||
		    brbe_record_is_target_only(brbinf))
			entry->priv = brbe_get_perf_priv(brbinf);
	}
}

/*
 * A branch record with BRBINFx_EL1.LASTFAILED set, implies that all
 * preceding consecutive branch records, that were in a transaction
 * (i.e their BRBINFx_EL1.TX set) have been aborted.
 *
 * Similarly BRBFCR_EL1.LASTFAILED set, indicate that all preceding
 * consecutive branch records up to the last record, which were in a
 * transaction (i.e their BRBINFx_EL1.TX set) have been aborted.
 *
 * --------------------------------- -------------------
 * | 00 | BRBSRC | BRBTGT | BRBINF | | TX = 1 | LF = 0 | [TX success]
 * --------------------------------- -------------------
 * | 01 | BRBSRC | BRBTGT | BRBINF | | TX = 1 | LF = 0 | [TX success]
 * --------------------------------- -------------------
 * | 02 | BRBSRC | BRBTGT | BRBINF | | TX = 0 | LF = 0 |
 * --------------------------------- -------------------
 * | 03 | BRBSRC | BRBTGT | BRBINF | | TX = 1 | LF = 0 | [TX failed]
 * --------------------------------- -------------------
 * | 04 | BRBSRC | BRBTGT | BRBINF | | TX = 1 | LF = 0 | [TX failed]
 * --------------------------------- -------------------
 * | 05 | BRBSRC | BRBTGT | BRBINF | | TX = 0 | LF = 1 |
 * --------------------------------- -------------------
 * | .. | BRBSRC | BRBTGT | BRBINF | | TX = 0 | LF = 0 |
 * --------------------------------- -------------------
 * | 61 | BRBSRC | BRBTGT | BRBINF | | TX = 1 | LF = 0 | [TX failed]
 * --------------------------------- -------------------
 * | 62 | BRBSRC | BRBTGT | BRBINF | | TX = 1 | LF = 0 | [TX failed]
 * --------------------------------- -------------------
 * | 63 | BRBSRC | BRBTGT | BRBINF | | TX = 1 | LF = 0 | [TX failed]
 * --------------------------------- -------------------
 *
 * BRBFCR_EL1.LASTFAILED == 1
 *
 * BRBFCR_EL1.LASTFAILED fails all those consecutive, in transaction
 * branches records near the end of the BRBE buffer.
 *
 * Architecture does not guarantee a non transaction (TX = 0) branch
 * record between two different transactions. So it is possible that
 * a subsequent lastfailed record (TX = 0, LF = 1) might erroneously
 * mark more than required transactions as aborted.
 */
static void process_branch_aborts(struct pmu_hw_events *cpuc)
{
	u64 brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	bool lastfailed = !!(brbfcr & BRBFCR_EL1_LASTFAILED);
	int idx = brbe_get_numrec(cpuc->percpu_pmu->reg_brbidr) - 1;
	struct perf_branch_entry *entry;

	do {
		entry = &cpuc->branches->branch_entries[idx];
		if (entry->in_tx) {
			entry->abort = lastfailed;
		} else {
			lastfailed = entry->abort;
			entry->abort = false;
		}
	} while (idx--, idx >= 0);
}

static void brbe_regset_branch_entries(struct pmu_hw_events *cpuc, struct perf_event *event,
				       struct brbe_regset *regset, int idx)
{
	struct perf_branch_entry *entry = &cpuc->branches->branch_entries[idx];
	u64 brbinf = regset[idx].brbinf;

	perf_clear_branch_entry_bitfields(entry);
	if (brbe_record_is_complete(brbinf)) {
		entry->from = regset[idx].brbsrc;
		entry->to = regset[idx].brbtgt;
	} else if (brbe_record_is_source_only(brbinf)) {
		entry->from = regset[idx].brbsrc;
		entry->to = 0;
	} else if (brbe_record_is_target_only(brbinf)) {
		entry->from = 0;
		entry->to = regset[idx].brbtgt;
	}
	capture_brbe_flags(entry, event, brbinf);
}

static void process_branch_entries(struct pmu_hw_events *cpuc, struct perf_event *event,
				   struct brbe_regset *regset, int nr_regset)
{
	int idx;

	for (idx = 0; idx < nr_regset; idx++)
		brbe_regset_branch_entries(cpuc, event, regset, idx);

	cpuc->branches->branch_stack.nr = nr_regset;
	cpuc->branches->branch_stack.hw_idx = -1ULL;
}

void armv8pmu_branch_read(struct pmu_hw_events *cpuc, struct perf_event *event)
{
	struct arm64_perf_task_context *task_ctx = event->pmu_ctx->task_ctx_data;
	struct brbe_regset live[BRBE_MAX_ENTRIES];
	int nr_live, nr_store, nr_hw_entries;
	u64 brbfcr, brbcr;

	brbcr = read_sysreg_s(SYS_BRBCR_EL1);
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);

	/* Ensure pause on PMU interrupt is enabled */
	WARN_ON_ONCE(!(brbcr & BRBCR_ELx_FZP));

	/* Pause the buffer */
	write_sysreg_s(brbfcr | BRBFCR_EL1_PAUSED, SYS_BRBFCR_EL1);
	isb();

	/*
	 * Overflown event's branch_sample_type does not match the configured
	 * branch filters in the BRBE HW. So the captured branch records here
	 * cannot be co-related to the overflown event. Report to the user as
	 * if no branch records have been captured, and flush branch records.
	 */
	if ((cpuc->brbe_sample_type != event->attr.branch_sample_type) ||
	    (event->ctx->task && cpuc->brbe_context != event->ctx)) {
		cpuc->branches->branch_stack.nr = 0;
		cpuc->branches->branch_stack.hw_idx = -1ULL;
		goto unpause_reset;
	}

	nr_hw_entries = brbe_get_numrec(cpuc->percpu_pmu->reg_brbidr);
	nr_live = capture_brbe_regset(live, nr_hw_entries);
	if (event->ctx->task) {
		nr_store = task_ctx->nr_brbe_records;
		nr_store = stitch_stored_live_entries(task_ctx->store, live, nr_store,
						      nr_live, nr_hw_entries);
		process_branch_entries(cpuc, event, task_ctx->store, nr_store);
		task_ctx->nr_brbe_records = 0;
	} else {
		process_branch_entries(cpuc, event, live, nr_live);
	}
	process_branch_aborts(cpuc);

unpause_reset:
	/* Unpause the buffer */
	write_sysreg_s(brbfcr & ~BRBFCR_EL1_PAUSED, SYS_BRBFCR_EL1);
	isb();
	armv8pmu_branch_reset();
}
