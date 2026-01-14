/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Branch Record Buffer Extension Helpers.
 *
 * Copyright (C) 2022-2023 ARM Limited
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
#define pr_fmt(fmt) "brbe: " fmt

#include <linux/perf/arm_pmu.h>

#define BRBFCR_EL1_BRANCH_FILTERS (BRBFCR_EL1_DIRECT   | \
				   BRBFCR_EL1_INDIRECT | \
				   BRBFCR_EL1_RTN      | \
				   BRBFCR_EL1_INDCALL  | \
				   BRBFCR_EL1_DIRCALL  | \
				   BRBFCR_EL1_CONDDIR)

#define BRBFCR_EL1_DEFAULT_CONFIG (BRBFCR_EL1_BANK_MASK | \
				   BRBFCR_EL1_PAUSED    | \
				   BRBFCR_EL1_EnI       | \
				   BRBFCR_EL1_BRANCH_FILTERS)

/*
 * BRBTS_EL1 is currently not used for branch stack implementation
 * purpose but BRBCR_ELx.TS needs to have a valid value from all
 * available options. BRBCR_ELx_TS_VIRTUAL is selected for this.
 */
#define BRBCR_ELx_DEFAULT_TS      FIELD_PREP(BRBCR_ELx_TS_MASK, BRBCR_ELx_TS_VIRTUAL)

#define BRBCR_ELx_CONFIG_MASK     (BRBCR_ELx_EXCEPTION | \
				   BRBCR_ELx_ERTN      | \
				   BRBCR_ELx_CC        | \
				   BRBCR_ELx_MPRED     | \
				   BRBCR_ELx_ExBRE     | \
				   BRBCR_ELx_E0BRE     | \
				   BRBCR_ELx_FZP       | \
				   BRBCR_ELx_TS_MASK)
/*
 * BRBE Buffer Organization
 *
 * BRBE buffer is arranged as multiple banks of 32 branch record
 * entries each. An individual branch record in a given bank could
 * be accessed, after selecting the bank in BRBFCR_EL1.BANK and
 * accessing the registers i.e [BRBSRC, BRBTGT, BRBINF] set with
 * indices [0..31].
 *
 * Bank 0
 *
 *	---------------------------------	------
 *	| 00 | BRBSRC | BRBTGT | BRBINF |	| 00 |
 *	---------------------------------	------
 *	| 01 | BRBSRC | BRBTGT | BRBINF |	| 01 |
 *	---------------------------------	------
 *	| .. | BRBSRC | BRBTGT | BRBINF |	| .. |
 *	---------------------------------	------
 *	| 31 | BRBSRC | BRBTGT | BRBINF |	| 31 |
 *	---------------------------------	------
 *
 * Bank 1
 *
 *	---------------------------------	------
 *	| 32 | BRBSRC | BRBTGT | BRBINF |	| 00 |
 *	---------------------------------	------
 *	| 33 | BRBSRC | BRBTGT | BRBINF |	| 01 |
 *	---------------------------------	------
 *	| .. | BRBSRC | BRBTGT | BRBINF |	| .. |
 *	---------------------------------	------
 *	| 63 | BRBSRC | BRBTGT | BRBINF |	| 31 |
 *	---------------------------------	------
 */
#define BRBE_BANK_MAX_ENTRIES 32
#define BRBE_MAX_BANK 2
#define BRBE_MAX_ENTRIES (BRBE_BANK_MAX_ENTRIES * BRBE_MAX_BANK)

#define BRBE_BANK0_IDX_MIN 0
#define BRBE_BANK0_IDX_MAX 31
#define BRBE_BANK1_IDX_MIN 32
#define BRBE_BANK1_IDX_MAX 63

struct brbe_regset {
	unsigned long brbsrc;
	unsigned long brbtgt;
	unsigned long brbinf;
};

struct arm64_perf_task_context {
	struct brbe_regset store[BRBE_MAX_ENTRIES];
	int nr_brbe_records;
};

struct brbe_hw_attr {
	int	brbe_version;
	int	brbe_cc;
	int	brbe_nr;
	int	brbe_format;
};

enum brbe_bank_idx {
	BRBE_BANK_IDX_INVALID = -1,
	BRBE_BANK_IDX_0,
	BRBE_BANK_IDX_1,
	BRBE_BANK_IDX_MAX
};

#define RETURN_READ_BRBSRCN(n) \
	read_sysreg_s(SYS_BRBSRC##n##_EL1)

#define RETURN_READ_BRBTGTN(n) \
	read_sysreg_s(SYS_BRBTGT##n##_EL1)

#define RETURN_READ_BRBINFN(n) \
	read_sysreg_s(SYS_BRBINF##n##_EL1)

#define BRBE_REGN_SWITCH(x, case_macro)				\
	do {							\
		switch (x) {					\
		case 0: return case_macro(0); break;		\
		case 1: return case_macro(1); break;		\
		case 2: return case_macro(2); break;		\
		case 3: return case_macro(3); break;		\
		case 4: return case_macro(4); break;		\
		case 5: return case_macro(5); break;		\
		case 6: return case_macro(6); break;		\
		case 7: return case_macro(7); break;		\
		case 8: return case_macro(8); break;		\
		case 9: return case_macro(9); break;		\
		case 10: return case_macro(10); break;		\
		case 11: return case_macro(11); break;		\
		case 12: return case_macro(12); break;		\
		case 13: return case_macro(13); break;		\
		case 14: return case_macro(14); break;		\
		case 15: return case_macro(15); break;		\
		case 16: return case_macro(16); break;		\
		case 17: return case_macro(17); break;		\
		case 18: return case_macro(18); break;		\
		case 19: return case_macro(19); break;		\
		case 20: return case_macro(20); break;		\
		case 21: return case_macro(21); break;		\
		case 22: return case_macro(22); break;		\
		case 23: return case_macro(23); break;		\
		case 24: return case_macro(24); break;		\
		case 25: return case_macro(25); break;		\
		case 26: return case_macro(26); break;		\
		case 27: return case_macro(27); break;		\
		case 28: return case_macro(28); break;		\
		case 29: return case_macro(29); break;		\
		case 30: return case_macro(30); break;		\
		case 31: return case_macro(31); break;		\
		default:					\
			pr_warn("unknown register index\n");	\
			return -1;				\
		}						\
	} while (0)

static inline int buffer_to_brbe_idx(int buffer_idx)
{
	return buffer_idx % BRBE_BANK_MAX_ENTRIES;
}

static inline u64 get_brbsrc_reg(int buffer_idx)
{
	int brbe_idx = buffer_to_brbe_idx(buffer_idx);

	BRBE_REGN_SWITCH(brbe_idx, RETURN_READ_BRBSRCN);
}

static inline u64 get_brbtgt_reg(int buffer_idx)
{
	int brbe_idx = buffer_to_brbe_idx(buffer_idx);

	BRBE_REGN_SWITCH(brbe_idx, RETURN_READ_BRBTGTN);
}

static inline u64 get_brbinf_reg(int buffer_idx)
{
	int brbe_idx = buffer_to_brbe_idx(buffer_idx);

	BRBE_REGN_SWITCH(brbe_idx, RETURN_READ_BRBINFN);
}

static inline u64 brbe_record_valid(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_VALID_MASK, brbinf);
}

static inline bool brbe_invalid(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_NONE;
}

static inline bool brbe_record_is_complete(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_FULL;
}

static inline bool brbe_record_is_source_only(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_SOURCE;
}

static inline bool brbe_record_is_target_only(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_TARGET;
}

static inline int brbe_get_in_tx(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_T_MASK, brbinf);
}

static inline int brbe_get_mispredict(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_MPRED_MASK, brbinf);
}

static inline int brbe_get_lastfailed(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_LASTFAILED_MASK, brbinf);
}

static inline int brbe_get_cycles(u64 brbinf)
{
	/*
	 * Captured cycle count is unknown and hence
	 * should not be passed on to the user space.
	 */
	if (brbinf & BRBINFx_EL1_CCU)
		return 0;

	return FIELD_GET(BRBINFx_EL1_CC_MASK, brbinf);
}

static inline int brbe_get_type(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_TYPE_MASK, brbinf);
}

static inline int brbe_get_el(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_EL_MASK, brbinf);
}

static inline int brbe_get_numrec(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_NUMREC_MASK, brbidr);
}

static inline int brbe_get_format(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_FORMAT_MASK, brbidr);
}

static inline int brbe_get_cc_bits(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_CC_MASK, brbidr);
}
