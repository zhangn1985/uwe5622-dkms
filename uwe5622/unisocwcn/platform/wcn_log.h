#ifndef _WCN_LOG
#define _WCN_LOG

#include "mdbg_type.h"

#define WCN_LOG_MAX_MINOR 2

struct mdbg_device_t {
	struct wcnlog_dev *dev[WCN_LOG_MAX_MINOR];
	struct ring_device *ring_dev;
};

extern struct mdbg_device_t	*mdbg_dev;
extern unsigned char flag_reset;
extern struct completion ge2_completion;

void marlin_hold_cpu(void);
void wcnlog_clear_log(void);

int log_dev_init(void);
int log_dev_exit(void);
#endif
