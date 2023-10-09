// SPDX-License-Identifier: MIT
/*
 * SPDX-FileCopyrightText: 2021 Kyunghwan Kwon <k@mononn.com>
 */

#include <linux/cbor/base.h>
#include <linux/module.h>

#if !defined(assert)
#define assert(expr)
#endif

static size_t copy_le(uint8_t *dst, uint8_t const *src, size_t len)
{
	for (size_t i = 0; i < len; i++)
		dst[len - i - 1] = src[i];

	return len;
}

static size_t copy_be(uint8_t *dst, uint8_t const *src, size_t len)
{
	for (size_t i = 0; i < len; i++)
		dst[i] = src[i];

	return len;
}

size_t cbor_copy(uint8_t *dst, uint8_t const *src, size_t len)
{
#if defined(CBOR_BIG_ENDIAN)
	return copy_be(dst, src, len);
#else
	return copy_le(dst, src, len);
#endif
}
EXPORT_SYMBOL_GPL(cbor_copy);

size_t cbor_copy_be(uint8_t *dst, uint8_t const *src, size_t len)
{
	return copy_be(dst, src, len);
}
EXPORT_SYMBOL_GPL(cbor_copy_be);

uint8_t cbor_get_following_bytes(uint8_t additional_info)
{
	if (additional_info < 24)
		return 0;
	else if (additional_info == 31)
		return (uint8_t)CBOR_INDEFINITE_VALUE;
	else if (additional_info >= 28)
		return (uint8_t)CBOR_RESERVED_VALUE;

	return (uint8_t)(1u << (additional_info - 24));
}
EXPORT_SYMBOL_GPL(cbor_get_following_bytes);

cbor_item_data_t cbor_get_item_type(cbor_item_t const *item)
{
	return item->type;
}
EXPORT_SYMBOL_GPL(cbor_get_item_type);

size_t cbor_get_item_size(cbor_item_t const *item)
{
	return item->size;
}
EXPORT_SYMBOL_GPL(cbor_get_item_size);

void cbor_reader_init(cbor_reader_t *reader, cbor_item_t *items, size_t maxitems)
{
	assert(reader != NULL);

	reader->items = items;
	reader->maxitems = maxitems;
	reader->itemidx = 0;
}
EXPORT_SYMBOL_GPL(cbor_reader_init);

void cbor_writer_init(cbor_writer_t *writer, void *buf, size_t bufsize)
{
	assert(writer != NULL);
	assert(buf != NULL);

	writer->buf = (uint8_t *)buf;
	writer->bufsize = bufsize;
	writer->bufidx = 0;
}
EXPORT_SYMBOL_GPL(cbor_writer_init);

size_t cbor_writer_len(cbor_writer_t const *writer)
{
	return writer->bufidx;
}
EXPORT_SYMBOL_GPL(cbor_writer_len);

uint8_t const *cbor_writer_get_encoded(cbor_writer_t const *writer)
{
	return (uint8_t const *)writer->buf;
}
EXPORT_SYMBOL_GPL(cbor_writer_get_encoded);

MODULE_DESCRIPTION("CBOR helper functions");
MODULE_AUTHOR("Kyunghwan Kwon");
MODULE_LICENSE("GPL");
