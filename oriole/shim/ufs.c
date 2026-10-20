/*
 * Read-only UFS probe for oriole: take over the UFS host the stock
 * bootloader leaves running under `fastboot boot` (its log has no "UFS
 * shutdown complete" on that path) the way topaz's xnu driver does
 * (bsd/dev/sweet_ufs.c ufs_take_over_host + ufs_scsi), with the Exynos
 * differences from Linux ufs-exynos.c / the oriole U-Boot port:
 *   - UFSHCD_QUIRK_PRDT_BYTE_GRAN: UTRD response/PRDT offsets and lengths
 *     are in bytes, not DWORDs
 *   - HCI_UTRL_NEXUS_TYPE (vendor HCI + 0x40): bit per slot, set for SCSI
 * Then TEST UNIT READY, INQUIRY and READ(10) of LUN0 block 1 (GPT header),
 * in the bootloader's own list. Found so far: the bootloader leaves the link
 * in hibern8 (DME_HIBERNATE_EXIT needed before any command).
 * Nothing is written to the device. MMU off: descriptors and data are in
 * uncached memory, no cache maintenance.
 */
#include <stdint.h>
#include "../common/con.h"
#include "../common/gs101.h"
#include "ufs.h"

#define UFS_HCI		0x14700000UL	/* UFSHCI */
#define UFS_VS_HCI	0x14701100UL	/* Exynos vendor HCI */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_IE		0x24
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)

#define REG_AHIT	0x18
#define REG_UICCMD	0x90
#define REG_UCMDARG1	0x94
#define REG_UCMDARG2	0x98
#define REG_UCMDARG3	0x9c
#define IS_UCCS		(1u << 10)
#define HCS_UCRDY	(1u << 3)

#define HCI_TXPRDT_ENTRY_SIZE	0x00
#define HCI_RXPRDT_ENTRY_SIZE	0x04
#define HCI_UTRL_NEXUS_TYPE	0x40
#define HCI_DATA_REORDER	0x60
#define HCI_SW_RST		0x50
#define HCI_V2P1_CTRL		0x8c
#define HCI_CLKSTOP_CTRL	0xb0
#define HCI_MISC		0xb4
#define HCI_IOP_ACG_DISABLE	0x100
#define CLK_STOP_MASK		0x1f
#define CLK_CTRL_EN_MASK	((1u << 7) | (1u << 6) | (1u << 5))

#define UFS_BLOCK	4096u

struct utrd {
	uint32_t dw0, dw1, dw2, dw3;
	uint32_t ucdba, ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (bytes here) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] PRDT length (bytes here) */
};

struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;
	} prdt[1];
};

static uint8_t tag;
/* Where slot 0's UTRD, the UCD and the data buffer live for scsi(). */
static uintptr_t utrd_at, ucd_at, data_at;

static uint32_t hci_rd(uint32_t off) { return rd32(UFS_HCI + off); }
static void hci_wr(uint32_t off, uint32_t v) { wr32(UFS_HCI + off, v); }

static void zero(volatile void *p, uint32_t n)
{
	volatile uint8_t *b = p;
	for (uint32_t i = 0; i < n; i++)
		b[i] = 0;
}

static uint8_t cur_lun;	/* 0 = LUN0 (sda); 0xd0 = UFS Device well-known LUN */
static uint8_t last_key, last_asc, last_ascq;

/* One SCSI command in slot 0, data in (to `data`) if len. 0 or an error code. */
static int scsi(const uint8_t *cdb, unsigned cdb_len, uint32_t len)
{
	volatile struct utrd *u = (volatile struct utrd *)utrd_at;
	volatile struct ucd *c = (volatile struct ucd *)ucd_at;
	uint64_t ucd_pa = ucd_at, data_pa = data_at;

	zero(u, sizeof(*u));
	zero(c, sizeof(*c));
	c->cmd[0] = 0x01;			/* COMMAND UPIU */
	c->cmd[1] = len ? 0x40 : 0x00;		/* R: data in */
	c->cmd[2] = cur_lun;
	c->cmd[3] = ++tag;
	c->cmd[12] = (uint8_t)(len >> 24);
	c->cmd[13] = (uint8_t)(len >> 16);
	c->cmd[14] = (uint8_t)(len >> 8);
	c->cmd[15] = (uint8_t)len;
	for (unsigned i = 0; i < cdb_len; i++)
		c->cmd[16 + i] = cdb[i];

	u->dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS cmd, DD, I */
	u->dw2 = 0x0f;				/* OCS invalid until updated */
	u->ucdba = (uint32_t)ucd_pa;
	u->ucdbau = (uint32_t)(ucd_pa >> 32);
	u->resp = (512u << 16) | 512u;		/* PRDT_BYTE_GRAN */
	if (len) {
		c->prdt[0].addr_lo = (uint32_t)data_pa;
		c->prdt[0].addr_hi = (uint32_t)(data_pa >> 32);
		c->prdt[0].dbc = len - 1;
		u->prdt = (1024u << 16) | 16u;	/* one 16-byte entry, PRDT_BYTE_GRAN */
	}
	wr32(UFS_VS_HCI + HCI_UTRL_NEXUS_TYPE, rd32(UFS_VS_HCI + HCI_UTRL_NEXUS_TYPE) | 1u);
	__asm__ volatile("dsb sy" ::: "memory");
	hci_wr(REG_UTRLDBR, 1);

	uint64_t end = now() + rsr("cntfrq_el0") * 2;
	while (hci_rd(REG_UTRLDBR) & 1) {
		if (now() > end) {
			hci_wr(REG_UTRLCLR, 1);	/* UFSHCI_QUIRK_BROKEN_REQ_LIST_CLR: bit set = clear */
			out("UFS: cmd %x timed out, IS %x, OCS %x\n", cdb[0], hci_rd(REG_IS), u->dw2);
			return -1;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	hci_wr(REG_IS, hci_rd(REG_IS));

	/*
	 * UFSHCD_QUIRK_BROKEN_OCS_FATAL_ERROR: the host reports OCS 7 whenever
	 * the response UPIU carries a failure; judge by the UPIU then.
	 * Response UPIU: [6] response, [7] SCSI status, sense data from [34]
	 * (fixed format: key [36] & 0xf, ASC [46], ASCQ [47]).
	 */
	uint32_t ocs = u->dw2 & 0xff;
	last_key = c->resp[36] & 0xf;
	last_asc = c->resp[46];
	last_ascq = c->resp[47];
	if ((ocs && ocs != 7) || c->resp[0] != 0x21 || c->resp[6] || c->resp[7]) {
		out("UFS: cmd %x lun %x failed: ocs %x upiu %x response %x status %x sense %x/%x/%x\n", cdb[0],
		    cur_lun, ocs, c->resp[0], c->resp[6], c->resp[7], last_key, last_asc, last_ascq);
		return -2;
	}
	return 0;
}

static void hex16(const char *label, const uint8_t *p)
{
	out("%s", label);
	for (int i = 0; i < 16; i++)
		out(" %c", "0123456789abcdef"[p[i] >> 4]), out("%c", "0123456789abcdef"[p[i] & 15]);
	out("\n");
}

static int is_gpt(const volatile uint8_t *d)
{
	static const char m[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
	for (int i = 0; i < 8; i++)
		if (d[i] != (uint8_t)m[i])
			return 0;
	return 1;
}

/* TUR, INQUIRY, READ(10) LUN0 LBA1 with the current utrd_at/ucd_at/data_at. */
static int probe_cmds(const char *how)
{
	out("UFS [%s]: utrd %lx ucd %lx data %lx\n", how, (uint64_t)utrd_at, (uint64_t)ucd_at,
	    (uint64_t)data_at);
	static const uint8_t tur[6] = { 0x00 };
	/* START STOP UNIT to the UFS Device W-LUN: power condition ACTIVE (no data) */
	static const uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
	int err = -1;
	for (int i = 0; i < 6 && err; i++) {
		err = scsi(tur, sizeof(tur), 0);
		if (err && last_key == 2 && i == 2) {	/* NOT READY: wake the device */
			cur_lun = 0xd0;
			int r = scsi(ssu, sizeof(ssu), 0);
			cur_lun = 0;
			out("UFS [%s]: START STOP UNIT (active) to W-LUN d0: %s\n", how, r ? "failed" : "ok");
		}
	}
	out("UFS [%s]: TEST UNIT READY %s\n", how, err ? "failed" : "ok");
	if (err)
		return err;
	static const uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
	if (scsi(inq, sizeof(inq), 36) == 0) {
		char id[25];
		const volatile uint8_t *d = (const volatile uint8_t *)data_at;
		for (int i = 0; i < 24; i++)
			id[i] = d[8 + i] >= 0x20 && d[8 + i] < 0x7f ? (char)d[8 + i] : '.';
		id[24] = 0;
		out("UFS [%s]: INQUIRY '%s'\n", how, id);
	}
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };	/* LBA 1, 1 block */
	zero((volatile void *)data_at, 16);
	err = scsi(rd10, sizeof(rd10), UFS_BLOCK);
	if (!err) {
		hex16("UFS: LUN0 LBA1:", (const uint8_t *)data_at);
		err = is_gpt((const volatile uint8_t *)data_at) ? 0 : -3;
		out("UFS [%s]: GPT header %s\n", how, err ? "NOT found" : "found (EFI PART)");
	}
	return err;
}

/* UIC command; returns the config result code (ARG2[7:0]) or -1 on timeout. */
static int uic(uint32_t cmd, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t *out3)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 2;
	while (!(hci_rd(REG_HCS) & HCS_UCRDY))
		if (now() > end)
			return -1;
	hci_wr(REG_IS, IS_UCCS);
	hci_wr(REG_UCMDARG1, arg1);
	hci_wr(REG_UCMDARG2, arg2);
	hci_wr(REG_UCMDARG3, arg3);
	hci_wr(REG_UICCMD, cmd);
	end = now() + rsr("cntfrq_el0") / 2;
	while (!(hci_rd(REG_IS) & IS_UCCS))
		if (now() > end)
			return -1;
	hci_wr(REG_IS, IS_UCCS);
	if (out3)
		*out3 = hci_rd(REG_UCMDARG3);
	return (int)(hci_rd(REG_UCMDARG2) & 0xff);
}

static void dme_get(const char *name, uint32_t attr)
{
	uint32_t v = 0;
	int r = uic(0x01, attr << 16, 0, 0, &v);	/* DME_GET */
	out("UFS: DME_GET %s (%x): result %x value %x\n", name, attr, (uint32_t)r, v);
}

void ufs_probe(void)
{
	out("UFS: cap %x ver %x hce %x hcs %x is %x ie %x ahit %x\n", hci_rd(REG_CAP), hci_rd(REG_VER),
	    hci_rd(REG_HCE), hci_rd(REG_HCS), hci_rd(REG_IS), hci_rd(REG_IE), hci_rd(REG_AHIT));
	out("UFS: utrlba %x:%x rsr %x dbr %x; vs tx/rxprdt %x/%x nexus %x reorder %x\n",
	    hci_rd(REG_UTRLBAU), hci_rd(REG_UTRLBA), hci_rd(REG_UTRLRSR), hci_rd(REG_UTRLDBR),
	    rd32(UFS_VS_HCI + HCI_TXPRDT_ENTRY_SIZE), rd32(UFS_VS_HCI + HCI_RXPRDT_ENTRY_SIZE),
	    rd32(UFS_VS_HCI + HCI_UTRL_NEXUS_TYPE), rd32(UFS_VS_HCI + HCI_DATA_REORDER));
	out("UFS: vs clkstop %x misc %x acg_dis %x v2p1 %x sw_rst %x\n",
	    rd32(UFS_VS_HCI + HCI_CLKSTOP_CTRL), rd32(UFS_VS_HCI + HCI_MISC),
	    rd32(UFS_VS_HCI + HCI_IOP_ACG_DISABLE), rd32(UFS_VS_HCI + HCI_V2P1_CTRL),
	    rd32(UFS_VS_HCI + HCI_SW_RST));

	uint32_t hce = hci_rd(REG_HCE), hcs = hci_rd(REG_HCS);
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		out("UFS: host not ready, no takeover\n");
		return;
	}
	if (hci_rd(REG_UTRLDBR)) {
		out("UFS: requests outstanding, no takeover\n");
		return;
	}
	hci_wr(REG_IE, 0);
	hci_wr(REG_IS, hci_rd(REG_IS));

	dme_get("PA_PWRMODE", 0x1571);
	dme_get("PA_RXGEAR", 0x1583);
	dme_get("PA_TXGEAR", 0x1568);
	dme_get("PA_ACTIVETXDATALANES", 0x1560);

	/* The bootloader's own list: descriptors where it kept them, data in the abl carve-out. */
	uintptr_t bl_list = ((uint64_t)hci_rd(REG_UTRLBAU) << 32) | hci_rd(REG_UTRLBA);
	const volatile uint32_t *d0 = (const volatile uint32_t *)bl_list;
	utrd_at = bl_list;
	ucd_at = ((uint64_t)d0[5] << 32) | (d0[4] & ~0x7fu);
	data_at = 0xf8d00000;	/* inside reserved-memory abl@f8800000 (16 MiB) */
	if (!probe_cmds("A as left"))
		goto ok;

	/* B: clocks: no clock stop, no automatic clock control (Linux exynos_ufs_ctrl_clkstop(false)) */
	wr32(UFS_VS_HCI + HCI_CLKSTOP_CTRL, rd32(UFS_VS_HCI + HCI_CLKSTOP_CTRL) & ~CLK_STOP_MASK);
	wr32(UFS_VS_HCI + HCI_MISC, rd32(UFS_VS_HCI + HCI_MISC) & ~CLK_CTRL_EN_MASK);
	out("UFS: clkstop -> %x misc -> %x\n", rd32(UFS_VS_HCI + HCI_CLKSTOP_CTRL), rd32(UFS_VS_HCI + HCI_MISC));
	if (!probe_cmds("B clocks on"))
		goto ok;

	/* C: link out of hibern8 */
	hci_wr(REG_AHIT, 0);
	int r = uic(0x18, 0, 0, 0, 0);	/* DME_HIBERNATE_EXIT */
	out("UFS: DME_HIBERNATE_EXIT result %x, IS %x\n", (uint32_t)r, hci_rd(REG_IS));
	dme_get("PA_PWRMODE", 0x1571);
	if (!probe_cmds("C h8 exit"))
		goto ok;
	return;
ok:
	/* D: data DMA straight into xnu's RAM above 4 GiB */
	data_at = XNU_PHYS_BASE + 0x80000000UL;
	probe_cmds("D data >4G");
}
