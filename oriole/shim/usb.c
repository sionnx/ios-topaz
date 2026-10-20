/*
 * Read-only DWC3 probe for oriole: the USB controller state the stock
 * bootloader leaves after `fastboot boot` (its fastboot gadget ran on it),
 * to decide how topaz's xnu gadget (bsd/dev/sweet_usb.c, CDC-ECM live log)
 * can take it over. DWC3 core at 0x11110000 (live DT/iomem: dwc3 globals
 * 0x1111c100); only registers are read.
 */
#include <stdint.h>
#include "../common/con.h"
#include "../common/gs101.h"
#include "usb.h"

#define DWC3		0x11110000UL

static uint32_t d(uint32_t off) { return rd32(DWC3 + off); }

void usb_probe(void)
{
	out("USB: GSNPSID %x GCTL %x GSTS %x GUSB2PHYCFG %x GUSB3PIPECTL %x\n", d(0xc120), d(0xc110),
	    d(0xc118), d(0xc200), d(0xc2c0));
	out("USB: DCFG %x DCTL %x DEVTEN %x DSTS %x DALEPENA %x\n", d(0xc700), d(0xc704), d(0xc708),
	    d(0xc70c), d(0xc720));
	out("USB: GEVNTADR %x%x GEVNTSIZ %x GEVNTCOUNT %x GHWPARAMS0 %x 1 %x 3 %x\n", d(0xc404),
	    d(0xc400), d(0xc408), d(0xc40c), d(0xc140), d(0xc144), d(0xc14c));
	out("USB: GUCTL %x GUCTL1 %x GSBUSCFG0 %x GTXTHRCFG %x\n", d(0xc12c), d(0xc11c), d(0xc100),
	    d(0xc108));
}
