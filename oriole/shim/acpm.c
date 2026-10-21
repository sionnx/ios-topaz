/*
 * Bare-metal ACPM IPC client for gs101 (Pixel 6). The S2MPG10 main PMIC is
 * reachable only through the APM coprocessor's IPC (no SPMI/I3C path from the
 * AP). The APM firmware is already running in fastboot (it manages DVFS), so
 * its SRAM rings and doorbell are live; we just post commands the way the
 * Linux acpm_mfd driver does.
 *
 * Protocol (drivers/soc/google/acpm/acpm_mfd.c + acpm_ipc.c, confirmed on the
 * live Pixel 6 device tree):
 *   cmd[0] = (TYPE&0xF)<<8 | (REG&0xFF) | (SPEEDY_CH&0xF)<<12 | (seq&0x3f)<<16
 *   cmd[1] = FUNC | VAL<<8 | MASK<<16            (UPDATE: FUNC=2)
 *   cmd[3] = timestamp (0 is fine)
 * TYPE = I2C_ADDR_PMIC = 0x01, SPEEDY_CH = 0 (acpm-speedy-channel), IPC channel
 * = 2 (doorbell bit + ring select). Reply: seq echoed in cmd[0] bits[21:16],
 * status in cmd[1] bits[31:24] (0 = ok), read value in cmd[1] bits[15:8].
 *
 * Ring layout is parsed at runtime from the framework in SRAM (don't hardcode
 * base/front/rear/sizes). See NOTES.md ORIOLE.
 */
#include <stdint.h>
#include "../common/con.h"
#include "../common/gs101.h"
#include "acpm.h"

#define APM_SRAM	0x02039000UL	/* live DT acpm_ipc reg[1] */
#define ACPM_FW		(APM_SRAM + 0xa000)	/* initdata-base 0xa000 -> 0x02043000 */
#define MBOX_INTR	0x17610000UL	/* live DT acpm_ipc reg[0] */
#define APM_INTGR	0x40		/* INTGR1 doorbell */
#define MFD_CH		2		/* acpm-ipc-channel for the MFD/PMIC */
#define IPC_CH_SIZE	0x48		/* sizeof(struct ipc_channel) */

#define PMIC_TYPE	0x01		/* I2C_ADDR_PMIC */
#define SPEEDY_CH	0
#define FUNC_READ	0
#define FUNC_WRITE	1
#define FUNC_UPDATE	2

/* channel_info sits at ipc_channel + 0x10; these are its internal offsets
 * (framework.h). Parsed/printed and sanity-checked at runtime. */
static uint32_t ch_desc;		/* absolute SRAM addr of ch2 ipc_channel */
static uint32_t rx_base, rx_front_a, rx_rear_a;	/* AP->APM (send) ring */
static uint32_t tx_base, tx_front_a, tx_rear_a;	/* APM->AP (recv) ring */
static uint32_t q_len, q_esz;
static int acpm_ok;
static uint8_t seqno = 1;

static void adelay_us(uint32_t us)
{
	uint64_t end = now() + (rsr("cntfrq_el0") / 1000000) * us;
	while (now() < end)
		;
}

void acpm_init(void)
{
	uint32_t ipc_channels = rd32(ACPM_FW + 0x08);
	ch_desc = APM_SRAM + ipc_channels + MFD_CH * IPC_CH_SIZE;
	out("ACPM: fw %x ipc_channels %x ch2 @ %x\n", (uint32_t)ACPM_FW, ipc_channels, ch_desc);

	/* Raw dump of the ch2 descriptor so the ring offsets can be verified
	 * against the SRAM contents (base values read back as 0x020xxxxx). */
	for (uint32_t i = 0; i < 0x50; i += 16)
		out("  ch2+%x: %x %x %x %x\n", i, rd32(ch_desc + i), rd32(ch_desc + i + 4),
		    rd32(ch_desc + i + 8), rd32(ch_desc + i + 12));

	/* channel_info at ch_desc+0x10. The rear/front fields hold a POINTER (SRAM
	 * offset) to where the actual index lives, not the index itself (verified
	 * by the ch2 dump: 0x382e8/ec/f0 are consecutive addresses). base is the
	 * data-ring SRAM offset. So rear/front need one more dereference. */
	rx_rear_a  = APM_SRAM + rd32(ch_desc + 0x10);
	rx_front_a = APM_SRAM + rd32(ch_desc + 0x14);
	rx_base    = rd32(ch_desc + 0x18);
	tx_rear_a  = APM_SRAM + rd32(ch_desc + 0x28);
	tx_front_a = APM_SRAM + rd32(ch_desc + 0x2C);
	tx_base    = rd32(ch_desc + 0x30);
	q_len      = rd32(ch_desc + 0x34);
	q_esz      = rd32(ch_desc + 0x38);
	out("ACPM: rx_base %x front@%x(=%x) rear@%x(=%x) tx_base %x front@%x(=%x) rear@%x(=%x) qlen %x qesz %x\n",
	    rx_base, rx_front_a, rd32(rx_front_a), rx_rear_a, rd32(rx_rear_a),
	    tx_base, tx_front_a, rd32(tx_front_a), tx_rear_a, rd32(tx_rear_a), q_len, q_esz);

	/* Sanity: bases must point inside APM SRAM (256 KiB), sizes reasonable. */
	acpm_ok = (rx_base < 0x40000 && tx_base < 0x40000 && q_len >= 1 && q_len <= 64 &&
		   q_esz >= 16 && q_esz <= 64);
	if (!acpm_ok)
		out("ACPM: ring pointers look wrong (offsets need adjusting) - not sending\n");
}

/* Post one IPC command, wait for the reply with our seq. Returns cmd[1] in
 * *resp (status in bits[31:24], read value in bits[15:8]). */
static int acpm_cmd(uint32_t c0, uint32_t c1, uint32_t *resp)
{
	if (!acpm_ok)
		return -1;
	uint8_t s = seqno++ & 0x3f;
	if (seqno == 0)
		seqno = 1;
	c0 |= (uint32_t)s << 16;

	uint32_t front = rd32(rx_front_a);
	uint32_t slot = APM_SRAM + rx_base + q_esz * front;
	wr32(slot + 0, c0);
	wr32(slot + 4, c1);
	wr32(slot + 8, 0);
	wr32(slot + 12, 0);
	for (uint32_t o = 16; o < q_esz; o += 4)
		wr32(slot + o, 0);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(rx_front_a, (front + 1) % q_len);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(MBOX_INTR + APM_INTGR, 1u << MFD_CH);	/* doorbell */

	uint64_t end = now() + rsr("cntfrq_el0") / 5;	/* 200 ms */
	while (now() < end) {
		uint32_t tf = rd32(tx_front_a);
		uint32_t tr = rd32(tx_rear_a);
		if (tf != tr) {
			uint32_t tslot = APM_SRAM + tx_base + q_esz * tr;
			uint32_t r0 = rd32(tslot), r1 = rd32(tslot + 4);
			wr32(tx_rear_a, (tr + 1) % q_len);
			__asm__ volatile("dsb sy" ::: "memory");
			if (((r0 >> 16) & 0x3f) == s) {
				if (resp)
					*resp = r1;
				return 0;
			}
			/* a reply for someone else; keep draining */
		}
	}
	return -1;
}

int acpm_pmic_read(uint8_t reg, uint8_t *val)
{
	uint32_t c0 = (PMIC_TYPE << 8) | reg | (SPEEDY_CH << 12);
	uint32_t c1 = FUNC_READ;
	uint32_t r = 0;
	int rc = acpm_cmd(c0, c1, &r);
	out("ACPM read reg %x -> rc %d resp %x (val %x status %x)\n", reg, rc, r,
	    (r >> 8) & 0xff, (r >> 24) & 0xff);
	if (val)
		*val = (r >> 8) & 0xff;
	return rc;
}

int acpm_pmic_update(uint8_t reg, uint8_t val, uint8_t mask)
{
	uint32_t c0 = (PMIC_TYPE << 8) | reg | (SPEEDY_CH << 12);
	uint32_t c1 = FUNC_UPDATE | ((uint32_t)val << 8) | ((uint32_t)mask << 16);
	uint32_t r = 0;
	int rc = acpm_cmd(c0, c1, &r);
	out("ACPM update reg %x val %x mask %x -> rc %d resp %x (status %x)\n", reg, val,
	    mask, rc, r, (r >> 24) & 0xff);
	(void)adelay_us;
	return rc;
}
