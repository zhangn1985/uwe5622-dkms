/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include <wcn_bus.h>

#include "wcn_glb.h"

int mdbg_log_read(int channel, struct mbuf_t *head,
		  struct mbuf_t *tail, int num);
#ifdef CONFIG_WCN_PCIE
int mdbg_log_push(int chn, struct mbuf_t **head,
		  struct mbuf_t **tail, int *num);
#endif

#ifdef CONFIG_WCN_PCIE
static struct mchn_ops_t mdbg_ringc_ops = {
	.channel = WCN_RING_RX,
	.inout = WCNBUS_RX,
	.hif_type = 1,
	.buf_size = 1056,
	.pool_size = 6,
	.cb_in_irq = 0,
	.pop_link = mdbg_log_read,
	.push_link = mdbg_log_push,
};
#else
static struct mchn_ops_t mdbg_ringc_ops = {
	.channel = WCN_RING_RX,
	.inout = WCNBUS_RX,
	.pool_size = 1,
	.pop_link = mdbg_log_read,
};
#endif

#ifdef CONFIG_WCN_PCIE
int mdbg_log_push(int chn, struct mbuf_t **head, struct mbuf_t **tail, int *num)
{
	WCN_INFO("%s enter num=%d,mbuf used done", __func__, *num);

	return 0;
}
#endif

int mdbg_log_read(int channel, struct mbuf_t *head,
		  struct mbuf_t *tail, int num)
{
	sprdwcn_bus_push_list(channel, head, tail, num);

	return 0;
}

static void mdbg_pt_ring_reg(void)
{
	sprdwcn_bus_chn_init(&mdbg_ringc_ops);
}

static void mdbg_pt_ring_unreg(void)
{
	sprdwcn_bus_chn_deinit(&mdbg_ringc_ops);
}

int mdbg_log_channel_init(void)
{
	mdbg_pt_ring_reg();
	WCN_DEBUG("mdbg log channel init success!");

	return 0;
}

void mdbg_log_channel_remove(void)
{
	MDBG_FUNC_ENTERY;
	mdbg_pt_ring_unreg();
}
