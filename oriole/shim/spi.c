/*
 * Read-only Exynos SPI11 + ST FTM5 touch probe for oriole (Pixel 6). Takes
 * over the SPI controller the bootloader leaves running (Android used the
 * touchscreen), reads the FTM5 chip id over SPI, and reports it. Validates
 * the USI/SPI/CS-GPIO programming in bare metal before the kernel driver.
 * See oriole/TOUCH.md for the full spec. Nothing is written to the touch IC
 * except, optionally, a HW system reset.
 */
#include <stdint.h>
#include "../common/con.h"
#include "../common/gs101.h"
#include "spi.h"

#define SPI_BASE	0x10D40000UL	/* samsung,exynos-spi (spi11) */
#define USI_SYSREG	0x10C21000UL	/* syscon@10C21000: SW_CONF at +0xC */
#define USI_SW_CONF_OFF	0xC
#define USI_SPI_SW_CONF	0x2		/* bits[2:0] = 0b010 selects SPI */
#define PINCTRL_PERIC1	0x10C40000UL	/* google,gs101-pinctrl */

/* gpp23 bank offset within PINCTRL_PERIC1 (gs101 SoC data). CON at +0x00
 * (4 bits/pin), DAT at +0x04 (1 bit/pin). CS = gpp23 pin 3, active low. */
#ifndef GPP23_OFF
#define GPP23_OFF	0x0		/* TODO: fill from gs101 pinctrl data */
#endif
#define CS_PIN		3

/* Exynos SPI registers */
#define CH_CFG		0x00
#define CLK_CFG		0x04
#define MODE_CFG	0x08
#define SLAVE_SEL	0x0C
#define INT_EN		0x10
#define SPI_STATUS	0x14
#define TX_DATA		0x18
#define RX_DATA		0x1C
#define PACKET_CNT	0x20
#define PENDING_CLR	0x24
#define USI_CON		0xC4
#define CH_HS_EN	(1u << 6)
#define CH_SW_RST	(1u << 5)
#define CH_SLAVE	(1u << 4)
#define CH_RXCH_ON	(1u << 1)
#define CH_TXCH_ON	(1u << 0)
#define SS_SIG_INACT	(1u << 0)
#define PKT_EN		(1u << 16)

static uint32_t sr(uint32_t off) { return rd32(SPI_BASE + off); }
static void sw(uint32_t off, uint32_t v) { wr32(SPI_BASE + off, v); }

/* Drive the CS GPIO (gpp23 pin 3): out=output+value. */
static void cs_set(int high)
{
	uintptr_t con = PINCTRL_PERIC1 + GPP23_OFF + 0x00;
	uintptr_t dat = PINCTRL_PERIC1 + GPP23_OFF + 0x04;
	uint32_t c = rd32(con);
	c &= ~(0xfu << (CS_PIN * 4));
	c |= (1u << (CS_PIN * 4));	/* output */
	wr32(con, c);
	uint32_t d = rd32(dat);
	if (high)
		d |= (1u << CS_PIN);
	else
		d &= ~(1u << CS_PIN);
	wr32(dat, d);
	__asm__ volatile("dsb sy" ::: "memory");
}

static void spi_hwinit(void)
{
	/* USI: select SPI function, then clear USI reset. */
	uint32_t c = rd32(USI_SYSREG + USI_SW_CONF_OFF);
	wr32(USI_SYSREG + USI_SW_CONF_OFF, (c & ~0x7u) | USI_SPI_SW_CONF);
	sw(USI_CON, 0);
	__asm__ volatile("isb");
	sw(CH_CFG, CH_SW_RST);
	for (volatile int i = 0; i < 1000; i++)
		;
	sw(CH_CFG, 0);			/* master, mode 0, channels off */
	sw(SLAVE_SEL, SS_SIG_INACT);
	sw(INT_EN, 0);
	sw(MODE_CFG, 0);		/* byte transfers, no DMA */
	sw(PACKET_CNT, 0);
	sw(PENDING_CLR, 0x1f);
}

/* Full-duplex `len` bytes: send tx[] (or zeros), fill rx[]. CS stays as set. */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	uint32_t ch = CH_RXCH_ON | CH_TXCH_ON;
	sw(PACKET_CNT, 0);
	sw(PACKET_CNT, PKT_EN | (len & 0xffff));
	for (uint32_t i = 0; i < len; i++)
		sw(TX_DATA, tx ? tx[i] : 0);
	sw(CH_CFG, ch);			/* start */
	uint64_t end = now() + rsr("cntfrq_el0") / 10;	/* 100 ms */
	while (((sr(SPI_STATUS) >> 15) & 0x7f) < len) {
		if (now() > end) {
			out("SPI: rx timeout, STATUS %x lvl %d\n", sr(SPI_STATUS),
			    (sr(SPI_STATUS) >> 15) & 0x7f);
			return -1;
		}
	}
	for (uint32_t i = 0; i < len; i++)
		rx[i] = (uint8_t)sr(RX_DATA);
	sw(CH_CFG, 0);
	return 0;
}

/* FTS_CMD_HW_REG_R (0xFB) + 4-byte big-endian address, read toRead + 1 dummy. */
static int ftm5_hwreg_read(uint32_t addr, uint8_t *out_buf, uint32_t toRead)
{
	uint8_t tx[8] = { 0xFB, (uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
			  (uint8_t)(addr >> 8), (uint8_t)addr };
	uint8_t rx[16] = { 0 };
	uint32_t n = 5 + 1 + toRead;	/* cmd+addr clocked while TX, then dummy+data */
	if (n > sizeof(rx))
		return -1;
	cs_set(0);
	int r = spi_xfer(tx, rx, n);
	cs_set(1);
	if (r)
		return r;
	/* data starts after cmd(5) + 1 dummy */
	for (uint32_t i = 0; i < toRead; i++)
		out_buf[i] = rx[5 + 1 + i];
	return 0;
}

void spi_probe(void)
{
	out("SPI: CH_CFG %x STATUS %x USI_CON %x SW_CONF %x\n", sr(CH_CFG), sr(SPI_STATUS),
	    sr(USI_CON), rd32(USI_SYSREG + USI_SW_CONF_OFF));
	out("SPI: gpp23 CON %x DAT %x (GPP23_OFF %x)\n", rd32(PINCTRL_PERIC1 + GPP23_OFF),
	    rd32(PINCTRL_PERIC1 + GPP23_OFF + 4), (uint32_t)GPP23_OFF);
	spi_hwinit();
	uint8_t id[2] = { 0, 0 };
	if (ftm5_hwreg_read(0x20000000, id, 2) == 0)
		out("FTM5: chip id %x %x (expect 36 48) %s\n", id[0], id[1],
		    (id[0] == 0x36 && id[1] == 0x48) ? "OK" : "??");
	else
		out("FTM5: chip id read failed\n");
}
