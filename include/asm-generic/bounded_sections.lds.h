/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ASM_GENERIC_BOUNDED_SECTIONS_H
#define _ASM_GENERIC_BOUNDED_SECTIONS_H

#define BOUNDED_SECTION_PRE_LABEL(_sec_, _label_, _BEGIN_, _END_)	\
	_BEGIN_##_label_ = .;						\
	KEEP(*(_sec_))							\
	_END_##_label_ = .;

#define BOUNDED_SECTION_POST_LABEL(_sec_, _label_, _BEGIN_, _END_)	\
	_label_##_BEGIN_ = .;						\
	KEEP(*(_sec_))							\
	_label_##_END_ = .;

#define BOUNDED_SECTION_BY(_sec_, _label_)				\
	BOUNDED_SECTION_PRE_LABEL(_sec_, _label_, __start, __stop)

#define BOUNDED_SECTION(_sec)	 BOUNDED_SECTION_BY(_sec, _sec)

#define HEADERED_SECTION_PRE_LABEL(_sec_, _label_, _BEGIN_, _END_, _HDR_) \
	_HDR_##_label_	= .;						\
	KEEP(*(.gnu.linkonce.##_sec_))					\
	BOUNDED_SECTION_PRE_LABEL(_sec_, _label_, _BEGIN_, _END_)

#define HEADERED_SECTION_POST_LABEL(_sec_, _label_, _BEGIN_, _END_, _HDR_) \
	_label_##_HDR_ = .;						\
	KEEP(*(.gnu.linkonce.##_sec_))					\
	BOUNDED_SECTION_POST_LABEL(_sec_, _label_, _BEGIN_, _END_)

#define HEADERED_SECTION_BY(_sec_, _label_)				\
	HEADERED_SECTION_PRE_LABEL(_sec_, _label_, __start, __stop)

#define HEADERED_SECTION(_sec)	 HEADERED_SECTION_BY(_sec, _sec)

#endif /* _ASM_GENERIC_BOUNDED_SECTIONS_H */
