# Pixel 6 (oriole/gs101) touch driver spec — ST FTM5 on Exynos USI-SPI

Hardware facts (from the live Pixel 6 device tree, `pixel6-backup-20261020/live-oriole.dts.txt`)
and the vendor source `android-gs-raviole-5.10-android13-qpr3`
(`drivers/spi/spi-s3c64xx.c`; `google-modules/touch/fts_touch/ftm5/`). Record the branch's
commit hash when the driver lands, since the vendor branch moves.

## Addresses
- **SPI controller**: `samsung,exynos-spi` @ **0x10D40000** (4 KiB), FIFO 64, spi-max 10 MHz,
  mode 0 (CPOL=0,CPHA=0), interrupts SPI 0x28e. `clk_from_cmu=true` (clock from CMU, bootloader
  left it clocked — take over the running controller like UFS/USB).
- **USI SW_CONF syscon** (select SPI function): `/syscon@10C21000` @ **0x10C21000**, write offset
  **0xC** bits[2:0] = **0x2** (USI_SPI_SW_CONF; mask 0x7). Then clear USI reset: write SPI+0xC4
  (USI_CON) = 0.
- **CS GPIO** = **gpp23 pin 3** on `google,gs101-pinctrl` @ **0x10C40000** (active config
  `spi11-cs`/`ts_spi_cs_active` use pin-function 1 = GPIO output, software-driven, active low).
  gpp23 bank offset = **0x60** within 0x10C40000 (gs101_pin_peric1[] `GS101_PIN_BANK_EINTG(8,
  0x60, "gpp23", ...)` in mainline `pinctrl-exynos-arm64.c`; bank type exynos850_bank_type_off:
  CON = bank+0x00 4 bits/pin, DAT = bank+0x04 1 bit/pin). Absolute CON = **0x10C40060**,
  DAT = **0x10C40064**. To drive CS: CON pin3 field = 1 (output); DAT bit3 = 0 assert / 1 deassert.
- **IRQ GPIO** (st,irq-gpio): gpa9 on pinctrl@174D0000 (ts-irq) — not needed for polling.
- **Reset GPIO** (st,reset-gpio): ts-reset on pinctrl@10C40000 — bootloader left FTM5 powered;
  a HW system reset over SPI (below) is enough, GPIO reset optional.
- vdd/avdd regulators: left on by the bootloader.
- **Touch coords**: max 0x437 x 0x95f = **1079 x 2399 = the 1080x2400 panel pixels**, so pass
  straight to `sweet_hid_touch(finger, down, x, y)` (SweetHID.cpp maps 0-1079/0-2399 to the
  presented 1125x2436 display; no scaling).

## CMU_PERIC1 clock — REQUIRED, the bootloader does NOT clock spi11
SPI_PROBE run 1 (2026-10-21) proved the bootloader leaves spi11 unclocked (TX
FIFO filled, nothing clocked out, STATUS 0x01000200 TX_LVL=8 RX_LVL=0). Android's
touch driver clocks it; we must too. From mainline `clk-gs101.c` (DT ties spi11
to IPCLK_4 + PCLK_4, all gs101 PERIC1 gates use **bit 21**):
- CMU_PERIC1 base = **0x10C00000**.
- MUX usi11 @ **0x10C00640** bit4: 0 = oscclk (24 MHz, no PLL dep), 1 = dout_cmu_peric1_ip.
- DIV usi11 @ **0x10C0180C** bits[3:0] = divider-1. `clk_from_cmu=true`, so SPI
  rate = this divider, NOT a controller prescaler. Probe uses oscclk /4 = 6 MHz.
- ipclk gate @ **0x10C0202C** bit21; pclk gate @ **0x10C0204C** bit21;
  usi11 reset-sync clk @ **0x10C02068** bit21 (belt-and-braces).
- Then USI: SW_CONF @ 0x10C2100C = (v&~7)|2 (SPI); USI_CON (SPI+0xC4) &= ~bit0
  (clear reset); USI_OPTION (SPI+0xC8) |= CLKREQ_ON(bit1), &= ~CLKSTOP_ON(bit2).
Sequence: mux→div→gates→SW_CONF→USI_CON→USI_OPTION, then drive SPI. (exynos-usi.c
exynos_usi_enable/configure.)

## Exynos SPI registers (offset from 0x10D40000)
`CH_CFG 0x00, CLK_CFG 0x04, MODE_CFG 0x08, SLAVE_SEL 0x0C, INT_EN 0x10, STATUS 0x14,
TX_DATA 0x18, RX_DATA 0x1C, PACKET_CNT 0x20, PENDING_CLR 0x24, USI_CON 0xC4, USI_OPTION 0xC8`
- CH_CFG: HS_EN=bit6, SW_RST=bit5, SLAVE=bit4 (master = clear), CPOL_L=bit3, CPHA_B=bit2,
  RXCH_ON=bit1, TXCH_ON=bit0.
- STATUS: RX_FIFO_LVL = (STATUS>>15)&0x7F, TX_FIFO_LVL = (STATUS>>6)&0x7F, TX_DONE = bit25.
- PACKET_CNT: EN=bit16, count = bits[15:0] (= len*8/bpw).
- SLAVE_SEL: SIG_INACT=bit0 (set in hwinit).
- MODE_CFG: 8-bit transfers → BUS_TSZ (bits18:17) and CH_TSZ (bits30:29) both BYTE(0).

### hwinit (polled): SLAVE_SEL=SIG_INACT(1); INT_EN=0; MODE_CFG=0; PACKET_CNT=0;
### PENDING_CLR = clear all overrun/underrun bits. (clk_from_cmu → don't touch CLK_CFG.)
### Polled full-duplex transfer of `len` bytes (dma off):
1. chcfg = CH_CFG & ~TXCH_ON | RXCH_ON (RX on even for pure TX, to clock exactly).
2. PACKET_CNT: clear EN; write count=len; set EN(bit16).
3. MODE_CFG: clear 4BURST/TX/RXDMA.
4. if TX: chcfg |= TXCH_ON; push all TX bytes to TX_DATA (0x18).
5. write MODE_CFG; write chcfg to CH_CFG (starts the transfer).
6. poll until ((STATUS>>15)&0x7F) >= len.
7. read len bytes from RX_DATA (0x1C).
CS held LOW (gpp23-3) across the whole command+data sequence. USI mux must be set first.

## ST FTM5 SPI protocol
Commands: `FTS_CMD_HW_REG_R=0xFB`, `FTS_CMD_HW_REG_W=0xFA`, `FIFO_CMD_READALL=0x87`.
SPI dummy = 1 (skip the first returned byte). HW-reg address = 4 bytes, big-endian.
- **Read chip id** (should be 0x36,0x48): TX `0xFB 0x20 0x00 0x00 0x00` (0xFB + addr 0x20000000
  MSB-first), then clock out 3 bytes, drop the 1st dummy → id[0]=0x36 id[1]=0x48.
- **System reset** before events: write `0xFA 0x20 0x00 0x00 0x24 0x81` (HW_REG_W to
  0x20000024 = 0x81), then poll for an event 0x03 (CONTROLLER_READY).
- **Event FIFO**: TX `0x87`, read 1 dummy + 8 bytes = one event. First event's byte[7]&0x1F =
  events remaining (cap 31); drain the rest with more `0x87` reads of 8 bytes each.
  Bare-metal polling: periodically send `0x87`, read 8 bytes; evt[0]!=0x00 → an event.
- **Event IDs** (evt[0]): 0x00 none, 0x03 controller-ready, 0x13 ENTER, 0x23 MOTION, 0x33 LEAVE.
- **Touch event 8-byte layout**:
  - evt[1]: [7:4]=touchId, [3:0]=touchType (1=finger).
  - evt[2]: X[7:0]. evt[3]: [3:0]=X[11:8], [7:4]=Y[3:0]. evt[4]: Y[11:4].
  - `x = ((evt[3]&0x0F)<<8) | evt[2]`; `y = (evt[4]<<4) | ((evt[3]&0xF0)>>4)`.
  - ENTER/MOTION → `sweet_hid_touch(touchId, 1, x, y)`; LEAVE → `sweet_hid_touch(touchId, 0, x, y)`.

## Plan
1. Shim read-only probe (bare metal): set USI SW_CONF, take over SPI11, drive gpp23-3 CS, read
   FTM5 chip id (expect 0x36 0x48). Validates the SPI/USI/GPIO programming cheaply (no CI).
2. Kernel driver `bsd/dev/oriole_touch.c`: same SPI takeover, system reset, poll thread reading
   the event FIFO -> sweet_hid_touch. Gated to oriole (called from sweet_ufs_init's non-topaz
   branch, like the USB probe). topaz's sweet_touch.c (FocalTech/I2C) stays topaz-only.

Sources: spi-s3c64xx.c and ftm5/ in android-gs-raviole-5.10-android13-qpr3 (per the research pass).
