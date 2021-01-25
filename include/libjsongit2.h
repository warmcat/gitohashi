/*
 * libjsongit2.h - Public API for JSON wrapper for libgit2
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

#if !defined(__LIB_JSON_GIT2_H__b1c6cef9e87b714318802950c4e08a15ffaa6559)
#define __LIB_JSON_GIT2_H__b1c6cef9e87b714318802950c4e08a15ffaa6559

#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#if defined(__GNUC__)
#define JG2_VISIBLE __attribute__((visibility("default")))
#else
#define JG2_VISIBLE
#endif

struct jg2_vhost;
struct jg2_ctx;

typedef void * jg2_md5_context;

struct jg2_vhost_config {

	/* mandatory */

	const char *virtual_base_urlpath; /**< Mandatory; like "/git" */
	const char *repo_base_dir; /**< Mandatory; like "/srv/repositories" */

	/* optional */

	const char *acl_user; /**< like "v-warmcat"... this user is checked for
	 	 	 	   READ access to allow showing repos on this
	 	 	 	   vhost... context may supply an authorized
	 	 	 	   user additionally that is additively checked.
	 	 	 	   NULL if not used. */

	const char *vhost_html_filepath; /**< like "/etc/jsongit2/vh1.html"
		this is optional if you don't want JSON sandwiched in HTML */
	const char *avatar_url; /**< NULL for "//www.gravatar.com/avatar/",
				     else your server / proxy cache base URL */
	const char *json_cache_base; /**< directory to place JSON cache,
					* /var/cache/libjsongit2 recommended;
					* disabled if left NULL */

	uint64_t cache_size_limit; /**< goal for max cache size in bytes,
				    *   0 means use default of 256MiB */

	uid_t cache_uid; /**< if you create the vhosts while still being root
			 * and later change uid + gid, you can set the uid
			 * of the cache directory permissions here, even if the
			 * cache dir is somewhere privileged like /var/cache/...
			 * The mode of the cache directory and its subdirecties
			 * is set to 0700.  Leave at 0 if you set the cache dirs
			 * up externally */

	int email_hash_bins; /**< email cache hash bins (0 defaults to 16) */
	int email_hash_depth; /**< max emails per hash bin (0 defaults to 16) */

	void *avatar_arg; /**< opaque pointer passed to avatar callback, if set */

#define JG2_VHOST_BLOG_MODE 1
	unsigned int flags; /* OR-ed flags: JG2_VHOST_BLOG_MODE = "blog mode" */
	const char *blog_repo_name; /**< the repo name of the blog, if blog mode */

	/* optional md5 acceleration */

	jg2_md5_context (*md5_alloc)(void);
	/**< user code can provide accelerated md5, default of NULL means
	     use internal implementation.  All four callbacks must be given
	     if md5_alloc() is set.  Note that md5_fini does NOT free() the
	     pointer returned by md5_alloc(), since often you want to
	     allocate once and reuse. */
	void (*md5_init)(jg2_md5_context _ctx);
	/**< callback prepares \p _ctx to perform a new hash */
	int (*md5_upd)(jg2_md5_context _ctx, const unsigned char *input,
		       size_t ilen);
	/**< callback adds \p ilen bytes from \p input to the hash */
	int (*md5_fini)(jg2_md5_context _ctx, unsigned char output[16]);
	/**< copies the final hash to \p output.  \p _ctx must be freed by the
	 * user afterwards (or reused after md5_init() on it again) */

	/* optional event callbacks */

	void (*refchange)(void *user);
	/**< user handler for contexts who repo reflist has changed */
	int (*avatar)(void *avatar_arg, const unsigned char *md5);
	/**< optional hook called when an avatar md5 was computed... eg
	 * can be used to prime a side-cache with the avatar image
	 */
};

struct jg2_ctx_create_args {
	const char *repo_path; /**< filesystem path to the repo */
#define JG2_CTX_FLAG_HTML 1
#define JG2_CTX_FLAG_BOT 2
	int flags; /**< bitwise-ORed flags: JG2_CTX_FLAG_HTML = generate HTML
			around the JSON, using the vhost HTML file;
			absent = pure JSON, JG2_CTX_FLAG_BOT = don't create
			cache entries for this context */
	const char **mimetype; /**< pointer to const char * to take pointer
				    to mimetype */
	unsigned long *length; /**< pointer to unsigned long to take 0 or
				    length of http data if known */
	char *etag; /**< pointer to char buffer to take Etag token part */
	size_t etag_length; /**< length of etag char buffer */
	const char *client_etag; /** NULL, or etag the client offered */
	const char *authorized; /**< NULL or gitolite name for ACL use */
	const char *accept_language; /**< client's accept-language hdr if any */
	void *user; /**< opaque user pointer to attach to ctx */
};

/**
 * jg2_library_init() - "vhost" library init
 *
 * \param config: config struct
 *
 * Returns the allocated jg2 "vhost" pointer, or NULL if failed.
 *
 * The user code must call this at least once and pass the returned pointer
 * when creating the contexts.  This call serves two purposes... first to
 * hide the required call to init git2 library, and second to hold sticky,
 * pooled data like the repo current refs and email md5 cache that contexts
 * can share.
 *
 * It's OK to call this more than once, for example once per web server vhost
 * that will be using this api.  But it must be called at least once.
 */
JG2_VISIBLE struct jg2_vhost *
jg2_vhost_create(const struct jg2_vhost_config *config);

/**
 * jg2_library_deinit() - "vhost" deinit
 *
 * \param vhost: pointer to the vhost struct to be deallocated
 *
 * This must be called once before the process exits on every vhost pointer
 * allocated via jg2_vhost_create().
 */
JG2_VISIBLE void
jg2_vhost_destroy(struct jg2_vhost *vhost);

/**
 * jg2_ctx_create() - create a jg2 connection context
 *
 * \param vhost: pointer to vhost library init struct context will bind to
 * \param ctx: pointer to the pointer that will be allocated
 * \param args: arguments that should be set on entry
 *
 * This creates the context for an individual connection with a urlpath.  It
 * doesn't produce output itself, it "sets up the connection".  Many connection
 * contexts can be ongoing simultaneously.
 *
 * Returns 0 if the context was allocated, or nonzero if it wasn't allocated
 * due to a problem.
 */

#define JG2_CTX_CREATE_ACL_DENIED 3
#define JG2_CTX_CREATE_OOM 2
#define JG2_CTX_CREATE_REPO_OPEN_FAIL 1

JG2_VISIBLE int
jg2_ctx_create(struct jg2_vhost *vhost, struct jg2_ctx **ctx,
	       const struct jg2_ctx_create_args *args);

/**
 * jg2_ctx_destroy() - destroy a jg2 context
 *
 * \param ctx: pointer to the pointer that will be allocated
 *
 * Deallocates anything in the context.  This should be called when the
 * connection the context is representing is closed.
 */
JG2_VISIBLE int
jg2_ctx_destroy(struct jg2_ctx *ctx);

/**
 * jg2_ctx_fill() - fill a buffer with content
 *
 * \param ctx: pointer to the context
 * \param buf: buffer to write content into
 * \param len: length of buf that may be used
 * \param used: pointer to size_t filled with count of bytes in buf used
 * \param outlive: NULL or pointer to char set to 1 if the filler wishes to
 *		   outlive the wsi
 *
 * This generates the "next buffer load" of output from the connection context.
 * Work and anything using memory is deferred until it is actually needed, for
 * example trees are walked incrementally and blobs opened one at a time only
 * when needed for output and closed before moving on.
 *
 * If the work is bigger than one buffer it returns when the buffer is full
 * and resumes with a new buffer next call to jg2_ctx_fill().
 *
 * Returns < 0 on error, or the amount of buffer bytes written
 */
JG2_VISIBLE int
jg2_ctx_fill(struct jg2_ctx *ctx, char *buf, size_t len, size_t *used,
	     char *outlive);

#endif /* __LIB_JSON_GIT2_H__b1c6cef9e87b714318802950c4e08a15ffaa6559 */
