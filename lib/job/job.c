/*
 * libjsongit2 - jobs
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

#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#define lp_to_rei(p, _n) lws_list_ptr_container(p, struct repo_entry_info, _n)

static jg2_job jobs[] = {
	job_reflist,
	job_log,
	job_commit,
	job_commit,
	job_tree,
	job_plain,
	job_repos,
	job_snapshot,
	job_blame,
	job_blog,
	job_search,
};

jg2_job
jg2_get_job(jg2_job_enum n)
{
	return jobs[n];
}

int
jg2_job_naked(struct jg2_ctx *ctx)
{
	if (!ctx->sr.e[JG2_PE_MODE])
		return 0;

	if (!strcmp(ctx->sr.e[JG2_PE_MODE], "plain"))
		return JG2_JOB_PLAIN;

	if (!strcmp(ctx->sr.e[JG2_PE_MODE], "snapshot"))
		return JG2_JOB_SNAPSHOT;

	if (!strcmp(ctx->sr.e[JG2_PE_MODE], "patch"))
		return JG2_JOB_PATCH;

	return 0; /* nope */
}

int
job_spool_from_cache(struct jg2_ctx *ctx)
{
	int left, n;

	if (ctx->fd_cache == -1)
		return -1;

	left = lws_ptr_diff(ctx->end, ctx->p);
	if (left <= JG2_RESERVE_SEAL)
		return 0;
	left -= JG2_RESERVE_SEAL;

	n = read(ctx->fd_cache, ctx->p, left);
	if (n < 0) {
		lwsl_err("%s: error reading from cache, errno: %d\n",
			 __func__, errno);
		close(ctx->fd_cache);
		ctx->fd_cache = -1;

		return -1;
	}

	if (n < (int)sizeof(ctx->last_from_cache)) {
		size_t m, old = sizeof(ctx->last_from_cache) - (size_t)n;
		for (m = 0; m < old; m++)
			ctx->last_from_cache[m] = ctx->last_from_cache[old + m];
		memcpy(ctx->last_from_cache + old, ctx->p, n);
	} else
		memcpy(ctx->last_from_cache, ctx->p + (size_t)n - sizeof(ctx->last_from_cache), sizeof(ctx->last_from_cache));

	ctx->p += n;
	ctx->existing_cache_pos += n;

	if (!n || ctx->existing_cache_pos == ctx->existing_cache_size) {
		/* finished */
		ctx->final = 1;
		ctx->job = NULL;
		close(ctx->fd_cache);
		ctx->fd_cache = -1;

		if (ctx->last_from_cache[4] == ']' && ctx->last_from_cache[5] == '}') {
			ctx->p[-1] = ' ';
			ctx->existing_cache_pos--;
			ctx->last_from_cache[5] = ' ';
		}

		//if (ctx->meta_last_job)
			ctx->meta = 1;
		meta_trailer(ctx, NULL);
	}

	return 0;
}

/* requires vhost lock */

static void
__jg2_job_hash_visible_repos(struct jg2_ctx *ctx)
{
	struct jg2_vhost *vh = ctx->vhost;
	lws_list_ptr lp = vh->repodir->rei_head;

	while (lp) {
		struct repo_entry_info *rei = lp_to_rei(lp, next);
		char *p = (char *)(rei + 1);

		if (!jg2_acl_check(ctx, p, ctx->acl_user))
			vh->cfg.md5_upd(ctx->md5_ctx, (unsigned char *)p,
					strlen(p));

		lws_list_ptr_advance(lp);
	}
}

/* requires vhost lock (because it may want the jrepo refs) */

void
__jg2_job_compute_cache_hash(struct jg2_ctx *ctx, jg2_job_enum job, int count,
			     char *md5_hex33)
{
	uint16_t je = job + (JG2_JSON_EPOCH << 8);
	uint32_t c32 = (uint32_t)count;

	/* calculate what the cache file would have been called */

	ctx->vhost->cfg.md5_init(ctx->md5_ctx);

	/* item 1: the job + an epoch changed when libjsongit2 is updated and
	 *         older JSON should be invalidated.  Older cache guys will
	 *         no longer be referenced and get reaped from old age.
	 */
	ctx->vhost->cfg.md5_upd(ctx->md5_ctx, (unsigned char *)&je, 2);

	/* item 2: the low 32-bits of the count */
	if (job != JG2_JOB_SEARCH_TRIE) {
		ctx->vhost->cfg.md5_upd(ctx->md5_ctx, (unsigned char *)&c32, 4);

		if (ctx->sr.e[JG2_PE_SEARCH])
			ctx->vhost->cfg.md5_upd(ctx->md5_ctx,
				(unsigned char *)ctx->sr.e[JG2_PE_SEARCH],
				strlen(ctx->sr.e[JG2_PE_SEARCH]));
	}

	/* item 3: the repo refs hash (if we are affiliated with a repo) */
	if (ctx->jrepo) {
		if (job != JG2_JOB_SEARCH_TRIE)
			ctx->vhost->cfg.md5_upd(ctx->md5_ctx,
					   ctx->jrepo->md5_refs,
					   sizeof(ctx->jrepo->md5_refs));

	/* item 4: the repo filepath (if we are affiliated with a repo) */
		ctx->vhost->cfg.md5_upd(ctx->md5_ctx,
					(unsigned char *)ctx->jrepo->repo_path,
					strlen(ctx->jrepo->repo_path));

	/* item 5: the mode we are looking for results with, if any */
		if (ctx->sr.e[JG2_PE_MODE])
			ctx->vhost->cfg.md5_upd(ctx->md5_ctx,
					(unsigned char *)ctx->sr.e[JG2_PE_MODE],
					strlen(ctx->sr.e[JG2_PE_MODE]));

	/* item 6: the path part inside the repo, if any */
		if (job != JG2_JOB_SEARCH_TRIE && ctx->sr.e[JG2_PE_PATH])
			ctx->vhost->cfg.md5_upd(ctx->md5_ctx,
					(unsigned char *)ctx->sr.e[JG2_PE_PATH],
					strlen(ctx->sr.e[JG2_PE_PATH]));

	/* item 7: the oid if the job could use it (and we have a repo) */
		switch(job) {
		case JG2_JOB_REFLIST: /* doesn't use oid perspective */
			break;
		case JG2_JOB_REPOLIST:/* doesn't use oid perspective */
			break;
#if LIBGIT2_HAS_BLAME
		case JG2_JOB_BLAME: /* blame is tied to blob hash */
			if (!blob_from_commit(ctx)) {
				char hoid[GIT_OID_HEXSZ + 1];

				oid_to_hex_cstr(hoid, git_blob_id(ctx->u.blob));
				ctx->vhost->cfg.md5_upd(ctx->md5_ctx,
					(unsigned char *)hoid, strlen(hoid));

				git_object_free(ctx->u.obj);
				ctx->u.obj = NULL;
				ctx->body = NULL;
				ctx->size = 0;
			}
			break;
#endif
		default:
			ctx->vhost->cfg.md5_upd(ctx->md5_ctx,
						(unsigned char *)ctx->hex_oid,
						sizeof(ctx->hex_oid) - 1);
		}

	/* item 8: repo info */

		if (job != JG2_JOB_SEARCH_TRIE && ctx->sr.e[JG2_PE_NAME]) {
			lws_list_ptr lp = ctx->vhost->repodir->rei_head;

			while (lp) {
				struct repo_entry_info *rei =
							lp_to_rei(lp, next);
				char *p = (char *)(rei + 1);
				size_t n = LWS_ARRAY_SIZE(rei->conf_len);

				if (!strcmp(p, ctx->sr.e[JG2_PE_NAME]))
					n = 0;

				for (; n < LWS_ARRAY_SIZE(rei->conf_len); n++) {
					if (!rei->conf_len[n])
						continue;

					ctx->vhost->cfg.md5_upd(ctx->md5_ctx,
							(unsigned char *)p,
							rei->conf_len[n]);
					p += rei->conf_len[n];
				}

				lws_list_ptr_advance(lp);
			}
		}

	} else {
		/*
		 * there's no repo context, so this is the list of accessible
		 * repos.  We have to include the current list of accessible
		 * repos, allowing for the vhost and ctx ACLs, in the hash to
		 * call it a match.
		 *
		 * Otherwise the list of ACLs could change, the ctx authorized
		 * acl_user could change or we could look from a different
		 * vhost and wrongly match.  Only including the vhost name and
		 * ctx authorized user name isn't enough, since the effect of
		 * those depends on the gitolite config.  We could hash that,
		 * but the gitolite config may change for unrelated reasons.
		 *
		 * Start by hashing in the gitolite-admin HEAD oid, since it
		 * may also change gitweb config strings etc
		 */

		ctx->vhost->cfg.md5_upd(ctx->md5_ctx, (unsigned char *)
			ctx->vhost->repodir->hexoid_gitolite_conf,
			sizeof(ctx->vhost->repodir->hexoid_gitolite_conf) - 1);

		__jg2_job_hash_visible_repos(ctx);
	}

	ctx->vhost->cfg.md5_fini(ctx->md5_ctx, ctx->job_hash);
	md5_to_hex_cstr(md5_hex33, ctx->job_hash);
}

/**
 * jg2_ctx_set_job() - set the current "job" the context is doing
 *
 * \param ctx: pointer to the context
 * \param job: job function enum
 * \param hex_oid: the hex string oid to start the job with, or a ref
 *		   like "refs/head/master", or NULL
 * \param count: resrict the job to output this many elements
 * \param flags: Or-able bitflags: JG2_JOB_FLAG_FINAL = last job,
 *		 JG2_JOB_FLAG_HTML = wrap JSON inside vhost html
 *
 * Set to NULL if no active job.  Otherwise, set to a job function pointer
 * from the jg2_jobs() table.  Use the enums JG2_JOB_* to select a
 * job from the list.
 *
 * Note hex_oid if given has its content copied by this api, and isn't required
 * to persist on the caller side.
 */

static void
jg2_ctx_set_job(struct jg2_ctx *ctx, jg2_job_enum job, const char *hex_oid,
		int count, int flags)
{
	char md5_hex[(JG2_MD5_LEN * 2) + 1];

	if (!(flags & JG2_JOB_FLAG_CHAINED)) {
		if (ctx->fd_cache != -1) {
			close(ctx->fd_cache);
			ctx->fd_cache = -1;
		}
	}

	ctx->partway = ctx->final = 0;
	ctx->job = jg2_get_job(job);

	if (hex_oid) {
		strncpy(ctx->hex_oid, hex_oid, sizeof(ctx->hex_oid) - 1);
		ctx->hex_oid[sizeof(ctx->hex_oid) - 1] = '\0';
	} else
		ctx->hex_oid[0] = '\0';
	ctx->count = count;
	ctx->meta_last_job = !!(flags & JG2_JOB_FLAG_FINAL);

	if (flags & JG2_JOB_FLAG_CHAINED)
		/*
		 * We don't have any independent fate for choosing about the
		 * cache, continue what we did when we decided to do the thing
		 * that led to this chained action.
		 */
		return;

	ctx->us_gen = 0;
	ctx->cache_written_p = ctx->p;

	/* caching is disabled? */
	if (!ctx->vhost->cfg.json_cache_base)
		return;

	/* don't cache autocomplete */
	if (ctx->sr.e[JG2_PE_MODE] && !strcmp(ctx->sr.e[JG2_PE_MODE], "ac"))
		return;

	pthread_mutex_lock(&ctx->vhost->lock); /* =================== vh lock */
	__jg2_job_compute_cache_hash(ctx, job, count, md5_hex);

	ctx->existing_cache_pos = 0;
	ctx->job_cache_query = lws_diskcache_query(ctx->vhost->cachedir->dcs,
				ctx->flags & JG2_CTX_FLAG_BOT, md5_hex,
				&ctx->fd_cache, ctx->cache,
				sizeof(ctx->cache) - 1,
				&ctx->existing_cache_size);

	if (ctx->job_cache_query == LWS_DISKCACHE_QUERY_EXISTS) {
		ctx->job = job_spool_from_cache;
		if (!ctx->sr.e[JG2_PE_MODE] || !jg2_job_naked(ctx)) {
			pthread_mutex_unlock(&ctx->vhost->lock); /* vh unlock */
			meta_header(ctx);

			return;
		}
	}
	pthread_mutex_unlock(&ctx->vhost->lock); /* ---- vhost unlock */
}

jg2_job
jg2_ctx_get_job(struct jg2_ctx *ctx)
{
	return ctx->job;
}

struct cfg_item {
	const char *git;
	const char *json;
};

static const struct cfg_item items[] = {
	{ "gitweb.description", "desc" },
	{ "gitweb.owner", "owner" },
	{ "gitweb.url", "url" },
};

int
jg2_get_repo_config(git_repository *gr, struct repo_entry_info *rei, char *p)
{
	git_config *cfg, *oc;
	int n, len = -1;

#if LIBGIT2_HAS_REPO_CONFIG_SNAP
	if (git_repository_config_snapshot(&cfg, gr))
#else
	if (git_repository_config(&cfg, gr))
#endif
		goto post;

	if (git_config_open_level(&oc, cfg, GIT_CONFIG_LEVEL_LOCAL))
		goto post1;

	len = 0;

	for (n = 0; n < (int)LWS_ARRAY_SIZE(items); n++) {
		const char *str;
		int slen, m;

		if (rei)
			rei->conf_len[n] = 0;

		m = git_config_get_string(&str, oc, items[n].git);
		if (m)
			continue;

		slen = strlen(str);

		if (rei) {
			rei->conf_len[n] = slen + 1;
			memcpy(p, str, slen + 1);
			p += slen + 1;
		} else
			len += slen + 1;
	}

	git_config_free(oc);

post1:
	git_config_free(cfg);

post:
	return len;
}

const char *
jg2_rei_string(const struct repo_entry_info *rei, enum rei_string_index n)
{
	const char *p = (const char *)(rei + 1);

	if (n == REI_STRING_NAME)
		return p;
	p += rei->name_len;
	if (n == REI_STRING_CONFIG_DESC)
		return p;
	p += rei->conf_len[0];
	if (n == REI_STRING_CONFIG_OWNER)
		return p;
	p += rei->conf_len[1];
	if (n == REI_STRING_CONFIG_URL)
		return p;

	return NULL;
}

static uint64_t
timeval_us(struct timeval *t)
{
	return ((uint64_t)t->tv_sec * 1000000ull) + t->tv_usec;
}

static void
cache_write_complete(struct jg2_ctx *ctx)
{
	char final_name[128], *p;

	close(ctx->fd_cache);
	ctx->fd_cache = -1;

	/*
	 * We may have unwittingly been in a race with
	 * another thread / process creating the same
	 * thing for another client.  We prepended the
	 * pid + ctx pointer to the temp filepath so
	 * they won't clash as files, but only one can
	 * be renamed to the final filepath.  So we may
	 * fail if we lost the race... no worries, the
	 * job still got done.  Just delete ourselves
	 * if the rename fails.
	 */

	p = strchr(ctx->cache, '~');
	if (!p)
		unlink(ctx->cache);
	else {
		int n = lws_ptr_diff(p, ctx->cache);

		memcpy(final_name, ctx->cache, n);
		final_name[n] = '\0';
		if (rename(ctx->cache, final_name))
			unlink(ctx->cache);
	}
}

static void
cache_write(struct jg2_ctx *ctx, jg2_job job_in)
{
	int n, count;

	if (job_in == job_spool_from_cache)
		return;

	if (ctx->fd_cache == -1)
		return;

	if (ctx->cache_written_p == ctx->p)
		return;

	count = lws_ptr_diff(ctx->p, ctx->cache_written_p);
	n = write(ctx->fd_cache, ctx->cache_written_p, count);

	ctx->cache_written_p = ctx->p;

	if (n == count)
		return;

	/*
	 * ...if we met an error writing into it, close
	 * and delete the cache file, but continue with
	 * generating the live ctx data
	 */

	lwsl_notice("%s: cache write %s, fd %d, "
		    "failed: errno %d\n", __func__,
		    ctx->cache, ctx->fd_cache, errno);
	close(ctx->fd_cache);
	ctx->fd_cache = -1;
	unlink(ctx->cache);
}

void
meta_header(struct jg2_ctx *ctx)
{
	const char *av = "//www.gravatar.com/avatar/";
	const struct repo_entry_info *rei = NULL;
	struct jg2_repodir *rd = ctx->vhost->repodir;
	int f = 0, m = 0;
	char pure[128];
	size_t n;

	ctx->started = 1;
	ctx->subsequent = 0;
//	ctx->sealed_items = 0;

	if (ctx->meta || ctx->destroying)
		return;

	if (ctx->vhost->cfg.avatar_url)
		av = ctx->vhost->cfg.avatar_url;

#if LIBGIT2_HAS_BLAME
	f |= 1;
#endif
#if defined(JG2_HAVE_ARCHIVE_H)
	f |= 2;
#endif
	f |= ctx->blog_mode << 2;

	/*
	 * We always issue this first section fresh.  That allows it to
	 * contain things like the accept-languages header the browser sent,
	 * something that's specific to the client (and not possible to get
	 * at otherwise from the browser in a standardized way).
	 *
	 * f b0 = capable of blame
	 *   b1 = capable of archiving
	 *   b2 = blog mode
	 */

	CTX_BUF_APPEND("{"
		       "\"schema\":\"libjg2-1\",\n"
		       "\"vpath\":\"%s\",\n"
		       "\"avatar\":\"%s\",\n"
		       "\"alang\":\"%s\",\n"
		       "\"gen_ut\":%lu,\n",
		       ctx->vhost->cfg.virtual_base_urlpath, av,
		       ellipsis_purify(pure, ctx->alang, sizeof(pure)),
		       (unsigned long)ctx->tv_gen.tv_sec);

	if (ctx->sr.e[JG2_PE_NAME])
		CTX_BUF_APPEND("\"reponame\":\"%s\",\n",
				ctx->sr.e[JG2_PE_NAME]);

	pthread_mutex_lock(&rd->lock); /* ====================== repodir lock */

	if (ctx->sr.e[JG2_PE_NAME])
		rei = __jg2_repodir_repo(rd, ctx->sr.e[JG2_PE_NAME]);

	if (!ctx->jrepo || !ctx->jrepo->repo || !rei)
		goto post;

	for (n = REI_STRING_CONFIG_DESC; n <= REI_STRING_CONFIG_URL; n++) {
		const char *str = jg2_rei_string(rei, n);

		if (!str) {
			m++;
			continue;
		}

		if (n == REI_STRING_CONFIG_OWNER) {
			CTX_BUF_APPEND("\"%s\": {", items[m].json);
			identity_json(str, ctx);
			CTX_BUF_APPEND("},");
		} else
			CTX_BUF_APPEND("\"%s\":\"%s\",\n", items[m].json,
				      ellipsis_purify(pure, str, sizeof(pure)));
		m++;
	}

post:
	pthread_mutex_unlock(&rd->lock); /* ------------------ repodir unlock */
	CTX_BUF_APPEND("\"f\":%d,\n\"items\": [\n", f);

	ctx->cache_written_p = ctx->p;

	ctx->meta = 1;
}

void
job_common_header(struct jg2_ctx *ctx)
{
	char path[128], *p;
	size_t l = strlen(ctx->vhost->cfg.json_cache_base);

	if (strlen(ctx->cache) < l + 6) {
		path[0] = '?';
		path[1] = '\0';
	} else {
		strncpy(path, ctx->cache + l + 5, sizeof(path) - 1);
		path[sizeof(path) - 1] = '\0';

		p = strchr(path, '~');
		if (p)
			*p = '\0';
	}

	CTX_BUF_APPEND("{"
			    "\"schema\":\"libjg2-1\",\n");
	if (ctx->vhost->cfg.json_cache_base)
		CTX_BUF_APPEND("\"cid\":\"%s\",\n", path);
}

void
meta_trailer(struct jg2_ctx *ctx, const char *term)
{
	struct timeval t2;
	const char *mode = jg2_ctx_get_path(ctx, JG2_PE_MODE, NULL, 0);
	int pc = 0, pc1 = 0, idx, bl, bnaic = mode && !strcmp(mode, "blame");// && ctx->last_from_cache[4] != ']' && ctx->last_from_cache[5] != '}';
	int appendo = (ctx->no_rider || jg2_job_naked(ctx)) || !mode || strcmp(mode, "blame") || (ctx->last_from_cache[4] != ']' || ctx->last_from_cache[5] != '}');
	int cfixup = ctx->last_from_cache[4] == ']' && ctx->last_from_cache[5] == ' ';
	uint32_t files, done;

	if (lws_ptr_diff(ctx->end, ctx->p) < JG2_RESERVE_SEAL)
		lwsl_err("%s: JG2_RESERVE_SEAL %d but only %d left\n", __func__,
			 JG2_RESERVE_SEAL, lws_ptr_diff(ctx->end, ctx->p));

	ctx->final = 1;
	bl = mode && !strcmp(mode, "blame") && ctx->meta_last_job && !ctx->sealed_items;

	gettimeofday(&t2, NULL);

	/*
	 * the numbers are padded so that they don't affect the result length
	 * for tests like ab that care about it
	 */

	if (term && !bl)
		/* s section is 12 chars + numbers */
		CTX_BUF_APPEND("%s,\"s\":{\"c\":%8lu,\"u\":%8u}", term,
		       (unsigned long)ctx->tv_gen.tv_sec,
		       (unsigned int)(ctx->us_gen + (timeval_us(&t2) -
				      timeval_us(&ctx->tv_last))));
	ctx->tv_last = t2;

	/* we also always seal it with } CR */

	if (appendo && term && !bl && !ctx->sealed_items && !cfixup)
		CTX_BUF_APPEND("}\n");

	if ((!ctx->meta || ctx->destroying) && (!mode || strcmp(mode, "blame")))
		return;

	if (!ctx->meta_last_job && !jg2_job_naked(ctx)) {
		CTX_BUF_APPEND(",");
		return;
	}

	/* if we were writing a job cache, it's over now */

	if (ctx->job != job_spool_from_cache && ctx->fd_cache != -1) {
		cache_write(ctx, ctx->job);
		cache_write_complete(ctx);
	}

	if (ctx->vhost->cache_tries)
		pc = (ctx->vhost->cache_hits * 100) / ctx->vhost->cache_tries;

	if (ctx->vhost->etag_tries)
		pc1 = (ctx->vhost->etag_hits * 100) / ctx->vhost->etag_tries;

	if (!ctx->no_rider && !jg2_job_naked(ctx)) {
		if (bl || (!mode || strcmp(mode, "blame"))) {
			if (appendo && !ctx->sealed_items && !cfixup)
				CTX_BUF_APPEND("]");
		CTX_BUF_APPEND(",\"g\":%8lu,\"chitpc\":%8u,\"ehitpc\":%8u",
			       (unsigned long)(timeval_us(&t2) -
			         timeval_us(&ctx->tv_gen)), pc, pc1);

		if (ctx->sr.e[JG2_PE_NAME]) {

			idx = job_search_check_indexed(ctx, &files, &done);

			CTX_BUF_APPEND(",\"indexed\":%d\n", idx);

			if (idx == LWS_DISKCACHE_QUERY_ONGOING)
				CTX_BUF_APPEND(", \"index_files\":%d,\n"
					       "\"index_done\":%d\n", files,
					       done);
		}

		if (bnaic && !cfixup)
			CTX_BUF_APPEND("}]");

		CTX_BUF_APPEND(",\n \"ab\": %d, \"si\": %d, \"db\":%d, \"di\":%d, \"sat\":%d, \"lfc\": \"%02x%02x\"", ctx->appended_blob, ctx->sealed_items, ctx->did_bat, ctx->did_inline, ctx->did_sat, ctx->last_from_cache[4],ctx->last_from_cache[5]);

		CTX_BUF_APPEND("}\n\n");
		}
	}

	ctx->started = ctx->meta = 0;
}

/*
 * This performs sequencing for delivering a sandwich of
 *
 *  - optionally first part of vhost HTML before the magic comment
 *
 *  - one or more jobs generating JSON
 *
 *  - optionally the remainder of the HTML after the magic comment
 *
 * This also makes the selection of which JSON generation jobs need
 * doing based on the context's urlpath.
 *
 * For anything it generates, it statefully respects the buffer size
 * and waits to be called again to continue to emit (not necessarily
 * to the same buffer)
 */

int
jg2_ctx_fill(struct jg2_ctx *ctx, char *buf, size_t len, size_t *used,
	     char *outlive)
{
	const char *mode, *vid, *reponame, *search;
	size_t m = 0, left = len - 1;
	struct timeval t2;
	char id[64];
	jg2_job job_in;
	int more;

//	lwsl_err("%s\n", __func__);

	*used = 0;

	ctx->buf = buf;
	ctx->p = buf;
	ctx->cache_written_p = buf;
	ctx->len = len;
	ctx->end = (char *)buf + len - 1;
	ctx->final = 0;
	ctx->outlive = outlive;

	reponame = jg2_ctx_get_path(ctx, JG2_PE_NAME, NULL, 0);
	mode = jg2_ctx_get_path(ctx, JG2_PE_MODE, NULL, 0);
	vid = jg2_ctx_get_path(ctx, JG2_PE_VIRT_ID, id, sizeof(id));
	search = jg2_ctx_get_path(ctx, JG2_PE_SEARCH, NULL, 0);

	switch (ctx->html_state) {

	case HTML_STATE_HTML_META:
		m = ctx->vhost->meta - ctx->html_pos > left ?
		    left : ctx->vhost->meta - ctx->html_pos;
		memcpy(ctx->p, ctx->vhost->html_content + ctx->html_pos, m);
		ctx->html_pos += m;
		ctx->p += m;

		if (ctx->html_pos != ctx->vhost->meta ||
		    lws_ptr_diff(ctx->end, ctx->p) < 1024)
			break;

		CTX_BUF_APPEND("<meta name=\"Description\" content=\""
			       "generated-by:gitohashi git web interface, "
			       "repository: %s, mode: %s, path: %s, rev: %s\">",
			       reponame ? reponame : "-", mode ? mode : "-",
			       jg2_ctx_get_path(ctx, JG2_PE_NAME, NULL, 0) ?
			       jg2_ctx_get_path(ctx, JG2_PE_NAME, NULL, 0) :
				       "-", vid ? vid : "-");

		ctx->html_pos += JG2_HTML_META_LEN;
		ctx->html_state = HTML_STATE_HTML_HEADER;

		/* fallthru */

	case HTML_STATE_HTML_HEADER:
		m = ctx->vhost->dynamic - ctx->html_pos > left ?
		    left : ctx->vhost->dynamic - ctx->html_pos;
		memcpy(ctx->p, ctx->vhost->html_content + ctx->html_pos, m);
		ctx->html_pos += m;
		ctx->p += m;

		if (ctx->html_pos != ctx->vhost->dynamic ||
		    lws_ptr_diff(ctx->end, ctx->p) < 1024)
			break;

		ctx->html_pos += JG2_HTML_DYNAMIC_LEN;

		/* fallthru */

	case HTML_STATE_JOB1:
		ctx->html_state = HTML_STATE_JSON;

		if (mode && !strcmp(mode, "log")) {
			ctx->job_state = EMIT_STATE_LOG;
			jg2_ctx_set_job(ctx, JG2_JOB_LOG, vid, 50,
					JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "plain")) {
			ctx->job_state = EMIT_STATE_PLAIN;
			jg2_ctx_set_job(ctx, JG2_JOB_PLAIN, vid, 0,
						JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "commit")) {
			ctx->job_state = EMIT_STATE_COMMITBODY;
			jg2_ctx_set_job(ctx, JG2_JOB_COMMIT,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "patch")) {
			ctx->job_state = EMIT_STATE_PATCH;
			jg2_ctx_set_job(ctx, JG2_JOB_PATCH,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "tags")) {
			ctx->job_state = EMIT_STATE_TAGS;
			jg2_ctx_set_job(ctx, JG2_JOB_REFLIST,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "branches")) {
			ctx->job_state = EMIT_STATE_BRANCHES;
			jg2_ctx_set_job(ctx, JG2_JOB_REFLIST,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "tree")) {
			ctx->job_state = EMIT_STATE_TREE;
			jg2_ctx_set_job(ctx, JG2_JOB_TREE,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "blog")) {
			ctx->job_state = EMIT_STATE_BLOG;
			jg2_ctx_set_job(ctx, JG2_JOB_BLOG,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "ac")) { /* autocomplete */
			ctx->job_state = EMIT_STATE_SEARCH;
			jg2_ctx_set_job(ctx, JG2_JOB_SEARCH,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "fp")) { /* filepath */
			ctx->job_state = EMIT_STATE_SEARCH;
			jg2_ctx_set_job(ctx, JG2_JOB_SEARCH,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else if (mode && !strcmp(mode, "search")) {
			ctx->job_state = EMIT_STATE_SEARCH;
			jg2_ctx_set_job(ctx, JG2_JOB_SEARCH,
					vid, 0, JG2_JOB_FLAG_FINAL);
#if LIBGIT2_HAS_BLAME
		} else if (mode && !strcmp(mode, "blame")) {
			ctx->job_state = EMIT_STATE_TREE;
			jg2_ctx_set_job(ctx, JG2_JOB_TREE,
					vid, 0, JG2_JOB_FLAG_FINAL);
#endif
		} else if (mode && !strcmp(mode, "summary")) {
			ctx->job_state = EMIT_STATE_SUMMARY;
			jg2_ctx_set_job(ctx, JG2_JOB_REFLIST,
					vid, 0, 0);
#if defined(JG2_HAVE_ARCHIVE_H)
		} else if (mode && !strcmp(mode, "snapshot")) {
			ctx->job_state = EMIT_STATE_SNAPSHOT;
			jg2_ctx_set_job(ctx, JG2_JOB_SNAPSHOT,
					vid, 0, 0);
#endif
		} else if (!mode && (!reponame || !reponame[0])) {
			ctx->job_state = EMIT_STATE_REPOLIST;
			jg2_ctx_set_job(ctx, JG2_JOB_REPOLIST,
					vid, 0, JG2_JOB_FLAG_FINAL);
		} else {
			ctx->job_state = EMIT_STATE_TREE;
			jg2_ctx_set_job(ctx, JG2_JOB_TREE,
					vid, 0, JG2_JOB_FLAG_FINAL);
		}

		/* fallthru */

	case HTML_STATE_JSON:

		// lwsl_err("%s: STATE_JSON\n", __func__);

		if (!jg2_ctx_get_job(ctx))
			break;

		/*
		 * We do it in the ctx so the job function itself can also
		 * compute the elapsed time accurately.
		 */
		job_in = ctx->job;
		gettimeofday(&ctx->tv_last, NULL);
		more = jg2_ctx_get_job(ctx)(ctx);
		gettimeofday(&t2, NULL);
		if (more < 0) {
			char pure[256];

			lwsl_notice("%s: get_job failed %d (%d) %s\n", __func__,
				    more, ctx->job_state, ctx->status);

			meta_header(ctx);
			job_common_header(ctx);

			ellipsis_purify(pure, ctx->status, sizeof(pure));

			CTX_BUF_APPEND(" \"error\": \"%s\"}", pure);
			CTX_BUF_APPEND("]}");

			/* hm... let's say we completed */
			ctx->final = 1;
			ctx->meta_last_job = 1;
			ctx->partway = 0;
			ctx->sealed_items = 1;
		}
		// lwsl_err("%s: job says %d\n", __func__, more);

		ctx->us_gen += timeval_us(&t2) - timeval_us(&ctx->tv_last);

		cache_write(ctx, job_in);

		/*
		 * final: 0 = still going, 1 = final, 2 = final send but stay
		 * 					  on job state
		 */
		ctx->partway = ctx->final != 1;
		if (!ctx->meta_last_job)
			ctx->final = 0;

		if (ctx->final == 2) {
			*used = lws_ptr_diff(ctx->p, ctx->buf);
			lwsl_err("ctx->final says 2\n");
//			ctx->meta_last_job = 1;
			return 1;
		}

		if (ctx->partway)
			break;

		switch (ctx->job_state) {
		case EMIT_STATE_SUMMARY:
			ctx->job_state = EMIT_STATE_SUMMARY_LOG;
			jg2_ctx_set_job(ctx, JG2_JOB_LOG, "refs/heads/master",
					10, JG2_JOB_FLAG_CHAINED |
					    JG2_JOB_FLAG_FINAL);
			break;
		case EMIT_STATE_TREE:

			ctx->u.obj = NULL;

			/*
			 * after processing the ls for a tree, we might have
			 * found a doc file we want to show inline additionally
			 */
			if (ctx->inline_filename[0] && !ctx->did_inline) {
				/* we found a document we want to show inline */
				lwsl_debug("doing inline %s %s\n",
						ctx->inline_filename,
						ctx->sr.e[JG2_PE_PATH]);

				ctx->job_state = EMIT_STATE_TREE;
				jg2_ctx_set_job(ctx, JG2_JOB_TREE, vid, 0,
						JG2_JOB_FLAG_CHAINED |
						JG2_JOB_FLAG_FINAL);
				ctx->meta = 1;
				break;
			}

			if (ctx->job_cache_query != LWS_DISKCACHE_QUERY_EXISTS &&
			    search && !ctx->did_sat && ctx->sr.e[JG2_PE_PATH]) {
				ctx->job_state = EMIT_STATE_TREE;
				jg2_ctx_set_job(ctx, JG2_JOB_SEARCH, vid, 0,
						JG2_JOB_FLAG_CHAINED |
						JG2_JOB_FLAG_FINAL);
				ctx->did_sat = 1;
				ctx->meta = 1;
				break;
			}

			if (ctx->blame_after_tree && !ctx->did_bat &&
			    ctx->sr.e[JG2_PE_PATH] != ctx->inline_filename) {
				ctx->job_state = EMIT_STATE_BLAME;
				jg2_ctx_set_job(ctx, JG2_JOB_BLAME, vid, 0,
						JG2_JOB_FLAG_CHAINED |
						JG2_JOB_FLAG_FINAL);
				ctx->did_bat = 1;
				ctx->meta = 1;
				break;
			}

			/* fallthru */
		default:
			/*
			 * This is the end for a single JSON job, chained or
			 * just on its own.  Finish up the cache file if we
			 * were making one.
			 */

			if (ctx->fd_cache != -1)
				cache_write_complete(ctx);

			if (ctx->flags & JG2_CTX_FLAG_HTML)
				ctx->html_state = HTML_STATE_HTML_TRAILER;
			else
				ctx->html_state = HTML_STATE_COMPLETED;

			break;
		}
		break;

	case HTML_STATE_HTML_TRAILER:
		// lwsl_err("HTML_STATE_HTML_TRAILER\n");
		left = lws_ptr_diff(ctx->end, ctx->p);
		m = ctx->vhost->html_len - ctx->html_pos > left ?
		    left : ctx->vhost->html_len - ctx->html_pos;
		memcpy(ctx->p, ctx->vhost->html_content + ctx->html_pos, m);
		ctx->html_pos += m;
		ctx->p += m;

		if (ctx->html_pos == ctx->vhost->html_len)
			ctx->html_state = HTML_STATE_COMPLETED;
		break;

	default:
		return 1;
	}

	*used = lws_ptr_diff(ctx->p, ctx->buf);

	return ctx->html_state == HTML_STATE_COMPLETED;
}
