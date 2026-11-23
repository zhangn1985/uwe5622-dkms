#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/wait.h>

#include "rdc_debug.h"
#include "wcn_glb.h"

#define MDBG_WRITE_SIZE			(64)
#define WCN_LOG_MAJOR 255
static int wcn_log_major = WCN_LOG_MAJOR;
static struct class		*wcnlog_class;

int log_dev_init(void)
{
	MDBG_FUNC_ENTERY;
	return mdbg_log_channel_init();
}

int log_dev_exit(void)
{
	mdbg_log_channel_remove();

	return 0;
}
