obj-$(CONFIG_WCN_BOOT) := marlin.o

MARLINOFILES += wcn_boot.o
MARLINOFILES += wcn_dump.o
marlin-$(CONFIG_WCN_SWD) += wcn_swd_dp.o
MARLINOFILES += bufring.o
MARLINOFILES += wcn_txrx.o
MARLINOFILES += rf/rf.o
MARLINOFILES += rdc_debug.o
MARLINOFILES += wcn_log.o
MARLINOFILES += wcn_misc.o
MARLINOFILES += wcn_procfs.o

marlin-objs += $(MARLINOFILES)

#WCN_PLATFORM
obj-$(CONFIG_WCN_PLATFORM) := wcn_platform.o

wcn_platform-c-objs +=  wcn_procfs.o \
			wcn_txrx.o \
			rdc_debug.o \
			wcn_log.o \
			wcn_misc.o \
			rf/rf.o \
			bufring.o
wcn_platform-objs += $(wcn_platform-c-objs)
