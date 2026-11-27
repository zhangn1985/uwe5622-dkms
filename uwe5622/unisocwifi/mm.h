#ifndef __SPRDWL_MM_H__
#define __SPRDWL_MM_H__

struct sprdwl_mm {
	int hif_offset;
};

int sprdwl_mm_init(struct sprdwl_mm *mm_entry, void *intf);
int sprdwl_mm_deinit(struct sprdwl_mm *mm_entry, void *intf);
void mm_mh_data_process(struct sprdwl_mm *mm_entry, void *data,
			int len, int buffer_type);
#endif /* __SPRDWL_MM_H__ */
