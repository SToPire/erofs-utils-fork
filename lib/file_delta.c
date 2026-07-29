// SPDX-License-Identifier: GPL-2.0+ OR MIT
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <config.h>
#include "erofs/config.h"
#include "erofs/inode.h"
#include "erofs/print.h"
#include "erofs/xattr.h"
#include "liberofs_file_delta.h"
#include "liberofs_rebuild.h"

struct erofs_file_delta_vfile {
	struct erofs_vfile vf;
	struct erofs_file_delta *delta;
	struct erofs_vfile lower;
	u64 pos;
};

static bool erofs_file_delta_valid_target(const char *target)
{
	const char *component;

	if (!target || target[0] != '/' || !target[1])
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
	erofs_dm_persistent_close(&delta->dm);
	free(delta->target);
	free(delta->store_path);
	free(delta);
}

int erofs_file_delta_parse_spec(struct list_head *deltas, const char *arg)
{
	static const char format[] = "dm-persistent";
	struct erofs_file_delta *delta, *pos;
	const char *first, *second;
	size_t target_len;
	int ret;

	first = strchr(arg, ':');
	if (!first || (size_t)(first - arg) != sizeof(format) - 1 ||
	    memcmp(arg, format, sizeof(format) - 1))
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
	delta->dm.fd = -1;
	delta->target = strndup(first + 1, target_len);
	delta->store_path = strdup(second + 1);
	if (!delta->target || !delta->store_path) {
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

void erofs_file_delta_cleanup(struct list_head *deltas)
{
	struct erofs_file_delta *delta, *n;

	list_for_each_entry_safe(delta, n, deltas, list) {
		list_del(&delta->list);
		erofs_file_delta_free(delta);
	}
}

int erofs_file_delta_manager_init(struct erofs_file_delta_manager *mgr,
				  struct erofs_sb_info *sbi,
				  struct list_head *deltas)
{
	struct erofs_file_delta *delta;
	int ret;

	init_list_head(&mgr->deltas);
	mgr->initialized = true;
	list_splice_tail(deltas, &mgr->deltas);
	init_list_head(deltas);
	list_for_each_entry(delta, &mgr->deltas, list) {
		delta->lower_inode = (struct erofs_inode) { .sbi = sbi };
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
		if (delta->lower_inode.datalayout != EROFS_INODE_FLAT_PLAIN) {
			erofs_err("file-delta TARGET %s has unsupported datalayout %u",
				  delta->target, delta->lower_inode.datalayout);
			ret = -EOPNOTSUPP;
			goto err;
		}
		if (delta->lower_inode.i_nlink > 1) {
			erofs_err("file-delta TARGET %s is hard-linked",
				  delta->target);
			ret = -EOPNOTSUPP;
			goto err;
		}

		ret = erofs_dm_persistent_open(&delta->dm, delta->store_path,
					       delta->lower_inode.i_size);
		if (ret) {
			const char *kind = ret == -E2BIG ? "resource limit" :
				ret == -EINVAL ? "forward-delta policy" :
				"dm-persistent v1 format";

			erofs_err("file-delta STORE %s failed %s validation: %s",
				  delta->store_path, kind, erofs_strerror(ret));
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

struct erofs_file_delta *erofs_file_delta_lookup_target(
		struct erofs_file_delta_manager *mgr, const char *target)
{
	struct erofs_file_delta *delta;

	if (!mgr || !mgr->initialized)
		return NULL;
	list_for_each_entry(delta, &mgr->deltas, list)
		if (!strcmp(delta->target, target))
			return delta;
	return NULL;
}

static ssize_t erofs_file_delta_vfpread(struct erofs_vfile *vf, void *buf,
					size_t len, u64 offset)
{
	struct erofs_file_delta_vfile *dvf =
		container_of(vf, struct erofs_file_delta_vfile, vf);

	return erofs_dm_persistent_pread(&dvf->delta->dm, &dvf->lower,
					 buf, len, offset);
}

static ssize_t erofs_file_delta_vfread(struct erofs_vfile *vf, void *buf,
				       size_t len)
{
	struct erofs_file_delta_vfile *dvf =
		container_of(vf, struct erofs_file_delta_vfile, vf);
	ssize_t ret;

	ret = erofs_file_delta_vfpread(vf, buf, len, dvf->pos);
	if (ret > 0)
		dvf->pos += ret;
	return ret;
}

static off_t erofs_file_delta_vflseek(struct erofs_vfile *vf, u64 offset,
				      int whence)
{
	struct erofs_file_delta_vfile *dvf =
		container_of(vf, struct erofs_file_delta_vfile, vf);
	u64 size = dvf->delta->lower_inode.i_size;

	switch (whence) {
	case SEEK_SET:
		if (offset > size)
			return -EINVAL;
		dvf->pos = offset;
		break;
	case SEEK_CUR:
		if (offset > size - dvf->pos)
			return -EINVAL;
		dvf->pos += offset;
		break;
	case SEEK_END:
		if (offset)
			return -EINVAL;
		dvf->pos = size;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return dvf->pos;
}

static void erofs_file_delta_vfclose(struct erofs_vfile *vf)
{
	free(container_of(vf, struct erofs_file_delta_vfile, vf));
}

static struct erofs_vfops erofs_file_delta_vfops = {
	.pread = erofs_file_delta_vfpread,
	.read = erofs_file_delta_vfread,
	.lseek = erofs_file_delta_vflseek,
	.close = erofs_file_delta_vfclose,
};

struct erofs_vfile *erofs_file_delta_open_vfile(struct erofs_file_delta *delta)
{
	struct erofs_file_delta_vfile *dvf;
	int ret;

	dvf = calloc(1, sizeof(*dvf));
	if (!dvf)
		return ERR_PTR(-ENOMEM);
	dvf->vf.ops = &erofs_file_delta_vfops;
	dvf->vf.fd = -1;
	dvf->delta = delta;
	ret = erofs_iopen(&dvf->lower, &delta->lower_inode);
	if (ret) {
		free(dvf);
		return ERR_PTR(ret);
	}
	return &dvf->vf;
}

static bool erofs_file_delta_next_component(const char *current,
					    const char *target,
					    const char **component,
					    size_t *component_len,
					    bool *is_target)
{
	const char *relative = target + 1;
	const char *slash;
	size_t current_len = strlen(current);

	if (current_len) {
		if (strncmp(relative, current, current_len) ||
		    relative[current_len] != '/')
			return false;
		relative += current_len + 1;
	}
	if (!*relative)
		return false;
	slash = strchr(relative, '/');
	*component = relative;
	*component_len = slash ? (size_t)(slash - relative) : strlen(relative);
	*is_target = !slash;
	return true;
}

static struct erofs_inode *erofs_file_delta_clone_lower(
		struct erofs_importer *im, const char *image_path,
		struct erofs_inode *parent, struct erofs_file_delta *delta)
{
	struct erofs_inode *inode;
	u64 ino;
	int ret;

	inode = erofs_new_inode(im->sbi);
	if (IS_ERR(inode))
		return inode;
	ino = inode->i_ino[0];
	inode->nid = delta ? delta->lower_inode.nid : EROFS_NID_UNALLOCATED;
	if (!delta) {
		ret = erofs_ilookup(image_path, inode);
		if (ret)
			goto err;
	} else {
		ret = erofs_read_inode_from_disk(inode);
		if (ret)
			goto err;
	}
	if (asprintf(&inode->i_srcpath, "%s%s", im->params->source,
		     image_path) < 0) {
		ret = -ENOMEM;
		goto err;
	}
	ret = erofs_read_xattrs_from_disk(inode);
	if (ret)
		goto err;

	inode->i_ino[0] = ino;
	inode->i_ino[1] = inode->nid;
	inode->nid = EROFS_NID_UNALLOCATED;
	inode->dev = im->sbi->dev;
	inode->i_parent = parent;
	inode->i_nlink = S_ISDIR(inode->i_mode) ? 2 : 1;
	inode->datalayout = EROFS_INODE_FLAT_PLAIN;
	inode->incremental_copyup = true;
	inode->file_delta = delta;
	if (delta)
		inode->datasource = EROFS_INODE_DATA_SOURCE_FILE_DELTA;
	erofs_insert_ihash(inode);
	return inode;
err:
	erofs_iput(inode);
	return ERR_PTR(ret);
}

int erofs_file_delta_copyup_targets(struct erofs_importer *im,
				    struct erofs_inode *dir,
				    u64 *nr_subdirs,
				    unsigned int *i_nlink)
{
	struct erofs_file_delta_manager *mgr = im->file_deltas;
	const char *current = erofs_fspath(dir->i_srcpath);
	struct erofs_file_delta *delta;
	int ret;

	if (!mgr || !mgr->initialized)
		return 0;
	list_for_each_entry(delta, &mgr->deltas, list) {
		const char *component;
		struct erofs_inode lower = { .sbi = im->sbi };
		struct erofs_inode *inode;
		struct erofs_dentry *d;
		char *name, *image_path;
		size_t component_len;
		bool is_target;

		if (!erofs_file_delta_next_component(current, delta->target,
				&component, &component_len, &is_target))
			continue;
		name = strndup(component, component_len);
		if (!name)
			return -ENOMEM;
		d = erofs_d_lookup(dir, name);
		if (d) {
			if (is_target) {
				erofs_err("file-delta TARGET %s conflicts with SOURCE %s",
					  delta->target, d->inode->i_srcpath);
				free(name);
				return -EEXIST;
			}
			if (d->type != EROFS_FT_DIR) {
				free(name);
				return -ENOTDIR;
			}
			free(name);
			continue;
		}

		if (*current) {
			ret = asprintf(&image_path, "/%s/%s", current, name);
		} else {
			ret = asprintf(&image_path, "/%s", name);
		}
		if (ret < 0) {
			free(name);
			return -ENOMEM;
		}
		if (!is_target) {
			ret = erofs_ilookup(image_path, &lower);
			if (ret) {
				erofs_err("file-delta ancestor %s cannot be read: %s",
					  image_path, erofs_strerror(ret));
				goto out_free;
			}
			if (!S_ISDIR(lower.i_mode)) {
				ret = -ENOTDIR;
				goto out_free;
			}
		}
		inode = erofs_file_delta_clone_lower(im, image_path, dir,
						     is_target ? delta : NULL);
		if (IS_ERR(inode)) {
			ret = PTR_ERR(inode);
			goto out_free;
		}
		d = erofs_d_alloc(dir, name);
		if (IS_ERR(d)) {
			ret = PTR_ERR(d);
			erofs_iput(inode);
			goto out_free;
		}
		d->inode = inode;
		d->type = erofs_mode_to_ftype(inode->i_mode);
		++*nr_subdirs;
		*i_nlink += S_ISDIR(inode->i_mode);
		ret = 0;
out_free:
		free(image_path);
		free(name);
		if (ret)
			return ret;
	}
	return 0;
}
