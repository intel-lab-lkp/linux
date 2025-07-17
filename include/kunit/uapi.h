/* SPDX-License-Identifier: GPL-2.0 */
/*
 * KUnit Userspace testing API.
 *
 * Copyright (C) 2025, Linutronix GmbH.
 * Author: Thomas Weißschuh <thomas.weissschuh@linutronix.de>
 */

#ifndef _KUNIT_UAPI_H
#define _KUNIT_UAPI_H

#include <linux/types.h>

struct kunit;

/**
 * struct kunit_uapi_blob - Blob embedded build artifact
 * @path: Path of the embedded artifact.
 * @data: Start of the embedded data in memory.
 * @end: End of the embedded data in memory.
 */
struct kunit_uapi_blob {
	const char *const path;
	const u8 *data;
	const u8 *end;
};

#if IS_ENABLED(CONFIG_KUNIT_UAPI)

/**
 * KUNIT_UAPI_EMBED_BLOB() - Embed another build artifact into the kernel
 * @_name: The name of symbol under which the artifact is embedded.
 * @_path: Path to the artifact on disk.
 *
 * Embeds a build artifact like a userspace executable into the kernel or current module.
 * The build artifact is read from disk and needs to be already built.
 */
#define KUNIT_UAPI_EMBED_BLOB(_name, _path)					\
	asm (									\
	"	.pushsection .rodata, \"a\"				\n"	\
	"	.global " __stringify(CONCATENATE(_name, _data)) "	\n"	\
	__stringify(CONCATENATE(_name, _data)) ":			\n"	\
	"	.incbin " __stringify(_path) "				\n"	\
	"	.size " __stringify(CONCATENATE(_name, _data)) ", "		\
			". - " __stringify(CONCATENATE(_name, _data)) "	\n"	\
	"	.global " __stringify(CONCATENATE(_name, _end)) "	\n"	\
	__stringify(CONCATENATE(_name, _end)) ":			\n"	\
	"	.popsection						\n"	\
	);									\
										\
	extern const char CONCATENATE(_name, _data)[];				\
	extern const char CONCATENATE(_name, _end)[];				\
										\
	static const struct kunit_uapi_blob _name = {				\
		.path	= _path,						\
		.data	= CONCATENATE(_name, _data),				\
		.end	= CONCATENATE(_name, _end),				\
	}									\

#else /* !CONFIG_KUNIT_UAPI */

/* Unresolved external reference, to be optimized away */
#define KUNIT_UAPI_EMBED_BLOB(_name, _path)					\
	extern const struct kunit_uapi_blob _name

#endif /* CONFIG_KUNIT_UAPI */

/**
 * kunit_uapi_run_kselftest() - Run a userspace kselftest as part of kunit
 * @test: The test context object.
 * @executable: kselftest executable to run
 *
 * Runs the kselftest and forwards its TAP output and exit status to kunit.
 */
void kunit_uapi_run_kselftest(struct kunit *test, const struct kunit_uapi_blob *executable);

#endif /* _KUNIT_UAPI_H */
