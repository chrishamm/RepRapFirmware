# Makefile for RepRapFirmware-DuetNG (SAM4E8E)
# written by Christian Hammacher
#
# Licensed under the terms of the GNU Public License v2
# see http://www.gnu.org/licenses/gpl-2.0.html
#

# Workspace directories
BUILD_PATH := $(PWD)/../Release/obj-duetNG
OUTPUT_PATH := $(PWD)/../Release

# Firmware port for 1200bps touch
PRIMARY_PORT := /dev/ttyACM0
SECONDARYL_PORT := /dev/ttyACM1


# Set Arduino Duet core options
DRIVERS := wdt usart udp uart twi tc supc spi rtt rtc rstc pwm pmc pio pdc matrix hsmci gpbr efc dmac dacc chipid can afec
INCLUDES := -I"$(DUET_BOARD_PATH)/cores/arduino" -I"$(DUET_BOARD_PATH)/asf" -I"$(DUET_BOARD_PATH)/asf/sam/utils" -I"$(DUET_BOARD_PATH)/asf/sam/utils/header_files" -I"$(DUET_BOARD_PATH)/asf/sam/utils/preprocessor" -I"$(DUET_BOARD_PATH)/asf/sam/utils/cmsis/sam4e/include" -I"$(DUET_BOARD_PATH)/asf/sam/drivers" $(foreach driver,$(DRIVERS),-I"$(DUET_BOARD_PATH)/asf/sam/drivers/$(driver)") -I"$(DUET_BOARD_PATH)/asf/sam/services/flash_efc" -I"$(DUET_BOARD_PATH)/asf/common/utils" -I"$(DUET_BOARD_PATH)/asf/common/services/clock" -I"$(DUET_BOARD_PATH)/asf/common/services/ioport" -I"$(DUET_BOARD_PATH)/asf/common/services/sleepmgr" -I"$(DUET_BOARD_PATH)/asf/common/services/usb" -I"$(DUET_BOARD_PATH)/asf/common/services/usb/udc" -I"$(DUET_BOARD_PATH)/asf/common/services/usb/class/cdc" -I"$(DUET_BOARD_PATH)/asf/common/services/usb/class/cdc/device" -I"$(DUET_BOARD_PATH)/asf/thirdparty/CMSIS/Include" -I"$(DUET_BOARD_PATH)/variants/duetNG"
INCLUDES += -I"$(DUET_LIBRARY_PATH)/SharedSpi" -I"$(DUET_LIBRARY_PATH)/Storage" -I"$(DUET_LIBRARY_PATH)/Wire"

# Set board options
INCLUDES += -I"$(PWD)" -I"$(PWD)/DuetNG"
INCLUDES += -I"$(PWD)/Libraries/Fatfs" -I"$(PWD)/Libraries/Flash" -I"$(PWD)/Libraries/MCP4461" -I"$(PWD)/Libraries/TemperatureSensor" -I"$(PWD)/Libraries/sha1"

# Get source files
VPATH := $(PWD) $(PWD)/DuetNG
VPATH += $(PWD)/Libraries/Fatfs $(PWD)/Libraries/Flash $(PWD)/Libraries/MCP4461 $(PWD)/Libraries/TemperatureSensor $(PWD)/Libraries/sha1

C_SOURCES += $(foreach dir,$(VPATH),$(wildcard $(dir)/*.c)) $(wildcard $(PWD)/*.c)
CPP_SOURCES := $(foreach dir,$(VPATH),$(wildcard $(dir)/*.cpp)) $(wildcard $(PWD)/*.cpp)

C_OBJS := $(foreach src,$(C_SOURCES),$(BUILD_PATH)/$(notdir $(src:.c=.c.o)))
CPP_OBJS := $(foreach src,$(CPP_SOURCES),$(BUILD_PATH)/$(notdir $(src:.cpp=.cpp.o)))

DEPS := $(C_OBJS:%.o=%.d) $(CPP_OBJS:%.o=%.d)

# Set GCC options
CFLAGS := -DVERSION=\"$(VERSION)\" -DDATE=\"$(DATE)\" -D__SAM4E8E__ -DDUET_NG -Dprintf=iprintf
CFLAGS += -Wall -c -std=gnu11 -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -ffunction-sections -fdata-sections -nostdlib --param max-inline-insns-single=500 -MMD -MP
CPPFLAGS := -DVERSION=\"$(VERSION)\" -DDATE=\"$(DATE)\" -D__SAM4E8E__ -DDUET_NG -Dprintf=iprintf
CPPFLAGS += -Wall -c -std=gnu++11 -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -ffunction-sections -fdata-sections -fno-threadsafe-statics -fno-rtti -fno-exceptions -nostdlib --param max-inline-insns-single=500 -MMD -MP


FORCESYM := -u _sbrk -u link -u _close -u _fstat -u _isatty -u _lseek -u _read -u _write -u _exit -u kill -u _getpid
LDFLAGS := -L"$(DUET_BOARD_PATH)/variants/duetNG" $(OPTIMISATION) -Wl,--gc-sections -Wl,--fatal-warnings -mcpu=cortex-m4 -T"$(DUET_BOARD_PATH)/variants/duetNG/linker_scripts/gcc/flash.ld" -Wl,-Map,"$(OUTPUT_PATH)/RepRapFirmware-DuetNG.map" -o "$(OUTPUT_PATH)/RepRapFirmware-DuetNG.elf" -mthumb -Wl,--cref -Wl,--check-sections -Wl,--gc-sections -Wl,--entry=Reset_Handler -Wl,--unresolved-symbols=report-all -Wl,--warn-common -Wl,--warn-section-align -Wl,--warn-unresolved-symbols -Wl,--start-group $(FORCESYM) $(BUILD_PATH)/*.o -lDuetNG -Wl,--end-group -lm -gcc


# ================================= Target all ======================================
.PHONY += all
all: $(OUTPUT_PATH)/RepRapFirmware-DuetNG-$(VERSION).bin
$(OUTPUT_PATH)/RepRapFirmware-DuetNG-$(VERSION).bin: $(OUTPUT_PATH)/RepRapFirmware-DuetNG-$(VERSION).elf
	@echo "  BIN     ../Release/RepRapFirmware-DuetNG-$(VERSION).bin"
	@$(OBJCOPY) -O binary $(OUTPUT_PATH)/RepRapFirmware-DuetNG-$(VERSION).elf $(OUTPUT_PATH)/RepRapFirmware-DuetNG-$(VERSION).bin

$(OUTPUT_PATH)/RepRapFirmware-DuetNG-$(VERSION).elf: $(BUILD_PATH) $(OUTPUT_PATH) $(C_OBJS) $(CPP_OBJS)
	@echo "  LD      ../Release/RepRapFirmware-DuetNG-$(VERSION).elf"
	@$(LD) $(LDFLAGS) -o $(OUTPUT_PATH)/RepRapFirmware-DuetNG-$(VERSION).elf
-include $(DEPS)

$(BUILD_PATH)/%.c.o: %.c
	@echo "  CC      $(subst $(PWD)/,,$<)"
	@$(CC) $(CFLAGS) $(OPTIMISATION) $(INCLUDES) $< -o $@

$(BUILD_PATH)/%.cpp.o: %.cpp
	@echo "  CC      $(subst $(PWD)/,,$<)"
	@$(CXX) $(CPPFLAGS) $(OPTIMISATION) $(INCLUDES) $< -o $@

$(BUILD_PATH):
	@mkdir -p $(BUILD_PATH)

$(OUTPUT_PATH):
	@mkdir -p $(OUTPUT_PATH)


# ================================= Target clean ====================================
.PHONY += clean
clean:
	@rm -rf $(BUILD_PATH) $(OUTPUT_PATH)
	$(info DuetNG build directories removed.)


# ================================= Target upload ===================================
.PHONY += upload
upload: $(OUTPUT_PATH)/RepRapFirmware-DuetNG-$(VERSION).bin
	@echo "=> Rebooting hardware into bootloader mode..."
	@stty -F $(PRIMARY_PORT) 115200 -ixon crtscts || true
	@echo -n "M999 PERASE" > $(PRIMARY_PORT) || true
	@sleep 1
	@echo "=> Flashing new firmware binary..."
	@test -s $(PRIMARY_PORT) || $(BOSSAC_4E_PATH) $(PRIMARY_PORT) at91sam4e8-ek ./DuetNG/flash.txt || true
	@test -s $(SECONDARY_PORT) || $(BOSSAC_4E_PATH) $(SECONDARY_PORT) at91sam4e8-ek ./DuetNG/flash.txt || true
	@echo "=> Flashing complete! Reset your board to boot the new firmware."
