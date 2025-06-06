/* SPDX-License-Identifier: GPL-2.0-only */
/******************************************************************************
 *
 * Copyright(c) 2009 - 2014 Intel Corporation. All rights reserved.
 * Copyright(C) 2016        Intel Deutschland GmbH
 * Copyright(c) 2018, 2023, 2025 Intel Corporation
 *****************************************************************************/

#ifndef __IWLWIFI_DEVICE_TRACE
#define __IWLWIFI_DEVICE_TRACE
#include <linux/skbuff.h>
#include <linux/ieee80211.h>
#include <net/cfg80211.h>
#include <net/mac80211.h>
#include "iwl-trans.h"
static inline bool iwl_trace_data(struct sk_buff *skb)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct ieee80211_hdr *hdr = (void *)skb->data;
	__le16 fc = hdr->frame_control;
	int offs = 24; /* start with normal header length */

	if (!ieee80211_is_data(fc))
		return false;

	/* If upper layers wanted TX status it's an important frame */
	if (info->flags & IEEE80211_TX_CTL_REQ_TX_STATUS)
		return false;

	/* Try to determine if the frame is EAPOL. This might have false
	 * positives (if there's no RFC 1042 header and we compare to some
	 * payload instead) but since we're only doing tracing that's not
	 * a problem.
	 */

	if (ieee80211_has_a4(fc))
		offs += 6;
	if (ieee80211_is_data_qos(fc))
		offs += 2;
	/* don't account for crypto - these are unencrypted */

	/* also account for the RFC 1042 header, of course */
	offs += 6;

	return skb->len <= offs + 2 ||
		*(__be16 *)(skb->data + offs) != cpu_to_be16(ETH_P_PAE);
}

static inline size_t iwl_rx_trace_len(const struct iwl_trans *trans,
				      void *rxbuf, size_t len,
				      size_t *out_hdr_offset)
{
	struct iwl_cmd_header *cmd = (void *)((u8 *)rxbuf + sizeof(__le32));
	struct ieee80211_hdr *hdr = NULL;
	size_t hdr_offset;

	if (cmd->cmd != trans->conf.rx_mpdu_cmd)
		return len;

	hdr_offset = sizeof(struct iwl_cmd_header) +
		     trans->conf.rx_mpdu_cmd_hdr_size;

	if (out_hdr_offset)
		*out_hdr_offset = hdr_offset;

	hdr = (void *)((u8 *)cmd + hdr_offset);
	if (!ieee80211_is_data(hdr->frame_control))
		return len;
	/* maybe try to identify EAPOL frames? */
	return sizeof(__le32) + sizeof(*cmd) +
		trans->conf.rx_mpdu_cmd_hdr_size +
		ieee80211_hdrlen(hdr->frame_control);
}

#include <linux/tracepoint.h>
#include <linux/device.h>


#if !defined(CONFIG_IWLWIFI_DEVICE_TRACING) || defined(__CHECKER__)
#undef TRACE_EVENT
#define TRACE_EVENT(name, proto, ...) \
static inline void trace_ ## name(proto) {}
#undef DECLARE_EVENT_CLASS
#define DECLARE_EVENT_CLASS(...)
#undef DEFINE_EVENT
#define DEFINE_EVENT(evt_class, name, proto, ...) \
static inline void trace_ ## name(proto) {}
#endif

#define DEV_ENTRY	__string(dev, dev_name(dev))
#define DEV_ASSIGN	__assign_str(dev)

#include "iwl-devtrace-io.h"
#include "iwl-devtrace-ucode.h"
#include "iwl-devtrace-msg.h"
#include "iwl-devtrace-data.h"
#include "iwl-devtrace-iwlwifi.h"

#ifdef CONFIG_IWLWIFI_DEVICE_TRACING
DECLARE_TRACEPOINT(iwlwifi_dev_rx);
DECLARE_TRACEPOINT(iwlwifi_dev_rx_data);
#endif

void __trace_iwlwifi_dev_rx(struct iwl_trans *trans, void *pkt, size_t len);

static inline void maybe_trace_iwlwifi_dev_rx(struct iwl_trans *trans,
					      void *pkt, size_t len)
{
#ifdef CONFIG_IWLWIFI_DEVICE_TRACING
	if (tracepoint_enabled(iwlwifi_dev_rx) ||
	    tracepoint_enabled(iwlwifi_dev_rx_data))
		__trace_iwlwifi_dev_rx(trans, pkt, len);
#endif
}
#endif /* __IWLWIFI_DEVICE_TRACE */
