/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2013-2018, 2021, The Linux Foundation. All rights reserved.
 */

#ifndef _RMNET_MAP_H_
#define _RMNET_MAP_H_
#include <linux/if_rmnet.h>

#include <linux/skbuff.h>
#include "rmnet_config.h"

struct rmnet_map_control_command {
	u8  command_name;
	u8  cmd_type:2;
	u8  reserved:6;
	u16 reserved2;
	u32 transaction_id;
	union {
		struct {
			u16 ip_family:2;
			u16 reserved:14;
			__be16 flow_control_seq_num;
			__be32 qos_id;
		} flow_control;
		u8 data[0];
	};
}  __aligned(1);

enum rmnet_map_commands {
	RMNET_MAP_COMMAND_NONE,
	RMNET_MAP_COMMAND_FLOW_DISABLE,
	RMNET_MAP_COMMAND_FLOW_ENABLE,
	RMNET_MAP_COMMAND_FLOW_START = 7,
	RMNET_MAP_COMMAND_FLOW_END = 8,
	/* These should always be the last 2 elements */
	RMNET_MAP_COMMAND_UNKNOWN,
	RMNET_MAP_COMMAND_ENUM_LENGTH
};

#define RMNET_MAP_COMMAND_REQUEST     0
#define RMNET_MAP_COMMAND_ACK         1
#define RMNET_MAP_COMMAND_UNSUPPORTED 2
#define RMNET_MAP_COMMAND_INVALID     3

#define RMNET_MAP_NO_PAD_BYTES        0
#define RMNET_MAP_ADD_PAD_BYTES       1

struct sk_buff *rmnet_map_deaggregate(struct sk_buff *skb,
				      struct rmnet_port *port);
struct rmnet_map_header *rmnet_map_add_map_header(struct sk_buff *skb,
						  int hdrlen,
						  struct rmnet_port *port,
						  int pad);
void rmnet_map_command(struct sk_buff *skb, struct rmnet_port *port);
int rmnet_map_checksum_downlink_packet(struct sk_buff *skb, u16 len);
void rmnet_map_checksum_uplink_packet(struct sk_buff *skb,
				      struct rmnet_port *port,
				      struct net_device *orig_dev,
				      int csum_type);
int rmnet_map_process_next_hdr_packet(struct sk_buff *skb, u16 len);

	return skb->data;
}

static inline struct rmnet_map_control_command *
rmnet_map_get_cmd_start(struct sk_buff *skb)
{
	unsigned char *data = rmnet_map_data_ptr(skb);

	data += sizeof(struct rmnet_map_header);
	return (struct rmnet_map_control_command *)data;
}

static inline u8 rmnet_map_get_next_hdr_type(struct sk_buff *skb)
{
	unsigned char *data = rmnet_map_data_ptr(skb);

	data += sizeof(struct rmnet_map_header);
	return ((struct rmnet_map_v5_coal_header *)data)->header_type;
}

static inline bool rmnet_map_get_csum_valid(struct sk_buff *skb)
{
	unsigned char *data = rmnet_map_data_ptr(skb);

	data += sizeof(struct rmnet_map_header);
	return ((struct rmnet_map_v5_csum_header *)data)->csum_valid_required;
}

struct sk_buff *rmnet_map_deaggregate(struct sk_buff *skb,
				      struct rmnet_port *port);
struct rmnet_map_header *rmnet_map_add_map_header(struct sk_buff *skb,
						  int hdrlen, int pad,
						  struct rmnet_port *port);
void rmnet_map_command(struct sk_buff *skb, struct rmnet_port *port);
int rmnet_map_checksum_downlink_packet(struct sk_buff *skb, u16 len);
void rmnet_map_checksum_uplink_packet(struct sk_buff *skb,
				      struct rmnet_port *port,
				      struct net_device *orig_dev,
				      int csum_type);
bool rmnet_map_v5_csum_buggy(struct rmnet_map_v5_coal_header *coal_hdr);
int rmnet_map_process_next_hdr_packet(struct sk_buff *skb,
				      struct sk_buff_head *list,
				      u16 len);
int rmnet_map_tx_agg_skip(struct sk_buff *skb, int offset);
void rmnet_map_tx_aggregate(struct sk_buff *skb, struct rmnet_port *port);
void rmnet_map_tx_aggregate_init(struct rmnet_port *port);
void rmnet_map_tx_aggregate_exit(struct rmnet_port *port);
void rmnet_map_update_ul_agg_config(struct rmnet_port *port, u16 size,
				    u8 count, u8 features, u32 time);
void rmnet_map_dl_hdr_notify(struct rmnet_port *port,
			     struct rmnet_map_dl_ind_hdr *dl_hdr);
void rmnet_map_dl_hdr_notify_v2(struct rmnet_port *port,
				struct rmnet_map_dl_ind_hdr *dl_hdr,
				struct rmnet_map_control_command_header *qcmd);
void rmnet_map_dl_trl_notify(struct rmnet_port *port,
			     struct rmnet_map_dl_ind_trl *dltrl);
void rmnet_map_dl_trl_notify_v2(struct rmnet_port *port,
				struct rmnet_map_dl_ind_trl *dltrl,
				struct rmnet_map_control_command_header *qcmd);
int rmnet_map_flow_command(struct sk_buff *skb,
			   struct rmnet_port *port,
			   bool rmnet_perf);
void rmnet_map_cmd_init(struct rmnet_port *port);
int rmnet_map_dl_ind_register(struct rmnet_port *port,
			      struct rmnet_map_dl_ind *dl_ind);
int rmnet_map_dl_ind_deregister(struct rmnet_port *port,
				struct rmnet_map_dl_ind *dl_ind);
void rmnet_map_cmd_exit(struct rmnet_port *port);
#endif /* _RMNET_MAP_H_ */
