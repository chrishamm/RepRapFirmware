/****************************************************************************************************

RepRapFirmware - Configuration

This is where all machine-independent configuration and other definitions are set up.  Nothing that
depends on any particular RepRap, RepRap component, or RepRap controller  should go in here. Define
machine-dependent things in Platform.h

-----------------------------------------------------------------------------------------------------

Version 0.1

18 November 2012

Adrian Bowyer
RepRap Professional Ltd
http://reprappro.com

Licence: GPL

****************************************************************************************************/

#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#define NAME "RepRapFirmware"
#define VERSION "1.10-b3-ch"
#define DATE "2016-03-23"

#define AUTHORS "reprappro, dc42, chrishamm, t3p3, dnewman"

// Comment out the following line if you don't want to build the firmware with Flash save support
#define FLASH_SAVE_ENABLED

// If enabled, the following control the use of the optional ExternalDrivers module
//#define EXTERNAL_DRIVERS		(1)
//#define FIRST_EXTERNAL_DRIVE	(4)

// Other firmware that we might switch to be compatible with.

enum Compatibility
{
	me = 0,
	reprapFirmware = 1,
	marlin = 2,
	teacup = 3,
	sprinter = 4,
	repetier = 5
};

// Generic constants

const float ABS_ZERO = -273.15;							// Celsius
const float NEARLY_ABS_ZERO = -273.0;					// Celsius
const float ROOM_TEMPERATURE = 21.0;					// Celsius

const float INCH_TO_MM = 25.4;
const float MINUTES_TO_SECONDS = 60.0;
const float SECONDS_TO_MINUTES = 1.0 / MINUTES_TO_SECONDS;

const float LONG_TIME = 300.0;							// Seconds
const float MINIMUM_TOOL_WARNING_INTERVAL = 4.0;		// Seconds

// Heater values

const float HEAT_SAMPLE_TIME = 0.5;						// Seconds
const float HEAT_PWM_AVERAGE_TIME = 5.0;				// Seconds

const float TEMPERATURE_CLOSE_ENOUGH = 2.5;				// Celsius
const float TEMPERATURE_LOW_SO_DONT_CARE = 40.0;		// Celsius
const float HOT_ENOUGH_TO_EXTRUDE = 160.0;				// Celsius
const float HOT_ENOUGH_TO_RETRACT = 90.0;				// Celsius
const float TIME_TO_HOT = 150.0;						// Seconds

const uint8_t MAX_BAD_TEMPERATURE_COUNT = 4;			// Number of bad temperature samples permitted before a heater fault is reported
const float BAD_LOW_TEMPERATURE = -10.0;				// Celsius
const float DEFAULT_TEMPERATURE_LIMIT = 300.0;			// Celsius
const float HOT_END_FAN_TEMPERATURE = 45.0;				// Temperature at which a thermostatic hot end fan comes on
const float BAD_ERROR_TEMPERATURE = 2000.0;				// Must exceed DEFAULT_TEMPERATURE_LIMIT

const unsigned int SLOW_HEATER_PWM_FREQUENCY = 10;		// Hz
const unsigned int NORMAL_HEATER_PWM_FREQUENCY = 500;	// Hz
const unsigned int DEFAULT_FAN_PWM_FREQUENCY = 500;		// Hz (increase to 25kHz for 4-wire PWM fans)

// Default Z probe values

const size_t MAX_PROBE_POINTS = 16;						// Maximum number of probe points

const float DEFAULT_Z_DIVE = 5.0;						// Millimetres
const float DEFAULT_PROBE_SPEED = 2.0;					// Default Z probing speed
const float DEFAULT_TRAVEL_SPEED = 100.0;				// Default speed for travel to probe points

const float TRIANGLE_ZERO = -0.001;						// Millimetres
const float SILLY_Z_VALUE = -9999.0;					// Millimetres

// String lengths

const size_t LONG_STRING_LENGTH = 1024;
const size_t FORMAT_STRING_LENGTH = 256;
const size_t SHORT_STRING_LENGTH = 40;

const size_t GCODE_LENGTH = 100;
const size_t GCODE_REPLY_LENGTH = 2048;
const size_t MESSAGE_LENGTH = 256;

// Output buffer lengths

const uint16_t OUTPUT_BUFFER_SIZE = 256;				// How many bytes does each OutputBuffer hold?
const size_t OUTPUT_BUFFER_COUNT = 16;					// How many OutputBuffer instances do we have?

// Move system

const float DEFAULT_FEEDRATE = 3000.0;					// The initial requested feed rate after resetting the printer
const float DEFAULT_IDLE_TIMEOUT = 30.0;				// Seconds

// Default nozzle and filament values

const float NOZZLE_DIAMETER = 0.5;						// Millimetres
const float FILAMENT_WIDTH = 1.75;						// Millimetres

// Webserver defaults

#define DEFAULT_PASSWORD "reprap"						// Default machine password
#define DEFAULT_NAME "My Duet"							// Default machine name
#define HOSTNAME "duet"

#define INDEX_PAGE_FILE "reprap.htm"
#define FOUR04_PAGE_FILE "html404.htm"

// Filesystem and upload defaults

#define FS_PREFIX "0:"
#define GCODE_DIR "0:/gcodes/"							// Place to find G-Code files on the SD card
#define SYS_DIR "0:/sys/"								// Ditto - System files
#define MACRO_DIR "0:/macros/"							// Ditto - Macro files
#define WEB_DIR "0:/www/"								// Ditto - Web files

#define CONFIG_FILE "config.g"
#define DEFAULT_FILE "default.g"
#define HOME_X_G "homex.g"
#define HOME_Y_G "homey.g"
#define HOME_Z_G "homez.g"
#define HOME_ALL_G "homeall.g"
#define HOME_DELTA_G "homedelta.g"
#define BED_EQUATION_G "bed.g"
#define PAUSE_G "pause.g"
#define RESUME_G "resume.g"
#define STOP_G "stop.g"
#define SLEEP_G "sleep.g"

#define EOF_STRING "<!-- **EoF** -->"

#define IAP_UPDATE_FILE "iap.bin"
#define IAP_FIRMWARE_FILE "RepRapFirmware.bin"

// List defaults

const char LIST_SEPARATOR = ':';
const char FILE_LIST_SEPARATOR = ',';
const char FILE_LIST_BRACKET = '"';

#endif

// vim: ts=4:sw=4
