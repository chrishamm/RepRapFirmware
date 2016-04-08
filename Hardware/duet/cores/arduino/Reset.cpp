/*
  Copyright (c) 2012 Arduino.  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <Arduino.h>
#include "Reset.h"

#include "flash_efc.h"
#include "wdt.h"

#ifdef __cplusplus
extern "C" {
#endif

__attribute__ ((long_call, section (".ramfunc")))
void banzai() {
	// Disable all interrupts
	__disable_irq();

	// Set bootflag to run SAM-BA bootloader at restart
	const int EEFC_FCMD_CGPB = 0x0C;
	const int EEFC_KEY = 0x5A;
	while ((EFC0->EEFC_FSR & EEFC_FSR_FRDY) == 0);
	EFC0->EEFC_FCR =
		EEFC_FCR_FCMD(EEFC_FCMD_CGPB) |
		EEFC_FCR_FARG(1) |
		EEFC_FCR_FKEY(EEFC_KEY);
	while ((EFC0->EEFC_FSR & EEFC_FSR_FRDY) == 0);

	// From here flash memory is no more available.

	// BANZAIIIIIII!!!
	const int RSTC_KEY = 0xA5;
	RSTC->RSTC_CR =
		RSTC_CR_KEY(RSTC_KEY) |
		RSTC_CR_PROCRST |
		RSTC_CR_PERRST;

	while (true);
}

static int ticks = -1;

void initiateReset(int _ticks) {
	ticks = _ticks;
}

void cancelReset() {
	ticks = -1;
}

void tickReset() {
	if (ticks == -1)
		return;
	ticks--;
	if (ticks == 0)
		banzai();
}

void Reset()
{
	rstc_start_software_reset(RSTC);
	for(;;) {}
}

// Switch into boot mode and reset
void EraseAndReset()
{
	cpu_irq_disable();

#if SAM3XA_SERIES
# define IFLASH_ADDR				IFLASH0_ADDR
# define IFLASH_PAGE_SIZE			IFLASH0_PAGE_SIZE
# define IFLASH_NB_OF_PAGES			((IFLASH1_ADDR + IFLASH1_SIZE - IFLASH_ADDR) / IFLASH_PAGE_SIZE)
#endif

	for(size_t i = 0; i <= IFLASH_NB_OF_PAGES; i++)
	{
		WDT_Restart(WDT);
		size_t pageStartAddr = IFLASH_ADDR + i * IFLASH_PAGE_SIZE;
		flash_unlock(pageStartAddr, pageStartAddr + IFLASH_PAGE_SIZE - 1, nullptr, nullptr);
	}

	flash_clear_gpnvm(1);			// tell the system to boot from ROM next time
	rstc_start_software_reset(RSTC);
	for(;;) {}
}

#ifdef __cplusplus
}
#endif
