/* SPDX-License-Identifier: GPL-2.0+ OR MIT */
#ifndef __EROFS_LIB_LIBEROFS_DM_PERSISTENT_H
#define __EROFS_LIB_LIBEROFS_DM_PERSISTENT_H

#include "erofs/defs.h"
#include "erofs/io.h"

#define EROFS_DM_SNAP_MAGIC		0x70416e53U
#define EROFS_DM_DISK_VERSION		1U
#define EROFS_DM_SECTOR_SIZE		512U

/* Published implementation resource limits, distinct from format validity. */
#define EROFS_DM_MAX_CHUNK_BYTES		(64U * 1024U * 1024U)
#define EROFS_DM_MAX_EXCEPTIONS		(1U << 24)

struct erofs_dm_exception {
	u64 old_chunk;
	u64 new_chunk;
};

struct erofs_dm_delta {
	int fd;
	u64 store_size;
	u64 target_size;
	u64 chunk_bytes;
	u64 exceptions_per_area;
	size_t nr_exceptions;
	struct erofs_dm_exception *exceptions;
};

int erofs_dm_persistent_open(struct erofs_dm_delta *delta, const char *path,
				     u64 target_size);
void erofs_dm_persistent_close(struct erofs_dm_delta *delta);

int erofs_dm_persistent_lookup(const struct erofs_dm_delta *delta, u64 offset,
			       u64 *next, bool *dirty, u64 *store_offset);
ssize_t erofs_dm_persistent_pread(struct erofs_dm_delta *delta,
				 struct erofs_vfile *lower, void *buf,
				 size_t len, u64 offset);

#endif
