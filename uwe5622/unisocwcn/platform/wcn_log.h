#ifndef _WCN_LOG
#define _WCN_LOG

#include "mdbg_type.h"

#define WCN_LOG_MAX_MINOR 2

extern unsigned char flag_reset;
extern struct completion ge2_completion;

void marlin_hold_cpu(void);

int log_dev_init(void);
int log_dev_exit(void);
#endif
