/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/syscalls.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/firmware.h>
#include <linux/file.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mfd/syscon.h>
#include <linux/mfd/core.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unistd.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <marlin_platform.h>
#include <wcn_bus.h>

#include "rf/rf.h"
#include "../sleep/sdio_int.h"
#include "../sleep/slp_mgr.h"
#include "rdc_debug.h"
#include "mdbg_type.h"
#include "wcn_glb_reg.h"
#include "wcn_glb.h"

u32 wcn_print_level = WCN_DEBUG_OFF;

#ifdef CONFIG_WCN_SDIO
#include "../sdio/sdiohal.h"
#endif

#define WCN_FW_NAME	"wcnmodem.bin"

#ifndef REG_PMU_APB_XTL_WAIT_CNT0
#define REG_PMU_APB_XTL_WAIT_CNT0 0xe42b00ac
#endif

struct wcn_sync_info_t {
	unsigned int init_status;
	unsigned int prj_type;
	unsigned int tsx_dac_data;
	unsigned int sdio_config;
	unsigned int dcache_status;
	unsigned int dcache_start_addr;
	unsigned int dcache_end_addr;
	unsigned int mem_ap_cmd;
	unsigned int rsvd[3];
	unsigned int bind_verify_data[4];
};

/*
 * sdio config to cp side
 * bit[31:24]: reserved
 * bit[23]: wake_host_data_separation:
 *	0: if BT_WAKEUP_HOST en or WL_WAKEUP_HOST en,
 *	    wifi and bt packets can wake host;
 *	1: if BT_WAKEUP_HOST en, ONLY bt packets can wake host;
 *	    if WL_WAKEUP_HOST en, ONLY wifi packets can wake host
 * bit[22:18]: wake_host_level_duration_10ms: BT_WAKEUP_HOST or WL_WAKEUP_HOST
 *	      level dyration time per 10ms, example: 0:0ms; 3:30ms; 20:200ms
 * bit[17:16]: wl_wake_host_trigger_type:
 *	     00:WL_WAKEUP_HOST  trigger type low
 *	     01:WL_WAKEUP_HOST  trigger type rising
 *	     10:WL_WAKEUP_HOST  trigger type falling
 *	     11:WL_WAKEUP_HOST  trigger type high
 * bit[15]: wl_wake_host_en: 0: disable, 1: enable
 * bit[14:13]: sdio_irq_trigger_type:
 *	      00:pubint gpio irq trigger type low
 *	      01:pubint gpio irq trigger type rising [NOT support]
 *	      10:pubint gpio irq trigger type falling [NOT support]
 *	      11:pubint gpio irq trigger type high
 * bit[12:11]: sdio_irq_type:
 *	      00:dedicated irq, gpio1
 *	      01:inband data1 irq
 *	      10:use BT_WAKEUP_HOST(pubint) pin as gpio irq
 *	      11:use WL_WAKEUP_HOST(esmd3) pin as gpio irq
 * bit[10:9]: bt_wake_host_trigger_type:
 *	     00:BT_WAKEUP_HOST  trigger type low
 *	     01:BT_WAKEUP_HOST  trigger type rising
 *	     10:BT_WAKEUP_HOST  trigger type falling
 *	     11:BT_WAKEUP_HOST  trigger type high
 * bit[8]: bt_wake_host_en: 0: disable, 1: enable
 * bit[7:5]: sdio_blk_size: 000: blocksize 840; 001: blocksize 512
 * bit[4]: sdio_rx_mode: 0: adma; 1: sdma
 * bit[3:1]: vendor_id: 000: default id, unisoc[0x0]
 *		       001: hisilicon default version, pull chipen after resume
 *		       010: hisilicon version, keep power (NOT pull chipen) and
 *			    reset sdio after resume
 * bit[0]: sdio_config_en: 0: disable sdio config
 *		          1: enable sdio config
 */
union wcn_sdiohal_config {
	unsigned int val;
	struct {
		unsigned char sdio_config_en:1;
		unsigned char vendor_id:3;
		unsigned char sdio_rx_mode:1;
		unsigned char sdio_blk_size:3;
		unsigned char bt_wake_host_en:1;
		unsigned char bt_wake_host_trigger_type:2;
		unsigned char sdio_irq_type:2;
		unsigned char sdio_irq_trigger_type:2;
		unsigned char wl_wake_host_en:1;
		unsigned char wl_wake_host_trigger_type:2;
		unsigned char wake_host_level_duration_10ms:5;
		unsigned char wake_host_data_separation:1;
		unsigned int  reserved:8;
	} cfg;
};

struct wcn_clock_info {
	enum wcn_clock_type type;
	enum wcn_clock_mode mode;
	/*
	 * xtal-26m-clk-type-gpio config in the dts.
	 * if xtal-26m-clk-type config in the dts,this gpio unvalid.
	 */
	int gpio;
};

struct marlin_device {
	struct device *dev;
	struct wcn_clock_info clk_xtal_26m;
	int coexist;
	int wakeup_ap;
	int ap_send_data;
	struct gpio_desc *reset;
	struct gpio_desc *chip_en;
	int int_ap;
	/* power sequence */
	/* VDDIO->DVDD12->chip_en->rst_N->AVDD12->AVDD33 */
	struct regulator *dvdd12;
	struct regulator *avdd12;
	/* for PCIe */
	struct regulator *avdd18;
	/* for wifi PA, BT TX RX */
	struct regulator *avdd33;
	/* for internal 26M clock */
	struct regulator *dcxo18;
	struct regulator *avdd33_usb20;
	struct clk *clk_32k;

	struct clk *clk_parent;
	struct clk *clk_enable;
	struct mutex power_lock;
	struct completion carddetect_done;
	struct completion download_done;
	int download_result;
	unsigned long power_state;
	char *write_buffer;
	struct work_struct download_wq;
	bool no_power_off;
	int wifi_need_download_ini_flag;
	int first_power_on_flag;
	unsigned char download_finish_flag;
	unsigned char bt_wl_wake_host_en;
	unsigned int bt_wake_host_int_num;
	unsigned int wl_wake_host_int_num;
	struct wcn_sync_info_t sync_f;
	char *btwf_path;
};

struct wifi_calibration {
	struct wifi_config_t config_data;
	struct wifi_cali_t cali_data;
};

static struct wifi_calibration wifi_data;
marlin_reset_callback marlin_reset_func;
void *marlin_callback_para;

struct marlin_device *marlin_dev;

unsigned char  flag_reset;
char functionmask[8];
static unsigned int reg_val;
static unsigned int clk_wait_val;
static unsigned int cp_clk_wait_val;
static unsigned int marlin2_clk_wait_reg;

#ifdef CONFIG_WCN_PMIC
/* for PMIC control */
static struct regmap *reg_map;
#endif
#define IMG_HEAD_MAGIC "WCNM"
#define IMG_HEAD_MAGIC_COMBINE "WCNE"

#define IMG_MARLINAA_TAG "MLAA"
#define IMG_MARLINAB_TAG "MLAB"
#define IMG_MARLINAC_TAG "MLAC"
#define IMG_MARLINAD_TAG "MLAD"

#define IMG_MARLIN3_AA_TAG "30AA"
#define IMG_MARLIN3_AB_TAG "30AB"
#define IMG_MARLIN3_AC_TAG "30AC"
#define IMG_MARLIN3_AD_TAG "30AD"

#define IMG_MARLIN3L_AA_TAG "3LAA"
#define IMG_MARLIN3L_AB_TAG "3LAB"
#define IMG_MARLIN3L_AC_TAG "3LAC"
#define IMG_MARLIN3L_AD_TAG "3LAD"

#define IMG_MARLIN3E_AA_TAG "3EAA"
#define IMG_MARLIN3E_AB_TAG "3EAB"
#define IMG_MARLIN3E_AC_TAG "3EAC"
#define IMG_MARLIN3E_AD_TAG "3EAD"

#define MARLIN_MASK 0x27F
#define AUTO_RUN_MASK 0X100

/* #define E2S(x) { case x: return #x; } */

struct head {
	char magic[4];
	unsigned int version;
	unsigned int img_count;
};

struct imageinfo {
	char tag[4];
	unsigned int offset;
	unsigned int size;
};

struct combin_img_info {
	unsigned int addr;			/* image target address */
	unsigned int offset;			/* image combin img offset */
	unsigned int size;			/* image size */
};

unsigned long marlin_get_power_state(void)
{
	return marlin_dev->power_state;
}
EXPORT_SYMBOL_GPL(marlin_get_power_state);

unsigned char marlin_get_bt_wl_wake_host_en(void)
{
	return marlin_dev->bt_wl_wake_host_en;
}
EXPORT_SYMBOL_GPL(marlin_get_bt_wl_wake_host_en);

/* return chipid, for example:
 * 0x2355000x: Marlin3 series
 * 0x2355B00x: Marlin3 lite series
 * 0x5663000x: Marlin3E series
 * 0: read chipid fail or not unisoc module
 */
#define WCN_CHIPID_MASK (0xFFFFF000)
unsigned int marlin_get_wcn_chipid(void)
{
	int ret;
	static unsigned long chip_id;
	if (likely(chip_id))
		return chip_id;


#ifndef CONFIG_CHECK_DRIVER_BY_CHIPID
	ret = sprdwcn_bus_reg_read(CHIPID_REG, &chip_id, 4);
	if (ret < 0) {
		WCN_ERR("marlin read chip ID fail, ret=%d\n", ret);
		return 0;
	}

#else
	/* At first, read Marlin3E chipid register. */
	ret = sprdwcn_bus_reg_read(CHIPID_REG_M3E, &chip_id, 4);
	if (ret < 0) {
		WCN_ERR("read marlin3E chip id fail, ret=%d\n", ret);
		return 0;
	}

	/* If it is not Marlin3E, then read Marlin3 chipid register. */
	if ((chip_id & WCN_CHIPID_MASK) != MARLIN3E_AA_CHIPID) {
		ret = sprdwcn_bus_reg_read(CHIPID_REG_M3_M3L, &chip_id, 4);
		if (ret < 0) {
			WCN_ERR("read marlin3 chip id fail, ret=%d\n", ret);
			return 0;
		}
	}
#endif
	WCN_INFO("%s: chipid: 0x%lx\n", __func__, chip_id);

	return chip_id;
}
EXPORT_SYMBOL_GPL(marlin_get_wcn_chipid);

/* return chip model, for example:
 * 0: WCN_CHIP_INVALID
 * 1: WCN_CHIP_MARLIN3
 * 2: WCN_CHIP_MARLIN3L
 * 3: WCN_CHIP_MARLIN3E
 */
enum wcn_chip_model wcn_get_chip_model(void)
{
	static enum wcn_chip_model chip_model = WCN_CHIP_INVALID;
	unsigned int chip_id;
	static const char *chip_model_str[] = {
		"ERRO",
		"Marlin3",
		"Marlin3Lite",
		"Marlin3E",
	};

	if (likely(chip_model))
		return chip_model;

	/* if read chipid fail or not unisoc module, chip_id is 0. */
	chip_id = marlin_get_wcn_chipid();
	if (chip_id == 0)
		chip_model = WCN_CHIP_INVALID;
	else if ((chip_id & WCN_CHIPID_MASK) == MARLIN3_AA_CHIPID)
		chip_model = WCN_CHIP_MARLIN3;
	else if ((chip_id & WCN_CHIPID_MASK) == MARLIN3L_AA_CHIPID)
		chip_model = WCN_CHIP_MARLIN3L;
	else if ((chip_id & WCN_CHIPID_MASK) == MARLIN3E_AA_CHIPID)
		chip_model = WCN_CHIP_MARLIN3E;
	else
		chip_model = WCN_CHIP_INVALID;
	WCN_DEBUG("%s: chip_model: %s\n", __func__, chip_model_str[chip_model]);

	return chip_model;
}
EXPORT_SYMBOL_GPL(wcn_get_chip_model);

/* return chip type, for example:
 * 0: WCN_CHIP_ID_INVALID
 * 1: WCN_CHIP_ID_AA
 * 2: WCN_CHIP_ID_AB
 * 3: WCN_CHIP_ID_AC
 * 4: WCN_CHIP_ID_AD
 */
enum wcn_chip_id_type wcn_get_chip_type(void)
{
	static enum wcn_chip_id_type chip_type = WCN_CHIP_ID_INVALID;
	unsigned int chip_id;
	static const char *chip_type_str[] = {
		"ERRO",
		"AA",
		"AB",
		"AC",
		"AD",
	};

	if (likely(chip_type))
		return chip_type;

	/* if read chipid fail or not unisoc module, chip_id is 0. */
	chip_id = marlin_get_wcn_chipid();
	if (chip_id == 0)
		chip_type = WCN_CHIP_ID_INVALID;
	else
		chip_type = (chip_id & 0xF) + 1;
	WCN_DEBUG("%s: chip_type: %s\n", __func__, chip_type_str[chip_type]);

	return chip_type;
}
EXPORT_SYMBOL_GPL(wcn_get_chip_type);

#define WCN_CHIP_NAME_UNKNOWN "UNKNOWN"

/* Marlin3_AD_0x23550003 */
const char *wcn_get_chip_name(void)
{
	unsigned int chip_id, pos = 0;
	enum wcn_chip_model chip_model;
	enum wcn_chip_id_type chip_type;
	static char wcn_chip_name[32] = { 0 };
	static const char *chip_model_str[] = {
		"ERRO_",
		"Marlin3_",
		"Marlin3Lite_",
		"Marlin3E_",
	};
	static const char *chip_type_str[] = {
		"ERRO_",
		"AA_",
		"AB_",
		"AC_",
		"AD_",
	};

	if (wcn_chip_name[0])
		return wcn_chip_name;

	/* if read chipid fail or not unisoc module, chip_id is 0. */
	chip_id = marlin_get_wcn_chipid();
	if (chip_id == 0) {
		memcpy((void *)wcn_chip_name, WCN_CHIP_NAME_UNKNOWN,
			   strlen(WCN_CHIP_NAME_UNKNOWN));
		goto out;
	}

	chip_model = wcn_get_chip_model();
	if (chip_model == WCN_CHIP_INVALID) {
		memcpy((void *)wcn_chip_name, WCN_CHIP_NAME_UNKNOWN,
			   strlen(WCN_CHIP_NAME_UNKNOWN));
		goto out;
	}
	sprintf((char *)wcn_chip_name, chip_model_str[chip_model]);
	pos += strlen(chip_model_str[chip_model]);

	chip_type = wcn_get_chip_type();
	if (chip_type == WCN_CHIP_ID_INVALID) {
		memcpy((void *)wcn_chip_name, WCN_CHIP_NAME_UNKNOWN,
			   strlen(WCN_CHIP_NAME_UNKNOWN));
		goto out;
	}
	sprintf((char *)(wcn_chip_name + pos), chip_type_str[chip_type]);
	pos += strlen(chip_type_str[chip_type]);
	sprintf((char *)(wcn_chip_name + pos), "0x%x", chip_id);

out:
	WCN_DEBUG("%s: chip_name: %s\n", __func__, wcn_chip_name);
	return wcn_chip_name;
}
EXPORT_SYMBOL_GPL(wcn_get_chip_name);

/*
 * Some platforms not insmod bsp ko dynamically. This function is used for
 * wifi or bt to insmod driver statically.
 * return: 0: unisoc module; -1: other module
 */
#ifdef CONFIG_WCN_CHECK_MODULE_VENDOR
static int marlin_find_sdio_device_id(unsigned char *path)
{
	int i, open_cnt = 6;
	struct file *filp;
	loff_t pos;
	unsigned char read_buf[64], sdio_id_path[64];
	char *sdio_id_pos;

	sprintf(sdio_id_path, path);
	for (i = 0; i < open_cnt; i++) {
		filp = filp_open(sdio_id_path, O_RDONLY, 0644);
		if (IS_ERR(filp))
			msleep(100);
		else
			break;
	}
	if (i >= open_cnt) {
		WCN_ERR("%s open %s fail no.%d cnt=%d\n", __func__,
			sdio_id_path, (int)IS_ERR(filp), i);
		/* other module */
		return -1;
	}
	WCN_INFO("%s open %s success cnt=%d\n", __func__,
		 sdio_id_path, i);
	pos = 0;
	kernel_read(filp, read_buf, sizeof(read_buf), &pos);
	WCN_INFO("%s read_buf: %s\n", __func__, read_buf);
	sdio_id_pos = strstr(read_buf, "SDIO_ID=0000:0000");
	if (!sdio_id_pos) {
		WCN_ERR("%s sdio id not match (0000:0000)\n", __func__);
		filp_close(filp, NULL);
		/* other module */
		return -1;
	}
	filp_close(filp, NULL);
	WCN_INFO("%s: This is unisoc module\n", __func__);

	/* unisoc module */
	return 0;
}
#endif

#define SDIO_DEVICE_ID_PATH0 ("/sys/bus/sdio/devices/mmc0:8800:1/uevent")
#define SDIO_DEVICE_ID_PATH1 ("/sys/bus/sdio/devices/mmc1:8800:1/uevent")
#define SDIO_DEVICE_ID_PATH2 ("/sys/bus/sdio/devices/mmc2:8800:1/uevent")
int marlin_get_wcn_module_vendor(void)
{
	int ret = 0;

#ifdef CONFIG_WCN_CHECK_MODULE_VENDOR
	ret = marlin_find_sdio_device_id(SDIO_DEVICE_ID_PATH1);
	if (!ret)
		return ret;
	ret = marlin_find_sdio_device_id(SDIO_DEVICE_ID_PATH2);
	if (!ret)
		return ret;
	ret = marlin_find_sdio_device_id(SDIO_DEVICE_ID_PATH0);
	return ret;
#endif

	/* unisoc module */
	return ret;
}
EXPORT_SYMBOL_GPL(marlin_get_wcn_module_vendor);

/* return number of marlin antennas
 * MARLIN_TWO_ANT; MARLIN_THREE_ANT; ...
 */
int marlin_get_ant_num(void)
{
	return 1;
}
EXPORT_SYMBOL_GPL(marlin_get_ant_num);

/* get the subsys string */
const char *strno(int subsys)
{
	switch (subsys) {
	case MARLIN_BLUETOOTH:
		return "MARLIN_BLUETOOTH";
	case MARLIN_FM:
		return "MARLIN_FM";
	case MARLIN_WIFI:
		return "MARLIN_WIFI";
	case MARLIN_WIFI_FLUSH:
		return "MARLIN_WIFI_FLUSH";
	case MARLIN_SDIO_TX:
		return "MARLIN_SDIO_TX";
	case MARLIN_SDIO_RX:
		return "MARLIN_SDIO_RX";
	case MARLIN_MDBG:
		return "MARLIN_MDBG";
	case WCN_AUTO:
		return "WCN_AUTO";
	case MARLIN_ALL:
		return "MARLIN_ALL";
	default: return "MARLIN_SUBSYS_UNKNOWN";
	}
/* #undef E2S */
}

#define marlin_firmware_get_combin_info(buffer) \
		(struct combin_img_info *)(buffer + sizeof(struct head))

#define bin_magic_is(data, magic_tag) \
	!strncmp(((struct head *)data)->magic, magic_tag, strlen(magic_tag))

#define marlin_fw_get_img_count(img) (((struct head *)img)->img_count)

static const struct imageinfo *marlin_imageinfo_get_from_data(const char *tag,
		const void *data)
{
	const struct imageinfo *imageinfo;
	int imageinfo_count;
	int i;

	imageinfo = (struct imageinfo *)(data + sizeof(struct head));
	imageinfo_count = marlin_fw_get_img_count(data);

	for (i = 0; i < imageinfo_count; i++)
		if (!strncmp(imageinfo[i].tag, tag, 4))
			return &(imageinfo[i]);
	return NULL;
}

static const struct imageinfo *marlin_judge_images(const unsigned char *buffer)
{
	unsigned long chip_id;
	const struct imageinfo *image_info;

	chip_id = marlin_get_wcn_chipid();
	if (buffer == NULL)
		return NULL;

#ifndef CONFIG_CHECK_DRIVER_BY_CHIPID
	if (wcn_get_chip_model() == WCN_CHIP_MARLIN3) {
		switch (chip_id) {
		case MARLIN_AA_CHIPID:
			return marlin_imageinfo_get_from_data(IMG_MARLINAA_TAG,
								  buffer);
		case MARLIN_AB_CHIPID:
			return marlin_imageinfo_get_from_data(IMG_MARLINAB_TAG,
								  buffer);
		case MARLIN_AC_CHIPID:
		case MARLIN_AD_CHIPID:
			/* bin of m3 AC and AD chip is the same. */
			image_info = marlin_imageinfo_get_from_data(
				IMG_MARLINAC_TAG, buffer);
			if (image_info) {
				WCN_INFO("%s find %s tag\n", __func__,
					 IMG_MARLINAC_TAG);
				return image_info;
			}
			return marlin_imageinfo_get_from_data(IMG_MARLINAD_TAG,
								  buffer);
		default:
			WCN_ERR("Marlin can't find correct imginfo!\n");
		}
	}
	if (wcn_get_chip_model() == WCN_CHIP_MARLIN3L) {
		switch (chip_id) {
		case MARLIN_AA_CHIPID:
			return marlin_imageinfo_get_from_data(IMG_MARLINAA_TAG,
								  buffer);
		case MARLIN_AB_CHIPID:
			return marlin_imageinfo_get_from_data(IMG_MARLINAB_TAG,
								  buffer);
		default:
			WCN_ERR("Marlin can't find correct imginfo!\n");
		}
	}
	if (wcn_get_chip_model() == WCN_CHIP_MARLIN3E) {
		switch (chip_id) {
		case MARLIN_AA_CHIPID:
		case MARLIN_AB_CHIPID:
			/* bin of m3e AA and AB chip is the same. */
			image_info = marlin_imageinfo_get_from_data(
				IMG_MARLINAA_TAG, buffer);
			if (image_info) {
				WCN_INFO("%s find %s tag\n", __func__,
					 IMG_MARLINAA_TAG);
				return image_info;
			}
			return marlin_imageinfo_get_from_data(IMG_MARLINAB_TAG,
								  buffer);
		default:
			WCN_ERR("Marlin can't find correct imginfo!\n");
		}
	}

#else  /*ELOF CONFIG_CHECK_DRIVER_BY_CHIPID*/

	switch (chip_id) {
	case MARLIN3_AA_CHIPID:
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3_AA_TAG, buffer);
	case MARLIN3_AB_CHIPID:
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3_AB_TAG, buffer);
	case MARLIN3_AC_CHIPID:
	case MARLIN3_AD_CHIPID:
		/* bin of m3 AC and AD chip is the same. */
		image_info = marlin_imageinfo_get_from_data(
			IMG_MARLIN3_AC_TAG, buffer);
		if (image_info) {
			WCN_INFO("%s find %s tag\n", __func__,
				 IMG_MARLIN3_AC_TAG);
			return image_info;
		}
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3_AD_TAG, buffer);
	case MARLIN3L_AA_CHIPID:
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3L_AA_TAG, buffer);
	case MARLIN3L_AB_CHIPID:
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3L_AB_TAG, buffer);
	case MARLIN3L_AC_CHIPID:
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3L_AC_TAG, buffer);
	case MARLIN3L_AD_CHIPID:
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3L_AD_TAG, buffer);
	case MARLIN3E_AA_CHIPID:
	case MARLIN3E_AB_CHIPID:
		/* bin of m3e AA and AB chip is the same. */
		image_info = marlin_imageinfo_get_from_data(
			IMG_MARLIN3E_AA_TAG, buffer);
		if (image_info) {
			WCN_INFO("%s find %s tag\n", __func__,
				 IMG_MARLIN3E_AA_TAG);
			return image_info;
		}
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3E_AB_TAG, buffer);
	case MARLIN3E_AC_CHIPID:
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3E_AC_TAG, buffer);
	case MARLIN3E_AD_CHIPID:
		return marlin_imageinfo_get_from_data(
			IMG_MARLIN3E_AD_TAG, buffer);
	default:
		WCN_INFO("%s Cannot find Chip Firmware\n", __func__);
		break;
	}
#endif /*EOF CONFIG_CHECK_DRIVER_BY_CHIPID*/
	return NULL;
}

static int sprdwcn_bus_direct_write_dispack(unsigned int addr, const void *buf,
		size_t buf_size, size_t packet_max_size)
{
	int ret = 0;
	size_t offset = 0;
	void *kbuf = marlin_dev->write_buffer;

	while (offset < buf_size) {
		size_t temp_size = min(packet_max_size, buf_size - offset);

		memcpy(kbuf, buf + offset, temp_size);
		ret = sprdwcn_bus_direct_write(addr + offset, kbuf, temp_size);
		if (ret < 0)
			goto OUT;

		offset += temp_size;
	}
OUT:
	if (ret < 0)
		WCN_ERR(" %s: dt write error:%d\n", __func__, ret);
	return ret;
}

struct marlin_firmware {
	const u8 *data;
	size_t size;
	bool is_from_fs;
	const void *priv;
};

/* this function __must__ be paired with marlin_firmware_release !*/
/* Suggest use it like Documentation/firmware_class/README:65
 *
 *	if(marlin_request_firmware(&fw) == 0)
 *		handle_this_firmware(fw);
 *	marlin_release_firmware(fw);
 */
static int marlin_request_firmware(struct marlin_firmware **mfirmware_p)
{
	struct marlin_firmware *mfirmware;
	const struct firmware *firmware;
	int ret;

	*mfirmware_p = NULL;
	mfirmware = kmalloc_obj(struct marlin_firmware, GFP_KERNEL);
	if (!mfirmware)
		return -ENOMEM;
	if (marlin_dev->btwf_path[0] == '/') {
		dev_err(marlin_dev->dev, "firmware name must be relative: %s\n",
			marlin_dev->btwf_path);
		kfree(mfirmware);
		return -EINVAL;
	}
	ret = request_firmware(&firmware, marlin_dev->btwf_path, marlin_dev->dev);
	if (ret) {
		kfree(mfirmware);
		return ret;
	}
	mfirmware->priv = firmware;
	mfirmware->data = firmware->data;
	mfirmware->size = firmware->size;
	mfirmware->is_from_fs = true;

	if (mfirmware->size < sizeof(functionmask)) {
		if (mfirmware->is_from_fs)
			release_firmware(mfirmware->priv);
		kfree(mfirmware);
		return -EINVAL;
	}
	memcpy(functionmask, mfirmware->data, 8);
	if ((functionmask[0] == 0x00) && (functionmask[1] == 0x00)) {
		mfirmware->data += 8;
		mfirmware->size -= 8;
	} else {
		functionmask[7] = 0;
	}

	*mfirmware_p = mfirmware;

	return 0;
}

static int marlin_firmware_parse_image(struct marlin_firmware *mfirmware)
{
	if (mfirmware->size < sizeof(struct head))
		return -EINVAL;

	if (bin_magic_is(mfirmware->data, IMG_HEAD_MAGIC)) {
		const struct imageinfo *info;
		unsigned int count = marlin_fw_get_img_count(mfirmware->data);

		if (count > (mfirmware->size - sizeof(struct head)) /
			    sizeof(struct imageinfo))
			return -EINVAL;

		WCN_INFO("%s imagepack is %s type,need parse it\n",
			 __func__, IMG_HEAD_MAGIC);
		info = marlin_judge_images(mfirmware->data);
		if (!info) {
			WCN_ERR("marlin:%s imginfo is NULL\n", __func__);
			return -1;
		}
		if (mfirmware->is_from_fs &&
		    (info->offset > mfirmware->size ||
		     info->size > mfirmware->size - info->offset))
			return -EINVAL;
		mfirmware->size = info->size;
		mfirmware->data += info->offset;
	} else if (bin_magic_is(mfirmware->data, IMG_HEAD_MAGIC_COMBINE)) {
		/* cal the combin size */
		unsigned int img_count, index;
		const struct combin_img_info *img_info;
		size_t img_real_size = 0;

		WCN_INFO("%s imagepack is %s type,need parse it\n",
			 __func__, IMG_HEAD_MAGIC_COMBINE);

		img_count = marlin_fw_get_img_count(mfirmware->data);
		if (!img_count || img_count >
		    (mfirmware->size - sizeof(struct head)) / sizeof(*img_info))
			return -EINVAL;
		img_info = marlin_firmware_get_combin_info(mfirmware->data);

		if (mfirmware->is_from_fs) {
			for (index = 0; index < img_count; index++) {
				if (img_info[index].offset > mfirmware->size ||
				    img_info[index].size >
					mfirmware->size - img_info[index].offset)
					return -EINVAL;
				img_real_size = max(img_real_size,
					(size_t)img_info[index].offset + img_info[index].size);
			}
		} else {
			img_real_size = (size_t)img_info[img_count - 1].size +
				img_info[img_count - 1].offset;
		}

		mfirmware->size = img_real_size;
	}

	return 0;
}

static int marlin_firmware_write(struct marlin_firmware *mfirmware)
{
	int i = 0;
	int combin_img_count;
	const struct combin_img_info *imginfoe;
	int err;

	if (mfirmware->size < sizeof(struct head))
		return -EINVAL;

	if (bin_magic_is(mfirmware->data, IMG_HEAD_MAGIC_COMBINE)) {
		WCN_INFO("marlin %s imagepack is WCNE type,need parse it\n",
			__func__);

		combin_img_count = marlin_fw_get_img_count(mfirmware->data);
		if (combin_img_count <= 0 || combin_img_count >
		    (mfirmware->size - sizeof(struct head)) / sizeof(*imginfoe))
			return -EINVAL;
		imginfoe = marlin_firmware_get_combin_info(mfirmware->data);
		if (!imginfoe) {
			WCN_ERR("marlin:%s imginfo is NULL\n", __func__);
			return -1;
		}

		for (i = 0; i < combin_img_count; i++) {
			if (imginfoe[i].offset > mfirmware->size ||
			    imginfoe[i].size > mfirmware->size - imginfoe[i].offset) {
				WCN_ERR("%s memory crossover\n", __func__);
				return -1;
			}
			err = sprdwcn_bus_direct_write_dispack(imginfoe[i].addr,
					mfirmware->data + imginfoe[i].offset,
					imginfoe[i].size, PACKET_SIZE);
			if (err) {
				WCN_ERR("%s download error\n", __func__);
				return -1;
			}
		}
	} else {
		err = sprdwcn_bus_direct_write_dispack(CP_START_ADDR,
				mfirmware->data, mfirmware->size, PACKET_SIZE);
		if (err) {
			WCN_ERR("%s download error\n", __func__);
			return -1;
		}
	}
	WCN_INFO("combin_img %d %s finish and successful\n", i, __func__);

	return 0;
}

static void marlin_release_firmware(struct marlin_firmware *mfirmware)
{
	if (mfirmware) {
		if (mfirmware->is_from_fs)
			release_firmware(mfirmware->priv);
		kfree(mfirmware);
	}
}

/* BT WIFI FM download */
static int btwifi_download_firmware(void)
{
	int ret;
	struct marlin_firmware *mfirmware;

	ret = marlin_request_firmware(&mfirmware);
	if (ret) {
		WCN_ERR("%s request firmware error\n", __func__);
		goto OUT;
	}
	ret = marlin_firmware_parse_image(mfirmware);
	if (ret) {
		WCN_ERR("%s firmware parse AA\\AB error\n", __func__);
		goto OUT;
	}

	ret = marlin_firmware_write(mfirmware);
	if (ret) {
		WCN_ERR("%s firmware write error\n", __func__);
		goto OUT;
	}

OUT:
	marlin_release_firmware(mfirmware);

	return ret;
}

static int marlin_parse_dt(struct device *dev)
{
#ifdef CONFIG_WCN_PARSE_DTS
	struct gpio_desc *int_ap;
#endif
#ifdef CONFIG_WCN_PARSE_DTS
	struct device_node *np = dev->of_node;
#endif
#ifdef CONFIG_WCN_PMIC
	struct regmap *pmu_apb_gpr;
	struct wcn_clock_info *clk;
#endif
	int ret = -1;

	if (!marlin_dev)
		return ret;

#if defined(CONFIG_WCN_PARSE_DTS) && defined(CONFIG_WCN_PMIC)
	clk = &marlin_dev->clk_xtal_26m;
	clk->gpio = of_get_named_gpio(np, "xtal-26m-clk-type-gpio", 0);
	if (!gpio_is_valid(clk->gpio))
		WCN_INFO("xtal-26m-clk gpio not config\n");

	/* xtal-26m-clk-type has priority over than xtal-26m-clk-type-gpio */
	ret = of_property_read_string(np, "xtal-26m-clk-type",
					  (const char **)&buf);
	if (!ret) {
		WCN_INFO("force config xtal 26m clk %s\n", buf);
		if (!strncmp(buf, "TCXO", 4))
			clk->type = WCN_CLOCK_TYPE_TCXO;
		else if (!strncmp(buf, "TSX", 3))
			clk->type = WCN_CLOCK_TYPE_TSX;
		else
			WCN_ERR("force config xtal 26m clk %s err!\n", buf);
	} else {
		WCN_INFO("unforce config xtal 26m clk:%d", clk->type);
	}

	marlin_dev->dvdd12 = devm_regulator_get(dev, "dvdd12");
	if (IS_ERR(marlin_dev->dvdd12)) {
		WCN_ERR("Get regulator of dvdd12 error!\n");
		WCN_ERR("Maybe share the power with mem\n");
	}

	if (of_property_read_bool(np, "bound-avdd12")) {
		WCN_INFO("forbid avdd12 power ctrl\n");
		marlin_dev->bound_avdd12 = true;
	} else {
		WCN_INFO("do avdd12 power ctrl\n");
		marlin_dev->bound_avdd12 = false;
	}

	marlin_dev->avdd12 = devm_regulator_get(dev, "avdd12");
	if (IS_ERR(marlin_dev->avdd12)) {
		WCN_ERR("avdd12 err =%ld\n", PTR_ERR(marlin_dev->avdd12));
		if (PTR_ERR(marlin_dev->avdd12) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		WCN_ERR("Get regulator of avdd12 error!\n");
	}

	marlin_dev->avdd33 = devm_regulator_get(dev, "avdd33");
	if (IS_ERR(marlin_dev->avdd33)) {
		if (PTR_ERR(marlin_dev->avdd33) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		WCN_ERR("Get regulator of avdd33 error!\n");
	}

	marlin_dev->dcxo18 = devm_regulator_get(dev, "dcxo18");
	if (IS_ERR(marlin_dev->dcxo18)) {
		if (PTR_ERR(marlin_dev->dcxo18) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		WCN_ERR("Get regulator of dcxo18 error!\n");
	}

	if (of_property_read_bool(np, "bound-dcxo18")) {
		WCN_INFO("forbid dcxo18 power ctrl\n");
		marlin_dev->bound_dcxo18 = true;
	} else {
		WCN_INFO("do dcxo18 power ctrl\n");
		marlin_dev->bound_dcxo18 = false;
	}

	marlin_dev->clk_32k = devm_clk_get(dev, "clk_32k");
	if (IS_ERR(marlin_dev->clk_32k)) {
		WCN_ERR("can't get wcn clock dts config: clk_32k\n");
		return -1;
	}

	marlin_dev->clk_parent = devm_clk_get(dev, "source");
	if (IS_ERR(marlin_dev->clk_parent)) {
		WCN_ERR("can't get wcn clock dts config: source\n");
		return -1;
	}
	clk_set_parent(marlin_dev->clk_32k, marlin_dev->clk_parent);

	marlin_dev->clk_enable = devm_clk_get(dev, "enable");
	if (IS_ERR(marlin_dev->clk_enable)) {
		WCN_ERR("can't get wcn clock dts config: enable\n");
		return -1;
	}
#endif /* end of CONFIG_WCN_PARSE_DTS and CONFIG_WCN_PMIC */

#ifdef CONFIG_WCN_PARSE_DTS
	marlin_dev->chip_en = devm_gpiod_get_index(dev, "wl-reg-on",
						  0, GPIOD_OUT_LOW);
	if (IS_ERR(marlin_dev->chip_en)) {
		if (PTR_ERR(marlin_dev->chip_en) == -ENOENT)
			marlin_dev->chip_en = NULL;
		else
			return PTR_ERR(marlin_dev->chip_en);
	}
#else
	marlin_dev->chip_en = NULL;
#endif

#ifdef CONFIG_WCN_PARSE_DTS
	marlin_dev->reset = devm_gpiod_get_index(dev, "bt-reg-on",
						  0, GPIOD_OUT_LOW);
	if (IS_ERR(marlin_dev->reset)) {
		if (PTR_ERR(marlin_dev->reset) == -ENOENT)
			marlin_dev->reset = NULL;
		else
			return PTR_ERR(marlin_dev->reset);
	}
#else
	marlin_dev->reset = NULL;
#endif

#ifdef CONFIG_WCN_PARSE_DTS
	int_ap = devm_gpiod_get_index_optional(dev, "pub-int", 0,
						       GPIOD_IN);
	if (IS_ERR(int_ap))
		return PTR_ERR(int_ap);
	marlin_dev->int_ap = int_ap ? desc_to_gpio(int_ap) : 0;
#else
	marlin_dev->int_ap = 0;
#endif
	if (marlin_dev->int_ap > 0) {
		WCN_INFO("%s int gpio=%d\n", __func__, marlin_dev->int_ap);
		if (!gpio_is_valid(marlin_dev->int_ap)) {
			WCN_ERR("int irq is invalid: %d\n",
				marlin_dev->int_ap);
			return -EINVAL;
		}
	}

#ifdef CONFIG_WCN_PARSE_DTS
#ifdef CONFIG_WCN_PMIC
	if (gpio_is_valid(clk->gpio)) {
		ret = gpio_request(clk->gpio, "wcn_xtal_26m_type");
		if (ret)
			WCN_ERR("xtal 26m gpio request err: %d\n", ret);
	}
#endif

	ret = of_property_read_string(np, "unisoc,btwf-file-name",
					  (const char **)&marlin_dev->btwf_path);
#if defined(CONFIG_RK_BOARD) && defined(CONFIG_WCN_SDIO)
	if (ret)
		marlin_dev->btwf_path = WCN_FW_NAME;
#endif
		WCN_DEBUG("btwf firmware path is %s\n", marlin_dev->btwf_path);

#else
		WCN_DEBUG("btwf firmware path is %s\n", WCN_FW_NAME);
#endif /* end of CONFIG_WCN_PARSE_DTS */

#ifdef CONFIG_WCN_PARSE_DTS
	if (of_property_read_bool(np, "keep-power-on")) {
		WCN_INFO("wcn config keep power on\n");
		marlin_dev->no_power_off = true;
	}
#else
#ifndef CONFIG_WCN_POWER_UP_DOWN
	WCN_INFO("wcn config keep power on\n");
	marlin_dev->no_power_off = true;
#endif
#endif

#ifdef CONFIG_WCN_PARSE_DTS
	if (of_property_read_bool(np, "bt-wake-host")) {
		struct gpio_desc *bt_wake_host_gpio;

		WCN_INFO("wcn config bt wake host\n");
		marlin_dev->bt_wl_wake_host_en |= BIT(BT_WAKE_HOST);
		bt_wake_host_gpio =
			devm_gpiod_get_index(dev, "bt-wake-host", 0,
						     GPIOD_IN);
		if (IS_ERR(bt_wake_host_gpio))
			return PTR_ERR(bt_wake_host_gpio);
	}
#else
#ifdef CONFIG_BT_WAKE_HOST_EN
	WCN_INFO("wcn config bt wake host\n");
	marlin_dev->bt_wl_wake_host_en |= BIT(BT_WAKE_HOST);
#endif /* end of CONFIG_BT_WAKE_HOST_EN */
#endif /* end of CONFIG_WCN_PARSE_DTS */

#ifdef CONFIG_WCN_PARSE_DTS
	if (of_property_read_bool(np, "wl-wake-host")) {
		struct gpio_desc *wl_wake_host_gpio;

		WCN_INFO("wcn config wifi wake host\n");
		marlin_dev->bt_wl_wake_host_en |= BIT(WL_WAKE_HOST);
		wl_wake_host_gpio =
			devm_gpiod_get_index(dev, "wl-wake-host", 0,
						     GPIOD_IN);
		if (IS_ERR(wl_wake_host_gpio))
			return PTR_ERR(wl_wake_host_gpio);
	}
#else
#ifdef CONFIG_WL_WAKE_HOST_EN
	WCN_INFO("wcn config wifi wake host\n");
	marlin_dev->bt_wl_wake_host_en |= BIT(WL_WAKE_HOST);
#endif /* end of CONFIG_WL_WAKE_HOST_EN */
#endif /* end of CONFIG_WCN_PARSE_DTS */

#if defined(CONFIG_WCN_PARSE_DTS) && defined(CONFIG_WCN_PMIC)
	pmu_apb_gpr = syscon_regmap_lookup_by_phandle(np,
				"unisoc,syscon-pmu-apb");
	if (IS_ERR(pmu_apb_gpr)) {
		WCN_ERR("%s:failed to find pmu_apb_gpr(26M)(ignore)\n",
				__func__);
		return -EINVAL;
	}
	ret = regmap_read(pmu_apb_gpr, REG_PMU_APB_XTL_WAIT_CNT0,
					&clk_wait_val);
	WCN_INFO("marlin2 clk_wait value is 0x%x\n", clk_wait_val);

	ret = of_property_read_u32(np, "unisoc,reg-m2-apb-xtl-wait-addr",
			&marlin2_clk_wait_reg);
	if (ret) {
		WCN_ERR("Did not find reg-m2-apb-xtl-wait-addr\n");
		return -EINVAL;
	}
	WCN_INFO("marlin2 clk reg is 0x%x\n", marlin2_clk_wait_reg);
#endif

	return 0;
}

static int marlin_gpio_free(struct device *dev)
{
	if (!marlin_dev)
		return -1;

	return 0;
}

static int marlin_clk_enable(bool enable)
{
	int ret = 0;

	return ret;

	if (enable) {
		ret = clk_prepare_enable(marlin_dev->clk_32k);
		ret = clk_prepare_enable(marlin_dev->clk_enable);
		WCN_INFO("marlin %s successfully!\n", __func__);
	} else {
		clk_disable_unprepare(marlin_dev->clk_enable);
		clk_disable_unprepare(marlin_dev->clk_32k);
	}

	return ret;
}

static int marlin_avdd18_dcxo_enable(bool enable)
{
	int ret = 0;

	return 0;

	WCN_INFO("avdd18_dcxo enable 1v8 %d\n", enable);
	if (marlin_dev->dcxo18 == NULL)
		return 0;

	if (enable) {
		if (regulator_is_enabled(marlin_dev->dcxo18))
			return 0;
		regulator_set_voltage(marlin_dev->dcxo18,
						  1800000, 1800000);
		ret = regulator_enable(marlin_dev->dcxo18);
		if (ret)
			WCN_ERR("fail to enable avdd18_dcxo\n");
	} else {
		if (regulator_is_enabled(marlin_dev->dcxo18)) {
			ret = regulator_disable(marlin_dev->dcxo18);
			if (ret)
				WCN_ERR("fail to disable avdd18_dcxo\n");
		}
	}

	return ret;
}

static int marlin_digital_power_enable(bool enable)
{
	int ret = 0;

	return ret;

	WCN_INFO("marlin_digital_power_enable D1v2 %d\n", enable);
	if (marlin_dev->dvdd12 == NULL)
		return 0;

	if (enable) {
		/* gpio_direction_output(marlin_dev->reset, 0); */

		regulator_set_voltage(marlin_dev->dvdd12,
						  1200000, 1200000);
		ret = regulator_enable(marlin_dev->dvdd12);
	} else {
		if (regulator_is_enabled(marlin_dev->dvdd12))
			ret = regulator_disable(marlin_dev->dvdd12);
	}

	return ret;
}
static int marlin_analog_power_enable(bool enable)
{
	int ret = 0;

	return ret;

	if (marlin_dev->avdd12 != NULL) {
		msleep(20);
		WCN_INFO("marlin_analog_power_enable 1v2 %d\n", enable);
		if (enable) {
			regulator_set_voltage(marlin_dev->avdd12,
			1200000, 1200000);
			ret = regulator_enable(marlin_dev->avdd12);
		} else {
			if (regulator_is_enabled(marlin_dev->avdd12))
				ret =
				regulator_disable(marlin_dev->avdd12);
		}
	}

	if (marlin_dev->avdd33_usb20 != NULL) {
		WCN_INFO(" avdd33_usb20 enable:%d\n", enable);
		if (enable) {
			regulator_set_voltage(marlin_dev->avdd33_usb20,
			3300000, 3300000);
			ret = regulator_enable(marlin_dev->avdd33_usb20);
		} else {
			if (regulator_is_enabled(marlin_dev->avdd33_usb20))
				ret =
				regulator_disable(marlin_dev->avdd33_usb20);
		}
	}

	return ret;
}

void marlin_read_cali_data(void)
{
	int err;

	WCN_INFO("marlin sync entry is_calibrated:%d\n",
		wifi_data.cali_data.cali_config.is_calibrated);

	if (!wifi_data.cali_data.cali_config.is_calibrated) {
		memset(&wifi_data.cali_data, 0x0,
			sizeof(struct wifi_cali_t));
		err = sprdwcn_bus_reg_read(CALI_OFSET_REG,
			&wifi_data.cali_data, sizeof(struct wifi_cali_t));
		if (err < 0) {
			WCN_ERR("marlin read cali data fail:%d\n", err);
			return;
		}
	}

	if ((marlin2_clk_wait_reg > 0) && (clk_wait_val > 0)) {
		sprdwcn_bus_reg_read(marlin2_clk_wait_reg,
					&cp_clk_wait_val, 4);
		WCN_INFO("marlin2 cp_clk_wait_val is 0x%x\n", cp_clk_wait_val);
		clk_wait_val = ((clk_wait_val & 0xFF00) >> 8);
		cp_clk_wait_val =
			((cp_clk_wait_val & 0xFFFFFC00) | clk_wait_val);
		WCN_INFO("marlin2 cp_clk_wait_val is modifyed 0x%x\n",
					cp_clk_wait_val);
		err = sprdwcn_bus_reg_write(marlin2_clk_wait_reg,
						   &cp_clk_wait_val, 4);
		if (err < 0)
			WCN_ERR("marlin2 write 26M error:%d\n", err);
	}

	/* write this flag to notify cp that ap read calibration data */
	reg_val = 0xbbbbbbbb;
	err = sprdwcn_bus_reg_write(CALI_REG, &reg_val, 4);
	if (err < 0) {
		WCN_ERR("marlin write cali finish error:%d\n", err);
		return;
	}

	sprdwcn_bus_runtime_get();

	//complete(&marlin_dev->download_done);
}

#ifdef CONFIG_WCN_SDIO
static void marlin_send_sdio_config_to_cp_vendor(void)
{
	union wcn_sdiohal_config sdio_cfg = {0};

#if (defined(CONFIG_RK_BOARD))
	/* Vendor config */

	/* bit[0]: sdio_config_en:
	 * 0: disable sdio config
	 * 1: enable sdio config
	 */
	sdio_cfg.cfg.sdio_config_en = 1;

	/* bit[3:1]: vendor_id:
	 * 000: default id, unisoc[0x0]
	 * 001: hisilicon default version, pull chipen after resume
	 * 010: hisilicon version, keep power (NOT pull chipen) and
	 *      reset sdio after resume
	 */
#if defined(CONFIG_WCN_RESUME_POWER_DOWN)
	sdio_cfg.cfg.vendor_id = WCN_VENDOR_RESUME_POWER_DOWN;
	WCN_INFO("sdio_config vendor:[power down after resume]\n");
#elif defined(CONFIG_WCN_RESUME_KEEPPWR_RESETSDIO)
	sdio_cfg.cfg.vendor_id = WCN_VENDOR_RESUME_KEEPPWR_RESETSDIO;
	WCN_INFO("sdio_config vendor:[keeppwr, reset sdio after resume]\n");
#else
	sdio_cfg.cfg.vendor_id = WCN_VENDOR_DEFAULT;
	WCN_DEBUG("sdio_config vendor:[default]\n");
#endif

	/* bit[4]: sdio_rx_mode: 0: adma; 1: sdma */
	if (sprdwcn_bus_get_rx_mode()) {
		sdio_cfg.cfg.sdio_rx_mode = 0;
		WCN_DEBUG("sdio_config rx mode:[adma]\n");
	} else {
		sdio_cfg.cfg.sdio_rx_mode = 1;
		WCN_INFO("sdio_config rx mode:[sdma]\n");
	}

	/* bit[7:5]: sdio_blk_size: 000: blocksize 840; 001: blocksize 512 */
	if (sprdwcn_bus_get_blk_size() == 512) {
		sdio_cfg.cfg.sdio_blk_size = 1;
		WCN_INFO("sdio_config blksize:[512]\n");
	} else
		WCN_DEBUG("sdio_config blksize:[840]\n");

	/*
	 * bit[8]: bt_wake_host_en: 0: disable, 1: enable
	 *
	 * When bit[8] is 1, bit[10:9] region will be parsed:
	 * bit[10:9]: bt_wake_host_trigger_type:
	 * 00:BT_WAKEUP_HOST  trigger type low
	 * 01:BT_WAKEUP_HOST  trigger type rising
	 * 10:BT_WAKEUP_HOST  trigger type falling
	 * 11:BT_WAKEUP_HOST  trigger type high
	 */
	if (marlin_get_bt_wl_wake_host_en() & BIT(BT_WAKE_HOST)) {
		sdio_cfg.cfg.bt_wake_host_en = 1;
		WCN_DEBUG("sdio_config bt_wake_host:[en]\n");
		sdio_cfg.cfg.bt_wake_host_trigger_type = 3;
		WCN_INFO("sdio_config bt_wake_host trigger:[high]\n");
	}

	/* bit[12:11]: sdio_irq_type:
	 * 00:dedicated irq, gpio1
	 * 01:inband data1 irq
	 * 10:use BT_WAKEUP_HOST(pubint) pin as gpio irq
	 * 11:use WL_WAKEUP_HOST(esmd3) pin as gpio irq
	 */
	if (sprdwcn_bus_get_irq_type() != 0) {
		sdio_cfg.cfg.sdio_irq_type = 1;
		WCN_INFO("sdio_config irq:[inband]\n");
	} else {
#ifdef CONFIG_CUSTOMIZE_SDIO_IRQ_TYPE
		sdio_cfg.cfg.sdio_irq_type = CONFIG_CUSTOMIZE_SDIO_IRQ_TYPE;
		if (sdio_cfg.cfg.sdio_irq_type == 0)
			WCN_INFO("sdio_config sdio_irq:[gpio1]\n");
		else if (sdio_cfg.cfg.sdio_irq_type == 2)
			WCN_INFO("sdio_config sdio_irq:[pubint]\n");
		else if (sdio_cfg.cfg.sdio_irq_type == 3)
			WCN_INFO("sdio_config sdio_irq:[esmd3]\n");
		else
			WCN_INFO("sdio_config sdio_irq:[error]\n");
#else
		sdio_cfg.cfg.sdio_irq_type = 0;
		WCN_INFO("sdio_config sdio_irq:[gpio1]\n");
#endif
	}

	/*
	 * When bit[12:11] is 10/11, bit[14:13] region will be parsed:
	 * bit[14:13]: sdio_irq_trigger_type:
	 * 00:pubint gpio irq trigger type low
	 * 01:pubint gpio irq trigger type rising [NOT support]
	 * 10:pubint gpio irq trigger type falling [NOT support]
	 * 11:pubint gpio irq trigger type high
	 */

	 /*
	 * bit[15]: wl_wake_host_en: 0: disable, 1: enable
	 *
	 * When bit[15] is 1, bit[17:16] region will be parsed:
	 * bit[17:16]: wl_wake_host_trigger_type:
	 * 00:WL_WAKEUP_HOST  trigger type low
	 * 01:WL_WAKEUP_HOST  trigger type rising
	 * 10:WL_WAKEUP_HOST  trigger type falling
	 * 11:WL_WAKEUP_HOST  trigger type high
	 */
	if (marlin_get_bt_wl_wake_host_en() & BIT(WL_WAKE_HOST)) {
		sdio_cfg.cfg.wl_wake_host_en = 1;
		WCN_DEBUG("sdio_config wl_wake_host:[en]\n");
		sdio_cfg.cfg.wl_wake_host_trigger_type = 3;
		WCN_INFO("sdio_config wl_wake_host trigger:[high]\n");
	}

	/*
	 * bit[22:18]: wake_host_level_duration_10s: BT_WAKEUP_HOST or
	 * WL_WAKEUP_HOST level dyration time per 10ms,
	 * example: 0:0ms; 3:30ms; 20:200ms
	 */
	sdio_cfg.cfg.wake_host_level_duration_10ms = 2;
	WCN_INFO("sdio_config wake_host_level_duration_time:[%dms]\n",
		 (sdio_cfg.cfg.wake_host_level_duration_10ms * 10));

	/*
	 * bit[23]: wake_host_data_separation:
	 * 0: if BT_WAKEUP_HOST en or WL_WAKEUP_HOST en,
	 *    wifi and bt packets can wake host;
	 * 1: if BT_WAKEUP_HOST en, ONLY bt packets can wake host;
	 *    if WL_WAKEUP_HOST en, ONLY wifi packets can wake host
	 */
#if (defined(CONFIG_RK_BOARD))
	sdio_cfg.cfg.wake_host_data_separation = 1;
	WCN_DEBUG("sdio_config wake_host_data_separation:[yes]\n");
#else
	WCN_INFO("sdio_config wake_host_data_separation:[bt/wifi reuse]\n");
#endif

#else
	/* Default config */
	sdio_cfg.val = 0;
#endif

	marlin_dev->sync_f.sdio_config = sdio_cfg.val;
}

static int marlin_send_sdio_config_to_cp(void)
{
	int sdio_config_off = 0;

	sdio_config_off = (unsigned long)(&(marlin_dev->sync_f.sdio_config)) -
		(unsigned long)(&(marlin_dev->sync_f));
	WCN_DEBUG("sdio_config_offset:0x%x\n", sdio_config_off);

	marlin_send_sdio_config_to_cp_vendor();

	WCN_INFO("%s sdio_config:0x%x (%sable config)\n",
		 __func__, marlin_dev->sync_f.sdio_config,
		 (marlin_dev->sync_f.sdio_config & BIT(0)) ? "en" : "dis");

	return sprdwcn_bus_reg_write(SYNC_ADDR + sdio_config_off,
					 &(marlin_dev->sync_f.sdio_config), 4);
}
#endif
static int marlin_write_cali_data(void)
{
	int ret = 0, init_state = 0;
	int i = 0;

	do {
		i++;
		ret = sprdwcn_bus_reg_read(SYNC_ADDR, &init_state, 4);
		if (ret < 0) {
			WCN_ERR("%s marlin3 read SYNC_ADDR error:%d\n",
				__func__, ret);
			return ret;
		}
		WCN_INFO("%s sync init_state:0x%x\n", __func__, init_state);

		if (init_state != SYNC_CALI_WAITING)
			msleep(20);
		/* wait cp in the state of waiting cali data */
		else {
#ifdef CONFIG_WCN_SDIO
			/*write sdio config to cp*/
			ret = marlin_send_sdio_config_to_cp();
			if (ret < 0) {
				WCN_ERR("write sdio_config error:%d\n", ret);
				return ret;
			}
#endif
			/*tell cp2 can handle cali data*/
			init_state = SYNC_CALI_WRITE_DONE;
			ret = sprdwcn_bus_reg_write(SYNC_ADDR, &init_state, 4);
			if (ret < 0) {
				WCN_ERR("write cali_done flag error:%d\n", ret);
				return ret;
			}

			i = 0;
			WCN_INFO("marlin_write_cali_data finish\n");
			return ret;
		}

		if (i > 10)
			i = 0;
	} while (i);

	return ret;

}

enum wcn_clock_type wcn_get_xtal_26m_clk_type(void)
{
	return marlin_dev->clk_xtal_26m.type;
}
EXPORT_SYMBOL_GPL(wcn_get_xtal_26m_clk_type);

enum wcn_clock_mode wcn_get_xtal_26m_clk_mode(void)
{
	return marlin_dev->clk_xtal_26m.mode;
}
EXPORT_SYMBOL_GPL(wcn_get_xtal_26m_clk_mode);

static int spi_read_rf_reg(unsigned int addr, unsigned int *data)
{
	unsigned int reg_data = 0;
	int ret;

	reg_data = ((addr & 0x7fff) << 16) | SPI_BIT31;
	ret = sprdwcn_bus_reg_write(SPI_BASE_ADDR, &reg_data, 4);
	if (ret < 0) {
		WCN_ERR("write SPI RF reg error:%d\n", ret);
		return ret;
	}

	usleep_range(4000, 6000);

	ret = sprdwcn_bus_reg_read(SPI_BASE_ADDR, &reg_data, 4);
	if (ret < 0) {
		WCN_ERR("read SPI RF reg error:%d\n", ret);
		return ret;
	}
	*data = reg_data & 0xffff;

	return 0;
}

static int check_cp_clock_mode(void)
{
	int ret = 0;
	unsigned int temp_val;


	ret = spi_read_rf_reg(AD_DCXO_BONDING_OPT, &temp_val);
	if (ret < 0) {
		WCN_ERR("read AD_DCXO_BONDING_OPT error:%d\n", ret);
		return ret;
	}
	WCN_DEBUG("read AD_DCXO_BONDING_OPT val:0x%x\n", temp_val);
	if ((temp_val & tsx_mode) == tsx_mode)
		WCN_INFO("clock mode: TSX\n");
	else {
		WCN_INFO("clock mode: TCXO, outside clock\n");
		//marlin_avdd18_dcxo_enable(false);
	}

	return ret;
}

/* release CPU */
static int marlin_start_run(void)
{
	int ret = 0;
	unsigned int ss_val;

	WCN_DEBUG("marlin_start_run\n");

#ifdef CONFIG_WCN_SLP
	sdio_pub_int_btwf_en0();
	/* after chip power on, reset sleep status */
	slp_mgr_reset();
#endif

	ret = sprdwcn_bus_reg_read(CP_RESET_REG, &ss_val, 4);
	if (ret < 0) {
		WCN_ERR("%s read reset reg error:%d\n", __func__, ret);
		return ret;
	}
	WCN_INFO("%s read reset reg val:0x%x\n", __func__, ss_val);
	ss_val  &= (~(RESET_BIT));
	WCN_INFO("after do %s reset reg val:0x%x\n", __func__, ss_val);
	ret = sprdwcn_bus_reg_write(CP_RESET_REG, &ss_val, 4);
	if (ret < 0) {
		WCN_ERR("%s write reset reg error:%d\n", __func__, ret);
		return ret;
	}
	ret = sprdwcn_bus_reg_read(CP_RESET_REG, &ss_val, 4);
	if (ret < 0) {
		WCN_ERR("%s read reset reg error:%d\n", __func__, ret);
		return ret;
	}
	WCN_DEBUG("%s reset reg val:0x%x\n", __func__, ss_val);

	return ret;
}

//#if IS_ENABLED(CONFIG_AW_BIND_VERIFY)
#include <crypto/sha2.h>

static void expand_seed(u8 *seed, u8 *out)
{
	unsigned char hash[64];
	int i;

	sha256(seed, 4, hash);

	for (i = 0; i < 4; i++) {
		memcpy(&out[i * 9], &hash[i * 8], 8);
		out[i * 9 + 8] = seed[i];
	}
}

static int wcn_bind_verify_calculate_verify_data(u8 *in, u8 *out)
{
	u8 seed[4], buf[36], a, b, c;
	int i, n;

	for (i = 0, n = 0; i < 4; i++) {
		a = in[n++];
		b = in[n++];
		c = in[n++];
		seed[i] = (a & b) ^ (a & c) ^ (b & c) ^ in[i + 12];
	}

	expand_seed(seed, buf);

	for (i = 0, n = 0; i < 12; i++) {
		a = buf[n++];
		b = buf[n++];
		c = buf[n++];
		out[i] = (a & b) ^ (a & c) ^ (b & c);
	}

	for (i = 0, n = 0; i < 4; i++) {
		a = out[n++];
		b = out[n++];
		c = out[n++];
		seed[i] = (a & b) ^ (~a & c);
	}

	expand_seed(seed, buf);

	for (i = 0, n = 0; i < 12; i++) {
		a = buf[n++];
		b = buf[n++];
		c = buf[n++];
		out[i] = (a & b) ^ (~a & c);
	}

	for (i = 0, n = 0; i < 4; i++) {
		a = out[n++];
		b = out[n++];
		c = out[n++];
		out[i + 12] = (a & b) ^ (a & c) ^ (b & c) ^ seed[i];
	}

	return 0;
}

static int marlin_bind_verify(void)
{
	unsigned char din[16], dout[16];
	int bind_verify_data_off = 0, init_state, ret = 0;

	/*transform confuse data to verify data*/
	memcpy(din, &marlin_dev->sync_f.bind_verify_data[0], 16);
	WCN_INFO("%s confuse data: 0x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
		 __func__,
		 din[0], din[1], din[2], din[3],
		 din[4], din[5], din[6], din[7],
		 din[8], din[9], din[10], din[11],
		 din[12], din[13], din[14], din[15]);
	wcn_bind_verify_calculate_verify_data(din, dout);
	WCN_INFO("%s verify data: 0x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
		 __func__,
		 dout[0], dout[1], dout[2], dout[3],
		 dout[4], dout[5], dout[6], dout[7],
		 dout[8], dout[9], dout[10], dout[11],
		 dout[12], dout[13], dout[14], dout[15]);

	/*send bind verify data to cp2*/
	memcpy(&marlin_dev->sync_f.bind_verify_data[0], dout, 16);
	bind_verify_data_off = (unsigned long)
		(&(marlin_dev->sync_f.bind_verify_data[0])) -
		(unsigned long)(&(marlin_dev->sync_f));
	ret = sprdwcn_bus_direct_write(SYNC_ADDR + bind_verify_data_off,
		&(marlin_dev->sync_f.bind_verify_data[0]), 16);
	if (ret < 0) {
		WCN_ERR("write bind verify data error:%d\n", ret);
		return ret;
	}

	/*tell cp2 can handle bind verify data*/
	init_state = SYNC_VERIFY_WRITE_DONE;
	ret = sprdwcn_bus_reg_write(SYNC_ADDR, &init_state, 4);
	if (ret < 0) {
		WCN_ERR("write bind verify flag error:%d\n", ret);
		return ret;
	}

	return ret;
}
//#endif

static int check_cp_ready(void)
{
	int ret = 0;
	int i = 0;
	do {
		i++;
		ret = sprdwcn_bus_direct_read(SYNC_ADDR,
			&(marlin_dev->sync_f), sizeof(struct wcn_sync_info_t));
		if (ret < 0) {
			WCN_ERR("%s marlin3 read SYNC_ADDR error:%d\n",
				__func__, ret);
			return ret;
		}
		WCN_INFO("%s sync val:0x%x, prj_type val:0x%x\n", __func__,
				marlin_dev->sync_f.init_status,
				marlin_dev->sync_f.prj_type);
		if (marlin_dev->sync_f.init_status == SYNC_ALL_FINISHED)
			i = 0;
//#ifdef CONFIG_AW_BIND_VERIFY
		else if (marlin_dev->sync_f.init_status ==
			SYNC_VERIFY_WAITING) {
			ret = marlin_bind_verify();
			if (ret != 0) {
				WCN_ERR("%s bind verify error:%d\n",
					__func__, ret);
				return ret;
			}
		}
//#endif
		else
			msleep(20);
		if (i > 10)
			return -1;
	} while (i);

	return 0;
}

static int chip_reset_release(int val)
{
	static unsigned int reset_count;

	if (!marlin_dev->reset)
		return 0;

	if (val) {
		if (reset_count == 0)
			gpiod_set_value_cansleep(marlin_dev->reset, 1);
		reset_count++;
	} else {
		gpiod_set_value_cansleep(marlin_dev->reset, 0);
		reset_count--;
	}

	return 0;
}
void marlin_chip_en(bool enable, bool reset)
{
	static unsigned int chip_en_count;

	/*
	 * Incar board pull chipen gpio at pin control.
	 * Hisi board pull chipen gpio at hi_sdio_detect.ko.
	 */
	if (!marlin_dev->chip_en)
		return;

	if (reset) {
			gpiod_set_value_cansleep(marlin_dev->chip_en, 0);
			WCN_INFO("marlin chip en reset\n");
			msleep(100);
			gpiod_set_value_cansleep(marlin_dev->chip_en, 1);
		} else if (enable) {
			if (chip_en_count == 0) {
				gpiod_set_value_cansleep(marlin_dev->chip_en, 0);
				mdelay(1);
				gpiod_set_value_cansleep(marlin_dev->chip_en, 1);
				mdelay(1);
				WCN_INFO("marlin chip en pull up\n");
			}
			chip_en_count++;
		} else {
			chip_en_count--;
			if (chip_en_count == 0) {
				gpiod_set_value_cansleep(marlin_dev->chip_en, 0);
				WCN_INFO("marlin chip en pull down\n");
			}
		}
}
EXPORT_SYMBOL_GPL(marlin_chip_en);

int set_cp_mem_status(int subsys, int val)
{
	int ret;
	unsigned int temp_val;

	return 0;

	ret = sprdwcn_bus_reg_read(REG_WIFI_MEM_CFG1, &temp_val, 4);
	if (ret < 0) {
		WCN_ERR("%s read wifimem_cfg1 error:%d\n", __func__, ret);
		return ret;
	}
	WCN_INFO("%s read btram poweron(bit22)val:0x%x\n", __func__, temp_val);

	if ((subsys == MARLIN_BLUETOOTH) && (val == 1)) {
		temp_val = temp_val & (~FORCE_SHUTDOWN_BTRAM);
		WCN_INFO("wr btram poweron(bit22) val:0x%x\n", temp_val);
		ret = sprdwcn_bus_reg_write(REG_WIFI_MEM_CFG1, &temp_val, 4);
		if (ret < 0) {
			WCN_ERR("write wifimem_cfg1 reg error:%d\n", ret);
			return ret;
		}
		return 0;
	} else if (test_bit(MARLIN_BLUETOOTH, &marlin_dev->power_state) &&
		   (subsys != MARLIN_BLUETOOTH))
		return 0;

	temp_val = temp_val | FORCE_SHUTDOWN_BTRAM;
	WCN_INFO(" shut down btram(bit22) val:0x%x\n", temp_val);
	ret = sprdwcn_bus_reg_write(REG_WIFI_MEM_CFG1, &temp_val, 4);
	if (ret < 0) {
		WCN_ERR("write wifimem_cfg1 reg error:%d\n", ret);
		return ret;
	}

	return ret;
}

static void marlin_scan_finish(void)
{
	WCN_INFO("marlin_scan_finish!\n");
	complete(&marlin_dev->carddetect_done);
}

static void pre_btwifi_download_sdio(struct work_struct *work)
{
	int ret;

	wcn_get_chip_name();

	ret = btwifi_download_firmware();
	if (!ret)
		ret = marlin_start_run();
	if (!ret) {
		check_cp_clock_mode();
		marlin_write_cali_data();
		/* check_cp_ready must be in front of mem_pd_save_bin,
		 * save bin task is scheduled after verify.
		 */
		ret = check_cp_ready();
		if (ret) {
			sprdwcn_bus_set_carddump_status(true);
			goto out;
		}

		sprdwcn_bus_runtime_get();

#ifndef CONFIG_WCND
#ifndef CONFIG_CPLOG_DEBUG
#endif
#endif
	}
out:
	marlin_dev->download_result = ret;
	complete(&marlin_dev->download_done);
}

/* for example: wifipa bound XTLEN3 */
int pmic_bound_xtl_assert(unsigned int enable)
{
#ifdef CONFIG_WCN_PMIC
	unsigned int val;

	regmap_read(reg_map, ANA_REG_GLB_LDO_XTL_EN10, &val);
	WCN_INFO("%s:%d, XTL_EN10 =0x%x\n", __func__, enable, val);
	regmap_update_bits(reg_map,
			   ANA_REG_GLB_LDO_XTL_EN10,
			   BIT_LDO_WIFIPA_EXT_XTL3_EN,
			   enable);
	regmap_read(reg_map, ANA_REG_GLB_LDO_XTL_EN10, &val);
	WCN_INFO("after XTL_EN10 =0x%x\n", val);
#endif
	return 0;
}

void wifipa_enable(int enable)
{
	int ret = -1;

	return;

	if (marlin_dev->avdd33) {
		WCN_INFO("marlin_analog_power_enable 3v3 %d\n", enable);
		msleep(20);
		if (enable) {
			if (regulator_is_enabled(marlin_dev->avdd33))
				return;
			regulator_set_voltage(marlin_dev->avdd33,
						  3300000, 3300000);
			ret = regulator_enable(marlin_dev->avdd33);
			if (ret)
				WCN_ERR("fail to enable wifipa\n");
		} else {
			if (regulator_is_enabled(marlin_dev->avdd33)) {
				ret =
				regulator_disable(marlin_dev->avdd33);
				if (ret)
					WCN_ERR("fail to disable wifipa\n");
				WCN_INFO(" wifi pa disable\n");
			}
		}
	}
}


void set_wifipa_status(int subsys, int val)
{
	return;

	if (val == 1) {
		if (((subsys == MARLIN_BLUETOOTH) || (subsys == MARLIN_WIFI)) &&
			((marlin_dev->power_state & 0x5) == 0)) {
			wifipa_enable(1);
			pmic_bound_xtl_assert(1);
		}

		if (((subsys != MARLIN_BLUETOOTH) && (subsys != MARLIN_WIFI)) &&
			((marlin_dev->power_state & 0x5) == 0)) {
			wifipa_enable(0);
			pmic_bound_xtl_assert(0);
		}

	} else {
		if (((subsys == MARLIN_BLUETOOTH) &&
			 ((marlin_dev->power_state & 0x4) == 0)) ||
			((subsys == MARLIN_WIFI) &&
			 ((marlin_dev->power_state & 0x1) == 0))) {
			wifipa_enable(0);
			pmic_bound_xtl_assert(0);
		}
	}
}

/*
 * RST_N (LOW)
 * VDDIO -> DVDD12/11 ->CHIP_EN ->DVDD_CORE(inner)
 * ->(>=550uS) RST_N (HIGH)
 * ->(>=100uS) ADVV12
 * ->(>=10uS)  AVDD33
 */
int chip_power_on(int subsys)
{
	if (!sdiohal_is_ready())
		return -ENODEV;
	marlin_avdd18_dcxo_enable(true);
	marlin_clk_enable(true);
	marlin_digital_power_enable(true);
	marlin_chip_en(true, false);
	msleep(20);
	chip_reset_release(1);
	marlin_analog_power_enable(true);
	wifipa_enable(1);
#ifdef CONFIG_WCN_SDIO
	if (marlin_dev->int_ap > 0)
		sdio_pub_int_poweron(true);
#endif

	return 0;
}

int chip_power_off(int subsys)
{

	marlin_avdd18_dcxo_enable(false);
	marlin_clk_enable(false);
	marlin_chip_en(false, false);
	marlin_digital_power_enable(false);
	marlin_analog_power_enable(false);
	chip_reset_release(0);
	marlin_dev->wifi_need_download_ini_flag = 0;
	marlin_dev->power_state = 0;
#ifdef CONFIG_WCN_SDIO
	if (marlin_dev->int_ap > 0)
		sdio_pub_int_poweron(false);
#endif

	return 0;
}

int open_power_ctl(void)
{
	marlin_dev->no_power_off = 0;
	clear_bit(WCN_AUTO, &marlin_dev->power_state);

	return 0;
}
EXPORT_SYMBOL_GPL(open_power_ctl);

void marlin_schedule_download_wq(void)
{
	unsigned long timeleft;

	marlin_dev->wifi_need_download_ini_flag = 0;
	schedule_work(&marlin_dev->download_wq);
	timeleft = wait_for_completion_timeout(
		&marlin_dev->download_done,
		msecs_to_jiffies(POWERUP_WAIT_MS));
	if (!timeleft) {
		WCN_ERR("marlin download timeout\n");
	} else if (marlin_dev->download_result)
		WCN_ERR("marlin download failed: %d\n", marlin_dev->download_result);

}

static int marlin_set_power(int subsys, int val)
{
	unsigned long timeleft;
	int ret = -EIO;
	bool chip_powered = false;

	WCN_DEBUG("mutex_lock\n");
	mutex_lock(&marlin_dev->power_lock);

	WCN_INFO("marlin power state:%lx, subsys: [%s] power %d\n",
			marlin_dev->power_state, strno(subsys), val);
	init_completion(&marlin_dev->download_done);

	/*  power on */
	if (val) {
		/* First open downloads the BT/Wi-Fi firmware. */
		marlin_dev->first_power_on_flag++;
		if (marlin_dev->first_power_on_flag == 1) {
			WCN_INFO("the first power on start\n");
			ret = chip_power_on(subsys);
			if (ret < 0) {
				WCN_ERR("chip power on fail\n");
				goto out;
			}
			chip_powered = true;
			set_wifipa_status(subsys, val);
			set_bit(subsys, &marlin_dev->power_state);
			WCN_INFO("then marlin start to download\n");
			schedule_work(&marlin_dev->download_wq);
			timeleft = wait_for_completion_timeout(
				&marlin_dev->download_done,
				msecs_to_jiffies(POWERUP_WAIT_MS));
			if (!timeleft) {
				WCN_ERR("marlin download timeout\n");
				ret = -ETIMEDOUT;
				cancel_work_sync(&marlin_dev->download_wq);
				goto out;
			}
			ret = marlin_dev->download_result;
			if (ret)
				goto out;
			marlin_dev->download_finish_flag = 1;
			WCN_INFO("then marlin download finished and run ok\n");
			marlin_dev->first_power_on_flag = 2;
			WCN_DEBUG("mutex_unlock\n");
			mutex_unlock(&marlin_dev->power_lock);
			return 0;
		}
		/* WCN_AUTO is retained for power-state compatibility. */
		else if (subsys == WCN_AUTO) {
			if (marlin_dev->no_power_off) {
				WCN_INFO("have power on, no action\n");
				set_wifipa_status(subsys, val);
				set_bit(subsys, &marlin_dev->power_state);
			}
		}
		else if ((marlin_dev->power_state & AUTO_RUN_MASK) != 0) {
			WCN_INFO("auto-run subsystem is already active\n");
			set_wifipa_status(subsys, val);
			set_bit(subsys, &marlin_dev->power_state);

			goto check_power_state_notify;
		}
		else if (((marlin_dev->power_state) & MARLIN_MASK) != 0) {
			WCN_INFO("marlin is already powered on\n");
			set_wifipa_status(subsys, val);
			set_bit(subsys, &marlin_dev->power_state);

			goto check_power_state_notify;
		}
		else {
			WCN_INFO("no module to power on, start to power on\n");
			ret = chip_power_on(subsys);
			if (ret < 0) {
				WCN_ERR("chip power on fail\n");
				goto out;
			}
			chip_powered = true;
			set_wifipa_status(subsys, val);
			set_bit(subsys, &marlin_dev->power_state);

			WCN_INFO("BT/Wi-Fi start to download\n");
			schedule_work(&marlin_dev->download_wq);
			timeleft = wait_for_completion_timeout(
				&marlin_dev->download_done,
				msecs_to_jiffies(POWERUP_WAIT_MS));
			if (!timeleft) {
				WCN_ERR("marlin download timeout\n");
				ret = -ETIMEDOUT;
				cancel_work_sync(&marlin_dev->download_wq);
				goto out;
			}
			ret = marlin_dev->download_result;
			if (ret)
				goto out;
			marlin_dev->download_finish_flag = 1;
			WCN_INFO("BT/Wi-Fi download finished and run ok\n");
		}

		WCN_INFO("wcn chip power on and run finish: [%s]\n",
				  strno(subsys));
	/* power off */
	} else {
		if (marlin_dev->power_state == 0)
			goto check_power_state_notify;

		if (flag_reset)
			marlin_dev->power_state = 0;

		if (marlin_dev->no_power_off) {
			if (!flag_reset) {
				if (subsys != WCN_AUTO) {
					/* in order to not download again */
					set_bit(WCN_AUTO,
						&marlin_dev->power_state);
					clear_bit(subsys,
						&marlin_dev->power_state);
				}

				MDBG_LOG("marlin reset flag_reset:%d\n",
					flag_reset);

				goto check_power_state_notify;
			}
		}

		if (!marlin_dev->download_finish_flag)
			goto check_power_state_notify;

		set_wifipa_status(subsys, val);
		clear_bit(subsys, &marlin_dev->power_state);
		if (marlin_dev->power_state != 0) {
			WCN_INFO("can not power off, other module is on\n");
			goto check_power_state_notify;
		}

		set_cp_mem_status(subsys, val);
		WCN_INFO("wcn chip start power off!\n");
		sprdwcn_bus_runtime_put();
		chip_power_off(subsys);
		WCN_INFO("marlin power off!\n");
		marlin_dev->download_finish_flag = 0;
		if (flag_reset)
			flag_reset = FALSE;
	} /* power off end */

	/* power on off together's Action */
	WCN_DEBUG("mutex_unlock\n");
	mutex_unlock(&marlin_dev->power_lock);

	return 0;

out:
	if (marlin_dev->first_power_on_flag == 1)
		marlin_dev->first_power_on_flag = 0;
	if (marlin_dev->download_finish_flag) {
		sprdwcn_bus_runtime_put();
		marlin_dev->download_finish_flag = 0;
	}
	if (chip_powered)
		chip_power_off(subsys);
	marlin_dev->power_state = 0;
	WCN_DEBUG("mutex_unlock\n");
	mutex_unlock(&marlin_dev->power_lock);

	return ret ? ret : -EIO;

check_power_state_notify:
	WCN_DEBUG("mutex_unlock\n");
	mutex_unlock(&marlin_dev->power_lock);

	return 0;
}

void marlin_power_off(enum marlin_sub_sys subsys)
{
	WCN_INFO("%s all\n", __func__);

	marlin_dev->no_power_off = false;
	set_bit(subsys, &marlin_dev->power_state);
	marlin_set_power(subsys, false);
}

int marlin_get_power(enum marlin_sub_sys subsys)
{
	if (subsys == MARLIN_ALL)
		return marlin_dev->power_state != 0;
	else
		return test_bit(subsys, &marlin_dev->power_state);
}
EXPORT_SYMBOL_GPL(marlin_get_power);

int marlin_get_module_status(void)
{
	if (test_bit(MARLIN_BLUETOOTH, &marlin_dev->power_state) ||
		test_bit(MARLIN_FM, &marlin_dev->power_state) ||
		test_bit(MARLIN_WIFI, &marlin_dev->power_state) ||
		test_bit(MARLIN_MDBG, &marlin_dev->power_state) ||
		test_bit(WCN_AUTO, &marlin_dev->power_state))
		return 1;
	return 0;
}
EXPORT_SYMBOL_GPL(marlin_get_module_status);

int is_first_power_on(enum marlin_sub_sys subsys)
{
	if (marlin_dev->wifi_need_download_ini_flag == 1)
		return 1;	/* the first */
	else
		return 0;	/* not the first */
}
EXPORT_SYMBOL_GPL(is_first_power_on);

void marlin_wifi_ini_failed(void)
{
	marlin_dev->wifi_need_download_ini_flag = 0;
}
EXPORT_SYMBOL_GPL(marlin_wifi_ini_failed);

int cali_ini_need_download(enum marlin_sub_sys subsys)
{
	unsigned int pd_wifi_st = 0;


	if ((marlin_dev->wifi_need_download_ini_flag == 1) || pd_wifi_st) {
		WCN_INFO("cali_ini_need_download return 1\n");
		return 1;	/* the first */
	}
	return 0;	/* not the first */
}
EXPORT_SYMBOL_GPL(cali_ini_need_download);

int marlin_set_wakeup(enum marlin_sub_sys subsys)
{
	int ret = 0;	/* temp */

	return 0;
	if (unlikely(marlin_dev->download_finish_flag != true))
		return -1;

	return ret;
}
EXPORT_SYMBOL_GPL(marlin_set_wakeup);

int marlin_set_sleep(enum marlin_sub_sys subsys, bool enable)
{
	return 0;	/* temp */

	if (unlikely(marlin_dev->download_finish_flag != true))
		return -1;

	return 0;
}
EXPORT_SYMBOL_GPL(marlin_set_sleep);

/* Temporary modification for UWE5623:
 * cmd52 read/write timeout -110 issue.
 */
void marlin_read_test_after_reset(void)
{
	int ret;
	unsigned int reg_addr = AON_APB_TEST_READ_REG, reg_val;

	ret = sprdwcn_bus_reg_read(reg_addr, &reg_val, 4);
	if (ret < 0)
		WCN_ERR("%s read 0x%x error:%d\n", __func__, reg_addr, ret);
	else
		WCN_INFO("%s read 0x%x = 0x%x\n", __func__, reg_addr, reg_val);
}

int marlin_reset_reg(void)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(marlin_reset_reg);

int start_marlin(u32 subsys)
{
	int ret;
	WCN_INFO("%s [%s]\n", __func__, strno(subsys));
	if (sprdwcn_bus_get_carddump_status() != 0) {
		WCN_ERR("%s SDIO card dump\n", __func__);
		return -1;
	}

	if (subsys == MARLIN_WIFI) {
		/* not need write cali */
		if (marlin_dev->wifi_need_download_ini_flag == 0)
			/* need write cali */
			marlin_dev->wifi_need_download_ini_flag = 1;
		else
			/* not need write cali */
			marlin_dev->wifi_need_download_ini_flag = 2;
	}
	ret = marlin_set_power(subsys, true);
	if (ret)
		return ret;


	return 0;
}
EXPORT_SYMBOL_GPL(start_marlin);

int stop_marlin(u32 subsys)
{
	if (sprdwcn_bus_get_carddump_status() != 0) {
		WCN_ERR("%s SDIO card dump\n", __func__);
		return -1;
	}

	return marlin_set_power(subsys, false);
}
EXPORT_SYMBOL_GPL(stop_marlin);

static const struct mfd_cell marlin_cells[] = {
	{
		.name = "unisoc_wifi",
		.of_compatible = "sprd,unisoc-wifi",
	},
	{
		.name = "mtty",
		.of_compatible = "sprd,mtty",
	},
};

int marlin_core_init(struct device *dev)
{
	int ret;
#ifdef CONFIG_WCN_PMIC
	struct device_node *regmap_np;
	struct platform_device *pdev_regmap = NULL;
#endif

	marlin_dev = devm_kzalloc(dev,
			sizeof(struct marlin_device), GFP_KERNEL);
	if (!marlin_dev)
		return -ENOMEM;
	marlin_dev->dev = dev;
	dev_set_drvdata(dev, marlin_dev);
	marlin_dev->write_buffer = devm_kzalloc(dev,
			PACKET_SIZE, GFP_KERNEL);
	if (marlin_dev->write_buffer == NULL) {
		dev_set_drvdata(dev, NULL);
		devm_kfree(dev, marlin_dev);
		marlin_dev = NULL;
		WCN_ERR("marlin_probe write buffer low memory\n");
		return -ENOMEM;
	}
	mutex_init(&(marlin_dev->power_lock));
	marlin_dev->power_state = 0;
	ret = marlin_parse_dt(dev);
	if (ret) {
		mutex_destroy(&marlin_dev->power_lock);
		devm_kfree(dev, marlin_dev->write_buffer);
		dev_set_drvdata(dev, NULL);
		devm_kfree(dev, marlin_dev);
		marlin_dev = NULL;
		return ret;
	}
	if (marlin_dev->reset)
		gpiod_direction_output(marlin_dev->reset, 0);
	init_completion(&marlin_dev->carddetect_done);
#ifdef CONFIG_WCN_SLP
	slp_mgr_init();
#endif
	/* register ops */
	wcn_bus_init();
	sprdwcn_bus_register_rescan_cb(marlin_scan_finish);
	/* Initialize the selected bus implementation. */
	ret = sprdwcn_bus_preinit(dev);
	if (ret) {
		wcn_bus_deinit();
#ifdef CONFIG_WCN_SLP
		slp_mgr_deinit();
#endif
		mutex_destroy(&marlin_dev->power_lock);
		devm_kfree(dev, marlin_dev->write_buffer);
		dev_set_drvdata(dev, NULL);
		devm_kfree(dev, marlin_dev);
		marlin_dev = NULL;
		return ret;
	}
#ifdef CONFIG_WCN_SDIO
	if (marlin_dev->int_ap > 0)
		sdio_pub_int_init(marlin_dev->int_ap);
#endif

	flag_reset = 0;

	INIT_WORK(&marlin_dev->download_wq, pre_btwifi_download_sdio);

#ifdef CONFIG_WCN_PMIC
	regmap_np = of_find_compatible_node(NULL, NULL, "sprd,pmic-glb");
	if (!regmap_np) {
		WCN_ERR("get pmic glb node fail\n");
		return -ENODEV;
	}

	pdev_regmap = of_find_device_by_node(regmap_np);
	if (!pdev_regmap) {
		of_node_put(regmap_np);
		WCN_ERR("get pmic device node fail\n");
		return -ENODEV;
	}

	reg_map = dev_get_regmap(pdev_regmap->dev.parent, NULL);
	if (!reg_map) {
		WCN_ERR("get regmap error\n");
		of_node_put(regmap_np);
		return PTR_ERR(reg_map);
	}
#endif

	WCN_INFO("marlin_probe ok!\n");

	return 0;
}

int marlin_add_children(struct device *dev)
{
	return mfd_add_devices(dev, PLATFORM_DEVID_NONE, marlin_cells,
			       ARRAY_SIZE(marlin_cells), NULL, 0, NULL);
}

void marlin_core_remove(struct device *dev)
{
	mfd_remove_devices(dev);

	cancel_work_sync(&marlin_dev->download_wq);
#ifdef CONFIG_WCN_SDIO
	if (marlin_dev->int_ap > 0)
		sdio_pub_int_deinit();
#endif
	sprdwcn_bus_deinit();
	if ((marlin_dev->power_state != 0) && (!marlin_dev->no_power_off)) {
		WCN_INFO("marlin some subsys power is on, warning!\n");
		wifipa_enable(0);
		pmic_bound_xtl_assert(0);
		marlin_chip_en(false, false);
	}
	wcn_bus_deinit();
#ifdef CONFIG_WCN_SLP
	slp_mgr_deinit();
#endif
	marlin_gpio_free(dev);
	mutex_destroy(&marlin_dev->power_lock);
	devm_kfree(dev, marlin_dev->write_buffer);
	dev_set_drvdata(dev, NULL);
	devm_kfree(dev, marlin_dev);
	marlin_dev = NULL;

	WCN_INFO("marlin_remove ok!\n");
}

void marlin_core_shutdown(struct device *dev)
{
	/* When the following three conditions are met at the same time,
	 * wcn chip will be powered off:
	 * 1. chip has been powered on (power_state is not 0);
	 * 2. config power up and down (not keep power on);
	 * 3. bt/wifi wake host is disabled.
	 */
	if ((marlin_dev->power_state != 0) && (!marlin_dev->no_power_off) &&
		(!marlin_get_bt_wl_wake_host_en())) {
		WCN_INFO("marlin some subsys power is on, warning!\n");
		wifipa_enable(0);
		pmic_bound_xtl_assert(0);
		marlin_chip_en(false, false);
	}

	WCN_INFO("marlin_shutdown end\n");
}

int marlin_reset_register_notify(void *callback_func, void *para)
{
	marlin_reset_func = (marlin_reset_callback)callback_func;
	marlin_callback_para = para;

	return 0;
}
EXPORT_SYMBOL_GPL(marlin_reset_register_notify);

int marlin_reset_unregister_notify(void)
{
	marlin_reset_func = NULL;
	marlin_callback_para = NULL;

	return 0;
}
EXPORT_SYMBOL_GPL(marlin_reset_unregister_notify);

MODULE_LICENSE("GPL v2");
MODULE_FIRMWARE(WCN_FW_NAME);
MODULE_DESCRIPTION("Spreadtrum  WCN Marlin Driver");
MODULE_AUTHOR("Yufeng Yang <yufeng.yang@spreadtrum.com>");
