// SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "erofs/config.h"
#include "erofs/hotfile.h"
#include "erofs/print.h"

struct erofs_hotfile_manifest {
	struct erofs_hotfile_entry {
		char *path;
		unsigned int rank;
	} *file_paths;
	unsigned int nr_files;
	unsigned int cap_files;
	struct erofs_hotfile_entry *dir_paths;
	unsigned int nr_dirs;
	unsigned int cap_dirs;
};

static struct erofs_hotfile_manifest hotfile_manifest;

static bool erofs_hotfile_is_local_rootfs(char **root)
{
	struct stat st;

	if (!cfg.c_src_path)
		return false;
	if (stat(cfg.c_src_path, &st))
		return false;
	if (!S_ISDIR(st.st_mode))
		return false;
	if (root)
		*root = cfg.c_src_path;
	return true;
}

static int erofs_hotfile_cmp(const void *a, const void *b)
{
	const struct erofs_hotfile_entry *lhs = a;
	const struct erofs_hotfile_entry *rhs = b;

	return strcmp(lhs->path, rhs->path);
}

static int erofs_hotfile_path_cmp(const void *a, const void *b)
{
	const char *lhs = a;
	const struct erofs_hotfile_entry *rhs = b;

	return strcmp(lhs, rhs->path);
}

static char *erofs_hotfile_normalize(const char *path)
{
	const unsigned char *src = (const unsigned char *)path;
	const unsigned char *end;
	char *out;
	size_t len = 0;
	bool slash = false;

	while (*src && isspace(*src))
		++src;
	end = src + strlen((const char *)src);
	while (end > src && isspace(end[-1]))
		--end;

	while (src < end && *src == '/')
		++src;

	out = malloc((end - src) + 1);
	if (!out)
		return NULL;

	while (src < end) {
		if (*src == '/') {
			if (!slash)
				out[len++] = '/';
			slash = true;
		} else {
			out[len++] = *src;
			slash = false;
		}
		++src;
	}

	while (len > 0 && out[len - 1] == '/')
		--len;
	out[len] = '\0';
	return out;
}

static bool erofs_hotfile_line_is_dir(const char *path)
{
	const unsigned char *src = (const unsigned char *)path;
	const unsigned char *end;

	while (*src && isspace(*src))
		++src;
	if (*src == '#')
		return false;

	end = src + strlen((const char *)src);
	while (end > src && isspace(end[-1]))
		--end;

	return end > src && end[-1] == '/';
}

static int erofs_hotfile_add_file(char *path, unsigned int rank);
static int erofs_hotfile_add_dir(char *path, unsigned int rank);
static int erofs_hotfile_collect_parent_dirs(const char *path,
					     unsigned int rank);

static char *erofs_hotfile_clean_path(const char *path)
{
	char **stack;
	char *normalized, *token, *saveptr;
	char *out;
	size_t len, depth = 0, i, pos = 0;

	normalized = erofs_hotfile_normalize(path);
	if (!normalized)
		return NULL;

	len = strlen(normalized);
	stack = calloc(len + 1, sizeof(*stack));
	if (!stack) {
		free(normalized);
		return NULL;
	}

	for (token = strtok_r(normalized, "/", &saveptr); token;
	     token = strtok_r(NULL, "/", &saveptr)) {
		if (!strcmp(token, "."))
			continue;
		if (!strcmp(token, "..")) {
			if (!depth) {
				free(stack);
				free(normalized);
				errno = EXDEV;
				return NULL;
			}
			--depth;
			continue;
		}
		stack[depth++] = token;
	}

	out = malloc(len + 1);
	if (!out) {
		free(stack);
		free(normalized);
		return NULL;
	}

	for (i = 0; i < depth; ++i) {
		size_t n = strlen(stack[i]);

		if (i)
			out[pos++] = '/';
		memcpy(out + pos, stack[i], n);
		pos += n;
	}
	out[pos] = '\0';
	free(stack);
	free(normalized);
	return out;
}

static char *erofs_hotfile_join_symlink_target(const char *linkpath,
					       const char *target,
					       const char *suffix)
{
	char *joined, *parent = NULL, *slash;
	int ret;

	if (target[0] == '/') {
		char *cleaned;

		ret = asprintf(&joined, "%s%s%s", target,
			       suffix && suffix[0] ? "/" : "",
			       suffix && suffix[0] ? suffix : "");
		if (ret < 0)
			return NULL;
		cleaned = erofs_hotfile_clean_path(joined);
		free(joined);
		return cleaned;
	}

	parent = strdup(linkpath);
	if (!parent)
		return NULL;
	slash = strrchr(parent, '/');
	if (slash)
		*slash = '\0';
	else
		parent[0] = '\0';

	ret = asprintf(&joined, "%s%s%s%s%s",
		       parent,
		       parent[0] ? "/" : "",
		       target,
		       suffix && suffix[0] ? "/" : "",
		       suffix && suffix[0] ? suffix : "");
	free(parent);
	if (ret < 0)
		return NULL;
	parent = erofs_hotfile_clean_path(joined);
	free(joined);
	return parent;
}

static int erofs_hotfile_add_file_path(const char *path, unsigned int rank)
{
	char *dup;
	int err;

	dup = strdup(path);
	if (!dup)
		return -ENOMEM;
	err = erofs_hotfile_add_file(dup, rank);
	if (err) {
		free(dup);
		return err;
	}
	return erofs_hotfile_collect_parent_dirs(path, rank);
}

static int erofs_hotfile_add_dir_path(const char *path, unsigned int rank)
{
	char *dup;
	int err;

	dup = strdup(path);
	if (!dup)
		return -ENOMEM;
	err = erofs_hotfile_add_dir(dup, rank);
	if (err) {
		free(dup);
		return err;
	}
	return erofs_hotfile_collect_parent_dirs(path, rank);
}

static int erofs_hotfile_add_plain_path(const char *path, bool dir_entry,
					unsigned int rank)
{
	if (!path[0])
		return 0;
	return dir_entry ? erofs_hotfile_add_dir_path(path, rank) :
		erofs_hotfile_add_file_path(path, rank);
}

static int erofs_hotfile_add_resolved_path(const char *path, bool dir_entry,
					   unsigned int rank)
{
	char *root, *current;
	int depth, err;

	if (!path[0])
		return 0;

	if (!erofs_hotfile_is_local_rootfs(&root))
		return erofs_hotfile_add_plain_path(path, dir_entry, rank);

	current = erofs_hotfile_clean_path(path);
	if (!current)
		return erofs_hotfile_add_plain_path(path, dir_entry, rank);

	for (depth = 0; depth < 32; ++depth) {
		char *walk, *saveptr, *part, *prefix = NULL;
		bool redirected = false;
		size_t prefix_len = 0;

		walk = strdup(current);
		if (!walk) {
			free(current);
			return -ENOMEM;
		}

		for (part = strtok_r(walk, "/", &saveptr); part;
		     part = strtok_r(NULL, "/", &saveptr)) {
			char *fullpath, *next, *target;
			const char *suffix = saveptr;
			struct stat st;

			if (prefix_len) {
				if (asprintf(&next, "%s/%s", prefix, part) < 0) {
					free(prefix);
					free(walk);
					free(current);
					return -ENOMEM;
				}
				free(prefix);
				prefix = next;
			} else {
				prefix = strdup(part);
				if (!prefix) {
					free(walk);
					free(current);
					return -ENOMEM;
				}
			}
			prefix_len = strlen(prefix);

			if (asprintf(&fullpath, "%s/%s", root, prefix) < 0) {
				free(prefix);
				free(walk);
				free(current);
				return -ENOMEM;
			}
			if (lstat(fullpath, &st)) {
				free(fullpath);
				free(prefix);
				free(walk);
				err = erofs_hotfile_add_plain_path(current,
								   dir_entry,
								   rank);
				free(current);
				return err;
			}
			if (!S_ISLNK(st.st_mode)) {
				free(fullpath);
				continue;
			}

			err = erofs_hotfile_add_file_path(prefix, rank);
			if (err) {
				free(fullpath);
				free(prefix);
				free(walk);
				free(current);
				return err;
			}

			target = malloc(st.st_size + 1);
			if (!target) {
				free(fullpath);
				free(prefix);
				free(walk);
				free(current);
				return -ENOMEM;
			}
			err = readlink(fullpath, target, st.st_size);
			free(fullpath);
			if (err < 0) {
				err = -errno;
				free(target);
				free(prefix);
				free(walk);
				free(current);
				return err;
			}
			target[err] = '\0';

			next = erofs_hotfile_join_symlink_target(prefix, target,
								 suffix);
			free(target);
			free(prefix);
			free(walk);
			free(current);
			if (!next)
				return -ENOMEM;
			current = next;
			redirected = true;
			break;
		}

		if (!redirected) {
			free(prefix);
			free(walk);
			err = erofs_hotfile_add_plain_path(current, dir_entry,
							   rank);
			free(current);
			return err;
		}
	}
	free(current);
	return -ELOOP;
}

static int erofs_hotfile_add(struct erofs_hotfile_entry **paths,
			     unsigned int *nr, unsigned int *cap,
			     char *path, unsigned int rank)
{
	struct erofs_hotfile_entry *npaths;

	if (*nr >= *cap) {
		unsigned int ncap = *cap ? *cap * 2 : 64;

		npaths = realloc(*paths, ncap * sizeof(*npaths));
		if (!npaths)
			return -ENOMEM;
		*paths = npaths;
		*cap = ncap;
	}
	(*paths)[*nr].path = path;
	(*paths)[*nr].rank = rank;
	++(*nr);
	return 0;
}

static int erofs_hotfile_add_file(char *path, unsigned int rank)
{
	return erofs_hotfile_add(&hotfile_manifest.file_paths,
				 &hotfile_manifest.nr_files,
				 &hotfile_manifest.cap_files,
				 path, rank);
}

static int erofs_hotfile_add_dir(char *path, unsigned int rank)
{
	return erofs_hotfile_add(&hotfile_manifest.dir_paths,
				 &hotfile_manifest.nr_dirs,
				 &hotfile_manifest.cap_dirs,
				 path, rank);
}

static int erofs_hotfile_collect_parent_dirs(const char *path,
					     unsigned int rank)
{
	char *dir = strdup(path);
	int err = 0;

	if (!dir)
		return -ENOMEM;

	while (1) {
		char *slash = strrchr(dir, '/');

		if (!slash)
			break;
		*slash = '\0';
		if (!dir[0])
			break;

		slash = strdup(dir);
		if (!slash) {
			err = -ENOMEM;
			break;
		}
		err = erofs_hotfile_add_dir(slash, rank);
		if (err)
			break;
	}
	free(dir);
	return err;
}

static unsigned int erofs_hotfile_sort_dedupe(struct erofs_hotfile_entry *paths,
					      unsigned int nr)
{
	unsigned int i, out = 0;

	if (!nr)
		return 0;

	qsort(paths, nr, sizeof(paths[0]), erofs_hotfile_cmp);
	for (i = 0; i < nr; ++i) {
		if (out && !strcmp(paths[out - 1].path, paths[i].path)) {
			if (paths[i].rank < paths[out - 1].rank) {
				free(paths[out - 1].path);
				paths[out - 1] = paths[i];
			} else {
				free(paths[i].path);
			}
			continue;
		}
		paths[out++] = paths[i];
	}
	return out;
}

int erofs_hotfile_load(const char *path)
{
	FILE *fp;
	char *line = NULL;
	size_t linesz = 0;
	ssize_t nread;
	unsigned int rank = 0;
	int err = 0;

	fp = fopen(path, "r");
	if (!fp)
		return -errno;

	while ((nread = getline(&line, &linesz, fp)) >= 0) {
		char *normalized;
		bool dir_entry;

		if (!nread)
			continue;
		dir_entry = erofs_hotfile_line_is_dir(line);
		normalized = erofs_hotfile_normalize(line);
		if (!normalized) {
			err = -ENOMEM;
			break;
		}
		if ((!normalized[0] && !dir_entry) || normalized[0] == '#') {
			free(normalized);
			continue;
		}
		err = erofs_hotfile_add_resolved_path(normalized, dir_entry,
						      rank);
		free(normalized);
		if (err)
			break;
		++rank;
	}
	free(line);
	fclose(fp);
	if (err)
		goto err_out;

	if (!hotfile_manifest.nr_files && !hotfile_manifest.nr_dirs)
		return 0;

	hotfile_manifest.nr_files = erofs_hotfile_sort_dedupe(
		hotfile_manifest.file_paths, hotfile_manifest.nr_files);
	hotfile_manifest.nr_dirs = erofs_hotfile_sort_dedupe(
		hotfile_manifest.dir_paths, hotfile_manifest.nr_dirs);
	erofs_info("loaded %u hot files and %u hot ancestor dirs from %s",
		   hotfile_manifest.nr_files, hotfile_manifest.nr_dirs, path);
	return 0;

err_out:
	erofs_hotfile_exit();
	return err;
}

bool erofs_hotfile_enabled(void)
{
	return hotfile_manifest.nr_files || hotfile_manifest.nr_dirs;
}

unsigned int erofs_get_hot_file_rank(const char *path)
{
	char *normalized;
	struct erofs_hotfile_entry *found;
	const char *fspath;
	unsigned int rank = EROFS_HOT_RANK_NONE;

	if (!erofs_hotfile_enabled())
		return EROFS_HOT_RANK_NONE;

	fspath = erofs_fspath(path);
	normalized = erofs_hotfile_normalize(fspath);
	if (!normalized)
		return EROFS_HOT_RANK_NONE;

	found = bsearch(normalized, hotfile_manifest.file_paths,
			hotfile_manifest.nr_files,
			sizeof(hotfile_manifest.file_paths[0]),
			erofs_hotfile_path_cmp);
	if (found)
		rank = found->rank;

	if (rank != 0 && hotfile_manifest.nr_files) {
		char *root = NULL;
		char *fullpath = NULL;
		struct stat st;

		if (erofs_hotfile_is_local_rootfs(&root) &&
		    asprintf(&fullpath, "%s/%s", root, normalized) >= 0 &&
		    lstat(fullpath, &st) == 0 && S_ISLNK(st.st_mode)) {
			char *resolved = realpath(fullpath, NULL);

			if (resolved && !strncmp(resolved, root, strlen(root)) &&
			    resolved[strlen(root)] == '/') {
				char *resolved_norm =
					erofs_hotfile_normalize(resolved + strlen(root));

				if (resolved_norm) {
					struct erofs_hotfile_entry *resolved_found;

					resolved_found = bsearch(resolved_norm,
						hotfile_manifest.file_paths,
						hotfile_manifest.nr_files,
						sizeof(hotfile_manifest.file_paths[0]),
						erofs_hotfile_path_cmp);
					if (resolved_found && resolved_found->rank < rank)
						rank = resolved_found->rank;
					free(resolved_norm);
				}
			}
			free(resolved);
		}
		free(fullpath);
	}
	free(normalized);
	return rank;
}

unsigned int erofs_get_hot_dir_rank(const char *path)
{
	char *normalized;
	struct erofs_hotfile_entry *found;
	const char *fspath;

	if (!erofs_hotfile_enabled())
		return EROFS_HOT_RANK_NONE;

	fspath = erofs_fspath(path);
	normalized = erofs_hotfile_normalize(fspath);
	if (!normalized)
		return EROFS_HOT_RANK_NONE;

	found = bsearch(normalized, hotfile_manifest.dir_paths,
			hotfile_manifest.nr_dirs,
			sizeof(hotfile_manifest.dir_paths[0]),
			erofs_hotfile_path_cmp);
	free(normalized);
	return found ? found->rank : EROFS_HOT_RANK_NONE;
}

void erofs_hotfile_exit(void)
{
	unsigned int i;

	for (i = 0; i < hotfile_manifest.nr_files; ++i)
		free(hotfile_manifest.file_paths[i].path);
	for (i = 0; i < hotfile_manifest.nr_dirs; ++i)
		free(hotfile_manifest.dir_paths[i].path);
	free(hotfile_manifest.file_paths);
	free(hotfile_manifest.dir_paths);
	hotfile_manifest.file_paths = NULL;
	hotfile_manifest.nr_files = 0;
	hotfile_manifest.cap_files = 0;
	hotfile_manifest.dir_paths = NULL;
	hotfile_manifest.nr_dirs = 0;
	hotfile_manifest.cap_dirs = 0;
}
