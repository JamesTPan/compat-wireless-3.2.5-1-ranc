/*
 * Copyright 2002-2005, Instant802 Networks, Inc.
 * Copyright 2005-2006, Devicescape Software, Inc.
 * Copyright 2006-2007	Jiri Benc <jbenc@suse.cz>
 * Copyright 2007-2010	Johannes Berg <johannes@sipsolutions.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/jiffies.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/rcupdate.h>
#include <linux/export.h>
#include <net/mac80211.h>
#include <net/ieee80211_radiotap.h>

#include "ieee80211_i.h"
#include "driver-ops.h"
#include "led.h"
#include "mesh.h"
#include "wep.h"
#include "wpa.h"
#include "tkip.h"
#include "wme.h"

// Network coding 
#include "queueing.h"

// queue list
struct queue_ele *rxList = NULL;

// batch list
struct matrix_ele *hList = NULL;
struct matrix_ele *hLast = NULL;
spinlock_t hList_lock;

// workqueue for decoding 
struct workqueue_struct *decode_wq;
struct delayed_work decode_w;
bool wq_terminate = false;

// double coding 
struct batch_schedule dc_schedule;
struct spinlock dcs_lock;

static inline void remove_queue_ele(unsigned long ele)
{
	struct queue_ele *pt = (struct queue_ele *) ele;

	spin_lock(&dcs_lock);
	if(pt->addr.prev != NULL && pt->addr.next != NULL){
		pt->addr.prev->next = pt->addr.next;
		pt->addr.next->prev = pt->addr.prev;
		pt->addr.prev = NULL;
		pt->addr.next = NULL;
	}
	spin_unlock(&dcs_lock);

	remove_queue(&rxList, pt);
}

/*
 * monitor mode reception
 *
 * This function cleans up the SKB, i.e. it removes all the stuff
 * only useful for monitoring.
 */
static struct sk_buff *remove_monitor_info(struct ieee80211_local *local,
					   struct sk_buff *skb)
{
	if (local->hw.flags & IEEE80211_HW_RX_INCLUDES_FCS) {
		if (likely(skb->len > FCS_LEN))
			__pskb_trim(skb, skb->len - FCS_LEN);
		else {
			/* driver bug */
			WARN_ON(1);
			dev_kfree_skb(skb);
			skb = NULL;
		}
	}

	return skb;
}

static inline int should_drop_frame(struct sk_buff *skb,
				    int present_fcs_len)
{
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

	if (status->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_FAILED_PLCP_CRC))
		return 1;
	if (unlikely(skb->len < 16 + present_fcs_len))
		return 1;
	if (ieee80211_is_ctl(hdr->frame_control) &&
	    !ieee80211_is_pspoll(hdr->frame_control) &&
	    !ieee80211_is_back_req(hdr->frame_control))
		return 1;
	return 0;
}

static int
ieee80211_rx_radiotap_len(struct ieee80211_local *local,
			  struct ieee80211_rx_status *status)
{
	int len;

	/* always present fields */
	len = sizeof(struct ieee80211_radiotap_header) + 9;

	if (status->flag & RX_FLAG_MACTIME_MPDU)
		len += 8;
	if (local->hw.flags & IEEE80211_HW_SIGNAL_DBM)
		len += 1;

	if (len & 1) /* padding for RX_FLAGS if necessary */
		len++;

	if (status->flag & RX_FLAG_HT) /* HT info */
		len += 3;

	return len;
}

/*
 * ieee80211_add_rx_radiotap_header - add radiotap header
 *
 * add a radiotap header containing all the fields which the hardware provided.
 */
static void
ieee80211_add_rx_radiotap_header(struct ieee80211_local *local,
				 struct sk_buff *skb,
				 struct ieee80211_rate *rate,
				 int rtap_len)
{
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_radiotap_header *rthdr;
	unsigned char *pos;
	u16 rx_flags = 0;

	rthdr = (struct ieee80211_radiotap_header *)skb_push(skb, rtap_len);
	memset(rthdr, 0, rtap_len);

	/* radiotap header, set always present flags */
	rthdr->it_present =
		cpu_to_le32((1 << IEEE80211_RADIOTAP_FLAGS) |
			    (1 << IEEE80211_RADIOTAP_CHANNEL) |
			    (1 << IEEE80211_RADIOTAP_ANTENNA) |
			    (1 << IEEE80211_RADIOTAP_RX_FLAGS));
	rthdr->it_len = cpu_to_le16(rtap_len);

	pos = (unsigned char *)(rthdr+1);

	/* the order of the following fields is important */

	/* IEEE80211_RADIOTAP_TSFT */
	if (status->flag & RX_FLAG_MACTIME_MPDU) {
		put_unaligned_le64(status->mactime, pos);
		rthdr->it_present |=
			cpu_to_le32(1 << IEEE80211_RADIOTAP_TSFT);
		pos += 8;
	}

	/* IEEE80211_RADIOTAP_FLAGS */
	if (local->hw.flags & IEEE80211_HW_RX_INCLUDES_FCS)
		*pos |= IEEE80211_RADIOTAP_F_FCS;
	if (status->flag & (RX_FLAG_FAILED_FCS_CRC | RX_FLAG_FAILED_PLCP_CRC))
		*pos |= IEEE80211_RADIOTAP_F_BADFCS;
	if (status->flag & RX_FLAG_SHORTPRE)
		*pos |= IEEE80211_RADIOTAP_F_SHORTPRE;
	pos++;

	/* IEEE80211_RADIOTAP_RATE */
	if (!rate || status->flag & RX_FLAG_HT) {
		/*
		 * Without rate information don't add it. If we have,
		 * MCS information is a separate field in radiotap,
		 * added below. The byte here is needed as padding
		 * for the channel though, so initialise it to 0.
		 */
		*pos = 0;
	} else {
		rthdr->it_present |= cpu_to_le32(1 << IEEE80211_RADIOTAP_RATE);
		*pos = rate->bitrate / 5;
	}
	pos++;

	/* IEEE80211_RADIOTAP_CHANNEL */
	put_unaligned_le16(status->freq, pos);
	pos += 2;
	if (status->band == IEEE80211_BAND_5GHZ)
		put_unaligned_le16(IEEE80211_CHAN_OFDM | IEEE80211_CHAN_5GHZ,
				   pos);
	else if (status->flag & RX_FLAG_HT)
		put_unaligned_le16(IEEE80211_CHAN_DYN | IEEE80211_CHAN_2GHZ,
				   pos);
	else if (rate && rate->flags & IEEE80211_RATE_ERP_G)
		put_unaligned_le16(IEEE80211_CHAN_OFDM | IEEE80211_CHAN_2GHZ,
				   pos);
	else if (rate)
		put_unaligned_le16(IEEE80211_CHAN_CCK | IEEE80211_CHAN_2GHZ,
				   pos);
	else
		put_unaligned_le16(IEEE80211_CHAN_2GHZ, pos);
	pos += 2;

	/* IEEE80211_RADIOTAP_DBM_ANTSIGNAL */
	if (local->hw.flags & IEEE80211_HW_SIGNAL_DBM) {
		*pos = status->signal;
		rthdr->it_present |=
			cpu_to_le32(1 << IEEE80211_RADIOTAP_DBM_ANTSIGNAL);
		pos++;
	}

	/* IEEE80211_RADIOTAP_LOCK_QUALITY is missing */

	/* IEEE80211_RADIOTAP_ANTENNA */
	*pos = status->antenna;
	pos++;

	/* IEEE80211_RADIOTAP_DB_ANTNOISE is not used */

	/* IEEE80211_RADIOTAP_RX_FLAGS */
	/* ensure 2 byte alignment for the 2 byte field as required */
	if ((pos - (u8 *)rthdr) & 1)
		pos++;
	if (status->flag & RX_FLAG_FAILED_PLCP_CRC)
		rx_flags |= IEEE80211_RADIOTAP_F_RX_BADPLCP;
	put_unaligned_le16(rx_flags, pos);
	pos += 2;

	if (status->flag & RX_FLAG_HT) {
		rthdr->it_present |= cpu_to_le32(1 << IEEE80211_RADIOTAP_MCS);
		*pos++ = IEEE80211_RADIOTAP_MCS_HAVE_MCS |
			 IEEE80211_RADIOTAP_MCS_HAVE_GI |
			 IEEE80211_RADIOTAP_MCS_HAVE_BW;
		*pos = 0;
		if (status->flag & RX_FLAG_SHORT_GI)
			*pos |= IEEE80211_RADIOTAP_MCS_SGI;
		if (status->flag & RX_FLAG_40MHZ)
			*pos |= IEEE80211_RADIOTAP_MCS_BW_40;
		pos++;
		*pos++ = status->rate_idx;
	}
}

/*
 * This function copies a received frame to all monitor interfaces and
 * returns a cleaned-up SKB that no longer includes the FCS nor the
 * radiotap header the driver might have added.
 */
static struct sk_buff *
ieee80211_rx_monitor(struct ieee80211_local *local, struct sk_buff *origskb,
		     struct ieee80211_rate *rate)
{
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(origskb);
	struct ieee80211_sub_if_data *sdata;
	int needed_headroom = 0;
	struct sk_buff *skb, *skb2;
	struct net_device *prev_dev = NULL;
	int present_fcs_len = 0;

	/*
	 * First, we may need to make a copy of the skb because
	 *  (1) we need to modify it for radiotap (if not present), and
	 *  (2) the other RX handlers will modify the skb we got.
	 *
	 * We don't need to, of course, if we aren't going to return
	 * the SKB because it has a bad FCS/PLCP checksum.
	 */

	/* room for the radiotap header based on driver features */
	needed_headroom = ieee80211_rx_radiotap_len(local, status);

	if (local->hw.flags & IEEE80211_HW_RX_INCLUDES_FCS)
		present_fcs_len = FCS_LEN;

	/* make sure hdr->frame_control is on the linear part */
	if (!pskb_may_pull(origskb, 2)) {
		dev_kfree_skb(origskb);
		return NULL;
	}

	if (!local->monitors) {
		if (should_drop_frame(origskb, present_fcs_len)) {
			dev_kfree_skb(origskb);
			return NULL;
		}

		return remove_monitor_info(local, origskb);
	}

	if (should_drop_frame(origskb, present_fcs_len)) {
		/* only need to expand headroom if necessary */
		skb = origskb;
		origskb = NULL;

		/*
		 * This shouldn't trigger often because most devices have an
		 * RX header they pull before we get here, and that should
		 * be big enough for our radiotap information. We should
		 * probably export the length to drivers so that we can have
		 * them allocate enough headroom to start with.
		 */
		if (skb_headroom(skb) < needed_headroom &&
		    pskb_expand_head(skb, needed_headroom, 0, GFP_ATOMIC)) {
			dev_kfree_skb(skb);
			return NULL;
		}
	} else {
		/*
		 * Need to make a copy and possibly remove radiotap header
		 * and FCS from the original.
		 */
		skb = skb_copy_expand(origskb, needed_headroom, 0, GFP_ATOMIC);

		origskb = remove_monitor_info(local, origskb);

		if (!skb)
			return origskb;
	}

	/* prepend radiotap information */
	ieee80211_add_rx_radiotap_header(local, skb, rate, needed_headroom);

	skb_reset_mac_header(skb);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = htons(ETH_P_802_2);

	list_for_each_entry_rcu(sdata, &local->interfaces, list) {
		if (sdata->vif.type != NL80211_IFTYPE_MONITOR)
			continue;

		if (sdata->u.mntr_flags & MONITOR_FLAG_COOK_FRAMES)
			continue;

		if (!ieee80211_sdata_running(sdata))
			continue;

		if (prev_dev) {
			skb2 = skb_clone(skb, GFP_ATOMIC);
			if (skb2) {
				skb2->dev = prev_dev;
				netif_receive_skb(skb2);
			}
		}

		prev_dev = sdata->dev;
		sdata->dev->stats.rx_packets++;
		sdata->dev->stats.rx_bytes += skb->len;
	}

	if (prev_dev) {
		skb->dev = prev_dev;
		netif_receive_skb(skb);
	} else
		dev_kfree_skb(skb);

	return origskb;
}


static void ieee80211_parse_qos(struct ieee80211_rx_data *rx)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)rx->skb->data;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);
	int tid, seqno_idx, security_idx;

	/* does the frame have a qos control field? */
	if (ieee80211_is_data_qos(hdr->frame_control)) {
		u8 *qc = ieee80211_get_qos_ctl(hdr);
		/* frame has qos control */
		tid = *qc & IEEE80211_QOS_CTL_TID_MASK;
		if (*qc & IEEE80211_QOS_CTL_A_MSDU_PRESENT)
			status->rx_flags |= IEEE80211_RX_AMSDU;

		seqno_idx = tid;
		security_idx = tid;
	} else {
		/*
		 * IEEE 802.11-2007, 7.1.3.4.1 ("Sequence Number field"):
		 *
		 *	Sequence numbers for management frames, QoS data
		 *	frames with a broadcast/multicast address in the
		 *	Address 1 field, and all non-QoS data frames sent
		 *	by QoS STAs are assigned using an additional single
		 *	modulo-4096 counter, [...]
		 *
		 * We also use that counter for non-QoS STAs.
		 */
		seqno_idx = NUM_RX_DATA_QUEUES;
		security_idx = 0;
		if (ieee80211_is_mgmt(hdr->frame_control))
			security_idx = NUM_RX_DATA_QUEUES;
		tid = 0;
	}

	rx->seqno_idx = seqno_idx;
	rx->security_idx = security_idx;
	/* Set skb->priority to 1d tag if highest order bit of TID is not set.
	 * For now, set skb->priority to 0 for other cases. */
	rx->skb->priority = (tid > 7) ? 0 : tid;
}

/**
 * DOC: Packet alignment
 *
 * Drivers always need to pass packets that are aligned to two-byte boundaries
 * to the stack.
 *
 * Additionally, should, if possible, align the payload data in a way that
 * guarantees that the contained IP header is aligned to a four-byte
 * boundary. In the case of regular frames, this simply means aligning the
 * payload to a four-byte boundary (because either the IP header is directly
 * contained, or IV/RFC1042 headers that have a length divisible by four are
 * in front of it).  If the payload data is not properly aligned and the
 * architecture doesn't support efficient unaligned operations, mac80211
 * will align the data.
 *
 * With A-MSDU frames, however, the payload data address must yield two modulo
 * four because there are 14-byte 802.3 headers within the A-MSDU frames that
 * push the IP header further back to a multiple of four again. Thankfully, the
 * specs were sane enough this time around to require padding each A-MSDU
 * subframe to a length that is a multiple of four.
 *
 * Padding like Atheros hardware adds which is between the 802.11 header and
 * the payload is not supported, the driver is required to move the 802.11
 * header to be directly in front of the payload in that case.
 */
static void ieee80211_verify_alignment(struct ieee80211_rx_data *rx)
{
#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
	WARN_ONCE((unsigned long)rx->skb->data & 1,
		  "unaligned packet at 0x%p\n", rx->skb->data);
#endif
}


/* rx handlers */

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_passive_scan(struct ieee80211_rx_data *rx)
{
	struct ieee80211_local *local = rx->local;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);
	struct sk_buff *skb = rx->skb;

	if (likely(!(status->rx_flags & IEEE80211_RX_IN_SCAN) &&
		   !local->sched_scanning))
		return RX_CONTINUE;

	if (test_bit(SCAN_HW_SCANNING, &local->scanning) ||
	    local->sched_scanning)
		return ieee80211_scan_rx(rx->sdata, skb);

	if (test_bit(SCAN_SW_SCANNING, &local->scanning)) {
		/* drop all the other packets during a software scan anyway */
		if (ieee80211_scan_rx(rx->sdata, skb) != RX_QUEUED)
			dev_kfree_skb(skb);
		return RX_QUEUED;
	}

	/* scanning finished during invoking of handlers */
	I802_DEBUG_INC(local->rx_handlers_drop_passive_scan);
	return RX_DROP_UNUSABLE;
}


static int ieee80211_is_unicast_robust_mgmt_frame(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;

	if (skb->len < 24 || is_multicast_ether_addr(hdr->addr1))
		return 0;

	return ieee80211_is_robust_mgmt_frame(hdr);
}


static int ieee80211_is_multicast_robust_mgmt_frame(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;

	if (skb->len < 24 || !is_multicast_ether_addr(hdr->addr1))
		return 0;

	return ieee80211_is_robust_mgmt_frame(hdr);
}


/* Get the BIP key index from MMIE; return -1 if this is not a BIP frame */
static int ieee80211_get_mmie_keyidx(struct sk_buff *skb)
{
	struct ieee80211_mgmt *hdr = (struct ieee80211_mgmt *) skb->data;
	struct ieee80211_mmie *mmie;

	if (skb->len < 24 + sizeof(*mmie) ||
	    !is_multicast_ether_addr(hdr->da))
		return -1;

	if (!ieee80211_is_robust_mgmt_frame((struct ieee80211_hdr *) hdr))
		return -1; /* not a robust management frame */

	mmie = (struct ieee80211_mmie *)
		(skb->data + skb->len - sizeof(*mmie));
	if (mmie->element_id != WLAN_EID_MMIE ||
	    mmie->length != sizeof(*mmie) - 2)
		return -1;

	return le16_to_cpu(mmie->key_id);
}


static ieee80211_rx_result
ieee80211_rx_mesh_check(struct ieee80211_rx_data *rx)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)rx->skb->data;
	char *dev_addr = rx->sdata->vif.addr;

	if (ieee80211_is_data(hdr->frame_control)) {
		if (is_multicast_ether_addr(hdr->addr1)) {
			if (ieee80211_has_tods(hdr->frame_control) ||
				!ieee80211_has_fromds(hdr->frame_control))
				return RX_DROP_MONITOR;
			if (memcmp(hdr->addr3, dev_addr, ETH_ALEN) == 0)
				return RX_DROP_MONITOR;
		} else {
			if (!ieee80211_has_a4(hdr->frame_control))
				return RX_DROP_MONITOR;
			if (memcmp(hdr->addr4, dev_addr, ETH_ALEN) == 0)
				return RX_DROP_MONITOR;
		}
	}

	/* If there is not an established peer link and this is not a peer link
	 * establisment frame, beacon or probe, drop the frame.
	 */

	if (!rx->sta || sta_plink_state(rx->sta) != NL80211_PLINK_ESTAB) {
		struct ieee80211_mgmt *mgmt;

		if (!ieee80211_is_mgmt(hdr->frame_control))
			return RX_DROP_MONITOR;

		if (ieee80211_is_action(hdr->frame_control)) {
			u8 category;
			mgmt = (struct ieee80211_mgmt *)hdr;
			category = mgmt->u.action.category;
			if (category != WLAN_CATEGORY_MESH_ACTION &&
				category != WLAN_CATEGORY_SELF_PROTECTED)
				return RX_DROP_MONITOR;
			return RX_CONTINUE;
		}

		if (ieee80211_is_probe_req(hdr->frame_control) ||
		    ieee80211_is_probe_resp(hdr->frame_control) ||
		    ieee80211_is_beacon(hdr->frame_control) ||
		    ieee80211_is_auth(hdr->frame_control))
			return RX_CONTINUE;

		return RX_DROP_MONITOR;

	}

	return RX_CONTINUE;
}

#define SEQ_MODULO 0x1000
#define SEQ_MASK   0xfff

static inline int seq_less(u16 sq1, u16 sq2)
{
	return ((sq1 - sq2) & SEQ_MASK) > (SEQ_MODULO >> 1);
}

static inline u16 seq_inc(u16 sq)
{
	return (sq + 1) & SEQ_MASK;
}

static inline u16 seq_sub(u16 sq1, u16 sq2)
{
	return (sq1 - sq2) & SEQ_MASK;
}


static void ieee80211_release_reorder_frame(struct ieee80211_hw *hw,
					    struct tid_ampdu_rx *tid_agg_rx,
					    int index)
{
	struct ieee80211_local *local = hw_to_local(hw);
	struct sk_buff *skb = tid_agg_rx->reorder_buf[index];
	struct ieee80211_rx_status *status;

	lockdep_assert_held(&tid_agg_rx->reorder_lock);

	if (!skb)
		goto no_frame;

	/* release the frame from the reorder ring buffer */
	tid_agg_rx->stored_mpdu_num--;
	tid_agg_rx->reorder_buf[index] = NULL;
	status = IEEE80211_SKB_RXCB(skb);
	status->rx_flags |= IEEE80211_RX_DEFERRED_RELEASE;
	skb_queue_tail(&local->rx_skb_queue, skb);

no_frame:
	tid_agg_rx->head_seq_num = seq_inc(tid_agg_rx->head_seq_num);
}

static void ieee80211_release_reorder_frames(struct ieee80211_hw *hw,
					     struct tid_ampdu_rx *tid_agg_rx,
					     u16 head_seq_num)
{
	int index;

	lockdep_assert_held(&tid_agg_rx->reorder_lock);

	while (seq_less(tid_agg_rx->head_seq_num, head_seq_num)) {
		index = seq_sub(tid_agg_rx->head_seq_num, tid_agg_rx->ssn) %
							tid_agg_rx->buf_size;
		ieee80211_release_reorder_frame(hw, tid_agg_rx, index);
	}
}

/*
 * Timeout (in jiffies) for skb's that are waiting in the RX reorder buffer. If
 * the skb was added to the buffer longer than this time ago, the earlier
 * frames that have not yet been received are assumed to be lost and the skb
 * can be released for processing. This may also release other skb's from the
 * reorder buffer if there are no additional gaps between the frames.
 *
 * Callers must hold tid_agg_rx->reorder_lock.
 */
#define HT_RX_REORDER_BUF_TIMEOUT (HZ / 10)

static void ieee80211_sta_reorder_release(struct ieee80211_hw *hw,
					  struct tid_ampdu_rx *tid_agg_rx)
{
	int index, j;

	lockdep_assert_held(&tid_agg_rx->reorder_lock);

	/* release the buffer until next missing frame */
	index = seq_sub(tid_agg_rx->head_seq_num, tid_agg_rx->ssn) %
						tid_agg_rx->buf_size;
	if (!tid_agg_rx->reorder_buf[index] &&
	    tid_agg_rx->stored_mpdu_num > 1) {
		/*
		 * No buffers ready to be released, but check whether any
		 * frames in the reorder buffer have timed out.
		 */
		int skipped = 1;
		for (j = (index + 1) % tid_agg_rx->buf_size; j != index;
		     j = (j + 1) % tid_agg_rx->buf_size) {
			if (!tid_agg_rx->reorder_buf[j]) {
				skipped++;
				continue;
			}
			if (skipped &&
			    !time_after(jiffies, tid_agg_rx->reorder_time[j] +
					HT_RX_REORDER_BUF_TIMEOUT))
				goto set_release_timer;

#ifdef CONFIG_MAC80211_HT_DEBUG
			if (net_ratelimit())
				wiphy_debug(hw->wiphy,
					    "release an RX reorder frame due to timeout on earlier frames\n");
#endif
			ieee80211_release_reorder_frame(hw, tid_agg_rx, j);

			/*
			 * Increment the head seq# also for the skipped slots.
			 */
			tid_agg_rx->head_seq_num =
				(tid_agg_rx->head_seq_num + skipped) & SEQ_MASK;
			skipped = 0;
		}
	} else while (tid_agg_rx->reorder_buf[index]) {
		ieee80211_release_reorder_frame(hw, tid_agg_rx, index);
		index =	seq_sub(tid_agg_rx->head_seq_num, tid_agg_rx->ssn) %
							tid_agg_rx->buf_size;
	}

	if (tid_agg_rx->stored_mpdu_num) {
		j = index = seq_sub(tid_agg_rx->head_seq_num,
				    tid_agg_rx->ssn) % tid_agg_rx->buf_size;

		for (; j != (index - 1) % tid_agg_rx->buf_size;
		     j = (j + 1) % tid_agg_rx->buf_size) {
			if (tid_agg_rx->reorder_buf[j])
				break;
		}

 set_release_timer:

		mod_timer(&tid_agg_rx->reorder_timer,
			  tid_agg_rx->reorder_time[j] + 1 +
			  HT_RX_REORDER_BUF_TIMEOUT);
	} else {
		del_timer(&tid_agg_rx->reorder_timer);
	}
}

/*
 * As this function belongs to the RX path it must be under
 * rcu_read_lock protection. It returns false if the frame
 * can be processed immediately, true if it was consumed.
 */
static bool ieee80211_sta_manage_reorder_buf(struct ieee80211_hw *hw,
					     struct tid_ampdu_rx *tid_agg_rx,
					     struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	u16 sc = le16_to_cpu(hdr->seq_ctrl);
	u16 mpdu_seq_num = (sc & IEEE80211_SCTL_SEQ) >> 4;
	u16 head_seq_num, buf_size;
	int index;
	bool ret = true;

	spin_lock(&tid_agg_rx->reorder_lock);

	buf_size = tid_agg_rx->buf_size;
	head_seq_num = tid_agg_rx->head_seq_num;

	/* frame with out of date sequence number */
	if (seq_less(mpdu_seq_num, head_seq_num)) {
		dev_kfree_skb(skb);
		goto out;
	}

	/*
	 * If frame the sequence number exceeds our buffering window
	 * size release some previous frames to make room for this one.
	 */
	if (!seq_less(mpdu_seq_num, head_seq_num + buf_size)) {
		head_seq_num = seq_inc(seq_sub(mpdu_seq_num, buf_size));
		/* release stored frames up to new head to stack */
		ieee80211_release_reorder_frames(hw, tid_agg_rx, head_seq_num);
	}

	/* Now the new frame is always in the range of the reordering buffer */

	index = seq_sub(mpdu_seq_num, tid_agg_rx->ssn) % tid_agg_rx->buf_size;

	/* check if we already stored this frame */
	if (tid_agg_rx->reorder_buf[index]) {
		dev_kfree_skb(skb);
		goto out;
	}

	/*
	 * If the current MPDU is in the right order and nothing else
	 * is stored we can process it directly, no need to buffer it.
	 * If it is first but there's something stored, we may be able
	 * to release frames after this one.
	 */
	if (mpdu_seq_num == tid_agg_rx->head_seq_num &&
	    tid_agg_rx->stored_mpdu_num == 0) {
		tid_agg_rx->head_seq_num = seq_inc(tid_agg_rx->head_seq_num);
		ret = false;
		goto out;
	}

	/* put the frame in the reordering buffer */
	tid_agg_rx->reorder_buf[index] = skb;
	tid_agg_rx->reorder_time[index] = jiffies;
	tid_agg_rx->stored_mpdu_num++;
	ieee80211_sta_reorder_release(hw, tid_agg_rx);

 out:
	spin_unlock(&tid_agg_rx->reorder_lock);
	return ret;
}

/*
 * Reorder MPDUs from A-MPDUs, keeping them on a buffer. Returns
 * true if the MPDU was buffered, false if it should be processed.
 */
static void ieee80211_rx_reorder_ampdu(struct ieee80211_rx_data *rx)
{
	struct sk_buff *skb = rx->skb;
	struct ieee80211_local *local = rx->local;
	struct ieee80211_hw *hw = &local->hw;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *) skb->data;
	struct sta_info *sta = rx->sta;
	struct tid_ampdu_rx *tid_agg_rx;
	u16 sc;
	int tid;

	if (!ieee80211_is_data_qos(hdr->frame_control))
		goto dont_reorder;

	/*
	 * filter the QoS data rx stream according to
	 * STA/TID and check if this STA/TID is on aggregation
	 */

	if (!sta)
		goto dont_reorder;

	tid = *ieee80211_get_qos_ctl(hdr) & IEEE80211_QOS_CTL_TID_MASK;

	tid_agg_rx = rcu_dereference(sta->ampdu_mlme.tid_rx[tid]);
	if (!tid_agg_rx)
		goto dont_reorder;

	/* qos null data frames are excluded */
	if (unlikely(hdr->frame_control & cpu_to_le16(IEEE80211_STYPE_NULLFUNC)))
		goto dont_reorder;

	/* new, potentially un-ordered, ampdu frame - process it */

	/* reset session timer */
	if (tid_agg_rx->timeout)
		mod_timer(&tid_agg_rx->session_timer,
			  TU_TO_EXP_TIME(tid_agg_rx->timeout));

	/* if this mpdu is fragmented - terminate rx aggregation session */
	sc = le16_to_cpu(hdr->seq_ctrl);
	if (sc & IEEE80211_SCTL_FRAG) {
		skb->pkt_type = IEEE80211_SDATA_QUEUE_TYPE_FRAME;
		skb_queue_tail(&rx->sdata->skb_queue, skb);
		ieee80211_queue_work(&local->hw, &rx->sdata->work);
		return;
	}

	/*
	 * No locking needed -- we will only ever process one
	 * RX packet at a time, and thus own tid_agg_rx. All
	 * other code manipulating it needs to (and does) make
	 * sure that we cannot get to it any more before doing
	 * anything with it.
	 */
	if (ieee80211_sta_manage_reorder_buf(hw, tid_agg_rx, skb))
		return;

 dont_reorder:
	skb_queue_tail(&local->rx_skb_queue, skb);
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_check(struct ieee80211_rx_data *rx)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)rx->skb->data;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);

	/* Drop duplicate 802.11 retransmissions (IEEE 802.11 Chap. 9.2.9) */
	if (rx->sta && !is_multicast_ether_addr(hdr->addr1)) {
		if (unlikely(ieee80211_has_retry(hdr->frame_control) &&
			     rx->sta->last_seq_ctrl[rx->seqno_idx] ==
			     hdr->seq_ctrl)) {
			if (status->rx_flags & IEEE80211_RX_RA_MATCH) {
				rx->local->dot11FrameDuplicateCount++;
				rx->sta->num_duplicates++;
			}
			return RX_DROP_UNUSABLE;
		} else
			rx->sta->last_seq_ctrl[rx->seqno_idx] = hdr->seq_ctrl;
	}

	if (unlikely(rx->skb->len < 16)) {
		I802_DEBUG_INC(rx->local->rx_handlers_drop_short);
		return RX_DROP_MONITOR;
	}

	/* Drop disallowed frame classes based on STA auth/assoc state;
	 * IEEE 802.11, Chap 5.5.
	 *
	 * mac80211 filters only based on association state, i.e. it drops
	 * Class 3 frames from not associated stations. hostapd sends
	 * deauth/disassoc frames when needed. In addition, hostapd is
	 * responsible for filtering on both auth and assoc states.
	 */

	if (ieee80211_vif_is_mesh(&rx->sdata->vif))
		return ieee80211_rx_mesh_check(rx);

	if (unlikely((ieee80211_is_data(hdr->frame_control) ||
		      ieee80211_is_pspoll(hdr->frame_control)) &&
		     rx->sdata->vif.type != NL80211_IFTYPE_ADHOC &&
		     rx->sdata->vif.type != NL80211_IFTYPE_WDS &&
		     (!rx->sta || !test_sta_flag(rx->sta, WLAN_STA_ASSOC)))) {
		if (rx->sta && rx->sta->dummy &&
		    ieee80211_is_data_present(hdr->frame_control)) {
			u16 ethertype;
			u8 *payload;

			payload = rx->skb->data +
				ieee80211_hdrlen(hdr->frame_control);
			ethertype = (payload[6] << 8) | payload[7];
			if (cpu_to_be16(ethertype) ==
			    rx->sdata->control_port_protocol)
				return RX_CONTINUE;
		}
		return RX_DROP_MONITOR;
	}

	return RX_CONTINUE;
}


static ieee80211_rx_result debug_noinline
ieee80211_rx_h_decrypt(struct ieee80211_rx_data *rx)
{
	struct sk_buff *skb = rx->skb;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	int keyidx;
	int hdrlen;
	ieee80211_rx_result result = RX_DROP_UNUSABLE;
	struct ieee80211_key *sta_ptk = NULL;
	int mmie_keyidx = -1;
	__le16 fc;

	/*
	 * Key selection 101
	 *
	 * There are four types of keys:
	 *  - GTK (group keys)
	 *  - IGTK (group keys for management frames)
	 *  - PTK (pairwise keys)
	 *  - STK (station-to-station pairwise keys)
	 *
	 * When selecting a key, we have to distinguish between multicast
	 * (including broadcast) and unicast frames, the latter can only
	 * use PTKs and STKs while the former always use GTKs and IGTKs.
	 * Unless, of course, actual WEP keys ("pre-RSNA") are used, then
	 * unicast frames can also use key indices like GTKs. Hence, if we
	 * don't have a PTK/STK we check the key index for a WEP key.
	 *
	 * Note that in a regular BSS, multicast frames are sent by the
	 * AP only, associated stations unicast the frame to the AP first
	 * which then multicasts it on their behalf.
	 *
	 * There is also a slight problem in IBSS mode: GTKs are negotiated
	 * with each station, that is something we don't currently handle.
	 * The spec seems to expect that one negotiates the same key with
	 * every station but there's no such requirement; VLANs could be
	 * possible.
	 */

	/*
	 * No point in finding a key and decrypting if the frame is neither
	 * addressed to us nor a multicast frame.
	 */
	if (!(status->rx_flags & IEEE80211_RX_RA_MATCH))
		return RX_CONTINUE;

	/* start without a key */
	rx->key = NULL;

	if (rx->sta)
		sta_ptk = rcu_dereference(rx->sta->ptk);

	fc = hdr->frame_control;

	if (!ieee80211_has_protected(fc))
		mmie_keyidx = ieee80211_get_mmie_keyidx(rx->skb);

	if (!is_multicast_ether_addr(hdr->addr1) && sta_ptk) {
		rx->key = sta_ptk;
		if ((status->flag & RX_FLAG_DECRYPTED) &&
		    (status->flag & RX_FLAG_IV_STRIPPED))
			return RX_CONTINUE;
		/* Skip decryption if the frame is not protected. */
		if (!ieee80211_has_protected(fc))
			return RX_CONTINUE;
	} else if (mmie_keyidx >= 0) {
		/* Broadcast/multicast robust management frame / BIP */
		if ((status->flag & RX_FLAG_DECRYPTED) &&
		    (status->flag & RX_FLAG_IV_STRIPPED))
			return RX_CONTINUE;

		if (mmie_keyidx < NUM_DEFAULT_KEYS ||
		    mmie_keyidx >= NUM_DEFAULT_KEYS + NUM_DEFAULT_MGMT_KEYS)
			return RX_DROP_MONITOR; /* unexpected BIP keyidx */
		if (rx->sta)
			rx->key = rcu_dereference(rx->sta->gtk[mmie_keyidx]);
		if (!rx->key)
			rx->key = rcu_dereference(rx->sdata->keys[mmie_keyidx]);
	} else if (!ieee80211_has_protected(fc)) {
		/*
		 * The frame was not protected, so skip decryption. However, we
		 * need to set rx->key if there is a key that could have been
		 * used so that the frame may be dropped if encryption would
		 * have been expected.
		 */
		struct ieee80211_key *key = NULL;
		struct ieee80211_sub_if_data *sdata = rx->sdata;
		int i;

		if (ieee80211_is_mgmt(fc) &&
		    is_multicast_ether_addr(hdr->addr1) &&
		    (key = rcu_dereference(rx->sdata->default_mgmt_key)))
			rx->key = key;
		else {
			if (rx->sta) {
				for (i = 0; i < NUM_DEFAULT_KEYS; i++) {
					key = rcu_dereference(rx->sta->gtk[i]);
					if (key)
						break;
				}
			}
			if (!key) {
				for (i = 0; i < NUM_DEFAULT_KEYS; i++) {
					key = rcu_dereference(sdata->keys[i]);
					if (key)
						break;
				}
			}
			if (key)
				rx->key = key;
		}
		return RX_CONTINUE;
	} else {
		u8 keyid;
		/*
		 * The device doesn't give us the IV so we won't be
		 * able to look up the key. That's ok though, we
		 * don't need to decrypt the frame, we just won't
		 * be able to keep statistics accurate.
		 * Except for key threshold notifications, should
		 * we somehow allow the driver to tell us which key
		 * the hardware used if this flag is set?
		 */
		if ((status->flag & RX_FLAG_DECRYPTED) &&
		    (status->flag & RX_FLAG_IV_STRIPPED))
			return RX_CONTINUE;

		hdrlen = ieee80211_hdrlen(fc);

		if (rx->skb->len < 8 + hdrlen)
			return RX_DROP_UNUSABLE; /* TODO: count this? */

		/*
		 * no need to call ieee80211_wep_get_keyidx,
		 * it verifies a bunch of things we've done already
		 */
		skb_copy_bits(rx->skb, hdrlen + 3, &keyid, 1);
		keyidx = keyid >> 6;

		/* check per-station GTK first, if multicast packet */
		if (is_multicast_ether_addr(hdr->addr1) && rx->sta)
			rx->key = rcu_dereference(rx->sta->gtk[keyidx]);

		/* if not found, try default key */
		if (!rx->key) {
			rx->key = rcu_dereference(rx->sdata->keys[keyidx]);

			/*
			 * RSNA-protected unicast frames should always be
			 * sent with pairwise or station-to-station keys,
			 * but for WEP we allow using a key index as well.
			 */
			if (rx->key &&
			    rx->key->conf.cipher != WLAN_CIPHER_SUITE_WEP40 &&
			    rx->key->conf.cipher != WLAN_CIPHER_SUITE_WEP104 &&
			    !is_multicast_ether_addr(hdr->addr1))
				rx->key = NULL;
		}
	}

	if (rx->key) {
		if (unlikely(rx->key->flags & KEY_FLAG_TAINTED))
			return RX_DROP_MONITOR;

		rx->key->tx_rx_count++;
		/* TODO: add threshold stuff again */
	} else {
		return RX_DROP_MONITOR;
	}

	if (skb_linearize(rx->skb))
		return RX_DROP_UNUSABLE;
	/* the hdr variable is invalid now! */

	switch (rx->key->conf.cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104:
		/* Check for weak IVs if possible */
		if (rx->sta && ieee80211_is_data(fc) &&
		    (!(status->flag & RX_FLAG_IV_STRIPPED) ||
		     !(status->flag & RX_FLAG_DECRYPTED)) &&
		    ieee80211_wep_is_weak_iv(rx->skb, rx->key))
			rx->sta->wep_weak_iv_count++;

		result = ieee80211_crypto_wep_decrypt(rx);
		break;
	case WLAN_CIPHER_SUITE_TKIP:
		result = ieee80211_crypto_tkip_decrypt(rx);
		break;
	case WLAN_CIPHER_SUITE_CCMP:
		result = ieee80211_crypto_ccmp_decrypt(rx);
		break;
	case WLAN_CIPHER_SUITE_AES_CMAC:
		result = ieee80211_crypto_aes_cmac_decrypt(rx);
		break;
	default:
		/*
		 * We can reach here only with HW-only algorithms
		 * but why didn't it decrypt the frame?!
		 */
		return RX_DROP_UNUSABLE;
	}

	/* either the frame has been decrypted or will be dropped */
	status->flag |= RX_FLAG_DECRYPTED;

	return result;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_check_more_data(struct ieee80211_rx_data *rx)
{
	struct ieee80211_local *local;
	struct ieee80211_hdr *hdr;
	struct sk_buff *skb;

	local = rx->local;
	skb = rx->skb;
	hdr = (struct ieee80211_hdr *) skb->data;

	if (!local->pspolling)
		return RX_CONTINUE;

	if (!ieee80211_has_fromds(hdr->frame_control))
		/* this is not from AP */
		return RX_CONTINUE;

	if (!ieee80211_is_data(hdr->frame_control))
		return RX_CONTINUE;

	if (!ieee80211_has_moredata(hdr->frame_control)) {
		/* AP has no more frames buffered for us */
		local->pspolling = false;
		return RX_CONTINUE;
	}

	/* more data bit is set, let's request a new frame from the AP */
	ieee80211_send_pspoll(local, rx->sdata);

	return RX_CONTINUE;
}

static void ap_sta_ps_start(struct sta_info *sta)
{
	struct ieee80211_sub_if_data *sdata = sta->sdata;
	struct ieee80211_local *local = sdata->local;

	atomic_inc(&sdata->bss->num_sta_ps);
	set_sta_flag(sta, WLAN_STA_PS_STA);
	if (!(local->hw.flags & IEEE80211_HW_AP_LINK_PS))
		drv_sta_notify(local, sdata, STA_NOTIFY_SLEEP, &sta->sta);
#ifdef CONFIG_MAC80211_VERBOSE_PS_DEBUG
	printk(KERN_DEBUG "%s: STA %pM aid %d enters power save mode\n",
	       sdata->name, sta->sta.addr, sta->sta.aid);
#endif /* CONFIG_MAC80211_VERBOSE_PS_DEBUG */
}

static void ap_sta_ps_end(struct sta_info *sta)
{
	struct ieee80211_sub_if_data *sdata = sta->sdata;

	atomic_dec(&sdata->bss->num_sta_ps);

#ifdef CONFIG_MAC80211_VERBOSE_PS_DEBUG
	printk(KERN_DEBUG "%s: STA %pM aid %d exits power save mode\n",
	       sdata->name, sta->sta.addr, sta->sta.aid);
#endif /* CONFIG_MAC80211_VERBOSE_PS_DEBUG */

	if (test_sta_flag(sta, WLAN_STA_PS_DRIVER)) {
#ifdef CONFIG_MAC80211_VERBOSE_PS_DEBUG
		printk(KERN_DEBUG "%s: STA %pM aid %d driver-ps-blocked\n",
		       sdata->name, sta->sta.addr, sta->sta.aid);
#endif /* CONFIG_MAC80211_VERBOSE_PS_DEBUG */
		return;
	}

	ieee80211_sta_ps_deliver_wakeup(sta);
}

int ieee80211_sta_ps_transition(struct ieee80211_sta *sta, bool start)
{
	struct sta_info *sta_inf = container_of(sta, struct sta_info, sta);
	bool in_ps;

	WARN_ON(!(sta_inf->local->hw.flags & IEEE80211_HW_AP_LINK_PS));

	/* Don't let the same PS state be set twice */
	in_ps = test_sta_flag(sta_inf, WLAN_STA_PS_STA);
	if ((start && in_ps) || (!start && !in_ps))
		return -EINVAL;

	if (start)
		ap_sta_ps_start(sta_inf);
	else
		ap_sta_ps_end(sta_inf);

	return 0;
}
EXPORT_SYMBOL(ieee80211_sta_ps_transition);

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_uapsd_and_pspoll(struct ieee80211_rx_data *rx)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct ieee80211_hdr *hdr = (void *)rx->skb->data;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);
	int tid, ac;

	if (!rx->sta || !(status->rx_flags & IEEE80211_RX_RA_MATCH))
		return RX_CONTINUE;

	if (sdata->vif.type != NL80211_IFTYPE_AP &&
	    sdata->vif.type != NL80211_IFTYPE_AP_VLAN)
		return RX_CONTINUE;

	/*
	 * The device handles station powersave, so don't do anything about
	 * uAPSD and PS-Poll frames (the latter shouldn't even come up from
	 * it to mac80211 since they're handled.)
	 */
	if (sdata->local->hw.flags & IEEE80211_HW_AP_LINK_PS)
		return RX_CONTINUE;

	/*
	 * Don't do anything if the station isn't already asleep. In
	 * the uAPSD case, the station will probably be marked asleep,
	 * in the PS-Poll case the station must be confused ...
	 */
	if (!test_sta_flag(rx->sta, WLAN_STA_PS_STA))
		return RX_CONTINUE;

	if (unlikely(ieee80211_is_pspoll(hdr->frame_control))) {
		if (!test_sta_flag(rx->sta, WLAN_STA_SP)) {
			if (!test_sta_flag(rx->sta, WLAN_STA_PS_DRIVER))
				ieee80211_sta_ps_deliver_poll_response(rx->sta);
			else
				set_sta_flag(rx->sta, WLAN_STA_PSPOLL);
		}

		/* Free PS Poll skb here instead of returning RX_DROP that would
		 * count as an dropped frame. */
		dev_kfree_skb(rx->skb);

		return RX_QUEUED;
	} else if (!ieee80211_has_morefrags(hdr->frame_control) &&
		   !(status->rx_flags & IEEE80211_RX_DEFERRED_RELEASE) &&
		   ieee80211_has_pm(hdr->frame_control) &&
		   (ieee80211_is_data_qos(hdr->frame_control) ||
		    ieee80211_is_qos_nullfunc(hdr->frame_control))) {
		tid = *ieee80211_get_qos_ctl(hdr) & IEEE80211_QOS_CTL_TID_MASK;
		ac = ieee802_1d_to_ac[tid & 7];

		/*
		 * If this AC is not trigger-enabled do nothing.
		 *
		 * NB: This could/should check a separate bitmap of trigger-
		 * enabled queues, but for now we only implement uAPSD w/o
		 * TSPEC changes to the ACs, so they're always the same.
		 */
		if (!(rx->sta->sta.uapsd_queues & BIT(ac)))
			return RX_CONTINUE;

		/* if we are in a service period, do nothing */
		if (test_sta_flag(rx->sta, WLAN_STA_SP))
			return RX_CONTINUE;

		if (!test_sta_flag(rx->sta, WLAN_STA_PS_DRIVER))
			ieee80211_sta_ps_deliver_uapsd(rx->sta);
		else
			set_sta_flag(rx->sta, WLAN_STA_UAPSD);
	}

	return RX_CONTINUE;
}

/**
 * ieee80211_rx_h_sta_process
 * modified                                             James Tsunghsiao Pan (100062587)
 * purpose:                                             allow coding packet
 */
static ieee80211_rx_result debug_noinline
ieee80211_rx_h_sta_process(struct ieee80211_rx_data *rx)
{
	struct sta_info *sta = rx->sta;
	struct sk_buff *skb = rx->skb;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;

	if (!sta)
		return RX_CONTINUE;

	/*
	 * Update last_rx only for IBSS packets which are for the current
	 * BSSID to avoid keeping the current IBSS network alive in cases
	 * where other STAs start using different BSSID.
	 */
	if (rx->sdata->vif.type == NL80211_IFTYPE_ADHOC) {
		u8 *bssid = ieee80211_get_bssid(hdr, rx->skb->len,
						NL80211_IFTYPE_ADHOC);
		if (compare_ether_addr(bssid, rx->sdata->u.ibss.bssid) == 0) {
			sta->last_rx = jiffies;
			if (ieee80211_is_data(hdr->frame_control) || ieee80211_is_coding(hdr->frame_control)) {
				sta->last_rx_rate_idx = status->rate_idx;
				sta->last_rx_rate_flag = status->flag;
			}
		}
	} else if (!is_multicast_ether_addr(hdr->addr1)) {
		/*
		 * Mesh beacons will update last_rx when if they are found to
		 * match the current local configuration when processed.
		 */
		sta->last_rx = jiffies;
		if (ieee80211_is_data(hdr->frame_control) || ieee80211_is_coding(hdr->frame_control)) {
			sta->last_rx_rate_idx = status->rate_idx;
			sta->last_rx_rate_flag = status->flag;
		}
	}

	if (!(status->rx_flags & IEEE80211_RX_RA_MATCH))
		return RX_CONTINUE;

	if (rx->sdata->vif.type == NL80211_IFTYPE_STATION)
		ieee80211_sta_rx_notify(rx->sdata, hdr);

	sta->rx_fragments++;
	sta->rx_bytes += rx->skb->len;
	sta->last_signal = status->signal;
	ewma_add(&sta->avg_signal, -status->signal);

	/*
	 * Change STA power saving mode only at the end of a frame
	 * exchange sequence.
	 */
	if (!(sta->local->hw.flags & IEEE80211_HW_AP_LINK_PS) &&
	    !ieee80211_has_morefrags(hdr->frame_control) &&
	    !(status->rx_flags & IEEE80211_RX_DEFERRED_RELEASE) &&
	    (rx->sdata->vif.type == NL80211_IFTYPE_AP ||
	     rx->sdata->vif.type == NL80211_IFTYPE_AP_VLAN)) {
		if (test_sta_flag(sta, WLAN_STA_PS_STA)) {
			/*
			 * Ignore doze->wake transitions that are
			 * indicated by non-data frames, the standard
			 * is unclear here, but for example going to
			 * PS mode and then scanning would cause a
			 * doze->wake transition for the probe request,
			 * and that is clearly undesirable.
			 */
			if (ieee80211_is_data(hdr->frame_control) &&
			    !ieee80211_has_pm(hdr->frame_control))
				ap_sta_ps_end(sta);
		} else {
			if (ieee80211_has_pm(hdr->frame_control))
				ap_sta_ps_start(sta);
		}
	}

	/*
	 * Drop (qos-)data::nullfunc frames silently, since they
	 * are used only to control station power saving mode.
	 */
	if (ieee80211_is_nullfunc(hdr->frame_control) ||
	    ieee80211_is_qos_nullfunc(hdr->frame_control)) {
		I802_DEBUG_INC(rx->local->rx_handlers_drop_nullfunc);

		/*
		 * If we receive a 4-addr nullfunc frame from a STA
		 * that was not moved to a 4-addr STA vlan yet, drop
		 * the frame to the monitor interface, to make sure
		 * that hostapd sees it
		 */
		if (ieee80211_has_a4(hdr->frame_control) &&
		    (rx->sdata->vif.type == NL80211_IFTYPE_AP ||
		     (rx->sdata->vif.type == NL80211_IFTYPE_AP_VLAN &&
		      !rx->sdata->u.vlan.sta)))
			return RX_DROP_MONITOR;
		/*
		 * Update counter and free packet here to avoid
		 * counting this as a dropped packed.
		 */
		sta->rx_packets++;
		dev_kfree_skb(rx->skb);
		return RX_QUEUED;
	}

	return RX_CONTINUE;
} /* ieee80211_rx_h_sta_process */

static inline struct ieee80211_fragment_entry *
ieee80211_reassemble_add(struct ieee80211_sub_if_data *sdata,
			 unsigned int frag, unsigned int seq, int rx_queue,
			 struct sk_buff **skb)
{
	struct ieee80211_fragment_entry *entry;
	int idx;

	idx = sdata->fragment_next;
	entry = &sdata->fragments[sdata->fragment_next++];
	if (sdata->fragment_next >= IEEE80211_FRAGMENT_MAX)
		sdata->fragment_next = 0;

	if (!skb_queue_empty(&entry->skb_list)) {
#ifdef CONFIG_MAC80211_VERBOSE_DEBUG
		struct ieee80211_hdr *hdr =
			(struct ieee80211_hdr *) entry->skb_list.next->data;
		printk(KERN_DEBUG "%s: RX reassembly removed oldest "
		       "fragment entry (idx=%d age=%lu seq=%d last_frag=%d "
		       "addr1=%pM addr2=%pM\n",
		       sdata->name, idx,
		       jiffies - entry->first_frag_time, entry->seq,
		       entry->last_frag, hdr->addr1, hdr->addr2);
#endif
		__skb_queue_purge(&entry->skb_list);
	}

	__skb_queue_tail(&entry->skb_list, *skb); /* no need for locking */
	*skb = NULL;
	entry->first_frag_time = jiffies;
	entry->seq = seq;
	entry->rx_queue = rx_queue;
	entry->last_frag = frag;
	entry->ccmp = 0;
	entry->extra_len = 0;

	return entry;
}

static inline struct ieee80211_fragment_entry *
ieee80211_reassemble_find(struct ieee80211_sub_if_data *sdata,
			  unsigned int frag, unsigned int seq,
			  int rx_queue, struct ieee80211_hdr *hdr)
{
	struct ieee80211_fragment_entry *entry;
	int i, idx;

	idx = sdata->fragment_next;
	for (i = 0; i < IEEE80211_FRAGMENT_MAX; i++) {
		struct ieee80211_hdr *f_hdr;

		idx--;
		if (idx < 0)
			idx = IEEE80211_FRAGMENT_MAX - 1;

		entry = &sdata->fragments[idx];
		if (skb_queue_empty(&entry->skb_list) || entry->seq != seq ||
		    entry->rx_queue != rx_queue ||
		    entry->last_frag + 1 != frag)
			continue;

		f_hdr = (struct ieee80211_hdr *)entry->skb_list.next->data;

		/*
		 * Check ftype and addresses are equal, else check next fragment
		 */
		if (((hdr->frame_control ^ f_hdr->frame_control) &
		     cpu_to_le16(IEEE80211_FCTL_FTYPE)) ||
		    compare_ether_addr(hdr->addr1, f_hdr->addr1) != 0 ||
		    compare_ether_addr(hdr->addr2, f_hdr->addr2) != 0)
			continue;

		if (time_after(jiffies, entry->first_frag_time + 2 * HZ)) {
			__skb_queue_purge(&entry->skb_list);
			continue;
		}
		return entry;
	}

	return NULL;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_defragment(struct ieee80211_rx_data *rx)
{
	struct ieee80211_hdr *hdr;
	u16 sc;
	__le16 fc;
	unsigned int frag, seq;
	struct ieee80211_fragment_entry *entry;
	struct sk_buff *skb;
	struct ieee80211_rx_status *status;

	hdr = (struct ieee80211_hdr *)rx->skb->data;
	fc = hdr->frame_control;
	sc = le16_to_cpu(hdr->seq_ctrl);
	frag = sc & IEEE80211_SCTL_FRAG;

	if (likely((!ieee80211_has_morefrags(fc) && frag == 0) ||
		   (rx->skb)->len < 24 ||
		   is_multicast_ether_addr(hdr->addr1))) {
		/* not fragmented */
		goto out;
	}
	I802_DEBUG_INC(rx->local->rx_handlers_fragments);

	if (skb_linearize(rx->skb))
		return RX_DROP_UNUSABLE;

	/*
	 *  skb_linearize() might change the skb->data and
	 *  previously cached variables (in this case, hdr) need to
	 *  be refreshed with the new data.
	 */
	hdr = (struct ieee80211_hdr *)rx->skb->data;
	seq = (sc & IEEE80211_SCTL_SEQ) >> 4;

	if (frag == 0) {
		/* This is the first fragment of a new frame. */
		entry = ieee80211_reassemble_add(rx->sdata, frag, seq,
						 rx->seqno_idx, &(rx->skb));
		if (rx->key && rx->key->conf.cipher == WLAN_CIPHER_SUITE_CCMP &&
		    ieee80211_has_protected(fc)) {
			int queue = rx->security_idx;
			/* Store CCMP PN so that we can verify that the next
			 * fragment has a sequential PN value. */
			entry->ccmp = 1;
			memcpy(entry->last_pn,
			       rx->key->u.ccmp.rx_pn[queue],
			       CCMP_PN_LEN);
		}
		return RX_QUEUED;
	}

	/* This is a fragment for a frame that should already be pending in
	 * fragment cache. Add this fragment to the end of the pending entry.
	 */
	entry = ieee80211_reassemble_find(rx->sdata, frag, seq,
					  rx->seqno_idx, hdr);
	if (!entry) {
		I802_DEBUG_INC(rx->local->rx_handlers_drop_defrag);
		return RX_DROP_MONITOR;
	}

	/* Verify that MPDUs within one MSDU have sequential PN values.
	 * (IEEE 802.11i, 8.3.3.4.5) */
	if (entry->ccmp) {
		int i;
		u8 pn[CCMP_PN_LEN], *rpn;
		int queue;
		if (!rx->key || rx->key->conf.cipher != WLAN_CIPHER_SUITE_CCMP)
			return RX_DROP_UNUSABLE;
		memcpy(pn, entry->last_pn, CCMP_PN_LEN);
		for (i = CCMP_PN_LEN - 1; i >= 0; i--) {
			pn[i]++;
			if (pn[i])
				break;
		}
		queue = rx->security_idx;
		rpn = rx->key->u.ccmp.rx_pn[queue];
		if (memcmp(pn, rpn, CCMP_PN_LEN))
			return RX_DROP_UNUSABLE;
		memcpy(entry->last_pn, pn, CCMP_PN_LEN);
	}

	skb_pull(rx->skb, ieee80211_hdrlen(fc));
	__skb_queue_tail(&entry->skb_list, rx->skb);
	entry->last_frag = frag;
	entry->extra_len += rx->skb->len;
	if (ieee80211_has_morefrags(fc)) {
		rx->skb = NULL;
		return RX_QUEUED;
	}

	rx->skb = __skb_dequeue(&entry->skb_list);
	if (skb_tailroom(rx->skb) < entry->extra_len) {
		I802_DEBUG_INC(rx->local->rx_expand_skb_head2);
		if (unlikely(pskb_expand_head(rx->skb, 0, entry->extra_len,
					      GFP_ATOMIC))) {
			I802_DEBUG_INC(rx->local->rx_handlers_drop_defrag);
			__skb_queue_purge(&entry->skb_list);
			return RX_DROP_UNUSABLE;
		}
	}
	while ((skb = __skb_dequeue(&entry->skb_list))) {
		memcpy(skb_put(rx->skb, skb->len), skb->data, skb->len);
		dev_kfree_skb(skb);
	}

	/* Complete frame has been reassembled - process it now */
	status = IEEE80211_SKB_RXCB(rx->skb);
	status->rx_flags |= IEEE80211_RX_FRAGMENTED;

 out:
	if (rx->sta)
		rx->sta->rx_packets++;
	if (is_multicast_ether_addr(hdr->addr1))
		rx->local->dot11MulticastReceivedFrameCount++;
	else
		ieee80211_led_rx(rx->local);
	return RX_CONTINUE;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_remove_qos_control(struct ieee80211_rx_data *rx)
{
	u8 *data = rx->skb->data;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)data;

	if (!ieee80211_is_data_qos(hdr->frame_control))
		return RX_CONTINUE;

	/* remove the qos control field, update frame type and meta-data */
	memmove(data + IEEE80211_QOS_CTL_LEN, data,
		ieee80211_hdrlen(hdr->frame_control) - IEEE80211_QOS_CTL_LEN);
	hdr = (struct ieee80211_hdr *)skb_pull(rx->skb, IEEE80211_QOS_CTL_LEN);
	/* change frame type to non QOS */
	hdr->frame_control &= ~cpu_to_le16(IEEE80211_STYPE_QOS_DATA);

	return RX_CONTINUE;
}

static int
ieee80211_802_1x_port_control(struct ieee80211_rx_data *rx)
{
	if (unlikely(!rx->sta ||
	    !test_sta_flag(rx->sta, WLAN_STA_AUTHORIZED)))
		return -EACCES;

	return 0;
}

static int
ieee80211_drop_unencrypted(struct ieee80211_rx_data *rx, __le16 fc)
{
	struct sk_buff *skb = rx->skb;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);

	/*
	 * Pass through unencrypted frames if the hardware has
	 * decrypted them already.
	 */
	if (status->flag & RX_FLAG_DECRYPTED)
		return 0;

	/* Drop unencrypted frames if key is set. */
	if (unlikely(!ieee80211_has_protected(fc) &&
		     !ieee80211_is_nullfunc(fc) &&
		     ieee80211_is_data(fc) &&
		     (rx->key || rx->sdata->drop_unencrypted)))
		return -EACCES;

	return 0;
}

static int
ieee80211_drop_unencrypted_mgmt(struct ieee80211_rx_data *rx)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)rx->skb->data;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);
	__le16 fc = hdr->frame_control;

	/*
	 * Pass through unencrypted frames if the hardware has
	 * decrypted them already.
	 */
	if (status->flag & RX_FLAG_DECRYPTED)
		return 0;

	if (rx->sta && test_sta_flag(rx->sta, WLAN_STA_MFP)) {
		if (unlikely(!ieee80211_has_protected(fc) &&
			     ieee80211_is_unicast_robust_mgmt_frame(rx->skb) &&
			     rx->key)) {
			if (ieee80211_is_deauth(fc))
				cfg80211_send_unprot_deauth(rx->sdata->dev,
							    rx->skb->data,
							    rx->skb->len);
			else if (ieee80211_is_disassoc(fc))
				cfg80211_send_unprot_disassoc(rx->sdata->dev,
							      rx->skb->data,
							      rx->skb->len);
			return -EACCES;
		}
		/* BIP does not use Protected field, so need to check MMIE */
		if (unlikely(ieee80211_is_multicast_robust_mgmt_frame(rx->skb) &&
			     ieee80211_get_mmie_keyidx(rx->skb) < 0)) {
			if (ieee80211_is_deauth(fc))
				cfg80211_send_unprot_deauth(rx->sdata->dev,
							    rx->skb->data,
							    rx->skb->len);
			else if (ieee80211_is_disassoc(fc))
				cfg80211_send_unprot_disassoc(rx->sdata->dev,
							      rx->skb->data,
							      rx->skb->len);
			return -EACCES;
		}
		/*
		 * When using MFP, Action frames are not allowed prior to
		 * having configured keys.
		 */
		if (unlikely(ieee80211_is_action(fc) && !rx->key &&
			     ieee80211_is_robust_mgmt_frame(
				     (struct ieee80211_hdr *) rx->skb->data)))
			return -EACCES;
	}

	return 0;
}

static int
__ieee80211_data_to_8023(struct ieee80211_rx_data *rx, bool *port_control)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)rx->skb->data;
	bool check_port_control = false;
	struct ethhdr *ehdr;
	int ret;

	*port_control = false;
	if (ieee80211_has_a4(hdr->frame_control) &&
	    sdata->vif.type == NL80211_IFTYPE_AP_VLAN && !sdata->u.vlan.sta)
		return -1;

	if (sdata->vif.type == NL80211_IFTYPE_STATION &&
	    !!sdata->u.mgd.use_4addr != !!ieee80211_has_a4(hdr->frame_control)) {

		if (!sdata->u.mgd.use_4addr)
			return -1;
		else
			check_port_control = true;
	}

	if (is_multicast_ether_addr(hdr->addr1) &&
	    sdata->vif.type == NL80211_IFTYPE_AP_VLAN && sdata->u.vlan.sta)
		return -1;

	ret = ieee80211_data_to_8023(rx->skb, sdata->vif.addr, sdata->vif.type);
	if (ret < 0)
		return ret;

	ehdr = (struct ethhdr *) rx->skb->data;
	if (ehdr->h_proto == rx->sdata->control_port_protocol)
		*port_control = true;
	else if (check_port_control)
		return -1;

	return 0;
}

/*
 * requires that rx->skb is a frame with ethernet header
 */
static bool ieee80211_frame_allowed(struct ieee80211_rx_data *rx, __le16 fc)
{
	static const u8 pae_group_addr[ETH_ALEN] __aligned(2)
		= { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x03 };
	struct ethhdr *ehdr = (struct ethhdr *) rx->skb->data;

	/*
	 * Allow EAPOL frames to us/the PAE group address regardless
	 * of whether the frame was encrypted or not.
	 */
	if (ehdr->h_proto == rx->sdata->control_port_protocol &&
	    (compare_ether_addr(ehdr->h_dest, rx->sdata->vif.addr) == 0 ||
	     compare_ether_addr(ehdr->h_dest, pae_group_addr) == 0))
		return true;

	if (ieee80211_802_1x_port_control(rx) ||
	    ieee80211_drop_unencrypted(rx, fc))
		return false;

	return true;
}

/*
 * requires that rx->skb is a frame with ethernet header
 */
static void
ieee80211_deliver_skb(struct ieee80211_rx_data *rx)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct net_device *dev = sdata->dev;
	struct sk_buff *skb, *xmit_skb;
	struct ethhdr *ehdr = (struct ethhdr *) rx->skb->data;
	struct sta_info *dsta;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);

	skb = rx->skb;
	xmit_skb = NULL;

	if ((sdata->vif.type == NL80211_IFTYPE_AP ||
	     sdata->vif.type == NL80211_IFTYPE_AP_VLAN) &&
	    !(sdata->flags & IEEE80211_SDATA_DONT_BRIDGE_PACKETS) &&
	    (status->rx_flags & IEEE80211_RX_RA_MATCH) &&
	    (sdata->vif.type != NL80211_IFTYPE_AP_VLAN || !sdata->u.vlan.sta)) {
		if (is_multicast_ether_addr(ehdr->h_dest)) {
			/*
			 * send multicast frames both to higher layers in
			 * local net stack and back to the wireless medium
			 */
			xmit_skb = skb_copy(skb, GFP_ATOMIC);
			if (!xmit_skb && net_ratelimit())
				printk(KERN_DEBUG "%s: failed to clone "
				       "multicast frame\n", dev->name);
		} else {
			dsta = sta_info_get(sdata, skb->data);
			if (dsta) {
				/*
				 * The destination station is associated to
				 * this AP (in this VLAN), so send the frame
				 * directly to it and do not pass it to local
				 * net stack.
				 */
				xmit_skb = skb;
				skb = NULL;
			}
		}
	}

	if (skb) {
		int align __maybe_unused;

#ifndef CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS
		/*
		 * 'align' will only take the values 0 or 2 here
		 * since all frames are required to be aligned
		 * to 2-byte boundaries when being passed to
		 * mac80211. That also explains the __skb_push()
		 * below.
		 */
		align = ((unsigned long)(skb->data + sizeof(struct ethhdr))) & 3;
		if (align) {
			if (WARN_ON(skb_headroom(skb) < 3)) {
				dev_kfree_skb(skb);
				skb = NULL;
			} else {
				u8 *data = skb->data;
				size_t len = skb_headlen(skb);
				skb->data -= align;
				memmove(skb->data, data, len);
				skb_set_tail_pointer(skb, len);
			}
		}
#endif

		if (skb) {
			/* deliver to local stack */
			skb->protocol = eth_type_trans(skb, dev);
			memset(skb->cb, 0, sizeof(skb->cb));
			netif_receive_skb(skb);
		}
	}

	if (xmit_skb) {
		/* send to wireless media */
		xmit_skb->protocol = htons(ETH_P_802_3);
		skb_reset_network_header(xmit_skb);
		skb_reset_mac_header(xmit_skb);
		dev_queue_xmit(xmit_skb);
	}
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_amsdu(struct ieee80211_rx_data *rx)
{
	struct net_device *dev = rx->sdata->dev;
	struct sk_buff *skb = rx->skb;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	__le16 fc = hdr->frame_control;
	struct sk_buff_head frame_list;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);

	if (unlikely(!ieee80211_is_data(fc)))
		return RX_CONTINUE;

	if (unlikely(!ieee80211_is_data_present(fc)))
		return RX_DROP_MONITOR;

	if (!(status->rx_flags & IEEE80211_RX_AMSDU))
		return RX_CONTINUE;

	if (ieee80211_has_a4(hdr->frame_control) &&
	    rx->sdata->vif.type == NL80211_IFTYPE_AP_VLAN &&
	    !rx->sdata->u.vlan.sta)
		return RX_DROP_UNUSABLE;

	if (is_multicast_ether_addr(hdr->addr1) &&
	    ((rx->sdata->vif.type == NL80211_IFTYPE_AP_VLAN &&
	      rx->sdata->u.vlan.sta) ||
	     (rx->sdata->vif.type == NL80211_IFTYPE_STATION &&
	      rx->sdata->u.mgd.use_4addr)))
		return RX_DROP_UNUSABLE;

	skb->dev = dev;
	__skb_queue_head_init(&frame_list);

	if (skb_linearize(skb))
		return RX_DROP_UNUSABLE;

	ieee80211_amsdu_to_8023s(skb, &frame_list, dev->dev_addr,
				 rx->sdata->vif.type,
				 rx->local->hw.extra_tx_headroom, true);

	while (!skb_queue_empty(&frame_list)) {
		rx->skb = __skb_dequeue(&frame_list);

		if (!ieee80211_frame_allowed(rx, fc)) {
			dev_kfree_skb(rx->skb);
			continue;
		}
		dev->stats.rx_packets++;
		dev->stats.rx_bytes += rx->skb->len;

		ieee80211_deliver_skb(rx);
	}

	return RX_QUEUED;
}

#ifdef CONFIG_MAC80211_MESH
static ieee80211_rx_result
ieee80211_rx_h_mesh_fwding(struct ieee80211_rx_data *rx)
{
	struct ieee80211_hdr *hdr;
	struct ieee80211s_hdr *mesh_hdr;
	unsigned int hdrlen;
	struct sk_buff *skb = rx->skb, *fwd_skb;
	struct ieee80211_local *local = rx->local;
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);

	hdr = (struct ieee80211_hdr *) skb->data;
	hdrlen = ieee80211_hdrlen(hdr->frame_control);
	mesh_hdr = (struct ieee80211s_hdr *) (skb->data + hdrlen);

	/* frame is in RMC, don't forward */
	if (ieee80211_is_data(hdr->frame_control) &&
	    is_multicast_ether_addr(hdr->addr1) &&
	    mesh_rmc_check(hdr->addr3, mesh_hdr, rx->sdata))
		return RX_DROP_MONITOR;

	if (!ieee80211_is_data(hdr->frame_control))
		return RX_CONTINUE;

	if (!mesh_hdr->ttl)
		/* illegal frame */
		return RX_DROP_MONITOR;

	if (ieee80211_queue_stopped(&local->hw, skb_get_queue_mapping(skb))) {
		IEEE80211_IFSTA_MESH_CTR_INC(&sdata->u.mesh,
						dropped_frames_congestion);
		return RX_DROP_MONITOR;
	}

	if (mesh_hdr->flags & MESH_FLAGS_AE) {
		struct mesh_path *mppath;
		char *proxied_addr;
		char *mpp_addr;

		if (is_multicast_ether_addr(hdr->addr1)) {
			mpp_addr = hdr->addr3;
			proxied_addr = mesh_hdr->eaddr1;
		} else {
			mpp_addr = hdr->addr4;
			proxied_addr = mesh_hdr->eaddr2;
		}

		rcu_read_lock();
		mppath = mpp_path_lookup(proxied_addr, sdata);
		if (!mppath) {
			mpp_path_add(proxied_addr, mpp_addr, sdata);
		} else {
			spin_lock_bh(&mppath->state_lock);
			if (compare_ether_addr(mppath->mpp, mpp_addr) != 0)
				memcpy(mppath->mpp, mpp_addr, ETH_ALEN);
			spin_unlock_bh(&mppath->state_lock);
		}
		rcu_read_unlock();
	}

	/* Frame has reached destination.  Don't forward */
	if (!is_multicast_ether_addr(hdr->addr1) &&
	    compare_ether_addr(sdata->vif.addr, hdr->addr3) == 0)
		return RX_CONTINUE;

	mesh_hdr->ttl--;

	if (status->rx_flags & IEEE80211_RX_RA_MATCH) {
		if (!mesh_hdr->ttl)
			IEEE80211_IFSTA_MESH_CTR_INC(&rx->sdata->u.mesh,
						     dropped_frames_ttl);
		else {
			struct ieee80211_hdr *fwd_hdr;
			struct ieee80211_tx_info *info;

			fwd_skb = skb_copy(skb, GFP_ATOMIC);

			if (!fwd_skb && net_ratelimit())
				printk(KERN_DEBUG "%s: failed to clone mesh frame\n",
						   sdata->name);
			if (!fwd_skb)
				goto out;

			fwd_hdr =  (struct ieee80211_hdr *) fwd_skb->data;
			memcpy(fwd_hdr->addr2, sdata->vif.addr, ETH_ALEN);
			info = IEEE80211_SKB_CB(fwd_skb);
			memset(info, 0, sizeof(*info));
			info->flags |= IEEE80211_TX_INTFL_NEED_TXPROCESSING;
			info->control.vif = &rx->sdata->vif;
			if (is_multicast_ether_addr(fwd_hdr->addr1)) {
				IEEE80211_IFSTA_MESH_CTR_INC(&sdata->u.mesh,
								fwded_mcast);
				skb_set_queue_mapping(fwd_skb,
					ieee80211_select_queue(sdata, fwd_skb));
				ieee80211_set_qos_hdr(sdata, fwd_skb);
			} else {
				int err;
				/*
				 * Save TA to addr1 to send TA a path error if a
				 * suitable next hop is not found
				 */
				memcpy(fwd_hdr->addr1, fwd_hdr->addr2,
						ETH_ALEN);
				err = mesh_nexthop_lookup(fwd_skb, sdata);
				/* Failed to immediately resolve next hop:
				 * fwded frame was dropped or will be added
				 * later to the pending skb queue.  */
				if (err)
					return RX_DROP_MONITOR;

				IEEE80211_IFSTA_MESH_CTR_INC(&sdata->u.mesh,
								fwded_unicast);
			}
			IEEE80211_IFSTA_MESH_CTR_INC(&sdata->u.mesh,
						     fwded_frames);
			ieee80211_add_pending_skb(local, fwd_skb);
		}
	}

 out:
	if (is_multicast_ether_addr(hdr->addr1) ||
	    sdata->dev->flags & IFF_PROMISC)
		return RX_CONTINUE;
	else
		return RX_DROP_MONITOR;
}
#endif

/**
 * decoding using workqueue
 * modified by                                          James Tsunghsiao Pan (100062587)
 * purpose:                                             decoding
 */

void wq_h_decode_and_forward(void)
{
	struct matrix_ele *pt;
	struct ieee80211_rx_data *nrx;
	bool port_control;
	int err;
	struct sk_buff_head *seg = NULL;
	struct ieee80211_hdr *hdr;
	__le16 fc;

	while(1){
		spin_lock(&hList_lock);
		if(hLast == NULL) break;
		
		pt = hList;
		hList = hList->next;
		if(hList == NULL) hLast = NULL;
		spin_unlock(&hList_lock);

		// deliver to upper layer
		if(matrix_getInverse(pt))
			seg = matrix_decode(pt);
		else
			printk(KERN_DEBUG "[mac80211] RX: get inverse matrix error\n");
		
		while(seg != NULL && !skb_queue_empty(seg)){
			nrx = (struct ieee80211_rx_data *) kmalloc(sizeof(struct ieee80211_rx_data), 
								GFP_KERNEL);
			if(nrx == NULL) {
				printk(KERN_DEBUG "[mac80211] RX: rx_data allocate error\n");
				continue;
			}
			
			memcpy(nrx, pt->rx, sizeof(struct ieee80211_rx_data));
			nrx->skb = skb_dequeue(seg);
			
			hdr = (struct ieee80211_hdr *) nrx->skb->data;
			fc = hdr->frame_control;
		
			err = __ieee80211_data_to_8023(nrx, &port_control);

			if (unlikely(err) || !ieee80211_frame_allowed(nrx, fc)){
				printk(KERN_ALERT "[mac80211] RX: data->8023 error\n");
				dev_kfree_skb(nrx->skb);
				kfree(nrx);
				continue;
			}
	
			nrx->skb->dev = pt->dev;
	
			pt->dev->stats.rx_packets++;
			pt->dev->stats.rx_bytes += nrx->skb->len;
                
			ieee80211_deliver_skb(nrx);
        
			nrx->skb = NULL;        
			kfree(nrx);
		}
        
		//free matrix
		matrix_free(pt);
		if(seg != NULL) kfree(seg);
		seg = NULL;
	}

	spin_unlock(&hList_lock);
}

extern void ieee80211_xmit(struct ieee80211_sub_if_data*, struct sk_buff*);
extern struct queue_ele *qList;
extern void tx_finish_remove(unsigned long);

/**
 * generate double coding frames
 * modified by                                          James Tsunghsiao Pan (100062587)
 * purpose:                                             generate frames
 */

static bool double_coding_frame_gen(struct queue_ele *pt)
{
	struct ieee80211_sub_if_data *sdata = pt->sdata;
	struct sk_buff *dcp, *skb;
	struct coding_header dchdr, *chdr;
	struct ieee80211_hdr *hdr, dhdr;
	unsigned int chdr_len = 0, data_len = 0;
	u8 *pureData, seq;
	int i=0, j;

	pt->addr.next = NULL;
	pt->addr.prev = NULL;
	
	if(pt->tx_number >= RELAY_THRESH && !skb_queue_empty(&pt->batch)){
		pt->tx_number -= RELAY_THRESH;
		skb = skb_dequeue(&pt->batch);
		hdr = (struct ieee80211_hdr *) skb->data;
		chdr = (struct coding_header *) (skb->data + 24);
		chdr_len = 4 + chdr->more_data_len*6;
		data_len = skb->tail - skb->data - 24 - chdr_len;
		skb_queue_head(&pt->batch, skb);

		dhdr.frame_control = cpu_to_le16(IEEE80211_FTYPE_CODING|IEEE80211_STYPE_DCODING);
		// we use the 4th addr space when TX a double coding frame
		// addr1 = DST
		// addr2 = RELAY = STA
		// addr3 = BSSID
		// addr4 = SRC
		// NOTE: addr2 and addr4 are switched at DST
		memcpy(dhdr.addr1, hdr->addr1, ETH_ALEN);
		memcpy(dhdr.addr2, sdata->vif.addr, ETH_ALEN);
		memcpy(dhdr.addr3, hdr->addr3, ETH_ALEN);
		memcpy(dhdr.addr4, hdr->addr2, ETH_ALEN);
		dhdr.duration_id = 0;
		dhdr.seq_ctrl = 0;

		dchdr.batch_id = chdr->batch_id;
		dchdr.batch_size = chdr->batch_size;
		dchdr.seq_num = pt->seq++;
		if(unlikely(pt->seq == 0)) pt->seq = 2;
		else if(unlikely(pt->seq == 1)) pt->seq = 2;
		dchdr.more_data_len = pt->batch.qlen;

		pureData = (u8 *) kmalloc(data_len, GFP_ATOMIC);
		memset(pureData, 0, data_len);
		dcp = alloc_skb(34 + dchdr.more_data_len + data_len, GFP_ATOMIC);
		memcpy(skb_put(dcp, 30), &dhdr,30);
		memcpy(skb_put(dcp, 4), &dchdr,4);

		// double coding 
		for(i = 0, seq = dchdr.seq_num; i < dchdr.more_data_len; i++){
			skb = skb_dequeue(&pt->batch);
			chdr = (struct coding_header *)(skb->data + 24);
			chdr_len = 4 + chdr->more_data_len*6;
			memcpy(skb_put(dcp, 1), &(chdr->seq_num), 1);
			
			for(j = 0; j < data_len; j++){
				*(pureData+j) = GFadd((u8) *(pureData+j),
						      GFmul((u8) *(skb->data+24+chdr_len+j),
							    seq)
							   );
			}

			skb_queue_tail(&pt->batch, skb);
			seq = GFmul(seq, dchdr.seq_num);
		}

		memcpy(skb_put(dcp, data_len), pureData, data_len);
		kfree(pureData);

		skb_set_queue_mapping(dcp, 1);
		dcp->priority = 5;

		ieee80211_xmit(sdata, dcp);
	} else return false; 

	// add back to dc_schedule
	if(pt->tx_number >= RELAY_THRESH){
		pt->addr.prev = dc_schedule.prev;
		dc_schedule.prev->next = &pt->addr;
		pt->addr.next = &dc_schedule;
		dc_schedule.prev = &pt->addr;
	}

	return true;
}

/**
 * relay schedule the generation of double coded frame
 * modified by                                          James Tsunghsiao Pan (100062587)
 * purpose:                                             scheduling
 */

void relay_dc_gen(void)
{
	unsigned long data;

	spin_lock(&dcs_lock);
	while(dc_schedule.next != &dc_schedule){
		data = dc_schedule.next->data;
		dc_schedule.prev->prev->next = &dc_schedule;
		dc_schedule.prev = dc_schedule.prev->prev;
		if(double_coding_frame_gen((struct queue_ele *) data)) break;
	}
	spin_unlock(&dcs_lock);
}
EXPORT_SYMBOL(relay_dc_gen);

/**
 * timer expired Queue Process
 * Auther 						James Tsunghsiao Pan (100062587)
 * purpose: 						coding packet handling
 */
static ieee80211_rx_result debug_noinline
ieee80211_rx_h_coding(struct ieee80211_rx_data *rx)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct net_device *dev = sdata->dev;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)rx->skb->data, dhdr;
	struct coding_header *chdr = NULL;
	struct queue_ele *pt;
	bool exist, overhear = false;
	struct sk_buff *bAck, *dcp;
	struct ieee80211_hdr bAck_mhdr;
	struct batch_header bAck_bhdr;
	struct batch_header *bAck_hdr;
	int i = 0, ret;
	u8 addr[6];
#ifdef CONFIG_MAC80211_RELAY_FEEDBACK
	u8 a_relay[6];
#endif

	if (unlikely(!ieee80211_is_coding(hdr->frame_control)))
                return RX_CONTINUE;

	if(ieee80211_is_coding_dcrd(hdr->frame_control)){
		if(compare_ether_addr(sdata->vif.addr, hdr->addr1) != 0)
			return RX_DROP_UNUSABLE;

#ifdef CONFIG_MAC80211_RELAY_FEEDBACK
		// addr switch
		// addr1 = RELAY -> DST
		// addr2 = DST -> SRC
		// addr3 = BSSID 
		// addr4 = SRC -> RELAY
		memcpy(addr, hdr->addr2, 6);
		memcpy(hdr->addr2, hdr->addr4, 6);
		memcpy(hdr->addr4, hdr->addr1, 6);
		memcpy(hdr->addr1, addr, 6);
		goto find_queue;
#endif

		return RX_DROP_UNUSABLE;
	} else if(ieee80211_is_coding_doucoding(hdr->frame_control)) {
		if(compare_ether_addr(sdata->vif.addr, hdr->addr1) != 0)
			return RX_DROP_UNUSABLE;

		chdr = (struct coding_header *) (rx->skb->data + 30);
		// addr switch
		// addr1 = DST = this STA
		// addr2 = RELAY -> SRC
		// addr3 = BSSID
		// addr4 = SRC -> RELAY
		memcpy(addr, hdr->addr2, 6);
		memcpy(hdr->addr2, hdr->addr4, 6);
		memcpy(hdr->addr4, addr, 6);

		//return RX_DROP_UNUSABLE;
		goto find_queue;
	} else if(ieee80211_is_coding_batchack(hdr->frame_control)) {
		bAck_hdr = (struct batch_header *)(rx->skb->data + 24);

		if(compare_ether_addr(sdata->vif.addr, hdr->addr2) == 0)
			return RX_DROP_UNUSABLE;

		if(compare_ether_addr(sdata->vif.addr, hdr->addr1) != 0)
			overhear = true;
		
		if(overhear){
			for(pt = rxList; pt != NULL; pt = pt->next){
				if(!compare_ether_addr(hdr->addr2, pt->dst) && !compare_ether_addr(hdr->addr1, pt->src)){
					if((!pt->finished) && (bAck_hdr->batch_id == pt->id)){
						pt->finished = true;
						while(!skb_queue_empty(&pt->batch))
							skb_queue_purge(&pt->batch);
						pt->tx_number = 0;
#ifdef CONFIG_MAC80211_RELAY_FEEDBACK
#ifdef CONFIG_MAC80211_RELAY_FEEDBACK_MOD
						if(pt->id%FB_CYCLE != 0) goto skip1;
#endif
						pt->add_number = RELAY_BASIC;
						pt->inc_number = INT_WORK_RATE_INCR;
skip1:
#endif						
						
						spin_lock(&dcs_lock);
						if(pt->addr.prev != NULL && pt->addr.next != NULL){
							pt->addr.prev->next = pt->addr.next;
							pt->addr.next->prev = pt->addr.prev;
							pt->addr.prev = NULL;
							pt->addr.next = NULL;
						}
						spin_unlock(&dcs_lock);
					}
					
					break;
				}
			}

			return RX_DROP_UNUSABLE;
		}

		for(pt = qList; pt != NULL; pt = pt->next){
			if(!compare_ether_addr(hdr->addr2, pt->dst)){
				if(bAck_hdr->batch_id == pt->id) {
					if(!pt->finished){
						pt->finished = true;
						tx_finish_remove((unsigned long) pt);
					}
					
				}
 
				break;
			}
		}

                return RX_DROP_UNUSABLE;
	} else if(unlikely(!ieee80211_is_coding_coding(hdr->frame_control))) {
		printk(KERN_DEBUG "[mac80211] RX: undefined coding packet\n");
                return RX_DROP_UNUSABLE;
	}

	// coding packets
	chdr = (struct coding_header *) (rx->skb->data + 24);	

	if(compare_ether_addr(sdata->vif.addr, hdr->addr1) != 0) {
#ifdef CONFIG_MAC80211_RELAY_DISABLE
		return RX_DROP_UNUSABLE;
#endif
		for(i = 0; i < chdr->more_data_len; i++)
			if(!compare_ether_addr(sdata->vif.addr, (u8 *) (rx->skb->data + 28 + i*6))){
				overhear = true;
				break;
			}

		if(!overhear) return RX_DROP_UNUSABLE;
	}
	
find_queue:
	pt = NULL;
	exist = false;
	
	// Stroe
	if(rxList == NULL) {
#ifdef CONFIG_MAC80211_RELAY_DISABLE
		if(unlikely(ieee80211_is_coding_dcrd(hdr->frame_control)))
			return RX_DROP_UNUSABLE;
#endif
		printk(KERN_DEBUG "[mac80211] RX: rx pair create(1)\n");
		rxList = (struct queue_ele *) kmalloc(sizeof(struct queue_ele), GFP_ATOMIC);
		if(!rxList){
			// protection mechanism 
			printk(KERN_ALERT "[mac80211] RX: rx batch pair create error! (1)\n");
			return RX_DROP_UNUSABLE;
		}
		rxList->next = NULL;
		memcpy(rxList->dst, rx->skb->data+4, 6);
		memcpy(rxList->src, rx->skb->data+10, 6);
		rxList->id = chdr->batch_id;
		pt = rxList;
		skb_queue_head_init(&pt->batch);
		skb_queue_head_init(&pt->qhead);
		pt->matrix = NULL;
		pt->finished = false;
		pt->tx_number = 0;
		pt->add_number = RELAY_BASIC;
		pt->inc_number = INT_WORK_RATE_INCR;
		get_random_bytes(&pt->seq, 1);
		if(unlikely(pt->seq == 0 || pt->seq == 1)) pt->seq = 2;
		init_timer(&pt->c_timer);
		init_timer(&pt->timer);
		pt->c_timer.function = &remove_queue_ele;
		pt->c_timer.data = ((unsigned long) pt);
		pt->addr.next = NULL;
		pt->addr.prev = NULL;
		pt->addr.data = ((unsigned long) pt);
		pt->sdata = sdata;
	} else {
		for(pt = rxList; pt != NULL; pt = pt->next){
			if(memcmp(&pt->dst, rx->skb->data+4, 6) == 0 
			   && memcmp(&pt->src, rx->skb->data+10, 6) == 0) {
				exist = true;
				break;
			}
		}
		
		if(exist == false){
#ifdef CONFIG_MAC80211_RELAY_FEEDBACK
			if(unlikely(ieee80211_is_coding_dcrd(hdr->frame_control)))
				return RX_DROP_UNUSABLE;
#endif

			printk(KERN_DEBUG "[mac80211] RX: rx pair create(2)\n");
			for(pt = rxList; pt->next != NULL; pt = pt->next);
			
			pt->next = (struct queue_ele *) kmalloc(sizeof(struct queue_ele), 
								GFP_ATOMIC);
			if(!pt->next){
				// protection mechanism
                        	printk(KERN_ALERT "[mac80211] RX: rx batch pair create error! (2)\n");
                        	return RX_DROP_UNUSABLE;
                	}
			pt = pt->next;
			pt->next = NULL;
			memcpy(pt->dst, rx->skb->data+4, 6);
			memcpy(pt->src, rx->skb->data+10, 6);
			skb_queue_head_init(&pt->batch);
			skb_queue_head_init(&pt->qhead);
			pt->matrix = NULL;
			pt->finished = false;
			pt->tx_number = 0;
			pt->add_number = RELAY_BASIC;
			pt->inc_number = INT_WORK_RATE_INCR;
			get_random_bytes(&pt->seq, 1);
			if(unlikely(pt->seq == 0 || pt->seq == 1)) pt->seq = 2;
			init_timer(&pt->c_timer);
			init_timer(&pt->timer);
			pt->c_timer.function = &remove_queue_ele;
			pt->c_timer.data = ((unsigned long) pt);
			pt->addr.next = NULL;
			pt->addr.prev = NULL;
			pt->addr.data = ((unsigned long) pt);
			pt->sdata = sdata;
		}
	}

#ifdef CONFIG_MAC80211_RELAY_FEEDBACK	
	if(ieee80211_is_coding_dcrd(hdr->frame_control)){
		if(pt->id == *((u8 *) (rx->skb->data+30))){
			pt->tx_number = 0;
			pt->add_number >>= 2;
			if(pt->add_number < MIN_WORK_RATE) pt->add_number = MIN_WORK_RATE;
			pt->inc_number >>= 1;
		} else printk(KERN_DEBUG "RX: FEEDBACK LATE %x", *((u8 *) (rx->skb->data+30)));
		
		return RX_DROP_UNUSABLE;
	}
#endif
	
	del_timer(&pt->c_timer);
	pt->c_timer.expires = jiffies + CLEANUP_THRESH*HZ;
	pt->c_timer.function = &remove_queue_ele;
	pt->c_timer.data = ((unsigned long) pt);
	add_timer(&pt->c_timer);

	// batch collecting control
	if((pt->id == chdr->batch_id) && (pt->finished)){
		if(overhear){
			// ACK src
			dhdr.frame_control = cpu_to_le16(IEEE80211_FTYPE_CODING|IEEE80211_STYPE_BATCH_ACK);
			memcpy(dhdr.addr1, pt->dst, ETH_ALEN);
			memcpy(dhdr.addr2, pt->src, ETH_ALEN);
			memcpy(dhdr.addr3, sdata->vif.bss_conf.bssid, ETH_ALEN);
			dhdr.duration_id = 0;
			dhdr.seq_ctrl = 0;
			
			dcp = alloc_skb(26, GFP_ATOMIC);
			memcpy(skb_put(dcp, 24), &dhdr, 24);
			memcpy(skb_put(dcp, 1), &(chdr->batch_id), 1);
			memcpy(skb_put(dcp, 1), &(chdr->batch_size), 1);
			skb_set_queue_mapping(dcp, 0);
			dcp->priority = 7;
	
			ieee80211_xmit(sdata, dcp);
			return RX_DROP_UNUSABLE;
		}

		if(pt->tx_number%3 != 0){
			pt->tx_number++;
			return RX_DROP_UNUSABLE;
		}

		goto reack;
	} else if((pt->matrix == NULL && !overhear) || (overhear && skb_queue_empty(&pt->batch))){
		// initialization for used queue
		pt->id = chdr->batch_id;
		pt->finished = false;
	} else if(pt->id != chdr->batch_id){
		// previous batch	
		if((pt->id - chdr->batch_id + 0xff)%0xff < 5)
			return RX_DROP_UNUSABLE;

		//free matrix
		if(pt->matrix != NULL){
			matrix_free(pt->matrix);
			pt->matrix = NULL;
		}

		while(!skb_queue_empty(&pt->batch))
			skb_queue_purge(&pt->batch);
		pt->tx_number = 0;
#ifdef CONFIG_MAC80211_RELAY_FEEDBACK
#ifdef CONFIG_MAC80211_RELAY_FEEDBACK_MOD
		if(pt->id%FB_CYCLE != 0) goto skip2;
#endif
		pt->add_number = RELAY_BASIC;
		pt->inc_number = INT_WORK_RATE_INCR;
skip2:
#endif
		pt->id = chdr->batch_id;
		pt->finished = false;
		
		spin_lock(&dcs_lock);
		if(pt->addr.prev != NULL && pt->addr.next != NULL){
			pt->addr.prev->next = pt->addr.next;
			pt->addr.next->prev = pt->addr.prev;
			pt->addr.prev = NULL;
			pt->addr.next = NULL;
		}
		spin_unlock(&dcs_lock);
	}

	// if matrix == NULL -> allocate
	if(unlikely(pt->matrix == NULL) && !overhear){
		pt->matrix = matrix_alloc(chdr->batch_size);
		matrix_init(pt->matrix);
		pt->tx_number = 0;
	}

	if(overhear){
		skb_queue_tail(&pt->batch, rx->skb);
		pt->tx_number += pt->add_number;
#ifdef CONFIG_MAC80211_RELAY_FEEDBACK
		pt->add_number += pt->inc_number;
		if(unlikely(pt->add_number > MAX_WORK_RATE)) 
			pt->add_number = MAX_WORK_RATE;
#endif

		// add to TX queue called by tasklet
		spin_lock(&dcs_lock);
		if(pt->tx_number >= RELAY_THRESH && pt->addr.prev == NULL && pt->addr.next == NULL){
			pt->addr.prev = dc_schedule.prev;
			dc_schedule.prev->next = &pt->addr;
			pt->addr.next = &dc_schedule;
			dc_schedule.prev = &pt->addr;
		}
		spin_unlock(&dcs_lock);

		return RX_QUEUED;
	}

#ifdef CONFIG_MAC80211_RELAY_FEEDBACK
	if(ieee80211_is_coding_doucoding(hdr->frame_control))
		memcpy(a_relay, hdr->addr4, 6);
#endif

	//Gauss Elimination
	ret = matrix_add(pt->matrix, chdr->seq_num, rx->skb);

#ifdef CONFIG_MAC80211_RELAY_FEEDBACK
	if(ret == DECODE_RD){
		dhdr.frame_control = cpu_to_le16(IEEE80211_FTYPE_CODING|IEEE80211_STYPE_DCODING_RD);
		memcpy(dhdr.addr1, a_relay, ETH_ALEN);
		memcpy(dhdr.addr2, sdata->vif.addr, ETH_ALEN);
		memcpy(dhdr.addr3, sdata->vif.bss_conf.bssid, ETH_ALEN);
		memcpy(dhdr.addr4, pt->src, ETH_ALEN);
		dhdr.duration_id = 0;
		dhdr.seq_ctrl = 0;
		
		dcp = alloc_skb(31, GFP_ATOMIC);
		memcpy(skb_put(dcp, 30), &dhdr, 30);
		memcpy(skb_put(dcp, 1), &(pt->id), 1);
		skb_set_queue_mapping(dcp, 0);
		dcp->priority = 7;

		printk(KERN_DEBUG "[mac80211] RD TX%x", pt->id);
		ieee80211_xmit(sdata, dcp);
	}
#endif
	if(ret != DECODE_OK) return RX_QUEUED;

reack:
	// ACK
	bAck = alloc_skb(26, GFP_ATOMIC);
	memcpy(bAck_mhdr.addr1, rx->skb->data+10, 6);
	memcpy(bAck_mhdr.addr2, rx->skb->data+4, 6);
	memcpy(bAck_mhdr.addr3, rx->skb->data+16, 6);
	bAck_mhdr.duration_id = 0;
	bAck_mhdr.seq_ctrl = 0;
	bAck_mhdr.frame_control = cpu_to_le16(IEEE80211_FTYPE_CODING | IEEE80211_STYPE_BATCH_ACK);
	memcpy(&bAck_bhdr.batch_id, &pt->id, 1);
	memcpy(&bAck_bhdr.batch_size, &chdr->batch_size, 1);
	
	memcpy(skb_put(bAck, 24), &bAck_mhdr, 24);
	memcpy(skb_put(bAck, 2), &bAck_bhdr, 2);

	// queue mapping 
	skb_set_queue_mapping(bAck, 0);
	bAck->priority = 7;

	ieee80211_xmit(sdata, bAck);
	pt->tx_number++;	

	printk(KERN_DEBUG "[mac80211] RX: batch ACK TXing...\n");
	if(unlikely(pt->finished)) return RX_DROP_UNUSABLE; // reACK process terminated
	pt->finished = true;

	memcpy(pt->matrix->rx, rx, sizeof(struct ieee80211_rx_data));
	pt->matrix->rx->skb = NULL;
	pt->matrix->dev = dev;
	

	spin_lock(&hList_lock);
	
	if(hLast == NULL){
		hList = pt->matrix;
		hLast = pt->matrix;

	} else {
		hLast->next = pt->matrix;
		hLast = hLast->next;
	}

	pt->matrix = NULL;

	spin_unlock(&hList_lock);
	if(likely(!wq_terminate)) queue_delayed_work(decode_wq, &decode_w, 0);

        return RX_QUEUED;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_data(struct ieee80211_rx_data *rx)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct ieee80211_local *local = rx->local;
	struct net_device *dev = sdata->dev;
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)rx->skb->data;
	__le16 fc = hdr->frame_control;
	bool port_control;
	int err;

	if (unlikely(!ieee80211_is_data(hdr->frame_control)))
		return RX_CONTINUE;

	if (unlikely(!ieee80211_is_data_present(hdr->frame_control)))
		return RX_DROP_MONITOR;

	/*
	 * Allow the cooked monitor interface of an AP to see 4-addr frames so
	 * that a 4-addr station can be detected and moved into a separate VLAN
	 */
	if (ieee80211_has_a4(hdr->frame_control) &&
	    sdata->vif.type == NL80211_IFTYPE_AP)
		return RX_DROP_MONITOR;
	
	err = __ieee80211_data_to_8023(rx, &port_control);
	if (unlikely(err))
		return RX_DROP_UNUSABLE;
	
	if (!ieee80211_frame_allowed(rx, fc))
		return RX_DROP_MONITOR;

	if (rx->sdata->vif.type == NL80211_IFTYPE_AP_VLAN &&
	    unlikely(port_control) && sdata->bss) {
		sdata = container_of(sdata->bss, struct ieee80211_sub_if_data,
				     u.ap);
		dev = sdata->dev;
		rx->sdata = sdata;
	}

	rx->skb->dev = dev;

	dev->stats.rx_packets++;
	dev->stats.rx_bytes += rx->skb->len;

	if (local->ps_sdata && local->hw.conf.dynamic_ps_timeout > 0 &&
	    !is_multicast_ether_addr(
		    ((struct ethhdr *)rx->skb->data)->h_dest) &&
	    (!local->scanning &&
	     !test_bit(SDATA_STATE_OFFCHANNEL, &sdata->state))) {
			mod_timer(&local->dynamic_ps_timer, jiffies +
			 msecs_to_jiffies(local->hw.conf.dynamic_ps_timeout));
	}

	ieee80211_deliver_skb(rx);

	return RX_QUEUED;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_ctrl(struct ieee80211_rx_data *rx)
{
	struct ieee80211_local *local = rx->local;
	struct ieee80211_hw *hw = &local->hw;
	struct sk_buff *skb = rx->skb;
	struct ieee80211_bar *bar = (struct ieee80211_bar *)skb->data;
	struct tid_ampdu_rx *tid_agg_rx;
	u16 start_seq_num;
	u16 tid;

	if (likely(!ieee80211_is_ctl(bar->frame_control)))
		return RX_CONTINUE;

	if (ieee80211_is_back_req(bar->frame_control)) {
		struct {
			__le16 control, start_seq_num;
		} __packed bar_data;

		if (!rx->sta)
			return RX_DROP_MONITOR;

		if (skb_copy_bits(skb, offsetof(struct ieee80211_bar, control),
				  &bar_data, sizeof(bar_data)))
			return RX_DROP_MONITOR;

		tid = le16_to_cpu(bar_data.control) >> 12;

		tid_agg_rx = rcu_dereference(rx->sta->ampdu_mlme.tid_rx[tid]);
		if (!tid_agg_rx)
			return RX_DROP_MONITOR;

		start_seq_num = le16_to_cpu(bar_data.start_seq_num) >> 4;

		/* reset session timer */
		if (tid_agg_rx->timeout)
			mod_timer(&tid_agg_rx->session_timer,
				  TU_TO_EXP_TIME(tid_agg_rx->timeout));

		spin_lock(&tid_agg_rx->reorder_lock);
		/* release stored frames up to start of BAR */
		ieee80211_release_reorder_frames(hw, tid_agg_rx, start_seq_num);
		spin_unlock(&tid_agg_rx->reorder_lock);

		kfree_skb(skb);
		return RX_QUEUED;
	}

	/*
	 * After this point, we only want management frames,
	 * so we can drop all remaining control frames to
	 * cooked monitor interfaces.
	 */
	return RX_DROP_MONITOR;
}

static void ieee80211_process_sa_query_req(struct ieee80211_sub_if_data *sdata,
					   struct ieee80211_mgmt *mgmt,
					   size_t len)
{
	struct ieee80211_local *local = sdata->local;
	struct sk_buff *skb;
	struct ieee80211_mgmt *resp;

	if (compare_ether_addr(mgmt->da, sdata->vif.addr) != 0) {
		/* Not to own unicast address */
		return;
	}

	if (compare_ether_addr(mgmt->sa, sdata->u.mgd.bssid) != 0 ||
	    compare_ether_addr(mgmt->bssid, sdata->u.mgd.bssid) != 0) {
		/* Not from the current AP or not associated yet. */
		return;
	}

	if (len < 24 + 1 + sizeof(resp->u.action.u.sa_query)) {
		/* Too short SA Query request frame */
		return;
	}

	skb = dev_alloc_skb(sizeof(*resp) + local->hw.extra_tx_headroom);
	if (skb == NULL)
		return;

	skb_reserve(skb, local->hw.extra_tx_headroom);
	resp = (struct ieee80211_mgmt *) skb_put(skb, 24);
	memset(resp, 0, 24);
	memcpy(resp->da, mgmt->sa, ETH_ALEN);
	memcpy(resp->sa, sdata->vif.addr, ETH_ALEN);
	memcpy(resp->bssid, sdata->u.mgd.bssid, ETH_ALEN);
	resp->frame_control = cpu_to_le16(IEEE80211_FTYPE_MGMT |
					  IEEE80211_STYPE_ACTION);
	skb_put(skb, 1 + sizeof(resp->u.action.u.sa_query));
	resp->u.action.category = WLAN_CATEGORY_SA_QUERY;
	resp->u.action.u.sa_query.action = WLAN_ACTION_SA_QUERY_RESPONSE;
	memcpy(resp->u.action.u.sa_query.trans_id,
	       mgmt->u.action.u.sa_query.trans_id,
	       WLAN_SA_QUERY_TR_ID_LEN);

	ieee80211_tx_skb(sdata, skb);
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_mgmt_check(struct ieee80211_rx_data *rx)
{
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *) rx->skb->data;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);

	/*
	 * From here on, look only at management frames.
	 * Data and control frames are already handled,
	 * and unknown (reserved) frames are useless.
	 */
	if (rx->skb->len < 24)
		return RX_DROP_MONITOR;

	if (!ieee80211_is_mgmt(mgmt->frame_control))
		return RX_DROP_MONITOR;

	if (!(status->rx_flags & IEEE80211_RX_RA_MATCH))
		return RX_DROP_MONITOR;

	if (ieee80211_drop_unencrypted_mgmt(rx))
		return RX_DROP_UNUSABLE;

	return RX_CONTINUE;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_action(struct ieee80211_rx_data *rx)
{
	struct ieee80211_local *local = rx->local;
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *) rx->skb->data;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);
	int len = rx->skb->len;

	if (!ieee80211_is_action(mgmt->frame_control))
		return RX_CONTINUE;

	/* drop too small frames */
	if (len < IEEE80211_MIN_ACTION_SIZE)
		return RX_DROP_UNUSABLE;

	if (!rx->sta && mgmt->u.action.category != WLAN_CATEGORY_PUBLIC)
		return RX_DROP_UNUSABLE;

	if (!(status->rx_flags & IEEE80211_RX_RA_MATCH))
		return RX_DROP_UNUSABLE;

	switch (mgmt->u.action.category) {
	case WLAN_CATEGORY_BACK:
		/*
		 * The aggregation code is not prepared to handle
		 * anything but STA/AP due to the BSSID handling;
		 * IBSS could work in the code but isn't supported
		 * by drivers or the standard.
		 */
		if (sdata->vif.type != NL80211_IFTYPE_STATION &&
		    sdata->vif.type != NL80211_IFTYPE_AP_VLAN &&
		    sdata->vif.type != NL80211_IFTYPE_AP)
			break;

		/* verify action_code is present */
		if (len < IEEE80211_MIN_ACTION_SIZE + 1)
			break;

		switch (mgmt->u.action.u.addba_req.action_code) {
		case WLAN_ACTION_ADDBA_REQ:
			if (len < (IEEE80211_MIN_ACTION_SIZE +
				   sizeof(mgmt->u.action.u.addba_req)))
				goto invalid;
			break;
		case WLAN_ACTION_ADDBA_RESP:
			if (len < (IEEE80211_MIN_ACTION_SIZE +
				   sizeof(mgmt->u.action.u.addba_resp)))
				goto invalid;
			break;
		case WLAN_ACTION_DELBA:
			if (len < (IEEE80211_MIN_ACTION_SIZE +
				   sizeof(mgmt->u.action.u.delba)))
				goto invalid;
			break;
		default:
			goto invalid;
		}

		goto queue;
	case WLAN_CATEGORY_SPECTRUM_MGMT:
		if (local->hw.conf.channel->band != IEEE80211_BAND_5GHZ)
			break;

		if (sdata->vif.type != NL80211_IFTYPE_STATION)
			break;

		/* verify action_code is present */
		if (len < IEEE80211_MIN_ACTION_SIZE + 1)
			break;

		switch (mgmt->u.action.u.measurement.action_code) {
		case WLAN_ACTION_SPCT_MSR_REQ:
			if (len < (IEEE80211_MIN_ACTION_SIZE +
				   sizeof(mgmt->u.action.u.measurement)))
				break;
			ieee80211_process_measurement_req(sdata, mgmt, len);
			goto handled;
		case WLAN_ACTION_SPCT_CHL_SWITCH:
			if (len < (IEEE80211_MIN_ACTION_SIZE +
				   sizeof(mgmt->u.action.u.chan_switch)))
				break;

			if (sdata->vif.type != NL80211_IFTYPE_STATION)
				break;

			if (memcmp(mgmt->bssid, sdata->u.mgd.bssid, ETH_ALEN))
				break;

			goto queue;
		}
		break;
	case WLAN_CATEGORY_SA_QUERY:
		if (len < (IEEE80211_MIN_ACTION_SIZE +
			   sizeof(mgmt->u.action.u.sa_query)))
			break;

		switch (mgmt->u.action.u.sa_query.action) {
		case WLAN_ACTION_SA_QUERY_REQUEST:
			if (sdata->vif.type != NL80211_IFTYPE_STATION)
				break;
			ieee80211_process_sa_query_req(sdata, mgmt, len);
			goto handled;
		}
		break;
	case WLAN_CATEGORY_SELF_PROTECTED:
		switch (mgmt->u.action.u.self_prot.action_code) {
		case WLAN_SP_MESH_PEERING_OPEN:
		case WLAN_SP_MESH_PEERING_CLOSE:
		case WLAN_SP_MESH_PEERING_CONFIRM:
			if (!ieee80211_vif_is_mesh(&sdata->vif))
				goto invalid;
			if (sdata->u.mesh.security != IEEE80211_MESH_SEC_NONE)
				/* userspace handles this frame */
				break;
			goto queue;
		case WLAN_SP_MGK_INFORM:
		case WLAN_SP_MGK_ACK:
			if (!ieee80211_vif_is_mesh(&sdata->vif))
				goto invalid;
			break;
		}
		break;
	case WLAN_CATEGORY_MESH_ACTION:
		if (!ieee80211_vif_is_mesh(&sdata->vif))
			break;
		if (mesh_action_is_path_sel(mgmt) &&
		  (!mesh_path_sel_is_hwmp(sdata)))
			break;
		goto queue;
	}

	return RX_CONTINUE;

 invalid:
	status->rx_flags |= IEEE80211_RX_MALFORMED_ACTION_FRM;
	/* will return in the next handlers */
	return RX_CONTINUE;

 handled:
	if (rx->sta)
		rx->sta->rx_packets++;
	dev_kfree_skb(rx->skb);
	return RX_QUEUED;

 queue:
	rx->skb->pkt_type = IEEE80211_SDATA_QUEUE_TYPE_FRAME;
	skb_queue_tail(&sdata->skb_queue, rx->skb);
	ieee80211_queue_work(&local->hw, &sdata->work);
	if (rx->sta)
		rx->sta->rx_packets++;
	return RX_QUEUED;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_userspace_mgmt(struct ieee80211_rx_data *rx)
{
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);

	/* skip known-bad action frames and return them in the next handler */
	if (status->rx_flags & IEEE80211_RX_MALFORMED_ACTION_FRM)
		return RX_CONTINUE;

	/*
	 * Getting here means the kernel doesn't know how to handle
	 * it, but maybe userspace does ... include returned frames
	 * so userspace can register for those to know whether ones
	 * it transmitted were processed or returned.
	 */

	if (cfg80211_rx_mgmt(rx->sdata->dev, status->freq,
			     rx->skb->data, rx->skb->len,
			     GFP_ATOMIC)) {
		if (rx->sta)
			rx->sta->rx_packets++;
		dev_kfree_skb(rx->skb);
		return RX_QUEUED;
	}


	return RX_CONTINUE;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_action_return(struct ieee80211_rx_data *rx)
{
	struct ieee80211_local *local = rx->local;
	struct ieee80211_mgmt *mgmt = (struct ieee80211_mgmt *) rx->skb->data;
	struct sk_buff *nskb;
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(rx->skb);

	if (!ieee80211_is_action(mgmt->frame_control))
		return RX_CONTINUE;

	/*
	 * For AP mode, hostapd is responsible for handling any action
	 * frames that we didn't handle, including returning unknown
	 * ones. For all other modes we will return them to the sender,
	 * setting the 0x80 bit in the action category, as required by
	 * 802.11-2007 7.3.1.11.
	 * Newer versions of hostapd shall also use the management frame
	 * registration mechanisms, but older ones still use cooked
	 * monitor interfaces so push all frames there.
	 */
	if (!(status->rx_flags & IEEE80211_RX_MALFORMED_ACTION_FRM) &&
	    (sdata->vif.type == NL80211_IFTYPE_AP ||
	     sdata->vif.type == NL80211_IFTYPE_AP_VLAN))
		return RX_DROP_MONITOR;

	/* do not return rejected action frames */
	if (mgmt->u.action.category & 0x80)
		return RX_DROP_UNUSABLE;

	nskb = skb_copy_expand(rx->skb, local->hw.extra_tx_headroom, 0,
			       GFP_ATOMIC);
	if (nskb) {
		struct ieee80211_mgmt *nmgmt = (void *)nskb->data;

		nmgmt->u.action.category |= 0x80;
		memcpy(nmgmt->da, nmgmt->sa, ETH_ALEN);
		memcpy(nmgmt->sa, rx->sdata->vif.addr, ETH_ALEN);

		memset(nskb->cb, 0, sizeof(nskb->cb));

		ieee80211_tx_skb(rx->sdata, nskb);
	}
	dev_kfree_skb(rx->skb);
	return RX_QUEUED;
}

static ieee80211_rx_result debug_noinline
ieee80211_rx_h_mgmt(struct ieee80211_rx_data *rx)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	ieee80211_rx_result rxs;
	struct ieee80211_mgmt *mgmt = (void *)rx->skb->data;
	__le16 stype;

	rxs = ieee80211_work_rx_mgmt(rx->sdata, rx->skb);
	if (rxs != RX_CONTINUE)
		return rxs;

	stype = mgmt->frame_control & cpu_to_le16(IEEE80211_FCTL_STYPE);

	if (!ieee80211_vif_is_mesh(&sdata->vif) &&
	    sdata->vif.type != NL80211_IFTYPE_ADHOC &&
	    sdata->vif.type != NL80211_IFTYPE_STATION)
		return RX_DROP_MONITOR;

	switch (stype) {
	case cpu_to_le16(IEEE80211_STYPE_BEACON):
	case cpu_to_le16(IEEE80211_STYPE_PROBE_RESP):
		/* process for all: mesh, mlme, ibss */
		break;
	case cpu_to_le16(IEEE80211_STYPE_DEAUTH):
	case cpu_to_le16(IEEE80211_STYPE_DISASSOC):
		if (is_multicast_ether_addr(mgmt->da) &&
		    !is_broadcast_ether_addr(mgmt->da))
			return RX_DROP_MONITOR;

		/* process only for station */
		if (sdata->vif.type != NL80211_IFTYPE_STATION)
			return RX_DROP_MONITOR;
		break;
	case cpu_to_le16(IEEE80211_STYPE_PROBE_REQ):
	case cpu_to_le16(IEEE80211_STYPE_AUTH):
		/* process only for ibss */
		if (sdata->vif.type != NL80211_IFTYPE_ADHOC)
			return RX_DROP_MONITOR;
		break;
	default:
		return RX_DROP_MONITOR;
	}

	/* queue up frame and kick off work to process it */
	rx->skb->pkt_type = IEEE80211_SDATA_QUEUE_TYPE_FRAME;
	skb_queue_tail(&sdata->skb_queue, rx->skb);
	ieee80211_queue_work(&rx->local->hw, &sdata->work);
	if (rx->sta)
		rx->sta->rx_packets++;

	return RX_QUEUED;
}

/* TODO: use IEEE80211_RX_FRAGMENTED */
static void ieee80211_rx_cooked_monitor(struct ieee80211_rx_data *rx,
					struct ieee80211_rate *rate)
{
	struct ieee80211_sub_if_data *sdata;
	struct ieee80211_local *local = rx->local;
	struct ieee80211_rtap_hdr {
		struct ieee80211_radiotap_header hdr;
		u8 flags;
		u8 rate_or_pad;
		__le16 chan_freq;
		__le16 chan_flags;
	} __packed *rthdr;
	struct sk_buff *skb = rx->skb, *skb2;
	struct net_device *prev_dev = NULL;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);

	/*
	 * If cooked monitor has been processed already, then
	 * don't do it again. If not, set the flag.
	 */
	if (rx->flags & IEEE80211_RX_CMNTR)
		goto out_free_skb;
	rx->flags |= IEEE80211_RX_CMNTR;

	if (skb_headroom(skb) < sizeof(*rthdr) &&
	    pskb_expand_head(skb, sizeof(*rthdr), 0, GFP_ATOMIC))
		goto out_free_skb;

	rthdr = (void *)skb_push(skb, sizeof(*rthdr));
	memset(rthdr, 0, sizeof(*rthdr));
	rthdr->hdr.it_len = cpu_to_le16(sizeof(*rthdr));
	rthdr->hdr.it_present =
		cpu_to_le32((1 << IEEE80211_RADIOTAP_FLAGS) |
			    (1 << IEEE80211_RADIOTAP_CHANNEL));

	if (rate) {
		rthdr->rate_or_pad = rate->bitrate / 5;
		rthdr->hdr.it_present |=
			cpu_to_le32(1 << IEEE80211_RADIOTAP_RATE);
	}
	rthdr->chan_freq = cpu_to_le16(status->freq);

	if (status->band == IEEE80211_BAND_5GHZ)
		rthdr->chan_flags = cpu_to_le16(IEEE80211_CHAN_OFDM |
						IEEE80211_CHAN_5GHZ);
	else
		rthdr->chan_flags = cpu_to_le16(IEEE80211_CHAN_DYN |
						IEEE80211_CHAN_2GHZ);

	skb_set_mac_header(skb, 0);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->pkt_type = PACKET_OTHERHOST;
	skb->protocol = htons(ETH_P_802_2);

	list_for_each_entry_rcu(sdata, &local->interfaces, list) {
		if (!ieee80211_sdata_running(sdata))
			continue;

		if (sdata->vif.type != NL80211_IFTYPE_MONITOR ||
		    !(sdata->u.mntr_flags & MONITOR_FLAG_COOK_FRAMES))
			continue;

		if (prev_dev) {
			skb2 = skb_clone(skb, GFP_ATOMIC);
			if (skb2) {
				skb2->dev = prev_dev;
				netif_receive_skb(skb2);
			}
		}

		prev_dev = sdata->dev;
		sdata->dev->stats.rx_packets++;
		sdata->dev->stats.rx_bytes += skb->len;
	}

	if (prev_dev) {
		skb->dev = prev_dev;
		netif_receive_skb(skb);
		return;
	}

 out_free_skb:
	dev_kfree_skb(skb);
}

static void ieee80211_rx_handlers_result(struct ieee80211_rx_data *rx,
					 ieee80211_rx_result res)
{
	switch (res) {
	case RX_DROP_MONITOR:
		I802_DEBUG_INC(rx->sdata->local->rx_handlers_drop);
		if (rx->sta)
			rx->sta->rx_dropped++;
		/* fall through */
	case RX_CONTINUE: {
		struct ieee80211_rate *rate = NULL;
		struct ieee80211_supported_band *sband;
		struct ieee80211_rx_status *status;

		status = IEEE80211_SKB_RXCB((rx->skb));

		sband = rx->local->hw.wiphy->bands[status->band];
		if (!(status->flag & RX_FLAG_HT))
			rate = &sband->bitrates[status->rate_idx];

		ieee80211_rx_cooked_monitor(rx, rate);
		break;
		}
	case RX_DROP_UNUSABLE:
		I802_DEBUG_INC(rx->sdata->local->rx_handlers_drop);
		if (rx->sta)
			rx->sta->rx_dropped++;
		dev_kfree_skb(rx->skb);
		break;
	case RX_QUEUED:
		I802_DEBUG_INC(rx->sdata->local->rx_handlers_queued);
		break;
	}
}

static void ieee80211_rx_handlers(struct ieee80211_rx_data *rx)
{
	ieee80211_rx_result res = RX_DROP_MONITOR;
	struct sk_buff *skb;

#define CALL_RXH(rxh)			\
	do {				\
		res = rxh(rx);		\
		if (res != RX_CONTINUE)	\
			goto rxh_next;  \
	} while (0);

	spin_lock(&rx->local->rx_skb_queue.lock);
	if (rx->local->running_rx_handler)
		goto unlock;

	rx->local->running_rx_handler = true;

	while ((skb = __skb_dequeue(&rx->local->rx_skb_queue))) {
		spin_unlock(&rx->local->rx_skb_queue.lock);

		/*
		 * all the other fields are valid across frames
		 * that belong to an aMPDU since they are on the
		 * same TID from the same station
		 */
		rx->skb = skb;

		CALL_RXH(ieee80211_rx_h_decrypt)
		CALL_RXH(ieee80211_rx_h_check_more_data)
		CALL_RXH(ieee80211_rx_h_uapsd_and_pspoll)
		CALL_RXH(ieee80211_rx_h_sta_process)
		CALL_RXH(ieee80211_rx_h_defragment)
		CALL_RXH(ieee80211_rx_h_michael_mic_verify)
		/* must be after MMIC verify so header is counted in MPDU mic */
#ifdef CONFIG_MAC80211_MESH
		if (ieee80211_vif_is_mesh(&rx->sdata->vif))
			CALL_RXH(ieee80211_rx_h_mesh_fwding);
#endif
		CALL_RXH(ieee80211_rx_h_remove_qos_control)
		CALL_RXH(ieee80211_rx_h_amsdu)
		CALL_RXH(ieee80211_rx_h_coding)
		CALL_RXH(ieee80211_rx_h_data)
		CALL_RXH(ieee80211_rx_h_ctrl);
		CALL_RXH(ieee80211_rx_h_mgmt_check)
		CALL_RXH(ieee80211_rx_h_action)
		CALL_RXH(ieee80211_rx_h_userspace_mgmt)
		CALL_RXH(ieee80211_rx_h_action_return)
		CALL_RXH(ieee80211_rx_h_mgmt)

 rxh_next:
		ieee80211_rx_handlers_result(rx, res);
		spin_lock(&rx->local->rx_skb_queue.lock);
#undef CALL_RXH
	}

	rx->local->running_rx_handler = false;

 unlock:
	spin_unlock(&rx->local->rx_skb_queue.lock);
}

static void ieee80211_invoke_rx_handlers(struct ieee80211_rx_data *rx)
{
	ieee80211_rx_result res = RX_DROP_MONITOR;

#define CALL_RXH(rxh)			\
	do {				\
		res = rxh(rx);		\
		if (res != RX_CONTINUE)	\
			goto rxh_next;  \
	} while (0);

	CALL_RXH(ieee80211_rx_h_passive_scan)
	CALL_RXH(ieee80211_rx_h_check)

	ieee80211_rx_reorder_ampdu(rx);

	ieee80211_rx_handlers(rx);
	return;

 rxh_next:
	ieee80211_rx_handlers_result(rx, res);

#undef CALL_RXH
}

/*
 * This function makes calls into the RX path, therefore
 * it has to be invoked under RCU read lock.
 */
void ieee80211_release_reorder_timeout(struct sta_info *sta, int tid)
{
	struct ieee80211_rx_data rx = {
		.sta = sta,
		.sdata = sta->sdata,
		.local = sta->local,
		/* This is OK -- must be QoS data frame */
		.security_idx = tid,
		.seqno_idx = tid,
		.flags = 0,
	};
	struct tid_ampdu_rx *tid_agg_rx;

	tid_agg_rx = rcu_dereference(sta->ampdu_mlme.tid_rx[tid]);
	if (!tid_agg_rx)
		return;

	spin_lock(&tid_agg_rx->reorder_lock);
	ieee80211_sta_reorder_release(&sta->local->hw, tid_agg_rx);
	spin_unlock(&tid_agg_rx->reorder_lock);

	ieee80211_rx_handlers(&rx);
}

/* main receive path */

/**
 * prepare_for_handlers function
 * modified                                             James Tsunghsiao Pan (100062587)
 * purpose:                                             allow unicast coding packet overhearing 
 */
static int prepare_for_handlers(struct ieee80211_rx_data *rx,
				struct ieee80211_hdr *hdr)
{
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct sk_buff *skb = rx->skb;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	u8 *bssid = ieee80211_get_bssid(hdr, skb->len, sdata->vif.type);
	int multicast = is_multicast_ether_addr(hdr->addr1);

	switch (sdata->vif.type) {
	case NL80211_IFTYPE_STATION:
		if (!bssid && !sdata->u.mgd.use_4addr)
			return 0;
		if (!multicast &&
		    compare_ether_addr(sdata->vif.addr, hdr->addr1) != 0) {
			if (!(sdata->dev->flags & IFF_PROMISC) ||
			    sdata->u.mgd.use_4addr)
				return 0;
			status->rx_flags &= ~IEEE80211_RX_RA_MATCH;
		}
		break;
	case NL80211_IFTYPE_ADHOC:
		if (!bssid)
			return 0;
		if (ieee80211_is_beacon(hdr->frame_control)) {
			return 1;
		}
		else if (!ieee80211_bssid_match(bssid, sdata->u.ibss.bssid)) {
			if (!(status->rx_flags & IEEE80211_RX_IN_SCAN))
				return 0;
			status->rx_flags &= ~IEEE80211_RX_RA_MATCH;
		} else if (!multicast &&
			   compare_ether_addr(sdata->vif.addr,
					      hdr->addr1) != 0) {
			// overhear coding packet
			if(ieee80211_is_coding(hdr->frame_control))
				break;
			
			if (!(sdata->dev->flags & IFF_PROMISC))
				return 0;
			
			status->rx_flags &= ~IEEE80211_RX_RA_MATCH;
		} else if(ieee80211_is_coding_coding(hdr->frame_control)) {
			if(!rx->sta){
				int rate_idx;
				if(status->flag & RX_FLAG_HT) rate_idx = 0;
				else rate_idx = status->rate_idx;
				rx->sta = ieee80211_ibss_add_sta(sdata, bssid, 
						hdr->addr2, BIT(rate_idx), GFP_ATOMIC);
			}
		} else if (!rx->sta) {
			int rate_idx;
			if (status->flag & RX_FLAG_HT)
				rate_idx = 0; /* TODO: HT rates */
			else
				rate_idx = status->rate_idx;
			
			rx->sta = ieee80211_ibss_add_sta(sdata, bssid,
					hdr->addr2, BIT(rate_idx), GFP_ATOMIC);
		}
		break;
	case NL80211_IFTYPE_MESH_POINT:
		if (!multicast &&
		    compare_ether_addr(sdata->vif.addr,
				       hdr->addr1) != 0) {
			if (!(sdata->dev->flags & IFF_PROMISC))
				return 0;

			status->rx_flags &= ~IEEE80211_RX_RA_MATCH;
		}
		break;
	case NL80211_IFTYPE_AP_VLAN:
	case NL80211_IFTYPE_AP:
		if (!bssid) {
			if (compare_ether_addr(sdata->vif.addr,
					       hdr->addr1))
				return 0;
		} else if (!ieee80211_bssid_match(bssid,
					sdata->vif.addr)) {
			if (!(status->rx_flags & IEEE80211_RX_IN_SCAN) &&
			    !ieee80211_is_beacon(hdr->frame_control) &&
			    !(ieee80211_is_action(hdr->frame_control) &&
			      sdata->vif.p2p))
				return 0;
			status->rx_flags &= ~IEEE80211_RX_RA_MATCH;
		}
		break;
	case NL80211_IFTYPE_WDS:
		if (bssid || !ieee80211_is_data(hdr->frame_control))
			return 0;
		if (compare_ether_addr(sdata->u.wds.remote_addr, hdr->addr2))
			return 0;
		break;
	default:
		/* should never get here */
		WARN_ON(1);
		break;
	}

	return 1;
}

/*
 * This function returns whether or not the SKB
 * was destined for RX processing or not, which,
 * if consume is true, is equivalent to whether
 * or not the skb was consumed.
 */
static bool ieee80211_prepare_and_rx_handle(struct ieee80211_rx_data *rx,
					    struct sk_buff *skb, bool consume)
{
	struct ieee80211_local *local = rx->local;
	struct ieee80211_sub_if_data *sdata = rx->sdata;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_hdr *hdr = (void *)skb->data;
	int prepares;

	rx->skb = skb;
	status->rx_flags |= IEEE80211_RX_RA_MATCH;
	prepares = prepare_for_handlers(rx, hdr);

	if (!prepares)
		return false;

	if (!consume) {
		skb = skb_copy(skb, GFP_ATOMIC);
		if (!skb) {
			if (net_ratelimit())
				wiphy_debug(local->hw.wiphy,
					"failed to copy skb for %s\n",
					sdata->name);
			return true;
		}

		rx->skb = skb;
	}

	ieee80211_invoke_rx_handlers(rx);
	return true;
}

/*
 * This is the actual Rx frames handler. as it blongs to Rx path it must
 * be called with rcu_read_lock protection.
 */
/**
 * __ieee80211_rx_handle_packet
 * modified                                             James Tsunghsiao Pan (100062587)
 * purpose:                                             allow coding packet
 */
static void __ieee80211_rx_handle_packet(struct ieee80211_hw *hw,
					 struct sk_buff *skb)
{
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);
	struct ieee80211_local *local = hw_to_local(hw);
	struct ieee80211_sub_if_data *sdata;
	struct ieee80211_hdr *hdr;
	__le16 fc;
	struct ieee80211_rx_data rx;
	struct ieee80211_sub_if_data *prev;
	struct sta_info *sta, *tmp, *prev_sta;
	int err = 0;

	fc = ((struct ieee80211_hdr *)skb->data)->frame_control;
	memset(&rx, 0, sizeof(rx));
	rx.skb = skb;
	rx.local = local;

	if (ieee80211_is_data(fc) || ieee80211_is_mgmt(fc) || ieee80211_is_coding(fc))
		local->dot11ReceivedFragmentCount++;

	if (unlikely(test_bit(SCAN_HW_SCANNING, &local->scanning) ||
		     test_bit(SCAN_OFF_CHANNEL, &local->scanning)))
		status->rx_flags |= IEEE80211_RX_IN_SCAN;

	if (ieee80211_is_mgmt(fc))
		err = skb_linearize(skb);
	else
		err = !pskb_may_pull(skb, ieee80211_hdrlen(fc));

	if (err) {
		dev_kfree_skb(skb);
		return;
	}

	hdr = (struct ieee80211_hdr *)skb->data;
	ieee80211_parse_qos(&rx);
	ieee80211_verify_alignment(&rx);

	if (ieee80211_is_data(fc) || ieee80211_is_coding(fc)) {
		prev_sta = NULL;

		for_each_sta_info_rx(local, hdr->addr2, sta, tmp) {
			if (!prev_sta) {
				prev_sta = sta;
				continue;
			}

			rx.sta = prev_sta;
			rx.sdata = prev_sta->sdata;
			ieee80211_prepare_and_rx_handle(&rx, skb, false);

			prev_sta = sta;
		}

		if (prev_sta) {
			rx.sta = prev_sta;
			rx.sdata = prev_sta->sdata;

			if (ieee80211_prepare_and_rx_handle(&rx, skb, true))
				return;
			goto out;
		}
	}

	prev = NULL;

	list_for_each_entry_rcu(sdata, &local->interfaces, list) {
		if (!ieee80211_sdata_running(sdata))
			continue;

		if (sdata->vif.type == NL80211_IFTYPE_MONITOR ||
		    sdata->vif.type == NL80211_IFTYPE_AP_VLAN)
			continue;

		/*
		 * frame is destined for this interface, but if it's
		 * not also for the previous one we handle that after
		 * the loop to avoid copying the SKB once too much
		 */

		if (!prev) {
			prev = sdata;
			continue;
		}

		rx.sta = sta_info_get_bss_rx(prev, hdr->addr2);
		rx.sdata = prev;
		ieee80211_prepare_and_rx_handle(&rx, skb, false);

		prev = sdata;
	}

	if (prev) {
		rx.sta = sta_info_get_bss_rx(prev, hdr->addr2);
		rx.sdata = prev;

		if (ieee80211_prepare_and_rx_handle(&rx, skb, true))
			return;
	}

 out:
	dev_kfree_skb(skb);
}

/*
 * This is the receive path handler. It is called by a low level driver when an
 * 802.11 MPDU is received from the hardware.
 */
void ieee80211_rx(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct ieee80211_local *local = hw_to_local(hw);
	struct ieee80211_rate *rate = NULL;
	struct ieee80211_supported_band *sband;
	struct ieee80211_rx_status *status = IEEE80211_SKB_RXCB(skb);

	WARN_ON_ONCE(softirq_count() == 0);

	if (WARN_ON(status->band < 0 ||
		    status->band >= IEEE80211_NUM_BANDS))
		goto drop;

	sband = local->hw.wiphy->bands[status->band];
	if (WARN_ON(!sband))
		goto drop;

	/*
	 * If we're suspending, it is possible although not too likely
	 * that we'd be receiving frames after having already partially
	 * quiesced the stack. We can't process such frames then since
	 * that might, for example, cause stations to be added or other
	 * driver callbacks be invoked.
	 */
	if (unlikely(local->quiescing || local->suspended))
		goto drop;

	/*
	 * The same happens when we're not even started,
	 * but that's worth a warning.
	 */
	if (WARN_ON(!local->started))
		goto drop;

	if (likely(!(status->flag & RX_FLAG_FAILED_PLCP_CRC))) {
		/*
		 * Validate the rate, unless a PLCP error means that
		 * we probably can't have a valid rate here anyway.
		 */

		if (status->flag & RX_FLAG_HT) {
			/*
			 * rate_idx is MCS index, which can be [0-76]
			 * as documented on:
			 *
			 * http://wireless.kernel.org/en/developers/Documentation/ieee80211/802.11n
			 *
			 * Anything else would be some sort of driver or
			 * hardware error. The driver should catch hardware
			 * errors.
			 */
			if (WARN((status->rate_idx < 0 ||
				 status->rate_idx > 76),
				 "Rate marked as an HT rate but passed "
				 "status->rate_idx is not "
				 "an MCS index [0-76]: %d (0x%02x)\n",
				 status->rate_idx,
				 status->rate_idx))
				goto drop;
		} else {
			if (WARN_ON(status->rate_idx < 0 ||
				    status->rate_idx >= sband->n_bitrates))
				goto drop;
			rate = &sband->bitrates[status->rate_idx];
		}
	}

	status->rx_flags = 0;

	/*
	 * key references and virtual interfaces are protected using RCU
	 * and this requires that we are in a read-side RCU section during
	 * receive processing
	 */
	rcu_read_lock();

	/*
	 * Frames with failed FCS/PLCP checksum are not returned,
	 * all other frames are returned without radiotap header
	 * if it was previously present.
	 * Also, frames with less than 16 bytes are dropped.
	 */
	skb = ieee80211_rx_monitor(local, skb, rate);
	if (!skb) {
		rcu_read_unlock();
		return;
	}

	ieee80211_tpt_led_trig_rx(local,
			((struct ieee80211_hdr *)skb->data)->frame_control,
			skb->len);
	__ieee80211_rx_handle_packet(hw, skb);

	rcu_read_unlock();

	return;
 drop:
	kfree_skb(skb);
}
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,32))
EXPORT_SYMBOL(ieee80211_rx);
#else
EXPORT_SYMBOL(mac80211_ieee80211_rx);
#endif


/* This is a version of the rx handler that can be called from hard irq
 * context. Post the skb on the queue and schedule the tasklet */
void ieee80211_rx_irqsafe(struct ieee80211_hw *hw, struct sk_buff *skb)
{
	struct ieee80211_local *local = hw_to_local(hw);

	BUILD_BUG_ON(sizeof(struct ieee80211_rx_status) > sizeof(skb->cb));

	skb->pkt_type = IEEE80211_RX_MSG;
	skb_queue_tail(&local->skb_queue, skb);
	tasklet_schedule(&local->tasklet);
}
EXPORT_SYMBOL(ieee80211_rx_irqsafe);
