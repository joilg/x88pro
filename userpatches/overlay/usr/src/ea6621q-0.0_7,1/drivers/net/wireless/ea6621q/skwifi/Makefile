# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_WLAN_VENDOR_SEEKWAVE) += skw.o

skw-objs += skw_core.o
skw-objs += skw_iface.o
skw-objs += skw_cfg80211.o
skw-objs += skw_msg.o
skw-objs += skw_rx.o
skw-objs += skw_tx.o
skw-objs += skw_regd.o
skw-objs += skw_mlme.o
skw-objs += skw_timer.o
skw-objs += skw_log.o
skw-objs += skw_iw.o
skw-objs += skw_work.o
skw-objs += skw_mbssid.o
skw-objs += skw_dentry.o
skw-objs += skw_util.o
skw-objs += skw_recovery.o
skw-objs += trace.o

skw-$(CONFIG_SKW_CALIB_DPD) += skw_calib.o
skw-$(CONFIG_SKW_EDMA) += skw_edma.o
skw-$(CONFIG_SKW_VENDOR) += skw_vendor.o
skw-$(CONFIG_SKW_DFS_MASTER) += skw_dfs.o
skw-$(CONFIG_SKW_TDLS) += skw_tdls.o

ccflags-$(CONFIG_SKW_REPEATER) += -DSKW_ARP_NAT -DSKW_DHCP_BROADCAST
ccflags-$(CONFIG_SKW_CALIB_DPD) += -DSKW_IMPORT_NS

ccflags-y += -I$(M)/include/linux/platform_data/

ifneq ($(skw_extra_flags),)
ccflags-y += $(skw_extra_flags)
endif

ifneq ($(skw_extra_symbols),)
KBUILD_EXTRA_SYMBOLS += $(skw_extra_symbols)
endif

CFLAGS_trace.o := -I$(src)

$(obj)/skw_core.o : $(obj)/version.h
$(obj)/version.h: $(src)/genver.pl
	@$(PERL) -s $(src)/genver.pl $@

clean-files := version.h
