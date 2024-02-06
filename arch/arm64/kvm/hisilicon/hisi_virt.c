// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright(c) 2022 Huawei Technologies Co., Ltd
 */

#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/init.h>
#include <linux/kvm_host.h>
#include "hisi_virt.h"

static enum hisi_cpu_type cpu_type = UNKNOWN_HI_TYPE;

static const char * const hisi_cpu_type_str[] = {
	"Hisi1612",
	"Hisi1616",
	"Hisi1620",
	"Unknown"
};

/* ACPI Hisi oem table id str */
static const char * const oem_str[] = {
	"HIP06",	/* Hisi 1612 */
	"HIP07",	/* Hisi 1616 */
	"HIP08"		/* Hisi 1620 */
};

/*
 * Probe Hisi CPU type form ACPI.
 */
static enum hisi_cpu_type acpi_get_hisi_cpu_type(void)
{
	struct acpi_table_header *table;
	acpi_status status;
	int i, str_size = ARRAY_SIZE(oem_str);

	/* Get oem table id from ACPI table header */
	status = acpi_get_table(ACPI_SIG_DSDT, 0, &table);
	if (ACPI_FAILURE(status)) {
		pr_warn("Failed to get ACPI table: %s\n",
			acpi_format_exception(status));
		return UNKNOWN_HI_TYPE;
	}

	for (i = 0; i < str_size; ++i) {
		if (!strncmp(oem_str[i], table->oem_table_id, 5))
			return i;
	}

	return UNKNOWN_HI_TYPE;
}

/* of Hisi cpu model str */
static const char * const of_model_str[] = {
	"Hi1612",
	"Hi1616"
};

/*
 * Probe Hisi CPU type from DT.
 */
static enum hisi_cpu_type of_get_hisi_cpu_type(void)
{
	const char *model;
	int ret, i, str_size = ARRAY_SIZE(of_model_str);

	/*
	 * Note: There may not be a "model" node in FDT, which
	 * is provided by the vendor. In this case, we are not
	 * able to get CPU type information through this way.
	 */
	ret = of_property_read_string(of_root, "model", &model);
	if (ret < 0) {
		pr_warn("Failed to get Hisi cpu model by OF.\n");
		return UNKNOWN_HI_TYPE;
	}

	for (i = 0; i < str_size; ++i) {
		if (strstr(model, of_model_str[i]))
			return i;
	}

	return UNKNOWN_HI_TYPE;
}

void probe_hisi_cpu_type(void)
{
	if (!acpi_disabled)
		cpu_type = acpi_get_hisi_cpu_type();
	else
		cpu_type = of_get_hisi_cpu_type();

	kvm_info("detected: Hisi CPU type '%s'\n", hisi_cpu_type_str[cpu_type]);
}

/*
 * We have the fantastic HHA ncsnp capability on Kunpeng 920,
 * with which hypervisor doesn't need to perform a lot of cache
 * maintenance like before (in case the guest has non-cacheable
 * Stage-1 mappings).
 */
#define NCSNP_MMIO_BASE	0x20107E238
bool hisi_ncsnp_supported(void)
{
	void __iomem *base;
	unsigned int high;
	bool supported = false;

	if (cpu_type != HI_1620)
		return supported;

	base = ioremap(NCSNP_MMIO_BASE, 4);
	if (!base) {
		pr_warn("Unable to map MMIO region when probing ncsnp!\n");
		return supported;
	}

	high = readl_relaxed(base) >> 28;
	iounmap(base);
	if (high != 0x1)
		supported = true;

	return supported;
}
