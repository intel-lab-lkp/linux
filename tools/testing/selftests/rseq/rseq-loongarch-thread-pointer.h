/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2023 Huang Pei <huangpei@loongson.cn> */

#ifndef _RSEQ_LOONGARCH_THREAD_POINTER
#define _RSEQ_LOONGARCH_THREAD_POINTER

#ifdef __cplusplus
extern "C" {
#endif

static inline void *rseq_thread_pointer(void)
{
	register void *__result asm ("$2");
	asm ("" : "=r" (__result));
	return __result;
}

#ifdef __cplusplus
}
#endif

#endif
