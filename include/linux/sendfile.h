/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SENDFILE_H
#define SENDFILE_H

#define SENDFILE_DEFAULT (0x1)  /* normal sendfile */
#define SENDFILE_ZC (0x2)       /* sendfile which generates ZC notifications */

#define SENDFILE_ALL (SENDFILE_DEFAULT|SENDFILE_ZC)

#endif
