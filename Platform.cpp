/****************************************************************************************************

 RepRapFirmware - Platform: RepRapPro Ormerod with Arduino Due controller

 Platform contains all the code and definitions to deal with machine-dependent things such as control
 pins, bed area, number of extruders, tolerable accelerations and speeds and so on.

 -----------------------------------------------------------------------------------------------------

 Version 0.1

 18 November 2012

 Adrian Bowyer
 RepRap Professional Ltd
 http://reprappro.com

 Licence: GPL

 ****************************************************************************************************/

#include "RepRapFirmware.h"
#include "DueFlashStorage.h"

extern char _end;
extern "C" char *sbrk(int i);

const uint8_t memPattern = 0xA5;

static uint32_t fanInterruptCount = 0;				// accessed only in ISR, so no need to declare it volatile
const uint32_t fanMaxInterruptCount = 32;			// number of fan interrupts that we average over
static volatile uint32_t fanLastResetTime = 0;		// time (microseconds) at which we last reset the interrupt count, accessed inside and outside ISR
static volatile uint32_t fanInterval = 0;			// written by ISR, read outside the ISR

//#define MOVE_DEBUG

#ifdef MOVE_DEBUG
unsigned int numInterruptsScheduled = 0;
unsigned int numInterruptsExecuted = 0;
uint32_t nextInterruptTime = 0;
uint32_t nextInterruptScheduledAt = 0;
uint32_t lastInterruptTime = 0;
#endif

// Arduino initialise and loop functions
// Put nothing in these other than calls to the RepRap equivalents

void setup()
{
	// Fill the free memory with a pattern so that we can check for stack usage and memory corruption
	char* heapend = sbrk(0);
	register const char * stack_ptr asm ("sp");
	while (heapend + 16 < stack_ptr)
	{
		*heapend++ = memPattern;
	}

	reprap.Init();
}

void loop()
{
	reprap.Spin();
}

extern "C"
{
	// This intercepts the 1ms system tick. It must return 'false', otherwise the Arduino core tick handler will be bypassed.
	int sysTickHook()
	{
		reprap.Tick();
		return 0;
	}
}

void watchdogSetup(void)
{
    // RepRapFirmware enables the watchdog by default
    watchdogEnable(1000);
}

//*************************************************************************************************
// PidParameters class

bool PidParameters::UsePID() const
{
	return kP >= 0;
}

float PidParameters::GetThermistorR25() const
{
	return thermistorInfR * exp(thermistorBeta / (25.0 - ABS_ZERO));
}

void PidParameters::SetThermistorR25AndBeta(float r25, float beta)
{
	thermistorInfR = r25 * exp(-beta / (25.0 - ABS_ZERO));
	thermistorBeta = beta;
}

bool PidParameters::operator==(const PidParameters& other) const
{
	return kI == other.kI && kD == other.kD && kP == other.kP && kT == other.kT && kS == other.kS
				&& fullBand == other.fullBand && pidMin == other.pidMin
				&& pidMax == other.pidMax && thermistorBeta == other.thermistorBeta && thermistorInfR == other.thermistorInfR
				&& thermistorSeriesR == other.thermistorSeriesR && adcLowOffset == other.adcLowOffset
				&& adcHighOffset == other.adcHighOffset;
}

//*******************************************************************************************************************
// Platform class

/*static*/ const uint8_t Platform::pinAccessAllowed[NUM_PINS_ALLOWED/8] = PINS_ALLOWED;

Platform::Platform() :
		autoSaveEnabled(false), board(DEFAULT_BOARD_TYPE), active(false), errorCodeBits(0), fileStructureInitialised(false),
		tickState(0), debugCode(0)
{
	// Output
	auxOutput = new OutputStack();
	aux2Output = new OutputStack();
	usbOutput = new OutputStack();

	// Files
	massStorage = new MassStorage(this);
	for(size_t i = 0; i < MAX_FILES; i++)
	{
		files[i] = new FileStore(this);
	}
}

//*******************************************************************************************************************

void Platform::Init()
{
	// Deal with power first
	digitalWrite(ATX_POWER_PIN, LOW);			// ensure ATX power is off by default
	pinMode(ATX_POWER_PIN, OUTPUT);

	SetBoardType(BoardType::Auto);

	// Comms
	baudRates[0] = MAIN_BAUD_RATE;
	baudRates[1] = AUX_BAUD_RATE;
	baudRates[2] = AUX2_BAUD_RATE;
	commsParams[0] = 0;
	commsParams[1] = 1;							// by default we require a checksum on data from the aux port, to guard against overrun errors
	commsParams[2] = 0;
	// Third serial channel isn't a G-Code source

	SERIAL_MAIN_DEVICE.begin(baudRates[0]);
	SERIAL_AUX_DEVICE.begin(baudRates[1]);					// this can't be done in the constructor because the Arduino port initialisation isn't complete at that point
	SERIAL_AUX2_DEVICE.begin(baudRates[2]);

	static_assert(sizeof(FlashData) + sizeof(SoftwareResetData) <= 1024, "NVData too large");

	ResetNvData();

	// We need to initialise at least some of the time stuff before we call MassStorage::Init()
	addToTime = 0.0;
	lastTimeCall = 0;
	lastTime = Time();
	longWait = lastTime;

	// File management
	massStorage->Init();

	for(size_t file = 0; file < MAX_FILES; file++)
	{
		files[file]->Init();
	}

	fileStructureInitialised = true;

	mcpDuet.begin(); //only call begin once in the entire execution, this begins the I2C comms on that channel for all objects
	mcpExpansion.setMCP4461Address(0x2E); //not required for mcpDuet, as this uses the default address
	configFile = CONFIG_FILE;
	defaultFile = DEFAULT_FILE;

	// DRIVES
	ARRAY_INIT(stepPins, STEP_PINS);
	ARRAY_INIT(directionPins, DIRECTION_PINS);
	ARRAY_INIT(directions, DIRECTIONS);
	ARRAY_INIT(enableValues, ENABLE_VALUES);
	ARRAY_INIT(enablePins, ENABLE_PINS);
	ARRAY_INIT(endStopPins, END_STOP_PINS);
	ARRAY_INIT(maxFeedrates, MAX_FEEDRATES);
	ARRAY_INIT(accelerations, ACCELERATIONS);
	ARRAY_INIT(driveStepsPerUnit, DRIVE_STEPS_PER_UNIT);
	ARRAY_INIT(instantDvs, INSTANT_DVS);
	ARRAY_INIT(potWipes, POT_WIPES);

	senseResistor = SENSE_RESISTOR;
	maxStepperDigipotVoltage = MAX_STEPPER_DIGIPOT_VOLTAGE;
	maxStepperDACVoltage = MAX_STEPPER_DAC_VOLTAGE;

	// Z PROBE

	zProbePin = Z_PROBE_PIN;
	zProbeAdcChannel = PinToAdcChannel(zProbePin);
	InitZProbe();		// this also sets up zProbeModulationPin

	// AXES

	ARRAY_INIT(axisMaxima, AXIS_MAXIMA);
	ARRAY_INIT(axisMinima, AXIS_MINIMA);

	idleCurrentFactor = DEFAULT_IDLE_CURRENT_FACTOR;
	SetSlowestDrive();

	// HEATERS - Bed is assumed to be the first

	ARRAY_INIT(tempSensePins, TEMP_SENSE_PINS);
	ARRAY_INIT(heatOnPins, HEAT_ON_PINS);
	ARRAY_INIT(max31855CsPins, MAX31855_CS_PINS);
	ARRAY_INIT(standbyTemperatures, STANDBY_TEMPERATURES);
	ARRAY_INIT(activeTemperatures, ACTIVE_TEMPERATURES);

	heatSampleTime = HEAT_SAMPLE_TIME;
	timeToHot = TIME_TO_HOT;

	// Directories

	gcodeDir = GCODE_DIR;
	macroDir = MACRO_DIR;
	sysDir = SYS_DIR;
	webDir = WEB_DIR;

	// Motors

	for(size_t drive = 0; drive < DRIVES; drive++)
	{
		SetPhysicalDrive(drive, drive);                                 // map drivers directly to axes and extruders
		if (stepPins[drive] >= 0)
		{
			pinMode(stepPins[drive], OUTPUT);
		}
		if (directionPins[drive] >= 0)
		{
			pinMode(directionPins[drive], OUTPUT);
		}
		if (enablePins[drive] >= 0)
		{
			pinMode(enablePins[drive], OUTPUT);
		}
		if (endStopPins[drive] >= 0)
		{
			pinMode(endStopPins[drive], INPUT_PULLUP);
		}
		motorCurrents[drive] = 0.0;
		DisableDrive(drive);
		driveState[drive] = DriveStatus::disabled;
		SetElasticComp(drive, 0.0);
		if (drive <= AXES)
		{
			endStopType[drive] = (drive == Y_AXIS)
									? EndStopType::lowEndStop	// for Ormerod 2/Huxley/Mendel compatibility
									: EndStopType::noEndStop;	// for Ormerod/Huxley/Mendel compatibility
			endStopLogicLevel[drive] = true;					// assume all endstops use active high logic e.g. normally-closed switch to ground
		}
	}

	extrusionAncilliaryPWM = 0.0;

	for(size_t heater = 0; heater < HEATERS; heater++)
	{
		if (heatOnPins[heater] >= 0)
		{
			digitalWrite(heatOnPins[heater], HIGH);	// turn the heater off
			pinMode(heatOnPins[heater], OUTPUT);
		}
		analogReadResolution(12);
		thermistorAdcChannels[heater] = PinToAdcChannel(tempSensePins[heater]);	// Translate the Arduino Due Analog pin number to the SAM ADC channel number
		SetThermistorNumber(heater, heater);				// map the thermistor straight through
		thermistorFilters[heater].Init(analogRead(tempSensePins[heater]));

		// Calculate and store the ADC average sum that corresponds to an overheat condition, so that we can check is quickly in the tick ISR
		float thermistorOverheatResistance = nvData.pidParams[heater].GetRInf()
				* exp(-nvData.pidParams[heater].GetBeta() / (BAD_HIGH_TEMPERATURE - ABS_ZERO));
		float thermistorOverheatAdcValue = (AD_RANGE_REAL + 1) * thermistorOverheatResistance
				/ (thermistorOverheatResistance + nvData.pidParams[heater].thermistorSeriesR);
		thermistorOverheatSums[heater] = (uint32_t) (thermistorOverheatAdcValue + 0.9) * THERMISTOR_AVERAGE_READINGS;
	}

	InitFans();

	// Hotend configuration
	nozzleDiameter = NOZZLE_DIAMETER;
	filamentWidth = FILAMENT_WIDTH;

	// Inkjet

	inkjetBits = INKJET_BITS;
	if (inkjetBits >= 0)
	{
		inkjetFireMicroseconds = INKJET_FIRE_MICROSECONDS;
		inkjetDelayMicroseconds = INKJET_DELAY_MICROSECONDS;

		inkjetSerialOut = INKJET_SERIAL_OUT;
		pinMode(inkjetSerialOut, OUTPUT);
		digitalWrite(inkjetSerialOut, 0);

		inkjetShiftClock = INKJET_SHIFT_CLOCK;
		pinMode(inkjetShiftClock, OUTPUT);
		digitalWrite(inkjetShiftClock, LOW);

		inkjetStorageClock = INKJET_STORAGE_CLOCK;
		pinMode(inkjetStorageClock, OUTPUT);
		digitalWrite(inkjetStorageClock, LOW);

		inkjetOutputEnable = INKJET_OUTPUT_ENABLE;
		pinMode(inkjetOutputEnable, OUTPUT);
		digitalWrite(inkjetOutputEnable, HIGH);

		inkjetClear = INKJET_CLEAR;
		pinMode(inkjetClear, OUTPUT);
		digitalWrite(inkjetClear, HIGH);
	}

	// Clear the spare pin configuration
	memset(pinInitialised, 0, sizeof(pinInitialised));

	// Get the show on the road...
	lastTime = Time();
	longWait = lastTime;
	InitialiseInterrupts();		// also sets 'active' to true
}

void Platform::InvalidateFiles()
{
	for (size_t i = 0; i < MAX_FILES; i++)
	{
		files[i]->Init();
	}
}

// Specify which thermistor channel a particular heater uses
void Platform::SetThermistorNumber(size_t heater, size_t thermistor)
{
	heaterTempChannels[heater] = thermistor;

	// Initialize the associated MAX31855?
	if (thermistor >= MAX31855_START_CHANNEL)
	{
		Max31855Devices[thermistor - MAX31855_START_CHANNEL].Init(max31855CsPins[thermistor - MAX31855_START_CHANNEL]);
	}
}

int Platform::GetThermistorNumber(size_t heater) const
{
	return heaterTempChannels[heater];
}

void Platform::SetSlowestDrive()
{
	slowestDrive = 0;
	for(size_t drive = 1; drive < DRIVES; drive++)
	{
		if (ConfiguredInstantDv(drive) < ConfiguredInstantDv(slowestDrive))
		{
			slowestDrive = drive;
		}
	}
}

void Platform::InitZProbe()
{
	zProbeOnFilter.Init(0);
	zProbeOffFilter.Init(0);
	zProbeModulationPin = (board == BoardType::Duet_07 || board == BoardType::Duet_085) ? Z_PROBE_MOD_PIN07 : Z_PROBE_MOD_PIN;

	if (nvData.zProbeType >= 1 && nvData.zProbeType <= 3)
	{
		pinMode(zProbeModulationPin, OUTPUT);
		digitalWrite(zProbeModulationPin, (nvData.zProbeType <= 2) ? HIGH : LOW);	// enable the IR LED or alternate sensor
	}
	else if (nvData.zProbeType == 4)
	{
		pinMode(endStopPins[E0_DRIVE], INPUT_PULLUP);
	}
}

uint16_t Platform::GetRawZProbeReading() const
{
	if (nvData.zProbeType == 4)
	{
		bool b = (bool)digitalRead(endStopPins[E0_DRIVE]);
		if (!endStopLogicLevel[AXES])
		{
			b = !b;
		}
		return (b) ? 4000 : 0;
	}
	else
	{
		return GetAdcReading(zProbeAdcChannel);
	}
}

// Return the Z probe data.
// The ADC readings are 12 bits, so we convert them to 10-bit readings for compatibility with the old firmware.
int Platform::ZProbe() const
{
	if (zProbeOnFilter.IsValid() && zProbeOffFilter.IsValid())
	{
		switch (nvData.zProbeType)
		{
			case 1:		// Simple or intelligent IR sensor
			case 3:		// Alternate sensor
			case 4:		// Mechanical Z probe
				return (int) ((zProbeOnFilter.GetSum() + zProbeOffFilter.GetSum()) / (8 * Z_PROBE_AVERAGE_READINGS));

			case 2:
				// Modulated IR sensor. We assume that zProbeOnFilter and zProbeOffFilter average the same number of readings.
				// Because of noise, it is possible to get a negative reading, so allow for this.
				return (int) (((int32_t) zProbeOnFilter.GetSum() - (int32_t) zProbeOffFilter.GetSum())
						/ (int)(4 * Z_PROBE_AVERAGE_READINGS));

			default:
				break;
		}
	}
	return 0;	// Z probe not turned on or not initialised yet
}

// Provide the Z probe secondary values and return the number of secondary values
int Platform::GetZProbeSecondaryValues(int& v1, int& v2)
{
	if (zProbeOnFilter.IsValid() && zProbeOffFilter.IsValid())
	{
		switch (nvData.zProbeType)
		{
			case 2:		// modulated IR sensor
				v1 = (int) (zProbeOnFilter.GetSum() / (4 * Z_PROBE_AVERAGE_READINGS));	// pass back the reading with IR turned on
				return 1;

			default:
				break;
		}
	}
	return 0;
}

int Platform::GetZProbeType() const
{
	return nvData.zProbeType;
}

void Platform::SetZProbeAxes(const bool axes[AXES])
{
	for(size_t axis = 0; axis < AXES; axis++)
	{
		nvData.zProbeAxes[axis] = axes[axis];
	}

	if (autoSaveEnabled)
	{
		WriteNvData();
	}
}

void Platform::GetZProbeAxes(bool (&axes)[AXES])
{
	for(size_t axis = 0; axis < AXES; axis++)
	{
		axes[axis] = nvData.zProbeAxes[axis];
	}
}

float Platform::ZProbeStopHeight() const
{
	switch (nvData.zProbeType)
	{
		case 0:
		case 4:
			return nvData.switchZProbeParameters.GetStopHeight(GetTemperature(0));
		case 1:
		case 2:
			return nvData.irZProbeParameters.GetStopHeight(GetTemperature(0));
		case 3:
			return nvData.alternateZProbeParameters.GetStopHeight(GetTemperature(0));
		default:
			return 0;
	}
}

float Platform::GetZProbeDiveHeight() const
{
	switch (nvData.zProbeType)
	{
		case 0:
		case 4:
			return nvData.switchZProbeParameters.diveHeight;
		case 1:
		case 2:
			return nvData.irZProbeParameters.diveHeight;
		case 3:
			return nvData.alternateZProbeParameters.diveHeight;
	}
	return DEFAULT_Z_DIVE;
}

float Platform::GetZProbeTravelSpeed() const
{
	switch (nvData.zProbeType)
	{
		case 0:
		case 4:
			return nvData.switchZProbeParameters.travelSpeed;
		case 1:
		case 2:
			return nvData.irZProbeParameters.travelSpeed;
		case 3:
			return nvData.alternateZProbeParameters.travelSpeed;
	}

	return DEFAULT_TRAVEL_SPEED;
}

void Platform::SetZProbeType(int pt)
{
	int newZProbeType = (pt >= 0 && pt <= 4) ? pt : 0;
	if (newZProbeType != nvData.zProbeType)
	{
		nvData.zProbeType = newZProbeType;
		if (autoSaveEnabled)
		{
			WriteNvData();
		}
	}
	InitZProbe();
}

const ZProbeParameters& Platform::GetZProbeParameters() const
{
	switch (nvData.zProbeType)
	{
		case 0:
		case 4:
		default:
			return nvData.switchZProbeParameters;

		case 1:
		case 2:
			return nvData.irZProbeParameters;

		case 3:
			return nvData.alternateZProbeParameters;
	}
}

bool Platform::SetZProbeParameters(const struct ZProbeParameters& params)
{
	switch (nvData.zProbeType)
	{
		case 0:
		case 4:
			if (nvData.switchZProbeParameters != params)
			{
				nvData.switchZProbeParameters = params;
				if (autoSaveEnabled)
				{
					WriteNvData();
				}
			}
			return true;

		case 1:
		case 2:
			if (nvData.irZProbeParameters != params)
			{
				nvData.irZProbeParameters = params;
				if (autoSaveEnabled)
				{
					WriteNvData();
				}
			}
			return true;

		case 3:
			if (nvData.alternateZProbeParameters != params)
			{
				nvData.alternateZProbeParameters = params;
				if (autoSaveEnabled)
				{
					WriteNvData();
				}
			}
			return true;
	}
	return false;
}

// Return true if we must home X and Y before we home Z (i.e. we are using a bed probe)
bool Platform::MustHomeXYBeforeZ() const
{
	return nvData.zProbeType != 0;
}

void Platform::ResetNvData()
{
	nvData.compatibility = me;

	ARRAY_INIT(nvData.ipAddress, IP_ADDRESS);
	ARRAY_INIT(nvData.netMask, NET_MASK);
	ARRAY_INIT(nvData.gateWay, GATE_WAY);
	ARRAY_INIT(nvData.macAddress, MAC_ADDRESS);

	nvData.zProbeType = 0;			// Default is to use the switch
	ARRAY_INIT(nvData.zProbeAxes, Z_PROBE_AXES);
	nvData.switchZProbeParameters.Init(0.0);
	nvData.irZProbeParameters.Init(Z_PROBE_STOP_HEIGHT);
	nvData.alternateZProbeParameters.Init(Z_PROBE_STOP_HEIGHT);

	for (size_t i = 0; i < HEATERS; ++i)
	{
		PidParameters& pp = nvData.pidParams[i];
		pp.thermistorSeriesR = DEFAULT_THERMISTOR_SERIES_RS[i];
		pp.SetThermistorR25AndBeta(DEFAULT_THERMISTOR_25_RS[i], DEFAULT_THERMISTOR_BETAS[i]);
		pp.kI = DEFAULT_PID_KIS[i];
		pp.kD = DEFAULT_PID_KDS[i];
		pp.kP = DEFAULT_PID_KPS[i];
		pp.kT = DEFAULT_PID_KTS[i];
		pp.kS = DEFAULT_PID_KSS[i];
		pp.fullBand = DEFAULT_PID_FULLBANDS[i];
		pp.pidMin = DEFAULT_PID_MINS[i];
		pp.pidMax = DEFAULT_PID_MAXES[i];
		pp.adcLowOffset = pp.adcHighOffset = 0.0;
	}

#ifdef FLASH_SAVE_ENABLED
	nvData.magic = FlashData::magicValue;
#endif
}

void Platform::ReadNvData()
{
#ifdef FLASH_SAVE_ENABLED
	DueFlashStorage::read(FlashData::nvAddress, &nvData, sizeof(nvData));
	if (nvData.magic != FlashData::magicValue)
	{
		// Nonvolatile data has not been initialised since the firmware was last written, so set up default values
		ResetNvData();
		// No point in writing it back here
	}
#else
	Message(GENERIC_MESSAGE, "Error: Cannot load non-volatile data, because Flash support has been disabled!\n");
#endif
}

void Platform::WriteNvData()
{
#ifdef FLASH_SAVE_ENABLED
	DueFlashStorage::write(FlashData::nvAddress, &nvData, sizeof(nvData));
#else
	Message(GENERIC_MESSAGE, "Error: Cannot write non-volatile data, because Flash support has been disabled!\n");
#endif
}

void Platform::SetAutoSave(bool enabled)
{
#ifdef FLASH_SAVE_ENABLED
	autoSaveEnabled = enabled;
#else
	Message(GENERIC_MESSAGE, "Error: Cannot enable auto-save, because Flash support has been disabled!\n");
#endif
}

// Send beep message to the AUX device. Should be eventually superseded by new-style status response
void Platform::Beep(int freq, int ms)
{
	MessageF(AUX_MESSAGE, "{\"beep_freq\":%d,\"beep_length\":%d}\n", freq, ms);
}

// Note: the use of floating point time will cause the resolution to degrade over time.
// For example, 1ms time resolution will only be available for about half an hour from startup.
// Personally, I (dc42) would rather just maintain and provide the time in milliseconds in a uint32_t.
// This would wrap round after about 49 days, but that isn't difficult to handle.
float Platform::Time()
{
	unsigned long now = micros();
	if (now < lastTimeCall) // Has timer overflowed?
	{
		addToTime += ((float) ULONG_MAX) * TIME_FROM_REPRAP;
	}
	lastTimeCall = now;
	return addToTime + TIME_FROM_REPRAP * (float) now;
}

void Platform::Exit()
{
	Message(GENERIC_MESSAGE, "Platform class exited.\n");
	active = false;
}

Compatibility Platform::Emulating() const
{
	if (nvData.compatibility == reprapFirmware)
		return me;
	return nvData.compatibility;
}

void Platform::SetEmulating(Compatibility c)
{
	if (c != me && c != reprapFirmware && c != marlin)
	{
		Message(GENERIC_MESSAGE, "Error: Attempt to emulate unsupported firmware.\n");
		return;
	}
	if (c == reprapFirmware)
	{
		c = me;
	}
	if (c != nvData.compatibility)
	{
		nvData.compatibility = c;
		if (autoSaveEnabled)
		{
			WriteNvData();
		}
	}
}

void Platform::SetMACAddress(uint8_t mac[])
{
	bool changed = false;
	for (size_t i = 0; i < 6; i++)
	{
		if (nvData.macAddress[i] != mac[i])
		{
			nvData.macAddress[i] = mac[i];
			changed = true;
		}
	}

	if (changed && autoSaveEnabled)
	{
		WriteNvData();
	}
}

void Platform::UpdateNetworkAddress(byte dst[4], const byte src[4])
{
	bool changed = false;
	for (size_t i = 0; i < 4; i++)
	{
		if (dst[i] != src[i])
		{
			dst[i] = src[i];
			changed = true;
		}
	}

	if (changed)
	{
		if (autoSaveEnabled)
		{
			WriteNvData();
		}
		reprap.GetNetwork()->SetIPAddress(nvData.ipAddress, nvData.netMask, nvData.gateWay);
	}
}

void Platform::SetIPAddress(byte ip[])
{
	UpdateNetworkAddress(nvData.ipAddress, ip);
}

void Platform::SetGateWay(byte gw[])
{
	UpdateNetworkAddress(nvData.gateWay, gw);
}

void Platform::SetNetMask(byte nm[])
{
	UpdateNetworkAddress(nvData.netMask, nm);
}

void Platform::Spin()
{
	if (!active)
		return;

	// Write non-blocking data to the AUX line
	OutputBuffer *auxOutputBuffer = auxOutput->GetFirstItem();
	if (auxOutputBuffer != nullptr)
	{
		size_t bytesToWrite = min<size_t>(SERIAL_AUX_DEVICE.canWrite(), auxOutputBuffer->BytesLeft());
		if (bytesToWrite > 0)
		{
			SERIAL_AUX_DEVICE.write(auxOutputBuffer->Read(bytesToWrite), bytesToWrite);
		}

		if (auxOutputBuffer->BytesLeft() == 0)
		{
			auxOutputBuffer = OutputBuffer::Release(auxOutputBuffer);
			auxOutput->SetFirstItem(auxOutputBuffer);
		}
	}

	// Write non-blocking data to the second AUX line
	OutputBuffer *aux2OutputBuffer = aux2Output->GetFirstItem();
	if (aux2OutputBuffer != nullptr)
	{
		size_t bytesToWrite = min<size_t>(SERIAL_AUX2_DEVICE.canWrite(), aux2OutputBuffer->BytesLeft());
		if (bytesToWrite > 0)
		{
			SERIAL_AUX2_DEVICE.write(aux2OutputBuffer->Read(bytesToWrite), bytesToWrite);
		}

		if (aux2OutputBuffer->BytesLeft() == 0)
		{
			aux2OutputBuffer = OutputBuffer::Release(aux2OutputBuffer);
			aux2Output->SetFirstItem(aux2OutputBuffer);
		}
	}

	// Write non-blocking data to the USB line
	OutputBuffer *usbOutputBuffer = usbOutput->GetFirstItem();
	if (usbOutputBuffer != nullptr)
	{
		if (!SERIAL_MAIN_DEVICE)
		{
			// If the USB port is not opened, free the data left for writing
			OutputBuffer::ReleaseAll(usbOutputBuffer);
			usbOutput->SetFirstItem(nullptr);
		}
		else
		{
			// Write as much data as we can...
			size_t bytesToWrite = min<size_t>(SERIAL_MAIN_DEVICE.canWrite(), usbOutputBuffer->BytesLeft());
			if (bytesToWrite > 0)
			{
				SERIAL_MAIN_DEVICE.write(usbOutputBuffer->Read(bytesToWrite), bytesToWrite);
			}

			if (usbOutputBuffer->BytesLeft() == 0)
			{
				usbOutputBuffer = OutputBuffer::Release(usbOutputBuffer);
				usbOutput->SetFirstItem(usbOutputBuffer);
			}
		}
	}

	// Thermostatically-controlled fans
	for (size_t fan = 0; fan < NUM_FANS; ++fan)
	{
		fans[fan].Check();
	}

	// Diagnostics test
	if (debugCode == (int)DiagnosticTestType::TestSpinLockup)
	{
		for (;;) {}
	}

	ClassReport(longWait);
}

static void eraseAndReset()
{
	cpu_irq_disable();
	for(size_t i = 0; i <= (IFLASH_LAST_PAGE_ADDRESS - IFLASH_ADDR) / IFLASH_PAGE_SIZE; i++)
	{
		size_t pageStartAddr = IFLASH_ADDR + i * IFLASH_PAGE_SIZE;
		flash_unlock(pageStartAddr, pageStartAddr + IFLASH_PAGE_SIZE - 1, nullptr, nullptr);
	}
	flash_clear_gpnvm(1);			// tell the system to boot from ROM next time
	rstc_start_software_reset(RSTC);
	for(;;) {}
}

void Platform::SoftwareReset(SoftwareResetReason reason)
{
	if (reason == SoftwareResetReason::erase)
	{
		eraseAndReset();			// does not return...
	}

	uint16_t resetReason = (uint16_t)reason;
	if (reason != SoftwareResetReason::user)
	{
		if (!SERIAL_MAIN_DEVICE.canWrite())
		{
			resetReason |= (uint16_t)SoftwareResetReason::inUsbOutput;	// if we are resetting because we are stuck in a Spin function, record whether we are trying to send to USB
		}
		if (reprap.GetNetwork()->InLwip())
		{
			resetReason |= (uint16_t)SoftwareResetReason::inLwipSpin;
		}
		if (!SERIAL_AUX_DEVICE.canWrite() || !SERIAL_AUX2_DEVICE.canWrite())
		{
			resetReason |= (uint16_t)SoftwareResetReason::inAuxOutput;	// if we are resetting because we are stuck in a Spin function, record whether we are trying to send to aux
		}
	}
	resetReason |= reprap.GetSpinningModule();

	// Record the reason for the software reset
	SoftwareResetData temp;
	temp.magic = SoftwareResetData::magicValue;
	temp.resetReason = resetReason;
	GetStackUsage(nullptr, nullptr, &temp.neverUsedRam);

	// Save diagnostics data to Flash and reset the software
	DueFlashStorage::write(SoftwareResetData::nvAddress, &temp, sizeof(SoftwareResetData));

	rstc_start_software_reset(RSTC);
	for(;;) {}
}

//*****************************************************************************************************************

// Interrupts

void TC3_Handler()
{
	TC1->TC_CHANNEL[0].TC_IDR = TC_IER_CPAS;	// disable the interrupt
#ifdef MOVE_DEBUG
	++numInterruptsExecuted;
	lastInterruptTime = Platform::GetInterruptClocks();
#endif
	reprap.Interrupt();
}

void TC4_Handler()
{
	TC_GetStatus(TC1, 1);
	reprap.GetNetwork()->Interrupt();
}

void FanInterrupt()
{
	++fanInterruptCount;
	if (fanInterruptCount == fanMaxInterruptCount)
	{
		uint32_t now = micros();
		fanInterval = now - fanLastResetTime;
		fanLastResetTime = now;
		fanInterruptCount = 0;
	}
}

void Platform::InitialiseInterrupts()
{
	// The SAM3X NVIC supports up to 16 user-defined priority levels (0-15) with 0 being the highest.
	// First set the tick interrupt to the highest priority. We need to to monitor the heaters and kick the watchdog.
	tickState = 0;
	currentHeater = 0;
	NVIC_SetPriority(SysTick_IRQn, 0);

	// UART isn't as critical as the SysTick IRQ, but still important enough to prevent garbage on the serial line
	SERIAL_AUX_DEVICE.setInterruptPriority(1);							// set priority for UART interrupt - must be higher than step interrupt

	// Timer interrupt for stepper motors
	// The clock rate we use is a compromise. Too fast and the 64-bit square roots take a long time to execute. Too slow and we lose resolution.
	// We choose a clock divisor of 32, which gives us 0.38us resolution. The next option is 128 which would give 1.524us resolution.
	pmc_set_writeprotect(false);
	pmc_enable_periph_clk((uint32_t) TC3_IRQn);
	TC_Configure(TC1, 0, TC_CMR_WAVE | TC_CMR_WAVSEL_UP | TC_CMR_TCCLKS_TIMER_CLOCK3);
	TC1 ->TC_CHANNEL[0].TC_IDR = ~(uint32_t)0;				// interrupts disabled for now
	TC_Start(TC1, 0);
	TC_GetStatus(TC1, 0);									// clear any pending interrupt
	NVIC_SetPriority(TC3_IRQn, 2);							// set high priority for this IRQ; it's time-critical
	NVIC_EnableIRQ(TC3_IRQn);

	// Timer interrupt to keep the networking timers running (called at 8Hz)
	pmc_enable_periph_clk((uint32_t) TC4_IRQn);
	TC_Configure(TC1, 1, TC_CMR_WAVE | TC_CMR_WAVSEL_UP_RC | TC_CMR_TCCLKS_TIMER_CLOCK2);
	uint32_t rc = (VARIANT_MCK/8)/8;						// 8 because we selected TIMER_CLOCK2 above
	TC_SetRA(TC1, 1, rc/2);									// 50% high, 50% low
	TC_SetRC(TC1, 1, rc);
	TC_Start(TC1, 1);
	TC1 ->TC_CHANNEL[1].TC_IER = TC_IER_CPCS;
	TC1 ->TC_CHANNEL[1].TC_IDR = ~TC_IER_CPCS;
	NVIC_SetPriority(TC4_IRQn, 3);							// Ethernet timers aren't as critical as the step and UART interrupts
	NVIC_EnableIRQ(TC4_IRQn);

	// Interrupt for 4-pin PWM fan sense line
	if (coolingFanRpmPin >= 0)
	{
		// Priorities for attachInterrupt() are defined in cores/arduino/WInterrupts.c
		attachInterrupt(coolingFanRpmPin, FanInterrupt, FALLING);
	}

	// Initialisation done
	active = true;
}

//*************************************************************************************************

// Debugging variables
//extern "C" uint32_t longestWriteWaitTime, shortestWriteWaitTime, longestReadWaitTime, shortestReadWaitTime;
//extern uint32_t maxRead, maxWrite;

void Platform::Diagnostics()
{
	Message(GENERIC_MESSAGE, "Platform Diagnostics:\n");

	// Print memory stats and error codes to USB and copy them to the current webserver reply
	const char *ramstart = (char *) 0x20070000;
	const struct mallinfo mi = mallinfo();
	Message(GENERIC_MESSAGE, "Memory usage:\n");
	MessageF(GENERIC_MESSAGE, "Program static ram used: %d\n", &_end - ramstart);
	MessageF(GENERIC_MESSAGE, "Dynamic ram used: %d\n", mi.uordblks);
	MessageF(GENERIC_MESSAGE, "Recycled dynamic ram: %d\n", mi.fordblks);
	size_t currentStack, maxStack, neverUsed;
	GetStackUsage(&currentStack, &maxStack, &neverUsed);
	MessageF(GENERIC_MESSAGE, "Current stack ram used: %d\n", currentStack);
	MessageF(GENERIC_MESSAGE, "Maximum stack ram used: %d\n", maxStack);
	MessageF(GENERIC_MESSAGE, "Never used ram: %d\n", neverUsed);

	// Show the up time and reason for the last reset
	const uint32_t now = (uint32_t)Time();		// get up time in seconds
	const char* resetReasons[8] = { "power up", "backup", "watchdog", "software", "external", "?", "?", "?" };
	MessageF(GENERIC_MESSAGE, "Last reset %02d:%02d:%02d ago, cause: %s\n",
			(unsigned int)(now/3600), (unsigned int)((now % 3600)/60), (unsigned int)(now % 60),
			resetReasons[(REG_RSTC_SR & RSTC_SR_RSTTYP_Msk) >> RSTC_SR_RSTTYP_Pos]);

	// Show the error code stored at the last software reset
	{
		SoftwareResetData temp;
		temp.magic = 0;
		DueFlashStorage::read(SoftwareResetData::nvAddress, &temp, sizeof(SoftwareResetData));
		if (temp.magic == SoftwareResetData::magicValue)
		{
			MessageF(GENERIC_MESSAGE, "Last software reset code & available RAM: 0x%04x, %u\n", temp.resetReason, temp.neverUsedRam);
			MessageF(GENERIC_MESSAGE, "Spinning module during software reset: %s\n", moduleName[temp.resetReason & 0x0F]);
		}
	}

	// Show the current error codes
	MessageF(GENERIC_MESSAGE, "Error status: %u\n", errorCodeBits);

	// Show the current probe position heights
	MessageF(GENERIC_MESSAGE, "Bed probe heights:");
	for(size_t i = 0; i < MAX_PROBE_POINTS; ++i)
	{
		MessageF(GENERIC_MESSAGE, " %.3f", reprap.GetMove()->ZBedProbePoint(i));
	}
	MessageF(GENERIC_MESSAGE, "\n");

	// Show the number of free entries in the file table
	size_t numFreeFiles = 0;
	for (size_t i = 0; i < MAX_FILES; i++)
	{
		if (!files[i]->inUse)
		{
			++numFreeFiles;
		}
	}
	MessageF(GENERIC_MESSAGE, "Free file entries: %u\n", numFreeFiles);

	// Show the longest write time
	MessageF(GENERIC_MESSAGE, "Longest block write time: %.1fms\n", FileStore::GetAndClearLongestWriteTime());

	// Debug
	//MessageF(GENERIC_MESSAGE, "Shortest/longest times read %.1f/%.1f write %.1f/%.1f ms, %u/%u\n",
	//		(float)shortestReadWaitTime/1000, (float)longestReadWaitTime/1000, (float)shortestWriteWaitTime/1000, (float)longestWriteWaitTime/1000,
	//		maxRead, maxWrite);
	//longestWriteWaitTime = longestReadWaitTime = 0; shortestReadWaitTime = shortestWriteWaitTime = 1000000;

	reprap.Timing();

#ifdef MOVE_DEBUG
	MessageF(GENERIC_MESSAGE, "Interrupts scheduled %u, done %u, last %u, next %u sched at %u, now %u\n",
			numInterruptsScheduled, numInterruptsExecuted, lastInterruptTime, nextInterruptTime, nextInterruptScheduledAt, GetInterruptClocks());
#endif
}

void Platform::DiagnosticTest(DiagnosticTestType d)
{
	switch (d)
	{
		case DiagnosticTestType::TestWatchdog:
			SysTick ->CTRL &= ~(SysTick_CTRL_TICKINT_Msk);	// disable the system tick interrupt so that we get a watchdog timeout reset
			break;

		case DiagnosticTestType::TestSpinLockup:
			debugCode = (int)d;								// tell the Spin function to loop
			break;

		case DiagnosticTestType::TestSerialBlock:			// write an arbitrary message via debugPrintf()
			debugPrintf("Diagnostic Test\n");
			break;

		default:
			break;
	}
}

// Return the stack usage and amount of memory that has never been used, in bytes
void Platform::GetStackUsage(size_t* currentStack, size_t* maxStack, size_t* neverUsed) const
{
	const char *ramend = (const char *) 0x20088000;
	register const char * stack_ptr asm ("sp");
	const char *heapend = sbrk(0);
	const char* stack_lwm = heapend;
	while (stack_lwm < stack_ptr && *stack_lwm == memPattern)
	{
		++stack_lwm;
	}
	if (currentStack) { *currentStack = ramend - stack_ptr; }
	if (maxStack) { *maxStack = ramend - stack_lwm; }
	if (neverUsed) { *neverUsed = stack_lwm - heapend; }
}

void Platform::ClassReport(float &lastTime)
{
	const Module spinningModule = reprap.GetSpinningModule();
	if (reprap.Debug(spinningModule))
	{
		if (Time() - lastTime >= LONG_TIME)
		{
			lastTime = Time();
			MessageF(HOST_MESSAGE, "Class %s spinning.\n", moduleName[spinningModule]);
		}
	}
}

//===========================================================================
//=============================Thermal Settings  ============================
//===========================================================================

// See http://en.wikipedia.org/wiki/Thermistor#B_or_.CE.B2_parameter_equation

// BETA is the B value
// RS is the value of the series resistor in ohms
// R_INF is R0.exp(-BETA/T0), where R0 is the thermistor resistance at T0 (T0 is in kelvin)
// Normally T0 is 298.15K (25 C).  If you write that expression in brackets in the #define the compiler 
// should compute it for you (i.e. it won't need to be calculated at run time).

// If the A->D converter has a range of 0..1023 and the measured voltage is V (between 0 and 1023)
// then the thermistor resistance, R = V.RS/(1024 - V)
// and the temperature, T = BETA/ln(R/R_INF)
// To get degrees celsius (instead of kelvin) add -273.15 to T

// Result is in degrees celsius

float Platform::GetTemperature(size_t heater, TempError* err) const
{
	// Note that at this point we're actually getting an averaged ADC read, not a "raw" temp.  For thermistors,
	// we're getting an averaged voltage reading which we'll convert to a temperature.
	if (DoThermistorAdc(heater))
	{
		int rawTemp = GetRawTemperature(heater);

		// If the ADC reading is N then for an ideal ADC, the input voltage is at least N/(AD_RANGE + 1) and less than (N + 1)/(AD_RANGE + 1), times the analog reference.
		// So we add 0.5 to to the reading to get a better estimate of the input.
		float reading = (float) rawTemp + 0.5;

		// Recognise the special case of thermistor disconnected.
		// For some ADCs, the high-end offset is negative, meaning that the ADC never returns a high enough value. We need to allow for this here.
		const PidParameters& p = nvData.pidParams[heater];
		if (p.adcHighOffset < 0.0)
		{
			rawTemp -= (int) p.adcHighOffset;
		}
		if (rawTemp >= (int)AD_DISCONNECTED_VIRTUAL)
		{
			// thermistor is disconnected
			if (err)
			{
				*err = TempError::errOpen;
			}
			return ABS_ZERO;
		}

		// Correct for the low and high ADC offsets
		reading -= p.adcLowOffset;
		reading *= (AD_RANGE_VIRTUAL + 1) / (AD_RANGE_VIRTUAL + 1 + p.adcHighOffset - p.adcLowOffset);

		float resistance = reading * p.thermistorSeriesR / ((AD_RANGE_VIRTUAL + 1) - reading);
		if (resistance > p.GetRInf())
		{
			return ABS_ZERO + p.GetBeta() / log(resistance / p.GetRInf());
		}
		else
		{
			// thermistor short circuit, return a high temperature
			if (err) *err = TempError::errShort;
			return BAD_ERROR_TEMPERATURE;
		}
	}
	else
	{
		// MAX31855 thermocouple chip
		float temp;
		MAX31855_error res = Max31855Devices[heaterTempChannels[heater] - MAX31855_START_CHANNEL].getTemperature(&temp);
		if (res == MAX31855_OK)
		{
			return temp;
		}
		if (err)
		{
			switch(res) {
			case MAX31855_OK      : *err = TempError::errOk; break;			// Success
			case MAX31855_ERR_SCV : *err = TempError::errShortVcc; break;	// Short to Vcc
			case MAX31855_ERR_SCG : *err = TempError::errShortGnd; break;	// Short to GND
			case MAX31855_ERR_OC  : *err = TempError::errOpen; break;		// Open connection
			case MAX31855_ERR_TMO : *err = TempError::errTimeout; break;	// SPI comms timeout
			case MAX31855_ERR_IO  : *err = TempError::errIO; break;			// SPI comms not functioning
			}
		}
		return BAD_ERROR_TEMPERATURE;
	}
}

/*static*/ const char* Platform::TempErrorStr(TempError err)
{
	switch(err)
	{
	default : return "Unknown temperature read error";
	case TempError::errOk : return "successful temperature read";
	case TempError::errShort : return "sensor circuit is shorted";
	case TempError::errShortVcc : return "sensor circuit is shorted to the voltage rail";
	case TempError::errShortGnd : return "sensor circuit is shorted to ground";
	case TempError::errOpen : return "sensor circuit is open/disconnected";
	case TempError::errTimeout : return "communication error whilst reading sensor; read took too long";
	case TempError::errIO: return "communication error whilst reading sensor; check sensor connections";
	}
}

// See if we need to turn on the hot end fan
bool Platform::AnyHeaterHot(uint16_t heaters, float t) const
{
	for (size_t h = 0; h < reprap.GetHeatersInUse(); ++h)
	{
		if (((1 << h) & heaters) != 0)
		{
			const float ht = GetTemperature(h);
			if (ht >= t || ht < BAD_LOW_TEMPERATURE)
			{
				return true;
			}
		}
	}
	return false;
}

void Platform::SetPidParameters(size_t heater, const PidParameters& params)
{
	if (heater < HEATERS && params != nvData.pidParams[heater])
	{
		nvData.pidParams[heater] = params;
		if (autoSaveEnabled)
		{
			WriteNvData();
		}
	}
}
const PidParameters& Platform::GetPidParameters(size_t heater) const
{
	// Default to hot bed if an invalid heater index is passed
	if (heater >= HEATERS)
		heater = 0;

	return nvData.pidParams[heater];
}

// power is a fraction in [0,1]

void Platform::SetHeater(size_t heater, float power)
{
	SetHeaterPwm(heater, (uint8_t)(255.0 * min<float>(1.0, max<float>(0.0, power))));
}

void Platform::SetHeaterPwm(size_t heater, uint8_t power)
{
	if (heatOnPins[heater] >= 0)
	{
		uint16_t freq = (reprap.GetHeat()->UseSlowPwm(heater)) ? SLOW_HEATER_PWM_FREQUENCY : NORMAL_HEATER_PWM_FREQUENCY;
		analogWriteDuet(heatOnPins[heater], (HEAT_ON == 0) ? 255 - power : power, freq);
	}
}

EndStopHit Platform::Stopped(size_t drive) const
{
	if (endStopType[drive] == EndStopType::noEndStop)
	{
		// No homing switch is configured for this axis, so see if we should use the Z probe
		if (nvData.zProbeType > 0 && drive < AXES && nvData.zProbeAxes[drive])
		{
			return GetZProbeResult();			// using the Z probe as a low homing stop for this axis, so just get its result
		}
	}
	else if (endStopPins[drive] >= 0)
	{
		if (digitalRead(endStopPins[drive]) == ((endStopLogicLevel[drive]) ? 1 : 0))
		{
			return (endStopType[drive] == EndStopType::highEndStop) ? EndStopHit::highHit : EndStopHit::lowHit;
		}
	}
	return EndStopHit::noStop;
}

// Return the Z probe result. We assume that if the Z probe is used as an endstop, it is used as the low stop.
EndStopHit Platform::GetZProbeResult() const
{
	const int zProbeVal = ZProbe();
	const int zProbeADValue =
		(nvData.zProbeType == 4) ? nvData.switchZProbeParameters.adcValue
		: (nvData.zProbeType == 3) ? nvData.alternateZProbeParameters.adcValue
		: nvData.irZProbeParameters.adcValue;
	return (zProbeVal >= zProbeADValue) ? EndStopHit::lowHit
		: (zProbeVal * 10 >= zProbeADValue * 9) ? EndStopHit::lowNear   // if we are at/above 90% of the target value
		: EndStopHit::noStop;
}

// This is called from the step ISR as well as other places, so keep it fast, especially in the case where the motor is already enabled
void Platform::SetDirection(size_t drive, bool direction)
{
	const int driver = driverNumbers[drive];
	if (driver >= 0)
	{
		const int pin = directionPins[driver];
		if (pin >= 0)
		{
			bool d = (direction == FORWARDS) ? directions[driver] : !directions[driver];
			digitalWrite(pin, d);
		}
	}
}

// Enable a drive. Must not be called from an ISR, or with interrupts disabled.
void Platform::EnableDrive(size_t drive)
{
	if (drive < DRIVES && driveState[drive] != DriveStatus::enabled)
	{
		driveState[drive] = DriveStatus::enabled;
		const int driver = driverNumbers[drive];
		if (driver >= 0)
		{
			UpdateMotorCurrent(driver);

			const int pin = enablePins[driver];
			if (pin >= 0)
			{
				digitalWrite(pin, enableValues[driver]);
			}
		}
	}
}

// Disable a drive, if it has a disable pin
void Platform::DisableDrive(size_t drive)
{
	if (drive < DRIVES)
	{
		const int driver = driverNumbers[drive];
		if (driver >= 0)
		{
			const int pin = enablePins[driver];
			if (pin >= 0)
			{
				digitalWrite(pin, !enableValues[driver]);
			}
		}
		driveState[drive] = DriveStatus::disabled;
	}
}

// Set drives to idle hold if they are enabled. If a drive is disabled, leave it alone.
void Platform::SetDrivesIdle()
{
	for(size_t drive = 0; drive < DRIVES; drive++)
	{
		if (driveState[drive] == DriveStatus::enabled)
		{
			driveState[drive] = DriveStatus::idle;
			UpdateMotorCurrent(drive);
		}
	}
}

// Set the current for a motor. Current is in mA.
void Platform::SetMotorCurrent(size_t drive, float current)
{
	if (drive < DRIVES)
	{
		motorCurrents[drive] = current;
		UpdateMotorCurrent(drive);
	}
}

// This must not be called from an ISR, or with interrupts disabled.
void Platform::UpdateMotorCurrent(size_t drive)
{
	if (drive < DRIVES)
	{
		float current = motorCurrents[drive];
		if (driveState[drive] == DriveStatus::idle)
		{
			current *= idleCurrentFactor;
		}
		unsigned short pot = (unsigned short)((0.256*current*8.0*senseResistor + maxStepperDigipotVoltage/2)/maxStepperDigipotVoltage);
		const size_t driver = driverNumbers[drive];

		if (driver < 4)
		{
			mcpDuet.setNonVolatileWiper(potWipes[driver], pot);
			mcpDuet.setVolatileWiper(potWipes[driver], pot);
		}
		else
		{
			if (board == BoardType::Duet_085)
			{
				// Extruder 1 is on DAC channel 0
				if (driver == 4)
				{
					unsigned short dac = (unsigned short)((0.256*current*8.0*senseResistor + maxStepperDACVoltage/2)/maxStepperDACVoltage);
					analogWrite(DAC0, dac);
				}
				else
				{
					mcpExpansion.setNonVolatileWiper(potWipes[driver - 1], pot);
					mcpExpansion.setVolatileWiper(potWipes[driver - 1], pot);
				}
			}
			else
			{
				mcpExpansion.setNonVolatileWiper(potWipes[driver], pot);
				mcpExpansion.setVolatileWiper(potWipes[driver], pot);
			}
		}
	}
}

float Platform::MotorCurrent(size_t drive) const
{
	return (drive < DRIVES) ? motorCurrents[drive] : 0.0;
}

// Set the motor idle current factor
void Platform::SetIdleCurrentFactor(float f)
{
	idleCurrentFactor = f;
	for (size_t drive = 0; drive < DRIVES; ++drive)
	{
		if (driveState[drive] == DriveStatus::idle)
		{
			UpdateMotorCurrent(drive);
		}
	}
}

// Set the physical drive (i.e. axis or extruder) number used by this driver
void Platform::SetPhysicalDrive(size_t driverNumber, int8_t physicalDrive)
{
	int oldDrive = GetPhysicalDrive(driverNumber);
	if (oldDrive >= 0)
	{
		driverNumbers[oldDrive] = -1;
		stepPinDescriptors[oldDrive] = OutputPin();
	}

	driverNumbers[physicalDrive] = driverNumber;
	stepPinDescriptors[physicalDrive] = OutputPin(stepPins[driverNumber]);
}

// Return the physical drive used by this driver, or -1 if not found
int Platform::GetPhysicalDrive(size_t driverNumber) const
{
	for(size_t drive = 0; drive < DRIVES; ++drive)
	{
		if (driverNumbers[drive] == (int8_t)driverNumber)
		{
			return drive;
		}
	}
	return -1;
}

// Get current cooling fan speed on a scale between 0 and 1
float Platform::GetFanValue(size_t fan) const
{
	return (fan < NUM_FANS) ? fans[fan].val : -1.0;
}

bool Platform::GetCoolingInverted(size_t fan) const
{
	return (fan < NUM_FANS) ? fans[fan].inverted : false;
}

void Platform::SetCoolingInverted(size_t fan, bool inv)
{
	if (fan < NUM_FANS)
	{
		fans[fan].inverted = inv;
	}
}

// This is a bit of a compromise - old RepRaps used fan speeds in the range
// [0, 255], which is very hardware dependent.  It makes much more sense
// to specify speeds in [0.0, 1.0].  This looks at the value supplied (which
// the G Code reader will get right for a float or an int) and attempts to
// do the right thing whichever the user has done.
void Platform::SetFanValue(size_t fan, float speed)
{
	if (fan < NUM_FANS)
	{
		fans[fan].SetValue(speed);
	}
}

// Get current fan RPM
float Platform::GetFanRPM()
{
	// The ISR sets fanInterval to the number of microseconds it took to get fanMaxInterruptCount interrupts.
	// We get 2 tacho pulses per revolution, hence 2 interrupts per revolution.
	// However, if the fan stops then we get no interrupts and fanInterval stops getting updated.
	// We must recognise this and return zero.
	return (fanInterval != 0 && micros() - fanLastResetTime < 3000000U)		// if we have a reading and it is less than 3 second old
			? (float)((30000000U * fanMaxInterruptCount)/fanInterval)		// then calculate RPM assuming 2 interrupts per rev
			: 0.0;															// else assume fan is off or tacho not connected
}

void Platform::InitFans()
{
	for (size_t i = 0; i < NUM_FANS; ++i)
	{
		// The cooling fan 0 output pin gets inverted if HEAT_ON == 0 on a Duet 0.4, 0.6 or 0.7
		fans[i].Init(COOLING_FAN_PINS[i], !HEAT_ON && board != BoardType::Duet_085);
	}
	if (NUM_FANS > 1)
	{
		// Set fan 1 to be thermostatic by default, monitoring all heaters except the default bed heater
		fans[1].SetHeatersMonitored(0xFFFF & ~(1 << BED_HEATER));
	}

	coolingFanRpmPin = COOLING_FAN_RPM_PIN;
	lastRpmResetTime = 0.0;
	if (coolingFanRpmPin >= 0)
	{
		pinModeDuet(coolingFanRpmPin, INPUT_PULLUP, 1500);					// enable pullup and 1500Hz debounce filter (500Hz only worked up to 7000RPM)
	}
}

float Platform::GetFanPwmFrequency(size_t fan) const
{
	if (fan < NUM_FANS)
	{
		return (float)fans[fan].freq;
	}
	return 0.0;
}

void Platform::SetFanPwmFrequency(size_t fan, float freq)
{
	if (fan < NUM_FANS)
	{
		fans[fan].SetPwmFrequency(freq);
	}
}

float Platform::GetTriggerTemperature(size_t fan) const
{
	if (fan < NUM_FANS)
	{
		return fans[fan].triggerTemperature;
	}
	return ABS_ZERO;

}

void Platform::SetTriggerTemperature(size_t fan, float t)
{
	if (fan < NUM_FANS)
	{
		fans[fan].SetTriggerTemperature(t);
	}
}

uint16_t Platform::GetHeatersMonitored(size_t fan) const
{
	if (fan < NUM_FANS)
	{
		return fans[fan].heatersMonitored;
	}
	return 0;
}

void Platform::SetHeatersMonitored(size_t fan, uint16_t h)
{
	if (fan < NUM_FANS)
	{
		fans[fan].SetHeatersMonitored(h);
	}
}

void Platform::Fan::Init(Pin p_pin, bool hwInverted)
{
	val = 0.0;
	freq = DEFAULT_FAN_PWM_FREQUENCY;
	pin = p_pin;
	hardwareInverted = hwInverted;
	inverted = false;
	heatersMonitored = 0;
	triggerTemperature = HOT_END_FAN_TEMPERATURE;
	Refresh();
}

void Platform::Fan::SetValue(float speed)
{
	if (heatersMonitored == 0)
	{
		if (speed > 1.0)
		{
			speed /= 255.0;
		}
		val = constrain<float>(speed, 0.0, 1.0);
		Refresh();
	}
}

void Platform::Fan::Refresh()
{
	if (pin >= 0)
	{
		uint32_t p = (uint32_t)(255.0 * val);
		bool invert = hardwareInverted;
		if (inverted)
		{
			invert = !invert;
		}
		analogWriteDuet(pin, (invert) ? (255 - p) : p, freq);
	}
}

void Platform::Fan::SetPwmFrequency(float p_freq)
{
	freq = (uint16_t)max<float>(1.0, min<float>(65535.0, p_freq));
	Refresh();
}

void Platform::Fan::Check()
{
	if (heatersMonitored != 0)
	{
		val = (reprap.GetPlatform()->AnyHeaterHot(heatersMonitored, triggerTemperature)) ? 1.0 : 0.0;
		Refresh();
	}
}

//-----------------------------------------------------------------------------------------------------

FileStore* Platform::GetFileStore(const char* directory, const char* fileName, bool write)
{
	if (!fileStructureInitialised)
		return nullptr;

	for(size_t i = 0; i < MAX_FILES; i++)
	{
		if (!files[i]->inUse)
		{
			files[i]->inUse = true;
			if (files[i]->Open(directory, fileName, write))
			{
				return files[i];
			}
			else
			{
				files[i]->inUse = false;
				return nullptr;
			}
		}
	}
	Message(HOST_MESSAGE, "Max open file count exceeded.\n");
	return nullptr;
}

FileStore* Platform::GetFileStore(const char* filePath, bool write)
{
	return GetFileStore(nullptr, filePath, write);
}

MassStorage* Platform::GetMassStorage()
{
	return massStorage;
}

void Platform::Message(MessageType type, const char *message)
{
	switch (type)
	{
		case FLASH_LED:
			// Message that is to flash an LED; the next two bytes define
			// the frequency and M/S ratio.
			// (not implemented yet)
			break;

		case AUX_MESSAGE:
			// Message that is to be sent to the first auxiliary device
			if (!auxOutput->IsEmpty())
			{
				// If we're still busy sending a response to the UART device, append this message to the output buffer
				auxOutput->GetLastItem()->cat(message);
			}
			else
			{
				// Send short strings immediately through the aux channel. There is no flow control on this port, so it can't block for long
				SERIAL_AUX_DEVICE.write(message);
				SERIAL_AUX_DEVICE.flush();
			}
			break;

		case AUX2_MESSAGE:
			// Message that is to be sent to the second auxiliary device
			if (!aux2Output->IsEmpty())
			{
				// If we're still busy sending a response to the USART device, append this message to the output buffer
				aux2Output->GetLastItem()->cat(message);
			}
			else
			{
				// Send short strings immediately through the aux channel. There is no flow control on this port, so it can't block for long
				SERIAL_AUX2_DEVICE.write(message);
				SERIAL_AUX2_DEVICE.flush();
			}
			break;

		case DISPLAY_MESSAGE:
			// Message that is to appear on a local display;  \f and \n should be supported.
			reprap.SetMessage(message);
			break;

		case DEBUG_MESSAGE:
			// Debug messages in blocking mode - potentially DANGEROUS, use with care!
			SERIAL_MAIN_DEVICE.write(message);
			SERIAL_MAIN_DEVICE.flush();
			break;

		case HOST_MESSAGE:
			// Message that is to be sent via the USB line (non-blocking)
			{
				// Ensure we have a valid buffer to write to that isn't referenced for other destinations
				OutputBuffer *usbOutputBuffer = usbOutput->GetLastItem();
				if (usbOutputBuffer == nullptr || usbOutputBuffer->References() > 1)
				{
					if (!OutputBuffer::Allocate(usbOutputBuffer))
					{
						// Should never happen
						return;
					}
					usbOutput->Push(usbOutputBuffer);
				}

				// Check if we need to write the indentation chars first
				const size_t stackPointer = reprap.GetGCodes()->GetStackPointer();
				if (stackPointer > 0)
				{
					// First, make sure we get the indentation right
					char indentation[STACK * 2 + 1];
					for(size_t i = 0; i < stackPointer * 2; i++)
					{
						indentation[i] = ' ';
					}
					indentation[stackPointer * 2] = 0;

					// Append the indentation string
					usbOutputBuffer->cat(indentation);
				}

				// Append the message string
				usbOutputBuffer->cat(message);
			}
			break;

		case HTTP_MESSAGE:
		case TELNET_MESSAGE:
			// Message that is to be sent to the web
			{
				const WebSource source = (type == HTTP_MESSAGE) ? WebSource::HTTP : WebSource::Telnet;
				reprap.GetWebserver()->HandleGCodeReply(source, message);
			}
			break;

		case GENERIC_MESSAGE:
			// Message that is to be sent to the web & host. Make this the default one, too.
		default:
			Message(HTTP_MESSAGE, message);
			Message(TELNET_MESSAGE, message);
			Message(HOST_MESSAGE, message);
			break;
	}
}

void Platform::Message(const MessageType type, const StringRef &message)
{
	Message(type, message.Pointer());
}

void Platform::Message(const MessageType type, OutputBuffer *buffer)
{
	switch (type)
	{
		case AUX_MESSAGE:
			// If no AUX device is attached, don't queue this buffer
			if (!reprap.GetGCodes()->HaveAux())
			{
				OutputBuffer::ReleaseAll(buffer);
				break;
			}

			// For big responses it makes sense to write big chunks of data in portions. Store this data here
			auxOutput->Push(buffer);
			break;

		case AUX2_MESSAGE:
			// Send this message to the second UART device
			aux2Output->Push(buffer);
			break;

		case DEBUG_MESSAGE:
			// Probably rarely used, but supported.
			while (buffer != nullptr)
			{
				SERIAL_MAIN_DEVICE.write(buffer->Data(), buffer->DataLength());
				SERIAL_MAIN_DEVICE.flush();

				buffer = OutputBuffer::Release(buffer);
			}
			break;

		case HOST_MESSAGE:
			if (!SERIAL_MAIN_DEVICE)
			{
				// If the serial USB line is not open, discard the message right away
				OutputBuffer::ReleaseAll(buffer);
			}
			else
			{
				// Else append incoming data to the stack
				usbOutput->Push(buffer);
			}
			break;

		case HTTP_MESSAGE:
		case TELNET_MESSAGE:
			// Message that is to be sent to the web
			{
				const WebSource source = (type == HTTP_MESSAGE) ? WebSource::HTTP : WebSource::Telnet;
				reprap.GetWebserver()->HandleGCodeReply(source, buffer);
			}
			break;

		case GENERIC_MESSAGE:
			// Message that is to be sent to the web & host.
			buffer->IncreaseReferences(2);		// This one is handled by two additional destinations
			Message(HTTP_MESSAGE, buffer);
			Message(TELNET_MESSAGE, buffer);
			Message(HOST_MESSAGE, buffer);
			break;

		default:
			// Everything else is unsupported (and probably not used)
			OutputBuffer::ReleaseAll(buffer);
			MessageF(HOST_MESSAGE, "Warning: Unsupported Message call for type %u!\n", type);
			break;
	}
}

void Platform::MessageF(MessageType type, const char *fmt, va_list vargs)
{
	char formatBuffer[FORMAT_STRING_LENGTH];
	StringRef formatString(formatBuffer, ARRAY_SIZE(formatBuffer));
	formatString.vprintf(fmt, vargs);

	Message(type, formatBuffer);
}

void Platform::MessageF(MessageType type, const char *fmt, ...)
{
	char formatBuffer[FORMAT_STRING_LENGTH];
	StringRef formatString(formatBuffer, ARRAY_SIZE(formatBuffer));

	va_list vargs;
	va_start(vargs, fmt);
	formatString.vprintf(fmt, vargs);
	va_end(vargs);

	Message(type, formatBuffer);
}

bool Platform::AtxPower() const
{
	return (digitalRead(ATX_POWER_PIN) == HIGH);
}

void Platform::SetAtxPower(bool on)
{
	digitalWrite(ATX_POWER_PIN, (on) ? HIGH : LOW);
}

void Platform::SetElasticComp(size_t extruder, float factor)
{
	if (extruder < DRIVES - AXES)
	{
		elasticComp[extruder] = factor;
	}
}

float Platform::ActualInstantDv(size_t drive) const
{
	float idv = instantDvs[drive];
	if (drive >= AXES)
	{
		float eComp = elasticComp[drive - AXES];
		// If we are using elastic compensation then we need to limit the instantDv to avoid velocity mismatches
		return (eComp <= 0.0) ? idv : min<float>(idv, 1.0/(eComp * driveStepsPerUnit[drive]));
	}
	else
	{
		return idv;
	}
}

void Platform::SetBaudRate(size_t chan, uint32_t br)
{
	if (chan < NUM_SERIAL_CHANNELS)
	{
		baudRates[chan] = br;
		ResetChannel(chan);
	}
}

uint32_t Platform::GetBaudRate(size_t chan) const
{
	return (chan < NUM_SERIAL_CHANNELS) ? baudRates[chan] : 0;
}

void Platform::SetCommsProperties(size_t chan, uint32_t cp)
{
	if (chan < NUM_SERIAL_CHANNELS)
	{
		commsParams[chan] = cp;
		ResetChannel(chan);
	}
}

uint32_t Platform::GetCommsProperties(size_t chan) const
{
	return (chan < NUM_SERIAL_CHANNELS) ? commsParams[chan] : 0;
}


// Re-initialise a serial channel.
// Ideally, this would be part of the Line class. However, the Arduino core inexplicably fails to make the serial I/O begin() and end() members
// virtual functions of a base class, which makes that difficult to do.
void Platform::ResetChannel(size_t chan)
{
	switch(chan)
	{
		case 0:
			SERIAL_MAIN_DEVICE.end();
			SERIAL_MAIN_DEVICE.begin(baudRates[0]);
			break;
		case 1:
			SERIAL_AUX_DEVICE.end();
			SERIAL_AUX_DEVICE.begin(baudRates[1]);
			break;
		default:
			break;
	}
}

void Platform::SetBoardType(BoardType bt)
{
	if (bt == BoardType::Auto)
	{
		// Determine whether this is a Duet 0.6 or a Duet 0.8.5 board.
		// If it is a 0.85 board then DAC0 (AKA digital pin 67) is connected to ground via a diode and a 2.15K resistor.
		// So we enable the pullup (value 150K-150K) on pin 67 and read it, expecting a LOW on a 0.8.5 board and a HIGH on a 0.6 board.
		// This may fail if anyone connects a load to the DAC0 pin on and Duet 0.6, hence we implement board selection in M115 as well.
		pinMode(DAC0_DIGITAL_PIN, INPUT_PULLUP);
		board = (digitalRead(DAC0_DIGITAL_PIN)) ? BoardType::Duet_06 : BoardType::Duet_085;
		pinMode(DAC0_DIGITAL_PIN, INPUT);	// turn pullup off
	}
	else
	{
		board = bt;
	}

	if (active)
	{
		InitZProbe();							// select and initialise the Z probe modulation pin
		InitFans();								// select whether cooling is inverted or not
	}
}

// Get a string describing the electronics
const char* Platform::GetElectronicsString() const
{
	switch (board)
	{
		case BoardType::Duet_06:				return "Duet 0.6";
		case BoardType::Duet_07:				return "Duet 0.7";
		case BoardType::Duet_085:				return "Duet 0.85";
		default:								return "Unidentified";
	}
}

// Direct pin operations
// Get the specified pin input level. Return true if it's HIGH, false if not or if not allowed.
bool Platform::GetInputPin(int pin)
{
	if (pin >= 0 && (unsigned int)pin < NUM_PINS_ALLOWED)
	{
		const size_t index = (unsigned int)pin/8;
		const uint8_t mask = 1 << ((unsigned int)pin & 7);
		if ((pinAccessAllowed[index] & mask) != 0)
		{
			if ((pinInitialised[index] & mask) == 0)
			{
				pinMode(pin, INPUT);
				pinInitialised[index] |= mask;
			}
			return (digitalRead(pin) == HIGH);
		}
	}
	return false;
}

// Set the specified pin to the specified output level. Return true if success, false if not allowed.
bool Platform::SetOutputPin(int pin, int level)
{
	if (pin >= 0 && (unsigned int)pin < NUM_PINS_ALLOWED && (level == 0 || level == 1))
	{
		const size_t index = (unsigned int)pin/8;
		const uint8_t mask = 1 << ((unsigned int)pin & 7);
		if ((pinAccessAllowed[index] & mask) != 0)
		{
			if ((pinInitialised[index] & mask) == 0)
			{
				pinMode(pin, OUTPUT);
				pinInitialised[index] |= mask;
			}
			digitalWrite(pin, level);
			return true;
		}
	}
	return false;
}

// Fire the inkjet (if any) in the given pattern
// If there is no inkjet, false is returned; if there is one this returns true
// So you can test for inkjet presence with if(platform->Inkjet(0))
bool Platform::Inkjet(int bitPattern)
{
	if (inkjetBits < 0)
		return false;
	if (!bitPattern)
		return true;

	for(int8_t i = 0; i < inkjetBits; i++)
	{
		if (bitPattern & 1)
		{
			digitalWrite(inkjetSerialOut, 1);			// Write data to shift register

			for(int8_t j = 0; j <= i; j++)
			{
				digitalWrite(inkjetShiftClock, HIGH);
				digitalWrite(inkjetShiftClock, LOW);
				digitalWrite(inkjetSerialOut, 0);
			}

			digitalWrite(inkjetStorageClock, HIGH);		// Transfers data from shift register to output register
			digitalWrite(inkjetStorageClock, LOW);

			digitalWrite(inkjetOutputEnable, LOW);		// Fire the droplet out
			delayMicroseconds(inkjetFireMicroseconds);
			digitalWrite(inkjetOutputEnable, HIGH);

			digitalWrite(inkjetClear, LOW);				// Clear to 0
			digitalWrite(inkjetClear, HIGH);

			delayMicroseconds(inkjetDelayMicroseconds); // Wait for the next bit
		}

		bitPattern >>= 1; // Put the next bit in the units column
	}

	return true;
}

/*********************************************************************************

 Files & Communication

 */

MassStorage::MassStorage(Platform* p) : platform(p)
{
	memset(&fileSystem, 0, sizeof(FATFS));
	findDir = new DIR();
}

void MassStorage::Init()
{
	// Initialize SD MMC stack

	sd_mmc_init();
	delay(20);

	bool abort = false;
	sd_mmc_err_t err;
	do {
		err = sd_mmc_check(0);
		if (err > SD_MMC_ERR_NO_CARD)
		{
			abort = true;
			delay(3000);	// Wait a few seconds, so users have a chance to see the following error message
		}
		else
		{
			abort = (err == SD_MMC_ERR_NO_CARD && platform->Time() > 5.0);
		}

		if (abort)
		{
			platform->Message(HOST_MESSAGE, "Cannot initialise the SD card: ");
			switch (err)
			{
				case SD_MMC_ERR_NO_CARD:
					platform->Message(HOST_MESSAGE, "Card not found\n");
					break;
				case SD_MMC_ERR_UNUSABLE:
					platform->Message(HOST_MESSAGE, "Card is unusable, try another one\n");
					break;
				case SD_MMC_ERR_SLOT:
					platform->Message(HOST_MESSAGE, "Slot unknown\n");
					break;
				case SD_MMC_ERR_COMM:
					platform->Message(HOST_MESSAGE, "General communication error\n");
					break;
				case SD_MMC_ERR_PARAM:
					platform->Message(HOST_MESSAGE, "Illegal input parameter\n");
					break;
				case SD_MMC_ERR_WP:
					platform->Message(HOST_MESSAGE, "Card write protected\n");
					break;
				default:
					platform->MessageF(HOST_MESSAGE, "Unknown (code %d)\n", err);
					break;
			}
			return;
		}
	} while (err != SD_MMC_OK);

	// Print some card details (optional)

	/*platform->MessageF(HOST_MESSAGE, "SD card detected!\nCapacity: %d\n", sd_mmc_get_capacity(0));
	platform->MessageF(HOST_MESSAGE, "Bus clock: %d\n", sd_mmc_get_bus_clock(0));
	platform->MessageF(HOST_MESSAGE, "Bus width: %d\nCard type: ", sd_mmc_get_bus_width(0));
	switch (sd_mmc_get_type(0))
	{
		case CARD_TYPE_SD | CARD_TYPE_HC:
			platform->Message(HOST_MESSAGE, "SDHC\n");
			break;
		case CARD_TYPE_SD:
			platform->Message(HOST_MESSAGE, "SD\n");
			break;
		case CARD_TYPE_MMC | CARD_TYPE_HC:
			platform->Message(HOST_MESSAGE, "MMC High Density\n");
			break;
		case CARD_TYPE_MMC:
			platform->Message(HOST_MESSAGE, "MMC\n");
			break;
		case CARD_TYPE_SDIO:
			platform->Message(HOST_MESSAGE, "SDIO\n");
			return;
		case CARD_TYPE_SD_COMBO:
			platform->Message(HOST_MESSAGE, "SD COMBO\n");
			break;
		case CARD_TYPE_UNKNOWN:
		default:
			platform->Message(HOST_MESSAGE, "Unknown\n");
			return;
	}*/

	// Mount the file system

	int mounted = f_mount(0, &fileSystem);
	if (mounted != FR_OK)
	{
		platform->MessageF(HOST_MESSAGE, "Can't mount filesystem 0: code %d\n", mounted);
	}
}

const char* MassStorage::CombineName(const char* directory, const char* fileName)
{
	size_t out = 0;
	size_t in = 0;

	// DC 2015-11-25 Only prepend the directory if the filename does not have an absolute path
	if (directory != nullptr && fileName[0] != '/' && (strlen(fileName) < 3 || !isdigit(fileName[0]) || fileName[1] != ':' || fileName[2] != '/'))
	{
		while (directory[in] != 0 && directory[in] != '\n')
		{
			combinedName[out] = directory[in];
			in++;
			out++;
			if (out >= ARRAY_SIZE(combinedName))
			{
				platform->Message(GENERIC_MESSAGE, "Error: CombineName() buffer overflow.\n");
				out = 0;
			}
		}

		if (in > 0 && directory[in - 1] != '/' && out < ARRAY_UPB(combinedName))
		{
			combinedName[out] = '/';
			out++;
		}
		in = 0;
	}

	while (fileName[in] != 0 && fileName[in] != '\n')
	{
		combinedName[out] = fileName[in];
		in++;
		out++;
		if (out >= ARRAY_SIZE(combinedName))
		{
			platform->Message(GENERIC_MESSAGE, "Error: CombineName() buffer overflow.\n");
			out = 0;
		}
	}
	combinedName[out] = 0;

	return combinedName;
}

// Open a directory to read a file list. Returns true if it contains any files, false otherwise.
bool MassStorage::FindFirst(const char *directory, FileInfo &file_info)
{
	TCHAR loc[FILENAME_LENGTH];

	// Remove the trailing '/' from the directory name
	size_t len = strnlen(directory, ARRAY_UPB(loc));
	if (len == 0)
	{
		loc[0] = 0;
	}
	else if (directory[len - 1] == '/')
	{
		strncpy(loc, directory, len - 1);
		loc[len - 1] = 0;
	}
	else
	{
		strncpy(loc, directory, len);
		loc[len] = 0;
	}

	findDir->lfn = nullptr;
	FRESULT res = f_opendir(findDir, loc);
	if (res == FR_OK)
	{
		FILINFO entry;
		entry.lfname = file_info.fileName;
		entry.lfsize = ARRAY_SIZE(file_info.fileName);

		for(;;)
		{
			res = f_readdir(findDir, &entry);
			if (res != FR_OK || entry.fname[0] == 0) break;
			if (StringEquals(entry.fname, ".") || StringEquals(entry.fname, "..")) continue;

			file_info.isDirectory = (entry.fattrib & AM_DIR);
			file_info.size = entry.fsize;
			uint16_t day = entry.fdate & 0x1F;
			if (day == 0)
			{
				// This can happen if a transfer hasn't been processed completely.
				day = 1;
			}
			file_info.day = day;
			file_info.month = (entry.fdate & 0x01E0) >> 5;
			file_info.year = (entry.fdate >> 9) + 1980;
			if (file_info.fileName[0] == 0)
			{
				strncpy(file_info.fileName, entry.fname, ARRAY_SIZE(file_info.fileName));
			}

			return true;
		}
	}

	return false;
}

// Find the next file in a directory. Returns true if another file has been read.
bool MassStorage::FindNext(FileInfo &file_info)
{
	FILINFO entry;
	entry.lfname = file_info.fileName;
	entry.lfsize = ARRAY_SIZE(file_info.fileName);

	findDir->lfn = nullptr;
	if (f_readdir(findDir, &entry) != FR_OK || entry.fname[0] == 0)
	{
		//f_closedir(findDir);
		return false;
	}

	file_info.isDirectory = (entry.fattrib & AM_DIR);
	file_info.size = entry.fsize;
	uint16_t day = entry.fdate & 0x1F;
	if (day == 0)
	{
		// This can happen if a transfer hasn't been processed completely.
		day = 1;
	}
	file_info.day = day;
	file_info.month = (entry.fdate & 0x01E0) >> 5;
	file_info.year = (entry.fdate >> 9) + 1980;
	if (file_info.fileName[0] == 0)
	{
		strncpy(file_info.fileName, entry.fname, ARRAY_SIZE(file_info.fileName));
	}

	return true;
}

// Month names. The first entry is used for invalid month numbers.
static const char *monthNames[13] = { "???", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Returns the name of the specified month or '???' if the specified value is invalid.
const char* MassStorage::GetMonthName(const uint8_t month)
{
	return (month <= 12) ? monthNames[month] : monthNames[0];
}

// Delete a file or directory
bool MassStorage::Delete(const char* directory, const char* fileName)
{
	const char* location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, fileName)
								: fileName;
	if (f_unlink(location) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't delete file %s\n", location);
		return false;
	}
	return true;
}

// Create a new directory
bool MassStorage::MakeDirectory(const char *parentDir, const char *dirName)
{
	const char* location = platform->GetMassStorage()->CombineName(parentDir, dirName);
	if (f_mkdir(location) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't create directory %s\n", location);
		return false;
	}
	return true;
}

bool MassStorage::MakeDirectory(const char *directory)
{
	if (f_mkdir(directory) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't create directory %s\n", directory);
		return false;
	}
	return true;
}

// Rename a file or directory
bool MassStorage::Rename(const char *oldFilename, const char *newFilename)
{
	if (f_rename(oldFilename, newFilename) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't rename file or directory %s to %s\n", oldFilename, newFilename);
		return false;
	}
	return true;
}

// Check if the specified file exists
bool MassStorage::FileExists(const char *file) const
{
 	FILINFO fil;
 	fil.lfname = nullptr;
	return (f_stat(file, &fil) == FR_OK);
}

bool MassStorage::FileExists(const char *directory, const char *fileName) const
{
	const char *location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, fileName)
							: fileName;
	return FileExists(location);
}

// Check if the specified directory exists
bool MassStorage::DirectoryExists(const char *path) const
{
 	DIR dir;
 	dir.lfn = nullptr;
	return (f_opendir(&dir, path) == FR_OK);
}

bool MassStorage::DirectoryExists(const char* directory, const char* subDirectory)
{
	return DirectoryExists(CombineName(directory, subDirectory));
}

//------------------------------------------------------------------------------------------------

FileStore::FileStore(Platform* p) : platform(p)
{
}

void FileStore::Init()
{
	bufferPointer = 0;
	inUse = false;
	writing = false;
	lastBufferEntry = 0;
	openCount = 0;
}

// Open a local file (for example on an SD card).
// This is protected - only Platform can access it.

bool FileStore::Open(const char* directory, const char* fileName, bool write)
{
	const char *location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, fileName)
							: fileName;
	writing = write;
	lastBufferEntry = FILE_BUFFER_SIZE - 1;
	bytesRead = 0;

	FRESULT openReturn = f_open(&file, location, (writing) ? FA_CREATE_ALWAYS | FA_WRITE : FA_OPEN_EXISTING | FA_READ);
	if (openReturn != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't open %s to %s, error code %d\n", location, (writing) ? "write" : "read", openReturn);
		return false;
	}

	bufferPointer = (writing) ? 0 : FILE_BUFFER_SIZE;
	inUse = true;
	openCount = 1;
	return true;
}

void FileStore::Duplicate()
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to dup a non-open file.\n");
		return;
	}
	++openCount;
}

bool FileStore::Close()
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to close a non-open file.\n");
		return false;
	}
	--openCount;
	if (openCount != 0)
	{
		return true;
	}
	bool ok = true;
	if (writing)
	{
		ok = Flush();
	}
	FRESULT fr = f_close(&file);
	inUse = false;
	writing = false;
	lastBufferEntry = 0;
	return ok && fr == FR_OK;
}

FilePosition FileStore::Position() const
{
	return bytesRead;
}

bool FileStore::Seek(FilePosition pos)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to seek on a non-open file.\n");
		return false;
	}
	if (writing)
	{
		WriteBuffer();
	}
	FRESULT fr = f_lseek(&file, pos);
	if (fr == FR_OK)
	{
		bufferPointer = (writing) ? 0 : FILE_BUFFER_SIZE;
		bytesRead = pos;
		return true;
	}
	return false;
}

bool FileStore::GoToEnd()
{
	return Seek(Length());
}

FilePosition FileStore::Length() const
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to size non-open file.\n");
		return 0;
	}
	return file.fsize;
}

float FileStore::FractionRead() const
{
	FilePosition len = Length();
	if (len <= 0)
	{
		return 0.0;
	}

	return (float)bytesRead / (float)len;
}

IOStatus FileStore::Status() const
{
	if (!inUse)
		return IOStatus::nothing;

	if (lastBufferEntry == FILE_BUFFER_SIZE)
		return IOStatus::byteAvailable;

	if (bufferPointer < (int)lastBufferEntry)
		return IOStatus::byteAvailable;

	return IOStatus::nothing;
}

bool FileStore::ReadBuffer()
{
	FRESULT readStatus = f_read(&file, buf, FILE_BUFFER_SIZE, &lastBufferEntry);	// Read a chunk of file
	if (readStatus)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Cannot read file.\n");
		return false;
	}
	bufferPointer = 0;
	return true;
}

// Single character read via the buffer
bool FileStore::Read(char& b)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to read from a non-open file.\n");
		return false;
	}

	if (bufferPointer >= (int)FILE_BUFFER_SIZE)
	{
		bool ok = ReadBuffer();
		if (!ok)
		{
			return false;
		}
	}

	if (bufferPointer >= (int)lastBufferEntry)
	{
		b = 0;  // Good idea?
		return false;
	}

	b = (char) buf[bufferPointer];
	bufferPointer++;
	bytesRead++;

	return true;
}

// Block read, doesn't use the buffer
// Returns -1 if the read process failed
int FileStore::Read(char* extBuf, unsigned int nBytes)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to read from a non-open file.\n");
		return -1;
	}

	size_t numBytesRead;
	FRESULT readStatus = f_read(&file, extBuf, nBytes, &numBytesRead);
	if (readStatus)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Cannot read file.\n");
		return -1;
	}
	bytesRead += numBytesRead;

	return numBytesRead;
}

bool FileStore::WriteBuffer()
{
	if (bufferPointer != 0)
	{
		bool ok = InternalWriteBlock((const char*)buf, bufferPointer);
		if (!ok)
		{
			platform->Message(GENERIC_MESSAGE, "Error: Cannot write to file. Disc may be full.\n");
			return false;
		}
		bufferPointer = 0;
	}
	return true;
}

bool FileStore::Write(char b)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to write byte to a non-open file.\n");
		return false;
	}
	buf[bufferPointer] = b;
	bufferPointer++;
	if (bufferPointer >= (int)FILE_BUFFER_SIZE)
	{
		return WriteBuffer();
	}
	return true;
}

bool FileStore::Write(const char* b)
{
	return Write(b, strlen(b));
}

// Direct block write that bypasses the buffer. Used when uploading files.
bool FileStore::Write(const char *s, unsigned int len)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to write block to a non-open file.\n");
		return false;
	}
	if (!WriteBuffer())
	{
		return false;
	}
	return InternalWriteBlock(s, len);
}

bool FileStore::InternalWriteBlock(const char *s, unsigned int len)
{
 	unsigned int bytesWritten;
	uint32_t time = micros();
 	FRESULT writeStatus = f_write(&file, s, len, &bytesWritten);
	time = micros() - time;
	if (time > longestWriteTime)
	{
		longestWriteTime = time;
	}
 	if ((writeStatus != FR_OK) || (bytesWritten != len))
 	{
 		platform->Message(GENERIC_MESSAGE, "Error: Cannot write to file. Disc may be full.\n");
 		return false;
 	}
 	return true;
 }

bool FileStore::Flush()
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to flush a non-open file.\n");
		return false;
	}
	if (!WriteBuffer())
	{
		return false;
	}
	return f_sync(&file) == FR_OK;
}

float FileStore::GetAndClearLongestWriteTime()
{
	float ret = (float)longestWriteTime/1000.0;
	longestWriteTime = 0;
	return ret;
}

uint32_t FileStore::longestWriteTime = 0;

//------------------------------------------------------------------------------------------------

// Build a short-form pin descriptor for a IO pin
OutputPin::OutputPin(unsigned int pin)
{
	const PinDescription& pinDesc = g_APinDescription[pin];
	pPort = pinDesc.pPort;
	ulPin = pinDesc.ulPin;
}

//------------------------------------------------------------------------------------------------

// Pragma pop_options is not supported on this platform, so we put this time-critical code right at the end of the file
//#pragma GCC push_options
#pragma GCC optimize ("O3")

// Schedule an interrupt at the specified clock count, or return true if that time is imminent or has passed already.
// Must be called with interrupts disabled,
/*static*/ bool Platform::ScheduleInterrupt(uint32_t tim)
{
	TC_SetRA(TC1, 0, tim);									// set up the compare register
	TC_GetStatus(TC1, 0);									// clear any pending interrupt
	int32_t diff = (int32_t)(tim - TC_ReadCV(TC1, 0));		// see how long we have to go
	if (diff < (int32_t)DDA::minInterruptInterval)			// if less than about 2us or already passed
	{
		return true;										// tell the caller to simulate an interrupt instead
	}

	TC1 ->TC_CHANNEL[0].TC_IER = TC_IER_CPAS;				// enable the interrupt
#ifdef MOVE_DEBUG
		++numInterruptsScheduled;
		nextInterruptTime = tim;
		nextInterruptScheduledAt = Platform::GetInterruptClocks();
#endif
	return false;
}

// Process a 1ms tick interrupt
// This function must be kept fast so as not to disturb the stepper timing, so don't do any floating point maths in here.
// This is what we need to do:
// 0.  Kick the watchdog.
// 1.  Kick off a new ADC conversion.
// 2.  Fetch and process the result of the last ADC conversion.
// 3a. If the last ADC conversion was for the Z probe, toggle the modulation output if using a modulated IR sensor.
// 3b. If the last ADC reading was a thermistor reading, check for an over-temperature situation and turn off the heater if necessary.
//     We do this here because the usual polling loop sometimes gets stuck trying to send data to the USB port.

//#define TIME_TICK_ISR	1		// define this to store the tick ISR time in errorCodeBits

void Platform::Tick()
{
#ifdef TIME_TICK_ISR
	uint32_t now = micros();
#endif
	switch (tickState)
	{
	case 1:			// last conversion started was a thermistor
	case 3:
		{
			if (DoThermistorAdc(currentHeater))
			{
				ThermistorAveragingFilter& currentFilter = const_cast<ThermistorAveragingFilter&>(thermistorFilters[currentHeater]);
				currentFilter.ProcessReading(GetAdcReading(thermistorAdcChannels[heaterTempChannels[currentHeater]]));
				StartAdcConversion(zProbeAdcChannel);
				if (currentFilter.IsValid())
				{
					uint32_t sum = currentFilter.GetSum();
					if (sum < thermistorOverheatSums[currentHeater] || sum >= AD_DISCONNECTED_REAL * THERMISTOR_AVERAGE_READINGS)
					{
						// We have an over-temperature or disconnected reading from this thermistor, so turn off the heater
						SetHeaterPwm(currentHeater, 0);
						RecordError(ErrorCode::BadTemp);
					}
				}
			}
			else
			{
				// Thermocouple case: oversampling is not necessary as the MAX31855 is itself continuously sampling and
				// averaging.  As such, the temperature reading is taken directly by Platform::GetTemperature() and
				// periodically called by PID::Spin() where temperature fault handling is taken care of.  However, we
				// must guard against overly long delays between successive calls of PID::Spin().

				StartAdcConversion(zProbeAdcChannel);
				if ((Time() - reprap.GetHeat()->GetLastSampleTime(currentHeater)) > maxPidSpinDelay)
				{
					SetHeaterPwm(currentHeater, 0);
					RecordError(ErrorCode::BadTemp);
				}
			}

			++currentHeater;
			if (currentHeater >= reprap.GetHeatersInUse())
			{
				currentHeater = 0;
			}
		}
		++tickState;
		break;

	case 2:			// last conversion started was the Z probe, with IR LED on
		const_cast<ZProbeAveragingFilter&>(zProbeOnFilter).ProcessReading(GetRawZProbeReading());
		if (DoThermistorAdc(currentHeater))
		{
			StartAdcConversion(thermistorAdcChannels[heaterTempChannels[currentHeater]]);		// read a thermistor
		}
		if (nvData.zProbeType == 2)									// if using a modulated IR sensor
		{
			digitalWrite(zProbeModulationPin, LOW);			// turn off the IR emitter
		}
		++tickState;
		break;

	case 4:			// last conversion started was the Z probe, with IR LED off if modulation is enabled
		const_cast<ZProbeAveragingFilter&>(zProbeOffFilter).ProcessReading(GetRawZProbeReading());
		// no break
	case 0:			// this is the state after initialisation, no conversion has been started
	default:
		if (DoThermistorAdc(currentHeater))
		{
			StartAdcConversion(thermistorAdcChannels[heaterTempChannels[currentHeater]]);		// read a thermistor
		}
		if (nvData.zProbeType == 2)									// if using a modulated IR sensor
		{
			digitalWrite(zProbeModulationPin, HIGH);			// turn on the IR emitter
		}
		tickState = 1;
		break;
	}
#ifdef TIME_TICK_ISR
	uint32_t now2 = micros();
	if (now2 - now > errorCodeBits)
	{
		errorCodeBits = now2 - now;
	}
#endif
}

/*static*/ uint16_t Platform::GetAdcReading(adc_channel_num_t chan)
{
	uint16_t rslt = (uint16_t) adc_get_channel_value(ADC, chan);
	adc_disable_channel(ADC, chan);
	return rslt;
}

/*static*/ void Platform::StartAdcConversion(adc_channel_num_t chan)
{
	adc_enable_channel(ADC, chan);
	adc_start(ADC);
}

// Pragma pop_options is not supported on this platform
//#pragma GCC pop_options

// vim: ts=4:sw=4
