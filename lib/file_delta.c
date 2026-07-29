// SPDX-License-Identifier: GPL-2.0+ OR MIT
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <config.h>
#include "erofs/blobchunk.h"
#include "erofs/config.h"
#include "erofs/print.h"
#include "liberofs_file_delta.h"

static bool erofs_file_delta_valid_target(const char *target)
{
	const char *component;

	if (!target || target[0] != '/' || !target[1])
		return false;
	if (strchr(target, ':'))
		return false;
	if (target[strlen(target) - 1] == '/')
		return false;

	for (component = target + 1; *component; ) {
		const char *end = strchr(component, '/');
		size_t len = end ? (size_t)(end - component) : strlen(component);

		if (!len || (len == 1 && component[0] == '.') ||
		    (len == 2 && component[0] == '.' && component[1] == '.'))
			return false;
		if (!end)
			break;
		component = end + 1;
	}
	return true;
}

static void erofs_file_delta_free(struct erofs_file_delta *delta)
{
	if (delta->ops && delta->ops->close)
		delta->ops->close(delta);
	free(delta->target);
	free(delta->source_path);
	free(delta);
}

static const struct erofs_file_delta_ops *erofs_file_delta_find_backend(
		const struct erofs_file_delta_ops * const *backends,
		const char *name, size_t namelen)
{
	const struct erofs_file_delta_ops *ops;

	if (!backends)
		return NULL;
	while ((ops = *backends++)) {
		if (strlen(ops->name) == namelen &&
		    !memcmp(ops->name, name, namelen))
			return ops;
	}
	return NULL;
}

int erofs_file_delta_parse_spec(struct list_head *deltas, const char *arg,
		const struct erofs_file_delta_ops * const *backends)
{
	const struct erofs_file_delta_ops *ops;
	struct erofs_file_delta *delta, *pos;
	const char *first, *second;
	size_t target_len;
	int ret;

	first = strchr(arg, ':');
	if (!first || first == arg)
		return -EINVAL;
	ops = erofs_file_delta_find_backend(backends, arg, first - arg);
	if (!ops)
		return -EOPNOTSUPP;
	if (!ops->name || !ops->open || !ops->close || !ops->count ||
	    !ops->read)
		return -EINVAL;
	second = strchr(first + 1, ':');
	if (!second || !second[1])
		return -EINVAL;
	target_len = second - (first + 1);
	if (!target_len)
		return -EINVAL;

	delta = calloc(1, sizeof(*delta));
	if (!delta)
		return -ENOMEM;
	delta->ops = ops;
	delta->target = strndup(first + 1, target_len);
	delta->source_path = strdup(second + 1);
	if (!delta->target || !delta->source_path) {
		ret = -ENOMEM;
		goto err_free_delta;
	}
	if (!erofs_file_delta_valid_target(delta->target)) {
		ret = -EINVAL;
		goto err_free_delta;
	}

	list_for_each_entry(pos, deltas, list) {
		if (!strcmp(pos->target, delta->target)) {
			ret = -EEXIST;
			goto err_free_delta;
		}
	}
	list_add_tail(&delta->list, deltas);
	return 0;

err_free_delta:
	erofs_file_delta_free(delta);
	return ret;
}

int erofs_file_delta_validate_config(struct list_head *deltas,
				     unsigned int chunkbits)
{
	const erofs_off_t chunksize = 1ULL << chunkbits;
	struct erofs_file_delta *delta;

	list_for_each_entry(delta, deltas, list) {
		if (delta->ops->max_chunksize &&
		    chunksize > delta->ops->max_chunksize) {
			if (!(delta->ops->max_chunksize & ((1U << 20) - 1)))
				erofs_err("file-delta chunksize %llu exceeds the %s %llu MiB limit",
					  chunksize | 0ULL, delta->ops->name,
					  delta->ops->max_chunksize >> 20);
			else
				erofs_err("file-delta chunksize %llu exceeds the %s %llu-byte limit",
					  chunksize | 0ULL, delta->ops->name,
					  delta->ops->max_chunksize | 0ULL);
			return -E2BIG;
		}
	}
	return 0;
}

void erofs_file_delta_cleanup(struct list_head *deltas)
{
	struct erofs_file_delta *delta, *n;

	list_for_each_entry_safe(delta, n, deltas, list) {
		list_del(&delta->list);
		erofs_file_delta_free(delta);
	}
}

int erofs_file_delta_manager_init(struct erofs_file_delta_manager *mgr,
		struct erofs_sb_info *parent, struct list_head *deltas,
		unsigned int chunkbits)
{
	struct erofs_file_delta *delta;
	erofs_off_t chunksize = 1ULL << chunkbits;
	int ret;

	init_list_head(&mgr->deltas);
	mgr->chunkbits = chunkbits;
	mgr->initialized = true;
	list_splice_tail(deltas, &mgr->deltas);
	init_list_head(deltas);

	list_for_each_entry(delta, &mgr->deltas, list) {
		delta->lower_inode = (struct erofs_inode) { .sbi = parent };
		ret = erofs_ilookup(delta->target, &delta->lower_inode);
		if (ret) {
			erofs_err("file-delta TARGET %s cannot be read: %s",
				  delta->target, erofs_strerror(ret));
			goto err;
		}
		if (!S_ISREG(delta->lower_inode.i_mode)) {
			erofs_err("file-delta TARGET %s is not a regular file",
				  delta->target);
			ret = -EINVAL;
			goto err;
		}
		if (!delta->lower_inode.i_size) {
			erofs_err("file-delta TARGET %s is empty", delta->target);
			ret = -EINVAL;
			goto err;
		}
		if (delta->lower_inode.datalayout != EROFS_INODE_CHUNK_BASED ||
		    !(delta->lower_inode.u.chunkformat &
		      EROFS_CHUNK_FORMAT_INDEXES)) {
			erofs_err("file-delta TARGET %s is not a device-indexed chunk file",
				  delta->target);
			ret = -EOPNOTSUPP;
			goto err;
		}
		if (delta->lower_inode.u.chunkbits != chunkbits ||
		    delta->lower_inode.i_size % chunksize) {
			erofs_err("file-delta TARGET %s has incompatible chunk geometry",
				  delta->target);
			ret = -EINVAL;
			goto err;
		}
		if (delta->lower_inode.i_nlink > 1) {
			erofs_err("file-delta TARGET %s is hard-linked",
				  delta->target);
			ret = -EOPNOTSUPP;
			goto err;
		}

		ret = delta->ops->open(delta, delta->lower_inode.i_size,
				       chunksize);
		if (ret) {
			erofs_err("file-delta %s source %s failed validation: %s",
				  delta->ops->name, delta->source_path,
				  erofs_strerror(ret));
			goto err;
		}
	}
	return 0;
err:
	erofs_file_delta_manager_exit(mgr);
	return ret;
}

void erofs_file_delta_manager_exit(struct erofs_file_delta_manager *mgr)
{
	if (!mgr->initialized)
		return;
	erofs_file_delta_cleanup(&mgr->deltas);
	mgr->initialized = false;
}

static const char *erofs_file_delta_relpath(const char *path)
{
	path = erofs_fspath(path);
	while (*path == '/')
		++path;
	return path;
}

struct erofs_file_delta *erofs_file_delta_lookup_target(
		struct erofs_file_delta_manager *mgr, const char *target)
{
	const char *relative = erofs_file_delta_relpath(target);
	struct erofs_file_delta *delta;

	if (!mgr || !mgr->initialized)
		return NULL;
	list_for_each_entry(delta, &mgr->deltas, list)
		if (!strcmp(delta->target + 1, relative))
			return delta;
	return NULL;
}

static bool erofs_file_delta_is_zero(const void *buf, size_t len)
{
	const u8 *p = buf;

	while (len--)
		if (*p++)
			return false;
	return true;
}

int erofs_file_delta_apply(struct erofs_file_delta_manager *mgr,
		struct erofs_file_delta *delta, struct erofs_inode *inode,
		struct erofs_sb_info *dst_sb)
{
	const erofs_off_t chunksize = 1ULL << mgr->chunkbits;
	const u64 count = inode->i_size >> mgr->chunkbits;
	const size_t nr_changes = delta->ops->count(delta);
	struct erofs_chunkitem **indexes = inode->chunkindexes;
	void *buf;
	size_t i;
	int ret = 0;

	if (delta->applied || inode->datalayout != EROFS_INODE_CHUNK_BASED ||
	    inode->u.chunkbits != mgr->chunkbits ||
	    inode->extent_isize != count * sizeof(struct erofs_inode_chunk_index))
		return -EINVAL;

	buf = malloc(chunksize);
	if (!buf)
		return -ENOMEM;
	for (i = 0; i < nr_changes; ++i) {
		struct erofs_chunkitem *chunk;
		u64 target_chunk;

		ret = delta->ops->read(delta, i, &target_chunk, buf);
		if (ret)
			break;
		if (target_chunk >= count) {
			ret = -EINVAL;
			break;
		}
		if (erofs_file_delta_is_zero(buf, chunksize))
			chunk = erofs_get_unhashed_chunk(dst_sb, 0,
						   EROFS_NULL_ADDR, 0);
		else
			chunk = erofs_blob_write_chunk(dst_sb, buf, chunksize);
		if (IS_ERR(chunk)) {
			ret = PTR_ERR(chunk);
			break;
		}
		indexes[target_chunk] = chunk;
	}
	free(buf);
	if (!ret)
		delta->applied = true;
	return ret;
}

int erofs_file_delta_manager_finish(struct erofs_file_delta_manager *mgr)
{
	struct erofs_file_delta *delta;

	if (!mgr || !mgr->initialized)
		return 0;
	list_for_each_entry(delta, &mgr->deltas, list) {
		if (!delta->applied) {
			erofs_err("file-delta TARGET %s was not applied", delta->target);
			return -ENOENT;
		}
	}
	return 0;
}
