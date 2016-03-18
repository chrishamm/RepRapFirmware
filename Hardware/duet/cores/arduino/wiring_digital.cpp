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

#include "Arduino.h"

#ifdef __cplusplus
extern "C" {
#endif

extern "C" void pinModeDuet(uint32_t ulPin, uint32_t ulMode, uint32_t debounceCutoff)
{
	if (ulPin > MaxPinNumber)
	{
		return;
	}

	const PinDescription& pinDesc = g_APinDescription[ulPin];
    if (pinDesc.ulPinType == PIO_NOT_A_PIN)
    {
        return;
    }

	switch (ulMode)
	{
		case INPUT:
			/* Enable peripheral for clocking input */
			pmc_enable_periph_clk(pinDesc.ulPeripheralId) ;
			PIO_Configure(
					pinDesc.pPort,
					PIO_INPUT,
					pinDesc.ulPin,
					(debounceCutoff == 0) ? 0 : PIO_DEBOUNCE );
			if (debounceCutoff != 0)
			{
				PIO_SetDebounceFilter(pinDesc.pPort, pinDesc.ulPin, debounceCutoff);	// enable debounce filer with specified cutoff frequency
			}
			break ;

		case INPUT_PULLUP:
			/* Enable peripheral for clocking input */
			pmc_enable_periph_clk(pinDesc.ulPeripheralId) ;
			PIO_Configure(
					pinDesc.pPort,
					PIO_INPUT,
					pinDesc.ulPin,
					(debounceCutoff == 0) ? PIO_PULLUP : PIO_PULLUP | PIO_DEBOUNCE );
			if (debounceCutoff != 0)
			{
				PIO_SetDebounceFilter(pinDesc.pPort, pinDesc.ulPin, debounceCutoff);	// enable debounce filer with specified cutoff frequency
			}
			break ;

		case OUTPUT:
			PIO_Configure(
					pinDesc.pPort,
					PIO_OUTPUT_1,
					pinDesc.ulPin,
					pinDesc.ulPinConfiguration ) ;

			/* if all pins are output, disable PIO Controller clocking, reduce power consumption */
			if (pinDesc.pPort->PIO_OSR == 0xffffffff)
			{
				pmc_disable_periph_clk(pinDesc.ulPeripheralId) ;
			}
			break ;

		default:
			break ;
	}
}

// This has now been optimised to speed it up, so it may no longer be used to enable the pullup resistor on an input pin (use mode INPUT_PULLUP instead).
extern "C"  void digitalWrite(uint32_t ulPin, uint32_t ulVal)
{
	if (ulPin <= MaxPinNumber)
	{
		const PinDescription& pinDesc = g_APinDescription[ulPin];
		if (pinDesc.ulPinType != PIO_NOT_A_PIN)
		{
			if (ulVal)		// we make use of the fact that LOW is zero and HIGH is nonzero
			{
				pinDesc.pPort->PIO_SODR = pinDesc.ulPin;
			}
			else
			{
				pinDesc.pPort->PIO_CODR = pinDesc.ulPin;
			}
		}
	}
}

extern "C" int digitalRead(uint32_t ulPin)
{
	if (ulPin > MaxPinNumber)
	{
		return LOW;
	}

	const PinDescription& pinDesc = g_APinDescription[ulPin];
	if (pinDesc.ulPinType == PIO_NOT_A_PIN)
    {
        return LOW ;
    }

	return PIO_Get(pinDesc.pPort, PIO_INPUT, pinDesc.ulPin);
}

extern "C" void setPullup(uint32_t ulPin, bool en)
{
	if (ulPin <= MaxPinNumber)
	{
		const PinDescription& pinDesc = g_APinDescription[ulPin];
		if (pinDesc.ulPinType != PIO_NOT_A_PIN)
		{
			PIO_PullUp(g_APinDescription[ulPin].pPort, g_APinDescription[ulPin].ulPin, (uint32_t)en) ;
		}
	}
}


#ifdef __cplusplus
}
#endif

