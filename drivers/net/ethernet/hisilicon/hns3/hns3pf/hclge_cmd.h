/*
 * Copyright (c) 2016~2017 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __HCLGE_CMD_H
#define __HCLGE_CMD_H
#include <linux/types.h>
#include <linux/io.h>

#define HCLGE_CMDQ_TX_TIMEOUT		1000

struct hclge_dev;
struct hclge_desc {
	__le16 opcode;

#define HCLGE_CMDQ_RX_INVLD_B		0
#define HCLGE_CMDQ_RX_OUTVLD_B		1

	__le16 flag;
	__le16 retval;
	__le16 rsv;
	__le32 data[6];
};

struct hclge_desc_cb {
	dma_addr_t dma;
	void *va;
	u32 length;
};

struct hclge_cmq_ring {
	dma_addr_t desc_dma_addr;
	struct hclge_desc *desc;
	struct hclge_desc_cb *desc_cb;
	struct hclge_dev  *dev;
	u32 head;
	u32 tail;

	u16 buf_size;
	u16 desc_num;
	int next_to_use;
	int next_to_clean;
	u8 flag;
	spinlock_t lock; /* Command queue lock */
};

enum hclge_cmd_return_status {
	HCLGE_CMD_EXEC_SUCCESS	= 0,
	HCLGE_CMD_NO_AUTH	= 1,
	HCLGE_CMD_NOT_SUPPORTED	= 2,
	HCLGE_CMD_QUEUE_FULL	= 3,
};

enum hclge_cmd_status {
	HCLGE_STATUS_SUCCESS	= 0,
	HCLGE_ERR_CSQ_FULL	= -1,
	HCLGE_ERR_CSQ_TIMEOUT	= -2,
	HCLGE_ERR_CSQ_ERROR	= -3,
};

struct hclge_cmq {
	struct hclge_cmq_ring csq;
	struct hclge_cmq_ring crq;
	u16 tx_timeout; /* Tx timeout */
	enum hclge_cmd_status last_status;
};

#define HCLGE_CMD_FLAG_IN_VALID_SHIFT	0
#define HCLGE_CMD_FLAG_OUT_VALID_SHIFT	1
#define HCLGE_CMD_FLAG_NEXT_SHIFT	2
#define HCLGE_CMD_FLAG_WR_OR_RD_SHIFT	3
#define HCLGE_CMD_FLAG_NO_INTR_SHIFT	4
#define HCLGE_CMD_FLAG_ERR_INTR_SHIFT	5

#define HCLGE_CMD_FLAG_IN	BIT(HCLGE_CMD_FLAG_IN_VALID_SHIFT)
#define HCLGE_CMD_FLAG_OUT	BIT(HCLGE_CMD_FLAG_OUT_VALID_SHIFT)
#define HCLGE_CMD_FLAG_NEXT	BIT(HCLGE_CMD_FLAG_NEXT_SHIFT)
#define HCLGE_CMD_FLAG_WR	BIT(HCLGE_CMD_FLAG_WR_OR_RD_SHIFT)
#define HCLGE_CMD_FLAG_NO_INTR	BIT(HCLGE_CMD_FLAG_NO_INTR_SHIFT)
#define HCLGE_CMD_FLAG_ERR_INTR	BIT(HCLGE_CMD_FLAG_ERR_INTR_SHIFT)

enum hclge_opcode_type {
	/* Generic command */
	HCLGE_OPC_QUERY_FW_VER		= 0x0001,
	HCLGE_OPC_CFG_RST_TRIGGER	= 0x0020,
	HCLGE_OPC_GBL_RST_STATUS	= 0x0021,
	HCLGE_OPC_QUERY_FUNC_STATUS	= 0x0022,
	HCLGE_OPC_QUERY_PF_RSRC		= 0x0023,
	HCLGE_OPC_QUERY_VF_RSRC		= 0x0024,
	HCLGE_OPC_GET_CFG_PARAM		= 0x0025,

	HCLGE_OPC_STATS_64_BIT		= 0x0030,
	HCLGE_OPC_STATS_32_BIT		= 0x0031,
	HCLGE_OPC_STATS_MAC		= 0x0032,
	/* Device management command */

	/* MAC commond */
	HCLGE_OPC_CONFIG_MAC_MODE	= 0x0301,
	HCLGE_OPC_CONFIG_AN_MODE	= 0x0304,
	HCLGE_OPC_QUERY_AN_RESULT	= 0x0306,
	HCLGE_OPC_QUERY_LINK_STATUS	= 0x0307,
	HCLGE_OPC_CONFIG_MAX_FRM_SIZE	= 0x0308,
	HCLGE_OPC_CONFIG_SPEED_DUP	= 0x0309,
	/* MACSEC command */

	/* PFC/Pause CMD*/
	HCLGE_OPC_CFG_MAC_PAUSE_EN      = 0x0701,
	HCLGE_OPC_CFG_PFC_PAUSE_EN      = 0x0702,
	HCLGE_OPC_CFG_MAC_PARA          = 0x0703,
	HCLGE_OPC_CFG_PFC_PARA          = 0x0704,
	HCLGE_OPC_QUERY_MAC_TX_PKT_CNT  = 0x0705,
	HCLGE_OPC_QUERY_MAC_RX_PKT_CNT  = 0x0706,
	HCLGE_OPC_QUERY_PFC_TX_PKT_CNT  = 0x0707,
	HCLGE_OPC_QUERY_PFC_RX_PKT_CNT  = 0x0708,
	HCLGE_OPC_PRI_TO_TC_MAPPING     = 0x0709,
	HCLGE_OPC_QOS_MAP               = 0x070A,

	/* ETS/scheduler commands */
	HCLGE_OPC_TM_PG_TO_PRI_LINK	= 0x0804,
	HCLGE_OPC_TM_QS_TO_PRI_LINK     = 0x0805,
	HCLGE_OPC_TM_NQ_TO_QS_LINK      = 0x0806,
	HCLGE_OPC_TM_RQ_TO_QS_LINK      = 0x0807,
	HCLGE_OPC_TM_PORT_WEIGHT        = 0x0808,
	HCLGE_OPC_TM_PG_WEIGHT          = 0x0809,
	HCLGE_OPC_TM_QS_WEIGHT          = 0x080A,
	HCLGE_OPC_TM_PRI_WEIGHT         = 0x080B,
	HCLGE_OPC_TM_PRI_C_SHAPPING     = 0x080C,
	HCLGE_OPC_TM_PRI_P_SHAPPING     = 0x080D,
	HCLGE_OPC_TM_PG_C_SHAPPING      = 0x080E,
	HCLGE_OPC_TM_PG_P_SHAPPING      = 0x080F,
	HCLGE_OPC_TM_PORT_SHAPPING      = 0x0810,
	HCLGE_OPC_TM_PG_SCH_MODE_CFG    = 0x0812,
	HCLGE_OPC_TM_PRI_SCH_MODE_CFG   = 0x0813,
	HCLGE_OPC_TM_QS_SCH_MODE_CFG    = 0x0814,
	HCLGE_OPC_TM_BP_TO_QSET_MAPPING = 0x0815,

	/* Packet buffer allocate command */
	HCLGE_OPC_TX_BUFF_ALLOC		= 0x0901,
	HCLGE_OPC_RX_PRIV_BUFF_ALLOC	= 0x0902,
	HCLGE_OPC_RX_PRIV_WL_ALLOC	= 0x0903,
	HCLGE_OPC_RX_COM_THRD_ALLOC	= 0x0904,
	HCLGE_OPC_RX_COM_WL_ALLOC	= 0x0905,
	HCLGE_OPC_RX_GBL_PKT_CNT	= 0x0906,

	/* PTP command */
	/* TQP management command */
	HCLGE_OPC_SET_TQP_MAP		= 0x0A01,

	/* TQP command */
	HCLGE_OPC_CFG_TX_QUEUE		= 0x0B01,
	HCLGE_OPC_QUERY_TX_POINTER	= 0x0B02,
	HCLGE_OPC_QUERY_TX_STATUS	= 0x0B03,
	HCLGE_OPC_CFG_RX_QUEUE		= 0x0B11,
	HCLGE_OPC_QUERY_RX_POINTER	= 0x0B12,
	HCLGE_OPC_QUERY_RX_STATUS	= 0x0B13,
	HCLGE_OPC_STASH_RX_QUEUE_LRO	= 0x0B16,
	HCLGE_OPC_CFG_RX_QUEUE_LRO	= 0x0B17,
	HCLGE_OPC_CFG_COM_TQP_QUEUE	= 0x0B20,
	HCLGE_OPC_RESET_TQP_QUEUE	= 0x0B22,

	/* TSO cmd */
	HCLGE_OPC_TSO_GENERIC_CONFIG	= 0x0C01,

	/* RSS cmd */
	HCLGE_OPC_RSS_GENERIC_CONFIG	= 0x0D01,
	HCLGE_OPC_RSS_INDIR_TABLE	= 0x0D07,
	HCLGE_OPC_RSS_TC_MODE		= 0x0D08,
	HCLGE_OPC_RSS_INPUT_TUPLE	= 0x0D02,

	/* Promisuous mode command */
	HCLGE_OPC_CFG_PROMISC_MODE	= 0x0E01,

	/* Interrupts cmd */
	HCLGE_OPC_ADD_RING_TO_VECTOR	= 0x1503,
	HCLGE_OPC_DEL_RING_TO_VECTOR	= 0x1504,

	/* MAC command */
	HCLGE_OPC_MAC_VLAN_ADD		    = 0x1000,
	HCLGE_OPC_MAC_VLAN_REMOVE	    = 0x1001,
	HCLGE_OPC_MAC_VLAN_TYPE_ID	    = 0x1002,
	HCLGE_OPC_MAC_VLAN_INSERT	    = 0x1003,
	HCLGE_OPC_MAC_ETHTYPE_ADD	    = 0x1010,
	HCLGE_OPC_MAC_ETHTYPE_REMOVE	= 0x1011,

	/* Multicast linear table cmd */
	HCLGE_OPC_MTA_MAC_MODE_CFG	    = 0x1020,
	HCLGE_OPC_MTA_MAC_FUNC_CFG	    = 0x1021,
	HCLGE_OPC_MTA_TBL_ITEM_CFG	    = 0x1022,
	HCLGE_OPC_MTA_TBL_ITEM_QUERY	= 0x1023,

	/* VLAN command */
	HCLGE_OPC_VLAN_FILTER_CTRL	    = 0x1100,
	HCLGE_OPC_VLAN_FILTER_PF_CFG	= 0x1101,
	HCLGE_OPC_VLAN_FILTER_VF_CFG	= 0x1102,

	/* MDIO command */
	HCLGE_OPC_MDIO_CONFIG		= 0x1900,

	/* QCN command */
	HCLGE_OPC_QCN_MOD_CFG		= 0x1A01,
	HCLGE_OPC_QCN_GRP_TMPLT_CFG	= 0x1A02,
	HCLGE_OPC_QCN_SHAPPING_IR_CFG	= 0x1A03,
	HCLGE_OPC_QCN_SHAPPING_BS_CFG	= 0x1A04,
	HCLGE_OPC_QCN_QSET_LINK_CFG	= 0x1A05,
	HCLGE_OPC_QCN_RP_STATUS_GET	= 0x1A06,
	HCLGE_OPC_QCN_AJUST_INIT	= 0x1A07,
	HCLGE_OPC_QCN_DFX_CNT_STATUS    = 0x1A08,

	/* Mailbox cmd */
	HCLGEVF_OPC_MBX_PF_TO_VF	= 0x2000,
};

#define HCLGE_TQP_REG_OFFSET		0x80000
#define HCLGE_TQP_REG_SIZE		0x200

#define HCLGE_RCB_INIT_QUERY_TIMEOUT	10
#define HCLGE_RCB_INIT_FLAG_EN_B	0
#define HCLGE_RCB_INIT_FLAG_FINI_B	8
struct hclge_config_rcb_init {
	__le16 rcb_init_flag;
	u8 rsv[22];
};

struct hclge_tqp_map {
	__le16 tqp_id;	/* Absolute tqp id for in this pf */
	u8 tqp_vf;	/* VF id */
#define HCLGE_TQP_MAP_TYPE_PF		0
#define HCLGE_TQP_MAP_TYPE_VF		1
#define HCLGE_TQP_MAP_TYPE_B		0
#define HCLGE_TQP_MAP_EN_B		1
	u8 tqp_flag;	/* Indicate it's pf or vf tqp */
	__le16 tqp_vid; /* Virtual id in this pf/vf */
	u8 rsv[18];
};

#define HCLGE_VECTOR_ELEMENTS_PER_CMD	10

enum hclge_int_type {
	HCLGE_INT_TX,
	HCLGE_INT_RX,
	HCLGE_INT_EVENT,
};

struct hclge_ctrl_vector_chain {
	u8 int_vector_id;
	u8 int_cause_num;
#define HCLGE_INT_TYPE_S	0
#define HCLGE_INT_TYPE_M	0x3
#define HCLGE_TQP_ID_S		2
#define HCLGE_TQP_ID_M		(0x7ff << HCLGE_TQP_ID_S)
#define HCLGE_INT_GL_IDX_S	13
#define HCLGE_INT_GL_IDX_M	(0x3 << HCLGE_INT_GL_IDX_S)
	__le16 tqp_type_and_id[HCLGE_VECTOR_ELEMENTS_PER_CMD];
	u8 vfid;
	u8 rsv;
};

#define HCLGE_TC_NUM		8
#define HCLGE_TC0_PRI_BUF_EN_B	15 /* Bit 15 indicate enable or not */
#define HCLGE_BUF_UNIT_S	7  /* Buf size is united by 128 bytes */
struct hclge_tx_buff_alloc {
	__le16 tx_pkt_buff[HCLGE_TC_NUM];
	u8 tx_buff_rsv[8];
};

struct hclge_rx_priv_buff {
	__le16 buf_num[HCLGE_TC_NUM];
	__le16 shared_buf;
	u8 rsv[6];
};

struct hclge_query_version {
	__le32 firmware;
	__le32 firmware_rsv[5];
};

#define HCLGE_RX_PRIV_EN_B	15
#define HCLGE_TC_NUM_ONE_DESC	4
struct hclge_priv_wl {
	__le16 high;
	__le16 low;
};

struct hclge_rx_priv_wl_buf {
	struct hclge_priv_wl tc_wl[HCLGE_TC_NUM_ONE_DESC];
};

struct hclge_rx_com_thrd {
	struct hclge_priv_wl com_thrd[HCLGE_TC_NUM_ONE_DESC];
};

struct hclge_rx_com_wl {
	struct hclge_priv_wl com_wl;
};

struct hclge_waterline {
	u32 low;
	u32 high;
};

struct hclge_tc_thrd {
	u32 low;
	u32 high;
};

struct hclge_priv_buf {
	struct hclge_waterline wl;	/* Waterline for low and high*/
	u32 buf_size;	/* TC private buffer size */
	u32 enable;	/* Enable TC private buffer or not */
};

#define HCLGE_MAX_TC_NUM	8
struct hclge_shared_buf {
	struct hclge_waterline self;
	struct hclge_tc_thrd tc_thrd[HCLGE_MAX_TC_NUM];
	u32 buf_size;
};

#define HCLGE_RX_COM_WL_EN_B	15
struct hclge_rx_com_wl_buf {
	__le16 high_wl;
	__le16 low_wl;
	u8 rsv[20];
};

#define HCLGE_RX_PKT_EN_B	15
struct hclge_rx_pkt_buf {
	__le16 high_pkt;
	__le16 low_pkt;
	u8 rsv[20];
};

#define HCLGE_PF_STATE_DONE_B	0
#define HCLGE_PF_STATE_MAIN_B	1
#define HCLGE_PF_STATE_BOND_B	2
#define HCLGE_PF_STATE_MAC_N_B	6
#define HCLGE_PF_MAC_NUM_MASK	0x3
#define HCLGE_PF_STATE_MAIN	BIT(HCLGE_PF_STATE_MAIN_B)
#define HCLGE_PF_STATE_DONE	BIT(HCLGE_PF_STATE_DONE_B)
struct hclge_func_status {
	__le32  vf_rst_state[4];
	u8 pf_state;
	u8 mac_id;
	u8 rsv1;
	u8 pf_cnt_in_mac;
	u8 pf_num;
	u8 vf_num;
	u8 rsv[2];
};

struct hclge_pf_res {
	__le16 tqp_num;
	__le16 buf_size;
	__le16 msixcap_localid_ba_nic;
	__le16 msixcap_localid_ba_rocee;
#define HCLGE_PF_VEC_NUM_S		0
#define HCLGE_PF_VEC_NUM_M		(0xff << HCLGE_PF_VEC_NUM_S)
	__le16 pf_intr_vector_number;
	__le16 pf_own_fun_number;
	__le32 rsv[3];
};

#define HCLGE_CFG_OFFSET_S	0
#define HCLGE_CFG_OFFSET_M	0xfffff /* Byte (8-10.3) */
#define HCLGE_CFG_RD_LEN_S	24
#define HCLGE_CFG_RD_LEN_M	(0xf << HCLGE_CFG_RD_LEN_S)
#define HCLGE_CFG_RD_LEN_BYTES	16
#define HCLGE_CFG_RD_LEN_UNIT	4

#define HCLGE_CFG_VMDQ_S	0
#define HCLGE_CFG_VMDQ_M	(0xff << HCLGE_CFG_VMDQ_S)
#define HCLGE_CFG_TC_NUM_S	8
#define HCLGE_CFG_TC_NUM_M	(0xff << HCLGE_CFG_TC_NUM_S)
#define HCLGE_CFG_TQP_DESC_N_S	16
#define HCLGE_CFG_TQP_DESC_N_M	(0xffff << HCLGE_CFG_TQP_DESC_N_S)
#define HCLGE_CFG_PHY_ADDR_S	0
#define HCLGE_CFG_PHY_ADDR_M	(0x1f << HCLGE_CFG_PHY_ADDR_S)
#define HCLGE_CFG_MEDIA_TP_S	8
#define HCLGE_CFG_MEDIA_TP_M	(0xff << HCLGE_CFG_MEDIA_TP_S)
#define HCLGE_CFG_RX_BUF_LEN_S	16
#define HCLGE_CFG_RX_BUF_LEN_M	(0xffff << HCLGE_CFG_RX_BUF_LEN_S)
#define HCLGE_CFG_MAC_ADDR_H_S	0
#define HCLGE_CFG_MAC_ADDR_H_M	(0xffff << HCLGE_CFG_MAC_ADDR_H_S)
#define HCLGE_CFG_DEFAULT_SPEED_S	16
#define HCLGE_CFG_DEFAULT_SPEED_M	(0xff << HCLGE_CFG_DEFAULT_SPEED_S)

struct hclge_cfg_param {
	__le32 offset;
	__le32 rsv;
	__le32 param[4];
};

#define HCLGE_MAC_MODE		0x0
#define HCLGE_DESC_NUM		0x40

#define HCLGE_ALLOC_VALID_B	0
struct hclge_vf_num {
	u8 alloc_valid;
	u8 rsv[23];
};

#define HCLGE_RSS_DEFAULT_OUTPORT_B	4
#define HCLGE_RSS_HASH_KEY_OFFSET_B	4
#define HCLGE_RSS_HASH_KEY_NUM		16
struct hclge_rss_config {
	u8 hash_config;
	u8 rsv[7];
	u8 hash_key[HCLGE_RSS_HASH_KEY_NUM];
};

struct hclge_rss_input_tuple {
	u8 ipv4_tcp_en;
	u8 ipv4_udp_en;
	u8 ipv4_sctp_en;
	u8 ipv4_fragment_en;
	u8 ipv6_tcp_en;
	u8 ipv6_udp_en;
	u8 ipv6_sctp_en;
	u8 ipv6_fragment_en;
	u8 rsv[16];
};

#define HCLGE_RSS_CFG_TBL_SIZE	16

struct hclge_rss_indirection_table {
	u16 start_table_index;
	u16 rss_set_bitmap;
	u8 rsv[4];
	u8 rss_result[HCLGE_RSS_CFG_TBL_SIZE];
};

#define HCLGE_RSS_TC_OFFSET_S		0
#define HCLGE_RSS_TC_OFFSET_M		(0x3ff << HCLGE_RSS_TC_OFFSET_S)
#define HCLGE_RSS_TC_SIZE_S		12
#define HCLGE_RSS_TC_SIZE_M		(0x7 << HCLGE_RSS_TC_SIZE_S)
#define HCLGE_RSS_TC_VALID_B		15
struct hclge_rss_tc_mode {
	u16 rss_tc_mode[HCLGE_MAX_TC_NUM];
	u8 rsv[8];
};

#define HCLGE_LINK_STS_B	0
#define HCLGE_LINK_STATUS	BIT(HCLGE_LINK_STS_B)
struct hclge_link_status {
	u8 status;
	u8 rsv[23];
};

struct hclge_promisc_param {
	u8 vf_id;
	u8 enable;
};

#define HCLGE_PROMISC_EN_B	1
#define HCLGE_PROMISC_EN_ALL	0x7
#define HCLGE_PROMISC_EN_UC	0x1
#define HCLGE_PROMISC_EN_MC	0x2
#define HCLGE_PROMISC_EN_BC	0x4
struct hclge_promisc_cfg {
	u8 flag;
	u8 vf_id;
	__le16 rsv0;
	u8 rsv1[20];
};

enum hclge_promisc_type {
	HCLGE_UNICAST	= 1,
	HCLGE_MULTICAST	= 2,
	HCLGE_BROADCAST	= 3,
};

#define HCLGE_MAC_TX_EN_B	6
#define HCLGE_MAC_RX_EN_B	7
#define HCLGE_MAC_PAD_TX_B	11
#define HCLGE_MAC_PAD_RX_B	12
#define HCLGE_MAC_1588_TX_B	13
#define HCLGE_MAC_1588_RX_B	14
#define HCLGE_MAC_APP_LP_B	15
#define HCLGE_MAC_LINE_LP_B	16
#define HCLGE_MAC_FCS_TX_B	17
#define HCLGE_MAC_RX_OVERSIZE_TRUNCATE_B	18
#define HCLGE_MAC_RX_FCS_STRIP_B	19
#define HCLGE_MAC_RX_FCS_B	20
#define HCLGE_MAC_TX_UNDER_MIN_ERR_B		21
#define HCLGE_MAC_TX_OVERSIZE_TRUNCATE_B	22

struct hclge_config_mac_mode {
	__le32 txrx_pad_fcs_loop_en;
	u8 rsv[20];
};

#define HCLGE_CFG_SPEED_S		0
#define HCLGE_CFG_SPEED_M		(0x3f << HCLGE_CFG_SPEED_S)

#define HCLGE_CFG_DUPLEX_B		7
#define HCLGE_CFG_DUPLEX_M		BIT(HCLGE_CFG_DUPLEX_B)

struct hclge_config_mac_speed_dup {
	u8 speed_dup;

#define HCLGE_CFG_MAC_SPEED_CHANGE_EN_B	0
	u8 mac_change_fec_en;
	u8 rsv[22];
};

#define HCLGE_QUERY_SPEED_S		3
#define HCLGE_QUERY_AN_B		0
#define HCLGE_QUERY_DUPLEX_B		2

#define HCLGE_QUERY_SPEED_M		(0x1f << HCLGE_QUERY_SPEED_S)
#define HCLGE_QUERY_AN_M		BIT(HCLGE_QUERY_AN_B)
#define HCLGE_QUERY_DUPLEX_M		BIT(HCLGE_QUERY_DUPLEX_B)

struct hclge_query_an_speed_dup {
	u8 an_syn_dup_speed;
	u8 pause;
	u8 rsv[23];
};

#define HCLGE_RING_ID_MASK		0x3ff
#define HCLGE_TQP_ENABLE_B		0

#define HCLGE_MAC_CFG_AN_EN_B		0
#define HCLGE_MAC_CFG_AN_INT_EN_B	1
#define HCLGE_MAC_CFG_AN_INT_MSK_B	2
#define HCLGE_MAC_CFG_AN_INT_CLR_B	3
#define HCLGE_MAC_CFG_AN_RST_B		4

#define HCLGE_MAC_CFG_AN_EN	BIT(HCLGE_MAC_CFG_AN_EN_B)

struct hclge_config_auto_neg {
	__le32  cfg_an_cmd_flag;
	u8      rsv[20];
};

#define HCLGE_MAC_MIN_MTU		64
#define HCLGE_MAC_MAX_MTU		9728
#define HCLGE_MAC_UPLINK_PORT		0x100

struct hclge_config_max_frm_size {
	__le16  max_frm_size;
	u8      rsv[22];
};

enum hclge_mac_vlan_tbl_opcode {
	HCLGE_MAC_VLAN_ADD,	/* Add new or modify mac_vlan */
	HCLGE_MAC_VLAN_UPDATE,  /* Modify other fields of this table */
	HCLGE_MAC_VLAN_REMOVE,  /* Remove a entry through mac_vlan key */
	HCLGE_MAC_VLAN_LKUP,    /* Lookup a entry through mac_vlan key */
};

#define HCLGE_MAC_VLAN_BIT0_EN_B	0x0
#define HCLGE_MAC_VLAN_BIT1_EN_B	0x1
#define HCLGE_MAC_EPORT_SW_EN_B		0xc
#define HCLGE_MAC_EPORT_TYPE_B		0xb
#define HCLGE_MAC_EPORT_VFID_S		0x3
#define HCLGE_MAC_EPORT_VFID_M		(0xff << HCLGE_MAC_EPORT_VFID_S)
#define HCLGE_MAC_EPORT_PFID_S		0x0
#define HCLGE_MAC_EPORT_PFID_M		(0x7 << HCLGE_MAC_EPORT_PFID_S)
struct hclge_mac_vlan_tbl_entry {
	u8	flags;
	u8      resp_code;
	__le16  vlan_tag;
	__le32  mac_addr_hi32;
	__le16  mac_addr_lo16;
	__le16  rsv1;
	u8      entry_type;
	u8      mc_mac_en;
	__le16  egress_port;
	__le16  egress_queue;
	u8      rsv2[6];
};

#define HCLGE_CFG_MTA_MAC_SEL_S		0x0
#define HCLGE_CFG_MTA_MAC_SEL_M		(0x3 << HCLGE_CFG_MTA_MAC_SEL_S)
#define HCLGE_CFG_MTA_MAC_EN_B		0x7
struct hclge_mta_filter_mode {
	u8	dmac_sel_en; /* Use lowest 2 bit as sel_mode, bit 7 as enable */
	u8      rsv[23];
};

#define HCLGE_CFG_FUNC_MTA_ACCEPT_B	0x0
struct hclge_cfg_func_mta_filter {
	u8	accept; /* Only used lowest 1 bit */
	u8      function_id;
	u8      rsv[22];
};

#define HCLGE_CFG_MTA_ITEM_ACCEPT_B	0x0
#define HCLGE_CFG_MTA_ITEM_IDX_S	0x0
#define HCLGE_CFG_MTA_ITEM_IDX_M	(0xfff << HCLGE_CFG_MTA_ITEM_IDX_S)
struct hclge_cfg_func_mta_item {
	u16	item_idx; /* Only used lowest 12 bit */
	u8      accept;   /* Only used lowest 1 bit */
	u8      rsv[21];
};

struct hclge_mac_vlan_add {
	__le16  flags;
	__le16  mac_addr_hi16;
	__le32  mac_addr_lo32;
	__le32  mac_addr_msk_hi32;
	__le16  mac_addr_msk_lo16;
	__le16  vlan_tag;
	__le16  ingress_port;
	__le16  egress_port;
	u8      rsv[4];
};

#define HNS3_MAC_VLAN_CFG_FLAG_BIT 0
struct hclge_mac_vlan_remove {
	__le16  flags;
	__le16  mac_addr_hi16;
	__le32  mac_addr_lo32;
	__le32  mac_addr_msk_hi32;
	__le16  mac_addr_msk_lo16;
	__le16  vlan_tag;
	__le16  ingress_port;
	__le16  egress_port;
	u8      rsv[4];
};

struct hclge_vlan_filter_ctrl {
	u8 vlan_type;
	u8 vlan_fe;
	u8 rsv[22];
};

struct hclge_vlan_filter_pf_cfg {
	u8 vlan_offset;
	u8 vlan_cfg;
	u8 rsv[2];
	u8 vlan_offset_bitmap[20];
};

struct hclge_vlan_filter_vf_cfg {
	u16 vlan_id;
	u8  resp_code;
	u8  rsv;
	u8  vlan_cfg;
	u8  rsv1[3];
	u8  vf_bitmap[16];
};

struct hclge_cfg_com_tqp_queue {
	__le16 tqp_id;
	__le16 stream_id;
	u8 enable;
	u8 rsv[19];
};

struct hclge_cfg_tx_queue_pointer {
	__le16 tqp_id;
	__le16 tx_tail;
	__le16 tx_head;
	__le16 fbd_num;
	__le16 ring_offset;
	u8 rsv[14];
};

#define HCLGE_TSO_MSS_MIN_S	0
#define HCLGE_TSO_MSS_MIN_M	(0x3FFF << HCLGE_TSO_MSS_MIN_S)

#define HCLGE_TSO_MSS_MAX_S	16
#define HCLGE_TSO_MSS_MAX_M	(0x3FFF << HCLGE_TSO_MSS_MAX_S)

struct hclge_cfg_tso_status {
	__le16 tso_mss_min;
	__le16 tso_mss_max;
	u8 rsv[20];
};

#define HCLGE_TSO_MSS_MIN	256
#define HCLGE_TSO_MSS_MAX	9668

#define HCLGE_TQP_RESET_B	0
struct hclge_reset_tqp_queue {
	__le16 tqp_id;
	u8 reset_req;
	u8 ready_to_reset;
	u8 rsv[20];
};

#define HCLGE_DEFAULT_TX_BUF		0x4000	 /* 16k  bytes */
#define HCLGE_TOTAL_PKT_BUF		0x108000 /* 1.03125M bytes */
#define HCLGE_DEFAULT_DV		0xA000	 /* 40k byte */
#define HCLGE_DEFAULT_NON_DCB_DV	0x7800	/* 30K byte */

#define HCLGE_TYPE_CRQ			0
#define HCLGE_TYPE_CSQ			1
#define HCLGE_NIC_CSQ_BASEADDR_L_REG	0x27000
#define HCLGE_NIC_CSQ_BASEADDR_H_REG	0x27004
#define HCLGE_NIC_CSQ_DEPTH_REG		0x27008
#define HCLGE_NIC_CSQ_TAIL_REG		0x27010
#define HCLGE_NIC_CSQ_HEAD_REG		0x27014
#define HCLGE_NIC_CRQ_BASEADDR_L_REG	0x27018
#define HCLGE_NIC_CRQ_BASEADDR_H_REG	0x2701c
#define HCLGE_NIC_CRQ_DEPTH_REG		0x27020
#define HCLGE_NIC_CRQ_TAIL_REG		0x27024
#define HCLGE_NIC_CRQ_HEAD_REG		0x27028
#define HCLGE_NIC_CMQ_EN_B		16
#define HCLGE_NIC_CMQ_ENABLE		BIT(HCLGE_NIC_CMQ_EN_B)
#define HCLGE_NIC_CMQ_DESC_NUM		1024
#define HCLGE_NIC_CMQ_DESC_NUM_S	3

int hclge_cmd_init(struct hclge_dev *hdev);
static inline void hclge_write_reg(void __iomem *base, u32 reg, u32 value)
{
	writel(value, base + reg);
}

#define hclge_write_dev(a, reg, value) \
	hclge_write_reg((a)->io_base, (reg), (value))
#define hclge_read_dev(a, reg) \
	hclge_read_reg((a)->io_base, (reg))

static inline u32 hclge_read_reg(u8 __iomem *base, u32 reg)
{
	u8 __iomem *reg_addr = READ_ONCE(base);

	return readl(reg_addr + reg);
}

#define HCLGE_SEND_SYNC(flag) \
	((flag) & HCLGE_CMD_FLAG_NO_INTR)

struct hclge_hw;
int hclge_cmd_send(struct hclge_hw *hw, struct hclge_desc *desc, int num);
void hclge_cmd_setup_basic_desc(struct hclge_desc *desc,
				enum hclge_opcode_type opcode, bool is_read);

int hclge_cmd_set_promisc_mode(struct hclge_dev *hdev,
			       struct hclge_promisc_param *param);

enum hclge_cmd_status hclge_cmd_mdio_write(struct hclge_hw *hw,
					   struct hclge_desc *desc);
enum hclge_cmd_status hclge_cmd_mdio_read(struct hclge_hw *hw,
					  struct hclge_desc *desc);

void hclge_destroy_cmd_queue(struct hclge_hw *hw);
#endif
