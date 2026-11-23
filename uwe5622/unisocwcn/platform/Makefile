obj-$(CONFIG_WCN_BOOT) := marlin.o

MARLINOFILES += wcn_boot.o
MARLINOFILES += wcn_txrx.o
MARLINOFILES += rf/rf.o
MARLINOFILES += wcn_log.o

marlin-objs += $(MARLINOFILES)

#WCN_PLATFORM
obj-$(CONFIG_WCN_PLATFORM) := wcn_platform.o

wcn_platform-c-objs +=  wcn_txrx.o \
			wcn_log.o \
			rf/rf.o
wcn_platform-objs += $(wcn_platform-c-objs)
