/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
#ifndef __EROFS_LIB_LIBEROFS_FILE_DELTA_H
#define __EROFS_LIB_LIBEROFS_FILE_DELTA_H

#include "erofs/internal.h"

struct erofs_file_delta;

struct erofs_file_delta_ops {
	const char *name;
	erofs_off_t max_chunksize;
	int (*open)(struct erofs_file_delta *delta, u64 target_size,
		    erofs_off_t chunksize);
	void (*close)(struct erofs_file_delta *delta);
	size_t (*count)(const struct erofs_file_delta *delta);
	int (*read)(const struct erofs_file_delta *delta, size_t index,
		    u64 *chunk, void *buf);
};

struct erofs_file_delta {
	struct list_head list;
	const struct erofs_file_delta_ops *ops;
	char *target;
	char *source_path;
	struct erofs_inode lower_inode;
	void *private;
	bool applied;
};

struct erofs_file_delta_manager {
	struct list_head deltas;
	unsigned int chunkbits;
	bool initialized;
};

int erofs_file_delta_parse_spec(struct list_head *deltas, const char *arg,
		const struct erofs_file_delta_ops * const *backends);
int erofs_file_delta_validate_config(struct list_head *deltas,
				     unsigned int chunkbits);
void erofs_file_delta_cleanup(struct list_head *deltas);

int erofs_file_delta_manager_init(struct erofs_file_delta_manager *mgr,
		struct erofs_sb_info *parent, struct list_head *deltas,
		unsigned int chunkbits);
void erofs_file_delta_manager_exit(struct erofs_file_delta_manager *mgr);
int erofs_file_delta_manager_finish(struct erofs_file_delta_manager *mgr);

struct erofs_file_delta *erofs_file_delta_lookup_target(
		struct erofs_file_delta_manager *mgr, const char *target);
int erofs_file_delta_apply(struct erofs_file_delta_manager *mgr,
		struct erofs_file_delta *delta, struct erofs_inode *inode,
		struct erofs_sb_info *dst_sb);

#endif
