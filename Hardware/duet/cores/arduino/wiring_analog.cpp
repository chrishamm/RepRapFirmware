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

static int _readResolution = 10;
static int _writeResolution = 8;

extern "C" void analogReadResolution(int res) {
	_readResolution = res;
}

extern "C" void analogWriteResolution(int res) {
	_writeResolution = res;
}

static inline uint32_t mapResolution(uint32_t value, uint32_t from, uint32_t to) {
	if (from == to)
		return value;
	if (from > to)
		return value >> (from-to);
	else
		return value << (to-from);
}

eAnalogReference analog_reference = AR_DEFAULT;

extern "C" void analogReference(eAnalogReference ulMode)
{
	analog_reference = ulMode;
}

extern "C" uint32_t analogRead(uint32_t ulPin)
{
  uint32_t ulValue = 0;
  uint32_t ulChannel;

  if (ulPin < A0)
    ulPin += A0;

  ulChannel = g_APinDescription[ulPin].ulADCChannelNumber ;

#if defined __SAM3U4E__
	switch ( g_APinDescription[ulPin].ulAnalogChannel )
	{
		// Handling ADC 10 bits channels
		case ADC0 :
		case ADC1 :
		case ADC2 :
		case ADC3 :
		case ADC4 :
		case ADC5 :
		case ADC6 :
		case ADC7 :
			// Enable the corresponding channel
			adc_enable_channel( ADC, ulChannel );

			// Start the ADC
			adc_start( ADC );

			// Wait for end of conversion
			while ((adc_get_status(ADC) & ADC_SR_DRDY) != ADC_SR_DRDY)
				;

			// Read the value
			ulValue = adc_get_latest_value(ADC);
			ulValue = mapResolution(ulValue, 10, _readResolution);

			// Disable the corresponding channel
			adc_disable_channel( ADC, ulChannel );

			// Stop the ADC
			//      adc_stop( ADC ) ; // never do adc_stop() else we have to reconfigure the ADC each time
			break;

		// Handling ADC 12 bits channels
		case ADC8 :
		case ADC9 :
		case ADC10 :
		case ADC11 :
		case ADC12 :
		case ADC13 :
		case ADC14 :
		case ADC15 :
			// Enable the corresponding channel
			adc12b_enable_channel( ADC12B, ulChannel );

			// Start the ADC12B
			adc12b_start( ADC12B );

			// Wait for end of conversion
			while ((adc12b_get_status(ADC12B) & ADC12B_SR_DRDY) != ADC12B_SR_DRDY)
				;

			// Read the value
			ulValue = adc12b_get_latest_value(ADC12B) >> 2;
			ulValue = mapResolution(ulValue, 12, _readResolution);

			// Stop the ADC12B
			//      adc12_stop( ADC12B ) ; // never do adc12_stop() else we have to reconfigure the ADC12B each time

			// Disable the corresponding channel
			adc12b_disable_channel( ADC12B, ulChannel );
			break;

		// Compiler could yell because we don't handle DAC pins
		default :
			ulValue=0;
			break;
	}
#endif

#if defined __SAM3X8E__ || defined __SAM3X8H__
	static uint32_t latestSelectedChannel = -1;
	switch ( g_APinDescription[ulPin].ulAnalogChannel )
	{
		// Handling ADC 12 bits channels
		case ADC0 :
		case ADC1 :
		case ADC2 :
		case ADC3 :
		case ADC4 :
		case ADC5 :
		case ADC6 :
		case ADC7 :
		case ADC8 :
		case ADC9 :
		case ADC10 :
		case ADC11 :

			// Enable the corresponding channel
			if (ulChannel != latestSelectedChannel) {
				adc_enable_channel( ADC, (adc_channel_num_t)ulChannel );
				if ( latestSelectedChannel != (uint32_t)-1 )
					adc_disable_channel( ADC, (adc_channel_num_t)latestSelectedChannel );
				latestSelectedChannel = ulChannel;
			}

			// Start the ADC
			adc_start( ADC );

			// Wait for end of conversion
			while ((adc_get_status(ADC) & ADC_ISR_DRDY) != ADC_ISR_DRDY)
				;

			// Read the value
			ulValue = adc_get_latest_value(ADC);
			ulValue = mapResolution(ulValue, ADC_RESOLUTION, _readResolution);

			break;

		// Compiler could yell because we don't handle DAC pins
		default :
			ulValue=0;
			break;
	}
#endif

	return ulValue;
}

/*static void TC_SetCMR_ChannelA(Tc *tc, uint32_t chan, uint32_t v)
{
	tc->TC_CHANNEL[chan].TC_CMR = (tc->TC_CHANNEL[chan].TC_CMR & 0xFFF0FFFF) | v;
}

static void TC_SetCMR_ChannelB(Tc *tc, uint32_t chan, uint32_t v)
{
	tc->TC_CHANNEL[chan].TC_CMR = (tc->TC_CHANNEL[chan].TC_CMR & 0xF0FFFFFF) | v;
}

static uint8_t PWMEnabled = 0;
static uint8_t pinEnabled[PINS_COUNT];
static uint8_t TCChanEnabled[] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

void analogOutputInit(void) {
	uint8_t i;
	for (i=0; i<PINS_COUNT; i++)
		pinEnabled[i] = 0;
}*/

static bool nonDuePWMEnabled = false;
static uint16_t PWMChanFreq[8] = {0, 0, 0, 0, 0, 0, 0, 0};		// there are only 8 PWM channels
static uint16_t PWMChanPeriod[8];

// Version of PWMC_ConfigureChannel from Arduino core, mended to not mess up PWM channel 0 when another channel is programmed
static void PWMC_ConfigureChannel_fixed( Pwm* pPwm, uint32_t ul_channel, uint32_t prescaler, uint32_t alignment, uint32_t polarity )
{
	/* Disable ul_channel (effective at the end of the current period) */
	if ((pPwm->PWM_SR & (1 << ul_channel)) != 0)
        {
		pPwm->PWM_DIS = 1 << ul_channel;
		while ((pPwm->PWM_SR & (1 << ul_channel)) != 0);
	}
	/* Configure ul_channel */
	pPwm->PWM_CH_NUM[ul_channel].PWM_CMR = prescaler | alignment | polarity;
}

/*
 * analogWriteDuet
 * Note this does not support the other functions of the original Arduino analog write function such as timer
 * counters and the DAC. Any hardware PWM pin that is defined as such within the unDefPinDescription[] struct
 * should work, and non hardware PWM pin will default to digitalWrite
 */

extern "C" void analogWriteDuet(uint32_t ulPin, uint32_t ulValue, uint16_t freq)
{
	if (ulPin > MaxPinNumber)
	{
		return;
	}
	if (ulValue > 255)
	{
		ulValue = 255;
	}

	const PinDescription& pinDesc = g_APinDescription[ulPin];
	const uint32_t attr = pinDesc.ulPinAttribute;
	if ((attr & PIN_ATTR_ANALOG) == PIN_ATTR_ANALOG)
	{
		const EAnalogChannel channel = pinDesc.ulADCChannelNumber;
		if (channel == DA0 || channel == DA1) {
			const uint32_t chDACC = ((channel == DA0) ? 0 : 1);
			if (dacc_get_channel_status(DACC_INTERFACE) == 0) {
				/* Enable clock for DACC_INTERFACE */
				pmc_enable_periph_clk(DACC_INTERFACE_ID);
				/* Reset DACC registers */
				dacc_reset(DACC_INTERFACE);
				/* Half word transfer mode */
				dacc_set_transfer_mode(DACC_INTERFACE, 0);
				/* Power save:
				 * sleep mode - 0 (disabled)
				 * fast wakeup - 0 (disabled)
				 */
				dacc_set_power_save(DACC_INTERFACE, 0, 0);
				/* Timing:
				 * refresh - 0x08 (1024*8 dacc clocks)
				 * max speed mode - 0 (disabled)
				 * startup time - 0x10 (1024 dacc clocks)
				 */
				dacc_set_timing(DACC_INTERFACE, 0x08, 0, 0x10);
				/* Set up analog current */
				dacc_set_analog_control(DACC_INTERFACE, DACC_ACR_IBCTLCH0(0x02) |
				DACC_ACR_IBCTLCH1(0x02) |
				DACC_ACR_IBCTLDACCORE(0x01));
			}
			/* Disable TAG and select output channel chDACC */
			dacc_set_channel_selection(DACC_INTERFACE, chDACC);
			if ((dacc_get_channel_status(DACC_INTERFACE) & (1 << chDACC)) == 0) {
				dacc_enable_channel(DACC_INTERFACE, chDACC);
			}
			// Write user value
			ulValue = mapResolution(ulValue, _writeResolution, DACC_RESOLUTION);
			dacc_write_conversion_data(DACC_INTERFACE, ulValue);
			while ((dacc_get_interrupt_status(DACC_INTERFACE) & DACC_ISR_EOC) == 0);
			return;
		}
	}

	if ((attr & PIN_ATTR_PWM) == PIN_ATTR_PWM)
	{
		const uint32_t chan = pinDesc.ulPWMChannel;
		if (freq == 0)
		{
			PWMChanFreq[chan] = freq;
			pinMode(ulPin, OUTPUT);
			digitalWrite(ulPin, (ulValue < 128) ? LOW : HIGH);
		}
		else if (PWMChanFreq[chan] != freq)
		{
			if (!nonDuePWMEnabled)
			{
				// PWM Startup code
				pmc_enable_periph_clk(PWM_INTERFACE_ID);
				PWMC_ConfigureClocks(PwmSlowClock, PwmFastClock, VARIANT_MCK);	// clock A is slow clock, B is fast clock
				nonDuePWMEnabled = true;
			}

			bool useFastClock = (freq >= PwmFastClock/65535);
			uint16_t period = ((useFastClock) ? PwmFastClock : PwmSlowClock)/freq - 1;
			// Setup PWM for this PWM channel
			PIO_Configure(pinDesc.pPort,
					pinDesc.ulPinType,
					pinDesc.ulPin,
					pinDesc.ulPinConfiguration);
			PWMC_ConfigureChannel_fixed(PWM_INTERFACE, chan, (useFastClock) ? PWM_CMR_CPRE_CLKB : PWM_CMR_CPRE_CLKA, 0, 0);
			PWMC_SetPeriod(PWM_INTERFACE, chan, period);
			PWMC_SetDutyCycle(PWM_INTERFACE, chan, (ulValue * (uint32_t)period)/255);
			PWMC_EnableChannel(PWM_INTERFACE, chan);
			PWMChanFreq[chan] = freq;
			PWMChanPeriod[chan] = period;
		}
		else
		{
			PWMC_SetDutyCycle(PWM_INTERFACE, chan, (ulValue * (uint32_t)PWMChanPeriod[chan])/255);
		}
	}
	else
	{
		// Defaults to digital write
		pinMode(ulPin, OUTPUT);
		digitalWrite(ulPin, (ulValue < 128) ? LOW : HIGH);
	}
}

// Convert an Arduino Due analog pin number to the corresponding ADC channel number
extern "C" EAnalogChannel PinToAdcChannel(int pin)
{
	if (pin < A0)
	{
		pin += A0;
	}
	return g_APinDescription[pin].ulADCChannelNumber;
}

// End
