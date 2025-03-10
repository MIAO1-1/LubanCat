/*
 * Datapath implementation for XRadio drivers
 *
 * Copyright (c) 2013
 * Xradio Technology Co., Ltd. <www.xradiotech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <net/mac80211.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>

#include "xradio.h"
#include "wsm.h"
#include "bh.h"
#include "ap.h"
#include "sta.h"
#include "sbus.h"

#define B_RATE_INDEX   0     /* 11b rate for important short frames in 2.4G. */
#define AG_RATE_INDEX  6     /* 11a/g rate for important short frames in 5G. */
#define XRADIO_INVALID_RATE_ID (0xFF)

/* rate should fall quickly to avoid dropping frames by aps.*/
#ifdef ENHANCE_ANTI_INTERFERE
#define HIGH_RATE_MAX_RETRY  9
#else
#define HIGH_RATE_MAX_RETRY  7
#endif

#ifdef CONFIG_XRADIO_TESTMODE
#include "nl80211_testmode_msg_copy.h"
#endif /* CONFIG_XRADIO_TESTMODE */
#ifdef TES_P2P_0002_ROC_RESTART
#include <linux/time.h>
#endif
static const struct ieee80211_rate *xradio_get_tx_rate(
							const struct xradio_common *hw_priv,
							const struct ieee80211_tx_rate *rate);

u32 TxedRateIdx_Map[24] = { 0 };
u32 RxedRateIdx_Map[24] = { 0 };

#ifdef CONFIG_XRADIO_DEBUGFS
/*for tx rates debug.*/
extern u8 rates_dbg_en;
extern u32 rates_debug[3];
extern u8 maxRate_dbg;
extern u8 retry_dbg;
extern u8 rate_sgi;
#endif
/* ******************************************************************** */
/* TX policy cache implementation					*/

static void tx_policy_dump(struct tx_policy *policy)
{
	txrx_printk(XRADIO_DBG_MSG, "[TX policy] "
		    "%.1X%.1X%.1X%.1X%.1X%.1X%.1X%.1X"
		    "%.1X%.1X%.1X%.1X%.1X%.1X%.1X%.1X"
		    "%.1X%.1X%.1X%.1X%.1X%.1X%.1X%.1X: %d\n",
		    policy->raw[0] & 0x0F, policy->raw[0] >> 4,
		    policy->raw[1] & 0x0F, policy->raw[1] >> 4,
		    policy->raw[2] & 0x0F, policy->raw[2] >> 4,
		    policy->raw[3] & 0x0F, policy->raw[3] >> 4,
		    policy->raw[4] & 0x0F, policy->raw[4] >> 4,
		    policy->raw[5] & 0x0F, policy->raw[5] >> 4,
		    policy->raw[6] & 0x0F, policy->raw[6] >> 4,
		    policy->raw[7] & 0x0F, policy->raw[7] >> 4,
		    policy->raw[8] & 0x0F, policy->raw[8] >> 4,
		    policy->raw[9] & 0x0F, policy->raw[9] >> 4,
		    policy->raw[10] & 0x0F, policy->raw[10] >> 4,
		    policy->raw[11] & 0x0F, policy->raw[11] >> 4,
		    policy->defined);
}

static void xradio_check_go_neg_conf_success(struct xradio_common *hw_priv,
					     u8 *action)
{
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);
	if (action[2] == 0x50 && action[3] == 0x6F && action[4] == 0x9A &&
	    action[5] == 0x09 && action[6] == 0x02) {
		if (action[17] == 0) {
			hw_priv->is_go_thru_go_neg = true;
		} else {
			hw_priv->is_go_thru_go_neg = false;
		}
	}
}

#ifdef TES_P2P_0002_ROC_RESTART
/*
 * TES_P2P_0002 WorkAround:
 * P2P GO Neg Process and P2P FIND may be collision.
 * When P2P Device is waiting for GO NEG CFM in 30ms,
 * P2P FIND may end with p2p listen, and then goes to p2p search.
 * Then xradio scan will occupy phy on other channel in 3+ seconds.
 * P2P Device will not be able to receive the GO NEG CFM.
 * We extend the roc period to remaind phy to receive
 * GO NEG CFM as WorkAround.
 */

s32 TES_P2P_0002_roc_dur;
s32 TES_P2P_0002_roc_sec;
s32 TES_P2P_0002_roc_usec;
u32 TES_P2P_0002_packet_id;
u32 TES_P2P_0002_state = TES_P2P_0002_STATE_IDLE;

static void xradio_frame_monitor(struct xradio_common *hw_priv,
				 struct sk_buff *skb, bool tx)
{
	struct ieee80211_hdr *frame = (struct ieee80211_hdr *)skb->data;
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;

	u8 *action = (u8 *) &mgmt->u.action.category;
	u8 *category_code = &(action[0]);
	u8 *action_code = &(action[1]);
	u8 *oui = &(action[2]);
	u8 *subtype = &(action[5]);
	u8 *oui_subtype = &(action[6]);
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (ieee80211_is_action(frame->frame_control) &&
	   *category_code == WLAN_CATEGORY_PUBLIC &&
	   *action_code == 0x09) {
		if ((oui[0] == 0x50) && (oui[1] == 0x6F) &&
		   (oui[2] == 0x9A) && (*subtype == 0x09)) {
			/* w, GO Negotiation Response */
			if (*oui_subtype == 0x01) {
				if ((TES_P2P_0002_state == TES_P2P_0002_STATE_IDLE) &&
				   (tx == true)) { /* w, p2p atturbute:status,id=0 */
					u8 *go_neg_resp_res = &(action[17]);
					if (*go_neg_resp_res == 0x0) {
						TES_P2P_0002_state = TES_P2P_0002_STATE_SEND_RESP;
						txrx_printk(XRADIO_DBG_NIY,
							    "[ROC_RESTART_STATE_SEND_RESP]\n");
					}
				}
			/* w, GO Negotiation Confirmation */
			} else if (*oui_subtype == 0x02) {
				if (tx == false) {
					TES_P2P_0002_state = TES_P2P_0002_STATE_IDLE;
					txrx_printk(XRADIO_DBG_NIY, "[ROC_RESTART_STATE_IDLE]"
						    "[GO Negotiation Confirmation]\n");
				}
			/* w, Provision Discovery Response */
			} else if (*oui_subtype == 0x08) {
				if (tx == false) {
					TES_P2P_0002_state = TES_P2P_0002_STATE_IDLE;
					txrx_printk(XRADIO_DBG_NIY, "[ROC_RESTART_STATE_IDLE]"
						    "[Provision Discovery Response]\n");
				}
			}
		}
	}
}
#endif

static void xradio_check_prov_desc_req(struct xradio_common *hw_priv,
						u8 *action)
{
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);
	if (action[2] == 0x50 && action[3] == 0x6F && action[4] == 0x9A &&
	    action[5] == 0x09 && action[6] == 0x07) {
		hw_priv->is_go_thru_go_neg = false;
	}
}

#ifdef AP_HT_COMPAT_FIX
#define AP_COMPAT_THRESHOLD  2000
#define AP_COMPAT_MIN_CNT    200
u8 ap_compat_bssid[ETH_ALEN] = { 0 };
static int xradio_apcompat_detect(struct xradio_vif *priv, u8 rx_rate)
{
	if (rx_rate < AG_RATE_INDEX) {
		priv->ht_compat_cnt++;
		txrx_printk(XRADIO_DBG_MSG, "%s:rate=%d.\n", __func__, rx_rate);
	} else {
		priv->ht_compat_det |= 1;
		priv->ht_compat_cnt = 0;
		txrx_printk(XRADIO_DBG_NIY, "%s:HT compat detect\n", __func__);
		return 0;
	}

	/* Enhance compatibility with some illegal APs.*/
	if (priv->ht_compat_cnt  > AP_COMPAT_THRESHOLD ||
		(priv->ht_compat_cnt > AP_COMPAT_MIN_CNT &&
		 priv->bssid[0] == 0xC8 &&
		 priv->bssid[1] == 0x3A &&
		 priv->bssid[2] == 0x35)) {
		struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
		memcpy(ap_compat_bssid, priv->bssid, ETH_ALEN);
		wsm_send_disassoc_to_self(hw_priv, priv);
		txrx_printk(XRADIO_DBG_WARN, "%s:SSID=%s, BSSID=" \
			    "%02x:%02x:%02x:%02x:%02x:%02x\n", __func__, priv->ssid,
			    ap_compat_bssid[0], ap_compat_bssid[1],
			    ap_compat_bssid[2], ap_compat_bssid[3],
			    ap_compat_bssid[4], ap_compat_bssid[5]);
		return 1;
	}
	return 0;
}

static void xradio_remove_ht_ie(struct xradio_vif *priv, struct sk_buff *skb)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
	u8 *ies        = NULL;
	size_t ies_len = 0;
	u8 *ht_ie      = NULL;

	if (!mgmt || memcmp(ap_compat_bssid, mgmt->bssid, ETH_ALEN))
		return;

	if (ieee80211_is_probe_resp(mgmt->frame_control))
		ies = mgmt->u.probe_resp.variable;
	else if (ieee80211_is_beacon(mgmt->frame_control))
		ies = mgmt->u.beacon.variable;
	else if (ieee80211_is_assoc_resp(mgmt->frame_control))
		ies = mgmt->u.assoc_resp.variable;
	else if (ieee80211_is_assoc_req(mgmt->frame_control))
		ies = mgmt->u.assoc_req.variable;
	else
		return;

	ies_len = skb->len - (ies - (u8 *)(skb->data));
	ht_ie   = (u8 *)xradio_get_ie(ies, ies_len, WLAN_EID_HT_CAPABILITY);
	if (ht_ie) {
		u8 ht_len   = *(ht_ie + 1) + 2;
		u8 move_len = (ies + ies_len) - (ht_ie + ht_len);
		memmove(ht_ie, (ht_ie + ht_len), move_len);
		skb_trim(skb, skb->len - ht_len);
		ies_len = skb->len - (ies - (u8 *)(skb->data));
		ht_ie = (u8 *)xradio_get_ie(ies, ies_len, WLAN_EID_HT_INFORMATION);
		if (ht_ie) {
			ht_len   = *(ht_ie + 1) + 2;
			move_len = (ies + ies_len) - (ht_ie + ht_len);
			memmove(ht_ie, (ht_ie + ht_len), move_len);
			skb_trim(skb, skb->len - ht_len);
		}
	}
	txrx_printk(XRADIO_DBG_WARN, "%s: BSSID=%02x:%02x:%02x:%02x:%02x:%02x\n",
		    __func__,
		    mgmt->bssid[0], mgmt->bssid[1],
		    mgmt->bssid[2], mgmt->bssid[3],
		    mgmt->bssid[4], mgmt->bssid[5]);
}
#endif /*AP_HT_COMPAT_FIX*/

static void tx_policy_build(struct xradio_vif *priv,
	/* [out] */ struct tx_policy *policy,
	struct ieee80211_tx_rate *rates, size_t count)
{
	int i, j;
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	struct ieee80211_rate *tmp_rate = NULL;
	unsigned limit = hw_priv->short_frame_max_tx_count;
	unsigned max_rates_cnt = count;
	unsigned total = 0;
	u8 lowest_rate_idx = 0;
	SYS_BUG(rates[0].idx < 0);
	memset(policy, 0, sizeof(*policy));
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	txrx_printk(XRADIO_DBG_MSG, "============================");
#if 0
	for (i = 0; i < count; ++i) {
		if (rates[i].idx >= 0) {
			tmp_rate = xradio_get_tx_rate(hw_priv, &rates[i]);
			txrx_printk(XRADIO_DBG_MSG, "[TX policy] Org %d.%dMps=%d",
				    tmp_rate->bitrate/10, tmp_rate->bitrate%10,
				    rates[i].count);
		}
	}
	txrx_printk(XRADIO_DBG_MSG, "----------------------------");
#endif

	/*
	 * minstrel is buggy a little bit, so distille
	 * incoming rates first.
	 */
	/* Sort rates in descending order. */
	total = rates[0].count;
	for (i = 1; i < count; ++i) {
		if (rates[i].idx > rates[i-1].idx) {
			rates[i].idx = rates[i-1].idx > 0 ? (rates[i-1].idx - 1) : -1;
		}
		if (rates[i].idx < 0 || i >= limit) {
			count = i;
			break;
		} else {
			total += rates[i].count;
		}
	}

	/*
	 * Add lowest rate to the end.
	 * TODO: it's better to do this in rate control of mac80211.
	 */
	if (unlikely(!(rates[0].flags & IEEE80211_TX_RC_MCS)) &&
		hw_priv->channel->band == NL80211_BAND_2GHZ) {
		u32 rateset = (priv->oper_rates | priv->base_rates) & ~0xf;
		if (rateset)
			lowest_rate_idx = __ffs(rateset);
		else
			lowest_rate_idx = 0xff;
		txrx_printk(XRADIO_DBG_MSG, "rateset=0x%x, lowest_rate_idx=%d\n",
					rateset, lowest_rate_idx);
	}
	if (count < max_rates_cnt && rates[count-1].idx > lowest_rate_idx) {
		rates[count].idx   = lowest_rate_idx;
		rates[count].count = rates[0].count;
		rates[count].flags = rates[0].flags;
		total += rates[count].count;
		count++;
	}

	/*
	 * Adjust tx count to limit, rates should fall quickly
	 * and lower rates should be more retry, because reorder
	 * buffer of reciever will be timeout and clear probably.
	 */
	if (count < 2) {
		rates[0].count = limit;
		total = limit;
	} else {
		u8 end_retry = 0;  /* the retry should be add to last rate. */
		if (limit > HIGH_RATE_MAX_RETRY) {
			end_retry = limit - HIGH_RATE_MAX_RETRY;
			limit     = HIGH_RATE_MAX_RETRY;
		}
		/* i<100 to avoid dead loop */
		for (i = 0; (limit != total) && (i < 100); ++i) {
			j = i % count;
			if (limit < total) {
				total += (rates[j].count > 1 ? -1 : 0);
				rates[j].count += (rates[j].count > 1 ? -1 : 0);
			} else {
				j = count - 1 - j;
				if (rates[j].count > 0) {
					total++;
					rates[j].count++;
				}
			}
		}
		if (end_retry) {
			rates[count-1].count += end_retry;
			limit += end_retry;
		}
	}

	/* Eliminate duplicates. */
	total = rates[0].count;
	for (i = 0, j = 1; j < count; ++j) {
		if (rates[j].idx < 0 || rates[j].idx > rates[i].idx)
			break;
		if (rates[j].idx == rates[i].idx) {
			rates[i].count += rates[j].count;
		} else {
			++i;
			if (i != j)
				rates[i] = rates[j];
		}
		total += rates[j].count;
	}
	count = i + 1;

	/*
	 * Re-fill policy trying to keep every requested rate and with
	 * respect to the global max tx retransmission count.
	 */
	if (limit < count)
		limit = count;
	if (total > limit) {
		for (i = 0; i < count; ++i) {
			int left = count - i - 1;
			if (rates[i].count > limit - left)
				rates[i].count = limit - left;
			limit -= rates[i].count;
		}
	}

	/*
	 * HACK!!! Device has problems (at least) switching from
	 * 54Mbps CTS to 1Mbps. This switch takes enormous amount
	 * of time (100-200 ms), leading to valuable throughput drop.
	 * As a workaround, additional g-rates are injected to the
	 * policy.
	 */
	if (count == 2 && !(rates[0].flags & IEEE80211_TX_RC_MCS) &&
			rates[0].idx > 4 && rates[0].count > 2 &&
			rates[1].idx < 2) {
		/* ">> 1" is an equivalent of "/ 2", but faster */
		int mid_rate = (rates[0].idx + 4) >> 1;

		/* Decrease number of retries for the initial rate */
		rates[0].count -= 2;

		if (mid_rate != 4) {
			/* Keep fallback rate at 1Mbps. */
			rates[3] = rates[1];

			/* Inject 1 transmission on lowest g-rate */
			rates[2].idx = 4;
			rates[2].count = 1;
			rates[2].flags = rates[1].flags;

			/* Inject 1 transmission on mid-rate */
			rates[1].idx = mid_rate;
			rates[1].count = 1;

			/* Fallback to 1 Mbps is a really bad thing,
			 * so let's try to increase probability of
			 * successful transmission on the lowest g rate
			 * even more */
			if (rates[0].count >= 3) {
				--rates[0].count;
				++rates[2].count;
			}

			/* Adjust amount of rates defined */
			count += 2;
		} else {
			/* Keep fallback rate at 1Mbps. */
			rates[2] = rates[1];

			/* Inject 2 transmissions on lowest g-rate */
			rates[1].idx = 4;
			rates[1].count = 2;

			/* Adjust amount of rates defined */
			count += 1;
		}
	}

	tmp_rate = (struct ieee80211_rate *)xradio_get_tx_rate(hw_priv, &rates[0]);
	if (tmp_rate)
		policy->defined = tmp_rate->hw_value + 1;

#ifdef ENHANCE_ANTI_INTERFERE
	/*
	 * We need to check if 11b rate be supported.
	 * We only add 11bg rate when no rate below 5.5Mbps is set,
	 * and last rate has enough reties. Add we need check fw version too.
	 */
	if (WSM_CAPS_11N_TO_11BG(hw_priv->wsm_caps) && !priv->vif->p2p &&
		((priv->oper_rates | priv->base_rates) & 0xf) == 0xf &&
		(rates[0].flags & IEEE80211_TX_RC_MCS) &&
		rates[count-1].idx == 0 && rates[count-1].count >= 3) {
		u8 mcs0_cnt = rates[count-1].count;
		u8 cnt1 = (mcs0_cnt>>2);
		u8 cnt2 = ((mcs0_cnt - (cnt1<<1))>>1);
		if (count <= 1) {
			mcs0_cnt -= (cnt1 + cnt1 + cnt2);
			rates[count-1].count = 0;
			policy->tbl[0] |= ((cnt1&0xf)<<(3<<2));
			policy->tbl[0] |= ((cnt2&0xf)<<(2<<2));
			policy->tbl[0] |= ((cnt1&0xf)<<(1<<2));
			policy->tbl[0] |= ((mcs0_cnt&0xf)<<(0<<2));
			policy->retry_count = cnt1 + cnt2 + cnt1 + mcs0_cnt;
			txrx_printk(XRADIO_DBG_MSG,
				"[TX policy]to11b=%d(f=%d),%d, 11=%d, 5.5=%d, 2=%d, 1=%d\n",
				rates[count-1].idx, rates[0].flags,
				rates[count-1].count, cnt1, cnt2, cnt1, mcs0_cnt);
		} else {
			mcs0_cnt -= (cnt1 + cnt1 + cnt2);
			rates[count-1].count = cnt1;
			policy->tbl[0] |= ((cnt1&0xf)<<(3<<2));
			policy->tbl[0] |= ((cnt2&0xf)<<(2<<2));
			policy->tbl[0] |= ((mcs0_cnt&0xf)<<(0<<2));
			policy->retry_count = cnt1 + cnt2 + mcs0_cnt;
			txrx_printk(XRADIO_DBG_MSG,
				"[TX policy]to11b=%d(f=%d),%d, 11=%d, 5.5=%d, 2=%d, 1=%d\n",
				rates[count-1].idx, rates[0].flags,
				rates[count-1].count, cnt1, cnt2, 0, mcs0_cnt);
		}
	} else {
		txrx_printk(XRADIO_DBG_MSG,
			"[TX policy]WSM_CAPS to11b=%d, p2p=%d, MSC=%d, rates=0x%08x, count(%lu)=%d\n",
			!!WSM_CAPS_11N_TO_11BG(hw_priv->wsm_caps), priv->vif->p2p,
			!!(rates[0].flags & IEEE80211_TX_RC_MCS),
			(priv->oper_rates | priv->base_rates),
			count-1, rates[count-1].count);
	}
#endif

	for (i = 0; i < count; ++i) {
		register unsigned rateid, off, shift, retries;

		tmp_rate = (struct ieee80211_rate *)xradio_get_tx_rate(hw_priv, &rates[i]);
		if (tmp_rate) {
			rateid = tmp_rate->hw_value;
		} else {
			break;
		}
		off = rateid >> 3;		/* eq. rateid / 8 */
		shift = (rateid & 0x07) << 2;	/* eq. (rateid % 8) * 4 */

		retries = rates[i].count;
		if (unlikely(retries > 0x0F))
			rates[i].count = retries = 0x0F;
		policy->tbl[off] |= __cpu_to_le32(retries << shift);
		policy->retry_count += retries;
		txrx_printk(XRADIO_DBG_MSG, "[TX policy] %d.%dMps=%d",
			    tmp_rate->bitrate/10, tmp_rate->bitrate%10, retries);
	}

	txrx_printk(XRADIO_DBG_MSG, "[TX policy] Dst Policy (%zu): " \
		"%d:%d, %d:%d, %d:%d, %d:%d, %d:%d\n",
		count,
		rates[0].idx, rates[0].count,
		rates[1].idx, rates[1].count,
		rates[2].idx, rates[2].count,
		rates[3].idx, rates[3].count,
		rates[4].idx, rates[4].count);
}

static inline bool tx_policy_is_equal(const struct tx_policy *wanted,
					const struct tx_policy *cached)
{
	size_t count = wanted->defined >> 1;

	if (wanted->defined > cached->defined)
		return false;
	if (count) {
		if (memcmp(wanted->raw, cached->raw, count))
			return false;
	}
	if (wanted->defined & 1) {
		if ((wanted->raw[count] & 0x0F) != (cached->raw[count] & 0x0F))
			return false;
	}
	return true;
}

static int tx_policy_find(struct tx_policy_cache *cache,
				const struct tx_policy *wanted)
{
	/* O(n) complexity. Not so good, but there's only 8 entries in
	 * the cache.
	 * Also lru helps to reduce search time. */
	struct tx_policy_cache_entry *it;
	/* Search for policy in "used" list */
	list_for_each_entry(it, &cache->used, link) {
		if (tx_policy_is_equal(wanted, &it->policy))
			return it - cache->cache;
	}
	/* Then - in "free list" */
	list_for_each_entry(it, &cache->free, link) {
		if (tx_policy_is_equal(wanted, &it->policy))
			return it - cache->cache;
	}
	return -1;
}

static inline void tx_policy_use(struct tx_policy_cache *cache,
				 struct tx_policy_cache_entry *entry)
{
	++entry->policy.usage_count;
	list_move(&entry->link, &cache->used);
}

static inline int tx_policy_release(struct tx_policy_cache *cache,
				    struct tx_policy_cache_entry *entry)
{
	int ret = --entry->policy.usage_count;
	if (!ret)
		list_move(&entry->link, &cache->free);
	return ret;
}

/* ******************************************************************** */
/* External TX policy cache API						*/

void tx_policy_init(struct xradio_common *hw_priv)
{
	struct tx_policy_cache *cache = &hw_priv->tx_policy_cache;
	int i;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	memset(cache, 0, sizeof(*cache));

	spin_lock_init(&cache->lock);
	INIT_LIST_HEAD(&cache->used);
	INIT_LIST_HEAD(&cache->free);

	for (i = 0; i < TX_POLICY_CACHE_SIZE; ++i)
		list_add(&cache->cache[i].link, &cache->free);
}

static int tx_policy_get(struct xradio_vif *priv,
		  struct ieee80211_tx_rate *rates,
		  u8 use_bg_rate, bool *renew)
{
	int idx;
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	struct tx_policy_cache *cache = &hw_priv->tx_policy_cache;
	struct tx_policy wanted;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (use_bg_rate) {
		u8 rate  = (u8)(use_bg_rate & 0x3f);
		u8 shitf = ((rate&0x7)<<2);
		u8 off   = (rate>>3);
		memset(&wanted, 0, sizeof(wanted));
		wanted.defined = rate + 1;
		wanted.retry_count = (hw_priv->short_frame_max_tx_count&0xf);
		wanted.tbl[off] = wanted.retry_count<<shitf;
		txrx_printk(XRADIO_DBG_NIY, "[TX policy] robust rate=%d\n", rate);
	} else
		tx_policy_build(priv, &wanted, rates, IEEE80211_TX_MAX_RATES);

	/* use rate policy instead of minstel policy in debug mode*/
#ifdef CONFIG_XRADIO_DEBUGFS
	if (rates_dbg_en & 0x2) {
		memset(&wanted, 0, sizeof(wanted));
		wanted.defined = maxRate_dbg + 1;
		wanted.retry_count = (hw_priv->short_frame_max_tx_count&0xf);
		memcpy(&wanted.tbl[0], &rates_debug[0], sizeof(wanted.tbl));
	}
#endif

	spin_lock_bh(&cache->lock);
	idx = tx_policy_find(cache, &wanted);
	if (idx >= 0) {
		txrx_printk(XRADIO_DBG_MSG, "[TX policy] Used TX policy: %d\n",
					idx);
		*renew = false;
	} else {
		struct tx_policy_cache_entry *entry;
		if (WARN_ON_ONCE(list_empty(&cache->free))) {
			spin_unlock_bh(&cache->lock);
			txrx_printk(XRADIO_DBG_ERROR, "[TX policy] no policy cache\n");
			return XRADIO_INVALID_RATE_ID;
		}
		/* If policy is not found create a new one
		 * using the oldest entry in "free" list */
		*renew = true;
		entry = list_entry(cache->free.prev,
			struct tx_policy_cache_entry, link);
		entry->policy = wanted;
		idx = entry - cache->cache;
		txrx_printk(XRADIO_DBG_MSG, "[TX policy] New TX policy: %d\n",
					idx);
		tx_policy_dump(&entry->policy);
	}
	tx_policy_use(cache, &cache->cache[idx]);
	if (unlikely(list_empty(&cache->free)) &&
		!cache->queue_locked) {
		/* Lock TX queues. */
		txrx_printk(XRADIO_DBG_WARN, "[TX policy] policy cache used up\n");
		xradio_tx_queues_lock(hw_priv);
		cache->queue_locked = true;
	}
	spin_unlock_bh(&cache->lock);

	/*force to upload retry limit when using debug rate policy */
#ifdef CONFIG_XRADIO_DEBUGFS
	if (retry_dbg & 0x2) {
		retry_dbg &= ~0x2;
		/* retry dgb need to be applied to policy. */
		*renew = true;
		cache->cache[idx].policy.uploaded = 0;
	}
#endif

	return idx;
}

static void tx_policy_put(struct xradio_common *hw_priv, int idx)
{
	int usage;
	/*int locked;*/
	struct tx_policy_cache *cache = &hw_priv->tx_policy_cache;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	spin_lock_bh(&cache->lock);
	/*locked = list_empty(&cache->free);*/
	usage = tx_policy_release(cache, &cache->cache[idx]);
	if (unlikely(cache->queue_locked) && !list_empty(&cache->free)) {
		/* Unlock TX queues. */
		xradio_tx_queues_unlock(hw_priv);
		cache->queue_locked = false;
	}
	spin_unlock_bh(&cache->lock);
}

/*
bool tx_policy_cache_full(struct xradio_common *hw_priv)
{
	bool ret;
	struct tx_policy_cache *cache = &hw_priv->tx_policy_cache;
	spin_lock_bh(&cache->lock);
	ret = list_empty(&cache->free);
	spin_unlock_bh(&cache->lock);
	return ret;
}
*/
extern u32 policy_upload;
extern u32 policy_num;
static int tx_policy_upload(struct xradio_common *hw_priv)
{
	struct tx_policy_cache *cache = &hw_priv->tx_policy_cache;
	int i;
	struct wsm_set_tx_rate_retry_policy arg = {
		.hdr = {
			.numTxRatePolicies = 0,
		}
	};
	int if_id = 0;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	spin_lock_bh(&cache->lock);
	/* Upload only modified entries. */
	for (i = 0; i < TX_POLICY_CACHE_SIZE; ++i) {
		struct tx_policy *src = &cache->cache[i].policy;
		if (src->retry_count && !src->uploaded) {
			struct wsm_set_tx_rate_retry_policy_policy *dst =
				&arg.tbl[arg.hdr.numTxRatePolicies];
			dst->policyIndex = i;
			dst->shortRetryCount = hw_priv->short_frame_max_tx_count-1;
			/* only RTS need use longRetryCount, should be short_frame. */
			dst->longRetryCount = hw_priv->short_frame_max_tx_count-1;

			/* BIT(2) - Terminate retries when Tx rate retry policy
			 *          finishes.
			 * BIT(3) - Count initial frame transmission as part of
			 *          rate retry counting but not as a retry
			 *          attempt */
			dst->policyFlags = BIT(2) | BIT(3);
			memcpy(dst->rateCountIndices, src->tbl,
					sizeof(dst->rateCountIndices));
			src->uploaded = 1;
			++arg.hdr.numTxRatePolicies;
		}
	}
	spin_unlock_bh(&cache->lock);
	atomic_set(&hw_priv->upload_count, 0);

	xradio_debug_tx_cache_miss(hw_priv);
	txrx_printk(XRADIO_DBG_MSG, "[TX policy] Upload %d policies\n",
				arg.hdr.numTxRatePolicies);
#ifdef CONFIG_XRADIO_DEBUGFS
	if (arg.tbl[0].policyIndex == 7)
		txrx_printk(XRADIO_DBG_MSG, "rate:0x%08x, 0x%08x, 0x%08x\n",
								arg.tbl[0].rateCountIndices[2],
								arg.tbl[0].rateCountIndices[1],
								arg.tbl[0].rateCountIndices[0]);
	policy_upload++;
	policy_num += arg.hdr.numTxRatePolicies;
#endif
	/*TODO: COMBO*/
	return wsm_set_tx_rate_retry_policy(hw_priv, &arg, if_id);
}

void tx_policy_upload_work(struct work_struct *work)
{
	struct xradio_common *hw_priv =
		container_of(work, struct xradio_common, tx_policy_upload_work);
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	SYS_WARN(tx_policy_upload(hw_priv));
	wsm_unlock_tx(hw_priv);
}

/* ******************************************************************** */
/* xradio TX implementation						*/

struct xradio_txinfo {
	struct sk_buff *skb;
	unsigned queue;
	struct ieee80211_tx_info *tx_info;
	const struct ieee80211_rate *rate;
	struct ieee80211_hdr *hdr;
	size_t hdrlen;
	const u8 *da;
	struct xradio_sta_priv *sta_priv;
	struct xradio_txpriv txpriv;
};

u32 xradio_rate_mask_to_wsm(struct xradio_common *hw_priv, u32 rates)
{
	u32 ret = 0;
	int i;
	u32 n_bitrates =
		  hw_priv->hw->wiphy->bands[hw_priv->channel->band]->n_bitrates;
	struct ieee80211_rate *bitrates =
		  hw_priv->hw->wiphy->bands[hw_priv->channel->band]->bitrates;

	for (i = 0; i < n_bitrates; ++i) {
		if (rates & BIT(i))
			ret |= BIT(bitrates[i].hw_value);
	}
	return ret;
}

static const struct ieee80211_rate *
xradio_get_tx_rate(const struct xradio_common *hw_priv,
		   const struct ieee80211_tx_rate *rate)
{
	if (rate->idx < 0)
		return NULL;
	if (rate->flags & IEEE80211_TX_RC_MCS)
		return &hw_priv->mcs_rates[rate->idx];
	return &hw_priv->hw->wiphy->bands[hw_priv->channel->band]->
		bitrates[rate->idx];
}

static inline s8 xradio_get_rate_idx(const struct xradio_common *hw_priv,
									 u8 flag, u16 hw_value)
{
	s16 ret = (s16)hw_value;
	if (flag & IEEE80211_TX_RC_MCS) {  /* 11n */
		if (hw_value <= hw_priv->mcs_rates[7].hw_value &&
			 hw_value >= hw_priv->mcs_rates[0].hw_value)
			ret -= hw_priv->mcs_rates[0].hw_value;
		else
			ret = -1;
	} else {  /* 11b/g */
		if (hw_value > 5 && hw_value < hw_priv->mcs_rates[0].hw_value) {
			ret -= hw_priv->hw->wiphy-> \
			       bands[hw_priv->channel->band]->bitrates[0].hw_value;
			if (hw_priv->hw->wiphy-> \
			   bands[hw_priv->channel->band]->bitrates[0].hw_value < 5) /* 11a*/
				ret -= 2;
		} else if (hw_value < 4) {
			ret -= hw_priv->hw->wiphy-> \
			       bands[hw_priv->channel->band]->bitrates[0].hw_value;
		} else {
			ret = -1;
		}
	}
	return (s8)ret;
}

static int
xradio_tx_h_calc_link_ids(struct xradio_vif *priv,
			  struct xradio_txinfo *t)
{
#ifndef P2P_MULTIVIF
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
#endif
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

#ifndef P2P_MULTIVIF
	if ((t->tx_info->flags & IEEE80211_TX_CTL_TX_OFFCHAN) ||
			(hw_priv->roc_if_id == priv->if_id))
		t->txpriv.offchannel_if_id = 2;
	else
		t->txpriv.offchannel_if_id = 0;
#endif

	if (likely(t->tx_info->control.sta && t->sta_priv->link_id))
		t->txpriv.raw_link_id =
				t->txpriv.link_id =
				t->sta_priv->link_id;
	else if (priv->mode != NL80211_IFTYPE_AP)
		t->txpriv.raw_link_id =
				t->txpriv.link_id = 0;
	else if (is_multicast_ether_addr(t->da)) {
		if (priv->enable_beacon) {
			t->txpriv.raw_link_id = 0;
			t->txpriv.link_id = priv->link_id_after_dtim;
		} else {
			t->txpriv.raw_link_id = 0;
			t->txpriv.link_id = 0;
		}
	} else {
		t->txpriv.link_id =
			xradio_find_link_id(priv, t->da);
		/* Do not assign valid link id for deauth/disassoc frame being
		transmitted to an unassociated STA */
		if (!(t->txpriv.link_id) &&
			(ieee80211_is_deauth(t->hdr->frame_control) ||
			ieee80211_is_disassoc(t->hdr->frame_control))) {
					t->txpriv.link_id = 0;
		} else {
			if (!t->txpriv.link_id)
				t->txpriv.link_id = xradio_alloc_link_id(priv, t->da);
			if (!t->txpriv.link_id) {
				txrx_printk(XRADIO_DBG_ERROR,
					    "%s: No more link IDs available.\n", __func__);
				return -ENOENT;
			}
		}
		t->txpriv.raw_link_id = t->txpriv.link_id;
	}
	if (t->txpriv.raw_link_id)
		priv->link_id_db[t->txpriv.raw_link_id - 1].timestamp =
				jiffies;

#if defined(CONFIG_XRADIO_USE_EXTENSIONS)
	if (t->tx_info->control.sta &&
			(t->tx_info->control.sta->uapsd_queues & BIT(t->queue)))
		t->txpriv.link_id = priv->link_id_uapsd;
#endif /* CONFIG_XRADIO_USE_EXTENSIONS */
	return 0;
}

static void
xradio_tx_h_pm(struct xradio_vif *priv,
	       struct xradio_txinfo *t)
{
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (unlikely(ieee80211_is_auth(t->hdr->frame_control))) {
		u32 mask = ~BIT(t->txpriv.raw_link_id);
		spin_lock_bh(&priv->ps_state_lock);
		priv->sta_asleep_mask &= mask;
		priv->pspoll_mask &= mask;
		spin_unlock_bh(&priv->ps_state_lock);
	}
}

static void
xradio_tx_h_calc_tid(struct xradio_vif *priv,
		     struct xradio_txinfo *t)
{
	if (ieee80211_is_data_qos(t->hdr->frame_control)) {
		u8 *qos = ieee80211_get_qos_ctl(t->hdr);
		t->txpriv.tid = qos[0] & IEEE80211_QOS_CTL_TID_MASK;
	} else if (ieee80211_is_data(t->hdr->frame_control)) {
		t->txpriv.tid = 0;
	}
}

/* IV/ICV injection. */
/* TODO: Quite unoptimal. It's better co modify mac80211
 * to reserve space for IV */
static int
xradio_tx_h_crypt(struct xradio_vif *priv,
		  struct xradio_txinfo *t)
{
	size_t iv_len;
	size_t icv_len;
	u8 *icv;
	u8 *newhdr;
	int is_multi_mfp = 0;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (ieee80211_is_mgmt(t->hdr->frame_control) &&
		 is_multicast_ether_addr(t->hdr->addr1) &&
		 ieee80211_is_robust_mgmt_frame(t->skb))
		 is_multi_mfp = 1;

	if (!t->tx_info->control.hw_key ||
	    (!(t->hdr->frame_control &
	     __cpu_to_le32(IEEE80211_FCTL_PROTECTED)) && (!is_multi_mfp)))
		return 0;

	iv_len = t->tx_info->control.hw_key->iv_len;
	icv_len = t->tx_info->control.hw_key->icv_len;
#ifdef AP_ARP_COMPAT_FIX
	t->txpriv.iv_len = iv_len;
#endif
	if (t->tx_info->control.hw_key->cipher == WLAN_CIPHER_SUITE_TKIP)
		icv_len += 8; /* MIC */

	if ((skb_headroom(t->skb) + skb_tailroom(t->skb) <
			 iv_len + icv_len + WSM_TX_EXTRA_HEADROOM) ||
			(skb_headroom(t->skb) <
			 iv_len + WSM_TX_EXTRA_HEADROOM)) {
		txrx_printk(XRADIO_DBG_ERROR,
			"Bug: no space allocated for crypto headers.\n"
			"headroom: %d, tailroom: %d, "
			"req_headroom: %zu, req_tailroom: %zu\n"
			"Please fix it in xradio_get_skb().\n",
			skb_headroom(t->skb), skb_tailroom(t->skb),
			iv_len + WSM_TX_EXTRA_HEADROOM, icv_len);
		return -ENOMEM;
	} else if (skb_tailroom(t->skb) < icv_len) {
		size_t offset = icv_len - skb_tailroom(t->skb);
		u8 *p;
		txrx_printk(XRADIO_DBG_ERROR,
			"Slowpath: tailroom is not big enough. "
			"Req: %zu, got: %d.\n",
			icv_len, skb_tailroom(t->skb));

		p = skb_push(t->skb, offset);
		memmove(p, &p[offset], t->skb->len - offset);
		skb_trim(t->skb, t->skb->len - offset);
	}

	newhdr = skb_push(t->skb, iv_len);
	memmove(newhdr, newhdr + iv_len, t->hdrlen);
	t->hdr = (struct ieee80211_hdr *) newhdr;
	t->hdrlen += iv_len;
	icv = skb_put(t->skb, icv_len);

	if (t->tx_info->control.hw_key->cipher == WLAN_CIPHER_SUITE_AES_CMAC) {
		struct ieee80211_mmie * mmie = (struct ieee80211_mmie *) icv;
		memset(mmie, 0, sizeof(struct ieee80211_mmie));
		mmie->element_id = WLAN_EID_MMIE;
		mmie->length = sizeof(*mmie) - 2;
	}
	return 0;
}

static int
xradio_tx_h_align(struct xradio_vif *priv, struct xradio_txinfo *t,
		  u8 *flags)
{
	size_t offset = (size_t)t->skb->data & 3;
	u8 *newhdr;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (!offset)
		return 0;

	if (skb_headroom(t->skb) < offset) {
		txrx_printk(XRADIO_DBG_ERROR,
			"Bug: no space allocated "
			"for DMA alignment.\n"
			"headroom: %d\n",
			skb_headroom(t->skb));
		return -ENOMEM;
	}
    /* offset = 1or3 process */
	if (offset & 1) {
		newhdr = skb_push(t->skb, offset);
		memmove(newhdr, newhdr + offset, t->skb->len-offset);
		skb_trim(t->skb, t->skb->len-offset);
		t->hdr = (struct ieee80211_hdr *) newhdr;
		xradio_debug_tx_align(priv);
		return 0;
	}
	/* offset=2 process */
	skb_push(t->skb, offset);
	t->hdrlen += offset;
	t->txpriv.offset += offset;
	*flags |= WSM_TX_2BYTES_SHIFT;
	xradio_debug_tx_align(priv);
	return 0;
}

static int
xradio_tx_h_action(struct xradio_vif *priv, struct xradio_txinfo *t)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)t->hdr;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (ieee80211_is_action(t->hdr->frame_control) &&
			mgmt->u.action.category == WLAN_CATEGORY_BACK)
		return 1;
	else
		return 0;
}

/* Add WSM header */
static struct wsm_tx *
xradio_tx_h_wsm(struct xradio_vif *priv, struct xradio_txinfo *t)
{
	struct wsm_tx *wsm;
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (skb_headroom(t->skb) < sizeof(struct wsm_tx)) {
		txrx_printk(XRADIO_DBG_ERROR,
			"Bug: no space allocated "
			"for WSM header.\n"
			"headroom: %d\n",
			skb_headroom(t->skb));
		return NULL;
	}

	wsm = (struct wsm_tx *)skb_push(t->skb, sizeof(struct wsm_tx));
	t->txpriv.offset += sizeof(struct wsm_tx);
	memset(wsm, 0, sizeof(*wsm));
	wsm->hdr.len = __cpu_to_le16(t->skb->len);
	wsm->hdr.id  = __cpu_to_le16(0x0004);
	wsm->queueId = (t->txpriv.raw_link_id << 2) | wsm_queue_id_to_wsm(t->queue);
	if (wsm->hdr.len > hw_priv->wsm_caps.sizeInpChBuf) {
		txrx_printk(XRADIO_DBG_ERROR, "%s,msg length too big=%d\n",
			    __func__, wsm->hdr.len);
		skb_pull(t->skb, sizeof(struct wsm_tx));  //skb revert.
		wsm = NULL;
	}

	return wsm;
}

/* BT Coex specific handling */
static void xradio_tx_h_bt(struct xradio_vif *priv, struct xradio_txinfo *t,
						   struct wsm_tx *wsm)
{
	u8 priority = 0;
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (!hw_priv->is_BT_Present)
		return;

	if (unlikely(ieee80211_is_nullfunc(t->hdr->frame_control)))
		priority = WSM_EPTA_PRIORITY_MGT;
	else if (ieee80211_is_data(t->hdr->frame_control)) {
		/* Skip LLC SNAP header (+6) */
		u8 *payload = &t->skb->data[t->hdrlen];
		u16 *ethertype = (u16 *) &payload[6];
		if (unlikely(*ethertype == __be16_to_cpu(ETH_P_PAE)))
			priority = WSM_EPTA_PRIORITY_EAPOL;
	} else if (unlikely(ieee80211_is_assoc_req(t->hdr->frame_control) ||
		ieee80211_is_reassoc_req(t->hdr->frame_control))) {
		struct ieee80211_mgmt *mgt_frame =
				(struct ieee80211_mgmt *)t->hdr;

		if (mgt_frame->u.assoc_req.listen_interval <
						priv->listen_interval) {
			txrx_printk(XRADIO_DBG_MSG,
				"Modified Listen Interval to %d from %d\n",
				priv->listen_interval,
				mgt_frame->u.assoc_req.listen_interval);
			/* Replace listen interval derieved from
			 * the one read from SDD */
			mgt_frame->u.assoc_req.listen_interval =
				priv->listen_interval;
		}
	}

	if (likely(!priority)) {
		if (ieee80211_is_action(t->hdr->frame_control))
			priority = WSM_EPTA_PRIORITY_ACTION;
		else if (ieee80211_is_mgmt(t->hdr->frame_control))
			priority = WSM_EPTA_PRIORITY_MGT;
		else if ((wsm->queueId == WSM_QUEUE_VOICE))
			priority = WSM_EPTA_PRIORITY_VOICE;
		else if ((wsm->queueId == WSM_QUEUE_VIDEO))
			priority = WSM_EPTA_PRIORITY_VIDEO;
		else
			priority = WSM_EPTA_PRIORITY_DATA;
	}

	txrx_printk(XRADIO_DBG_MSG, "[TX] EPTA priority %d.\n",
		priority);

	wsm->flags |= priority << 1;
}

static int
xradio_tx_h_rate_policy(struct xradio_vif *priv,
						struct xradio_txinfo *t,
						struct wsm_tx *wsm)
{
	bool tx_policy_renew = false;
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	/*use debug policy for data frames only*/
#ifdef CONFIG_XRADIO_DEBUGFS
	if ((rates_dbg_en & 0x1) && ieee80211_is_data(t->hdr->frame_control)) {
		rates_dbg_en |= 0x02;
	}
#endif

	t->txpriv.rate_id = tx_policy_get(priv,
		t->tx_info->control.rates, t->txpriv.use_bg_rate,
		&tx_policy_renew);
	if (t->txpriv.rate_id == XRADIO_INVALID_RATE_ID)
		return -EFAULT;

	wsm->flags |= t->txpriv.rate_id << 4;
	t->rate = xradio_get_tx_rate(hw_priv, &t->tx_info->control.rates[0]);
	if (t->txpriv.use_bg_rate)
		wsm->maxTxRate = (u8)(t->txpriv.use_bg_rate & 0x3f);
	else
		wsm->maxTxRate = t->rate->hw_value;

	/*set the maxTxRate and clear the dataframe flag of rates_dbg_en */
#ifdef CONFIG_XRADIO_DEBUGFS
	if (rates_dbg_en & 0x02) {
		wsm->maxTxRate = maxRate_dbg;
		rates_dbg_en  &= ~0x2;
	}
#endif

	if (t->rate->flags & IEEE80211_TX_RC_MCS) {
		if (priv->association_mode.greenfieldMode) {
			wsm->htTxParameters |=
				__cpu_to_le32(WSM_HT_TX_GREENFIELD);
			txrx_printk(XRADIO_DBG_NIY, "[TX] GF.\n");
		} else {
			wsm->htTxParameters |= __cpu_to_le32(WSM_HT_TX_MIXED);
			if (t->tx_info->control.rates[0].flags & IEEE80211_TX_RC_SHORT_GI) {
				wsm->htTxParameters |=
					__cpu_to_le32(WSM_HT_TX_SGI);
				txrx_printk(XRADIO_DBG_NIY, "[TX] SGI.\n");
			} else {
				txrx_printk(XRADIO_DBG_NIY, "[TX] LGI.\n");
			}
#ifdef CONFIG_XRADIO_DEBUGFS
			if (rate_sgi == 1)
				wsm->htTxParameters |= __cpu_to_le32(WSM_HT_TX_SGI);
			else if (rate_sgi == 2)
				wsm->htTxParameters &= __cpu_to_le32(~WSM_HT_TX_SGI);
#endif
		}
	}

	if (tx_policy_renew) {
		txrx_printk(XRADIO_DBG_MSG, "[TX] TX policy renew.\n");
		/* It's not so optimal to stop TX queues every now and then.
		 * Maybe it's better to reimplement task scheduling with
		 * a counter. */
		/* xradio_tx_queues_lock(priv); */
		/* Definetly better. TODO. */
		if (atomic_add_return(1, &hw_priv->upload_count) == 1) {
			wsm_lock_tx_async(hw_priv);
			if (queue_work(hw_priv->workqueue,
				  &hw_priv->tx_policy_upload_work) <= 0) {
				atomic_set(&hw_priv->upload_count, 0);
				wsm_unlock_tx(hw_priv);
			}
		}
	}
	return 0;
}

static bool
xradio_tx_h_pm_state(struct xradio_vif *priv, struct xradio_txinfo *t)
{
	int was_buffered = 1;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (t->txpriv.link_id == priv->link_id_after_dtim &&
			!priv->buffered_multicasts) {
		priv->buffered_multicasts = true;
		if (priv->sta_asleep_mask)
			queue_work(priv->hw_priv->workqueue,
				&priv->multicast_start_work);
	}

	if (t->txpriv.raw_link_id && t->txpriv.tid < XRADIO_MAX_TID)
		was_buffered = priv->link_id_db[t->txpriv.raw_link_id - 1]
				.buffered[t->txpriv.tid]++;

	return !was_buffered;
}

static void
xradio_tx_h_ba_stat(struct xradio_vif *priv,
		    struct xradio_txinfo *t)
{
	struct xradio_common *hw_priv = priv->hw_priv;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (priv->join_status != XRADIO_JOIN_STATUS_STA)
		return;
	if (!xradio_is_ht(&hw_priv->ht_info))
		return;
	if (!priv->setbssparams_done)
		return;
	if (!ieee80211_is_data(t->hdr->frame_control))
		return;

	spin_lock_bh(&hw_priv->ba_lock);
	hw_priv->ba_acc += t->skb->len - t->hdrlen;
	if (!(hw_priv->ba_cnt_rx || hw_priv->ba_cnt)) {
		mod_timer(&hw_priv->ba_timer,
			jiffies + XRADIO_BLOCK_ACK_INTERVAL);
	}
	hw_priv->ba_cnt++;
	spin_unlock_bh(&hw_priv->ba_lock);
}

static int
xradio_tx_h_skb_pad(struct xradio_common *priv,
		    struct wsm_tx *wsm,
		    struct sk_buff *skb)
{
	size_t len = __le16_to_cpu(wsm->hdr.len);
	size_t padded_len = priv->sbus_ops->align_size(priv->sbus_priv, len);
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (SYS_WARN(skb_padto(skb, padded_len) != 0)) {
		return -EINVAL;
	}
	return 0;
}

/* ******************************************************************** */
#ifdef CONFIG_XRADIO_DEBUGFS
u16  txparse_flags;
u16  rxparse_flags;
#endif

void xradio_tx(struct ieee80211_hw *dev, struct sk_buff *skb)
{
	struct xradio_common *hw_priv = dev->priv;
	struct xradio_txinfo t = {
		.skb = skb,
		.queue = skb_get_queue_mapping(skb),
		.tx_info = IEEE80211_SKB_CB(skb),
		.hdr = (struct ieee80211_hdr *)skb->data,
		.txpriv.tid = XRADIO_MAX_TID,
		.txpriv.rate_id = XRADIO_INVALID_RATE_ID,
#ifdef P2P_MULTIVIF
		.txpriv.raw_if_id = 0,
#endif
		.txpriv.use_bg_rate = 0,
#ifdef AP_ARP_COMPAT_FIX
		.txpriv.iv_len = 0,
#endif
	};
	struct ieee80211_sta *sta;
	struct wsm_tx *wsm;
	bool tid_update = 0;
	u8 flags = 0;
	int ret = 0;
	struct xradio_vif *priv;
	struct ieee80211_hdr *frame = (struct ieee80211_hdr *)skb->data;
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;

	if (!skb->data)
		SYS_BUG(1);

#ifdef HW_RESTART
	if (hw_priv->hw_restart) {
		txrx_printk(XRADIO_DBG_WARN, "%s, hw in reset.\n", __func__);
		ret = __LINE__;
		goto drop;
	}
#endif

	if (!(t.tx_info->control.vif)) {
		ret = __LINE__;
		goto drop;
	}
	priv = xrwl_get_vif_from_ieee80211(t.tx_info->control.vif);
	if (!priv) {
		ret = __LINE__;
		goto drop;
	}

	if (atomic_read(&priv->enabled) == 0) {
		ret = __LINE__;
		goto drop;
	}

#ifdef CONFIG_XRADIO_DEBUGFS
	/* parse frame for debug. */
	if (txparse_flags)
		xradio_parse_frame(skb, 0, txparse_flags, priv->if_id);
#endif

	/*
	 * dhcp and 8021 frames are important, use b/g rate and delay scan.
	 * it can make sense, such as accelerate connect.
	 */
	if (ieee80211_is_auth(frame->frame_control)) {
		hw_priv->scan_delay_time[priv->if_id] = jiffies;
		hw_priv->scan_delay_status[priv->if_id] = XRADIO_SCAN_DELAY;
	} else if (ieee80211_is_data_present(frame->frame_control)) {
		u8 *llc = skb->data+ieee80211_hdrlen(frame->frame_control);
		if (is_dhcp(llc) || is_8021x(llc)) {
			t.txpriv.use_bg_rate =
			hw_priv->hw->wiphy-> \
			   bands[hw_priv->channel->band]->bitrates[0].hw_value;
			if (priv->vif->p2p)
				t.txpriv.use_bg_rate = AG_RATE_INDEX;
			t.txpriv.use_bg_rate |= 0x80;
		}
		if (t.txpriv.use_bg_rate) {
			hw_priv->scan_delay_time[priv->if_id] = jiffies;
			hw_priv->scan_delay_status[priv->if_id] = XRADIO_SCAN_DELAY;
		}
	} else if (ieee80211_is_deauth(frame->frame_control) ||
		   ieee80211_is_disassoc(frame->frame_control)) {
		hw_priv->scan_delay_status[priv->if_id] = XRADIO_SCAN_ALLOW;
	}

#ifdef AP_HT_COMPAT_FIX
	if (ieee80211_is_assoc_req(frame->frame_control) &&
		priv->if_id == 0 && !(priv->ht_compat_det & 0x10)) {
		xradio_remove_ht_ie(priv, skb);
	}
#endif

#ifdef AP_ARP_COMPAT_FIX
	if ((priv->join_status == XRADIO_JOIN_STATUS_STA) &&
		(priv->arp_compat_cnt >= 2) &&
		ieee80211_is_data(frame->frame_control)) {
		static const u8 bcast_addr[ETH_ALEN] = { 0xff,
			0xff, 0xff, 0xff, 0xff, 0xff };
		if (!is_multicast_ether_addr(frame->addr1) &&
			(memcmp(frame->addr1, bcast_addr, ETH_ALEN) != 0)) {
			u8 machdrlen = ieee80211_hdrlen(frame->frame_control);
			u8 *llc_data = (u8 *)frame + machdrlen + 0;
			if (!is_arp(llc_data))
				priv->arp_compat_cnt = 0;
		}
	}
#endif

#ifdef CONFIG_XRADIO_TESTMODE
	spin_lock_bh(&hw_priv->tsm_lock);
	if (hw_priv->start_stop_tsm.start) {
		if (hw_priv->tsm_info.ac == t.queue)
			hw_priv->tsm_stats.txed_msdu_count++;
	}
	spin_unlock_bh(&hw_priv->tsm_lock);
#endif /*CONFIG_XRADIO_TESTMODE*/

#ifdef TES_P2P_0002_ROC_RESTART
	xradio_frame_monitor(hw_priv, skb, true);
#endif

	if (ieee80211_is_action(frame->frame_control) &&
		mgmt->u.action.category == WLAN_CATEGORY_PUBLIC) {
		u8 *action = (u8 *)&mgmt->u.action.category;
		xradio_check_go_neg_conf_success(hw_priv, action);
		xradio_check_prov_desc_req(hw_priv, action);
	}

	t.txpriv.if_id = priv->if_id;
	t.hdrlen = ieee80211_hdrlen(t.hdr->frame_control);
	t.da = ieee80211_get_DA(t.hdr);
	t.sta_priv =
		(struct xradio_sta_priv *)&t.tx_info->control.sta->drv_priv;

	if (SYS_WARN(t.queue >= 4)) {
		ret = __LINE__;
		goto drop;
	}

	/*
	spin_lock_bh(&hw_priv->tx_queue[t.queue].lock);
	if ((priv->if_id == 0) &&
		(hw_priv->tx_queue[t.queue].num_queued_vif[0] >=
			hw_priv->vif0_throttle)) {
		spin_unlock_bh(&hw_priv->tx_queue[t.queue].lock);
		ret = __LINE__;
		goto drop;
	} else if ((priv->if_id == 1) &&
		(hw_priv->tx_queue[t.queue].num_queued_vif[1] >=
			hw_priv->vif1_throttle)) {
		spin_unlock_bh(&hw_priv->tx_queue[t.queue].lock);
		ret = __LINE__;
		goto drop;
	}
	spin_unlock_bh(&hw_priv->tx_queue[t.queue].lock);
	*/

	ret = xradio_tx_h_calc_link_ids(priv, &t);
	if (ret) {
		ret = __LINE__;
		goto drop;
	}

	txrx_printk(XRADIO_DBG_MSG, "[TX] TX %d bytes (if_id: %d,"
			" queue: %d, link_id: %d (%d)).\n",
			skb->len, priv->if_id, t.queue, t.txpriv.link_id,
			t.txpriv.raw_link_id);

	xradio_tx_h_pm(priv, &t);
	xradio_tx_h_calc_tid(priv, &t);
	ret = xradio_tx_h_crypt(priv, &t);
	if (ret) {
		ret = __LINE__;
		goto drop;
	}
	ret = xradio_tx_h_align(priv, &t, &flags);
	if (ret) {
		ret = __LINE__;
		goto drop;
	}
	ret = xradio_tx_h_action(priv, &t);
	if (ret) {
		ret = __LINE__;
		goto drop;
	}
	wsm = xradio_tx_h_wsm(priv, &t);
	if (!wsm) {
		ret = __LINE__;
		goto drop;
	}
#ifdef CONFIG_XRADIO_TESTMODE
	flags |= WSM_TX_FLAG_EXPIRY_TIME;
#endif /*CONFIG_XRADIO_TESTMODE*/
	wsm->flags |= flags;
	xradio_tx_h_bt(priv, &t, wsm);
	ret = xradio_tx_h_rate_policy(priv, &t, wsm);
	if (ret) {
		ret = __LINE__;
		goto drop;
	}

	ret = xradio_tx_h_skb_pad(hw_priv, wsm, skb);
	if (ret) {
		ret = __LINE__;
		goto drop;
	}

	rcu_read_lock();
	sta = rcu_dereference(t.tx_info->control.sta);

	xradio_tx_h_ba_stat(priv, &t);
	spin_lock_bh(&priv->ps_state_lock);
	tid_update = xradio_tx_h_pm_state(priv, &t);
	SYS_BUG(xradio_queue_put(&hw_priv->tx_queue[t.queue],
			t.skb, &t.txpriv));
#ifdef ROC_DEBUG
	txrx_printk(XRADIO_DBG_ERROR, "QPUT %x, %pM, if_id - %d\n",
		t.hdr->frame_control, t.da, priv->if_id);
#endif
	spin_unlock_bh(&priv->ps_state_lock);

	/* To improve tcp tx in linux4.9
	 * skb_orphan will tell tcp that driver has processed this skb,
	 * so tcp can send other skb to driver.
	 * If this is a retransmitted frame by umac, driver do not skb_orphan it again.
	 */
	if (!(t.tx_info->flags & IEEE80211_TX_INTFL_RETRANSMISSION))
		skb_orphan(skb);

#if defined(CONFIG_XRADIO_USE_EXTENSIONS)
	if (tid_update && sta)
		mac80211_sta_set_buffered(sta,
				t.txpriv.tid, true);
#endif /* CONFIG_XRADIO_USE_EXTENSIONS */

	rcu_read_unlock();

	xradio_bh_wakeup(hw_priv);

	return;

drop:
	txrx_printk(XRADIO_DBG_WARN, "drop=%d, fctl=0x%04x.\n",
		    ret, frame->frame_control);
	if (!(t.tx_info->flags & IEEE80211_TX_INTFL_RETRANSMISSION))
		skb_orphan(skb);
	xradio_skb_post_gc(hw_priv, skb, &t.txpriv);
	return;
}

/* ******************************************************************** */

static int xradio_handle_pspoll(struct xradio_vif *priv,
				struct sk_buff *skb)
{
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	struct ieee80211_sta *sta;
	struct ieee80211_pspoll *pspoll =
		(struct ieee80211_pspoll *) skb->data;
	int link_id = 0;
	u32 pspoll_mask = 0;
	int drop = 1;
	int i;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (priv->join_status != XRADIO_JOIN_STATUS_AP)
		goto done;
	if (memcmp(priv->vif->addr, pspoll->bssid, ETH_ALEN))
		goto done;

	rcu_read_lock();
	sta = mac80211_find_sta(priv->vif, pspoll->ta);
	if (sta) {
		struct xradio_sta_priv *sta_priv;
		sta_priv = (struct xradio_sta_priv *)&sta->drv_priv;
		link_id = sta_priv->link_id;
		pspoll_mask = BIT(sta_priv->link_id);
	}
	rcu_read_unlock();
	if (!link_id)
		goto done;

	priv->pspoll_mask |= pspoll_mask;
	drop = 0;

	/* Do not report pspols if data for given link id is
	 * queued already. */
	for (i = 0; i < 4; ++i) {
		if (xradio_queue_get_num_queued(priv,
				&hw_priv->tx_queue[i],
				pspoll_mask)) {
			xradio_bh_wakeup(hw_priv);
			drop = 1;
			break;
		}
	}
	txrx_printk(XRADIO_DBG_NIY, "[RX] PSPOLL: %s\n", drop ? "local" : "fwd");
done:
	return drop;
}

/* ******************************************************************** */
extern u32 tx_retrylimit;
extern u32 tx_over_limit;
extern u32 tx_lower_limit;
extern int retry_mis;

void xradio_tx_confirm_cb(struct xradio_common *hw_priv,
			  struct wsm_tx_confirm *arg)
{
	u8 queue_id = xradio_queue_get_queue_id(arg->packetID);
	struct xradio_queue *queue = &hw_priv->tx_queue[queue_id];
	struct sk_buff *skb;
	const struct xradio_txpriv *txpriv;
	struct xradio_vif *priv;
	u32    feedback_retry = 0;

	if (arg->status) {
		txrx_printk(XRADIO_DBG_NIY, "status=%d, retry=%d, lastRate=%d\n",
			    arg->status, arg->ackFailures, arg->txedRate);
	} else {
		txrx_printk(XRADIO_DBG_MSG, "status=%d, retry=%d, lastRate=%d\n",
			    arg->status, arg->ackFailures, arg->txedRate);
	}

#ifdef TES_P2P_0002_ROC_RESTART
	if ((TES_P2P_0002_state == TES_P2P_0002_STATE_GET_PKTID) &&
		(arg->packetID == TES_P2P_0002_packet_id)) {
		if (arg->status == 0x00) {

			struct timeval TES_P2P_0002_tmval;
			s32 TES_P2P_0002_roc_time;
			s32 TES_P2P_0002_now_sec;
			s32 TES_P2P_0002_now_usec;
			bool TES_P2P_0002_roc_rst_need;

			xr_do_gettimeofday(&TES_P2P_0002_tmval);
			TES_P2P_0002_roc_rst_need	= false;
			TES_P2P_0002_now_sec = (s32)(TES_P2P_0002_tmval.tv_sec);
			TES_P2P_0002_now_usec = (s32)(TES_P2P_0002_tmval.tv_usec);
			TES_P2P_0002_roc_time = TES_P2P_0002_roc_dur -
			    (((TES_P2P_0002_now_sec - TES_P2P_0002_roc_sec) * 1000) +
			    ((TES_P2P_0002_now_usec - TES_P2P_0002_roc_usec) / 1000));

			/* tx rsp to rx cfm will need more than 60ms */
			if (TES_P2P_0002_roc_time < 100) {
				TES_P2P_0002_roc_time = 100;
				TES_P2P_0002_roc_rst_need = true;
			}

			if (TES_P2P_0002_roc_rst_need == true) {
				txrx_printk(XRADIO_DBG_WARN,
					    "[ROC RESTART ACTIVE ON][Confirm CallBack]");
				cancel_delayed_work_sync(&hw_priv->rem_chan_timeout);
				if (atomic_read(&hw_priv->remain_on_channel)) {
					queue_delayed_work(hw_priv->spare_workqueue,
							   &hw_priv->rem_chan_timeout,
							   (TES_P2P_0002_roc_time) * HZ / 1000);
				}
			}
		}
		TES_P2P_0002_state = TES_P2P_0002_STATE_IDLE;
		txrx_printk(XRADIO_DBG_NIY,
			    "[ROC_RESTART_STATE_IDLE][Confirm CallBack]");
	}
#endif

	if (unlikely(xradio_itp_tx_running(hw_priv)))
		return;

	priv = xrwl_hwpriv_to_vifpriv(hw_priv, arg->if_id);
	if (unlikely(!priv))
		return;
	if (unlikely(priv->mode == NL80211_IFTYPE_UNSPECIFIED)) {
		/* STA is stopped. */
		spin_unlock(&priv->vif_lock);
		return;
	}

	if (SYS_WARN(queue_id >= 4)) {
		spin_unlock(&priv->vif_lock);
		return;
	}

#ifdef CONFIG_XRADIO_TESTMODE
	spin_lock_bh(&hw_priv->tsm_lock);
	if ((arg->status == WSM_STATUS_RETRY_EXCEEDED) ||
	    (arg->status == WSM_STATUS_TX_LIFETIME_EXCEEDED)) {
		hw_priv->tsm_stats.msdu_discarded_count++;
	} else if ((hw_priv->start_stop_tsm.start) &&
		(arg->status == WSM_STATUS_SUCCESS)) {
		if (queue_id == hw_priv->tsm_info.ac) {
			struct timeval tmval;
			xr_do_gettimeofday(&tmval);
			u16 pkt_delay =
				hw_priv->start_stop_tsm.packetization_delay;
			if (hw_priv->tsm_info.sta_roamed &&
			    !hw_priv->tsm_info.use_rx_roaming) {
				hw_priv->tsm_info.roam_delay = tmval.tv_usec -
				hw_priv->tsm_info.txconf_timestamp_vo;
				if (hw_priv->tsm_info.roam_delay > pkt_delay)
					hw_priv->tsm_info.roam_delay -= pkt_delay;
				txrx_printk(XRADIO_DBG_MSG, "[TX] txConf"
				"Roaming: roam_delay = %u\n",
				hw_priv->tsm_info.roam_delay);
				hw_priv->tsm_info.sta_roamed = 0;
			}
			hw_priv->tsm_info.txconf_timestamp_vo = tmval.tv_usec;
		}
	}
	spin_unlock_bh(&hw_priv->tsm_lock);
#endif /*CONFIG_XRADIO_TESTMODE*/
	if ((arg->status == WSM_REQUEUE) &&
	    (arg->flags & WSM_TX_STATUS_REQUEUE)) {
		/* "Requeue" means "implicit suspend" */
		struct wsm_suspend_resume suspend = {
			.link_id = arg->link_id,
			.stop = 1,
			.multicast = !arg->link_id,
			.if_id = arg->if_id,
		};
		xradio_suspend_resume(priv, &suspend);
		txrx_printk(XRADIO_DBG_NIY, "Requeue for link_id %d (try %d)."
			" STAs asleep: 0x%.8X\n",
			arg->link_id,
			xradio_queue_get_generation(arg->packetID) + 1,
			priv->sta_asleep_mask);
#ifdef CONFIG_XRADIO_TESTMODE
		SYS_WARN(xradio_queue_requeue(hw_priv, queue,
				arg->packetID, true));
#else
		SYS_WARN(xradio_queue_requeue(queue,
				arg->packetID, true));
#endif
		spin_lock_bh(&priv->ps_state_lock);
		if (!arg->link_id) {
			priv->buffered_multicasts = true;
			if (priv->sta_asleep_mask) {
				queue_work(hw_priv->workqueue,
					&priv->multicast_start_work);
			}
		}
		spin_unlock_bh(&priv->ps_state_lock);
		spin_unlock(&priv->vif_lock);
	} else if (!xradio_queue_get_skb(
			queue, arg->packetID, &skb, &txpriv)) {
		struct ieee80211_tx_info *tx = IEEE80211_SKB_CB(skb);
		struct ieee80211_hdr *frame =
		    (struct ieee80211_hdr *)&skb->data[txpriv->offset];
		int tx_count = arg->ackFailures;
		u8 ht_flags = 0;
		int i;

		/*
		 * reset if_0 in firmware when STA-unjoined,
		 * fix the errors when switch APs in combo mode.
		 */
		if (unlikely(ieee80211_is_disassoc(frame->frame_control) ||
			  ieee80211_is_deauth(frame->frame_control))) {
			if (priv->join_status == XRADIO_JOIN_STATUS_STA) {
				wsm_send_deauth_to_self(hw_priv, priv);
				/* Shedule unjoin work */
				txrx_printk(XRADIO_DBG_WARN,
					    "Issue unjoin command(TX) by self.\n");
				if (cancel_delayed_work_sync(&priv->unjoin_delayed_work)) {
					wsm_lock_tx_async(hw_priv);
					if (queue_work(hw_priv->workqueue, &priv->unjoin_work) <= 0)
						wsm_unlock_tx(hw_priv);
				}
			}
		}

#ifdef ROC_DEBUG
#ifndef P2P_MULTIVIF
		if (txpriv->offchannel_if_id)
			txrx_printk(XRADIO_DBG_ERROR, "TX CONFIRM %x - %d - %d\n",
				skb->data[txpriv->offset],
				txpriv->offchannel_if_id, arg->status);
#else
		if (txpriv->if_id)
			txrx_printk(XRADIO_DBG_ERROR, "TX CONFIRM %x - %d - %d\n",
				skb->data[txpriv->offset],
				txpriv->raw_if_id, arg->status);
#endif
#endif
		if (priv->association_mode.greenfieldMode)
			ht_flags |= IEEE80211_TX_RC_GREEN_FIELD;

		/* bss loss confirm. */
		if (unlikely(priv->bss_loss_status == XRADIO_BSS_LOSS_CONFIRMING &&
		    priv->bss_loss_confirm_id == arg->packetID)) {
			spin_lock(&priv->bss_loss_lock);
			priv->bss_loss_status = arg->status ? XRADIO_BSS_LOSS_CONFIRMED :
						    XRADIO_BSS_LOSS_NONE;
			spin_unlock(&priv->bss_loss_lock);
		}

/*when less ap can't reply arp request accidentally,
*then disconnect actively,
*and wait system trigger reconnect again.
*/
#ifdef AP_ARP_COMPAT_FIX
		if (likely(!arg->status) &&
			(priv->join_status == XRADIO_JOIN_STATUS_STA) &&
			(ieee80211_is_data(frame->frame_control))) {
				u8 machdrlen = ieee80211_hdrlen(frame->frame_control);
				u8 *llc_data = (u8 *)frame + machdrlen + txpriv->iv_len;
				if (is_SNAP(llc_data) && is_arp(llc_data)) {
					u8 *arp_hdr = llc_data + LLC_LEN;
					u16 *arp_type = (u16 *)(arp_hdr + ARP_TYPE_OFFSET);
					if (*arp_type == cpu_to_be16(ARP_REQUEST))
						priv->arp_compat_cnt++;
					if (priv->arp_compat_cnt > 10) {
						txrx_printk(XRADIO_DBG_ERROR,
							"ap don't reply arp resp count=%d\n",
							priv->arp_compat_cnt);
						priv->arp_compat_cnt = 0;
						wsm_send_disassoc_to_self(hw_priv, priv);
					}
				}
		}
#endif

		if (likely(!arg->status)) {
			tx->flags |= IEEE80211_TX_STAT_ACK;
			priv->cqm_tx_failure_count = 0;
			++tx_count;
			if (arg->txedRate < 24)
				TxedRateIdx_Map[arg->txedRate]++;
			else
				SYS_WARN(1);
			xradio_debug_txed(priv);
			if (arg->flags & WSM_TX_STATUS_AGGREGATION) {
				/* Do not report aggregation to mac80211:
				 * it confuses minstrel a lot. */
				/* tx->flags |= IEEE80211_TX_STAT_AMPDU; */
				xradio_debug_txed_agg(priv);
			}
		} else {
			/* TODO: Update TX failure counters */
			if (unlikely(priv->cqm_tx_failure_thold &&
			     (++priv->cqm_tx_failure_count >
			      priv->cqm_tx_failure_thold))) {
				priv->cqm_tx_failure_thold = 0;
				queue_work(hw_priv->workqueue,
						&priv->tx_failure_work);
			}
			if (tx_count)
				++tx_count;
		}
		spin_unlock(&priv->vif_lock);

		tx->status.ampdu_len = 1;
		tx->status.ampdu_ack_len = 1;

#if 0
		tx_count = arg->ackFailures+1;
		for (i = 0; i < IEEE80211_TX_MAX_RATES; ++i) {
			if (tx->status.rates[i].count >= tx_count) {
				tx->status.rates[i].count = tx_count;
				if (likely(!arg->status)) {
					s8 txed_idx = xradio_get_rate_idx(hw_priv,
							 tx->status.rates[i].flags,
							 arg->txedRate);
					if (tx->status.rates[i].idx != txed_idx) {
						if (i < (IEEE80211_TX_MAX_RATES-1)) {
							i++;
							tx->status.rates[i].idx   = txed_idx;
							tx->status.rates[i].count = 1;
						} else if (txed_idx >= 0) {
							tx->status.rates[i].idx   = txed_idx;
							tx->status.rates[i].count = 1;
						}
					}
				}
				break;
			}
			tx_count -= tx->status.rates[i].count;
			if (tx->status.rates[i].flags & IEEE80211_TX_RC_MCS)
				tx->status.rates[i].flags |= ht_flags;
		}

		for (++i; i < IEEE80211_TX_MAX_RATES; ++i) {
			tx->status.rates[i].count = 0;
			tx->status.rates[i].idx = -1;
		}

#else
		txrx_printk(XRADIO_DBG_MSG,
			    "feedback:%08x, %08x, %08x.\n",
			     arg->rate_try[2], arg->rate_try[1], arg->rate_try[0]);
		if (txpriv->use_bg_rate) {   /* bg rates */
			tx->status.rates[0].count = arg->ackFailures+1;
		  tx->status.rates[0].idx   = 0;
		  tx->status.rates[1].idx   = -1;
		  tx->status.rates[2].idx   = -1;
		  tx->status.rates[3].idx   = -1;
		  tx->status.rates[4].idx   = -1;
		} else {
			int j;
			s8  txed_idx;
			register u8 rate_num = 0, shift = 0, retries = 0;
			u8  flag = tx->status.rates[0].flags;

			/* get retry rate idx. */
			for (i = 2; i >= 0; i--) {
				if (arg->rate_try[i]) {
					for (j = 7; j >= 0; j--) {
						shift   = j<<2;
						retries = (arg->rate_try[i]>>shift) & 0xf;
						if (retries) {
							feedback_retry += retries;
							txed_idx = xradio_get_rate_idx(hw_priv, flag,
										       ((i<<3) + j));
							txrx_printk(XRADIO_DBG_MSG,
								    "rate_num=%d, hw=%d, idx=%d, "
								    "retries=%d, flag=%d",
								    rate_num, ((i<<3)+j),
								    txed_idx, retries, flag);
							if (likely(txed_idx >= 0)) {
								tx->status.rates[rate_num].idx   = txed_idx;
								tx->status.rates[rate_num].count = retries;
								if (tx->status.rates[rate_num].flags &
									IEEE80211_TX_RC_MCS)
									tx->status.rates[rate_num].flags |=
									    ht_flags;
								rate_num++;
								if (rate_num >= IEEE80211_TX_MAX_RATES) {
									i = -1;
									break;
								}
							}
						}
					}
				}
			}

			/* If there is 11b rates in 11n mode, put it into MCS0 */
			if ((arg->rate_try[0]&0xffff) && (flag & IEEE80211_TX_RC_MCS)) {
				int br_retrys = 0;
				for (i = 0; i < 16; i += 4)
					br_retrys += ((arg->rate_try[0]>>i)&0xf);
				if (rate_num > 0 && tx->status.rates[rate_num-1].idx == 0) {
					tx->status.rates[rate_num-1].count += br_retrys;
				} else if (rate_num < IEEE80211_TX_MAX_RATES) {
					tx->status.rates[rate_num].idx   = 0;
					tx->status.rates[rate_num].count += br_retrys;
					rate_num++;
				}
			}

			/* clear other rate. */
			for (i = rate_num; i < IEEE80211_TX_MAX_RATES; ++i) {
				tx->status.rates[i].count = 0;
				tx->status.rates[i].idx = -1;
			}
			/* get successful rate idx. */
			if (!arg->status) {
				txed_idx = xradio_get_rate_idx(hw_priv, flag, arg->txedRate);
				if (rate_num == 0) {
					tx->status.rates[0].idx = txed_idx;
					tx->status.rates[0].count = 1;
				} else if (rate_num <= IEEE80211_TX_MAX_RATES) {
					--rate_num;
					if (txed_idx == tx->status.rates[rate_num].idx) {
						tx->status.rates[rate_num].count += 1;
					} else if (rate_num < (IEEE80211_TX_MAX_RATES-1)) {
						++rate_num;
						tx->status.rates[rate_num].idx   = txed_idx;
						tx->status.rates[rate_num].count = 1;
					} else if (txed_idx >= 0) {
						tx->status.rates[rate_num].idx   = txed_idx;
						tx->status.rates[rate_num].count = 1;
					}
				}
			}
		}
#endif

#ifdef CONFIG_XRADIO_DEBUGFS
		if (arg->status == WSM_STATUS_RETRY_EXCEEDED) {
			tx_retrylimit++;
			retry_mis += ((s32)hw_priv->short_frame_max_tx_count -
				       arg->ackFailures-1);
			if (arg->ackFailures != (hw_priv->short_frame_max_tx_count-1)) {
				if (arg->ackFailures < (hw_priv->short_frame_max_tx_count-1))
					tx_lower_limit++;
				else
					tx_over_limit++;
				txrx_printk(XRADIO_DBG_NIY,
					    "retry_err, ackFailures=%d, feedbk_retry=%d.\n",
					    arg->ackFailures, feedback_retry);
			}
		} else if (feedback_retry > hw_priv->short_frame_max_tx_count-1) {
			tx_over_limit++;
			txrx_printk(XRADIO_DBG_WARN,
				    "status=%d, ackFailures=%d, feedbk_retry=%d.\n",
				    arg->status, arg->ackFailures, feedback_retry);
		}
#endif

		txrx_printk(XRADIO_DBG_MSG, "[TX policy] Ack: " \
		"%d:%d, %d:%d, %d:%d, %d:%d, %d:%d\n",
		tx->status.rates[0].idx, tx->status.rates[0].count,
		tx->status.rates[1].idx, tx->status.rates[1].count,
		tx->status.rates[2].idx, tx->status.rates[2].count,
		tx->status.rates[3].idx, tx->status.rates[3].count,
		tx->status.rates[4].idx, tx->status.rates[4].count);

#ifdef CONFIG_XRADIO_TESTMODE
		xradio_queue_remove(hw_priv, queue, arg->packetID);
#else
		xradio_queue_remove(queue, arg->packetID);
#endif /*CONFIG_XRADIO_TESTMODE*/
	} else {
		spin_unlock(&priv->vif_lock);
		txrx_printk(XRADIO_DBG_WARN,
			"%s xradio_queue_get_skb failed.\n", __func__);
	}
}

static void xradio_notify_buffered_tx(struct xradio_vif *priv,
			       struct sk_buff *skb, int link_id, int tid)
{
#if defined(CONFIG_XRADIO_USE_EXTENSIONS)
	struct ieee80211_sta *sta;
	struct ieee80211_hdr *hdr;
	u8 *buffered;
	u8 still_buffered = 0;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (link_id && tid < XRADIO_MAX_TID) {
		buffered = priv->link_id_db
				[link_id - 1].buffered;

		spin_lock_bh(&priv->ps_state_lock);
		if (!SYS_WARN(!buffered[tid]))
			still_buffered = --buffered[tid];
		spin_unlock_bh(&priv->ps_state_lock);

		if (!still_buffered && tid < XRADIO_MAX_TID) {
			hdr = (struct ieee80211_hdr *) skb->data;
			rcu_read_lock();
			sta = mac80211_find_sta(priv->vif, hdr->addr1);
			if (sta)
				mac80211_sta_set_buffered(sta, tid, false);
			rcu_read_unlock();
		}
	}
#endif /* CONFIG_XRADIO_USE_EXTENSIONS */
}

void xradio_skb_dtor(struct xradio_common *hw_priv,
		     struct sk_buff *skb,
		     const struct xradio_txpriv *txpriv)
{
	struct xradio_vif *priv =
		__xrwl_hwpriv_to_vifpriv(hw_priv, txpriv->if_id);
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	skb_pull(skb, txpriv->offset);
	if (priv && txpriv->rate_id != XRADIO_INVALID_RATE_ID) {
		xradio_notify_buffered_tx(priv, skb,
				txpriv->raw_link_id, txpriv->tid);
		tx_policy_put(hw_priv, txpriv->rate_id);
	}
	if (likely(!xradio_is_itp(hw_priv))) {
		down(&hw_priv->dtor_lock);
		mac80211_tx_status(hw_priv->hw, skb);
		up(&hw_priv->dtor_lock);
	}
}
#ifdef CONFIG_XRADIO_TESTMODE
/* TODO It should be removed before official delivery */
static void frame_hexdump(char *prefix, u8 *data, int len)
{
	int i;

	txrx_printk(XRADIO_DBG_MSG, "%s hexdump:\n", prefix);
	for (i = 0; i < len; i++) {
		if (i + 10 < len) {
			txrx_printk(XRADIO_DBG_MSG, "%.1X %.1X %.1X %.1X" \
				"%.1X %.1X %.1X %.1X %.1X %.1X",
				data[i], data[i+1], data[i+2],
				data[i+3], data[i+4], data[i+5],
				data[i+6], data[i+7], data[i+8],
				data[i+9]);
			i += 9;
		} else {
			txrx_printk(XRADIO_DBG_MSG, "%.1X ", data[i]);
		}
	}
}
/**
 * c1200_tunnel_send_testmode_data - Send test frame to the driver
 *
 * @priv: pointer to xradio private structure
 * @skb: skb with frame
 *
 * Returns: 0 on success or non zero value on failure
 */
static int xradio_tunnel_send_testmode_data(struct xradio_common *hw_priv,
					    struct sk_buff *skb)
{
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);
	if (xradio_tesmode_event(hw_priv->hw->wiphy, XR_MSG_EVENT_FRAME_DATA,
				 skb->data, skb->len, GFP_ATOMIC))
		return -EINVAL;

	return 0;
}

/**
 * xradio_frame_test_detection - Detection frame_test
 *
 * @priv: pointer to xradio vif structure
 * @frame: ieee80211 header
 * @skb: skb with frame
 *
 * Returns: 1 - frame test detected, 0 - not detected
 */
static int xradio_frame_test_detection(struct xradio_vif *priv,
				       struct ieee80211_hdr *frame,
				       struct sk_buff *skb)
{
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	int hdrlen = ieee80211_hdrlen(frame->frame_control);
	int detected = 0;
	int ret;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (hdrlen + hw_priv->test_frame.len <= skb->len &&
	    memcmp(skb->data + hdrlen, hw_priv->test_frame.data,
		   hw_priv->test_frame.len) == 0) {
		detected = 1;
		txrx_printk(XRADIO_DBG_MSG, "TEST FRAME detected");
		frame_hexdump("TEST FRAME original:", skb->data, skb->len);
		ret = ieee80211_data_to_8023(skb, hw_priv->mac_addr,
				priv->mode);
		if (!ret) {
			frame_hexdump("FRAME 802.3:", skb->data, skb->len);
			ret = xradio_tunnel_send_testmode_data(hw_priv, skb);
		}
		if (ret)
			txrx_printk(XRADIO_DBG_ERROR, "Send TESTFRAME failed(%d)", ret);
	}
	return detected;
}
#endif /* CONFIG_XRADIO_TESTMODE */


static void
xradio_rx_h_ba_stat(struct xradio_vif *priv,
		    size_t hdrlen, size_t skb_len)
{
	struct xradio_common *hw_priv = priv->hw_priv;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (priv->join_status != XRADIO_JOIN_STATUS_STA)
		return;
	if (!xradio_is_ht(&hw_priv->ht_info))
		return;
	if (!priv->setbssparams_done)
		return;

	spin_lock_bh(&hw_priv->ba_lock);
	hw_priv->ba_acc_rx += skb_len - hdrlen;
	if (!(hw_priv->ba_cnt_rx || hw_priv->ba_cnt)) {
		mod_timer(&hw_priv->ba_timer,
			jiffies + XRADIO_BLOCK_ACK_INTERVAL);
	}
	hw_priv->ba_cnt_rx++;
	spin_unlock_bh(&hw_priv->ba_lock);
}

#if 0
u8 nettest_bssid[] = {0x00, 0x02, 0x03, 0x04, 0x05, 0x06};
u8 save_rate_ie;
#endif

void xradio_rx_cb(struct xradio_vif *priv,
		  struct wsm_rx *arg,
		  struct sk_buff **skb_p)
{
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	struct sk_buff *skb = *skb_p;
	struct ieee80211_rx_status *hdr = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_hdr *frame = (struct ieee80211_hdr *)skb->data;
#if defined(CONFIG_XRADIO_USE_EXTENSIONS)
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *)skb->data;
#endif
	struct xradio_link_entry *entry = NULL;
	unsigned long grace_period;
	bool early_data = false;
	size_t hdrlen = 0;
	u8   parse_iv_len = 0;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	hdr->flag = 0;

	if (unlikely(priv->mode == NL80211_IFTYPE_UNSPECIFIED)) {
		/* STA is stopped. */
		goto drop;
	}

#ifdef TES_P2P_0002_ROC_RESTART
	xradio_frame_monitor(hw_priv, skb, false);
#endif

#if defined(CONFIG_XRADIO_USE_EXTENSIONS)
	if ((ieee80211_is_action(frame->frame_control))
	    && (mgmt->u.action.category == WLAN_CATEGORY_PUBLIC)) {
		u8 *action = (u8 *)&mgmt->u.action.category;
		xradio_check_go_neg_conf_success(hw_priv, action);
	}
#endif

#ifdef CONFIG_XRADIO_TESTMODE
	spin_lock_bh(&hw_priv->tsm_lock);
	if (hw_priv->start_stop_tsm.start) {
		unsigned queue_id = skb_get_queue_mapping(skb);
		if (queue_id == 0) {
			struct timeval tmval;
			xr_do_gettimeofday(&tmval);
			if (hw_priv->tsm_info.sta_roamed &&
			    hw_priv->tsm_info.use_rx_roaming) {
				hw_priv->tsm_info.roam_delay = tmval.tv_usec -
					hw_priv->tsm_info.rx_timestamp_vo;
				txrx_printk(XRADIO_DBG_NIY, "[RX] RxInd Roaming:"
				"roam_delay = %u\n", hw_priv->tsm_info.roam_delay);
				hw_priv->tsm_info.sta_roamed = 0;
			}
			hw_priv->tsm_info.rx_timestamp_vo = tmval.tv_usec;
		}
	}
	spin_unlock_bh(&hw_priv->tsm_lock);
#endif /*CONFIG_XRADIO_TESTMODE*/
	if (arg->link_id && (arg->link_id != XRADIO_LINK_ID_UNMAPPED)
			&& (arg->link_id <= XRADIO_MAX_STA_IN_AP_MODE)) {
		entry =	&priv->link_id_db[arg->link_id - 1];
		if (entry->status == XRADIO_LINK_SOFT &&
				ieee80211_is_data(frame->frame_control))
			early_data = true;
		entry->timestamp = jiffies;
	}
#if defined(CONFIG_XRADIO_USE_EXTENSIONS)
	else if ((arg->link_id == XRADIO_LINK_ID_UNMAPPED)
			&& (priv->vif->p2p == WSM_START_MODE_P2P_GO)
			&& ieee80211_is_action(frame->frame_control)
			&& (mgmt->u.action.category == WLAN_CATEGORY_PUBLIC)) {
		txrx_printk(XRADIO_DBG_NIY, "[RX] Going to MAP&RESET link ID\n");

		if (work_pending(&priv->linkid_reset_work))
			SYS_WARN(1);

		memcpy(&priv->action_frame_sa[0],
				ieee80211_get_SA(frame), ETH_ALEN);
		priv->action_linkid = 0;
		schedule_work(&priv->linkid_reset_work);
	}

	if (arg->link_id && (arg->link_id != XRADIO_LINK_ID_UNMAPPED)
			&& (priv->vif->p2p == WSM_START_MODE_P2P_GO)
			&& ieee80211_is_action(frame->frame_control)
			&& (mgmt->u.action.category == WLAN_CATEGORY_PUBLIC)) {
		/* Link ID already exists for the ACTION frame.
		 * Reset and Remap */
		if (work_pending(&priv->linkid_reset_work))
			SYS_WARN(1);
		memcpy(&priv->action_frame_sa[0],
				ieee80211_get_SA(frame), ETH_ALEN);
		priv->action_linkid = arg->link_id;
		schedule_work(&priv->linkid_reset_work);
	}
#endif
	if (unlikely(arg->status)) {
		if (arg->status == WSM_STATUS_MICFAILURE) {
			txrx_printk(XRADIO_DBG_WARN, "[RX] IF=%d, MIC failure.\n",
				    priv->if_id);
			hdr->flag |= RX_FLAG_MMIC_ERROR;
		} else if (arg->status == WSM_STATUS_NO_KEY_FOUND) {
#ifdef MONITOR_MODE
			if (hw_priv->monitor_if_id == -1) {
#endif
				txrx_printk(XRADIO_DBG_WARN, "[RX] IF=%d, No key found.\n",
				    priv->if_id);
				goto drop;
#ifdef MONITOR_MODE
			}
#endif
		} else {
			txrx_printk(XRADIO_DBG_WARN, "[RX] IF=%d, Receive failure: %d.\n",
				priv->if_id, arg->status);
			goto drop;
		}
	}

	if (skb->len < sizeof(struct ieee80211_pspoll)) {
		txrx_printk(XRADIO_DBG_WARN, "Mailformed SDU rx'ed. "
			    "Size is lesser than IEEE header.\n");
		goto drop;
	}

	if (unlikely(ieee80211_is_pspoll(frame->frame_control)))
		if (xradio_handle_pspoll(priv, skb))
			goto drop;

	hdr->mactime = 0; /* Not supported by WSM */
	hdr->band = (arg->channelNumber > 14) ?
			NL80211_BAND_5GHZ : NL80211_BAND_2GHZ;
	hdr->freq = ieee80211_channel_to_frequency(
			arg->channelNumber,
			hdr->band);

#ifdef AP_HT_COMPAT_FIX
	if (!priv->ht_compat_det && priv->htcap &&
		ieee80211_is_data_qos(frame->frame_control)) {
		if (xradio_apcompat_detect(priv, arg->rxedRate))
			goto drop;
	}
#endif

	if (arg->rxedRate < 24)
		RxedRateIdx_Map[arg->rxedRate]++;
	else
		SYS_WARN(1);

	if (arg->rxedRate >= 14) {
		hdr->flag |= RX_FLAG_HT;
		hdr->rate_idx = arg->rxedRate - 14;
	} else if (arg->rxedRate >= 4) {
		if (hdr->band == NL80211_BAND_5GHZ)
			hdr->rate_idx = arg->rxedRate - 6;
		else
			hdr->rate_idx = arg->rxedRate - 2;
	} else {
		hdr->rate_idx = arg->rxedRate;
	}

	hdr->signal = (s8)arg->rcpiRssi;
	hdr->antenna = 0;

	hdrlen = ieee80211_hdrlen(frame->frame_control);

	if (WSM_RX_STATUS_ENCRYPTION(arg->flags)) {
		size_t iv_len = 0, icv_len = 0;

		hdr->flag |= RX_FLAG_DECRYPTED;

		/* Oops... There is no fast way to ask mac80211 about
		 * IV/ICV lengths. Even defineas are not exposed.*/
		switch (WSM_RX_STATUS_ENCRYPTION(arg->flags)) {
		case WSM_RX_STATUS_WEP:
			iv_len = 4 /* WEP_IV_LEN */;
			icv_len = 4 /* WEP_ICV_LEN */;
			break;
		case WSM_RX_STATUS_TKIP:
			iv_len = 8 /* TKIP_IV_LEN */;
			icv_len = 4 /* TKIP_ICV_LEN */
				+ 8 /*MICHAEL_MIC_LEN*/;
			break;
		case WSM_RX_STATUS_AES:
			iv_len = 8 /* CCMP_HDR_LEN */;
			icv_len = 8 /* CCMP_MIC_LEN */;
			break;
		case WSM_RX_STATUS_WAPI:
			iv_len = 18 /* WAPI_HDR_LEN */;
			icv_len = 16 /* WAPI_MIC_LEN */;
			hdr->flag |= RX_FLAG_IV_STRIPPED;
			break;
		default:
			SYS_WARN("Unknown encryption type");
			goto drop;
		}

		/* Firmware strips ICV in case of MIC failure. */
		if (arg->status == WSM_STATUS_MICFAILURE) {
			icv_len = 0;
			hdr->flag |= RX_FLAG_IV_STRIPPED;
		}

		if (skb->len < hdrlen + iv_len + icv_len) {
			txrx_printk(XRADIO_DBG_WARN, "Mailformed SDU rx'ed. "
				"Size is lesser than crypto headers.\n");
			goto drop;
		}

		if (WSM_RX_STATUS_ENCRYPTION(arg->flags) ==
		    WSM_RX_STATUS_TKIP) {
			/* Remove TKIP MIC 8 bytes*/
			memmove(skb->data + skb->len-icv_len,
				skb->data + skb->len-icv_len+8, 4);
			skb_trim(skb, skb->len - 8);
			hdr->flag |= RX_FLAG_MMIC_STRIPPED;
		} else if (unlikely(WSM_RX_STATUS_ENCRYPTION(arg->flags) ==
			   WSM_RX_STATUS_WAPI)) {
			/* Protocols not defined in mac80211 should be
			   stripped/crypted in driver/firmware */
			/* Remove IV, ICV and MIC */
			skb_trim(skb, skb->len - icv_len);
			memmove(skb->data + iv_len, skb->data, hdrlen);
			skb_pull(skb, iv_len);
		}
		parse_iv_len = iv_len;
	}

	xradio_debug_rxed(priv);
	if (arg->flags & WSM_RX_STATUS_AGGREGATE)
		xradio_debug_rxed_agg(priv);

#if 0
	/*for nettest*/
	if (ieee80211_is_probe_resp(frame->frame_control) &&
		!arg->status &&
		!memcmp(ieee80211_get_SA(frame), nettest_bssid, ETH_ALEN)) {
		const u8 *supp_rate_ie;
		u8 *ies = ((struct ieee80211_mgmt *)
			   (skb->data))->u.probe_resp.variable;
		size_t ies_len = skb->len - (ies - (u8 *)(skb->data));

		supp_rate_ie = xradio_get_ie(ies, ies_len, WLAN_EID_SUPP_RATES);
		save_rate_ie = supp_rate_ie[2];
		txrx_printk(XRADIO_DBG_WARN, "[netest]: save_rate_ie=%2x\n",
			    save_rate_ie);
	}

	if (ieee80211_is_assoc_resp(frame->frame_control) &&
		!arg->status &&
		!memcmp(ieee80211_get_SA(frame), nettest_bssid, ETH_ALEN)) {
		u8 *supp_rate_ie2;
		size_t ies_len;
		u8 *ies = ((struct ieee80211_mgmt *)
			   (skb->data))->u.assoc_resp.variable;
		ies_len = skb->len - (ies - (u8 *)(skb->data));
		supp_rate_ie2 = xradio_get_ie(ies, ies_len, WLAN_EID_SUPP_RATES);

		if ((supp_rate_ie2[1] == 1) && (supp_rate_ie2[2] == 0x80)) {
			supp_rate_ie2[2] = save_rate_ie;
			txrx_printk(XRADIO_DBG_WARN,
				    "[netest]: rate_ie modified=%2x\n",
				    supp_rate_ie2[2]);
		}
	}
	/*for test*/
#endif

	if (ieee80211_is_beacon(frame->frame_control) &&
		!arg->status &&
		!memcmp(ieee80211_get_SA(frame), priv->join_bssid, ETH_ALEN)) {
		const u8 *tim_ie;
		u8 *ies;
		size_t ies_len;
		priv->disable_beacon_filter = false;
		queue_work(hw_priv->workqueue, &priv->update_filtering_work);
		ies = ((struct ieee80211_mgmt *)
			  (skb->data))->u.beacon.variable;
		ies_len = skb->len - (ies - (u8 *)(skb->data));

		tim_ie = xradio_get_ie(ies, ies_len, WLAN_EID_TIM);
		if (tim_ie) {
			struct ieee80211_tim_ie *tim =
				(struct ieee80211_tim_ie *)&tim_ie[2];

			if (priv->join_dtim_period != tim->dtim_period) {
				priv->join_dtim_period = tim->dtim_period;
				queue_work(hw_priv->workqueue,
					&priv->set_beacon_wakeup_period_work);
			}
		}
		if (unlikely(priv->disable_beacon_filter)) {
			priv->disable_beacon_filter = false;
			queue_work(hw_priv->workqueue,
				&priv->update_filtering_work);
		}
	}
#ifdef AP_HT_CAP_UPDATE
    if (priv->mode == NL80211_IFTYPE_AP           &&
	ieee80211_is_beacon(frame->frame_control) &&
	((priv->ht_info&HT_INFO_MASK) != 0x0011)  &&
	!arg->status) {
	u8 *ies;
	size_t ies_len;
	const u8 *ht_cap;
	ies = ((struct ieee80211_mgmt *)(skb->data))->u.beacon.variable;
	ies_len = skb->len - (ies - (u8 *)(skb->data));
	ht_cap = xradio_get_ie(ies, ies_len, WLAN_EID_HT_CAPABILITY);
	if (!ht_cap) {
	    priv->ht_info |= 0x0011;
	    queue_work(hw_priv->workqueue, &priv->ht_info_update_work);
	}
    }
#endif

#ifdef AP_HT_COMPAT_FIX
	if (ieee80211_is_mgmt(frame->frame_control) &&
		priv->if_id == 0 && !(priv->ht_compat_det & 0x10)) {
		xradio_remove_ht_ie(priv, skb);
	}
#endif

#ifdef ROAM_OFFLOAD
	if ((ieee80211_is_beacon(frame->frame_control) ||
		 ieee80211_is_probe_resp(frame->frame_control)) &&
			!arg->status) {
		if (hw_priv->auto_scanning &&
			!atomic_read(&hw_priv->scan.in_progress))
			hw_priv->frame_rcvd = 1;

		if (!memcmp(ieee80211_get_SA(frame), priv->join_bssid, ETH_ALEN)) {
			if (hw_priv->beacon)
				dev_kfree_skb(hw_priv->beacon);
			hw_priv->beacon = skb_copy(skb, GFP_ATOMIC);
			if (!hw_priv->beacon)
				txrx_printk(XRADIO_DBG_ERROR,
					    "sched_scan: own beacon storing failed\n");
		}
	}
#endif /*ROAM_OFFLOAD*/

	/*scanResult.timestamp to adapt to Framework(WiFi) on Android5.0 or advanced version.*/
	if ((ieee80211_is_beacon(mgmt->frame_control) ||
		ieee80211_is_probe_resp(mgmt->frame_control))
		&& !arg->status) {
		struct timespec ts;
		uint64_t tmp;
		xr_get_monotonic_boottime(&ts);
		tmp = ts.tv_nsec;
		do_div(tmp, 1000);
		ts.tv_nsec = (uint32_t)tmp;
		if (ieee80211_is_beacon(mgmt->frame_control)) {
			mgmt->u.beacon.timestamp = ((u64)ts.tv_sec * 1000000 + ts.tv_nsec);
		} else if (ieee80211_is_probe_resp(mgmt->frame_control)) {
			mgmt->u.probe_resp.timestamp = ((u64)ts.tv_sec * 1000000 + ts.tv_nsec);
		}
	}

	/* don't delay scan before next connect */
	if (ieee80211_is_deauth(frame->frame_control) ||
	    ieee80211_is_disassoc(frame->frame_control))
		hw_priv->scan_delay_status[priv->if_id] = XRADIO_SCAN_ALLOW;

	/* Stay awake for 1sec. after frame is received to give
	 * userspace chance to react and acquire appropriate
	 * wakelock. */
	if (ieee80211_is_auth(frame->frame_control))
		grace_period = 5 * HZ;
	else if (ieee80211_is_deauth(frame->frame_control))
		grace_period = 5 * HZ;
	else
		grace_period = HZ;

	if (ieee80211_is_data(frame->frame_control))
		xradio_rx_h_ba_stat(priv, hdrlen, skb->len);
#ifdef CONFIG_PM
	xradio_pm_stay_awake(&hw_priv->pm_state, grace_period);
#endif
#ifdef CONFIG_XRADIO_TESTMODE
	if (hw_priv->test_frame.len > 0 &&
		priv->mode == NL80211_IFTYPE_STATION) {
		if (xradio_frame_test_detection(priv, frame, skb) == 1) {
			consume_skb(skb);
			*skb_p = NULL;
			return;
		}
	}
#endif /* CONFIG_XRADIO_TESTMODE */


#ifdef CONFIG_XRADIO_DEBUGFS
	/* parsse frame here for debug. */
	if (rxparse_flags)
		xradio_parse_frame(skb, parse_iv_len,
				   rxparse_flags|PF_RX, priv->if_id);
#endif

#ifdef AP_ARP_COMPAT_FIX
	if ((priv->join_status == XRADIO_JOIN_STATUS_STA) &&
		(priv->arp_compat_cnt >= 1)) {
		u16 fctl = frame->frame_control;
		if (ieee80211_is_data(fctl)) {
			u8 machdrlen = ieee80211_hdrlen(fctl);
			u8 *llc_data = (u8 *)frame + machdrlen + parse_iv_len;
			if (is_SNAP(llc_data) && is_arp(llc_data)) {
				u8 *arp_hdr = llc_data + LLC_LEN;
				u16 *arp_type = (u16 *)(arp_hdr + ARP_TYPE_OFFSET);
				if (*arp_type == cpu_to_be16(ARP_RESPONSE))
					priv->arp_compat_cnt = 0;
			}
		}
	}
#endif

	/* Some aps change channel to inform station by sending beacon with WLAN_EID_DS_PARAMS ie,
	 *then station needs to reconnect to ap.
	*/
	if (ieee80211_is_beacon(frame->frame_control) &&
		!arg->status &&
		(priv->join_status == XRADIO_JOIN_STATUS_STA) &&
		!memcmp(ieee80211_get_SA(frame), priv->join_bssid, ETH_ALEN)) {
		const u8 *ds_ie;
		u8 *ies;
		size_t ies_len;
		int ds_ie_partms_chan = 0;
		ies = ((struct ieee80211_mgmt *)
			  (skb->data))->u.beacon.variable;
		ies_len = skb->len - (ies - (u8 *)(skb->data));

		ds_ie = xradio_get_ie(ies, ies_len, WLAN_EID_DS_PARAMS);
		if (ds_ie && (ds_ie[1] == 1)) {
			ds_ie_partms_chan = ds_ie[2];
			if (ds_ie_partms_chan != hw_priv->join_chan) {
				txrx_printk(XRADIO_DBG_WARN, "***ap changes channel by beacon with ds ie,"
					"then station reconnects to ap, %d -> %d\n",
					hw_priv->join_chan, ds_ie_partms_chan);
				wsm_send_disassoc_to_self(hw_priv, priv);
			}
		}
	}

	if (xradio_realloc_resv_skb(hw_priv, *skb_p)) {
		*skb_p = NULL;
		return;
	}

	/* Try to  a packet for the case dev_alloc_skb failed in bh.*/
	if (unlikely(xradio_itp_rxed(hw_priv, skb)))
		consume_skb(skb);
	else if (unlikely(early_data)) {
		spin_lock_bh(&priv->ps_state_lock);
		/* Double-check status with lock held */
		if (entry->status == XRADIO_LINK_SOFT) {
			skb_queue_tail(&entry->rx_queue, skb);
			txrx_printk(XRADIO_DBG_WARN, "***skb_queue_tail\n");
		} else
			mac80211_rx_irqsafe(priv->hw, skb);
		spin_unlock_bh(&priv->ps_state_lock);
	} else {
		mac80211_rx_irqsafe(priv->hw, skb);
	}
	*skb_p = NULL;

	return;

drop:
	/* TODO: update failure counters */
	return;
}

/* ******************************************************************** */
/* Security								*/

int xradio_alloc_key(struct xradio_common *hw_priv)
{
	int idx;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	idx = ffs(~hw_priv->key_map) - 1;
	if (idx < 0 || idx > WSM_KEY_MAX_INDEX)
		return -1;

	hw_priv->key_map |= BIT(idx);
	hw_priv->keys[idx].entryIndex = idx;
	txrx_printk(XRADIO_DBG_NIY, "%s, idx=%d\n", __func__, idx);
	return idx;
}

void xradio_free_key(struct xradio_common *hw_priv, int idx)
{
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	SYS_BUG(!(hw_priv->key_map & BIT(idx)));
	memset(&hw_priv->keys[idx], 0, sizeof(hw_priv->keys[idx]));
	hw_priv->key_map &= ~BIT(idx);
	txrx_printk(XRADIO_DBG_NIY, "%s, idx=%d\n", __func__, idx);
}

void xradio_free_keys(struct xradio_common *hw_priv)
{
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	memset(&hw_priv->keys, 0, sizeof(hw_priv->keys));
	hw_priv->key_map = 0;
}

int xradio_upload_keys(struct xradio_vif *priv)
{
	struct xradio_common *hw_priv = xrwl_vifpriv_to_hwpriv(priv);
	int idx, ret = 0;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	for (idx = 0; idx <= WSM_KEY_MAX_IDX; ++idx)
		if (hw_priv->key_map & BIT(idx)) {
			ret = wsm_add_key(hw_priv, &hw_priv->keys[idx], priv->if_id);
			if (ret < 0)
				break;
		}
	return ret;
}
#if defined(CONFIG_XRADIO_USE_EXTENSIONS)
/* Workaround for WFD test case 6.1.10 */
void xradio_link_id_reset(struct work_struct *work)
{
	struct xradio_vif *priv =
		container_of(work, struct xradio_vif, linkid_reset_work);
	struct xradio_common *hw_priv = priv->hw_priv;
	int temp_linkid;
	txrx_printk(XRADIO_DBG_TRC, "%s\n", __func__);

	if (!priv->action_linkid) {
		/* In GO mode we can receive ACTION frames without a linkID */
		temp_linkid = xradio_alloc_link_id(priv,
				&priv->action_frame_sa[0]);
		SYS_WARN(!temp_linkid);
		if (temp_linkid) {
			/* Make sure we execute the WQ */
			flush_workqueue(hw_priv->workqueue);
			/* Release the link ID */
			spin_lock_bh(&priv->ps_state_lock);
			priv->link_id_db[temp_linkid - 1].prev_status =
				priv->link_id_db[temp_linkid - 1].status;
			priv->link_id_db[temp_linkid - 1].status =
				XRADIO_LINK_RESET;
			spin_unlock_bh(&priv->ps_state_lock);
			wsm_lock_tx_async(hw_priv);
			if (queue_work(hw_priv->workqueue,
				       &priv->link_id_work) <= 0)
				wsm_unlock_tx(hw_priv);
		}
	} else {
		spin_lock_bh(&priv->ps_state_lock);
		priv->link_id_db[priv->action_linkid - 1].prev_status =
			priv->link_id_db[priv->action_linkid - 1].status;
		priv->link_id_db[priv->action_linkid - 1].status =
			XRADIO_LINK_RESET_REMAP;
		spin_unlock_bh(&priv->ps_state_lock);
		wsm_lock_tx_async(hw_priv);
		if (queue_work(hw_priv->workqueue, &priv->link_id_work) <= 0)
				wsm_unlock_tx(hw_priv);
		flush_workqueue(hw_priv->workqueue);
	}
}
#endif
