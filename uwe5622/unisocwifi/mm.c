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

#include <wcn_bus.h>
#include "rx_msg.h"
#include "tx_msg.h"
#include "cmdevt.h"
#include "work.h"

static inline struct sk_buff *mm_build_skb(void *data, int len, int buffer_type)
{
	return build_skb(data, (buffer_type ? len : 0));
}

static struct sk_buff
*mm_data2skb_process(struct sprdwl_mm *mm_entry, void *data, int len)
{
	struct sk_buff *skb = NULL;

	skb = dev_alloc_skb(len);
	if (likely(skb))
		memcpy(skb->data, data, len);

	return skb;
}

inline bool is_compound_data(struct sprdwl_mm *mm_entry, void *data)
{
	struct rx_msdu_desc *msdu_desc =
		(struct rx_msdu_desc *)(data + mm_entry->hif_offset);

	wl_debug("%s: short_pkt_num: %d\n", __func__, msdu_desc->short_pkt_num);

	return (msdu_desc->short_pkt_num > 1);
}

static void
mm_compound_data_process(struct sprdwl_mm *mm_entry, void *compound_data,
			 int total_len, int buffer_type)
{
#define ALIGN_8BYTE(a) (((a) + 7) & ~7)
	void *pos_data = NULL;
	int num = 0, msdu_len = 0, len = 0;
	struct sk_buff *skb = NULL;
	struct sprdwl_rx_if *rx_if =
			container_of(mm_entry, struct sprdwl_rx_if, mm_entry);

	wl_debug("%s: num: %d, total_len: %d\n", __func__, num, total_len);

	pos_data = compound_data + mm_entry->hif_offset;
	total_len -= mm_entry->hif_offset;
	num = ((struct rx_msdu_desc *)pos_data)->short_pkt_num;

	while (num--) {
		msdu_len = msdu_total_len((struct rx_msdu_desc *)pos_data);
		len += ALIGN_8BYTE(msdu_len);
		if (unlikely(len > total_len)) {
			wl_err("%s: total_len:%d < len:%d, leave %d pkts\n",
				   __func__, total_len, len, (num + 1));
			break;
		}

		wl_debug("%s: msdu_len: %d, len: %d\n",
			 __func__, msdu_len, len);

		skb = mm_data2skb_process(mm_entry, pos_data, msdu_len);
		if (unlikely(!skb)) {
			wl_err("%s: alloc skb fail, leave %d pkts\n",
				   __func__, (num + 1));
			break;
		}

		sprdwl_rx_process(rx_if, skb);

		pos_data = (unsigned char *)pos_data +
			ALIGN_8BYTE(msdu_len + sizeof(struct rx_mh_desc));
		skb = NULL;
	}

	sprdwl_free_data(compound_data, buffer_type);
}

static void mm_normal_data_process(struct sprdwl_mm *mm_entry,
				   void *data, int len, int buffer_type)
{
	int skb_len = 0;
	unsigned short csum = 0;
	bool free_data = false;
	struct sk_buff *skb = NULL;
	struct rx_msdu_desc *msdu_desc =
		(struct rx_msdu_desc *)(data + mm_entry->hif_offset);
	struct sprdwl_rx_if *rx_if =
			container_of(mm_entry, struct sprdwl_rx_if, mm_entry);

	if (unlikely(len < sizeof(struct rx_msdu_desc))) {
		wl_err("%s: data len is %d, too short\n",
			   __func__, len);
		free_data = true;
	} else {
		csum = get_sdio_data_csum((void *)rx_if->intf, data);
		skb_len = SKB_DATA_ALIGN(sizeof(struct skb_shared_info)) +
			  SKB_DATA_ALIGN(msdu_total_len(msdu_desc) +
					 mm_entry->hif_offset);

		/*0: sdma, 1: adma*/
		if (sprdwcn_bus_get_rx_mode() == 1) {
			if (likely(skb_len <= len)) {
				/* Use len instead of skb_len
				 * because we could reserve more tailroom
				 */

				skb = mm_build_skb(data, skb_len, buffer_type);
			} else {
				/* Should not happen */
				wl_err("%s: data len is %d, skb need %d\n",
						__func__, len, skb_len);
				skb = mm_data2skb_process(mm_entry, data,
						SKB_WITH_OVERHEAD(skb_len));
				free_data = true;
			}
		} else {
			skb = mm_data2skb_process(mm_entry, data, SKB_WITH_OVERHEAD(skb_len));
			free_data = true;
		}

		if (unlikely(!skb)) {
			wl_err("%s: alloc skb fail\n", __func__);
			free_data = true;
		} else {
			skb_reserve(skb, mm_entry->hif_offset);

			if (likely(fill_skb_csum(skb, csum) >= 0))
				sprdwl_rx_process(rx_if, skb);
			else /* checksum error, free skb */
				dev_kfree_skb(skb);
		}
	}

	if (free_data)
		sprdwl_free_data(data, buffer_type);
}


/* NORMAL DATA */
void mm_mh_data_process(struct sprdwl_mm *mm_entry, void *data,
			int len, int buffer_type)
{
	if (is_compound_data(mm_entry, data))
		mm_compound_data_process(mm_entry, data, len, buffer_type);
	else
		mm_normal_data_process(mm_entry, data, len, buffer_type);
}

int sprdwl_mm_init(struct sprdwl_mm *mm_entry, void *intf)
{
	int ret = 0;

	mm_entry->hif_offset = ((struct sprdwl_intf *)intf)->hif_offset;


	return ret;
}

int sprdwl_mm_deinit(struct sprdwl_mm *mm_entry, void *intf)
{

	mm_entry->hif_offset = 0;
	return 0;
}
