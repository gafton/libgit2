/*
 * Copyright (C) 2009-2012 the libgit2 contributors
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_filter_h__
#define INCLUDE_filter_h__

#include "common.h"
#include "buffer.h"
#include "git2/odb.h"
#include "git2/repository.h"

typedef struct git_filter {
	int (*apply)(struct git_filter *self, git_buf *dest, const git_buf *source);
	void (*do_free)(struct git_filter *self);
} git_filter;

typedef enum {
	GIT_FILTER_TO_WORKTREE,
	GIT_FILTER_TO_ODB
} git_filter_mode;

typedef enum {
	GIT_CRLF_GUESS = -1,
	GIT_CRLF_BINARY = 0,
	GIT_CRLF_TEXT,
	GIT_CRLF_INPUT,
	GIT_CRLF_CRLF,
	GIT_CRLF_AUTO,
} git_crlf_t;

typedef struct {
	/* NUL, CR, LF and CRLF counts */
	unsigned int nul, cr, lf, crlf;

	/* These are just approximations! */
	unsigned int printable, nonprintable;
} git_text_stats;

/*
 * FILTER API
 */

/*
 * For any given path in the working directory, fill the `filters`
 * array with the relevant filters that need to be applied.
 *
 * Mode is either `GIT_FILTER_TO_WORKTREE` if you need to load the
 * filters that will be used when checking out a file to the working
 * directory, or `GIT_FILTER_TO_ODB` for the filters used when writing
 * a file to the ODB.
 *
 * @param filters Vector where to store all the loaded filters
 * @param repo Repository object that contains `path`
 * @param path Relative path of the file to be filtered
 * @param mode Filtering direction (WT->ODB or ODB->WT)
 * @return the number of filters loaded for the file (0 if the file
 *	doesn't need filtering), or a negative error code
 */
extern int git_filters_load(git_vector *filters, git_repository *repo, const char *path, int mode);

/*
 * Apply one or more filters to a file.
 *
 * The file must have been loaded as a `git_buf` object. Both the `source`
 * and `dest` buffers are owned by the caller and must be freed once
 * they are no longer needed.
 *
 * NOTE: Because of the double-buffering schema, the `source` buffer that contains
 * the original file may be tampered once the filtering is complete. Regardless, 
 * the `dest` buffer will always contain the final result of the filtering
 *
 * @param dest Buffer to store the result of the filtering
 * @param source Buffer containing the document to filter
 * @param filters A non-empty vector of filters as supplied by `git_filters_load`
 * @return GIT_SUCCESS on success, an error code otherwise
 */
extern int git_filters_apply(git_buf *dest, git_buf *source, git_vector *filters);

/*
 * Free the `filters` array generated by `git_filters_load`.
 *
 * Note that this frees both the array and its contents. The array will
 * be clean/reusable after this call.
 *
 * @param filters A filters array as supplied by `git_filters_load`
 */
extern void git_filters_free(git_vector *filters);

/*
 * Available filters
 */

/* Strip CRLF, from Worktree to ODB */
extern int git_filter_add__crlf_to_odb(git_vector *filters, git_repository *repo, const char *path);


/*
 * PLAINTEXT API
 */

/*
 * Gather stats for a piece of text
 *
 * Fill the `stats` structure with information on the number of
 * unreadable characters, carriage returns, etc, so it can be
 * used in heuristics.
 */
extern void git_text_gather_stats(git_text_stats *stats, const git_buf *text);

/*
 * Process `git_text_stats` data generated by `git_text_stat` to see
 * if it qualifies as a binary file
 */
extern int git_text_is_binary(git_text_stats *stats);

#endif
