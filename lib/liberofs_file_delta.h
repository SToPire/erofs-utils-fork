/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
#ifndef __EROFS_LIB_LIBEROFS_FILE_DELTA_H
#define __EROFS_LIB_LIBEROFS_FILE_DELTA_H

#include "erofs/importer.h"
#include "liberofs_dm_persistent.h"

struct erofs_file_delta {
	struct list_head list;
	char *target;
	char *store_path;
	struct erofs_inode lower_inode;
	struct erofs_dm_delta dm;
};

struct erofs_file_delta_manager {
	struct list_head deltas;
	bool initialized;
};

int erofs_file_delta_parse_spec(struct list_head *deltas, const char *arg);
void erofs_file_delta_cleanup(struct list_head *deltas);

int erofs_file_delta_manager_init(struct erofs_file_delta_manager *mgr,
				  struct erofs_sb_info *sbi,
				  struct list_head *deltas);
void erofs_file_delta_manager_exit(struct erofs_file_delta_manager *mgr);

struct erofs_file_delta *erofs_file_delta_lookup_target(
		struct erofs_file_delta_manager *mgr, const char *target);
struct erofs_vfile *erofs_file_delta_open_vfile(struct erofs_file_delta *delta);

int erofs_file_delta_copyup_targets(struct erofs_importer *im,
				    struct erofs_inode *dir,
				    u64 *nr_subdirs,
				    unsigned int *i_nlink);

#endif
