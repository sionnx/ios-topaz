#include <stdint.h>
uint64_t efi_main(void *image, void *st)
{
	(void)image; (void)st;
	register uint64_t x0 __asm__("x0") = 0x84000009;  /* PSCI SYSTEM_RESET */
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3) :: "memory");
	for (;;) __asm__ volatile("wfi");
	return 0;
}
