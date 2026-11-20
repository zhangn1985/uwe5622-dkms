/*
 * Copyright (C) 2018 Spreadtrum Communications Inc.
 *
 * File:		wcn_misc.c
 * Description:	WCN misc file for drivers. Some feature or function
 * isn't easy to classify, then write it in this file.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the	1
 * GNU General Public License for more details.
 */

#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/time.h>
#include <wcn_wrapper.h>
#if KERNEL_VERSION(4, 11, 0) <= LINUX_VERSION_CODE
#include <linux/sched/clock.h>
#endif

#include "wcn_misc.h"
#include "wcn_txrx.h"
#include "mdbg_type.h"

u32 wcn_print_level = WCN_DEBUG_OFF;

static struct atcmd_fifo s_atcmd_owner;
static unsigned long s_marlin_bootup_time;

void mdbg_atcmd_owner_init(void)
{
	memset(&s_atcmd_owner, 0, sizeof(s_atcmd_owner));
	mutex_init(&s_atcmd_owner.lock);
}

void mdbg_atcmd_owner_deinit(void)
{
	mutex_destroy(&s_atcmd_owner.lock);
}

static void mdbg_atcmd_owner_add(enum atcmd_owner owner)
{
	mutex_lock(&s_atcmd_owner.lock);
	s_atcmd_owner.owner[s_atcmd_owner.tail % ATCMD_FIFO_MAX] = owner;
	s_atcmd_owner.tail++;
	mutex_unlock(&s_atcmd_owner.lock);
}

enum atcmd_owner mdbg_atcmd_owner_peek(void)
{
	enum atcmd_owner owner;

	mutex_lock(&s_atcmd_owner.lock);
	owner = s_atcmd_owner.owner[s_atcmd_owner.head % ATCMD_FIFO_MAX];
	s_atcmd_owner.head++;
	mutex_unlock(&s_atcmd_owner.lock);

	WCN_DEBUG("owner=%d, head=%d\n", owner, s_atcmd_owner.head-1);
	return owner;
}

void mdbg_atcmd_clean(void)
{
	mutex_lock(&s_atcmd_owner.lock);
	memset(&s_atcmd_owner.owner[0], 0, ARRAY_SIZE(s_atcmd_owner.owner));
	s_atcmd_owner.head = s_atcmd_owner.tail = 0;
	mutex_unlock(&s_atcmd_owner.lock);
}

/* copy from function: kdb_gmtime */
static void wcn_gmtime(struct timespec64 *tv, struct wcn_tm *tm)
{
	/* This will work from 1970-2099, 2100 is not a leap year */
	static int mon_day[] = { 31, 29, 31, 30, 31, 30, 31,
				 31, 30, 31, 30, 31 };
	memset(tm, 0, sizeof(*tm));
	tm->tm_msec =  tv->tv_nsec/1000000;
	tm->tm_sec  = tv->tv_sec % (24 * 60 * 60);
	tm->tm_mday = tv->tv_sec / (24 * 60 * 60) +
		(2 * 365 + 1); /* shift base from 1970 to 1968 */
	tm->tm_min =  tm->tm_sec / 60 % 60;
	tm->tm_hour = tm->tm_sec / 60 / 60;
	tm->tm_sec =  tm->tm_sec % 60;
	tm->tm_year = 68 + 4*(tm->tm_mday / (4*365+1));
	tm->tm_mday %= (4*365+1);
	mon_day[1] = 29;
	while (tm->tm_mday >= mon_day[tm->tm_mon]) {
		tm->tm_mday -= mon_day[tm->tm_mon];
		if (++tm->tm_mon == 12) {
			tm->tm_mon = 0;
			++tm->tm_year;
			mon_day[1] = 28;
		}
	}
	++tm->tm_mday;
}

/*
 * Only marlin poweron and marlin starts to run,
 * it can call this function.
 * The time will be sent to marlin with loopcheck CMD.
 * NOTES:If marlin power off, and power on again, it
 * should call this function again.
 */
void marlin_bootup_time_update(void)
{
	s_marlin_bootup_time = local_clock();
	WCN_INFO("s_marlin_bootup_time=%ld",
		s_marlin_bootup_time);
}

unsigned long marlin_bootup_time_get(void)
{
	return s_marlin_bootup_time;
}

