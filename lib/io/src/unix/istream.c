/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * istream.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "../internal.h"

typedef struct {
	istream_t base;
	char *path;
	int fd;

	sqfs_u8 buffer[BUFSZ];
} file_istream_t;

static int file_precache(istream_t *strm)
{
	file_istream_t *file = (file_istream_t *)strm;

	assert(strm->buffer >= file->buffer);
	assert(strm->buffer <= (file->buffer + BUFSZ));
	assert(strm->buffer_used <= BUFSZ);
	assert((size_t)(strm->buffer - file->buffer) <=
	       (BUFSZ - strm->buffer_used));

	if (strm->buffer_used > 0)
		memmove(file->buffer, strm->buffer, strm->buffer_used);

	strm->buffer = file->buffer;

	while (!strm->eof && strm->buffer_used < BUFSZ) {
		ssize_t ret = read(file->fd, file->buffer + strm->buffer_used,
				   BUFSZ - strm->buffer_used);

		if (ret == 0)
			strm->eof = true;

		if (ret < 0) {
			if (errno == EINTR)
				continue;

			perror(file->path);
			return -1;
		}

		strm->buffer_used += ret;
	}

	return 0;
}

static const char *file_get_filename(istream_t *strm)
{
	file_istream_t *file = (file_istream_t *)strm;

	return file->path;
}

static void file_destroy(sqfs_object_t *obj)
{
	file_istream_t *file = (file_istream_t *)obj;

	if (file->fd != STDIN_FILENO)
		close(file->fd);

	free(file->path);
	free(file);
}

istream_t *istream_open_file(const char *path)
{
	file_istream_t *file = calloc(1, sizeof(*file));
	istream_t *strm = (istream_t *)file;

	if (file == NULL) {
		perror(path);
		return NULL;
	}

	sqfs_object_init(file, file_destroy, NULL);

	file->path = strdup(path);
	if (file->path == NULL) {
		perror(path);
		goto fail_free;
	}

	file->fd = open(path, O_RDONLY);
	if (file->fd < 0) {
		perror(path);
		goto fail_path;
	}

	strm->buffer = file->buffer;
	strm->precache = file_precache;
	strm->get_filename = file_get_filename;
	return strm;
fail_path:
	free(file->path);
fail_free:
	free(file);
	return NULL;
}

istream_t *istream_open_stdin(void)
{
	file_istream_t *file = calloc(1, sizeof(*file));
	istream_t *strm = (istream_t *)file;

	if (file == NULL)
		goto fail;

	sqfs_object_init(file, file_destroy, NULL);

	file->path = strdup("stdin");
	if (file->path == NULL)
		goto fail;

	file->fd = STDIN_FILENO;
	strm->buffer = file->buffer;
	strm->precache = file_precache;
	strm->get_filename = file_get_filename;
	return strm;
fail:
	perror("creating file wrapper for stdin");
	free(file);
	return NULL;
}
