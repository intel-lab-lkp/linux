/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/export.h>
#include <linux/init.h>

#include <asm/setup.h>

        __INITDATA

       .align 8
       .global cmdline_prepend
cmdline_prepend:
       .ifnc CONFIG_CMDLINE_PREPEND,""
       .ascii CONFIG_CMDLINE_PREPEND
       .string " "
       .else
       .byte 0x0
       .endif
#ifdef CONFIG_CMDLINE_EXTRA
       .space COMMAND_LINE_SIZE - (.-cmdline_prepend)
       .size cmdline_prepend, COMMAND_LINE_SIZE
#endif /* CONFIG_CMDLINE_EXTRA */

cmdline_prepend_end:
       .size cmdline_prepend, (cmdline_prepend_end - cmdline_prepend)

       .align 8
       .global cmdline_tmp
cmdline_tmp:
       .ifnc CONFIG_CMDLINE_PREPEND,""
       .size cmdline_tmp, COMMAND_LINE_SIZE
       .space COMMAND_LINE_SIZE
       .else
       .byte 0x0
       .endif
cmdline_tmp_end:
       .size cmdline_tmp, (cmdline_tmp_end - cmdline_tmp)

       .align 8
       .global cmdline_append
       .size cmdline_append, COMMAND_LINE_SIZE
cmdline_append:
       .ifnc CONFIG_CMDLINE_APPEND,""
       .ascii " "
       .string CONFIG_CMDLINE_APPEND
       .else
       .byte 0x0
       .endif
#ifdef CONFIG_CMDLINE_EXTRA
       .space COMMAND_LINE_SIZE - (.-cmdline_append)
#endif /* CONFIG_CMDLINE_EXTRA */
cmdline_append_end:
       .size cmdline_append, (cmdline_append_end - cmdline_append)

