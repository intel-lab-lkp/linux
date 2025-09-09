/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025, Intel Corporation. */

#ifndef _VIRT_MIGRATION_TLV_H_
#define _VIRT_MIGRATION_TLV_H_

#include <linux/list.h>

/* The ice driver uses a series of TLVs to define the live migration data that
 * is passed between PFs during a migration event. This data includes all of
 * the information required to migrate the VM onto a new VF without loss of
 * data.
 *
 * On a migration event, the initial PF will scan its VF structures for
 * relevant information and serialize it into TLVs which are passed as part of
 * the binary migration data.
 *
 * The target PF will read the binary migration data and deserialize it using
 * the TLV definitions.
 *
 * The first TLV in the binary data *MUST* be ICE_MIG_TLV_HEADER, and defines
 * the overall migration version and format.
 *
 * A receiving PF should scan the set of provided TLVs, and ensure that it
 * recognizes all of the provided data. Once validated, the PF can apply the
 * configuration to the target VF, ensuring it is configured appropriately to
 * match the VM.
 */

#define ICE_MIG_MAGIC	0xE8000001
#define ICE_MIG_VERSION	1

#define ICE_MIG_VF_ITR_NUM	4

/**
 * struct ice_migration_tlv - TLV header structure
 * @type: type identifier for the data
 * @len: length of the data block
 * @data: migration data payload
 *
 * Migration data is serialized using this structure as a series of
 * type-length-value chunks. Each TLV is defined by its type. The length can
 * be used to move to the next TLV in the full data payload.
 *
 * The data payload structure is defined by the structure associated with the
 * type as defined by the following enumerations and structures.
 *
 * TLVs are placed within the binary migration payload sequentially, and are
 * __packed in order to avoid padding.
 *
 * Some of the TLVs are variable length, which could result in excessive
 * unaligned accesses. While the compiler should insert appropriate
 * instructions to handle this access due to the __packed attribute, we
 * enforce that all TLV headers begin at a 4-byte aligned boundary by padding
 * all TLV sizes to multiple of 4-bytes. This minimizes the amount of
 * unaligned access without sacrificing significant additional space.
 */
struct ice_migration_tlv {
	u16 type;
	u16 len;
	u8 data[] __counted_by(len);
} __packed;

/**
 * enum ice_migration_tlvs - Valid TLV types
 *
 * @ICE_MIG_TLV_END: Used to mark the end of the TLV list. The TLV header will
 * have a len of 0 and no data.
 *
 * @ICE_MIG_TLV_HEADER: Header identifying the migration format. Must be the
 * first TLV in the list.
 *
 * @ICE_MIG_TLV_VF_INFO: General configuration of the VF, including data
 * exchanged over virtchnl as well as PF host configuration.
 *
 * @ICE_MIG_TLV_TX_QUEUE: Configuration for a Tx queue. Appears once per Tx
 * queue.
 *
 * @ICE_MIG_TLV_RX_QUEUE: Configuration for an Rx queue. Appears once per Rx
 * queue.
 *
 * @ICE_MIG_TLV_MSIX_REGS: MSI-X register data for the VF. Appears once per
 * MSI-X interrupt, including the miscellaneous interrupt for the mailbox.
 *
 * @NUM_ICE_MIG_TLV: Number of known TLV types. Any type equal to or larger
 * than this value is unrecognized by this version.
 *
 * Enumeration of valid types for the virtualization migration data. The TLV
 * data is transferred between PFs, so this must be treated as ABI that can't
 * change.
 */
enum ice_migration_tlvs {
	/* Do not change the order or add anything between, this is ABI! */
	ICE_MIG_TLV_END = 0,
	ICE_MIG_TLV_HEADER,
	ICE_MIG_TLV_VF_INFO,
	ICE_MIG_TLV_TX_QUEUE,
	ICE_MIG_TLV_RX_QUEUE,
	ICE_MIG_TLV_MSIX_REGS,

	/* Add new types above here */
	NUM_ICE_MIG_TLV
};

/**
 * struct ice_mig_tlv_entry - Wrapper to store TLV entries in linked list
 * @list_entry: list node used for temporary storage prior to STOP_COPY
 * @tlv: The migration TLV data.
 *
 * Because ice_migration_tlv is a variable length structure, this is also
 * a variable length structure.
 */
struct ice_mig_tlv_entry {
	struct list_head list_entry;
	struct ice_migration_tlv tlv;
};

/**
 * struct ice_mig_tlv_header - Migration version header
 * @magic: Magic number identifying this migration format. Always 0xE8000001.
 * @version: Version of the migration format.
 * @num_supported_tlvs: The value of NUM_ICE_MIG_TLV for the sender.
 *
 * Structure defining the version of the migration data payload. A magic
 * number and version are used to identify this format. This is to potentially
 * allow changing or extending the format in the future in a way that the
 * receiving system can recognize.
 *
 * The num_supported_tlvs field is used to inform the receiver of the
 * supported set of TLVs being sent with this payload. This allows the
 * receiver to quickly identify if the payload may contain data it does not
 * recognize.
 */
struct ice_mig_tlv_header {
	u32 magic;
	u16 version;
	u16 num_supported_tlvs;
} __packed;

/**
 * struct ice_mig_vf_info - Basic VF information
 * @dev_lan_addr: The current device LAN address
 * @hw_lan_addr: The HW LAN address
 * @driver_caps: Driver capabilities reported by the VF
 * @vlan_v2_caps: The VLAN V2 capabilities of the VF
 * @vf_ver: The reported virtchnl version of the VF
 * @min_tx_rate: The programmed minimum Tx rate of the VF
 * @max_tx_rate: The programmed maximum Tx rate of the VF
 * @virtchnl_op_max: The largest known virtchnl opcode
 * @allowlist_size: The size of the opcodes_allowlist
 * @num_vf_qs: The number of queues assigned to the VF
 * @num_msix: The number of MSI-X vectors used by the VF
 * @port_vlan_tpid: port VLAN TPID
 * @port_vlan_vid: port VLAN VID
 * @port_vlan_prio: port VLAN priority
 * @inner_vlan_strip_ena: True if the inner VLAN stripping is enabled
 * @outer_vlan_strip_ena: True if the outer VLAN stripping is enabled
 * @pf_set_mac: True if the PF administratively set the MAC address
 * @trusted: True of the PF set the trusted VF flag for this VF
 * @spoofchk: True if spoof checking is enabled on this VF
 * @driver_active: True if the VF driver has initialized over virtchnl.
 * @link_forced: True if the link status of this VF is forced
 * @link_up: The forced link status, ignored if link_forced is false
 * @opcodes_allowlist: The list of currently allowed opcodes as array of u32
 */
struct ice_mig_vf_info {
	u8 dev_lan_addr[ETH_ALEN];
	u8 hw_lan_addr[ETH_ALEN];
	u32 driver_caps;
	struct virtchnl_vlan_caps vlan_v2_caps;
	struct virtchnl_version_info vf_ver;
	u32 min_tx_rate;
	u32 max_tx_rate;
	u32 virtchnl_op_max;
	u16 num_vf_qs;
	u16 num_msix;
	u16 port_vlan_tpid;
	u16 port_vlan_vid;
	u8 port_vlan_prio;
	u8 inner_vlan_strip_ena:1;
	u8 outer_vlan_strip_ena:1;
	u8 pf_set_mac:1;
	u8 trusted:1;
	u8 spoofchk:1;
	u8 driver_active:1;
	u8 link_forced:1;
	u8 link_up:1;			/* only valid if VF link is forced */
	u32 opcodes_allowlist[]; /* __counted_by(virtchnl_op_max), in bits */
} __packed;

/**
 * struct ice_mig_tx_queue - Data to migrate a VF Tx queue
 * @dma: the base DMA address for the queue
 * @count: size of the Tx ring
 * @head: the current head position of the Tx ring
 * @queue_id: the VF relative Tx queue ID
 * @vector_id: the VF relative MSI-X vector associated with this queue
 * @vector_valid: if true, an MSI-X vector is associated with this queue
 * @ena: if true, the Tx queue is currently enabled, false otherwise
 * @reserved: reservied bitfield which must be zero
 */
struct ice_mig_tx_queue {
	u64 dma;
	u16 count;
	u16 head;
	u16 queue_id;
	u16 vector_id;
	u8 vector_valid:1;
	u8 ena:1;
	u8 reserved:6;
} __packed;

/**
 * struct ice_mig_rx_queue - Data to migrate a VF Rx queue
 * @dma: the base DMA address for the queue
 * @max_frame: the maximum frame size of the queue
 * @rx_buf_len: the length of the Rx buffers associated with the ring
 * @rxdid: the Rx descriptor format of the ring
 * @count: the size of the Rx ring
 * @head: the current head position of the ring
 * @tail: the current tail position of the ring
 * @queue_id: the VF relative Rx queue ID
 * @vector_id: the VF relative MSI-X vector associated with this queue
 * @vector_valid: if true, an MSI-X vector is associated with this queue
 * @crc_strip: if true, CRC stripping is enabled, false otherwise
 * @ena: if true, the Rx queue is currently enabled, false otherwise
 * @reserved: reserved bitfield which must be zero
 */
struct ice_mig_rx_queue {
	u64 dma;
	u16 max_frame;
	u16 rx_buf_len;
	u32 rxdid;
	u16 count;
	u16 head;
	u16 tail;
	u16 queue_id;
	u16 vector_id;
	u8 vector_valid:1;
	u8 crc_strip:1;
	u8 ena:1;
	u8 reserved:5;
} __packed;

/**
 * struct ice_mig_msix_regs - MSI-X register data for migrating VF
 * @int_dyn_ctl: Contents GLINT_DYN_CTL for this vector
 * @int_intr: Contents of GLINT_ITR for all ITRs of this vector
 * @tx_itr_idx: The ITR index used for transmit
 * @rx_itr_idx: The ITR index used for receive
 * @vector_id: The MSI-X vector, *including* the miscellaneous non-queue vector
 */
struct ice_mig_msix_regs {
	u32 int_dyn_ctl;
	u32 int_intr[ICE_MIG_VF_ITR_NUM];
	u16 tx_itr_idx;
	u16 rx_itr_idx;
	u16 vector_id;
} __packed;

/**
 * ice_mig_tlv_type - Convert a TLV type to its number
 * @p: the TLV structure type
 *
 * Generic which converts the specified TLV structure type to its TLV numeric
 * value. Used to reduce potential error when initializing a TLV header for
 * the migration payload.
 */
#define ice_mig_tlv_type(p)						\
	_Generic(*(p),							\
		 struct ice_mig_tlv_header : ICE_MIG_TLV_HEADER,	\
		 struct ice_mig_vf_info : ICE_MIG_TLV_VF_INFO,		\
		 struct ice_mig_tx_queue : ICE_MIG_TLV_TX_QUEUE,	\
		 struct ice_mig_rx_queue : ICE_MIG_TLV_RX_QUEUE,	\
		 struct ice_mig_msix_regs : ICE_MIG_TLV_MSIX_REGS,	\
		 default : ICE_MIG_TLV_END)

/**
 * ice_mig_alloc_tlv - Allocate a non-variable length TLV entry
 * @p: pointer to the TLV element type
 *
 * Shorthand macro which allocates space for both a TLV header and the TLV
 * element structure. For variable-length TLVs with a flexible array member,
 * use ice_mig_alloc_flex_tlv instead.
 *
 * Because the allocations are ultimately triggered from userspace, and must
 * be held until userspace actually initiates the migration, allocate with
 * GFP_KERLEL_ACCOUNT, causing the allocations to be accounted by kmemcg.
 *
 * Returns: pointer to the allocated TLV element, or NULL on failure to
 * allocate.
 */
#define ice_mig_alloc_tlv(p)						\
	({								\
		struct ice_mig_tlv_entry *entry;			\
		typeof(p) __elem;					\
		size_t tlv_size;					\
									\
		tlv_size = ALIGN(sizeof(*__elem), 4);			\
		entry = kzalloc(struct_size(entry, tlv.data, tlv_size), \
				GFP_KERNEL_ACCOUNT);			\
		if (!entry) {						\
			__elem = NULL;					\
		} else {						\
			entry->tlv.type = ice_mig_tlv_type(__elem);	\
			entry->tlv.len = tlv_size;			\
			__elem = (typeof(__elem))entry->tlv.data;	\
		}							\
		__elem;							\
	})

/**
 * ice_mig_alloc_flex_tlv - Allocate a variable length TLV with flexible array
 * @p: pointer to the TLV element type
 * @member: flexible array member element
 * @count: number of elements in the flexible array.
 *
 * Shorthand macro which allocates space for both a TLV header and the TLV
 * element structure, and its variable length flexible array member.
 *
 * Because the allocations are ultimately triggered from userspace, and must
 * be held until userspace actually initiates the migration, allocate with
 * GFP_KERLEL_ACCOUNT, causing the allocations to be accounted by kmemcg.
 *
 * Returns: pointer to the allocated TLV element, or NULL on failure to
 * allocate.
 */
#define ice_mig_alloc_flex_tlv(p, member, count)			\
	({								\
		struct ice_mig_tlv_entry *entry;			\
		typeof(p) __elem;					\
		size_t tlv_size;					\
									\
		tlv_size = ALIGN(struct_size(__elem, member, count), 4);\
		entry = kzalloc(struct_size(entry, tlv.data, tlv_size),	\
				GFP_KERNEL_ACCOUNT);			\
		if (!entry) {						\
			__elem = NULL;					\
		} else {						\
			entry->tlv.type = ice_mig_tlv_type(__elem);	\
			entry->tlv.len = tlv_size;			\
			__elem = (typeof(__elem))entry->tlv.data;	\
		}							\
		__elem;							\
	})

/**
 * ice_mig_tlv_add_tail - Add TLV element to tail of a TLV list
 * @p: pointer to the TLV element
 * @head: pointer to the head of the linked list to insert into
 *
 * Shorthand macro to find the struct ice_mig_tlv_entry header pointer of the
 * given TLV element and insert it into the list.
 */
#define ice_mig_tlv_add_tail(p, head)					   \
	({								   \
		struct ice_mig_tlv_entry *entry;			   \
		entry = container_of((void *)p, typeof(*entry), tlv.data); \
		list_add_tail(&entry->list_entry, head);		   \
	})

#endif /* _VIRT_MIGRATION_TLV_H_ */
