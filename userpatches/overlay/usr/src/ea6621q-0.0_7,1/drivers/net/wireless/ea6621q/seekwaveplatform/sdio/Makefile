#Seekwave Tech GPL-2.0
#Makefiie
#Platfrom sdio driver.
PWD :=$(shell pwd)
CURFOLDER ?=$(pwd)

ccflags-y :=-Idrivers/net/wireless/ea6621q/seekwaveplatform/skwutil

ifeq ($(CONFIG_SKW_BT),m)
	ccflags-y += -DCONFIG_BT_SEEKWAVE
endif

ifneq ($(skw_extra_flags),)
	ccflags-y += $(skw_extra_flags)	-DSKW_EXT_INC
endif

obj-$(CONFIG_SKW_SDIOHAL) += skw_sdio.o
skw_sdio-y := skw_sdio_main.o
skw_sdio-y += skw_sdio_rx.o
skw_sdio-y += skw_sdio_debugfs.o
skw_sdio-y += skw_sdio_log.o

#add the marco the skw boot debug
#ccflags-y += -DSKW_BOOT_DEBUG
