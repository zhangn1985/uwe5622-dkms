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

#ifndef __WCN_TXRX_H__
#define __WCN_TXRX_H__
#include <linux/interrupt.h>
#include <linux/types.h>
#include <linux/version.h>

#include "mdbg_type.h"
#include <wcn_bus.h>

#define SMP_HEADERFLAG 0X7E7E7E7E
#define SMP_RESERVEDFLAG 0X5A5A
#define SMP_DSP_CHANNEL_NUM 0X88
#define SMP_DSP_TYPE 0X9D
#define SMP_DSP_DUMP_TYPE 0X32

#define SYSNC_CODE_LEN 0X4
#define CHKSUM_LEN 0X2
#define ARMLOG_HEAD 9

#define SMP_HEAD_STR "at+smphead="

struct sme_head_tag {
	unsigned int seq_num;
	unsigned short len;
	unsigned char type;
	unsigned char subtype;
};

struct smp_head {
	unsigned int sync_code;
	unsigned short length;
	unsigned char channel_num;
	unsigned char packet_type;
	unsigned short reserved;
	unsigned short check_sum;
};

enum smp_diag_subtype_t {
	NORMAL_INFO = 0X0,
	DUMP_MEM_DATA,
	DUMP_MEM_END,
};

#define WCNBUS_TX 1
#define WCNBUS_RX 0
enum wcnbus_channel_t {
	WCN_AT_TX = 0,
	WCN_AT_RX = 13,
	/* TODO: The port num is temp,need to consult with cp */
	WCN_ADSP_RING_RX = 19,
	WCN_RSV_RX,
};

enum mdbg_channel_ops_t {
	MDBG_AT_TX_OPS = 0,
	MDBG_AT_RX_OPS,
	MDBG_ADSP_RING_RX_OPS = 4,
	MDBG_RSV_OPS = 5,
};

enum {
	MDBG_SUBTYPE_AT = 2,
};
#endif
