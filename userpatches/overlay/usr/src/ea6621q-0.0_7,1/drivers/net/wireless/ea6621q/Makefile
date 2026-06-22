# SPDX-License-Identifier: GPL-2.0
# Seekwave EA6621Q in-tree Makefile

SKW_EXTRA_INC := $(M)/include/linux/platform_data
SKW_EXTRA_FLAGS := -I$(SKW_EXTRA_INC) -include $(SKW_EXTRA_INC)/skw6160_config.h
SKW_EXTRA_FLAGS += -DCONFIG_SEEKWAVE_PLD_RELEASE -Wno-missing-prototypes -Wno-missing-declarations -Wno-unused-function

export skw_extra_flags := $(SKW_EXTRA_FLAGS)

obj-$(CONFIG_SEEKWAVE_BSP_DRIVERS) += seekwaveplatform/
obj-$(CONFIG_WLAN_VENDOR_SEEKWAVE) += skwifi/
obj-$(CONFIG_SKW_BT)               += swtbt4l/
