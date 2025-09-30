// SPDX-License-Identifier: GPL-2.0-only
/*
 * IMA supporting functions for TDX RTMR
 */

#include <asm/tdx-rtmr.h>
#include <asm/tdx.h>
#include <linux/list.h>

#define EV_EVENT_TAG		0x00000006U

struct cc_event_head {
	u32 mr_idx;
	u32 event_type;
	u32 count;
};

struct cc_event_data {
	u32 size;
	u8 data[];
} __packed;

struct cc_sha384_event {
	struct cc_event_head head;
	u16 algo_id;
	u8 digest[SHA384_DIGEST_SIZE];
	struct cc_event_data data;
} __packed;

struct rtmr_event {
	struct list_head list;
	struct cc_sha384_event event;
};

static LIST_HEAD(rtmr_event_log_head);

static ssize_t
tdx_rtmr_ccel_read(struct file *file, struct kobject *kobj,
		   struct bin_attribute *bin_attr, char *buf,
		   loff_t off, size_t len)
{
	size_t idx = 0;
	size_t event_size;
	size_t copied = 0;
	struct rtmr_event *pos;

	/* Traverse the event log list */
	list_for_each_entry(pos, &rtmr_event_log_head, list) {
		event_size = sizeof(pos->event) + pos->event.data.size;

		/* Skip events before the offset */
		if (idx + event_size <= off) {
			idx += event_size;
			continue;
		}

		/* Calculate how much to copy from this event */
		size_t event_offset = (off > idx) ? (off - idx) : 0;
		size_t to_copy = min(len - copied, event_size - event_offset);

		/* Copy event data to user buffer */
		memcpy(buf + copied, (char *)&pos->event + event_offset,
		       to_copy);
		copied += to_copy;
		idx += event_size;

		/* Stop if we've filled the buffer */
		if (copied >= len)
			break;
	}

	return copied;
}

static struct bin_attribute bin_attr_tdx_rtmr_ccel __ro_after_init = {
	.attr = { .name = "ccel", .mode = 0400, },
	.read = tdx_rtmr_ccel_read,
};

struct kobject *tdx_rtmr_kobj;

int __init tdx_rtmr_ccel_init(void)
{
	bin_attr_tdx_rtmr_ccel.size = 0;

	tdx_rtmr_kobj = kobject_create_and_add("tdx_rtmr", kernel_kobj);
	if (!tdx_rtmr_kobj)
		return -ENOMEM;

	return sysfs_create_bin_file(tdx_rtmr_kobj, &bin_attr_tdx_rtmr_ccel);
}

static void ccel_record_eventlog(struct rtmr_event *rtmr_event, void *digests,
				 const void *event_data, size_t event_data_len,
				 u8 mr_idx)
{
	struct cc_sha384_event *event = &rtmr_event->event;

	/* Setup Evenlog header */
	event->head.mr_idx = mr_idx + 1;
	event->head.event_type = EV_EVENT_TAG;
	event->head.count = 1;
	event->algo_id = TPM_ALG_SHA384;
	memcpy(event->digest, digests, SHA384_DIGEST_SIZE);

	event->data.size = event_data_len;
	memcpy(event->data.data, event_data, event->data.size);

	list_add_tail(&rtmr_event->list, &rtmr_event_log_head);
}

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
	void *rtmr_event;
	static const char event_data[] = "Runtime RTMR event log extend success";
	static size_t event_data_len = sizeof(event_data);

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
	rtmr_event = kmalloc(sizeof(struct rtmr_event) + event_data_len, GFP_KERNEL);
	if (!rtmr_event) {
		kfree(data);
		return -ENOMEM;
	}

	memcpy(data, digests[DEFAULT_SHA384_IDX].digest,
	       sizeof(digests[DEFAULT_SHA384_IDX].digest));

	/* Extend RTMR registers using "TDG.MR.RTMR.EXTEND" TDCALL */
	rc = tdx_mcall_extend_rtmr((u8)rtmr_idx, data);
	if (!rc)
		ccel_record_eventlog(rtmr_event, data, event_data,
				     event_data_len, (u8)rtmr_idx);
	else
		kfree(rtmr_event);
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
