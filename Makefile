export CONFIG_TTY_OVERY_SDIO=m
export CONFIG_RK_WIFI_DEVICE_UWE5622=y
export CONFIG_WLAN_UWE5622=m
export CONFIG_SPRDWL_NG=m
export CONFIG_UNISOC_WIFI_PS=y

KDIR := /usr/lib/modules/$(kernelver)/build
PWD  := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD)/uwe5622 modules
	$(MAKE) -C $(KDIR) M=$(PWD)/bluetooth modules

install:
	$(MAKE) -C $(KDIR) M=$(PWD)/uwe5622 modules_install
	$(MAKE) -C $(KDIR) M=$(PWD)/bluetooth modules_install
clean:
	$(MAKE) -C $(KDIR) M=$(PWD)/uwe5622 clean
	$(MAKE) -C $(KDIR) M=$(PWD)/bluetooth clean

