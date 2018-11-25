/*
 * libjsongit2 - wrapper for libgit2 with JSON IO
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

#include "private.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>

static struct jg2_global jg2_global;

void
jg2_repo_ref_destroy(struct jg2_ref *r)
{
	free(r->ref_name);
	free(r);
}

void
jg2_repo_reflist_destroy(struct jg2_ref *r)
{
	struct jg2_ref *r1;

	while (r) {
		r1 = r->next;
		jg2_repo_ref_destroy(r);
		r = r1;
	}
}

void
jg2_repo_destroy(struct jg2_repo *r)
{
	struct jg2_repo *r1 = r->vhost->repo_list, **ro = &r->vhost->repo_list;

	free(r->repo_path);
	if (r->repo) {
		git_repository_free(r->repo);
		r->repo = NULL;
	}

	/* remove from vhost repo list */

	while (r1) {
		if (r1 == r) {
			*ro = r1->next;
			break;
		}
		ro = &r1->next;
		r1 = r1->next;
	}

	jg2_repo_reflist_destroy(r->ref_list);

	pthread_mutex_destroy(&r->lock);

	free(r);
}

/* requires vhost lock */

int
__jg2_vhost_reference_html(struct jg2_vhost *vh)
{
	char *q;

	if (!vh->cfg.vhost_html_filepath)
		return 0;

	/* we hold the vhost's canned html in memory for speed */

	if (lwsac_cached_file(vh->cfg.vhost_html_filepath, &vh->html_content,
			    &vh->html_len))
		return 1;

	/* find the comment marker used for dynamic META */

	q = strstr((char *)vh->html_content, JG2_HTML_META);
	if (!q) {
		lwsl_err("%s: %s lacks \"%s\" marker\n", __func__,
			 vh->cfg.vhost_html_filepath, JG2_HTML_META);

		lwsac_use_cached_file_detach(&vh->html_content);

		return 1;
	}
	vh->meta = q - (char *)vh->html_content;

	/* find the comment marker used for dynamic JSON */

	q = strstr((char *)vh->html_content, JG2_HTML_DYNAMIC);
	if (!q) {
		lwsl_err("%s: %s lacks \"%s\" marker\n", __func__,
			 vh->cfg.vhost_html_filepath, JG2_HTML_DYNAMIC);

		lwsac_use_cached_file_detach(&vh->html_content);

		return 1;
	}

	vh->dynamic = q - (char *)vh->html_content;

	lwsac_use_cached_file_start(vh->html_content);

	return 0;
}

static struct jg2_repodir *
jg2_repodir_find_create(struct jg2_repodir **phead, const char *path)
{
	struct jg2_repodir *rd = *phead;

	while (rd) {
		if (!strcmp(rd->repo_base_dir, path)) {
			rd->refcount++;

			return rd;
		}
		rd = rd->next;
	}

	rd = jg2_zalloc(sizeof(*rd));
	if (!rd)
		return NULL;

	if (phead == &jg2_global.cachedir_head) {
		jg2_global.count_cachedirs++;
		if (jg2_global.count_cachedirs == 1 &&
		    cache_trim_thread_spawn(&jg2_global)) {
			lwsl_err("cache trim thread creation failed\n");
			free(rd);
			jg2_global.count_cachedirs--;

			return NULL;
		}
	}

	pthread_mutex_init(&rd->lock, NULL);

	strncpy(rd->repo_base_dir, path, sizeof(rd->repo_base_dir) - 1);
	rd->repo_base_dir[sizeof(rd->repo_base_dir) - 1] = '\0';

	rd->refcount = 1;
	rd->next = *phead;
	*phead = rd;

	rd->jg2_global = &jg2_global;

	return rd;
}

static void
jg2_repodir_destroy(struct jg2_repodir **phead, struct jg2_repodir *repodir)
{
	struct jg2_repodir *rd = *phead, **ord = phead;

	while (rd) {
		if (rd == repodir) {
			*ord = rd->next;

			if (rd->dcs)
				lws_diskcache_destroy(&rd->dcs);

			pthread_mutex_destroy(&rd->lock);
			lwsac_free(&rd->rei_lwsac_head);
			free(rd);
			break;
		}
		ord = &rd->next;
		rd = rd->next;
	}
}

void jg2_safe_libgit2_init(void)
{
#if LIBGIT2_HAS_REFCOUNTED_INIT
	git_libgit2_init();
#else
	if (!jg2_global.thread_init_refcount++)
		git_threads_init();
#endif
}

void jg2_safe_libgit2_deinit(void)
{
#if LIBGIT2_HAS_REFCOUNTED_INIT
	git_libgit2_shutdown();
#else
	giterr_clear();
	if (!--jg2_global.thread_init_refcount) {
		git_threads_shutdown();
	}
#endif
}

struct jg2_vhost *
jg2_vhost_create(const struct jg2_vhost_config *config)
{
	pthread_mutexattr_t attr;
	struct jg2_vhost *vhost;

	if (!config->repo_base_dir)
		return NULL;

	vhost = malloc(sizeof(*vhost));
	if (!vhost)
		return NULL;

	memset(vhost, 0, sizeof(*vhost));
	vhost->cfg = *config;

	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&vhost->lock, &attr);

	email_vhost_init(vhost);

	if (!jg2_global.vhost_head) {
		pthread_mutex_init(&jg2_global.lock, NULL);

		if (jg2_gitolite3_interface(&jg2_global, config->repo_base_dir)) {
			lwsl_err("couldn't init gl3 interface\n");

			goto bail;
		}
		lwsl_notice("%s: created gl3 interface, detected v%d\n",
				__func__, jg2_global.gitolite_version);
	}

	/* add ourselves to the global vhost list */

	pthread_mutex_lock(&jg2_global.lock); /* ================ global lock */
	vhost->vhost_list = jg2_global.vhost_head;
	jg2_global.vhost_head = vhost;
	vhost->jg2_global = &jg2_global;
	pthread_mutex_unlock(&jg2_global.lock); /* ------------ global unlock */

	pthread_mutex_lock(&vhost->lock); /* ===================== vhost lock */

	/*
	 * repodir and cachedir serve to resolve multiple vhosts pointing at
	 * the same repository main dir and cache dir into a single object.
	 */

	vhost->repodir = jg2_repodir_find_create(&jg2_global.repodir_head,
						 config->repo_base_dir);
	if (config->json_cache_base) {

		if (config->cache_uid)
			lws_diskcache_prepare(config->json_cache_base, 0700,
					      config->cache_uid);

		vhost->cachedir = jg2_repodir_find_create(
						&jg2_global.cachedir_head,
						config->json_cache_base);
		if (!vhost->cachedir)
			goto bail;

		vhost->cachedir->dcs = lws_diskcache_create(
				config->json_cache_base,
				config->cache_size_limit);

		if (!vhost->cachedir->dcs)
			goto bail;
	}

	if (vhost->cfg.vhost_html_filepath) {
		if (__jg2_vhost_reference_html(vhost))
			goto bail;

		lwsac_use_cached_file_end(&vhost->html_content);
	}

	pthread_mutex_unlock(&vhost->lock); /* ----------------- vhost unlock */

	jg2_safe_libgit2_init();

	return vhost;

bail:
	lwsl_err("%s: failed\n", __func__);
	pthread_mutex_unlock(&vhost->lock); /* ----------------- vhost unlock */

	free(vhost);

	return NULL;
}

/* must hold vhost lock */

static int
__jg2_ctx_destroy(struct jg2_ctx *ctx)
{
	struct jg2_ctx **oc = NULL, *c = NULL;

	if (!ctx)
		return 0;

	ctx->destroying = 1;

	lwsac_use_cached_file_end(&ctx->vhost->html_content);

	/*
	 * closing while an incomplete job is going wouldn't be strange
	 * (browser page closed, lost connection etc).  Get any active job
	 * to clean up after itself (it sees ctx->destroying set).
	 */
	if (ctx->job) {
		ctx->job(ctx);
		if (ctx->fd_cache != -1) {
			close(ctx->fd_cache);
			unlink(ctx->cache);
		}
	} else
		if (ctx->fd_cache != -1) {
			close(ctx->fd_cache);
			ctx->fd_cache = -1;
		}

	/* remove ourselves from "ctx using vhost" list */

	c = NULL;
	oc = &ctx->vhost->ctx_on_vh_list;
	if (oc)
		c = *oc;

	while (c) {
		if (c == ctx) {
			*oc = c->ctx_on_vh_next;
			break;
		}

		oc = &c->ctx_on_vh_next;
		c = c->ctx_on_vh_next;
	}

	/* remove ourselves from "ctx using repo" list */

	if (ctx->jrepo && ctx->jrepo->ctx_repo_list) {
		pthread_mutex_lock(&ctx->jrepo->lock);

		c = NULL;
		oc = &ctx->jrepo->ctx_repo_list;
		if (oc)
			c = *oc;

		while (c) {
			if (c == ctx) {
				*oc = c->ctx_using_repo_next;
				break;
			}

			oc = &c->ctx_using_repo_next;
			c = c->ctx_using_repo_next;
		}

		pthread_mutex_unlock(&ctx->jrepo->lock);
	}

#if 0
	if (!ctx->jrepo->ctx_repo_list) {
		/* nobody using this logical repo any more */
		struct jg2_repo *r, **ro;

		r = ctx->vhost->repo_list;
		ro = &ctx->vhost->repo_list;

		while (r) {
			if (r == ctx->jrepo) {
				*ro = r->next;
				break;
			}
			ro = &r->next;
			r = r->next;
		}

		jg2_repo_destroy(ctx->jrepo);
	}
#endif

	jg2_repopath_destroy(&ctx->sr);

	free(ctx->md5_ctx);

	free(ctx);

	return 0;
}

void
jg2_vhost_destroy(struct jg2_vhost *vhost)
{
	struct jg2_repo *r, *r1;
	struct jg2_vhost *vh, **ovh;

	pthread_mutex_lock(&vhost->lock); /* ===================== vhost lock */

	r = vhost->repo_list;

	while (r) {
		r1 = r->next;
		jg2_repo_destroy(r);
		r = r1;
	}

	giterr_clear();

	jg2_safe_libgit2_deinit();

	email_vhost_deinit(vhost);

	if (vhost->html_content)
		lwsac_use_cached_file_detach(&vhost->html_content);

	pthread_mutex_unlock(&vhost->lock); /* ----------------- vhost unlock */

	if (vhost->cachedir && !--vhost->cachedir->refcount) {
		void *retval;

		if (!--jg2_global.count_cachedirs)

			/*
			 * we're about to destroy things the cache thread relies
			 * on.  Bring the cache thread to an end.
			 */

			pthread_join(jg2_global.cache_thread, &retval);
	}

	/*
	 * remove ourselves from the repodir + cachedir
	 * (and destroy it if last user)
	 */

	if (vhost->repodir && !--vhost->repodir->refcount)
		jg2_repodir_destroy(&jg2_global.repodir_head, vhost->repodir);

	if (vhost->cachedir && !vhost->cachedir->refcount) {
		jg2_repodir_destroy(&jg2_global.cachedir_head, vhost->cachedir);
	}

	/* remove ourselves from the global vhost list */

	pthread_mutex_lock(&jg2_global.lock); /* ================ global lock */

	ovh = &jg2_global.vhost_head;
	vh = *ovh;
	while (vh) {
		if (vh == vhost) {
			*ovh = vh->vhost_list;
			break;
		}
		ovh = &vh->vhost_list;
		vh = vh->vhost_list;
	}

	pthread_mutex_unlock(&jg2_global.lock); /* ------------ global unlock */

	if (!jg2_global.vhost_head) {
		jg2_gitolite3_interface_destroy(&jg2_global);
		/* we were the last vhost going away, destroy global assets */
		pthread_mutex_destroy(&jg2_global.lock);
	}

	pthread_mutex_destroy(&vhost->lock);

	free(vhost);
}

struct mimetype_map {
	const char *suffix;
	const char *mimetype;
};

static const struct mimetype_map mime[] = {
	{ ".html",	"text/html" },
	{ ".js",	"application/javascript" },
	{ ".css",	"text/css" },
	{ ".jpg",	"image/jpeg" },
	{ ".jpeg",	"image/jpeg" },
	{ ".pdf",	"application/pdf" },
	{ ".png",	"image/png" },
	{ ".svg",	"image/svg+xml" },
	{ ".txt",	"text/plain" },
	{ ".md",	"text/plain" },
	{ ".zip",	"application/zip" },
	{ ".tar.gz",	"application/gzip" },
	{ ".tar.bz2",	"application/x-bzip2" },
	{ ".tar.xz",	"application/x-xz" },
};

/*
 * ctx lists are held in
 *
 *  - the jrepo (head jrepo->ctx_repo_list), and
 *  - the vhost (head vh->ctx_on_vh_list)
 */

int
jg2_ctx_create(struct jg2_vhost *vhost, struct jg2_ctx **_ctx,
	       const struct jg2_ctx_create_args *args)
{
	char filepath[256], created_r = 0;
	int m = -1, flags = args->flags;
	struct jg2_ctx *ctx;
	struct jg2_repo *r;

	*_ctx = jg2_zalloc(sizeof(*ctx));
	if (!*_ctx)
		return 0;

	ctx = *_ctx;

	ctx->acl_user = args->authorized;
	ctx->vhost = vhost;
	ctx->user = args->user;
	ctx->fd_cache = -1;
	gettimeofday(&ctx->tv_gen, NULL);

	if (args->etag_length)
		args->etag[0] = '\0';

	if (args->accept_language) {
		strncpy(ctx->alang, args->accept_language,
			sizeof(ctx->alang) - 1);
		ctx->alang[sizeof(ctx->alang) - 1] = '\0';
	} else
		ctx->alang[0] = '\0';

	jg2_repopath_split(args->repo_path, &ctx->sr);

	/* bots are not allowed to use blame */

	if ((flags & JG2_CTX_FLAG_BOT) && ctx->sr.e[JG2_PE_MODE] &&
	    !strcmp(ctx->sr.e[JG2_PE_MODE], "blame"))
		ctx->sr.e[JG2_PE_MODE] = "tree";

	/* figure out if we are in blog mode with this repo... either by
	 * vhost association to a specific blog repo... */

	ctx->blog_mode = !!(vhost->cfg.flags & JG2_VHOST_BLOG_MODE);
	if (ctx->blog_mode && vhost->cfg.blog_repo_name) {
		if (ctx->sr.e[JG2_PE_NAME])
			free((char *)ctx->sr.e[JG2_PE_NAME]);
		ctx->sr.e[JG2_PE_NAME] = strdup(vhost->cfg.blog_repo_name);
		if (!ctx->sr.e[JG2_PE_NAME]) {
			m = JG2_CTX_CREATE_OOM;
			goto bail1;
		}
	}

	/*
	 * ... or because the description for the repo the URL points to
	 * begins with '+'.
	 */

	if (ctx->sr.e[JG2_PE_NAME]) {
		const char *str;
		const struct repo_entry_info *rei;
		struct jg2_repodir *rd = vhost->repodir;

		pthread_mutex_lock(&rd->lock); /* ============== repodir lock */

		rei = __jg2_repodir_repo(rd, ctx->sr.e[JG2_PE_NAME]);
		if (rei) {
			str = jg2_rei_string(rei, REI_STRING_CONFIG_DESC);

			if (str && str[0] == '+')
				ctx->blog_mode = 1;
		}

		pthread_mutex_unlock(&rd->lock); /* ---------- repodir unlock */
	}

	if (ctx->blog_mode && !ctx->sr.e[JG2_PE_MODE])
		ctx->sr.e[JG2_PE_MODE] = "blog";

	/* /plain/, /snapshot/, /patch/ overrides sandwich mode */

	if (ctx->sr.e[JG2_PE_MODE] && (jg2_job_naked(ctx) ||
			!strcmp(ctx->sr.e[JG2_PE_MODE], "ac")))
		flags &= ~JG2_CTX_FLAG_HTML;

	ctx->flags = flags;
	ctx->html_state = (flags & JG2_CTX_FLAG_HTML) ? HTML_STATE_HTML_META :
							HTML_STATE_JOB1;

	*args->mimetype = "text/html; charset=utf-8";
	*args->length = 0;

	if (ctx->sr.e[JG2_PE_MODE] &&
	    !strcmp(ctx->sr.e[JG2_PE_MODE], "patch"))
		*args->mimetype = "text/plain; charset=utf-8";


	/*
	 * ensure that the repodir is prepared with the repo list and this
	 * vhost's acls
	 */
	pthread_mutex_lock(&vhost->repodir->lock); /* ========== repodir lock */
	__jg2_conf_gitolite_admin_head(ctx);
	pthread_mutex_unlock(&vhost->repodir->lock); /* ------ repodir unlock */

	if (ctx->sr.e[JG2_PE_NAME] && ctx->sr.e[JG2_PE_NAME][0] &&
	    jg2_acl_check(ctx, ctx->sr.e[JG2_PE_NAME], ctx->acl_user) &&
	    jg2_acl_check(ctx, ctx->sr.e[JG2_PE_NAME], vhost->cfg.acl_user)) {
		lwsl_notice("%s: ACL permission denied: %s\n", __func__,
			    ctx->sr.e[JG2_PE_NAME]);
		m = JG2_CTX_CREATE_ACL_DENIED;
		goto bail1;
	}

	ctx->md5_ctx = vhost->cfg.md5_alloc();

	if (!ctx->sr.e[JG2_PE_NAME] ||
	    !ctx->sr.e[JG2_PE_NAME][0]) {
		/* not having a reponame is legal (shows repo list) */
		return 0;
	}

	/* we never want to make gitolite-admin available... */

	if (!strcmp(ctx->sr.e[JG2_PE_NAME], "gitolite-admin")) {
		lwsl_err("%s: requested illegal repo\n", __func__);
		m = 1;
		goto bail1;
	}

	pthread_mutex_lock(&vhost->lock); /* ===================== vhost lock */

	lws_snprintf(filepath, sizeof(filepath), "%s/%s.git",
		     vhost->cfg.repo_base_dir, ctx->sr.e[JG2_PE_NAME]);

	/* is the repo already open? */

	r = vhost->repo_list;
	while (r) {

		if (!strcmp(r->repo_path, filepath)) {
			/* the new ctx knows its using this jrepo then... */
			ctx->jrepo = r;
			/*
			 * insert into ctx into the jrepo's
			 * list of ctx using it
			 */
			pthread_mutex_lock(&r->lock); /* ==========jrepo lock */
			ctx->ctx_using_repo_next = r->ctx_repo_list;
			r->ctx_repo_list = ctx;
			pthread_mutex_unlock(&r->lock); /*-------jrepo unlock */
			/* (keep vhost lock) */
			goto do_mime;
		}

		r = r->next;
	}

	/* no, we have to create it */

	m = JG2_CTX_CREATE_OOM;
	r = jg2_zalloc(sizeof(*r));
	if (!r) {
		pthread_mutex_unlock(&vhost->lock); /* --------- vhost unlock */
		goto bail1;
	}

	created_r = 1;
	pthread_mutex_init(&r->lock, NULL);

	r->repo_path = strdup(filepath);
	r->vhost = vhost;
	if (!r->repo_path) {
		pthread_mutex_unlock(&vhost->lock); /* --------- vhost unlock */
		goto bail2;
	}

	m = git_repository_open_ext(&r->repo, r->repo_path, 0, NULL);
	if (m < 0) {
		const git_error *err = giterr_last();

		lwsl_err("repo open failed %s: %d\n", r->repo_path, m);
		if (err)
			lwsl_err("Error %d: %s\n", err->klass, err->message);

		pthread_mutex_unlock(&vhost->lock); /* --------- vhost unlock */
		m = JG2_CTX_CREATE_REPO_OPEN_FAIL;
		goto bail3;
	}

	ctx->jrepo = r;

	__repo_reflist_update(vhost, r);

	/* we start the new jrepo's "ctx using repo" list with ourselves */
	r->ctx_repo_list = ctx;

	r->next = vhost->repo_list;
	vhost->repo_list = r;

do_mime:

	/* (vhost lock still held whether found or created r) */

	/* add ourselves to the vhost ctx list while we have the vhost lock */
	ctx->ctx_on_vh_next = vhost->ctx_on_vh_list;
	vhost->ctx_on_vh_list = ctx;

	if (vhost->cfg.vhost_html_filepath &&
	    __jg2_vhost_reference_html(vhost)) {
		/* file problem or OOM... */
		pthread_mutex_unlock(&vhost->lock); /* --------- vhost unlock */
		goto bail4;
	}

	/* for naked modes, we can compute the ETAG / cache hash */

	if (args->etag && args->etag_length && ctx->sr.e[JG2_PE_MODE] &&
	    ctx->sr.e[JG2_PE_PATH] && jg2_job_naked(ctx)) {
		char md5_hex33[33];

		vhost->etag_tries++;

		__jg2_job_compute_cache_hash(ctx, jg2_job_naked(ctx), 0,
					     md5_hex33);

		strncpy(args->etag, md5_hex33, args->etag_length - 1);
		args->etag[args->etag_length - 1] = '\0';

		if (args->client_etag && !strcmp(args->etag, args->client_etag))
			vhost->etag_hits++;
	}

	pthread_mutex_unlock(&vhost->lock); /* ----------------- vhost unlock */

	/* figure out plain mimetype stuff */

	if (ctx->sr.e[JG2_PE_MODE] && ctx->sr.e[JG2_PE_PATH] &&
	    !strcmp(ctx->sr.e[JG2_PE_MODE], "plain")) {
		char id[128];
		size_t sl, n, l = strlen(ctx->sr.e[JG2_PE_PATH]);
		const char *vid = jg2_ctx_get_path(ctx, JG2_PE_VIRT_ID, id,
						   sizeof(id));

		strncpy(ctx->hex_oid, vid, sizeof(ctx->hex_oid) - 1);
		ctx->hex_oid[sizeof(ctx->hex_oid) - 1] = '\0';

		if (!blob_from_commit(ctx)) {
			*args->length = ctx->size;
			git_blob_free(ctx->u.blob);
		} else
			lwsl_err("blob_from_commit failed\n");

		for (n = 0; n < LWS_ARRAY_SIZE(mime); n++) {
			sl = strlen(mime[n].suffix);

			if (l > sl &&
			    !strcmp(ctx->sr.e[JG2_PE_PATH] + l - sl,
				     mime[n].suffix)) {
				*args->mimetype = mime[n].mimetype;
				break;
			}
		}
	}

	if (ctx->sr.e[JG2_PE_MODE] && ctx->sr.e[JG2_PE_PATH] &&
	    !strcmp(ctx->sr.e[JG2_PE_MODE], "snapshot")) {
		size_t sl, n, l = strlen(ctx->sr.e[JG2_PE_PATH]);

		for (n = 0; n < LWS_ARRAY_SIZE(mime); n++) {
			sl = strlen(mime[n].suffix);

			if (l > sl &&
			    !strcmp(ctx->sr.e[JG2_PE_PATH] + l - sl,
				     mime[n].suffix)) {
				*args->mimetype = mime[n].mimetype;
				break;
			}
		}
	}

	return 0;

bail4:
	git_repository_free(r->repo);

bail3:
	free(r->repo_path);

bail2:
	if (created_r) {
		pthread_mutex_destroy(&r->lock);
		free(r);
	}

bail1:
	jg2_repopath_destroy(&ctx->sr);

	if (ctx->md5_ctx)
		free(ctx->md5_ctx);

	free(ctx);
	*_ctx = NULL;

	return m;
}


int
jg2_ctx_destroy(struct jg2_ctx *ctx)
{
	struct jg2_vhost *vh;

	if (!ctx)
		return 0;

	vh = ctx->vhost;

	pthread_mutex_lock(&vh->lock); /* ======================== vhost lock */

	ctx->destroying = 1;

	__jg2_ctx_destroy(ctx);

	pthread_mutex_unlock(&vh->lock); /*--------------------- vhost unlock */

	return 0;
}
