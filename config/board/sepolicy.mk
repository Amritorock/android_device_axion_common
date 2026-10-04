SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/axion/common/sepolicy/private
SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/axion/common/sepolicy/public
ifeq ($(BOARD_USES_QCOM_HARDWARE),true)
BOARD_VENDOR_SEPOLICY_DIRS += device/axion/common/sepolicy/vendor/qcom

ifneq (,$(filter device/qcom/sepolicy_vndr/legacy-um/generic/vendor/common, $(BOARD_VENDOR_SEPOLICY_DIRS)))
BOARD_SEPOLICY_M4DEFS += HAS_KGSL_MAX_GPUCLK=true
endif
endif

ifneq ($(filter mt%,$(TARGET_BOARD_PLATFORM)),)
BOARD_VENDOR_SEPOLICY_DIRS += device/axion/common/sepolicy/vendor/mtk
endif
