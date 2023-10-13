// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014 BHT Inc.
 *
 * File Name: debug.c
 *
 * Abstract: define related functions for debugging
 *
 * Version: 1.00
 *
 * Author: Peter.Guo
 *
 * Environment:	OS Independent
 *
 * History:
 *
 * 8/25/2014		Creation	Peter.Guo
 */

#define _NO_CRT_STDIO_INLINE
#include "../include/basic.h"
#include "../include/debug.h"
#include "../include/util.h"

#define DBG_CHR_IN_LINE	124
#define DBG_PREFIX	"BHT-"

u32 g_dbg_module = DBG_MODULE_CONTROL;
u32 g_dbg_feature = DBG_FEATURE_CONTROL;
u32 g_dbg_ctrl = DBG_CTRL_CONTROL;

#if DBG || _DEBUG
static char dbg_module[][14] = {
	"BHT-SDHOST : ",
	"BHT-VENHOST: ",
	"BHT-TRANS  : ",
	"BHT-CARD   : ",
	"BHT-CARD   : ",
	"BHT-CARD   : ",
	"BHT-CARD   : ",
	"BHT-TQFLOW : ",
	"BHT-TQPLC  : ",
	"BHT-TQDMA  : ",
	"BHT-THR    : ",
	"BHT-REQMNG : ",
	"BHT-MAINPM : ",
	"BHT-GENIO  : ",
	"BHT-TIMER  : ",
	"BHT-THERMAL: ",
	"BHT-CFG    : ",
	"BHT-OTHER  : ",
	"BHT-OSETAPI: ",
	"BHT-OSENTRY: ",
	"BHT-OSAPI  : ",
	"BHT-UNKNOW : "
};
#endif

typedef struct {
	u32 index;
	byte Data[DBG_CHR_IN_LINE];
} tO2DbgLineInfo;

#define O2DBG_MAX_COUNT 0x4000
#define O2DBG_MAX_RAM_SIZE (O2DBG_MAX_COUNT * sizeof(tO2DbgLineInfo))

/* Global Debug Virtual Buffer for RAM Debug */
tO2DbgLineInfo *pO2DbgInfo;
/* Global Ram Buffer item count */
atomic_t gO2DbgInfoCnt;

void x_assert(char *str, unsigned int uline)
{
	DbgErr("Assert failed %s line:%u\n", str, uline);
}

/*
 * Function Name: DbgRamInit
 * Abstract: This Function is called by driver init entry to allocate Global Memory for Debug
 *
 * Input:
 * Output:
 *
 * Return value: void
 * Notes:
 *
 */

void DbgRamInit(void)
{
	os_atomic_set(&gO2DbgInfoCnt, 0);
	pO2DbgInfo = (tO2DbgLineInfo *) os_alloc_vbuff(O2DBG_MAX_RAM_SIZE);
	if (pO2DbgInfo != NULL)
		os_memset(pO2DbgInfo, 0, O2DBG_MAX_RAM_SIZE);
}

void DbgRamInitNon(void)
{
	os_atomic_set(&gO2DbgInfoCnt, 0);
	pO2DbgInfo = NULL;
}

/*
 * Function Name: DbgRamInit
 * Abstract: This Function is called by driver remove entry to free Global Memory for Debug
 *
 * Input:
 * Output:
 *
 * Return value: void
 * Notes:
 *
 */
void DbgRamFree(void)
{
	os_atomic_set(&gO2DbgInfoCnt, 0);
	if (pO2DbgInfo != NULL)
		os_free_vbuff(pO2DbgInfo);
	pO2DbgInfo = NULL;

}

/*
 * Function Name: DbgRamAdd
 * Abstract: This Function is used to Add a Debug log to Debug Ram buffer
 *
 * Input: byte *dbgbuf: The Debug log want to add to ram buffer
 * Output:
 *
 * Return value: void
 * Notes:
 *
 */
static void DbgRamAdd(byte *dbgbuf)
{
	u32 i;
	int cnt = 0;
	int index = 0;
	u32 len = (u32) strlen(dbgbuf);

	if (pO2DbgInfo == NULL)
		return;
	cnt = (int)os_atomic_read(&gO2DbgInfoCnt);
	os_atomic_add(&gO2DbgInfoCnt, 1);
	index = cnt % O2DBG_MAX_COUNT;

	pO2DbgInfo[index].index = cnt;
	for (i = 0; i < DBG_CHR_IN_LINE; i++) {
		if (i < len)
			pO2DbgInfo[index].Data[i] = dbgbuf[i];
		else
			pO2DbgInfo[index].Data[i] = '\0';
	}
}

/*
 * Function Name: DbgErr
 * Abstract: This Function is used to print errlog and add log to Ram buffer
 *
 * Input: byte *info: The err log
 * Output:
 *
 * Return value: void
 * Notes:
 *
 */

void PrintMsg(byte *info, ...)
{
#define DBG_PREFIX_MSG      "BHT-MSG    : "

	byte dbgbuf[512];
	va_list ap;

	va_start(ap, info);

	{
		byte *buffer;

		buffer = dbgbuf;
		os_memcpy(buffer, DBG_PREFIX_MSG, 13);
		buffer += 13;

		vsnprintf(buffer, sizeof(dbgbuf) - 13, info, ap);
	}

	va_end(ap);
	os_print(dbgbuf);
}

/*
 * Function Name: DbgErr
 * Abstract: This Function is used to print errlog and add log to Ram buffer
 *
 * Input: byte *info: The err log
 * Output:
 *
 * Return value: void
 * Notes:
 *
 */

void DbgErr(byte *info, ...)
{
#define DBG_PREFIX_ERR      "BHT-ERROR  : "

	byte dbgbuf[512];
	va_list ap;

	va_start(ap, info);

	{
		byte *buffer;

		buffer = dbgbuf;
		os_memcpy(buffer, DBG_PREFIX_ERR, 13);
		buffer += 13;

		vsnprintf(buffer, sizeof(dbgbuf) - 13, info, ap);
	}

	va_end(ap);
	DbgRamAdd(dbgbuf);
	os_print(dbgbuf);
}

#if DBG || _DEBUG
/*
 * Function Name: DbgWarn
 * Abstract: This Function is used to print warn log and add log to Ram buffer
 *
 * Input: u32 module; The module id which to print warn log
 *        byte ram:   whether add the log to ram or not
 *        byte *info: the string for log
 * Output:
 *
 * Return value: void
 * Notes:
 *
 */

void DbgWarn(u32 module, byte ram, byte *info, ...)
{
#define DBG_PREFIX_WARN  "BHT-WARNING: "

	byte dbgbuf[512];
	va_list ap;

	va_start(ap, info);
	if (module & g_dbg_module) {
		byte *buffer;

		buffer = dbgbuf;
		os_memcpy(buffer, DBG_PREFIX_WARN, 13);
		buffer += 13;

		vsnprintf(buffer, sizeof(dbgbuf) - 13, info, ap);
	}

	va_end(ap);
	if (module & g_dbg_module) {
		os_print(dbgbuf);
		if (ram)
			DbgRamAdd(dbgbuf);
	}
}

/*
 * Function Name: DbgWarn
 * Abstract: This Function is used to print info log and add log to Ram buffer
 *
 * Input: u32 module; The module id which to print info log
 *        u32 feature: The feature the info is
 *        byte ram:   whether add the log to ram or not
 *        byte *info: the string for log
 * Output:
 *
 * Return value: void
 * Notes:
 *
 */
void DbgInfo(u32 module, u32 feature, byte ram, byte *info, ...)
{
	byte dbgbuf[512];
	va_list ap;

	va_start(ap, info);
	if ((module & g_dbg_module) && (g_dbg_feature & feature)) {
		byte *buffer;
		u32 idx = fls32(module);

		if (idx > 20)
			idx = 21;
		buffer = dbgbuf;
		os_memcpy(buffer, dbg_module[idx], 13);
		buffer += 13;

		vsnprintf(buffer, sizeof(dbgbuf) - 13, info, ap);
	}

	va_end(ap);
	if ((module & g_dbg_module) && (g_dbg_feature & feature)) {
		os_print(dbgbuf);
		if (ram)
			DbgRamAdd(dbgbuf);
	}
}

#ifdef DBG_PERFORMANCE
static u64 cputick2_100ns(u64 period, u64 freq)
{

	u64 timeIn100ns = 0;

	if (freq > 0) {
		/* difference between performance counters, needs to convert to 100ns. */
		u64 countersDiff = period;

		/* get seconds */
		timeIn100ns = countersDiff / freq;

		/* get milliseconds */
		countersDiff = (countersDiff % freq) * 1000;
		timeIn100ns *= 1000;
		timeIn100ns += countersDiff / freq;

		/* get 100 nanoseconds */
		countersDiff = (countersDiff % freq) * 10000;
		timeIn100ns *= 10000;
		timeIn100ns += countersDiff / freq;
	}

	return timeIn100ns;

}

void calc_req_start(tPerTick *tick, u32 sec_cnt, bool bWrite)
{
	u64 period = 0;
	u64 cpu;

	period = os_get_performance_tick(&cpu);
	if (tick->tick_io_end == 0) {
		os_memset(tick, 0, sizeof(tPerTick));
		/* not calculate */
		goto next;
	}

	/* not continue and not 1MB print */
	if ((bWrite != tick->last_dir || sec_cnt != 0x800)
	    && (tick->io_cnt != 0)) {
		DbgErr("Avg Write=%d iocnt=%d T1=%d T2=%d T3=%d\n",
		       tick->last_dir, tick->io_cnt,
		       cputick2_100ns(tick->avg_start_2_thr,
				      cpu) / tick->io_cnt,
		       cputick2_100ns(tick->avg_thr_2_iodone,
				      cpu) / tick->io_cnt,
		       (tick->io_cnt >
			1) ? cputick2_100ns(tick->avg_iodone_2_next,
					    cpu) / (tick->io_cnt - 1) : 0);
		os_memset(tick, 0, sizeof(tPerTick));
	}

next:
	if (sec_cnt == 0x800) {
		tick->last_dir = bWrite;
		tick->start_io_tick = tick->tick_start = period;
		tick->io_cnt++;
		if (tick->tick_io_end) {
			period = (tick->tick_start - tick->tick_io_end);
			tick->avg_iodone_2_next += period;
		}
	} else {
		/* We conly calculate 1MB case */
		os_memset(tick, 0, sizeof(tPerTick));
		tick->start_io_tick = period;
	}

}

void calc_io_end(tPerTick *tick)
{
	u64 period = 0;
	u64 cpu_freq = 0;

	period = os_get_performance_tick(&cpu_freq);
	tick->io_duration =
	    cputick2_100ns(period - tick->start_io_tick, cpu_freq);
	if (tick->tick_thr_start == 0)
		return;

	tick->tick_io_end = period;
	{
		period = tick->tick_io_end - tick->tick_thr_start;
		tick->avg_thr_2_iodone += period;
	}
}

void calc_thr_start(tPerTick *tick)
{
	u64 period = 0;

	if (tick->tick_start == 0)
		return;

	tick->tick_thr_start = os_get_performance_tick(NULL);
	{
		period = tick->tick_thr_start - tick->tick_start;
		tick->avg_start_2_thr += period;
	}
}
#endif
#endif
