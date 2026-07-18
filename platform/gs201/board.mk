PRODUCT_SOONG_NAMESPACES += device/axion/common/platform/gs201

include device/axion/common/config/board/tensor.mk
include device/axion/common/config/board/common.mk

PRODUCT_COPY_FILES += \
    device/axion/common/init/axion_init_gs201.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.axion.rc
