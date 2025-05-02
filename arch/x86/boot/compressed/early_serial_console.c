#include "misc.h"

unsigned int (*serial_in)(unsigned long addr, int offset);
void (*serial_out)(unsigned long addr, int offset, int value);

/* This might be accessed before .bss is cleared, so use .data instead. */
unsigned long early_serial_base __section(".data");

#include "../early_serial_console.c"
