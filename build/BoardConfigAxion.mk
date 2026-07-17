-include device/axion/common/config/soc_map.mk

ifeq ($(TARGET_SHIPS_AXION_KERNEL_MODULES),true)
include device/axion/common/config/board/kernel_modules.mk
endif

include device/axion/common/config/board/sepolicy.mk
