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
#include "acpm.h"

#define SPI_BASE	0x10D40000UL	/* samsung,exynos-spi (spi11) */
#define USI_SYSREG	0x10C21000UL	/* syscon@10C20000 +0x100C; 0x10C21000+0xC is the same PA */
#define USI_SW_CONF_OFF	0xC
#define USI_SPI_SW_CONF	0x2		/* bits[2:0] = 0b010 selects SPI */
#define PINCTRL_PERIC1	0x10C40000UL	/* google,gs101-pinctrl */
#define USI_OPTION	0xC8		/* SPI_BASE + 0xC8: HWACG CLKREQ/CLKSTOP */
#define USI_OPT_CLKREQ_ON  (1u << 1)
#define USI_OPT_CLKSTOP_ON (1u << 2)

/* CMU_PERIC1: the bootloader leaves spi11/usi11 unclocked (Android's touch
 * driver clocks it). Enable it here. All gs101 PERIC1 gates use bit 21.
 * (torvalds/linux clk-gs101.c; DT ties spi11 to IPCLK_4 + PCLK_4.) */
#define CMU_PERIC1	0x10C00000UL
#define CMU_MUX_USI11	0x0640		/* bit4: 0=oscclk(24M), 1=dout_cmu_peric1_ip */
#define CMU_DIV_USI11	0x180C		/* bits[3:0] = divider-1 */
#define CMU_GATE_IPCLK4	0x202C		/* bit21: spi11 ipclk (function clock) */
#define CMU_GATE_PCLK4	0x204C		/* bit21: spi11 pclk (APB bus clock) */
#define CMU_GATE_USI11	0x2068		/* bit21: usi11 reset-sync clk (belt-and-braces) */
#define CMU_GATE_BIT	(1u << 21)

/* gpp23 bank offset within PINCTRL_PERIC1 (gs101 SoC data: gs101_pin_peric1[]
 * GS101_PIN_BANK_EINTG(8, 0x60, "gpp23", ...) in pinctrl-exynos-arm64.c).
 * CON at bank+0x00 (4 bits/pin, 1=output), DAT at bank+0x04 (1 bit/pin).
 * CS = gpp23 pin 3, active low. Absolute: CON 0x10C40060, DAT 0x10C40064. */
#ifndef GPP23_OFF
#define GPP23_OFF	0x60
#endif
#define CS_PIN		3
/* FTM5 reset = gpp26 pin0 on the same PERIC1 pinctrl (st,reset-gpio, live DT).
 * gpp26 bank offset 0xc0 (gs101_pin_peric1[]). Active low: low = reset. */
#define GPP26_OFF	0xc0
#define RST_PIN		0

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
#define SWAP_CFG	0x28
#define FB_CLK		0x2C		/* feedback clock delay (master RX sampling) */
#define USI_CON		0xC4
#define CH_HS_EN	(1u << 6)
#define CH_SW_RST	(1u << 5)
#define CH_SLAVE	(1u << 4)
#define MODE_SELF_LOOPBACK (1u << 3)	/* MODE_CFG: internal TX->RX loopback */
#define CH_RXCH_ON	(1u << 1)
#define CH_TXCH_ON	(1u << 0)
/* CS_REG (== SLAVE_SEL 0x0C). gs101 quirk = CS_AUTO: the controller drives its
 * own chip-select and only generates SCLK when CS_AUTO is set. SIG_INACT holds
 * CS deasserted -> no clock (that was the TX-stuck bug). NSC_CNT_2 = CS-to-clock
 * setup cycles. (s3c64xx_spi_set_cs, gs101_spi_port_config.) */
#define SS_SIG_INACT	(1u << 0)
#define SS_CS_AUTO	(1u << 1)
#define SS_NSC_CNT_2	(2u << 4)
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

static void cmu_clk_enable(void)
{
	/* Select oscclk (24 MHz, no PLL dependency), /4 -> 6 MHz SCLK (< FTM5
	 * 10 MHz max). clk_from_cmu=true: SPI rate comes from this CMU divider. */
	wr32(CMU_PERIC1 + CMU_MUX_USI11, rd32(CMU_PERIC1 + CMU_MUX_USI11) & ~(1u << 4));
	uint32_t dv = rd32(CMU_PERIC1 + CMU_DIV_USI11);
	wr32(CMU_PERIC1 + CMU_DIV_USI11, (dv & ~0xfu) | 3);
	wr32(CMU_PERIC1 + CMU_GATE_IPCLK4, rd32(CMU_PERIC1 + CMU_GATE_IPCLK4) | CMU_GATE_BIT);
	wr32(CMU_PERIC1 + CMU_GATE_PCLK4, rd32(CMU_PERIC1 + CMU_GATE_PCLK4) | CMU_GATE_BIT);
	wr32(CMU_PERIC1 + CMU_GATE_USI11, rd32(CMU_PERIC1 + CMU_GATE_USI11) | CMU_GATE_BIT);
	__asm__ volatile("dsb sy" ::: "memory");
	for (volatile int i = 0; i < 2000; i++)
		;
	out("CMU: mux %x div %x ip %x pclk %x usi %x\n", rd32(CMU_PERIC1 + CMU_MUX_USI11),
	    rd32(CMU_PERIC1 + CMU_DIV_USI11), rd32(CMU_PERIC1 + CMU_GATE_IPCLK4),
	    rd32(CMU_PERIC1 + CMU_GATE_PCLK4), rd32(CMU_PERIC1 + CMU_GATE_USI11));
}

/* gpp23-0/1/2 = SPI11 CLK/MOSI/MISO (pin-function 2); the bootloader leaves
 * them as GPIO input, so the signals never reach the pins. (spi11_bus in
 * gs101-pinctrl.dtsi.) gpp23-3 (CS) is driven separately by cs_set. */
static void spi_pinmux(void)
{
	uintptr_t con = PINCTRL_PERIC1 + GPP23_OFF;
	uint32_t c = rd32(con);
	c &= ~((0xfu << 0) | (0xfu << 4) | (0xfu << 8));
	c |= (2u << 0) | (2u << 4) | (2u << 8);
	wr32(con, c);
	__asm__ volatile("dsb sy" ::: "memory");
}

static void spi_hwinit(void)
{
	cmu_clk_enable();
	spi_pinmux();
	/* USI: select SPI function, clear USI reset, keep the clock requested. */
	uint32_t c = rd32(USI_SYSREG + USI_SW_CONF_OFF);
	wr32(USI_SYSREG + USI_SW_CONF_OFF, (c & ~0x7u) | USI_SPI_SW_CONF);
	sw(USI_CON, 0);			/* clear USI_CON_RESET (bit0) and all */
	sw(USI_OPTION, (sr(USI_OPTION) | USI_OPT_CLKREQ_ON) & ~USI_OPT_CLKSTOP_ON);
	__asm__ volatile("isb");
	out("USI: SW_CONF %x USI_CON %x OPTION %x\n", rd32(USI_SYSREG + USI_SW_CONF_OFF),
	    sr(USI_CON), sr(USI_OPTION));
	sw(CH_CFG, CH_SW_RST);
	for (volatile int i = 0; i < 1000; i++)
		;
	sw(CH_CFG, 0);			/* master, mode 0 (CPOL=CPHA=0), channels off */
	sw(SLAVE_SEL, SS_CS_AUTO | SS_NSC_CNT_2);  /* auto CS -> controller clocks */
	sw(INT_EN, 0);
	sw(MODE_CFG, 0);		/* byte transfers (BUS/CH TSZ=BYTE), no DMA */
	sw(SWAP_CFG, 0);		/* no byte/bit swap */
	sw(FB_CLK, 1);			/* feedback-clock delay 1 (DT controller-data) */
	sw(PACKET_CNT, 0);
	sw(PENDING_CLR, 0x1f);
	out("SPI: after hwinit CH_CFG %x SLAVE_SEL %x gpp23 CON %x (pin0-2 want func2)\n",
	    sr(CH_CFG), sr(SLAVE_SEL), rd32(PINCTRL_PERIC1 + GPP23_OFF));
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
	out("FTM5 raw rx: %x %x %x %x %x %x %x %x\n", rx[0], rx[1], rx[2], rx[3],
	    rx[4], rx[5], rx[6], rx[7]);
	/* data starts after cmd(5) + 1 dummy */
	for (uint32_t i = 0; i < toRead; i++)
		out_buf[i] = rx[5 + 1 + i];
	return 0;
}

static void udelay_(uint32_t us)
{
	uint64_t end = now() + (rsr("cntfrq_el0") / 1000000) * us;
	while (now() < end)
		;
}

/* Full FTM5 bring-up (vendor fts_chip_powercycle order): hold reset low, power
 * the digital LDO25 (DVDD 1.8V) then 1 ms later the analog LDO26 (AVDD 2.8V)
 * over ACPM, settle, then release reset and wait for the firmware. The
 * bootloader leaves both touch LDOs off (no always-on/boot-on) and the IC in
 * reset, so none of this is done for us. reset gpp26-0 is active low. */
static void ftm5_bringup(void)
{
	uintptr_t con = PINCTRL_PERIC1 + GPP26_OFF;
	uintptr_t dat = PINCTRL_PERIC1 + GPP26_OFF + 4;
	uint32_t c = rd32(con);
	c &= ~(0xfu << (RST_PIN * 4));
	c |= (1u << (RST_PIN * 4));	/* gpp26-0 output */
	wr32(con, c);
	wr32(dat, rd32(dat) & ~(1u << RST_PIN));	/* assert reset (low) */
	__asm__ volatile("dsb sy" ::: "memory");

	/* Power the touch LDOs via ACPM (S2MPG10 LDO25=DVDD, LDO26=AVDD). */
	acpm_init();
	uint8_t v = 0;
	acpm_pmic_read(0x5C, &v);	/* DVDD state before */
	acpm_pmic_read(0x5D, &v);	/* AVDD state before */
	acpm_pmic_update(0x5C, 0x2C, 0x3F);	/* DVDD 1.8V */
	acpm_pmic_update(0x5C, 0x80, 0x80);	/* DVDD enable */
	udelay_(1000);			/* digital before analog */
	acpm_pmic_update(0x5D, 0x28, 0x3F);	/* AVDD 2.8V */
	acpm_pmic_update(0x5D, 0x80, 0x80);	/* AVDD enable */
	udelay_(5000);			/* regulators settle */
	acpm_pmic_read(0x5C, &v);	/* DVDD state after */
	acpm_pmic_read(0x5D, &v);	/* AVDD state after */

	udelay_(10000);			/* hold reset 10 ms with power up */
	wr32(dat, rd32(dat) | (1u << RST_PIN));		/* release reset (high) */
	__asm__ volatile("dsb sy" ::: "memory");
	out("FTM5: reset gpp26 CON %x DAT %x; waiting 60 ms for fw\n", rd32(con), rd32(dat));
	udelay_(60000);			/* firmware boot */
}

/* FTS_CMD_HW_REG_W (0xFA) to 0x20000024 = 0x81: FTM5 system reset. */
static void ftm5_system_reset(void)
{
	uint8_t tx[6] = { 0xFA, 0x20, 0x00, 0x00, 0x24, 0x81 };
	uint8_t rx[6] = { 0 };
	cs_set(0);
	spi_xfer(tx, rx, 6);
	cs_set(1);
}

void spi_probe(void)
{
	out("SPI: CH_CFG %x STATUS %x USI_CON %x SW_CONF %x\n", sr(CH_CFG), sr(SPI_STATUS),
	    sr(USI_CON), rd32(USI_SYSREG + USI_SW_CONF_OFF));
	out("SPI: gpp23 CON %x DAT %x (GPP23_OFF %x)\n", rd32(PINCTRL_PERIC1 + GPP23_OFF),
	    rd32(PINCTRL_PERIC1 + GPP23_OFF + 4), (uint32_t)GPP23_OFF);
	spi_hwinit();

	/* Decisive self-test: internal loopback (TX->RX, no external pins).
	 * Isolates "controller/clock broken" from "external CS/FTM5 problem".
	 * If RX echoes TX, the controller + CMU clock work and the fault is
	 * off-chip; if RX stays empty, the SPI11 clock never reaches the core. */
	uint32_t mc = sr(MODE_CFG);
	sw(MODE_CFG, mc | MODE_SELF_LOOPBACK);
	uint8_t ltx[4] = { 0xA5, 0x5A, 0x3C, 0xC3 }, lrx[4] = { 0, 0, 0, 0 };
	int lr = spi_xfer(ltx, lrx, 4);
	out("LOOPBACK: r=%d rx %x %x %x %x STATUS %x (expect a5 5a 3c c3)\n", lr,
	    lrx[0], lrx[1], lrx[2], lrx[3], sr(SPI_STATUS));
	sw(MODE_CFG, mc);

	/* Power the FTM5 (ACPM LDOs) and release reset, then read the chip id.
	 * No soft system-reset needed: chip id is a fixed HW register, readable
	 * once the IC is powered and out of reset (vendor reads it standalone). */
	ftm5_bringup();
	(void)ftm5_system_reset;

	uint8_t id[2] = { 0, 0 };
	if (ftm5_hwreg_read(0x20000000, id, 2) == 0)
		out("FTM5: chip id %x %x (expect 36 48) %s\n", id[0], id[1],
		    (id[0] == 0x36 && id[1] == 0x48) ? "OK" : "??");
	else
		out("FTM5: chip id read failed\n");
}
