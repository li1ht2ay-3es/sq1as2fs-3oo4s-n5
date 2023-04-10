/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * fstree_from_dir.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "config.h"
#include "mkfs.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_WIN32) || defined(__WINDOWS__)
static int add_node(fstree_t *fs, tree_node_t *root,
		    scan_node_callback cb, void *user,
		    unsigned int flags,
		    const dir_entry_t *entry)
{
	size_t length = strlen(entry->name);
	tree_node_t *n;

	if (entry->name[0] == '.') {
		if (length == 1)
			return 0;

		if (entry->name[1] == '.' && length == 2)
			return 0;
	}

	if (S_ISDIR(entry->mode)) {
		if (flags & DIR_SCAN_NO_DIR)
			return 0;
	} else {
		if (flags & DIR_SCAN_NO_FILE)
			return 0;
	}

	n = calloc(1, sizeof(*n) + length + 1);
	if (n == NULL) {
		fprintf(stderr, "creating tree node: out-of-memory\n");
		return -1;
	}

	n->mode = entry->mode;
	n->name = (char *)n->payload;
	memcpy(n->name, entry->name, length);

	if (cb != NULL) {
		int ret = cb(user, fs, n);

		if (ret != 0) {
			free(n);
			return ret < 0 ? ret : 0;
		}
	}

	if (flags & DIR_SCAN_KEEP_TIME) {
		if (entry->mtime < 0) {
			n->mod_time = 0;
		} else if (entry->mtime > 0x0FFFFFFFFLL) {
			n->mod_time = 0xFFFFFFFF;
		} else {
			n->mod_time = entry->mtime;
		}
	} else {
		n->mod_time = fs->defaults.mtime;
	}

	fstree_insert_sorted(root, n);
	return 0;
}

static int scan_dir(fstree_t *fs, tree_node_t *root,
		    const char *path,
		    scan_node_callback cb, void *user,
		    unsigned int flags)
{
	dir_iterator_t *it = dir_iterator_create(path);

	if (it == NULL)
		return -1;

	for (;;) {
		dir_entry_t *ent;
		int ret;

		ret = it->next(it, &ent);
		if (ret > 0)
			break;
		if (ret < 0) {
			sqfs_perror(path, "reading directory entry", ret);
			sqfs_drop(it);
			return -1;
		}

		ret = add_node(fs, root, cb, user, flags, ent);
		free(ent);
		if (ret != 0) {
			sqfs_drop(it);
			return -1;
		}
	}

	sqfs_drop(it);
	return 0;
}

int fstree_from_dir(fstree_t *fs, tree_node_t *root,
		    const char *path, scan_node_callback cb,
		    void *user, unsigned int flags)
{
	tree_node_t *n;

	if (scan_dir(fs, root, path, cb, user, flags))
		return -1;

	if (flags & DIR_SCAN_NO_RECURSION)
		return 0;

	for (n = root->data.dir.children; n != NULL; n = n->next) {
		if (!S_ISDIR(n->mode))
			continue;

		if (fstree_from_subdir(fs, n, path, n->name, cb, user, flags))
			return -1;
	}

	return 0;
}

int fstree_from_subdir(fstree_t *fs, tree_node_t *root,
		       const char *path, const char *subdir,
		       scan_node_callback cb, void *user,
		       unsigned int flags)
{
	size_t plen, slen;
	char *temp = NULL;
	tree_node_t *n;

	plen = strlen(path);
	slen = subdir == NULL ? 0 : strlen(subdir);

	if (slen == 0)
		return fstree_from_dir(fs, root, path, cb, user, flags);

	temp = calloc(1, plen + 1 + slen + 1);
	if (temp == NULL) {
		fprintf(stderr, "%s/%s: allocation failure.\n", path, subdir);
		return -1;
	}

	memcpy(temp, path, plen);
	temp[plen] = '/';
	memcpy(temp + plen + 1, subdir, slen);
	temp[plen + 1 + slen] = '\0';

	if (scan_dir(fs, root, temp, cb, user, flags))
		goto fail;

	if (flags & DIR_SCAN_NO_RECURSION) {
		free(temp);
		return 0;
	}

	for (n = root->data.dir.children; n != NULL; n = n->next) {
		if (!S_ISDIR(n->mode))
			continue;

		if (fstree_from_subdir(fs, n, temp, n->name, cb, user, flags))
			goto fail;
	}

	free(temp);
	return 0;
fail:
	free(temp);
	return -1;

}
#else
static void discard_node(tree_node_t *root, tree_node_t *n)
{
	tree_node_t *it;

	if (n == root->data.dir.children) {
		root->data.dir.children = n->next;
	} else {
		it = root->data.dir.children;

		while (it != NULL && it->next != n)
			it = it->next;

		if (it != NULL)
			it->next = n->next;
	}

	free(n);
}

static int populate_dir(int dir_fd, fstree_t *fs, tree_node_t *root,
			dev_t devstart, scan_node_callback cb,
			void *user, unsigned int flags)
{
	char *extra = NULL;
	struct dirent *ent;
	int ret, childfd;
	struct stat sb;
	tree_node_t *n;
	DIR *dir;

	dir = fdopendir(dir_fd);
	if (dir == NULL) {
		perror("fdopendir");
		close(dir_fd);
		return -1;
	}

	/* XXX: fdopendir can dup and close dir_fd internally
	   and still be compliant with the spec. */
	dir_fd = dirfd(dir);

	for (;;) {
		errno = 0;
		ent = readdir(dir);

		if (ent == NULL) {
			if (errno) {
				perror("readdir");
				goto fail;
			}
			break;
		}

		if (!strcmp(ent->d_name, "..") || !strcmp(ent->d_name, "."))
			continue;

		if (fstatat(dir_fd, ent->d_name, &sb, AT_SYMLINK_NOFOLLOW)) {
			perror(ent->d_name);
			goto fail;
		}

		switch (sb.st_mode & S_IFMT) {
		case S_IFSOCK:
			if (flags & DIR_SCAN_NO_SOCK)
				continue;
			break;
		case S_IFLNK:
			if (flags & DIR_SCAN_NO_SLINK)
				continue;
			break;
		case S_IFREG:
			if (flags & DIR_SCAN_NO_FILE)
				continue;
			break;
		case S_IFBLK:
			if (flags & DIR_SCAN_NO_BLK)
				continue;
			break;
		case S_IFCHR:
			if (flags & DIR_SCAN_NO_CHR)
				continue;
			break;
		case S_IFIFO:
			if (flags & DIR_SCAN_NO_FIFO)
				continue;
			break;
		default:
			break;
		}

		if ((flags & DIR_SCAN_ONE_FILESYSTEM) && sb.st_dev != devstart)
			continue;

		if (S_ISLNK(sb.st_mode)) {
			size_t size;

			if ((sizeof(sb.st_size) > sizeof(size_t)) &&
			    sb.st_size > SIZE_MAX) {
				errno = EOVERFLOW;
				goto fail_rdlink;
			}

			if (SZ_ADD_OV((size_t)sb.st_size, 1, &size)) {
				errno = EOVERFLOW;
				goto fail_rdlink;
			}

			extra = calloc(1, size);
			if (extra == NULL)
				goto fail_rdlink;

			if (readlinkat(dir_fd, ent->d_name,
				       extra, (size_t)sb.st_size) < 0) {
				goto fail_rdlink;
			}

			extra[sb.st_size] = '\0';
		}

		if (!(flags & DIR_SCAN_KEEP_TIME))
			sb.st_mtime = fs->defaults.mtime;

		if (S_ISDIR(sb.st_mode) && (flags & DIR_SCAN_NO_DIR)) {
			n = fstree_get_node_by_path(fs, root, ent->d_name,
						    false, false);
			if (n == NULL)
				continue;

			ret = 0;
		} else {
			n = fstree_mknode(root, ent->d_name,
					  strlen(ent->d_name), extra, &sb);
			if (n == NULL) {
				perror("creating tree node");
				goto fail;
			}

			ret = (cb == NULL) ? 0 : cb(user, fs, n);
		}

		free(extra);
		extra = NULL;

		if (ret < 0)
			goto fail;

		if (ret > 0) {
			discard_node(root, n);
			continue;
		}

		if (S_ISDIR(n->mode) && !(flags & DIR_SCAN_NO_RECURSION)) {
			childfd = openat(dir_fd, n->name, O_DIRECTORY |
					 O_RDONLY | O_CLOEXEC);
			if (childfd < 0) {
				perror(n->name);
				goto fail;
			}

			if (populate_dir(childfd, fs, n, devstart,
					 cb, user, flags)) {
				goto fail;
			}
		}
	}

	closedir(dir);
	return 0;
fail_rdlink:
	perror("readlink");
fail:
	closedir(dir);
	free(extra);
	return -1;
}

int fstree_from_subdir(fstree_t *fs, tree_node_t *root,
		       const char *path, const char *subdir,
		       scan_node_callback cb, void *user,
		       unsigned int flags)
{
	struct stat sb;
	int fd, subfd;

	if (!S_ISDIR(root->mode)) {
		fprintf(stderr,
			"scanning %s/%s into %s: target is not a directory\n",
			path, subdir == NULL ? "" : subdir, root->name);
		return -1;
	}

	fd = open(path, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror(path);
		return -1;
	}

	if (subdir != NULL) {
		subfd = openat(fd, subdir, O_DIRECTORY | O_RDONLY | O_CLOEXEC);

		if (subfd < 0) {
			fprintf(stderr, "%s/%s: %s\n", path, subdir,
				strerror(errno));
			close(fd);
			return -1;
		}

		close(fd);
		fd = subfd;
	}

	if (fstat(fd, &sb)) {
		fprintf(stderr, "%s/%s: %s\n", path,
			subdir == NULL ? "" : subdir,
			strerror(errno));
		close(fd);
		return -1;
	}

	return populate_dir(fd, fs, root, sb.st_dev, cb, user, flags);
}

int fstree_from_dir(fstree_t *fs, tree_node_t *root,
		    const char *path, scan_node_callback cb,
		    void *user, unsigned int flags)
{
	return fstree_from_subdir(fs, root, path, NULL, cb, user, flags);
}
#endif
