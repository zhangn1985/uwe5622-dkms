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
#include "wcn_misc.h"

#define MDBG_WRITE_SIZE			(64)
#define WCN_LOG_MAJOR 255
static int wcn_log_major = WCN_LOG_MAJOR;
static struct class		*wcnlog_class;

struct mdbg_device_t *mdbg_dev;

void wcnlog_clear_log(void)
{
	if (mdbg_dev->ring_dev->ring->rp
		!= mdbg_dev->ring_dev->ring->wp) {
		WCN_INFO("log:%ld left in ringbuf not read\n",
			(long)(mdbg_dev->ring_dev->ring->wp -
			mdbg_dev->ring_dev->ring->rp));
		mdbg_ring_clear(mdbg_dev->ring_dev->ring);
	}
}

int log_dev_init(void)
{
	int err;

	MDBG_FUNC_ENTERY;
	mdbg_dev = kzalloc_obj(struct mdbg_device_t, GFP_KERNEL);
	if (!mdbg_dev)
		return -ENOMEM;

	err = mdbg_ring_init();
	if (err < 0)
		return -ENOMEM;

	return 0;
}

int log_dev_exit(void)
{
	/* free for old mdbg_dev */
	mdbg_ring_remove();
	kfree(mdbg_dev);

	return 0;
}
