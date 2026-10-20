/*
 * Pixel 6 (oriole, gs101) addresses and helpers shared by the probe and the
 * shim. Values from the live device tree and probe 1-3 (NOTES.md, ORIOLE).
 */
#pragma once
#include <stdint.h>

#define FB_BASE		0xfac00000UL	/* mainline gs101: splash@fac00000, DPP0 scans it */
#define FB_W		1080
#define FB_H		2400
#define FB_STRIDE	(FB_W * 4)	/* a8r8g8b8 */
#define FB_SIZE		0x9e3400UL

#define RAMOOPS_BASE	0xfd3ff000UL	/* live DT: ramoops_mem, console zone first */
#define RAMOOPS_CONSOLE	0x200000UL	/* console-size */
#define PRZ_SIG		0x43474244U	/* "DBGC": PERSISTENT_RAM_SIG ^ 0 */

#define GICD_BASE	0x10400000UL	/* GIC-600 */
#define GICR_BASE	0x10440000UL
#define GICR_STRIDE	0x20000UL
#define WDT_CL0		0x10060000UL	/* both run with reset enabled at entry */
#define WDT_CL1		0x10070000UL

#define DPP_DMA(ch)		(0x1c0b0000UL + (ch) * 0x1000UL)
#define RDMA_IN_CTRL_0		0x08
#define RDMA_SRC_SIZE		0x10
#define RDMA_IMG_SIZE		0x18
#define RDMA_BASEADDR_Y8	0x40
#define DECON0			0x1c300000UL
#define DECON_TRIG_CON		0x30
#define DECON_TRIG_HW		((1U << 13) | (1U << 12) | (1U << 6) | (1U << 5) | 1U)
#define DECON_SHD_REG_UP_REQ	0x50
#define DECON_SHD_UP_ALL	((1U << 31) | (1U << 20) | 0x3fU)

/* Kernel-managed RAM: the upper 6 GiB, no reserved-memory in it */
#define XNU_PHYS_BASE	0x880000000UL
#define XNU_MEM_SIZE	0x180000000UL

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_OFF		0x84000008UL
#define PSCI_SYSTEM_RESET	0x84000009UL	/* NOT_SUPPORTED on this firmware */
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS		8	/* MPIDR Aff1 = 0..7: 4x A55, 2x A76, 2x X1 */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }
