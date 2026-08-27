/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef __UAPI_LINUX_BLK_CRYPTO_PROXY_H
#define __UAPI_LINUX_BLK_CRYPTO_PROXY_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define BCP_DIR_READ     0
#define BCP_DIR_WRITE    1

/*
 * BCP_BIND_CONTEXT - bind a host block device and hypervisor VM fd.
 *
 * Must be called once after open(), before any other ioctl.
 * Returns -EBUSY if already bound, -EOPNOTSUPP if no hypervisor op-set
 * is registered.
 *
 * @block_dev_fd: fd of the host block device to bind.
 * @vm_fd:        hypervisor VM fd identifying the guest.
 * @reserved:     must be zero.
 */
struct bcp_bind_context_arg {
	__s32 block_dev_fd;
	__s32 vm_fd;
	__u32 reserved;
};

/*
 * BCP_GET_CRYPTO_CAPS - query crypto capabilities of the bound block device.
 *
 * Requires BCP_BIND_CONTEXT; returns -ENXIO otherwise.
 *
 * @key_types_supported:  [out] BLK_CRYPTO_KEY_TYPE_* bitmask.
 * @max_dun_bytes:        [out] maximum DUN bytes supported.
 * @max_slots:            [out] maximum ICE keyslots available for the bound VM;
 *                              0 if the VM is not found in the table.
 * @num_modes:            [in] capacity of the buffer pointed to by @modes_ptr,
 *                              in entries. [out] number of entries actually
 *                              written to @modes_ptr (may be less than the
 *                              given capacity; the caller must use this
 *                              value, not its own capacity, to know how many
 *                              entries are valid).
 * @modes_ptr:             [in] pointer to a caller-allocated __u32 array of
 *                              at least @num_modes (as given) entries. Must
 *                              be non-NULL if @num_modes (as given) is > 0.
 *                              On return, holds a per-mode data_unit_size
 *                              bitmask array indexed by VIRTIO_BLK_CRYPTO_MODE_*
 *                              (virtio wire numbering, uapi/linux/virtio_blk.h)
 *                              -- NOT by enum blk_crypto_mode_num. Index 0 is
 *                              reserved and always 0, matching struct
 *                              virtio_blk_crypto_modes.modes[].
 *
 * @modes_ptr is a pointer + count rather than a fixed-size array embedded in
 * this struct so that sizeof(struct bcp_get_crypto_caps_arg) -- and hence the
 * _IOWR-encoded ioctl number -- does not depend on VIRTIO_BLK_CRYPTO_MODE_MAX.
 * The caller and this kernel may be built against different virtio_blk.h
 * versions (and thus different values of that constant); embedding a
 * VIRTIO_BLK_CRYPTO_MODE_MAX-sized array directly in this struct would make
 * the ioctl fail to even dispatch (-ENOTTY) whenever the two disagree.
 */

struct bcp_get_crypto_caps_arg {
	__u32 key_types_supported;
	__u32 max_dun_bytes;
	__u32 max_slots;
	__u32 num_modes;
	__aligned_u64 modes_ptr;
};

/*
 * BCP_SUBMIT_IO_BY_VSLOT - submit an encrypted bio using a virtual slot.
 *
 * The kernel resolves virt_slot to a physical ICE keyslot and submits the
 * I/O synchronously.  Large requests are split at data-unit boundaries
 * (BIO_MAX_VECS pages per bio).  Requires BCP_BIND_CONTEXT; returns -ENXIO
 * otherwise.
 *
 * @virt_slot:           guest-visible slot index (0-based within the VM's range).
 * @direction:           BCP_DIR_READ or BCP_DIR_WRITE.
 * @flags:               must be BCP_SUBMIT_IO_F_IOV.
 * @data_unit_size_bits: log2 of the encryption data unit size in bytes.
 * @sector:              start sector (512-byte units).
 * @dun:                 data unit number (single 64-bit limb, little-endian).
 * @iov_ptr:             pointer to scatter-gather array of struct bcp_iovec.
 * @iov_cnt:             number of entries in @iov_ptr[].
 * @reserved2:           must be zero.
 *
 * @sector, @dun and @iov_ptr use __aligned_u64 to guarantee identical struct
 * layout between 32-bit and 64-bit callers, as required by
 * .compat_ioctl = compat_ptr_ioctl.
 */

/* Maximum iovec segments per BCP_SUBMIT_IO_BY_VSLOT call (matches UIO_MAXIOV). */
#define BCP_MAX_IOV            1024

#define BCP_SUBMIT_IO_F_IOV    (1U << 0)	/* scatter-gather mode; must always be set */

struct bcp_iovec {
	__u64 iov_base;
	__u64 iov_len;
};

struct bcp_submit_io_by_vslot_arg {
	__u32 virt_slot;
	__u32 direction;
	__u32 flags;
	__u32 data_unit_size_bits;
	__aligned_u64 sector;
	__aligned_u64 dun;
	__aligned_u64 iov_ptr;
	__u32 iov_cnt;
	__u32 reserved2;
};

#define BCP_IOC_MAGIC    0xC7

#define BCP_BIND_CONTEXT       _IOW(BCP_IOC_MAGIC, 1, struct bcp_bind_context_arg)
#define BCP_GET_CRYPTO_CAPS    _IOWR(BCP_IOC_MAGIC, 2, struct bcp_get_crypto_caps_arg)
#define BCP_SUBMIT_IO_BY_VSLOT  _IOW(BCP_IOC_MAGIC, 3, struct bcp_submit_io_by_vslot_arg)

#endif /* __UAPI_LINUX_BLK_CRYPTO_PROXY_H */
