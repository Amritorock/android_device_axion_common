include device/axion/common/config/soc_map.mk

$(call inherit-product-if-exists, device/axion/common/$(AXION_SOC)/product.mk)

include device/axion/common/config/version.mk
include device/axion/common/config/dexpreopt.mk
include device/axion/common/config/packages.mk
include device/axion/common/config/flags.mk
include device/axion/common/config/properties.mk
include device/axion/common/config/product.mk
include device/axion/common/config/vulkan/vulkan.mk
include device/axion/common/config/ramplus/ramplus.mk

PRODUCT_PACKAGE_OVERLAYS += device/axion/common/overlay

PRODUCT_SOONG_NAMESPACES += device/axion/common
