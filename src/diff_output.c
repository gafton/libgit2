/*
 * Copyright (C) 2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include "common.h"
#include "git2/diff.h"
#include "git2/attr.h"
#include "git2/blob.h"
#include "xdiff/xdiff.h"
#include <ctype.h>
#include "diff.h"
#include "map.h"
#include "fileops.h"

typedef struct {
	git_diff_list *diff;
	void *cb_data;
	git_diff_hunk_fn hunk_cb;
	git_diff_line_fn line_cb;
	unsigned int index;
	git_diff_delta *delta;
} diff_output_info;

static int read_next_int(const char **str, int *value)
{
	const char *scan = *str;
	int v = 0, digits = 0;
	/* find next digit */
	for (scan = *str; *scan && !isdigit(*scan); scan++);
	/* parse next number */
	for (; isdigit(*scan); scan++, digits++)
		v = (v * 10) + (*scan - '0');
	*str = scan;
	*value = v;
	return (digits > 0) ? GIT_SUCCESS : GIT_ENOTFOUND;
}

static int diff_output_cb(void *priv, mmbuffer_t *bufs, int len)
{
	int err = GIT_SUCCESS;
	diff_output_info *info = priv;

	if (len == 1 && info->hunk_cb) {
		git_diff_range range = { -1, 0, -1, 0 };

		/* expect something of the form "@@ -%d[,%d] +%d[,%d] @@" */
		if (bufs[0].ptr[0] == '@') {
			const char *scan = bufs[0].ptr;
			if (!(err = read_next_int(&scan, &range.old_start)) && *scan == ',')
				err = read_next_int(&scan, &range.old_lines);
			if (!err &&
				!(err = read_next_int(&scan, &range.new_start)) && *scan == ',')
				err = read_next_int(&scan, &range.new_lines);
			if (!err && range.old_start >= 0 && range.new_start >= 0)
				err = info->hunk_cb(
					info->cb_data, info->delta, &range, bufs[0].ptr, bufs[0].size);
		}
	}
	else if ((len == 2 || len == 3) && info->line_cb) {
		int origin;

		/* expect " "/"-"/"+", then data, then maybe newline */
		origin =
			(*bufs[0].ptr == '+') ? GIT_DIFF_LINE_ADDITION :
			(*bufs[0].ptr == '-') ? GIT_DIFF_LINE_DELETION :
			GIT_DIFF_LINE_CONTEXT;

		err = info->line_cb(
			info->cb_data, info->delta, origin, bufs[1].ptr, bufs[1].size);

		/* deal with adding and removing newline at EOF */
		if (err == GIT_SUCCESS && len == 3) {
			if (origin == GIT_DIFF_LINE_ADDITION)
				origin = GIT_DIFF_LINE_ADD_EOFNL;
			else
				origin = GIT_DIFF_LINE_DEL_EOFNL;

			err = info->line_cb(
				info->cb_data, info->delta, origin, bufs[2].ptr, bufs[2].size);
		}
	}

	return err;
}

#define BINARY_DIFF_FLAGS (GIT_DIFF_FILE_BINARY|GIT_DIFF_FILE_NOT_BINARY)

static int set_file_is_binary_by_attr(git_repository *repo, git_diff_file *file)
{
	const char *value;
	int error = git_attr_get(repo, file->path, "diff", &value);
	if (error != GIT_SUCCESS)
		return error;
	if (GIT_ATTR_FALSE(value))
		file->flags |= GIT_DIFF_FILE_BINARY;
	else if (GIT_ATTR_TRUE(value))
		file->flags |= GIT_DIFF_FILE_NOT_BINARY;
	/* otherwise leave file->flags alone */
	return error;
}

static void set_delta_is_binary(git_diff_delta *delta)
{
	if ((delta->old.flags & GIT_DIFF_FILE_BINARY) != 0 ||
		(delta->new.flags & GIT_DIFF_FILE_BINARY) != 0)
		delta->binary = 1;
	else if ((delta->old.flags & GIT_DIFF_FILE_NOT_BINARY) != 0 ||
			 (delta->new.flags & GIT_DIFF_FILE_NOT_BINARY) != 0)
		delta->binary = 0;
	/* otherwise leave delta->binary value untouched */
}

static int file_is_binary_by_attr(
	git_diff_list *diff,
	git_diff_delta *delta)
{
	int error, mirror_new;

	delta->binary = -1;

	/* make sure files are conceivably mmap-able */
	if ((git_off_t)((size_t)delta->old.size) != delta->old.size ||
		(git_off_t)((size_t)delta->new.size) != delta->new.size)
	{
		delta->old.flags |= GIT_DIFF_FILE_BINARY;
		delta->new.flags |= GIT_DIFF_FILE_BINARY;
		delta->binary = 1;
		return GIT_SUCCESS;
	}

	/* check if user is forcing us to text diff these files */
	if (diff->opts.flags & GIT_DIFF_FORCE_TEXT) {
		delta->old.flags |= GIT_DIFF_FILE_NOT_BINARY;
		delta->new.flags |= GIT_DIFF_FILE_NOT_BINARY;
		delta->binary = 0;
		return GIT_SUCCESS;
	}

	/* check diff attribute +, -, or 0 */
	error = set_file_is_binary_by_attr(diff->repo, &delta->old);
	if (error != GIT_SUCCESS)
		return error;

	mirror_new = (delta->new.path == delta->old.path ||
				  strcmp(delta->new.path, delta->old.path) == 0);
	if (mirror_new)
		delta->new.flags &= (delta->old.flags & BINARY_DIFF_FLAGS);
	else
		error = set_file_is_binary_by_attr(diff->repo, &delta->new);

	set_delta_is_binary(delta);

	return error;
}

static int file_is_binary_by_content(
	git_diff_list *diff,
	git_diff_delta *delta,
	git_map *old_data,
	git_map *new_data)
{
	GIT_UNUSED(diff);

	if ((delta->old.flags & BINARY_DIFF_FLAGS) == 0) {
		size_t search_len = min(old_data->len, 4000);
		if (strnlen(old_data->data, search_len) != search_len)
			delta->old.flags |= GIT_DIFF_FILE_BINARY;
		else
			delta->old.flags |= GIT_DIFF_FILE_NOT_BINARY;
	}

	if ((delta->new.flags & BINARY_DIFF_FLAGS) == 0) {
		size_t search_len = min(new_data->len, 4000);
		if (strnlen(new_data->data, search_len) != search_len)
			delta->new.flags |= GIT_DIFF_FILE_BINARY;
		else
			delta->new.flags |= GIT_DIFF_FILE_NOT_BINARY;
	}

	set_delta_is_binary(delta);

	/* TODO: if value != NULL, implement diff drivers */

	return GIT_SUCCESS;
}

static void setup_xdiff_options(
	git_diff_options *opts, xdemitconf_t *cfg, xpparam_t *param)
{
	memset(cfg, 0, sizeof(xdemitconf_t));
	memset(param, 0, sizeof(xpparam_t));

	cfg->ctxlen =
		(!opts || !opts->context_lines) ? 3 : opts->context_lines;
	cfg->interhunkctxlen =
		(!opts || !opts->interhunk_lines) ? 3 : opts->interhunk_lines;

	if (!opts)
		return;

	if (opts->flags & GIT_DIFF_IGNORE_WHITESPACE)
		param->flags |= XDF_WHITESPACE_FLAGS;
	if (opts->flags & GIT_DIFF_IGNORE_WHITESPACE_CHANGE)
		param->flags |= XDF_IGNORE_WHITESPACE_CHANGE;
	if (opts->flags & GIT_DIFF_IGNORE_WHITESPACE_EOL)
		param->flags |= XDF_IGNORE_WHITESPACE_AT_EOL;
}

static int get_blob_content(
	git_repository *repo,
	const git_oid *oid,
	git_map *map,
	git_blob **blob)
{
	int error;

	if (git_oid_iszero(oid))
		return GIT_SUCCESS;

	if ((error = git_blob_lookup(blob, repo, oid)) == GIT_SUCCESS) {
		map->data = (void *)git_blob_rawcontent(*blob);
		map->len  = git_blob_rawsize(*blob);
	}

	return error;
}

static int get_workdir_content(
	git_repository *repo,
	git_diff_file *file,
	git_map *map)
{
	git_buf full_path = GIT_BUF_INIT;
	int error = git_buf_joinpath(
		&full_path, git_repository_workdir(repo), file->path);
	if (error != GIT_SUCCESS)
		return error;

	if (S_ISLNK(file->mode)) {
		file->flags |= GIT_DIFF_FILE_FREE_DATA;
		file->flags |= GIT_DIFF_FILE_BINARY;

		map->data = git__malloc((size_t)file->size + 1);
		if (map->data == NULL)
			error = GIT_ENOMEM;
		else {
			ssize_t read_len =
				p_readlink(full_path.ptr, map->data, (size_t)file->size + 1);
			if (read_len != (ssize_t)file->size)
				error = git__throw(
					GIT_EOSERR, "Failed to read symlink %s", file->path);
			else
				map->len = read_len;

		}
	}
	else {
		error = git_futils_mmap_ro_file(map, full_path.ptr);
		file->flags |= GIT_DIFF_FILE_UNMAP_DATA;
	}
	git_buf_free(&full_path);
	return error;
}

static void release_content(git_diff_file *file, git_map *map, git_blob *blob)
{
	if (blob != NULL)
		git_blob_free(blob);

	if (file->flags & GIT_DIFF_FILE_FREE_DATA) {
		git__free(map->data);
		map->data = NULL;
		file->flags &= ~GIT_DIFF_FILE_FREE_DATA;
	}
	else if (file->flags & GIT_DIFF_FILE_UNMAP_DATA) {
		git_futils_mmap_free(map);
		map->data = NULL;
		file->flags &= ~GIT_DIFF_FILE_UNMAP_DATA;
	}
}

int git_diff_foreach(
	git_diff_list *diff,
	void *data,
	git_diff_file_fn file_cb,
	git_diff_hunk_fn hunk_cb,
	git_diff_line_fn line_cb)
{
	int error = GIT_SUCCESS;
	diff_output_info info;
	git_diff_delta *delta;
	xpparam_t    xdiff_params;
	xdemitconf_t xdiff_config;
	xdemitcb_t   xdiff_callback;

	info.diff    = diff;
	info.cb_data = data;
	info.hunk_cb = hunk_cb;
	info.line_cb = line_cb;

	setup_xdiff_options(&diff->opts, &xdiff_config, &xdiff_params);
	memset(&xdiff_callback, 0, sizeof(xdiff_callback));
	xdiff_callback.outf = diff_output_cb;
	xdiff_callback.priv = &info;

	git_vector_foreach(&diff->deltas, info.index, delta) {
		git_blob *old_blob = NULL, *new_blob = NULL;
		git_map old_data, new_data;

		if (delta->status == GIT_DELTA_UNMODIFIED)
			continue;

		if (delta->status == GIT_DELTA_IGNORED &&
			(diff->opts.flags & GIT_DIFF_INCLUDE_IGNORED) == 0)
			continue;

		if (delta->status == GIT_DELTA_UNTRACKED &&
			(diff->opts.flags & GIT_DIFF_INCLUDE_UNTRACKED) == 0)
			continue;

		error = file_is_binary_by_attr(diff, delta);
		if (error < GIT_SUCCESS)
			goto cleanup;

		old_data.data = "";
		old_data.len = 0;
		new_data.data = "";
		new_data.len  = 0;

		/* TODO: Partial blob reading to defer loading whole blob.
		 * I.e. I want a blob with just the first 4kb loaded, then
		 * later on I will read the rest of the blob if needed.
		 */

		/* map files */
		if (delta->binary != 1 &&
			(hunk_cb || line_cb) &&
			(delta->status == GIT_DELTA_DELETED ||
			 delta->status == GIT_DELTA_MODIFIED))
		{
			if (diff->old_src == GIT_ITERATOR_WORKDIR)
				error = get_workdir_content(diff->repo, &delta->old, &old_data);
			else
				error = get_blob_content(
					diff->repo, &delta->old.oid, &old_data, &old_blob);
			if (error != GIT_SUCCESS)
				goto cleanup;
		}

		if (delta->binary != 1 &&
			(hunk_cb || line_cb || git_oid_iszero(&delta->new.oid)) &&
			(delta->status == GIT_DELTA_ADDED ||
			 delta->status == GIT_DELTA_MODIFIED))
		{
			if (diff->new_src == GIT_ITERATOR_WORKDIR)
				error = get_workdir_content(diff->repo, &delta->new, &new_data);
			else
				error = get_blob_content(
					diff->repo, &delta->new.oid, &new_data, &new_blob);
			if (error != GIT_SUCCESS)
				goto cleanup;

			if ((delta->new.flags | GIT_DIFF_FILE_VALID_OID) == 0) {
				error = git_odb_hash(
					&delta->new.oid, new_data.data, new_data.len, GIT_OBJ_BLOB);
				if (error != GIT_SUCCESS)
					goto cleanup;

				/* since we did not have the definitive oid, we may have
				 * incorrect status and need to skip this item.
				 */
				if (git_oid_cmp(&delta->old.oid, &delta->new.oid) == 0) {
					delta->status = GIT_DELTA_UNMODIFIED;
					goto cleanup;
				}
			}
		}

		/* if we have not already decided whether file is binary,
		 * check the first 4K for nul bytes to decide...
		 */
		if (delta->binary == -1) {
			error = file_is_binary_by_content(
				diff, delta, &old_data, &new_data);
			if (error < GIT_SUCCESS)
				goto cleanup;
		}

		/* TODO: if ignore_whitespace is set, then we *must* do text
		 * diffs to tell if a file has really been changed.
		 */

		if (file_cb != NULL) {
			error = file_cb(data, delta, (float)info.index / diff->deltas.length);
			if (error != GIT_SUCCESS)
				goto cleanup;
		}

		/* don't do hunk and line diffs if file is binary */
		if (delta->binary == 1)
			goto cleanup;

		/* nothing to do if we did not get data */
		if (!old_data.len && !new_data.len)
			goto cleanup;

		assert(hunk_cb || line_cb);

		info.delta = delta;

		xdl_diff((mmfile_t *)&old_data, (mmfile_t *)&new_data,
			&xdiff_params, &xdiff_config, &xdiff_callback);

cleanup:
		release_content(&delta->old, &old_data, old_blob);
		release_content(&delta->new, &new_data, new_blob);

		if (error != GIT_SUCCESS)
			break;
	}

	return error;
}


typedef struct {
	git_diff_list *diff;
	git_diff_output_fn print_cb;
	void *cb_data;
	git_buf *buf;
} diff_print_info;

static char pick_suffix(int mode)
{
	if (S_ISDIR(mode))
		return '/';
	else if (mode & 0100)
		/* in git, modes are very regular, so we must have 0100755 mode */
		return '*';
	else
		return ' ';
}

static int print_compact(void *data, git_diff_delta *delta, float progress)
{
	diff_print_info *pi = data;
	char code, old_suffix, new_suffix;

	GIT_UNUSED(progress);

	switch (delta->status) {
	case GIT_DELTA_ADDED: code = 'A'; break;
	case GIT_DELTA_DELETED: code = 'D'; break;
	case GIT_DELTA_MODIFIED: code = 'M'; break;
	case GIT_DELTA_RENAMED: code = 'R'; break;
	case GIT_DELTA_COPIED: code = 'C'; break;
	case GIT_DELTA_IGNORED: code = 'I'; break;
	case GIT_DELTA_UNTRACKED: code = '?'; break;
	default: code = 0;
	}

	if (!code)
		return GIT_SUCCESS;

	old_suffix = pick_suffix(delta->old.mode);
	new_suffix = pick_suffix(delta->new.mode);

	git_buf_clear(pi->buf);

	if (delta->old.path != delta->new.path &&
		strcmp(delta->old.path,delta->new.path) != 0)
		git_buf_printf(pi->buf, "%c\t%s%c -> %s%c\n", code,
			delta->old.path, old_suffix, delta->new.path, new_suffix);
	else if (delta->old.mode != delta->new.mode &&
		delta->old.mode != 0 && delta->new.mode != 0)
		git_buf_printf(pi->buf, "%c\t%s%c (%o -> %o)\n", code,
			delta->old.path, new_suffix, delta->old.mode, delta->new.mode);
	else if (old_suffix != ' ')
		git_buf_printf(pi->buf, "%c\t%s%c\n", code, delta->old.path, old_suffix);
	else
		git_buf_printf(pi->buf, "%c\t%s\n", code, delta->old.path);

	if (git_buf_lasterror(pi->buf) != GIT_SUCCESS)
		return git_buf_lasterror(pi->buf);

	return pi->print_cb(pi->cb_data, GIT_DIFF_LINE_FILE_HDR, pi->buf->ptr);
}

int git_diff_print_compact(
	git_diff_list *diff,
	void *cb_data,
	git_diff_output_fn print_cb)
{
	int error;
	git_buf buf = GIT_BUF_INIT;
	diff_print_info pi;

	pi.diff     = diff;
	pi.print_cb = print_cb;
	pi.cb_data  = cb_data;
	pi.buf      = &buf;

	error = git_diff_foreach(diff, &pi, print_compact, NULL, NULL);

	git_buf_free(&buf);

	return error;
}


static int print_oid_range(diff_print_info *pi, git_diff_delta *delta)
{
	char start_oid[8], end_oid[8];

	/* TODO: Determine a good actual OID range to print */
	git_oid_to_string(start_oid, sizeof(start_oid), &delta->old.oid);
	git_oid_to_string(end_oid, sizeof(end_oid), &delta->new.oid);

	/* TODO: Match git diff more closely */
	if (delta->old.mode == delta->new.mode) {
		git_buf_printf(pi->buf, "index %s..%s %o\n",
			start_oid, end_oid, delta->old.mode);
	} else {
		if (delta->old.mode == 0) {
			git_buf_printf(pi->buf, "new file mode %o\n", delta->new.mode);
		} else if (delta->new.mode == 0) {
			git_buf_printf(pi->buf, "deleted file mode %o\n", delta->old.mode);
		} else {
			git_buf_printf(pi->buf, "old mode %o\n", delta->old.mode);
			git_buf_printf(pi->buf, "new mode %o\n", delta->new.mode);
		}
		git_buf_printf(pi->buf, "index %s..%s\n", start_oid, end_oid);
	}

	return git_buf_lasterror(pi->buf);
}

static int print_patch_file(void *data, git_diff_delta *delta, float progress)
{
	int error;
	diff_print_info *pi = data;
	const char *oldpfx = pi->diff->opts.src_prefix;
	const char *oldpath = delta->old.path;
	const char *newpfx = pi->diff->opts.dst_prefix;
	const char *newpath = delta->new.path;

	GIT_UNUSED(progress);

	git_buf_clear(pi->buf);
	git_buf_printf(pi->buf, "diff --git %s%s %s%s\n", oldpfx, delta->old.path, newpfx, delta->new.path);
	if ((error = print_oid_range(pi, delta)) < GIT_SUCCESS)
		return error;

	if (git_oid_iszero(&delta->old.oid)) {
		oldpfx = "";
		oldpath = "/dev/null";
	}
	if (git_oid_iszero(&delta->new.oid)) {
		oldpfx = "";
		oldpath = "/dev/null";
	}

	if (delta->binary != 1) {
		git_buf_printf(pi->buf, "--- %s%s\n", oldpfx, oldpath);
		git_buf_printf(pi->buf, "+++ %s%s\n", newpfx, newpath);
	}

	if (git_buf_lasterror(pi->buf) != GIT_SUCCESS)
		return git_buf_lasterror(pi->buf);

	error = pi->print_cb(pi->cb_data, GIT_DIFF_LINE_FILE_HDR, pi->buf->ptr);
	if (error != GIT_SUCCESS || delta->binary != 1)
		return error;

	git_buf_clear(pi->buf);
	git_buf_printf(
		pi->buf, "Binary files %s%s and %s%s differ\n",
		oldpfx, oldpath, newpfx, newpath);
	if (git_buf_lasterror(pi->buf) != GIT_SUCCESS)
		return git_buf_lasterror(pi->buf);

	return pi->print_cb(pi->cb_data, GIT_DIFF_LINE_BINARY, pi->buf->ptr);
}

static int print_patch_hunk(
	void *data,
	git_diff_delta *d,
	git_diff_range *r,
	const char *header,
	size_t header_len)
{
	diff_print_info *pi = data;

	GIT_UNUSED(d);
	GIT_UNUSED(r);

	git_buf_clear(pi->buf);

	if (git_buf_printf(pi->buf, "%.*s", (int)header_len, header) == GIT_SUCCESS)
		return pi->print_cb(pi->cb_data, GIT_DIFF_LINE_HUNK_HDR, pi->buf->ptr);
	else
		return git_buf_lasterror(pi->buf);
}

static int print_patch_line(
	void *data,
	git_diff_delta *delta,
	char line_origin, /* GIT_DIFF_LINE value from above */
	const char *content,
	size_t content_len)
{
	diff_print_info *pi = data;

	GIT_UNUSED(delta);

	git_buf_clear(pi->buf);

	if (line_origin == GIT_DIFF_LINE_ADDITION ||
		line_origin == GIT_DIFF_LINE_DELETION ||
		line_origin == GIT_DIFF_LINE_CONTEXT)
		git_buf_printf(pi->buf, "%c%.*s", line_origin, (int)content_len, content);
	else if (content_len > 0)
		git_buf_printf(pi->buf, "%.*s", (int)content_len, content);

	if (git_buf_lasterror(pi->buf) != GIT_SUCCESS)
		return git_buf_lasterror(pi->buf);

	return pi->print_cb(pi->cb_data, line_origin, pi->buf->ptr);
}

int git_diff_print_patch(
	git_diff_list *diff,
	void *cb_data,
	git_diff_output_fn print_cb)
{
	int error;
	git_buf buf = GIT_BUF_INIT;
	diff_print_info pi;

	pi.diff     = diff;
	pi.print_cb = print_cb;
	pi.cb_data  = cb_data;
	pi.buf      = &buf;

	error = git_diff_foreach(
		diff, &pi, print_patch_file, print_patch_hunk, print_patch_line);

	git_buf_free(&buf);

	return error;
}


int git_diff_blobs(
	git_repository *repo,
	git_blob *old_blob,
	git_blob *new_blob,
	git_diff_options *options,
	void *cb_data,
	git_diff_hunk_fn hunk_cb,
	git_diff_line_fn line_cb)
{
	diff_output_info info;
	git_diff_delta delta;
	mmfile_t old, new;
	xpparam_t xdiff_params;
	xdemitconf_t xdiff_config;
	xdemitcb_t xdiff_callback;

	assert(repo);

	if (options && (options->flags & GIT_DIFF_REVERSE)) {
		git_blob *swap = old_blob;
		old_blob = new_blob;
		new_blob = swap;
	}

	if (old_blob) {
		old.ptr  = (char *)git_blob_rawcontent(old_blob);
		old.size = git_blob_rawsize(old_blob);
	} else {
		old.ptr  = "";
		old.size = 0;
	}

	if (new_blob) {
		new.ptr  = (char *)git_blob_rawcontent(new_blob);
		new.size = git_blob_rawsize(new_blob);
	} else {
		new.ptr  = "";
		new.size = 0;
	}

	/* populate a "fake" delta record */
	delta.status = old.ptr ?
		(new.ptr ? GIT_DELTA_MODIFIED : GIT_DELTA_DELETED) :
		(new.ptr ? GIT_DELTA_ADDED : GIT_DELTA_UNTRACKED);
	delta.old.mode = 0100644; /* can't know the truth from a blob alone */
	delta.new.mode = 0100644;
	git_oid_cpy(&delta.old.oid, git_object_id((const git_object *)old_blob));
	git_oid_cpy(&delta.new.oid, git_object_id((const git_object *)new_blob));
	delta.old.path = NULL;
	delta.new.path = NULL;
	delta.similarity = 0;

	info.diff    = NULL;
	info.delta   = &delta;
	info.cb_data = cb_data;
	info.hunk_cb = hunk_cb;
	info.line_cb = line_cb;

	setup_xdiff_options(options, &xdiff_config, &xdiff_params);
	memset(&xdiff_callback, 0, sizeof(xdiff_callback));
	xdiff_callback.outf = diff_output_cb;
	xdiff_callback.priv = &info;

	xdl_diff(&old, &new, &xdiff_params, &xdiff_config, &xdiff_callback);

	return GIT_SUCCESS;
}
