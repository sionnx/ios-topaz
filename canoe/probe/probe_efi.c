/*
 * canoe (OnePlus 15, SM8850) UEFI probe. The phone's bootloader is UEFI:
 * `fastboot boot` of a raw arm64 Image fails with "No EFI image found in
 * boot.img", so this is a minimal EFI application instead. It runs at EL1
 * under the firmware, prints to the UEFI console (visible on screen) and to
 * the ramoops console zone (readable from Android via /sys/fs/pstore after
 * the warm reset), then resets. It answers the port-deciding questions:
 *   - ID_AA64MMFR0_EL1 TGran16: do the Oryon cores have the 16K granule the
 *     iOS userland needs?
 *   - entry exception level, MIDR, LSE/RCpc/PAN/PARange
 *   - CNTFRQ, SCTLR/TCR as the firmware set them
 * Nothing is written to any partition. Build: canoe/probe/build-efi.sh.
 */
#include <stdint.h>

typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;

struct EFI_TABLE_HEADER {
	uint64_t Signature;
	uint32_t Revision, HeaderSize, CRC32, Reserved;
};

struct SIMPLE_TEXT_OUTPUT {
	void *Reset;
	EFI_STATUS(*OutputString)(struct SIMPLE_TEXT_OUTPUT *, CHAR16 *);
	/* rest unused */
};

struct RUNTIME_SERVICES {
	struct EFI_TABLE_HEADER Hdr;
	void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
	void *SetVirtualAddressMap, *ConvertPointer;
	void *GetVariable, *GetNextVariableName, *SetVariable;
	void *GetNextHighMonotonicCount;
	void (*ResetSystem)(uint32_t Type, EFI_STATUS Status, UINTN DataSize, void *Data);
};

struct EFI_SYSTEM_TABLE {
	struct EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	uint32_t FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	struct SIMPLE_TEXT_OUTPUT *ConOut;
	EFI_HANDLE StandardErrorHandle;
	struct SIMPLE_TEXT_OUTPUT *StdErr;
	struct RUNTIME_SERVICES *RuntimeServices;
	void *BootServices;
};

#define EFI_RESET_WARM	1

#define RAMOOPS_BASE	0xbffdbf000ULL	/* ramoops-region; record_size 0 so console is first */
#define RAMOOPS_CONSOLE	0x40000ULL
#define PRZ_SIG		0x43474244U	/* "DBGC" */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
static inline void wr32(uint64_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static struct SIMPLE_TEXT_OUTPUT *conout;
static uint32_t prz_len;

static void prz_putc(char c)
{
	if (prz_len >= RAMOOPS_CONSOLE - 12)
		return;
	*(volatile uint8_t *)(RAMOOPS_BASE + 12 + prz_len) = (uint8_t)c;
	prz_len++;
	wr32(RAMOOPS_BASE + 8, prz_len);
	wr32(RAMOOPS_BASE + 4, prz_len);
}

static void putc(char c)
{
	CHAR16 s[3];
	int n = 0;
	if (c == '\n')
		s[n++] = '\r';
	s[n++] = (CHAR16)c;
	s[n] = 0;
	if (conout)
		conout->OutputString(conout, s);
	prz_putc(c);
}

static void puts(const char *s)
{
	for (; *s; s++)
		putc(*s);
}

static void puthex(uint64_t v, int digits)
{
	for (int i = digits - 1; i >= 0; i--)
		putc("0123456789abcdef"[(v >> (4 * i)) & 0xf]);
}

static void line(const char *label, uint64_t v)
{
	puts(label);
	puts(" 0x");
	puthex(v, 16);
	putc('\n');
}

EFI_STATUS efi_main(EFI_HANDLE image, struct EFI_SYSTEM_TABLE *st)
{
	conout = st->ConOut;

	/* fresh ramoops console record */
	wr32(RAMOOPS_BASE + 0, PRZ_SIG);
	wr32(RAMOOPS_BASE + 4, 0);
	wr32(RAMOOPS_BASE + 8, 0);
	prz_len = 0;

	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1");
	uint64_t isar0 = rsr("id_aa64isar0_el1");
	uint64_t isar1 = rsr("id_aa64isar1_el1");
	uint64_t mmfr1 = rsr("id_aa64mmfr1_el1");

	puts("\nCANOE EFI PROBE 2  (ios-topaz, OnePlus 15 / SM8850)\n");
	puts("*** TGran16 (16K granule) = 0x");
	puthex((mmfr0 >> 20) & 0xf, 1);
	puts("  0=NO >=1=YES ***\n");
	puts("CurrentEL EL");
	puthex((rsr("CurrentEL") >> 2) & 0xf, 1);
	putc('\n');
	line("MIDR_EL1   ", rsr("midr_el1"));
	line("MPIDR_EL1  ", rsr("mpidr_el1"));
	line("ID_MMFR0   ", mmfr0);
	puts("  TGran16=0x");
	puthex((mmfr0 >> 20) & 0xf, 1);      /* >=1: 16K supported, 0: not */
	puts(" TGran16_2=0x");
	puthex((mmfr0 >> 32) & 0xf, 1);
	puts(" TGran4=0x");
	puthex((mmfr0 >> 28) & 0xf, 1);
	puts(" PARange=0x");
	puthex(mmfr0 & 0xf, 1);
	putc('\n');
	puts("  LSE=0x");
	puthex((isar0 >> 20) & 0xf, 1);
	puts(" RCpc=0x");
	puthex((isar1 >> 20) & 0xf, 1);
	puts(" PAN=0x");
	puthex((mmfr1 >> 20) & 0xf, 1);
	putc('\n');
	line("ID_ISAR0   ", isar0);
	line("ID_PFR0    ", rsr("id_aa64pfr0_el1"));
	line("CNTFRQ_EL0 ", rsr("cntfrq_el0"));
	line("CTR_EL0    ", rsr("ctr_el0"));
	line("SCTLR_EL1  ", rsr("sctlr_el1"));
	line("TCR_EL1    ", rsr("tcr_el1"));
	line("MAIR_EL1   ", rsr("mair_el1"));
	line("TTBR0_EL1  ", rsr("ttbr0_el1"));
	line("TTBR1_EL1  ", rsr("ttbr1_el1"));
	line("VBAR_EL1   ", rsr("vbar_el1"));
	puts("FirmwareRevision 0x");
	puthex(st->FirmwareRevision, 8);
	putc('\n');

	/* The decisive answer again, big and last so it is never scrolled off. */
	puts("\n>>> 16K GRANULE (TGran16): 0x");
	puthex((mmfr0 >> 20) & 0xf, 1);
	puts("  (0=NO, >=1=YES)\n");
	puts(">>> PROBE DONE - screen frozen, photograph it, then hold Power+VolUp ~30s\n");
	for (;;)
		__asm__ volatile("wfi");
	return 0;
}
