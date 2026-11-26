#include <linux/kernel.h>

#include "wl_core.h"
#include "dbg_ini_util.h"
#include "sprdwl.h"

bool is_valid_channel(struct wiphy *wiphy, u16 chn)
{
	int i;
	struct ieee80211_supported_band *bands;

	if (chn < 15) {
		if (chn < 1)
			return false;
		return true;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
	bands = wiphy->bands[NL80211_BAND_5GHZ];
#else
	bands = wiphy->bands[IEEE80211_BAND_5GHZ];
#endif
	for (i = 0; i < bands->n_channels; i++)
		if (chn == bands->channels[i].hw_value)
			return true;

	return false;
}

int sprdwl_dbg_new_beacon_head(const u8 *beacon_head, int head_len, u8 *new_head,  u16 chn)
{
	int len;
	u8 *ies = NULL, *new_ies = NULL;
	struct ieee80211_mgmt *mgmt;
	u8 supp_5g_rates[8] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
	u8 supp_24g_rates[8] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};

	if (beacon_head == NULL || new_head == NULL)
		return 0;

	memcpy(new_head, beacon_head, head_len);

	mgmt = (struct ieee80211_mgmt *)beacon_head;
	ies = mgmt->u.beacon.variable;
	len = beacon_head + head_len - ies;

	mgmt = (struct ieee80211_mgmt *)new_head;
	new_ies = mgmt->u.beacon.variable;

	while (len > 2) {
		switch (ies[0]) {
		case WLAN_EID_SUPP_RATES:
			*new_ies++ = WLAN_EID_SUPP_RATES;
			*new_ies++ = 8;
			if (chn > 14)
				memcpy(new_ies, supp_5g_rates, 8);
			else
				memcpy(new_ies, supp_24g_rates, 8);
			new_ies += 8;
			break;

		case WLAN_EID_DS_PARAMS:
			memcpy(new_ies, ies, ies[1] + 2);
			new_ies[2] = chn;
			new_ies += ies[1] + 2;
			break;
		default:
			memcpy(new_ies, ies, ies[1] + 2);
			new_ies += ies[1] + 2;
			break;
		}

		len -= ies[1] + 2;
		ies += ies[1] + 2;
	}

	return new_ies - new_head;
}

int sprdwl_dbg_new_beacon_tail(const u8 *beacon_tail, int tail_len, u8 *new_tail, u16 chn)
{
	int len;
	const u8 *ies = beacon_tail;
	u8 *tail = new_tail;
	u8 ext_24g_rates[] = {0x30, 0x48, 0x60, 0x6c, 0x0d, 0x1a, 0x27,\
						0x34, 0x4e, 0x68, 0x75, 0x82, 0x1a, 0x34,\
						0x4e, 0x68, 0x9c, 0xd0, 0xea, 0x04};
	u8 ext_5g_rates[] = {0x0d, 0x1a, 0x27, 0x34, 0x4e, 0x68, 0x75,\
						0x82, 0x1a, 0x34, 0x4e, 0x68, 0x9c, 0xd0, 0xea, 0x04};

	if (beacon_tail == NULL || new_tail == NULL)
		return 0;

#define ERP_INFO_BARKER_PREAMBLE_MODE 4
	if (chn <= 14) {
		*tail++ = WLAN_EID_ERP_INFO;
		*tail++ = 1;
		*tail++ = ERP_INFO_BARKER_PREAMBLE_MODE;
	}

	while (tail_len > 2) {
		switch (ies[0]) {
		case WLAN_EID_ERP_INFO:
			break;
		case WLAN_EID_EXT_SUPP_RATES:
			*tail++ = WLAN_EID_EXT_SUPP_RATES;
			if (chn <= 14) {
				len = sizeof(ext_24g_rates);
				*tail++ = len;
				memcpy(tail, ext_24g_rates, len);
				tail += len;
			} else {
				len = sizeof(ext_5g_rates);
				*tail++ = len;
				memcpy(tail, ext_5g_rates, len);
				tail += len;
			}
			break;
		case WLAN_EID_HT_OPERATION:
			memcpy(tail, ies, ies[1] + 2);
			tail[2] = chn;
			tail += ies[1] + 2;
			break;
		default:
			memcpy(tail, ies, ies[1] + 2);
			tail += ies[1] + 2;
			break;
		}

		ies += ies[1] + 2;
		tail_len -= ies[1] + 2;
	}

	return tail - new_tail;
}

#if 0
void sprdwl_dbg_reset_head_ds_params(u8 *beacon_head, int head_len, u16 chn)
{
	u8 *ies, *ie;
	int len;
	struct ieee80211_mgmt *mgmt;
	u8 supp_5g_rates[8] = {0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c};
	u8 supp_24g_rates[8] = {0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24};

	if (beacon_head == NULL)
		return 0;

	mgmt = (struct ieee80211_mgmt *)beacon_head;
	ies = mgmt->u.beacon.variable;

	len = beacon_head + head_len - ies;
	ie = (u8 *)cfg80211_find_ie(WLAN_EID_DS_PARAMS, ies, len);
	if (ie != NULL) {
		ie[2] = chn;
	}

	ie = (u8 *)cfg80211_find_ie(WLAN_EID_SUPP_RATES, ies, len);
	if (ie != NULL && ie[1] == 8) {
		if (chn <= 14)
			memcpy(ie + 2, supp_24g_rates, 8);
		else
			memcpy(ie + 2, supp_5g_rates, 8);

	}
}

void sprdwl_dbg_reset_tail_ht_oper(u8 *tail, int tail_len, u16 chn)
{
	u8 *ie;

	ie = (u8 *)cfg80211_find_ie(WLAN_EID_HT_OPERATION, tail, tail_len);
	if (ie != NULL) {
		ie[2] = chn;
	}
}
#endif
