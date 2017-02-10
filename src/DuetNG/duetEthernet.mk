# Makefile for RepRapFirmware (Duet Ethernet, SAM4E8E)
# written by Christian Hammacher
#
# Licensed under the terms of the GNU Public License v2
# see http://www.gnu.org/licenses/gpl-2.0.html
#

# Workspace directories
BUILD_PATH := $(PWD)/../Release/Duet-Ethernet/obj
OUTPUT_PATH := $(PWD)/../Release/Duet-Ethernet

# Firmware port for 1200bps touch
PRIMARY_PORT := /dev/ttyACM0
SECONDARY_PORT := /dev/ttyACM1


# Set Arduino Duet core options
DRIVERS := wdt usart udp uart twi tc supc spi rtt rtc rstc pwm pmc pio pdc matrix hsmci gpbr efc dmac dacc chipid can afec
INCLUDES := -I"$(DUET_BOARD_PATH)/cores/arduino" -I"$(DUET_BOARD_PATH)/asf" -I"$(DUET_BOARD_PATH)/asf/sam/utils" -I"$(DUET_BOARD_PATH)/asf/sam/utils/header_files" -I"$(DUET_BOARD_PATH)/asf/sam/utils/preprocessor" -I"$(DUET_BOARD_PATH)/asf/sam/utils/cmsis/sam4e/include" -I"$(DUET_BOARD_PATH)/asf/sam/drivers" $(foreach driver,$(DRIVERS),-I"$(DUET_BOARD_PATH)/asf/sam/drivers/$(driver)") -I"$(DUET_BOARD_PATH)/asf/sam/services/flash_efc" -I"$(DUET_BOARD_PATH)/asf/common/utils" -I"$(DUET_BOARD_PATH)/asf/common/services/clock" -I"$(DUET_BOARD_PATH)/asf/common/services/ioport" -I"$(DUET_BOARD_PATH)/asf/common/services/sleepmgr" -I"$(DUET_BOARD_PATH)/asf/common/services/usb" -I"$(DUET_BOARD_PATH)/asf/common/services/usb/udc" -I"$(DUET_BOARD_PATH)/asf/common/services/usb/class/cdc" -I"$(DUET_BOARD_PATH)/asf/common/services/usb/class/cdc/device" -I"$(DUET_BOARD_PATH)/asf/thirdparty/CMSIS/Include" -I"$(DUET_BOARD_PATH)/variants/duetNG"
INCLUDES += -I"$(DUET_LIBRARY_PATH)/Flash" -I"$(DUET_LIBRARY_PATH)/RTCDue" -I"$(DUET_LIBRARY_PATH)/SharedSpi" -I"$(DUET_LIBRARY_PATH)/Storage" -I"$(DUET_LIBRARY_PATH)/Wire"

# Set board options
INCLUDES += -I"$(PWD)" -I"$(PWD)/DuetNG" -I"$(PWD)/DuetNG/DuetEthernet" -I"$(PWD)/DuetNG/DuetEthernet/Wiznet/Ethernet" -I"$(PWD)/Webserver"

# Get source files
VPATH := $(PWD) $(PWD)/DuetNG $(PWD)/DuetNG/DuetEthernet $(PWD)/DuetNG/DuetEthernet/Wiznet/Ethernet $(PWD)/DuetNG/DuetEthernet/Wiznet/Ethernet/W5500 $(PWD)/DuetNG/DuetEthernet/Wiznet/Internet/DHCP $(PWD)/GCodes $(PWD)/Heating $(PWD)/Movement $(PWD)/Storage $(PWD)/Webserver
VPATH += $(PWD)/Libraries/Fatfs $(PWD)/Libraries/General $(PWD)/Libraries/Math $(PWD)/Libraries/MCP4461 $(PWD)/Libraries/TemperatureSensor $(PWD)/Libraries/sha1

C_SOURCES += $(foreach dir,$(VPATH),$(wildcard $(dir)/*.c)) $(wildcard $(PWD)/*.c)
CPP_SOURCES := $(foreach dir,$(VPATH),$(wildcard $(dir)/*.cpp)) $(wildcard $(PWD)/*.cpp)

C_OBJS := $(foreach src,$(C_SOURCES),$(BUILD_PATH)/$(notdir $(src:.c=.c.o)))
CPP_OBJS := $(foreach src,$(CPP_SOURCES),$(BUILD_PATH)/$(notdir $(src:.cpp=.cpp.o)))

DEPS := $(C_OBJS:%.o=%.d) $(CPP_OBJS:%.o=%.d)

# Set GCC options
CFLAGS := -DVERSION=\"$(VERSION)\" -DDATE=\"$(DATE)\" -D__SAM4E8E__ -DDUET_NG -DDUET_ETHERNET -Dprintf=iprintf
CFLAGS += -Wall -c -std=gnu11 -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -ffunction-sections -fdata-sections -nostdlib --param max-inline-insns-single=500 -MMD -MP
CPPFLAGS := -DVERSION=\"$(VERSION)\" -DDATE=\"$(DATE)\" -D__SAM4E8E__ -DDUET_NG -DDUET_ETHERNET -Dprintf=iprintf
CPPFLAGS += -Wall -c -std=gnu++11 -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=softfp -ffunction-sections -fdata-sections -fno-threadsafe-statics -fno-rtti -fno-exceptions -nostdlib --param max-inline-insns-single=500 -MMD -MP


FORCESYM := -u _sbrk -u link -u _close -u _fstat -u _isatty -u _lseek -u _read -u _write -u _exit -u kill -u _getpid
LDFLAGS := -L"$(DUET_BOARD_PATH)/variants/duetNG" $(OPTIMISATION) -Wl,--gc-sections -Wl,--fatal-warnings -mcpu=cortex-m4 -T"$(DUET_BOARD_PATH)/variants/duetNG/linker_scripts/gcc/flash.ld" -Wl,-Map,"$(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).map" -o "$(OUTPUT_PATH)/DuetEthernet-$(VERSION).elf" -mthumb -Wl,--cref -Wl,--check-sections -Wl,--gc-sections -Wl,--entry=Reset_Handler -Wl,--unresolved-symbols=report-all -Wl,--warn-common -Wl,--warn-section-align -Wl,--warn-unresolved-symbols -Wl,--start-group $(FORCESYM) $(BUILD_PATH)/*.o -lDuetNG -Wl,--end-group -lm -gcc


# ================================= Target all ======================================
.PHONY += all
all: $(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).bin
$(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).bin: $(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).elf
	@echo "  BIN     ../Release/Duet-Ethernet/DuetEthernetFirmware-$(VERSION).bin"
	@$(OBJCOPY) -O binary $(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).elf $(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).bin

$(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).elf: $(BUILD_PATH) $(OUTPUT_PATH) $(C_OBJS) $(CPP_OBJS)
	@echo "  LD      ../Release/Duet-Ethernet/DuetEthernetFirmware-$(VERSION).elf"
	@$(LD) $(LDFLAGS) -o $(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).elf
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
	@rm -rf $(BUILD_PATH) $(OUTPUT_PATH)/DuetEthernetFirmware-*.bin
	$(info DuetEthernet firmware builds removed.)


# ================================= Target upload ===================================
.PHONY += upload
upload: $(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).bin
	@echo "=> Rebooting hardware into bootloader mode..."
	@stty -F $(PRIMARY_PORT) 115200 -ixon crtscts || true
	@echo "M999 PERASE" > $(PRIMARY_PORT) || true
	@sleep 2
	@echo "=> Flashing new firmware binary..."
	@cp $(OUTPUT_PATH)/DuetEthernetFirmware-$(VERSION).bin $(OUTPUT_PATH)/DuetEthernetFirmware.bin
	@test -s $(PRIMARY_PORT) || $(BOSSAC_4E_PATH) $(PRIMARY_PORT) at91sam4e8-ek ./DuetNG/flash_ethernet.txt || true
	@test -s $(SECONDARY_PORT) || $(BOSSAC_4E_PATH) $(SECONDARY_PORT) at91sam4e8-ek ./DuetNG/flash_ethernet.txt || true
	@rm $(OUTPUT_PATH)/DuetEthernetFirmware.bin
	@echo "=> Flashing complete! Reset your board to boot the new firmware."
