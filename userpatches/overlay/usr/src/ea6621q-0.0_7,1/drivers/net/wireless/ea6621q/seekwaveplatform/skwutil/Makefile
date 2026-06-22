# Seekwavetech: GPL-2.0
#
# Copyright (C) 2021-2021 Seekwavetech
# Makefile for Seekwavetech on-board usb driver
#
#PWD := $(shell pwd)
#CURFOLDER ?= $(pwd)
#ccflags-y := -Idrivers/misc/seekwaveplatform/sdio \
	-Idrivers/misc/seekwaveplatform/usb

ifeq ($(CONFIG_OF), y)
        ccflags-y := -DCONFIG_OF
endif

ifneq ($(skw_extra_flags),)
	ccflags-y += $(skw_extra_flags)	-DSKW_EXT_INC
endif

obj-$(CONFIG_SKW_BSP_UCOM) += skw_bootcoms.o
skw_bootcoms-objs += \
	skw_boot.o \
	skw_user_com.o \
	skw_log_to_file.o

