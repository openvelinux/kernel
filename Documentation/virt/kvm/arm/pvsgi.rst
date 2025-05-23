.. SPDX-License-Identifier: GPL-2.0

Paravirtualized SGI support for HiSilicon
==========================================

KVM/arm64 provides some hypervisor service calls to support a paravirtualized
SGI(software generated interrupt) in HiSilicon Hip12 SoC.

Some SMCCC compatible hypercalls are defined:

* PV_SGI_FEATURES:      0xC6000090
* PV_SGI_ENABLE:        0xC6000091

The existence of the PV_SGI hypercall should be probed using the SMCCC 1.1
ARCH_FEATURES mechanism before calling it.

PV_SGI_FEATURES

    ============= ========    ==========
    Function ID:  (uint32)    0xC6000090
    PV_call_id:   (uint32)    The function to query for support.
                              Currently only PV_SGI_ENABLE is supported.
    Return value: (int64)     NOT_SUPPORTED (-1) or SUCCESS (0) if the relevant
                              PV-sgi feature is supported by the hypervisor.
    ============= ========    ==========

PV_SGI_ENABLE

    ============= ========    ==========
    Function ID:  (uint32)    0xC6000091
    Return value: (int64)     NOT_SUPPORTED (-1) or SUCCESS (0) if this feature
                              has been enabled.
    ============= ========    ==========
