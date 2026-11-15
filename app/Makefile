include $(DEVKITPRO)/libnx/switch_rules

TARGET := nx-music
BUILD := build
APP_TITLE := NX Music
APP_AUTHOR := NX Music
APP_VERSION := $(VERSION)
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
PKG_CONFIG := $(PORTLIBS)/bin/aarch64-none-elf-pkg-config
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags SDL2_ttf)
SDL_LIBS := $(shell $(PKG_CONFIG) --static --libs SDL2_ttf)
CFLAGS := $(ARCH) -D__SWITCH__ -DTUNE_API_VERSION=$(API_VERSION) -O2 -Wall -ffunction-sections -fdata-sections $(INCLUDE) $(SDL_CFLAGS)
CXXFLAGS := $(CFLAGS) -std=gnu++17
LDFLAGS := -specs=$(LIBNX)/switch.specs $(ARCH)
LIBS := $(SDL_LIBS) -lnx -lm

ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export DEPSDIR := $(CURDIR)/$(BUILD)
export VPATH := $(CURDIR)/source $(CURDIR)/../ipc $(CURDIR)/../common/frontend
export INCLUDE := -I$(LIBNX)/include -I$(PORTLIBS)/include -I$(CURDIR)/../ipc -I$(CURDIR)/../common/frontend
export LIBPATHS := -L$(PORTLIBS)/lib -L$(LIBNX)/lib
export OFILES := main.o player.o cover.o tune.o
export LD := $(CXX)
export NROFLAGS := --icon=$(LIBNX)/default_icon.jpg --nacp=$(OUTPUT).nacp

.PHONY: all clean
all:
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).nacp $(TARGET).nro
else
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
-include $(OFILES:.o=.d)
endif
