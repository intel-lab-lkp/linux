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
