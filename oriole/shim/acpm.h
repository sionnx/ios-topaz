/*
 * Minimal ACPM IPC client for the gs101 APM coprocessor: enough to read and
 * update S2MPG10 PMIC registers (to power the touch LDOs) from bare metal.
 * See oriole/TOUCH.md and NOTES.md ORIOLE for the protocol.
 */
#ifndef ORIOLE_ACPM_H
#define ORIOLE_ACPM_H
#include <stdint.h>

/* Parse the live ACPM framework in APM SRAM and print the ch2 ring pointers. */
void acpm_init(void);
/* S2MPG10 main-PMIC register read / masked update over ACPM. Return 0 on ok. */
int acpm_pmic_read(uint8_t reg, uint8_t *val);
int acpm_pmic_update(uint8_t reg, uint8_t val, uint8_t mask);

#endif
