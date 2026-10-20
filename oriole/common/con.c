/*
 * Output and board basics shared by the oriole probe and shim (con.h).
 */
#include <stdarg.h>
#include "con.h"
#include "gs101.h"
#include "../../shim/font.h"

unsigned cur_el;

/* ---- output: framebuffer text + ramoops console ---- */

#define SCALE	2
#define CH_W	(8 * SCALE)
#define CH_H	(16 * SCALE)
#define MARGIN	24
#define COLS	((FB_W - 2 * MARGIN) / CH_W)
#define ROWS	((FB_H - 2 * MARGIN) / CH_H)
#define FG	0xffffffffU

void display_kick(void)
{
	wr32(DECON0 + DECON_TRIG_CON, DECON_TRIG_HW);
	wr32(DECON0 + DECON_SHD_REG_UP_REQ, DECON_SHD_UP_ALL);
}

static uint32_t con_bg = BG;
static int con_row, con_col;

void fb_clear(uint32_t bg)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;
	for (int i = 0; i < FB_W * FB_H; i++)
		fb[i] = bg;
	con_bg = bg;
	con_row = con_col = 0;
}

static void fb_char(char ch)
{
	if (ch == '\n' || con_col >= COLS) {
		con_row++;
		con_col = 0;
		if (ch == '\n')
			return;
	}
	if (con_row >= ROWS)
		fb_clear(con_bg);	/* full: start over at the top */
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + con_col * CH_W, y0 = MARGIN + con_row * CH_H;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? FG : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = (volatile uint32_t *)FB_BASE +
					(y0 + gy * SCALE + sy) * (FB_STRIDE / 4) + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
	con_col++;
}

/*
 * persistent_ram_buffer { u32 sig; u32 start; u32 size; u8 data[]; }, used as
 * Linux does: start = next write offset, size = valid bytes (up to the
 * capacity once wrapped), so pstore shows the latest RAMOOPS_CONSOLE bytes.
 */
#define PRZ_CAP		(RAMOOPS_CONSOLE - 12)
static uint32_t prz_start, prz_size;

static void prz_init(void)
{
	wr32(RAMOOPS_BASE + 0, PRZ_SIG);
	wr32(RAMOOPS_BASE + 4, 0);
	wr32(RAMOOPS_BASE + 8, 0);
	prz_start = prz_size = 0;
}

void prz_putc(char ch)
{
	*(volatile uint8_t *)(RAMOOPS_BASE + 12 + prz_start) = (uint8_t)ch;
	prz_start = prz_start + 1 == PRZ_CAP ? 0 : prz_start + 1;
	if (prz_size < PRZ_CAP)
		prz_size++;
	wr32(RAMOOPS_BASE + 8, prz_size);
	wr32(RAMOOPS_BASE + 4, prz_start);
}

int con_screen = 1;

static void out_char(char ch)
{
	if (con_screen)
		fb_char(ch);
	prz_putc(ch);
}

static void out_hex(uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--)
		out_char("0123456789abcdef"[(v >> (4 * i)) & 0xf]);
}

void out(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	for (const char *p = fmt; *p; p++) {
		if (*p != '%') {
			out_char(*p);
			continue;
		}
		p++;
		if (*p == 'l' && p[1] == 'x') {
			p++;
			out_hex(va_arg(ap, uint64_t), 16);
		} else if (*p == 'x') {
			out_hex(va_arg(ap, uint32_t), 8);
		} else if (*p == 'd') {
			char buf[12];
			int n = 0;
			uint32_t v = va_arg(ap, uint32_t);
			do
				buf[n++] = '0' + v % 10;
			while ((v /= 10) && n < 11);
			while (n)
				out_char(buf[--n]);
		} else if (*p == 's') {
			for (const char *s = va_arg(ap, const char *); *s; s++)
				out_char(*s);
		} else if (*p == 'c') {
			out_char((char)va_arg(ap, int));
		} else {
			out_char('%');
			if (!*p)
				break;
			out_char(*p);
		}
	}
	va_end(ap);
	if (con_screen)
		display_kick();
}

/* ---- helpers ---- */

uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

uint64_t now(void)
{
	__asm__ volatile("isb");
	return rsr("cntpct_el0");
}

void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

/* WTCON/WTDAT/WTCNT of both watchdogs as the bootloader left them. */
uint32_t wdt_regs[2][3];

/*
 * PSCI SYSTEM_RESET is NOT_SUPPORTED on this firmware (probe 2). Fall back to
 * WDT0: the bootloader runs it with reset enabled (WTCON 0x1af39), so restore
 * that WTCON with a tiny count. The ramoops console zone survives this kind
 * of reset (as it survived Power + Volume up).
 */
void __attribute__((noreturn)) reset_after(unsigned s)
{
	out("RESET in %d s (PSCI SYSTEM_RESET, then WDT0)\n", s);
	delay_ms(s * 1000);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	uint32_t con = wdt_regs[0][0];
	if (!(con & (1U << 5)) || !(con & 1U))
		con = 0x1af39;
	out("PSCI reset returned; WDT0 con %x cnt 0x100\n", con);
	wr32(WDT_CL0, 0);
	wr32(WDT_CL0 + 4, 0x100);
	wr32(WDT_CL0 + 8, 0x100);
	wr32(WDT_CL0, con);
	__asm__ volatile("dsb sy" ::: "memory");
	delay_ms(5000);
	out("still alive 5 s after WDT0: cnt %x con %x\n", rd32(WDT_CL0 + 8), rd32(WDT_CL0));
	for (;;)
		__asm__ volatile("wfe");
}

/*
 * Exynos watchdogs: the oriole U-Boot port clears WTCON first thing. Stop
 * them before anything else and report the values the bootloader left.
 */
static void wdt_stop(void)
{
	for (int i = 0; i < 2; i++) {
		uintptr_t b = i ? WDT_CL1 : WDT_CL0;
		wdt_regs[i][0] = rd32(b);
		wdt_regs[i][1] = rd32(b + 4);
		wdt_regs[i][2] = rd32(b + 8);
		wr32(b, 0);
	}
}

void wdt_info(void)
{
	for (int i = 0; i < 2; i++)
		out("WDT%d was con %x dat %x cnt %x, now con %x\n", i, wdt_regs[i][0],
		    wdt_regs[i][1], wdt_regs[i][2], rd32(i ? WDT_CL1 : WDT_CL0));
}

/*
 * Under `fastboot boot` the panel still scans out the fastboot menu buffer.
 * Point DPP0's DMA at 0xfac00000 and latch it, as the oriole U-Boot port
 * does (oriole_take_over_display): RDMA_SRC_SIZE/IMG_SIZE, BASEADDR_Y8,
 * then DECON0 TRIG_CON + SHD_REG_UP_REQ. Pixel format stays as the
 * bootloader set it (RDMA_IN_CTRL_0 bits 13:8, reported).
 */
static uint32_t dpp_base[6], dpp_ctrl[6], dpp_size[6];

static void display_take_over(void)
{
	for (int ch = 0; ch < 6; ch++) {
		dpp_base[ch] = rd32(DPP_DMA(ch) + RDMA_BASEADDR_Y8);
		dpp_ctrl[ch] = rd32(DPP_DMA(ch) + RDMA_IN_CTRL_0);
		dpp_size[ch] = rd32(DPP_DMA(ch) + RDMA_IMG_SIZE);
	}
	uint32_t size = (FB_H << 16) | FB_W;
	wr32(DPP_DMA(0) + RDMA_SRC_SIZE, size);
	wr32(DPP_DMA(0) + RDMA_IMG_SIZE, size);
	wr32(DPP_DMA(0) + RDMA_BASEADDR_Y8, (uint32_t)FB_BASE);
	display_kick();
}

void display_info(void)
{
	for (int ch = 0; ch < 6; ch++)
		out("DPP%d was base %x ctrl %x size %x\n", ch, dpp_base[ch], dpp_ctrl[ch], dpp_size[ch]);
}

void board_init(void)
{
	cur_el = rsr("CurrentEL") >> 2;
	wdt_stop();
	display_take_over();
	prz_init();
	fb_clear(BG);
	display_kick();
}
