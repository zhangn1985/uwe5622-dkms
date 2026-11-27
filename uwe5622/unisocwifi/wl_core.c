/*
 * Copyright (C) 2016 Spreadtrum Communications Inc.
 *
 * Authors	:
 * star.liu <star.liu@spreadtrum.com>
 * yifei.li <yifei.li@spreadtrum.com>
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

#include <linux/platform_device.h>
#include <linux/mfd/core.h>
#include <linux/utsname.h>
#include <linux/version.h>
#include <marlin_platform.h>
#include <linux/of.h>
#include "sprdwl.h"
#include "wl_intf.h"
#include "wl_core.h"
#include "tx_msg.h"
#include "rx_msg.h"
#include "msg.h"
#include "txrx.h"

#ifdef WL_CONFIG_DEBUG
int sprdwl_debug_level = L_ERR;
#else
int sprdwl_debug_level = L_NONE;
#endif

struct device *sprdwl_dev;

/* TODO: Could we use netdev_alloc_frag instead of kmalloc?
 *       So we did not need to distinguish buffer type
 *       Maybe it could speed up alloc process, too
 */
void sprdwl_free_data(void *data, int buffer_type)
{
	if (buffer_type) { /* Fragment page buffer */
		put_page(virt_to_head_page(data));
	} else { /* Normal buffer */
		kfree(data);
	}
}

void sprdwl_tdls_flow_flush(struct sprdwl_vif *vif, const u8 *peer, u8 oper)
{
	struct sprdwl_intf *intf = vif->priv->hw_priv;
	u8 i;

	if (oper == NL80211_TDLS_SETUP || oper == NL80211_TDLS_ENABLE_LINK) {
		for (i = 0; i < MAX_TDLS_PEER; i++) {
			if (ether_addr_equal(intf->tdls_flow_count[i].da,
					     peer)) {
				memset(&intf->tdls_flow_count[i],
				       0,
				       sizeof(struct tdls_flow_count_para));
				break;
			}
		}
	}
}

void sprdwl_event_tdls_flow_count(struct sprdwl_vif *vif, u8 *data, u16 len)
{
	struct sprdwl_intf *intf = vif->priv->hw_priv;
	u8 i;
	u8 found = 0;
	struct tdls_update_peer_infor *peer_info =
		(struct tdls_update_peer_infor *)data;
	ktime_t kt;

	if (len < sizeof(struct tdls_update_peer_infor)) {
		wl_err("%s, event data len not in range\n", __func__);
		return;
	}
	for (i = 0; i < MAX_TDLS_PEER; i++) {
		if (ether_addr_equal(intf->tdls_flow_count[i].da,
				     peer_info->da)) {
			found = 1;
			break;
		}
	}
	/*0 to delete entry*/
	if (peer_info->valid == 0) {
		if (found == 0) {
			wl_err("%s, invalid da, fail to del\n", __func__);
			return;
		}
		memset(&intf->tdls_flow_count[i],
		       0,
		       sizeof(struct tdls_flow_count_para));

		for (i = 0; i < MAX_TDLS_PEER; i++) {
			if (intf->tdls_flow_count[i].valid == 1)
				found++;
		}
		if (found == 1)
			intf->tdls_flow_count_enable = 0;
	} else if (peer_info->valid == 1) {
		if (found == 0) {
			for (i = 0; i < MAX_TDLS_PEER; i++) {
				if (intf->tdls_flow_count[i].valid == 0) {
					found = 1;
					break;
				}
			}
		}
		if (found == 0) {
			wl_err("%s, no free TDLS entry\n", __func__);
			i = 0;
		}

		intf->tdls_flow_count_enable = 1;
		intf->tdls_flow_count[i].valid = 1;
		ether_addr_copy(intf->tdls_flow_count[i].da, peer_info->da);
		intf->tdls_flow_count[i].threshold = peer_info->txrx_len;
		intf->tdls_flow_count[i].data_len_counted = 0;

		wl_info("%s,%d, tdls_id=%d,threshold=%d, timer=%d, da=(%pM)\n",
			__func__, __LINE__, i,
			intf->tdls_flow_count[i].threshold,
			peer_info->timer, peer_info->da);

		kt = ktime_get();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		intf->tdls_flow_count[i].start_mstime =
			(u32)(div_u64(kt, NSEC_PER_MSEC));
#else
		intf->tdls_flow_count[i].start_mstime =
			(u32)(div_u64(kt.tv64, NSEC_PER_MSEC));
#endif
		intf->tdls_flow_count[i].timer =
			peer_info->timer;
		wl_info("%s,%d, tdls_id=%d,start_time:%u\n",
			__func__, __LINE__, i,
			intf->tdls_flow_count[i].start_mstime);
	}
}

void count_tdls_flow(struct sprdwl_vif *vif, u8 *data, u16 len)
{
	u8 i, found = 0;
	u32 msec;
	u8 elapsed_time;
	u8 unit_time;
	ktime_t kt;
	struct sprdwl_intf *intf = (struct sprdwl_intf *)vif->priv->hw_priv;
	int ret = 0;

	for (i = 0; i < MAX_TDLS_PEER; i++) {
		if ((intf->tdls_flow_count[i].valid == 1) &&
		    (ether_addr_equal(data, intf->tdls_flow_count[i].da)))
			goto count_it;
	}
	return;

count_it:
	kt = ktime_get();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
	msec = (u32)(div_u64(kt, NSEC_PER_MSEC));
#else
	msec = (u32)(div_u64(kt.tv64, NSEC_PER_MSEC));
#endif
	elapsed_time =
		(msec - intf->tdls_flow_count[i].start_mstime) / MSEC_PER_SEC;
	unit_time = elapsed_time / intf->tdls_flow_count[i].timer;
	wl_info("%s,%d, tdls_id=%d, len_counted=%d, len=%d, threshold=%dK\n",
		__func__, __LINE__, i,
		intf->tdls_flow_count[i].data_len_counted, len,
		intf->tdls_flow_count[i].threshold);
	wl_info("currenttime=%u, elapsetime=%d, unit_time=%d\n",
		msec, elapsed_time, unit_time);

	if ((intf->tdls_flow_count[i].data_len_counted == 0 &&
	     len > (intf->tdls_flow_count[i].threshold * 1024)) ||
	    (intf->tdls_flow_count[i].data_len_counted > 0 &&
	    ((intf->tdls_flow_count[i].data_len_counted + len) >
	     intf->tdls_flow_count[i].threshold * 1024 *
	     ((unit_time == 0) ? 1 : unit_time)))) {
		ret = sprdwl_send_tdls_cmd(vif, vif->ctx_id,
					   (u8 *)intf->tdls_flow_count[i].da,
					   SPRDWL_TDLS_CMD_CONNECT);
		memset(&intf->tdls_flow_count[i], 0,
			       sizeof(struct tdls_flow_count_para));
	} else {
		if (intf->tdls_flow_count[i].data_len_counted == 0) {
			intf->tdls_flow_count[i].start_mstime = msec;
			intf->tdls_flow_count[i].data_len_counted += len;
		}
		if ((intf->tdls_flow_count[i].data_len_counted > 0) &&
		    unit_time > 1) {
			intf->tdls_flow_count[i].start_mstime = msec;
			intf->tdls_flow_count[i].data_len_counted = len;
		}
		if ((intf->tdls_flow_count[i].data_len_counted > 0) &&
		    unit_time <= 1) {
			intf->tdls_flow_count[i].data_len_counted += len;
		}
	}
	for (i = 0; i < MAX_TDLS_PEER; i++) {
		if (intf->tdls_flow_count[i].valid == 1)
			found++;
	}
	if (found == 0)
		intf->tdls_flow_count_enable = 0;
}

static int sprdwl_ini_download_status(void)
{
	/*disable download ini function, just return 1*/
	/*	return 1; */
	/*fw is ready for receive ini file*/
	/* return !is_first_power_on(MARLIN_WIFI); */
	return !cali_ini_need_download(MARLIN_WIFI);
}

static void sprdwl_force_exit(void *spdev)
{
	struct sprdwl_intf *intf;

	intf = (struct sprdwl_intf *)spdev;
	intf->exit = 1;
}

static int sprdwl_is_exit(void *spdev)
{
	struct sprdwl_intf *intf;

	intf = (struct sprdwl_intf *)spdev;
	return intf->exit;
}

static void sprdwl_tcp_drop_msg(void *spdev, struct sprdwl_msg_buf *msgbuf)
{
	enum sprdwl_mode mode;
	struct sprdwl_msg_list *list;
	struct sprdwl_intf *intf = (struct sprdwl_intf *)spdev;

	if (msgbuf->skb)
		dev_kfree_skb(msgbuf->skb);
	mode = msgbuf->mode;
	list = msgbuf->msglist;
	sprdwl_free_msg_buf(msgbuf, list);
	sprdwl_wake_net_ifneed(intf, list, mode);
}

static struct sprdwl_if_ops sprdwl_core_ops = {
	.get_msg_buf = sprdwl_get_msg_buf,
	.free_msg_buf = sprdwl_tx_free_msg_buf,
	.tx = sprdwl_tx_msg_func,
	.force_exit = sprdwl_force_exit,
	.is_exit = sprdwl_is_exit,
	.tcp_drop_msg = sprdwl_tcp_drop_msg,
	.ini_download_status = sprdwl_ini_download_status
};

static int sprdwl_probe(struct platform_device *pdev)
{
	struct sprdwl_intf *intf;
	struct sprdwl_priv *priv;
	int ret;
	u8 i;

	if (!mfd_get_cell(pdev))
		return -ENODEV;

	ret = start_marlin(MARLIN_WIFI);
	if (ret) {
		wl_err("%s power on chipset failed\n", __func__);
		return ret;
	}

	intf = kzalloc(sizeof(*intf), GFP_ATOMIC);
	if (!intf) {
		ret = -ENOMEM;
		wl_err("%s alloc intf fail: %d\n", __func__, ret);
		goto err;
	}

	platform_set_drvdata(pdev, intf);
	intf->pdev = pdev;
	sprdwl_dev = &pdev->dev;

	for (i = 0; i < MAX_LUT_NUM; i++)
		intf->peer_entry[i].ctx_id = 0xff;

#ifdef STA_SOFTAP_SCC_MODE
	intf->sta_home_channel = 0;
#endif
	priv = sprdwl_core_create(get_hwintf_type(),
				  &sprdwl_core_ops);
	if (!priv) {
		wl_err("%s core create fail\n", __func__);
		ret = -ENXIO;
		goto err_core_create;
	}
	strscpy(priv->wl_ver.kernel_ver, utsname()->release,
			sizeof(priv->wl_ver.kernel_ver));
	strscpy(priv->wl_ver.drv_ver, SPRDWL_DRIVER_VERSION,
			sizeof(priv->wl_ver.drv_ver));
	strscpy(priv->wl_ver.update, SPRDWL_UPDATE,
			sizeof(priv->wl_ver.update));
	strscpy(priv->wl_ver.reserve, SPRDWL_RESERVE,
			sizeof(priv->wl_ver.reserve));
	wl_info("Spreadtrum WLAN Version:");
	wl_info("Kernel:%s,Driver:%s,update:%s,reserved:%s\n",
			 utsname()->release, SPRDWL_DRIVER_VERSION,
			 SPRDWL_UPDATE, SPRDWL_RESERVE);

	intf->hif_offset = sizeof(struct sdiohal_puh);
	intf->rx_cmd_port = SDIO_RX_CMD_PORT;
	intf->rx_data_port = SDIO_RX_DATA_PORT;
	intf->tx_cmd_port = SDIO_TX_CMD_PORT;
	intf->tx_data_port = SDIO_TX_DATA_PORT;
	ret = sprdwl_intf_init(priv, intf);
	if (ret) {
		wl_err("%s intf init failed: %d\n", __func__, ret);
		goto err_if_init;
	}

	ret = sprdwl_rx_init(intf);
	if (ret) {
		wl_err("%s rx init failed: %d\n", __func__, ret);
		goto err_rx_init;
	}

	ret = sprdwl_tx_init(intf);
	if (ret) {
		wl_err("%s tx_list init failed\n", __func__);
		goto err_tx_init;
	}

	ret = sprdwl_core_init(&pdev->dev, priv);
	if (ret)
		goto err_core_init;


	return ret;

err_core_init:
	sprdwl_tx_deinit(intf);
err_tx_init:
	sprdwl_rx_deinit(intf);
err_rx_init:
	sprdwl_intf_deinit(intf);
err_if_init:
	sprdwl_core_free((struct sprdwl_priv *)intf->priv);
err_core_create:
	kfree(intf);
err:
	stop_marlin(MARLIN_WIFI);
	return ret;
}

static void sprdwl_remove(struct platform_device *pdev)
{
	struct sprdwl_intf *intf = platform_get_drvdata(pdev);
	struct sprdwl_priv *priv = intf->priv;

	sprdwl_core_deinit(priv);
	sprdwl_tx_deinit(intf);
	sprdwl_rx_deinit(intf);
	sprdwl_intf_deinit(intf);
	sprdwl_core_free(priv);
	kfree(intf);
	stop_marlin(MARLIN_WIFI);

}

static const struct of_device_id sprdwl_of_match[] = {
	{.compatible = "sprd,unisoc-wifi",},
	{}
};
MODULE_DEVICE_TABLE(of, sprdwl_of_match);

static struct platform_driver sprdwl_driver = {
	.probe = sprdwl_probe,
	.remove = sprdwl_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "unisoc_wifi",
		.of_match_table = sprdwl_of_match,
	}
};

module_platform_driver(sprdwl_driver);

MODULE_DESCRIPTION("Spreadtrum Wireless LAN Driver");
MODULE_AUTHOR("Spreadtrum WCN Division");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:unisoc_wifi");
