#ifndef __CONGATEC_CGEB_H
#define __CONGATEC_CGEB_H

/* CGEB interface functions */
typedef enum {
	CgebGetCgebVersion =            0,
	CgebGetSysBiosVersion =         1,
	CgebGetVgaBiosVersion =         2,
	CgebGetDataSize =               3,
	CgebOpen =                      4,
	CgebClose =                     5,
	CgebMapGetMem =                 6,
	CgebMapChanged =                7,
	CgebMapGetPorts =               8,
	CgebDelayUs =                   9,
	CgebCgbcReadWrite =             10,
	CgebCgbcSetControl =            11,
	CgebCgbcGetInfo =               12,
	CgebCgbcHandleCommand =         13,
	CgebBoardGetInfo =              14,
	CgebBoardGetBootCounter =       15,
	CgebBoardGetRunningTimeMeter =  16,
	CgebBoardGetBootErrorLog =      17,
	CgebVgaCount =                  18,
	CgebVgaGetInfo =                19,
	CgebVgaGetContrast =            20,
	CgebVgaSetContrast =            21,
	CgebVgaGetContrastEnable =      22,
	CgebVgaSetContrastEnable =      23,
	CgebVgaGetBacklight =           24,
	CgebVgaSetBacklight =           25,
	CgebVgaGetBacklightEnable =     26,
	CgebVgaSetBacklightEnable =     27,
	CgebVgaEndDarkBoot =            28,
	CgebStorageAreaCount =          29,
	CgebStorageAreaGetInfo =        30,
	CgebStorageAreaRead =           31,
	CgebStorageAreaWrite =          32,
	CgebStorageAreaErase =          33,
	CgebStorageAreaEraseStatus =    34,
	CgebI2CCount =                  35,
	CgebI2CGetInfo =                36,
	CgebI2CGetAddrList =            37,
	CgebI2CTransfer =               38,
	CgebI2CGetFrequency =           39,
	CgebI2CSetFrequency =           40,
	CgebIOCount =                   41,
	CgebIOGetInfo =                 42,
	CgebIORead =                    43,
	CgebIOWrite =                   44,
	CgebIOGetDirection =            45,
	CgebIOSetDirection =            46,
	CgebWDogCount =                 47,
	CgebWDogGetInfo =               48,
	CgebWDogTrigger =               49,
	CgebWDogGetConfig =             50,
	CgebWDogSetConfig =             51,
	CgebPerformanceGetCurrent =     52,
	CgebPerformanceSetCurrent =     53,
	CgebPerformanceGetPolicyCaps =  54,
	CgebPerformanceGetPolicy =      55,
	CgebPerformanceSetPolicy =      56,
	CgebTemperatureCount =          57,
	CgebTemperatureGetInfo =        58,
	CgebTemperatureGetCurrent =     59,
	CgebTemperatureSetLimits =      60,
	CgebFanCount =                  61,
	CgebFanGetInfo =                62,
	CgebFanGetCurrent =             63,
	CgebFanSetLimits =              64,
	CgebVoltageCount =              65,
	CgebVoltageGetInfo =            66,
	CgebVoltageGetCurrent =         67,
	CgebVoltageSetLimits =          68,
	CgebStorageAreaLock =           69,
	CgebStorageAreaUnlock =         70,
	CgebStorageAreaIsLocked =       71,
} cgeb_function_t;

struct cgeb_function_parameters {
	u32 unit;               /* unit number or type */
	u32 pars[4];            /* input parameters */
	u32 rets[2];            /* return parameters */
	void *iptr;             /* input pointer */
	void *optr;             /* output pointer */
	size_t iptr_size;       /* size of input pointer */
	size_t optr_size;       /* size of output pointer */
	void *callback_data;    /* callback data pointer */
	int (*callback)(void __user *, void *, void *);
				/* Run when CGEB call finishes with userspace */
				/* optr, kernel optr, and callback_data */
};

struct cgeb_board_data;

int cgeb_call(struct cgeb_board_data *,
		struct cgeb_function_parameters *, cgeb_function_t);

int cgeb_call_simple(struct cgeb_board_data *,
		cgeb_function_t, u32,
		void *, size_t size, u32 *);

/*
 * Platform data for child devices
 */
struct cgeb_pdata {
	struct cgeb_board_data *board;
	int unit;
};

#endif /* __CONGATEC_CGEB_H */
