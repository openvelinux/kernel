/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2023 Intel Corporation */

#include <linux/tpm.h>
#include <asm/tdx.h>
#include <uapi/linux/tdx-guest.h>

#define DEFAULT_SHA384_IDX 0
#define TDX_GET_REPORT 4

/**
 * struct tdx_boot_digests - TDX guest boot measurements
 */
struct tdx_boot_digests {
	u8 boot_digest[4][64];
};

/**
 * struct tdreport_type - Type header of TDREPORT_STRUCT.
 * @type: Type of the TDREPORT (0 - SGX, 81 - TDX, rest are reserved)
 * @sub_type: Subtype of the TDREPORT (Default value is 0).
 * @version: TDREPORT version (Default value is 0).
 * @reserved: Added for future extension.
 *
 * More details can be found in TDX v1.0 module specification, sec
 * titled "REPORTTYPE".
 */
struct tdreport_type {
	__u8 type;
	__u8 sub_type;
	__u8 version;
	__u8 reserved;
};

/**
 * struct td_info - TDX guest measurements and configuration.
 * @attr: TDX Guest attributes (like debug, spet_disable, etc).
 * @xfam: Extended features allowed mask.
 * @mrtd: Build time measurement register.
 * @mrconfigid: Software-defined ID for non-owner-defined configuration
 *              of the guest - e.g., run-time or OS configuration.
 * @mrowner: Software-defined ID for the guest owner.
 * @mrownerconfig: Software-defined ID for owner-defined configuration of
 *                 the guest - e.g., specific to the workload.
 * @rtmr: Run time measurement registers.
 * @reserved: Added for future extension.
 *
 * It contains the measurements and initial configuration of the TDX guest
 * that was locked at initialization and a set of measurement registers
 * that are run-time extendable. More details can be found in TDX v1.0
 * Module specification, sec titled "TDINFO_STRUCT".
 */
struct td_info {
	__u8 attr[8];
	__u64 xfam;
	__u64 mrtd[6];
	__u64 mrconfigid[6];
	__u64 mrowner[6];
	__u64 mrownerconfig[6];
	__u64 rtmr[24];
	__u64 servtd_hash[6];
	__u64 reserved[8];
};

/**
 * struct reportmac - TDX guest report data, MAC and TEE hashes.
 * @type: TDREPORT type header.
 * @reserved1: Reserved for future extension.
 * @cpu_svn: CPU security version.
 * @tee_tcb_info_hash: SHA384 hash of TEE TCB INFO.
 * @tee_td_info_hash: SHA384 hash of TDINFO_STRUCT.
 * @reportdata: User defined unique data passed in TDG.MR.REPORT request.
 * @reserved2: Reserved for future extension.
 * @mac: CPU MAC ID.
 *
 * It is MAC-protected and contains hashes of the remainder of the
 * report structure along with user provided report data. More details can
 * be found in TDX v1.0 Module specification, sec titled "REPORTMACSTRUCT"
 */
struct reportmac {
	struct tdreport_type type;
	__u8 reserved1[12];
	__u8 cpu_svn[16];
	__u8 tee_tcb_info_hash[48];
	__u8 tee_td_info_hash[48];
	__u8 reportdata[64];
	__u8 reserved2[32];
	__u8 mac[32];
};

/*
 * struct tdreport - Output of TDCALL[TDG.MR.REPORT].
 * @reportmac: Mac protected header of size 256 bytes.
 * @tee_tcb_info: Additional attestable elements in the TCB are not
 *                reflected in the reportmac.
 * @reserved: Added for future extension.
 * @tdinfo: Measurements and configuration data of size 512 bytes.
 *
 * More details can be found in TDX v1.0 Module specification, sec
 * titled "TDREPORT_STRUCT".
 */
struct tdreport {
	struct reportmac reportmac;
	__u8 tee_tcb_info[239];
	__u8 reserved[17];
	struct td_info tdinfo;
};

struct tpm_chip *tdx_rtmr_device(void);
int ima_extend_rtmr(struct tpm_chip *chip, u32 rtmr_idx,
		    struct tpm_digest *digests);
int tdx_get_boot_measurements(struct tdx_boot_digests *boot_digests);
