/*
 * libjsongit2 - search
 *
 * Copyright (C) 2018 Andy Green <andy@warmcat.com>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation:
 *  version 2.1 of the License.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA  02110-1301  USA
 */

#include "../private.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>

#include <sys/stat.h>
#include <sys/types.h>

static void
remove_ongoing(struct jg2_ctx *ctx)
{
	struct ongoing_index **pon;
	int n = ctx->destroying;

	if (!ctx->ongoing)
		return;

	/*
	 * on the ctx->destroying / __jg2_ctx_destroy() path, the vh lock
	 * is already held
	 */

	if (!n)
		pthread_mutex_lock(&ctx->vhost->lock); /* ======== vhost lock */
	pon = &ctx->jrepo->indexing_list;
	while (*pon) {
		if (*pon == ctx->ongoing) {
			*pon = ctx->ongoing->next;
			lwsl_err("---------- ongoing free %p\n", ctx->ongoing);
			ctx->ongoing = NULL;
			free(ctx->ongoing);
			break;
		}
		pon = &(*pon)->next;
	}
	if (!n)
		pthread_mutex_unlock(&ctx->vhost->lock); /* ---- vhost unlock */
}

static void
job_search_destroy(struct jg2_ctx *ctx)
{
	int n = ctx->sp;

	remove_ongoing(ctx);

	while (n >= 0) {
		if (ctx->stack[n].tree) {
			if (ctx->stack[n].path)
				free(ctx->stack[n].path);
			//if (ctx->stack[n].tree)
			//	git_tree_free(ctx->stack[n].tree);
			/*
			 * libgit2 docs say don't free lev->tree... it seems it
			 * is cached and removed by lru inside libgit2
			 */
			ctx->stack[n].tree = NULL;
		}
		n--;
	}

	if (ctx->u.obj) {
		git_object_free(ctx->u.obj);
		ctx->u.obj = NULL;
	}

	if (ctx->t)
		lws_fts_destroy(&ctx->t);

	if (ctx->trie_fd != -1) {
		close(ctx->trie_fd);
		ctx->trie_fd = -1;
	}

	lwsac_free(&ctx->lwsac_head);

	ctx->job = NULL;
}

struct wl {
	const char *suff;
	int len;
	int priority; /* docs, then headers, then source */
};

struct wl whitelist[] = {
	{ ".c", 2, 2 },
	{ ".cpp", 4, 2 },
	{ ".h", 2, 3 },
	{ ".hpp", 4, 3 },
	{ ".txt", 4, 4 },
	{ ".html", 5, 1 },
	{ ".css", 4, 1 },
	{ ".js", 3, 1 },
	{ ".md", 3, 6 },
	{ ".in", 3, 1 },
	{ "README", 6, 5 },
	{ "Makefile", 8, 1 },
	{ NULL, 0, 0 }
};

/*
 * May return:
 *
 *  - JG2_CACHE_QUERY_NO_CACHE: not indexed
 *  - JG2_CACHE_QUERY_EXISTS: is indexed
 *  - JG2_CACHE_QUERY_ONGOING: is being created, *files and *done are written
 */

int
job_search_check_indexed(struct jg2_ctx *ctx, uint32_t *files, uint32_t *done)
{
	struct ongoing_index *ongoing = NULL;
	char hex[33], path[256];
	int fd, n;

	/* we must at least have the repo name */
	if (!ctx->sr.e[JG2_PE_NAME])
		return 1;

	if (!ctx->jrepo)
		return 1;

	pthread_mutex_lock(&ctx->vhost->lock); /* ================ vhost lock */
	__jg2_job_compute_cache_hash(ctx, JG2_JOB_SEARCH_TRIE, 1, hex);
	ongoing = ctx->jrepo->indexing_list;
	while (ongoing) {
		if (!strcmp(hex, ongoing->hash)) {
			*files = ongoing->index_files_to_do;
			*done = ongoing->index_files_done;
			break;
		}
		ongoing = ongoing->next;
	}
	if (!ongoing) {
		ctx->existing_cache_pos = 0;
		n = lws_diskcache_query(ctx->vhost->cachedir->dcs,
					JG2_CTX_FLAG_BOT, hex, &fd,
					path, sizeof(path) - 1,
					&ctx->existing_cache_size);
	}
	else
		n = LWS_DISKCACHE_QUERY_ONGOING;

	pthread_mutex_unlock(&ctx->vhost->lock); /* ------------ vhost unlock */

	if (n == LWS_DISKCACHE_QUERY_EXISTS)
		close(fd);

	return n;
}

static int
job_search_start(struct jg2_ctx *ctx)
{
	struct ongoing_index *ongoing = NULL;
	const git_tree_entry *te;
	char pure[256], hex[33];
	git_generic_ptr u;
	git_commit *c;
	struct wl *w;
	git_oid oid;
	int n;

	lwsl_err("%s: %p\n", __func__, ctx);

	ctx->trie_fd = -1;

	if (!ctx->hex_oid[0]) {
		lwsl_err("no oid\n");

		return 1;
	}

	ctx->lwsac_head = NULL;
	ctx->indexing = 0;

	pthread_mutex_lock(&ctx->vhost->lock); /* ================ vhost lock */
	__jg2_job_compute_cache_hash(ctx, JG2_JOB_SEARCH_TRIE, 1, hex);
	ongoing = ctx->jrepo->indexing_list;
	while (ongoing) {
		if (!strcmp(hex, ongoing->hash))
			break;
		ongoing = ongoing->next;
	}
	if (!ongoing) {
		ctx->existing_cache_pos = 0;

		/*
		 * this is creating / fetching the trie index file, not the
		 * query
		 */

		n = lws_diskcache_query(ctx->vhost->cachedir->dcs, 0, hex,
					&ctx->trie_fd, ctx->trie_filepath,
				        sizeof(ctx->trie_filepath) - 1,
				        &ctx->existing_cache_size);
	} else
		n = LWS_DISKCACHE_QUERY_ONGOING;

	if (n == LWS_DISKCACHE_QUERY_CREATING) {
		ongoing = malloc(sizeof(*ongoing));
		lwsl_err("---------- ongoing alloc %p\n", ongoing);
		if (ongoing) {
			ongoing->started = time(NULL);
			strcpy(ongoing->hash, hex);
			ongoing->index_files_to_do = 0;
			ongoing->index_files_done = 0;

			ongoing->next = ctx->jrepo->indexing_list;
			ctx->jrepo->indexing_list = ongoing;

			ctx->ongoing = ongoing;

			/*
			 * mark our task as wanting to continue independent
			 * of the lifetime of the initial wsi
			 */

			if (ctx->outlive)
				*ctx->outlive = 1;

			ctx->onetime = 1;
			if (!ctx->did_sat)
				ctx->meta = 0;
			ctx->no_rider = 1;
			meta_header(ctx);
			ctx->meta = 1;
			ctx->meta_last_job = 1;
			ctx->no_rider = 0;

			/*
			 * This "ongoing" result we ended up with cannot be
			 * cached after all... it's a transient situation.
			 *
			 * Close and delete the temp cache file related to
			 * it (this is not the cached index file... this is the
			 * cached query response, currently "ongoing")
			 */
			close(ctx->fd_cache);
			ctx->fd_cache = -1;
			unlink(ctx->cache);

			CTX_BUF_APPEND("{\"creating\":[");
		}
	}

	pthread_mutex_unlock(&ctx->vhost->lock); /* ------------ vhost unlock */

	/*
	 * If there is already an indexing ongoing, we don't need to do anything
	 * more except report that back to the client in the JSON
	 */

	if (n == LWS_DISKCACHE_QUERY_ONGOING) {
		if (!ongoing) {
			lwsl_err("oom\n");
			return -1;
		}
		ctx->indexing = 0;
		ctx->index_open_ro = 0;
		ctx->ac = 0;
		ctx->fp = 0;

		lwsl_err("ONGOING\n");

		meta_header(ctx);
		ctx->meta = 1;
		ctx->meta_last_job = 1;
		ctx->no_rider = 0;
		// ctx->partway = 0;

		/*
		 * This "ongoing" result we ended up with cannot be cached
		 * after all... it's a transient situation.
		 *
		 * Close and delete the temp cache file related to
		 * it (this is not the cached index file... this is the
		 * cached query response, currently "ongoing")
		 */
		close(ctx->fd_cache);
		ctx->fd_cache = -1;
		unlink(ctx->cache);

		CTX_BUF_APPEND("{\"ongoing\":[");

		return 0;
	}

	lwsl_notice("%s: trie cache '%s'\n", __func__, ctx->trie_filepath);

	if (n == LWS_DISKCACHE_QUERY_EXISTS) {
		lwsl_notice("%s: trie file %s exists in cache\n", __func__,
				ctx->trie_filepath);

		/*
		 * we don't need to do the indexing action... the start
		 * phase is finished we can just use the index
		 */

		ctx->index_open_ro = 1;

		return 0;
	}

	/* we have to create the index */

	lwsl_notice("%s: trie file %s must be created\n", __func__,
			ctx->trie_filepath);

	/* priority 1: branch (refs/heads/) */

	if (ctx->hex_oid[0] != 'r')
		lws_snprintf(pure, sizeof(pure), "refs/heads/%s", ctx->hex_oid);
	else
		strncpy(pure, ctx->hex_oid, sizeof(pure) - 1);
	pure[sizeof(pure) - 1] = '\0';
	if (git_reference_name_to_id(&oid, ctx->jrepo->repo, pure)) {

		/* priority 2: tag (refs/tags/) */

		lws_snprintf(pure, sizeof(pure), "refs/tags/%s", ctx->hex_oid);
		if (git_reference_name_to_id(&oid, ctx->jrepo->repo, pure)) {

			/* priority 3: oid */

			if (git_oid_fromstr(&oid, ctx->hex_oid)) {
				lwsl_err("%s: can't interpret ref '%s'\n",
					 __func__, ctx->hex_oid);

				return -1;
			}
		}
	}

	n = git_object_lookup(&u.obj, ctx->jrepo->repo, &oid, GIT_OBJ_ANY);
	if (n < 0) {
		lwsl_err("git_object_lookup failed\n");
		return -1;
	}

	if (git_object_type(u.obj) != GIT_OBJ_COMMIT) {
		lwsl_err("git object not a commit\n");
		goto bail;
	}

	/* convert the commit object to a tree object */

	c = u.commit;
	if (git_commit_tree(&u.tree, u.commit)) {
		lwsl_err("no tree from commit\n");
		goto bail;
	}

	git_commit_free(c);

	/* get ready to walk the entire tree */

	n = 0;
	pure[n++] = '/';
	pure[n] = '\0';

	ctx->sp = 0;
	ctx->stack[ctx->sp].tree = u.tree;
	ctx->stack[ctx->sp].path = strdup(pure);
	if (!ctx->stack[ctx->sp].path)
		goto bail;
	ctx->stack[ctx->sp].index = 0;

	/* compute the extent of the task first */

	lwsl_notice("%s: computing the extent of the tree...\n", __func__);

	do {
		struct tree_iter_level *lev = &ctx->stack[ctx->sp];

		te = git_tree_entry_byindex(lev->tree, lev->index++);
		if (!te) {

			/* this was the end of our current subtree... */

			free(lev->path);
			lev->path = NULL;
			// git_tree_free(lev->tree);
			/*
			 * libgit2 docs say don't free lev->tree... it seems it
			 * is cached and removed by lru inside libgit2
			 */
			lev->tree = NULL;
			lev->index = 0;

			if (ctx->sp) {
				/* let's go back up a level and continue... */
				ctx->sp--;
				continue;
			}

			/*
			 * oh... we have finished the root tree...
			 */
			break;
		}

		switch (git_tree_entry_type(te)) {
			char path[256];
			const char *ten;
			int len;

		case GIT_OBJ_TREE:

			if (ctx->sp == LWS_ARRAY_SIZE(ctx->stack) - 1) {
				lwsl_err("%s: too many dir levels %d\n",
					 __func__, ctx->sp + 1);

				goto bail;
			}

			lws_snprintf(path, sizeof(path), "%s%s/", lev->path,
				     git_tree_entry_name(te));

			lev = &ctx->stack[ctx->sp + 1];
			if (git_tree_lookup(&lev->tree, ctx->jrepo->repo,
					    git_tree_entry_id(te))) {
				lwsl_err("%s: unable to get tree\n", __func__);

				goto bail;
			}

			lev->path = strdup(path);
			if (!lev->path)
				goto bail;

			lev->index = 0;

			/* officially go down to the next level */
			ctx->sp++;
			break;

		case GIT_OBJ_BLOB:

			ten = git_tree_entry_name(te);
			len = strlen(ten);

			w = whitelist;
			do {
				const char *p;

				if (len >= w->len &&
				    ten[len - 1] == w->suff[w->len - 1]) {
					p = &ten[len - w->len];

					for (n = 0; n < w->len; n++)
						if (*p++ != w->suff[n])
							break;
					if (n == w->len)
						break;
				}
				w++;
			} while (w->suff);

			if (!w->suff)
				continue;

			if (ongoing) /* coverity */
				ongoing->index_files_to_do++;
			break;

		default:
			lwsl_err("%s: unexpected GIT_OBJ_ %d\n", __func__,
					git_tree_entry_type(te));

			goto bail;
		}

	} while (1);

	lwsl_notice("Task extent: %d files\n", ctx->ongoing->index_files_to_do);

	/* get ready to walk the entire tree */

	n = 0;
	pure[n++] = '/';
	pure[n] = '\0';

	ctx->sp = 0;
	ctx->stack[ctx->sp].tree = u.tree;
	ctx->stack[ctx->sp].path = strdup(pure);
	if (!ctx->stack[ctx->sp].path)
		goto bail;
	ctx->stack[ctx->sp].index = 0;

	/* initialize the trie */

	ctx->t = lws_fts_create(ctx->trie_fd);
	if (!ctx->t) {
		lwsl_err("%s: Unable to create the trie %d\n",
			 __func__, ctx->trie_fd);
		goto bail;
	}

	/*
	 * we do the one-time setup, then enter the normal job flow
	 * for the rest of the indexing action
	 */

	ctx->indexing = 1;

	return 0;

bail:
	if (ctx->t)
		lws_fts_destroy(&ctx->t);

	if (ctx->trie_fd != -1) {
		close(ctx->trie_fd);
		ctx->trie_fd = -1;
	}

	if (u.obj) {
		git_object_free(u.obj);
		u.obj = NULL;
	}

	lwsac_free(&ctx->lwsac_head);

	ctx->failed_in_start = 1;
	job_search_destroy(ctx);

	return -1;
}


int
job_search(struct jg2_ctx *ctx)
{
	struct lws_fts_search_params params;
	const git_tree_entry *te;
	struct lws_fts_file *jtf;
	int tfi, n, m;
	struct wl *w;

	lwsl_err("%s: %p\n", __func__, ctx);

	if (ctx->destroying) {
		lwsl_err("%s: %p: destroying\n", __func__, ctx);
		job_search_destroy(ctx);

		return 0;
	}

	if (!ctx->partway && job_search_start(ctx)) {
		lwsl_err("%s: start failed (%s)\n", __func__, ctx->hex_oid);
		return -1;
	}

	if (ctx->onetime) {
		ctx->onetime = 0;
		meta_trailer(ctx, "\n]");
		job_search_destroy(ctx);
		ctx->final = 2;
		return 0;
	}

	if (ctx->indexing)
		goto index;

	if (ctx->index_open_ro)
		goto index_reopen;

	if (ctx->ac) {
		while (ctx->ac && JG2_HAS_SPACE(ctx, 512)) {

			CTX_BUF_APPEND("%c\n{ \"ac\": \"%s\", "
				       "\"matches\": %d, \"agg\": %d",
				       ctx->subsequent ? ',' : ' ',
				       ((char *)(ctx->ac + 1)),
				       ctx->ac->instances,
				       ctx->ac->agg_instances);

			if (ctx->ac->elided)
				CTX_BUF_APPEND(", \"elided\": 1");

			CTX_BUF_APPEND("}\n");

			ctx->subsequent = 1;
			ctx->ac = ctx->ac->next;
		}

		if (!ctx->ac) {

			if (ctx->fp) {
				CTX_BUF_APPEND("]},{\"search\": [");
				ctx->subsequent = 0;
				return 0;
			}

			ctx->final = 1;
			meta_trailer(ctx, "\n]");
			job_search_destroy(ctx);

			return 1;
		}

		return 0;
	}

	if (ctx->fp) {
		while (ctx->fp && JG2_HAS_SPACE(ctx, 512)) {

			CTX_BUF_APPEND("%c\n{ \"fp\": \"%s\", "
				       "\"matches\": %d, \"lines\": %d }",
				       ctx->subsequent ? ',' : ' ',
				       ((char *)(ctx->fp + 1)) +
						       ctx->fp->matches_length,
				       ctx->fp->matches,
				       ctx->fp->lines_in_file);

			ctx->subsequent = 1;
			ctx->fp = ctx->fp->next;
		}

		if (!ctx->fp) {
			ctx->final = 1;
			meta_trailer(ctx, "\n]");
			job_search_destroy(ctx);

			return 1;
		}

		return 0;
	}

	//if (!ctx->result) {
		meta_trailer(ctx, "\n]");
		ctx->final = 1;
		job_search_destroy(ctx);

		return 1;
	//}

	//return 0;

index:

	do {
		struct tree_iter_level *lev = &ctx->stack[ctx->sp];
		int dr;

		lwsl_err("%s: ctx->sp %d\n", __func__, ctx->sp);
		te = git_tree_entry_byindex(lev->tree, lev->index++);
		if (!te) {

			/* this was the end of our current subtree... */

			free(lev->path);
			lev->path = NULL;
			/*
			 * libgit2 docs say don't free lev->tree... it seems it
			 * is cached and removed by lru inside libgit2
			 */
			lev->tree = NULL;
			lev->index = 0;

			if (ctx->sp) {
				/* let's go back up a level and continue... */
				ctx->sp--;
				continue;
			}

			/*
			 * oh... we have finished the root tree...
			 */
			break;
		}

		switch (git_tree_entry_type(te)) {
			char path[256];
			const char *ten;
			int len;

		case GIT_OBJ_TREE:

			if (ctx->sp == LWS_ARRAY_SIZE(ctx->stack) - 1) {
				lwsl_err("%s: too many dir levels %d\n",
					 __func__, ctx->sp + 1);

				goto bail;
			}

			lws_snprintf(path, sizeof(path), "%s%s/", lev->path,
				     git_tree_entry_name(te));

			lev = &ctx->stack[ctx->sp + 1];
			if (git_tree_lookup(&lev->tree, ctx->jrepo->repo,
					    git_tree_entry_id(te))) {
				lwsl_err("%s: unable to get tree\n", __func__);

				goto bail;
			}

			lev->path = strdup(path);
			if (!lev->path)
				goto bail;

			lev->index = 0;

			/* officially go down to the next level */
			ctx->sp++;
			break;

		case GIT_OBJ_BLOB:

			ten = git_tree_entry_name(te);
			len = strlen(ten);

			w = whitelist;
			do {
				const char *p;

				if (len >= w->len &&
				    ten[len - 1] == w->suff[w->len - 1]) {
					p = &ten[len - w->len];

					for (n = 0; n < w->len; n++)
						if (*p++ != w->suff[n])
							break;
					if (n == w->len)
						break;
				}
				w++;
			} while (w->suff);

			if (!w->suff)
				continue;

			ctx->ongoing->index_files_done++;

			if (git_blob_lookup(&ctx->u.blob, ctx->jrepo->repo,
					    git_tree_entry_id(te))) {
				lwsl_err("%s: unable to get blob\n", __func__);

				goto bail;
			}

			ctx->body = git_blob_rawcontent(ctx->u.blob);
			ctx->pos = 0;
			ctx->size = git_blob_rawsize(ctx->u.blob);

			m = lws_snprintf(path, sizeof(path), "%s%s", lev->path,
					 git_tree_entry_name(te));

			lwsl_notice("indexing %s\n", path + 1);
			tfi = lws_fts_file_index(ctx->t, path + 1, m - 1,
						 whitelist[n].priority);

			dr = lws_fts_fill(ctx->t, tfi, ctx->body, ctx->size);
			git_blob_free(ctx->u.blob);
			ctx->u.blob = NULL;

			if (dr) {
				lwsl_err("%s: OOM\n", __func__);
				goto bail;
			}

			break;

		default:
			lwsl_err("%s: unexpected GIT_OBJ_ %d\n", __func__,
					git_tree_entry_type(te));

			goto bail;
		}

	} while (1);

	lws_fts_serialize(ctx->t);
	lws_fts_destroy(&ctx->t);

	close(ctx->trie_fd);
	ctx->trie_fd = -1;
	lws_diskcache_finalize_name(ctx->trie_filepath);

	remove_ongoing(ctx);

	lwsl_notice("%s: completed OK\n", __func__);

	ctx->indexing = 0;
	ctx->index_open_ro = 1;
	// ctx->partway = 1;

index_reopen:

	lwsl_err("%s: %p: index_reopen\n", __func__, ctx);

	jtf = lws_fts_open(ctx->trie_filepath);
	if (!jtf)
		goto bail;

	ctx->ac = NULL;
	ctx->fp = NULL;

	memset(&params, 0, sizeof(params));

	params.needle = ctx->sr.e[JG2_PE_SEARCH];

	if (!strcmp(ctx->sr.e[JG2_PE_MODE], "ac"))
		params.flags = LWSFTS_F_QUERY_AUTOCOMPLETE;
	else
		params.flags = LWSFTS_F_QUERY_FILES;
	params.max_autocomplete = 16;

	if (ctx->did_sat && ctx->sr.e[JG2_PE_PATH]) {
		/* we're looking for results specific to the path */
		params.only_filepath = ctx->sr.e[JG2_PE_PATH];
		params.flags |= LWSFTS_F_QUERY_FILE_LINES;
	}

	ctx->result = lws_fts_search(jtf, &params);
	if (ctx->result) {
		ctx->lwsac_head = params.results_head;
		ctx->ac = ctx->result->autocomplete_head;
		ctx->fp = ctx->result->filepath_head;
	}

	// lwsl_err("%s: %p: %p %p\n", __func__, ctx->result, ctx->ac, ctx->fp);

	ctx->index_open_ro = 0;
	lws_fts_close(jtf);

	if (!ctx->did_sat)
		ctx->meta = 0;
	meta_header(ctx);
	if (ctx->ac)
		CTX_BUF_APPEND("{\"ac\": [");
	else
		CTX_BUF_APPEND("{\"search\": [");

	return 0;

bail:

	lwsl_err("%s: failing out\n", __func__);

	ctx->final = 1;
	meta_trailer(ctx, "\n]");
	job_search_destroy(ctx);

	return 0;
}

