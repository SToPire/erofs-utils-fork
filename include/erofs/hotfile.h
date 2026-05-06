/* SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0 */
#ifndef __EROFS_HOTFILE_H
#define __EROFS_HOTFILE_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <limits.h>
#include "defs.h"

#define EROFS_HOT_RANK_NONE	UINT_MAX

int erofs_hotfile_load(const char *path);
bool erofs_hotfile_enabled(void);
unsigned int erofs_get_hot_file_rank(const char *path);
unsigned int erofs_get_hot_dir_rank(const char *path);
void erofs_hotfile_exit(void);

#ifdef __cplusplus
}
#endif

#endif
