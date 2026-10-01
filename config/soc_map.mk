AXION_PLATFORM := $(strip $(TARGET_BOARD_PLATFORM))
AXION_SOC := $(AXION_PLATFORM)

ifneq ($(AXION_PLATFORM),)
-include device/axion/common/platform/$(AXION_PLATFORM)/board.mk
endif
