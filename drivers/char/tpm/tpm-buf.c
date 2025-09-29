// SPDX-License-Identifier: GPL-2.0
/*
 * Handling of TPM command and other buffers.
 */

#include <linux/tpm_command.h>
#include <linux/module.h>
#include <linux/tpm.h>

static void __tpm_buf_size_invariant(struct tpm_buf *buf, u16 buf_size)
{
	u32 buf_size_2 = (u32)buf->capacity + (u32)sizeof(*buf);

	if (!buf->capacity) {
		if (buf_size > TPM_BUFSIZE) {
			WARN(1, "%s: size overflow: %u\n", __func__, buf_size);
			buf->flags |= TPM_BUF_INVALID;
		}
	} else {
		if (buf_size != buf_size_2) {
			WARN(1, "%s: size mismatch: %u != %u\n", __func__, buf_size,
			     buf_size_2);
			buf->flags |= TPM_BUF_INVALID;
		}
	}
}

static void __tpm_buf_reset(struct tpm_buf *buf, u16 buf_size, u16 tag, u32 ordinal)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;

	__tpm_buf_size_invariant(buf, buf_size);

	if (buf->flags & TPM_BUF_INVALID)
		return;

	WARN_ON(tag != TPM_TAG_RQU_COMMAND && tag != TPM2_ST_NO_SESSIONS &&
		tag != TPM2_ST_SESSIONS && tag != 0);

	buf->flags = 0;
	buf->length = sizeof(*head);
	buf->capacity = buf_size - sizeof(*buf);
	buf->handles = 0;
	head->tag = cpu_to_be16(tag);
	head->length = cpu_to_be32(sizeof(*head));
	head->ordinal = cpu_to_be32(ordinal);
}

static void __tpm_buf_reset_sized(struct tpm_buf *buf, u16 buf_size)
{
	__tpm_buf_size_invariant(buf, buf_size);

	if (buf->flags & TPM_BUF_INVALID)
		return;

	buf->flags = TPM_BUF_TPM2B;
	buf->length = 2;
	buf->capacity = buf_size - sizeof(*buf);
	buf->handles = 0;
	buf->data[0] = 0;
	buf->data[1] = 0;
}

/**
 * tpm_buf_init() - Initialize a TPM command
 * @buf:	A &tpm_buf
 * @buf_size:	Size of the buffer.
 */
void tpm_buf_init(struct tpm_buf *buf, u16 buf_size)
{
	memset(buf, 0, buf_size);
	__tpm_buf_reset(buf, buf_size, TPM_TAG_RQU_COMMAND, 0);
}
EXPORT_SYMBOL_GPL(tpm_buf_init);

/**
 * tpm_buf_init_sized() - Initialize a sized buffer
 * @buf:	A &tpm_buf
 * @buf_size:	Size of the buffer.
 */
void tpm_buf_init_sized(struct tpm_buf *buf, u16 buf_size)
{
	memset(buf, 0, buf_size);
	__tpm_buf_reset_sized(buf, buf_size);
}
EXPORT_SYMBOL_GPL(tpm_buf_init_sized);

/**
 * tpm_buf_reset() - Re-initialize a TPM command
 * @buf:	A &tpm_buf
 * @tag:	TPM_TAG_RQU_COMMAND, TPM2_ST_NO_SESSIONS or TPM2_ST_SESSIONS
 * @ordinal:	A command ordinal
 */
void tpm_buf_reset(struct tpm_buf *buf, u16 tag, u32 ordinal)
{
	u16 buf_size = buf->capacity + sizeof(*buf);

	__tpm_buf_reset(buf, buf_size, tag, ordinal);
}
EXPORT_SYMBOL_GPL(tpm_buf_reset);

/**
 * tpm_buf_reset_sized() - Re-initialize a sized buffer
 * @buf:	A &tpm_buf
 */
void tpm_buf_reset_sized(struct tpm_buf *buf)
{
	u16 buf_size = buf->capacity + sizeof(*buf);

	__tpm_buf_reset_sized(buf, buf_size);
}
EXPORT_SYMBOL_GPL(tpm_buf_reset_sized);

/**
 * tpm_buf_length() - Return the number of bytes consumed by the data
 * @buf:	A &tpm_buf
 *
 * Return: The number of bytes consumed by the buffer
 */
u32 tpm_buf_length(struct tpm_buf *buf)
{
	if (buf->flags & TPM_BUF_INVALID)
		return 0;

	return buf->length;
}
EXPORT_SYMBOL_GPL(tpm_buf_length);

/**
 * tpm_buf_append() - Append data to an initialized buffer
 * @buf:	A &tpm_buf
 * @new_data:	A data blob
 * @new_length:	Size of the appended data
 */
void tpm_buf_append(struct tpm_buf *buf, const u8 *new_data, u16 new_length)
{
	u32 total_length = (u32)buf->length + (u32)new_length;

	if (buf->flags & TPM_BUF_INVALID)
		return;

	if (total_length > (u32)buf->capacity) {
		WARN(1, "tpm_buf: write overflow\n");
		buf->flags |= TPM_BUF_INVALID;
		return;
	}

	memcpy(&buf->data[buf->length], new_data, new_length);
	buf->length += new_length;

	if (buf->flags & TPM_BUF_TPM2B)
		((__be16 *)buf->data)[0] = cpu_to_be16(buf->length - 2);
	else
		((struct tpm_header *)buf->data)->length = cpu_to_be32(buf->length);
}
EXPORT_SYMBOL_GPL(tpm_buf_append);

void tpm_buf_append_u8(struct tpm_buf *buf, const u8 value)
{
	tpm_buf_append(buf, &value, 1);
}
EXPORT_SYMBOL_GPL(tpm_buf_append_u8);

void tpm_buf_append_u16(struct tpm_buf *buf, const u16 value)
{
	__be16 value2 = cpu_to_be16(value);

	tpm_buf_append(buf, (u8 *)&value2, 2);
}
EXPORT_SYMBOL_GPL(tpm_buf_append_u16);

void tpm_buf_append_u32(struct tpm_buf *buf, const u32 value)
{
	__be32 value2 = cpu_to_be32(value);

	tpm_buf_append(buf, (u8 *)&value2, 4);
}
EXPORT_SYMBOL_GPL(tpm_buf_append_u32);

/**
 * tpm_buf_append_handle() - Add a handle
 * @buf:	&tpm_buf instance
 * @handle:	a TPM object handle
 *
 * Add a handle to the buffer, and increase the count tracking the number of
 * handles in the command buffer. Works only for command buffers.
 */
void tpm_buf_append_handle(struct tpm_buf *buf, u32 handle)
{
	if (buf->flags & TPM_BUF_INVALID)
		return;

	if (buf->flags & TPM_BUF_TPM2B) {
		WARN(1, "tpm-buf: invalid type: TPM2B\n");
		buf->flags |= TPM_BUF_INVALID;
		return;
	}

	tpm_buf_append_u32(buf, handle);
	buf->handles++;
}

/**
 * tpm_buf_read() - Read from a TPM buffer
 * @buf:	&tpm_buf instance
 * @offset:	offset within the buffer
 * @count:	the number of bytes to read
 * @output:	the output buffer
 */
static void tpm_buf_read(struct tpm_buf *buf, off_t *offset, size_t count, void *output)
{
	off_t next_offset;

	if (buf->flags & TPM_BUF_INVALID)
		return;

	next_offset = *offset + count;
	if (next_offset > buf->length) {
		WARN(1, "tpm_buf: read out of boundary\n");
		buf->flags |= TPM_BUF_INVALID;
		return;
	}

	memcpy(output, &buf->data[*offset], count);
	*offset = next_offset;
}

/**
 * tpm_buf_read_u8() - Read 8-bit word from a TPM buffer
 * @buf:	&tpm_buf instance
 * @offset:	offset within the buffer
 *
 * Return: next 8-bit word
 */
u8 tpm_buf_read_u8(struct tpm_buf *buf, off_t *offset)
{
	u8 value = 0;

	tpm_buf_read(buf, offset, sizeof(value), &value);

	return value;
}
EXPORT_SYMBOL_GPL(tpm_buf_read_u8);

/**
 * tpm_buf_read_u16() - Read 16-bit word from a TPM buffer
 * @buf:	&tpm_buf instance
 * @offset:	offset within the buffer
 *
 * Return: next 16-bit word
 */
u16 tpm_buf_read_u16(struct tpm_buf *buf, off_t *offset)
{
	u16 value = 0;

	tpm_buf_read(buf, offset, sizeof(value), &value);

	return be16_to_cpu(value);
}
EXPORT_SYMBOL_GPL(tpm_buf_read_u16);

/**
 * tpm_buf_read_u32() - Read 32-bit word from a TPM buffer
 * @buf:	&tpm_buf instance
 * @offset:	offset within the buffer
 *
 * Return: next 32-bit word
 */
u32 tpm_buf_read_u32(struct tpm_buf *buf, off_t *offset)
{
	u32 value = 0;

	tpm_buf_read(buf, offset, sizeof(value), &value);

	return be32_to_cpu(value);
}
EXPORT_SYMBOL_GPL(tpm_buf_read_u32);

static bool tpm1_buf_is_command(struct tpm_buf *buf, u32 ordinal)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;

	return !(buf->flags & TPM_BUF_TPM2B) &&
	       be16_to_cpu(head->tag) == TPM_TAG_RQU_COMMAND &&
	       be32_to_cpu(head->ordinal) == ordinal;
}

/**
 * tpm1_buf_append_extend() - Append command body for TPM_Extend
 * @buf:	&tpm_buf instance
 * @pcr_idx:	index of the PCR
 * @hash:	SHA1 hash
 */
void tpm1_buf_append_extend(struct tpm_buf *buf, u32 pcr_idx, const u8 *hash)
{
	if (buf->flags & TPM_BUF_INVALID)
		return;

	if (!tpm1_buf_is_command(buf, TPM_ORD_EXTEND)) {
		WARN(1, "tpm_buf: invalid TPM_Extend command\n");
		buf->flags |= TPM_BUF_INVALID;
		return;
	}

	tpm_buf_append_u32(buf, pcr_idx);
	tpm_buf_append(buf, hash, TPM_DIGEST_SIZE);
}

static bool tpm2_buf_is_command(struct tpm_buf *buf, u32 ordinal)
{
	struct tpm_header *head = (struct tpm_header *)buf->data;
	u16 tag = be16_to_cpu(head->tag);

	return !(buf->flags & TPM_BUF_TPM2B) &&
	       (tag == TPM2_ST_SESSIONS || tag == TPM2_ST_NO_SESSIONS) &&
	       be32_to_cpu(head->ordinal) == ordinal;
}

/**
 * tpm2_buf_append_pcr_extend() - Append command body for TPM2_PCR_Extend
 * @buf:	&tpm_buf instance
 * @digests:	list of PCR digests
 * @banks:	PCR bank descriptors
 * @nr_banks:	number of PCR banks
 */
void tpm2_buf_append_pcr_extend(struct tpm_buf *buf, struct tpm_digest *digests,
				struct tpm_bank_info *banks,
				unsigned int nr_banks)
{
	int i;

	if (buf->flags & TPM_BUF_INVALID)
		return;

	if (!tpm2_buf_is_command(buf, TPM2_CC_PCR_EXTEND)) {
		WARN(1, "tpm_buf: invalid TPM2_PCR_Extend command\n");
		buf->flags |= TPM_BUF_INVALID;
		return;
	}

	tpm_buf_append_u32(buf, nr_banks);
	for (i = 0; i < nr_banks; i++) {
		tpm_buf_append_u16(buf, digests[i].alg_id);
		tpm_buf_append(buf, digests[i].digest, banks[i].digest_size);
	}
}
