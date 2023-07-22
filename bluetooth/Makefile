# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux Bluetooth subsystem.
#

CONFIG_BT=m
CONFIG_BT_BREDR=y
CONFIG_BT_LE=y
CONFIG_BT_HS=y
CONFIG_BT_LEDS=y
CONFIG_BT_DEBUGFS=y
obj-$(CONFIG_BT)	+= bluetooth_sprd.o


bluetooth_sprd-y := af_bluetooth.o hci_core.o hci_conn.o hci_event.o mgmt.o \
	hci_sock.o hci_sysfs.o l2cap_core.o l2cap_sock.o smp.o lib.o \
	ecdh_helper.o hci_request.o mgmt_util.o mgmt_config.o hci_codec.o \
	eir.o hci_sync.o

bluetooth_sprd-$(CONFIG_BT_BREDR) += sco.o
bluetooth_sprd-$(CONFIG_BT_LE) += iso.o
bluetooth_sprd-$(CONFIG_BT_HS) += a2mp.o amp.o
bluetooth_sprd-$(CONFIG_BT_LEDS) += leds.o
bluetooth_sprd-$(CONFIG_BT_MSFTEXT) += msft.o
bluetooth_sprd-$(CONFIG_BT_AOSPEXT) += aosp.o
bluetooth_sprd-$(CONFIG_BT_DEBUGFS) += hci_debugfs.o
bluetooth_sprd-$(CONFIG_BT_SELFTEST) += selftest.o
