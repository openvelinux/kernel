// SPDX-License-Identifier: GPL-2.0-only
/*
 * IMA supporting functions for TDX RTMR
 */

#include <asm/tdx-rtmr.h>
#include <asm/tdx.h>
/**
 * tdx_rtmr_device() - construct a fake TPM device for IMA usage
 * in RTMR
 */
struct tpm_chip *tdx_rtmr_device(void)
{
	struct tpm_chip *chip;
	u32 default_num = 1;

	pr_debug("Entering tdx_default_device function.\n");
	chip = kzalloc(sizeof(*chip), GFP_KERNEL);
	if (chip == NULL)
		return ERR_PTR(-ENOMEM);

	/*
	 * struct fake tpm bank for tdx.
	 * Only one bank is available(SHA384)
	 */
	chip->allocated_banks =
		kcalloc(1, sizeof(*chip->allocated_banks), GFP_KERNEL);
	if (!chip->allocated_banks) {
		pr_err("Error in allocating banks");
		kfree(chip);
		return ERR_PTR(-ENOMEM);
	}

	chip->allocated_banks[DEFAULT_SHA384_IDX].alg_id = TPM_ALG_SHA384;
	chip->allocated_banks[DEFAULT_SHA384_IDX].digest_size =
		hash_digest_size[HASH_ALGO_SHA384];
	chip->allocated_banks[DEFAULT_SHA384_IDX].crypto_id = HASH_ALGO_SHA384;
	chip->nr_allocated_banks = default_num;

	return chip;
}
EXPORT_SYMBOL_GPL(tdx_rtmr_device);

/**
 * ima_extend_rtmr - extend a RTMR value in SHA384 bank.
 * @chip:       a &struct tpm_chip instance, a fake struct for tdx device
 * @rtmr_idx:   the RTMR register to be retrieved
 * @digests:    array of tpm_digest structures used to extend RTMRs
 *
 */
int ima_extend_rtmr(struct tpm_chip *chip, u32 rtmr_idx,
		    struct tpm_digest *digests)
{
	int rc, i;
	u8 *data;

	/*
	 * RTMR index 2 mapping to PCR[10] and is
	 * allowed for IMA measurement update.
	 */
	if (rtmr_idx != 2)
		return -EINVAL;

	for (i = 0; i < chip->nr_allocated_banks; i++) {
		if (digests[i].alg_id != chip->allocated_banks[i].alg_id)
			return -EINVAL;
	}

	/* TDG.MR.RTMR.EXTEND TDCALL expects buffer to be 64B aligned */
	data = kmalloc(ALIGN(sizeof(digests[DEFAULT_SHA384_IDX].digest), 64),
		       GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	memcpy(data, digests[DEFAULT_SHA384_IDX].digest,
	       sizeof(digests[DEFAULT_SHA384_IDX].digest));

	/* Extend RTMR registers using "TDG.MR.RTMR.EXTEND" TDCALL */
	rc = tdx_mcall_extend_rtmr((u8)rtmr_idx, data);
	kfree(data);

	return rc;
}
EXPORT_SYMBOL_GPL(ima_extend_rtmr);

/**
 * tdx_get_boot_measurements - create a tdx_boot_digests structure to
 * contain TDVM boot measurements stored in MRTD, RTMR[0/1/2]
 */
int tdx_get_boot_measurements(struct tdx_boot_digests *boot_digests)
{
	int rc, i, j, k;
	u8 *reportdata, *tdreport;
	struct tdreport *report;

	reportdata = kmalloc(TDX_REPORTDATA_LEN, GFP_KERNEL);
	if (!reportdata)
		return -ENOMEM;

	tdreport = kzalloc(TDX_REPORT_LEN, GFP_KERNEL);
	if (!tdreport) {
		rc = -ENOMEM;
		kfree(reportdata);
		return rc;
	}

	/* Generate TDREPORT0 using "TDG.MR.REPORT" TDCAL */
	rc = tdx_mcall_get_report0(reportdata, tdreport);
	if (rc) {
		kfree(reportdata);
		kfree(tdreport);
		return rc;
	}

	/* Parse tdreport and retrieve info */
	report = (struct tdreport *)tdreport;

	for (i = 0; i < sizeof(report->tdinfo.mrtd) / sizeof(u64); i++) {
		memcpy(&boot_digests->boot_digest[0][i * 8],
		       &report->tdinfo.mrtd[i], sizeof(u64));
		memcpy(&boot_digests->boot_digest[1][i * 8],
		       &report->tdinfo.rtmr[i], sizeof(u64));
		j = i + 6;
		memcpy(&boot_digests->boot_digest[2][i * 8],
		       &report->tdinfo.rtmr[j], sizeof(u64));
		k = i + 12;
		memcpy(&boot_digests->boot_digest[3][i * 8],
		       &report->tdinfo.rtmr[k], sizeof(u64));
	}

	kfree(reportdata);
	kfree(tdreport);

	return rc;
}
EXPORT_SYMBOL_GPL(tdx_get_boot_measurements);
