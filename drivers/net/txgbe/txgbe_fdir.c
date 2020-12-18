/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2015-2020
 */

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>


#include "txgbe_logs.h"
#include "base/txgbe.h"
#include "txgbe_ethdev.h"

#define TXGBE_DEFAULT_FLEXBYTES_OFFSET  12 /*default flexbytes offset in bytes*/
#define TXGBE_MAX_FLX_SOURCE_OFF        62

#define IPV6_ADDR_TO_MASK(ipaddr, ipv6m) do { \
	uint8_t ipv6_addr[16]; \
	uint8_t i; \
	rte_memcpy(ipv6_addr, (ipaddr), sizeof(ipv6_addr));\
	(ipv6m) = 0; \
	for (i = 0; i < sizeof(ipv6_addr); i++) { \
		if (ipv6_addr[i] == UINT8_MAX) \
			(ipv6m) |= 1 << i; \
		else if (ipv6_addr[i] != 0) { \
			PMD_DRV_LOG(ERR, " invalid IPv6 address mask."); \
			return -EINVAL; \
		} \
	} \
} while (0)

#define IPV6_MASK_TO_ADDR(ipv6m, ipaddr) do { \
	uint8_t ipv6_addr[16]; \
	uint8_t i; \
	for (i = 0; i < sizeof(ipv6_addr); i++) { \
		if ((ipv6m) & (1 << i)) \
			ipv6_addr[i] = UINT8_MAX; \
		else \
			ipv6_addr[i] = 0; \
	} \
	rte_memcpy((ipaddr), ipv6_addr, sizeof(ipv6_addr));\
} while (0)

/**
 *  Initialize Flow Director control registers
 *  @hw: pointer to hardware structure
 *  @fdirctrl: value to write to flow director control register
 **/
static int
txgbe_fdir_enable(struct txgbe_hw *hw, uint32_t fdirctrl)
{
	int i;

	PMD_INIT_FUNC_TRACE();

	/* Prime the keys for hashing */
	wr32(hw, TXGBE_FDIRBKTHKEY, TXGBE_ATR_BUCKET_HASH_KEY);
	wr32(hw, TXGBE_FDIRSIGHKEY, TXGBE_ATR_SIGNATURE_HASH_KEY);

	/*
	 * Continue setup of fdirctrl register bits:
	 *  Set the maximum length per hash bucket to 0xA filters
	 *  Send interrupt when 64 filters are left
	 */
	fdirctrl |= TXGBE_FDIRCTL_MAXLEN(0xA) |
		    TXGBE_FDIRCTL_FULLTHR(4);

	/*
	 * Poll init-done after we write the register.  Estimated times:
	 *      10G: PBALLOC = 11b, timing is 60us
	 *       1G: PBALLOC = 11b, timing is 600us
	 *     100M: PBALLOC = 11b, timing is 6ms
	 *
	 *     Multiple these timings by 4 if under full Rx load
	 *
	 * So we'll poll for TXGBE_FDIR_INIT_DONE_POLL times, sleeping for
	 * 1 msec per poll time.  If we're at line rate and drop to 100M, then
	 * this might not finish in our poll time, but we can live with that
	 * for now.
	 */
	wr32(hw, TXGBE_FDIRCTL, fdirctrl);
	txgbe_flush(hw);
	for (i = 0; i < TXGBE_FDIR_INIT_DONE_POLL; i++) {
		if (rd32(hw, TXGBE_FDIRCTL) & TXGBE_FDIRCTL_INITDONE)
			break;
		msec_delay(1);
	}

	if (i >= TXGBE_FDIR_INIT_DONE_POLL) {
		PMD_INIT_LOG(ERR, "Flow Director poll time exceeded during enabling!");
		return -ETIMEDOUT;
	}
	return 0;
}

/*
 * Set appropriate bits in fdirctrl for: variable reporting levels, moving
 * flexbytes matching field, and drop queue (only for perfect matching mode).
 */
static inline int
configure_fdir_flags(const struct rte_fdir_conf *conf,
		     uint32_t *fdirctrl, uint32_t *flex)
{
	*fdirctrl = 0;
	*flex = 0;

	switch (conf->pballoc) {
	case RTE_FDIR_PBALLOC_64K:
		/* 8k - 1 signature filters */
		*fdirctrl |= TXGBE_FDIRCTL_BUF_64K;
		break;
	case RTE_FDIR_PBALLOC_128K:
		/* 16k - 1 signature filters */
		*fdirctrl |= TXGBE_FDIRCTL_BUF_128K;
		break;
	case RTE_FDIR_PBALLOC_256K:
		/* 32k - 1 signature filters */
		*fdirctrl |= TXGBE_FDIRCTL_BUF_256K;
		break;
	default:
		/* bad value */
		PMD_INIT_LOG(ERR, "Invalid fdir_conf->pballoc value");
		return -EINVAL;
	};

	/* status flags: write hash & swindex in the rx descriptor */
	switch (conf->status) {
	case RTE_FDIR_NO_REPORT_STATUS:
		/* do nothing, default mode */
		break;
	case RTE_FDIR_REPORT_STATUS:
		/* report status when the packet matches a fdir rule */
		*fdirctrl |= TXGBE_FDIRCTL_REPORT_MATCH;
		break;
	case RTE_FDIR_REPORT_STATUS_ALWAYS:
		/* always report status */
		*fdirctrl |= TXGBE_FDIRCTL_REPORT_ALWAYS;
		break;
	default:
		/* bad value */
		PMD_INIT_LOG(ERR, "Invalid fdir_conf->status value");
		return -EINVAL;
	};

	*flex |= TXGBE_FDIRFLEXCFG_BASE_MAC;
	*flex |= TXGBE_FDIRFLEXCFG_OFST(TXGBE_DEFAULT_FLEXBYTES_OFFSET / 2);

	switch (conf->mode) {
	case RTE_FDIR_MODE_SIGNATURE:
		break;
	case RTE_FDIR_MODE_PERFECT:
		*fdirctrl |= TXGBE_FDIRCTL_PERFECT;
		*fdirctrl |= TXGBE_FDIRCTL_DROPQP(conf->drop_queue);
		break;
	default:
		/* bad value */
		PMD_INIT_LOG(ERR, "Invalid fdir_conf->mode value");
		return -EINVAL;
	}

	return 0;
}

static inline uint32_t
reverse_fdir_bmks(uint16_t hi_dword, uint16_t lo_dword)
{
	uint32_t mask = hi_dword << 16;

	mask |= lo_dword;
	mask = ((mask & 0x55555555) << 1) | ((mask & 0xAAAAAAAA) >> 1);
	mask = ((mask & 0x33333333) << 2) | ((mask & 0xCCCCCCCC) >> 2);
	mask = ((mask & 0x0F0F0F0F) << 4) | ((mask & 0xF0F0F0F0) >> 4);
	return ((mask & 0x00FF00FF) << 8) | ((mask & 0xFF00FF00) >> 8);
}

int
txgbe_fdir_set_input_mask(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_hw_fdir_info *info = TXGBE_DEV_FDIR(dev);
	enum rte_fdir_mode mode = dev->data->dev_conf.fdir_conf.mode;
	/*
	 * mask VM pool and DIPv6 since there are currently not supported
	 * mask FLEX byte, it will be set in flex_conf
	 */
	uint32_t fdirm = TXGBE_FDIRMSK_POOL;
	uint32_t fdirtcpm;  /* TCP source and destination port masks. */
	uint32_t fdiripv6m; /* IPv6 source and destination masks. */

	PMD_INIT_FUNC_TRACE();

	if (mode != RTE_FDIR_MODE_SIGNATURE &&
	    mode != RTE_FDIR_MODE_PERFECT) {
		PMD_DRV_LOG(ERR, "Not supported fdir mode - %d!", mode);
		return -ENOTSUP;
	}

	/*
	 * Program the relevant mask registers.  If src/dst_port or src/dst_addr
	 * are zero, then assume a full mask for that field. Also assume that
	 * a VLAN of 0 is unspecified, so mask that out as well.  L4type
	 * cannot be masked out in this implementation.
	 */
	if (info->mask.dst_port_mask == 0 && info->mask.src_port_mask == 0) {
		/* use the L4 protocol mask for raw IPv4/IPv6 traffic */
		fdirm |= TXGBE_FDIRMSK_L4P;
	}

	/* TBD: don't support encapsulation yet */
	wr32(hw, TXGBE_FDIRMSK, fdirm);

	/* store the TCP/UDP port masks, bit reversed from port layout */
	fdirtcpm = reverse_fdir_bmks(rte_be_to_cpu_16(info->mask.dst_port_mask),
			rte_be_to_cpu_16(info->mask.src_port_mask));

	/* write all the same so that UDP, TCP and SCTP use the same mask
	 * (little-endian)
	 */
	wr32(hw, TXGBE_FDIRTCPMSK, ~fdirtcpm);
	wr32(hw, TXGBE_FDIRUDPMSK, ~fdirtcpm);
	wr32(hw, TXGBE_FDIRSCTPMSK, ~fdirtcpm);

	/* Store source and destination IPv4 masks (big-endian) */
	wr32(hw, TXGBE_FDIRSIP4MSK, ~info->mask.src_ipv4_mask);
	wr32(hw, TXGBE_FDIRDIP4MSK, ~info->mask.dst_ipv4_mask);

	if (mode == RTE_FDIR_MODE_SIGNATURE) {
		/*
		 * Store source and destination IPv6 masks (bit reversed)
		 */
		fdiripv6m = TXGBE_FDIRIP6MSK_DST(info->mask.dst_ipv6_mask) |
			    TXGBE_FDIRIP6MSK_SRC(info->mask.src_ipv6_mask);

		wr32(hw, TXGBE_FDIRIP6MSK, ~fdiripv6m);
	}

	return 0;
}

static int
txgbe_fdir_store_input_mask(struct rte_eth_dev *dev)
{
	struct rte_eth_fdir_masks *input_mask =
				&dev->data->dev_conf.fdir_conf.mask;
	enum rte_fdir_mode mode = dev->data->dev_conf.fdir_conf.mode;
	struct txgbe_hw_fdir_info *info = TXGBE_DEV_FDIR(dev);
	uint16_t dst_ipv6m = 0;
	uint16_t src_ipv6m = 0;

	if (mode != RTE_FDIR_MODE_SIGNATURE &&
	    mode != RTE_FDIR_MODE_PERFECT) {
		PMD_DRV_LOG(ERR, "Not supported fdir mode - %d!", mode);
		return -ENOTSUP;
	}

	memset(&info->mask, 0, sizeof(struct txgbe_hw_fdir_mask));
	info->mask.vlan_tci_mask = input_mask->vlan_tci_mask;
	info->mask.src_port_mask = input_mask->src_port_mask;
	info->mask.dst_port_mask = input_mask->dst_port_mask;
	info->mask.src_ipv4_mask = input_mask->ipv4_mask.src_ip;
	info->mask.dst_ipv4_mask = input_mask->ipv4_mask.dst_ip;
	IPV6_ADDR_TO_MASK(input_mask->ipv6_mask.src_ip, src_ipv6m);
	IPV6_ADDR_TO_MASK(input_mask->ipv6_mask.dst_ip, dst_ipv6m);
	info->mask.src_ipv6_mask = src_ipv6m;
	info->mask.dst_ipv6_mask = dst_ipv6m;

	return 0;
}

int
txgbe_fdir_set_flexbytes_offset(struct rte_eth_dev *dev,
				uint16_t offset)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	int i;

	for (i = 0; i < 64; i++) {
		uint32_t flexreg, flex;
		flexreg = rd32(hw, TXGBE_FDIRFLEXCFG(i / 4));
		flex = TXGBE_FDIRFLEXCFG_BASE_MAC;
		flex |= TXGBE_FDIRFLEXCFG_OFST(offset / 2);
		flexreg &= ~(TXGBE_FDIRFLEXCFG_ALL(~0UL, i % 4));
		flexreg |= TXGBE_FDIRFLEXCFG_ALL(flex, i % 4);
		wr32(hw, TXGBE_FDIRFLEXCFG(i / 4), flexreg);
	}

	txgbe_flush(hw);
	for (i = 0; i < TXGBE_FDIR_INIT_DONE_POLL; i++) {
		if (rd32(hw, TXGBE_FDIRCTL) &
			TXGBE_FDIRCTL_INITDONE)
			break;
		msec_delay(1);
	}
	return 0;
}

/*
 * txgbe_check_fdir_flex_conf -check if the flex payload and mask configuration
 * arguments are valid
 */
static int
txgbe_set_fdir_flex_conf(struct rte_eth_dev *dev, uint32_t flex)
{
	const struct rte_eth_fdir_flex_conf *conf =
				&dev->data->dev_conf.fdir_conf.flex_conf;
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	struct txgbe_hw_fdir_info *info = TXGBE_DEV_FDIR(dev);
	const struct rte_eth_flex_payload_cfg *flex_cfg;
	const struct rte_eth_fdir_flex_mask *flex_mask;
	uint16_t flexbytes = 0;
	uint16_t i;

	if (conf == NULL) {
		PMD_DRV_LOG(ERR, "NULL pointer.");
		return -EINVAL;
	}

	flex |= TXGBE_FDIRFLEXCFG_DIA;

	for (i = 0; i < conf->nb_payloads; i++) {
		flex_cfg = &conf->flex_set[i];
		if (flex_cfg->type != RTE_ETH_RAW_PAYLOAD) {
			PMD_DRV_LOG(ERR, "unsupported payload type.");
			return -EINVAL;
		}
		if (((flex_cfg->src_offset[0] & 0x1) == 0) &&
		    (flex_cfg->src_offset[1] == flex_cfg->src_offset[0] + 1) &&
		     flex_cfg->src_offset[0] <= TXGBE_MAX_FLX_SOURCE_OFF) {
			flex &= ~TXGBE_FDIRFLEXCFG_OFST_MASK;
			flex |=
			    TXGBE_FDIRFLEXCFG_OFST(flex_cfg->src_offset[0] / 2);
		} else {
			PMD_DRV_LOG(ERR, "invalid flexbytes arguments.");
			return -EINVAL;
		}
	}

	for (i = 0; i < conf->nb_flexmasks; i++) {
		flex_mask = &conf->flex_mask[i];
		if (flex_mask->flow_type != RTE_ETH_FLOW_UNKNOWN) {
			PMD_DRV_LOG(ERR, "flexmask should be set globally.");
			return -EINVAL;
		}
		flexbytes = (uint16_t)(((flex_mask->mask[1] << 8) & 0xFF00) |
					((flex_mask->mask[0]) & 0xFF));
		if (flexbytes == UINT16_MAX) {
			flex &= ~TXGBE_FDIRFLEXCFG_DIA;
		} else if (flexbytes != 0) {
		     /* TXGBE_FDIRFLEXCFG_DIA is set by default when set mask */
			PMD_DRV_LOG(ERR, " invalid flexbytes mask arguments.");
			return -EINVAL;
		}
	}

	info->mask.flex_bytes_mask = flexbytes ? UINT16_MAX : 0;
	info->flex_bytes_offset = (uint8_t)(TXGBD_FDIRFLEXCFG_OFST(flex) * 2);

	for (i = 0; i < 64; i++) {
		uint32_t flexreg;
		flexreg = rd32(hw, TXGBE_FDIRFLEXCFG(i / 4));
		flexreg &= ~(TXGBE_FDIRFLEXCFG_ALL(~0UL, i % 4));
		flexreg |= TXGBE_FDIRFLEXCFG_ALL(flex, i % 4);
		wr32(hw, TXGBE_FDIRFLEXCFG(i / 4), flexreg);
	}
	return 0;
}

int
txgbe_fdir_configure(struct rte_eth_dev *dev)
{
	struct txgbe_hw *hw = TXGBE_DEV_HW(dev);
	int err;
	uint32_t fdirctrl, flex, pbsize;
	int i;
	enum rte_fdir_mode mode = dev->data->dev_conf.fdir_conf.mode;

	PMD_INIT_FUNC_TRACE();

	/* supports mac-vlan and tunnel mode */
	if (mode != RTE_FDIR_MODE_SIGNATURE &&
	    mode != RTE_FDIR_MODE_PERFECT)
		return -ENOSYS;

	err = configure_fdir_flags(&dev->data->dev_conf.fdir_conf,
				   &fdirctrl, &flex);
	if (err)
		return err;

	/*
	 * Before enabling Flow Director, the Rx Packet Buffer size
	 * must be reduced.  The new value is the current size minus
	 * flow director memory usage size.
	 */
	pbsize = rd32(hw, TXGBE_PBRXSIZE(0));
	pbsize -= TXGBD_FDIRCTL_BUF_BYTE(fdirctrl);
	wr32(hw, TXGBE_PBRXSIZE(0), pbsize);

	/*
	 * The defaults in the HW for RX PB 1-7 are not zero and so should be
	 * initialized to zero for non DCB mode otherwise actual total RX PB
	 * would be bigger than programmed and filter space would run into
	 * the PB 0 region.
	 */
	for (i = 1; i < 8; i++)
		wr32(hw, TXGBE_PBRXSIZE(i), 0);

	err = txgbe_fdir_store_input_mask(dev);
	if (err < 0) {
		PMD_INIT_LOG(ERR, " Error on setting FD mask");
		return err;
	}

	err = txgbe_fdir_set_input_mask(dev);
	if (err < 0) {
		PMD_INIT_LOG(ERR, " Error on setting FD mask");
		return err;
	}

	err = txgbe_set_fdir_flex_conf(dev, flex);
	if (err < 0) {
		PMD_INIT_LOG(ERR, " Error on setting FD flexible arguments.");
		return err;
	}

	err = txgbe_fdir_enable(hw, fdirctrl);
	if (err < 0) {
		PMD_INIT_LOG(ERR, " Error on enabling FD.");
		return err;
	}
	return 0;
}
