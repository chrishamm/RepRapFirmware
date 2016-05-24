# Makefile for RepRapFirmware
# written by Christian Hammacher
#
# Licensed under the terms of the GNU Public License v2
# see http://www.gnu.org/licenses/gpl-2.0.html
#

# Referenced component versions
ARDUINO_VERSION := 1.6.8
GCC_VERSION := 4.8.3-2014q1
BOSSAC_VERSION := 1.3a-arduino

DUET_BOARD_VERSION := 1.1.6

# Workspace paths
LIBRARY_PATH = $(PWD)/Libraries
BUILD_PATH := $(PWD)/Release/obj
OUTPUT_PATH := $(PWD)/Release

# Compiler options
OPTIMIZATION := -O3

# Firmware port for 1200bps touch
DUET_PORT := /dev/ttyACM0

# ================================ Prerequisites ====================================

# Determine Arduino path
UNAME := $(shell uname -s)
ifeq ($(UNAME),Linux)
  ARDUINO_PATH := $(HOME)/.arduino15
endif
ifeq ($(UNAME),Darwin)
  ARDUINO_PATH := $(HOME)/Library/Arduino15
endif
ifeq (,$(wildcard $(ARDUINO_PATH)/.))
  $(error Arduino directory not found! Are you using Arduino $(ARDUINO_VERSION)?)
endif

# Detect Duet board path
DUET_BOARD_PATH := $(ARDUINO_PATH)/packages/RepRap/hardware/sam/$(DUET_BOARD_VERSION)
ifeq (,$(wildcard $(DUET_BOARD_PATH)/.))
  $(error Duet board not found! Install it first via Arduino''s Boards Manager.)
endif

# Detect library path
DUET_LIBRARY_PATH := $(DUET_BOARD_PATH)/libraries
ifeq (,$(wildcard $(DUET_LIBRARY_PATH)/.))
  $(error Duet libraries not found! Are you using Duet board v1.0.8 or newer?)
endif

# Detect GCC path
GCC_PATH := $(ARDUINO_PATH)/packages/arduino/tools/arm-none-eabi-gcc/$(GCC_VERSION)
ifeq (,$(wildcard $(GCC_PATH)/.))
  $(error GCC toolchain not found! Check your installation.)
endif

# Detect bossac path
BOSSAC_PATH := $(ARDUINO_PATH)/packages/arduino/tools/bossac/$(BOSSAC_VERSION)/bossac
ifeq (,$(wildcard $(BOSSAC_PATH)))
  $(warning Bossac not found! Uploading compiled binaries will not work.)
endif


# ================================ GCC Options ======================================

CROSS_COMPILE := arm-none-eabi-
CC := $(GCC_PATH)/bin/$(CROSS_COMPILE)gcc
CXX := $(GCC_PATH)/bin/$(CROSS_COMPILE)g++
LD := $(GCC_PATH)/bin/$(CROSS_COMPILE)gcc
OBJCOPY := $(GCC_PATH)/bin/$(CROSS_COMPILE)objcopy

INCLUDES := $(DUET_BOARD_PATH)/cores/arduino $(DUET_BOARD_PATH)/variants/duet
INCLUDES += $(DUET_BOARD_PATH)/system/libsam $(DUET_BOARD_PATH)/system/libsam/include $(DUET_BOARD_PATH)/system/CMSIS/CMSIS/Include $(DUET_BOARD_PATH)/system/CMSIS/Device/ATMEL
INCLUDES += $(DUET_LIBRARY_PATH)/EMAC $(DUET_LIBRARY_PATH)/Flash $(DUET_LIBRARY_PATH)/SPI $(DUET_LIBRARY_PATH)/Storage $(DUET_LIBRARY_PATH)/Wire
INCLUDES += $(LIBRARY_PATH)/Fatfs $(LIBRARY_PATH)/Lwip $(LIBRARY_PATH)/MAX31855 $(LIBRARY_PATH)/MCP4461 ${LIBRARY_PATH}/sha1

CFLAGS += -c -g $(OPTIMIZATION) -std=gnu11 -Wall -Wno-unused-variable -ffunction-sections -fdata-sections -nostdlib --param max-inline-insns-single=500 -Dprintf=iprintf -MMD -MP
CPPFLAGS += -c -g $(OPTIMIZATION) -Wall -Wno-unused-variable -std=gnu++11 -ffunction-sections -fdata-sections -nostdlib -fno-threadsafe-statics --param max-inline-insns-single=500 -fno-rtti -fno-exceptions -Dprintf=iprintf -MMD -MP

DEVICE_FLAGS = -mcpu=cortex-m3 -DF_CPU=84000000L -DARDUINO=$(subst .,,$(ARDUINO_VERSION)) -DARDUINO_SAM_DUE -DARDUINO_ARCH_SAM -D__SAM3X8E__ -mthumb -DUSB_VID=0x2341 -DUSB_PID=0x003e -DUSBCON -DUSB_MANUFACTURER=\"RepRap\" -DUSB_PRODUCT=\"Duet\"

CFLAGS += $(foreach dir,$(INCLUDES),-I$(dir))
CPPFLAGS += $(foreach dir,$(INCLUDES),-I$(dir))

LDFLAGS += $(OPTIMIZATION) -Wl,--gc-sections -mcpu=cortex-m3 "-T$(DUET_BOARD_PATH)/variants/duet/linker_scripts/gcc/flash.ld" "-Wl,-Map,$(OUTPUT_PATH)/RepRapFirmware.map" "-L$(BUILD_PATH)" -mthumb -Wl,--cref -Wl,--check-sections -Wl,--gc-sections -Wl,--entry=Reset_Handler -Wl,--unresolved-symbols=report-all -Wl,--warn-common -Wl,--warn-section-align -Wl,--warn-unresolved-symbols -Wl,--start-group $(BUILD_PATH)/*.o "$(DUET_BOARD_PATH)/variants/duet/libsam_sam3x8e_gcc_rel.a" -lm -lgcc -Wl,--end-group

# Unfortunately make doesn't support directory wildcards in targets, so instead we must explicitly specify the source paths by using VPATH
VPATH := $(DUET_BOARD_PATH)/cores/arduino $(DUET_BOARD_PATH)/cores/arduino/USB $(DUET_BOARD_PATH)/system/libsam/source $(DUET_BOARD_PATH)/variants/duet
VPATH += $(DUET_LIBRARY_PATH)/EMAC $(DUET_LIBRARY_PATH)/Flash $(DUET_LIBRARY_PATH)/SPI $(DUET_LIBRARY_PATH)/Storage $(DUET_LIBRARY_PATH)/Wire
VPATH += $(LIBRARY_PATH)/Fatfs $(LIBRARY_PATH)/Lwip/lwip/src/api $(LIBRARY_PATH)/Lwip/lwip/src/core $(LIBRARY_PATH)/Lwip/lwip/src/core/ipv4 $(LIBRARY_PATH)/Lwip/lwip/src/netif $(LIBRARY_PATH)/Lwip/contrib/apps/mdns $(LIBRARY_PATH)/Lwip/contrib/apps/netbios $(LIBRARY_PATH)/MAX31855 $(LIBRARY_PATH)/MCP4461 ${LIBRARY_PATH}/sha1

C_SOURCES += $(foreach dir,$(VPATH),$(wildcard $(dir)/*.c)) $(wildcard $(PWD)/*.c)
CPP_SOURCES := $(foreach dir,$(VPATH),$(wildcard $(dir)/*.cpp)) $(wildcard $(PWD)/*.cpp)

C_OBJS := $(foreach src,$(C_SOURCES),$(BUILD_PATH)/$(notdir $(src:.c=.c.o)))
CPP_OBJS := $(foreach src,$(CPP_SOURCES),$(BUILD_PATH)/$(notdir $(src:.cpp=.cpp.o)))

DEPS := $(C_OBJS:%.o=%.d) $(CPP_OBJS:%.o=%.d)


# ================================= Target all ======================================
.PHONY += all
all: $(OUTPUT_PATH)/RepRapFirmware.bin
$(OUTPUT_PATH)/RepRapFirmware.bin: $(OUTPUT_PATH)/RepRapFirmware.elf
	@echo "  BIN     RepRapFirmware.bin"
	@$(OBJCOPY) -O binary $(OUTPUT_PATH)/RepRapFirmware.elf $(OUTPUT_PATH)/RepRapFirmware.bin

$(OUTPUT_PATH)/RepRapFirmware.elf: $(BUILD_PATH) $(OUTPUT_PATH) $(C_OBJS) $(CPP_OBJS)
	@echo "  LD      RepRapFirmware.elf"
	@$(LD) $(LDFLAGS) -o $(OUTPUT_PATH)/RepRapFirmware.elf
-include $(DEPS)

$(BUILD_PATH)/%.c.o: %.c
	@echo "  CC      $(subst $(DUET_BOARD_PATH)/,,$(subst $(PWD)/,,$<))"
	@$(CC) $(CFLAGS) $(DEVICE_FLAGS) $< -o $@

$(BUILD_PATH)/%.cpp.o: %.cpp
	@echo "  CC      $(subst $(DUET_BOARD_PATH)/,,$(subst $(PWD)/,,$<))"
	@$(CXX) $(CPPFLAGS) $(DEVICE_FLAGS) $< -o $@

$(BUILD_PATH):
	@mkdir -p $(BUILD_PATH)

$(OUTPUT_PATH):
	@mkdir -p $(OUTPUT_PATH)


# ================================= Target clean ====================================
.PHONY += clean
clean:
	@rm -rf $(BUILD_PATH) $(OUTPUT_PATH)
	@rm -f $(PWD)/*.d
	$(info Build directories removed.)


# ================================= Target upload ===================================
.PHONY += upload
upload: $(OUTPUT_PATH)/RepRapFirmware.bin
	@echo "=> Rebooting hardware into bootloader mode..."
	@stty -F $(DUET_PORT) 1200 -ixon -crtscts || true
	@sleep 1
	@echo "=> Flashing new firmware binary..."
	@$(BOSSAC_PATH) -u -e -w -b $(OUTPUT_PATH)/RepRapFirmware.bin -R
