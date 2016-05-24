/*
 * conf_sd_mmc.h
 *
 *  Created on: 29 Mar 2016
 *      Author: David
 */

#ifndef CONF_SD_MMC_H_
#define CONF_SD_MMC_H_

#define CONF_BOARD_SD_MMC_HSMCI		1
#define SD_MMC_HSMCI_MEM_CNT        1
#define SD_MMC_HSMCI_SLOT_0_SIZE    4

#define SD_MMC_0_CD_GPIO            13		// Arduino digital pin 13
#define SD_MMC_0_CD_DETECT_VALUE    0

inline uint32_t sysclk_get_cpu_hz()
{
	return SystemCoreClock;
}

extern void delay(uint32_t ms);				// in core
inline void delay_ms(uint32_t ms)
{
	delay(ms);
}

#endif /* CONF_SD_MMC_H_ */
