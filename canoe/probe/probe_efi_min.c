#include <stdint.h>
struct TH { uint64_t sig; uint32_t rev, hs, crc, rsv; };
struct RS { struct TH h; void *a,*b,*c,*d,*e,*f,*g,*i,*j,*k;
	void (*ResetSystem)(uint32_t, uint64_t, uint64_t, void*); };
struct ST { struct TH h; uint16_t *fv; uint32_t frev; void *cih, *cin, *coh, *cout,*seh,*se;
	struct RS *rs; void *bs; };
uint64_t efi_main(void *image, struct ST *st)
{
	st->rs->ResetSystem(1, 0, 0, 0);   /* EfiResetWarm */
	for (;;) __asm__ volatile("wfi");
	return 0;
}
