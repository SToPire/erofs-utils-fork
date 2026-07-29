// SPDX-License-Identifier: GPL-2.0+ OR MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <config.h>
#ifdef HAVE_LINUX_FS_H
#include <linux/fs.h>
#endif
#include "liberofs_dm_persistent.h"

#ifndef EFSCORRUPTED
#ifdef EUCLEAN
#define EFSCORRUPTED EUCLEAN
#else
#define EFSCORRUPTED EIO
#endif
#endif

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

struct erofs_dm_disk_header {
	__le32 magic;
	__le32 valid;
	__le32 version;
	__le32 chunk_size;
} __packed;

struct erofs_dm_disk_exception {
	__le64 old_chunk;
	__le64 new_chunk;
} __packed;

static int erofs_dm_get_store_size(int fd, const struct stat *st, u64 *size)
{
	if (S_ISREG(st->st_mode)) {
		if (st->st_size < 0)
			return -EOVERFLOW;
		*size = st->st_size;
		return 0;
	}
	if (!S_ISBLK(st->st_mode))
		return -ENOTSUP;

#ifdef BLKGETSIZE64
	if (!ioctl(fd, BLKGETSIZE64, size))
		return 0;
#endif
#ifdef BLKGETSIZE
	{
		unsigned long sectors;

		if (!ioctl(fd, BLKGETSIZE, &sectors)) {
			if ((u64)sectors > UINT64_MAX >> 9)
				return -EOVERFLOW;
			*size = (u64)sectors << 9;
			return 0;
		}
	}
#endif
#if defined(BLKGETSIZE64) || defined(BLKGETSIZE)
	return -errno;
#else
	return -ENOTSUP;
#endif
}

static int erofs_dm_pread_exact(int fd, void *buf, size_t len, u64 offset)
{
	size_t done = 0;

	if (offset > INT64_MAX || len > INT64_MAX - offset)
		return -EOVERFLOW;
	while (done < len) {
		ssize_t ret;

#ifdef HAVE_PREAD64
		ret = pread64(fd, (char *)buf + done, len - done,
			      (off64_t)(offset + done));
#else
		ret = pread(fd, (char *)buf + done, len - done,
			    (off_t)(offset + done));
#endif
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (!ret)
			return -EIO;
		done += ret;
	}
	return 0;
}

static int erofs_dm_exception_cmp(const void *a, const void *b)
{
	const struct erofs_dm_exception *ea = a, *eb = b;

	return (ea->old_chunk > eb->old_chunk) -
	       (ea->old_chunk < eb->old_chunk);
}

static int erofs_dm_append_exception(struct erofs_dm_delta *delta,
				     size_t *capacity, u64 max_target_chunks,
				     u64 old_chunk, u64 new_chunk)
{
	struct erofs_dm_exception *p;
	size_t new_capacity, bytes;

	if (delta->nr_exceptions >= max_target_chunks)
		return -EINVAL;
	if (delta->nr_exceptions >= EROFS_DM_MAX_EXCEPTIONS)
		return -E2BIG;
	if (delta->nr_exceptions < *capacity)
		goto add;

	new_capacity = *capacity ? *capacity << 1 : 64;
	if (new_capacity < *capacity)
		return -E2BIG;
	if (new_capacity > EROFS_DM_MAX_EXCEPTIONS)
		new_capacity = EROFS_DM_MAX_EXCEPTIONS;
	if (new_capacity > max_target_chunks)
		new_capacity = max_target_chunks;
	if (new_capacity <= *capacity ||
	    __builtin_mul_overflow(new_capacity, sizeof(*p), &bytes))
		return -E2BIG;
	p = realloc(delta->exceptions, bytes);
	if (!p)
		return -ENOMEM;
	delta->exceptions = p;
	*capacity = new_capacity;
add:
	delta->exceptions[delta->nr_exceptions++] =
		(struct erofs_dm_exception) {
			.old_chunk = old_chunk,
			.new_chunk = new_chunk,
		};
	return 0;
}

static int erofs_dm_parse_exceptions(struct erofs_dm_delta *delta)
{
	u64 max_target_chunks = delta->target_size / delta->chunk_bytes;
	struct erofs_dm_disk_exception *area_buf;
	size_t capacity = 0;
	u64 area, stride;
	int ret = 0;
	bool partial = delta->target_size % delta->chunk_bytes;

	if (check_add_overflow(delta->exceptions_per_area, (u64)1, &stride) ||
	    (partial && check_add_overflow(max_target_chunks, (u64)1,
					   &max_target_chunks)))
		return -EFSCORRUPTED;

	area_buf = malloc(delta->chunk_bytes);
	if (!area_buf)
		return -ENOMEM;

	for (area = 0; ; ++area) {
		u64 metadata_chunk, metadata_offset, metadata_end;
		u64 i;

		if (__builtin_mul_overflow(stride, area, &metadata_chunk) ||
		    check_add_overflow(metadata_chunk, (u64)1, &metadata_chunk) ||
		    __builtin_mul_overflow(metadata_chunk, delta->chunk_bytes,
					   &metadata_offset) ||
		    check_add_overflow(metadata_offset, delta->chunk_bytes,
				       &metadata_end) ||
		    metadata_end > delta->store_size) {
			ret = -EFSCORRUPTED;
			break;
		}
		ret = erofs_dm_pread_exact(delta->fd, area_buf,
					   delta->chunk_bytes, metadata_offset);
		if (ret) {
			if (ret == -EIO)
				ret = -EFSCORRUPTED;
			break;
		}

		for (i = 0; i < delta->exceptions_per_area; ++i) {
			u64 old_chunk = le64_to_cpu(area_buf[i].old_chunk);
			u64 new_chunk = le64_to_cpu(area_buf[i].new_chunk);
			u64 payload_offset, payload_end;

			if (!new_chunk)
				goto terminated;
			if (new_chunk % stride == 1 ||
			    __builtin_mul_overflow(new_chunk,
						   delta->chunk_bytes,
						   &payload_offset) ||
			    check_add_overflow(payload_offset, delta->chunk_bytes,
					       &payload_end) ||
			    payload_end > delta->store_size) {
				ret = -EFSCORRUPTED;
				goto out;
			}
			if (old_chunk >= max_target_chunks) {
				ret = -EINVAL;
				goto out;
			}
			ret = erofs_dm_append_exception(delta, &capacity,
					max_target_chunks, old_chunk, new_chunk);
			if (ret)
				goto out;
		}
	}
	goto out;

terminated:
	if (delta->nr_exceptions > 1)
		qsort(delta->exceptions, delta->nr_exceptions,
		      sizeof(*delta->exceptions), erofs_dm_exception_cmp);
	for (area = 1; area < delta->nr_exceptions; ++area) {
		if (delta->exceptions[area - 1].old_chunk ==
		    delta->exceptions[area].old_chunk) {
			ret = -EINVAL;
			break;
		}
	}
out:
	free(area_buf);
	return ret;
}

int erofs_dm_persistent_open(struct erofs_dm_delta *delta, const char *path,
			     u64 target_size)
{
	struct erofs_dm_disk_header header;
	struct stat st;
	u32 sectors;
	u64 chunk_bytes;
	int ret;

	BUILD_BUG_ON(sizeof(header) != 16);
	BUILD_BUG_ON(sizeof(struct erofs_dm_disk_exception) != 16);
	*delta = (struct erofs_dm_delta) { .fd = -1 };
	delta->target_size = target_size;

	delta->fd = open(path, O_RDONLY | O_BINARY | O_CLOEXEC);
	if (delta->fd < 0) {
		ret = -errno;
		goto err;
	}
	if (fstat(delta->fd, &st)) {
		ret = -errno;
		goto err;
	}
	ret = erofs_dm_get_store_size(delta->fd, &st, &delta->store_size);
	if (ret)
		goto err;
	ret = erofs_dm_pread_exact(delta->fd, &header, sizeof(header), 0);
	if (ret) {
		if (ret == -EIO)
			ret = -EFSCORRUPTED;
		goto err;
	}
	if (le32_to_cpu(header.magic) != EROFS_DM_SNAP_MAGIC ||
	    le32_to_cpu(header.version) != EROFS_DM_DISK_VERSION) {
		ret = -EFSCORRUPTED;
		goto err;
	}
	if (le32_to_cpu(header.valid) != 1) {
		ret = -EINVAL;
		goto err;
	}

	sectors = le32_to_cpu(header.chunk_size);
	if (!sectors || (sectors & (sectors - 1)) ||
	    sectors > (INT_MAX >> 9) ||
	    __builtin_mul_overflow((u64)sectors,
				   (u64)EROFS_DM_SECTOR_SIZE, &chunk_bytes)) {
		ret = -EFSCORRUPTED;
		goto err;
	}
	if (chunk_bytes > EROFS_DM_MAX_CHUNK_BYTES) {
		ret = -E2BIG;
		goto err;
	}
	delta->chunk_bytes = chunk_bytes;
	delta->exceptions_per_area = chunk_bytes /
					    sizeof(struct erofs_dm_disk_exception);
	ret = erofs_dm_parse_exceptions(delta);
	if (ret)
		goto err;
	return 0;
err:
	erofs_dm_persistent_close(delta);
	return ret;
}

void erofs_dm_persistent_close(struct erofs_dm_delta *delta)
{
	if (delta->fd >= 0)
		close(delta->fd);
	free(delta->exceptions);
	*delta = (struct erofs_dm_delta) { .fd = -1 };
}

int erofs_dm_persistent_lookup(const struct erofs_dm_delta *delta, u64 offset,
			       u64 *next, bool *dirty, u64 *store_offset)
{
	u64 chunk, chunk_start, chunk_end;
	size_t lo = 0, hi = delta->nr_exceptions;

	if (offset >= delta->target_size)
		return -ENXIO;
	chunk = offset / delta->chunk_bytes;
	chunk_start = chunk * delta->chunk_bytes;
	if (check_add_overflow(chunk_start, delta->chunk_bytes, &chunk_end) ||
	    chunk_end > delta->target_size)
		chunk_end = delta->target_size;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;

		if (delta->exceptions[mid].old_chunk < chunk)
			lo = mid + 1;
		else
			hi = mid;
	}
	*next = chunk_end;
	*dirty = lo < delta->nr_exceptions &&
		 delta->exceptions[lo].old_chunk == chunk;
	if (*dirty) {
		u64 base;

		if (__builtin_mul_overflow(delta->exceptions[lo].new_chunk,
					   delta->chunk_bytes, &base) ||
		    check_add_overflow(base, offset - chunk_start, store_offset))
			return -EOVERFLOW;
	}
	return 0;
}

ssize_t erofs_dm_persistent_pread(struct erofs_dm_delta *delta,
				 struct erofs_vfile *lower, void *buf,
				 size_t len, u64 offset)
{
	u64 end;
	size_t done = 0;

	if (offset >= delta->target_size)
		return 0;
	if (len > SSIZE_MAX)
		len = SSIZE_MAX;
	len = min_t(u64, len, delta->target_size - offset);
	end = offset + len;
	while (offset < end) {
		u64 next, store_offset = 0;
		bool dirty;
		ssize_t ret;

		ret = erofs_dm_persistent_lookup(delta, offset, &next, &dirty,
						 &store_offset);
		if (ret)
			return ret;
		next = min_t(u64, next, end);
		if (dirty) {
			ret = erofs_dm_pread_exact(delta->fd, (char *)buf + done,
						   next - offset, store_offset);
			if (ret)
				return ret;
			ret = next - offset;
		} else {
			ret = erofs_io_pread(lower, (char *)buf + done,
					     next - offset, offset);
			if (ret < 0)
				return ret;
			if ((u64)ret != next - offset)
				return -EIO;
		}
		done += ret;
		offset += ret;
	}
	return done;
}
