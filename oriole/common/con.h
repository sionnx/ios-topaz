/*
 * Output and board basics shared by the oriole probe and shim: text on the
 * panel (framebuffer at 0xfac00000, DECON kicked after every out()) and in
 * the ramoops console zone (Android: /sys/fs/pstore/console-ramoops-0),
 * watchdog stop, WDT0 reset.
 */
#pragma once
#include <stdint.h>

#define BG	0xff202060U
#define BG_ERR	0xff800000U

extern unsigned cur_el;
extern uint32_t wdt_regs[2][3];	/* WTCON/WTDAT/WTCNT as the bootloader left them */

/* Stop both watchdogs, point DPP0 at the framebuffer, start a fresh ramoops record. */
void board_init(void);
void fb_clear(uint32_t bg);
void display_kick(void);
void display_info(void);
void wdt_info(void);
/* %x (u32), %lx (u64, 16 digits), %d (u32 decimal), %s, %c */
void out(const char *fmt, ...);

uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t now(void);
void delay_ms(unsigned ms);
/* PSCI SYSTEM_RESET, then WDT0 (PSCI reset is NOT_SUPPORTED here). */
void __attribute__((noreturn)) reset_after(unsigned s);
