/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2013, 2016-2017 The Linux Foundation. All rights reserved.
 *
 * RMNET Data ingress/egress handler
 */

#ifndef _RMNET_HANDLERS_H_
#define _RMNET_HANDLERS_H_

#include "rmnet_config.h"

void rmnet_egress_handler(struct sk_buff *skb);

void rmnet_egress_handler(struct sk_buff *skb);
void rmnet_deliver_skb(struct sk_buff *skb, struct rmnet_port *port);
void rmnet_deliver_skb_wq(struct sk_buff *skb, struct rmnet_port *port,
			  enum rmnet_packet_context ctx);
void rmnet_set_skb_proto(struct sk_buff *skb);
rx_handler_result_t _rmnet_map_ingress_handler(struct sk_buff *skb,
					       struct rmnet_port *port);
rx_handler_result_t rmnet_rx_handler(struct sk_buff **pskb);

#endif /* _RMNET_HANDLERS_H_ */
