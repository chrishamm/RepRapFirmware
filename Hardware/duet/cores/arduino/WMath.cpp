/*
  Copyright (c) 2011 Arduino.  All right reserved.

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

extern "C" {
#include "stdlib.h"
#include "stdint.h"
#include "trng.h"
}
#include "WMath.h"

extern uint32_t trueRandom()
{
	while (! (TRNG->TRNG_ISR & TRNG_ISR_DATRDY)) {}
	return (uint32_t)TRNG->TRNG_ODATA;
}

extern int32_t random(int32_t howbig)
{
	if (howbig <= 0)
	{
		return 0;
	}

	return trueRandom() % (uint32_t)howbig;
}

extern int32_t random(int32_t howsmall, int32_t howbig)
{
	if (howsmall >= howbig)
	{
		return howsmall;
	}

	return random(howbig - howsmall) + howsmall;
}

extern int32_t map(int32_t x, int32_t in_min, int32_t in_max, int32_t out_min, int32_t out_max)
{
	return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

extern uint16_t makeWord( uint16_t w )
{
	return w ;
}

extern uint16_t makeWord( uint8_t h, uint8_t l )
{
	return (h << 8) | l ;
}
