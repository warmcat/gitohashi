/*
 * libjg2 - private includes
 *
 * Copyright (C) 2018 - 2020 Andy Green <andy@warmcat.com>
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

#if !defined(__LIBJG2_PRIVATE_H__)
#define __LIBJG2_PRIVATE_H__

#define _GNU_SOURCE
#include <pthread.h>

#include <libjsongit2.h>
#include <git2.h>

#define LG2_VERSION(MA, MI) \
		((((LIBGIT2_VER_MAJOR) << 16) + (LIBGIT2_VER_MINOR)) - \
			(((MA) << 16) + (MI)))

#define LIBGIT2_HAS_BLAME_MAILMAP	(LG2_VERSION(0, 28) >= 0)
#define LIBGIT2_HAS_BLAME		(LG2_VERSION(0, 21) >= 0)
#define LIBGIT2_HAS_REPO_CONFIG_SNAP	(LG2_VERSION(0, 21) >= 0)
#define LIBGIT2_HAS_GIT_BUF		(LG2_VERSION(0, 24) > 0)
#define LIBGIT2_HAS_DIFF		(LG2_VERSION(0, 19) > 0)
#define LIBGIT2_HAS_STR_BUF		(LG2_VERSION(0, 19) > 0)
#define LIBGIT2_HAS_REFCOUNTED_INIT	(LG2_VERSION(0, 19) > 0)
#define LIBGIT2_HAS_LEAKY_ERR		(LG2_VERSION(0, 19) <= 0)

/* generated by cmake */
#include <jg2-config.h>

#include <libwebsockets.h>

#include <stddef.h>
#include <stdint.h>

#if defined(JG2_HAVE_ARCHIVE_H)
#include <archive.h>
#include <archive_entry.h>
#endif

#define JG2_JSON_EPOCH 1

struct jg2_ctx;
struct jg2_vhost;

#if defined(__GNUC__)
#define JG2_FORMAT(string_index) __attribute__ ((format(printf, string_index, \
		   string_index + 1)))
#else
#define JG2_FORMAT(string_index)
#endif

#define CTX_BUF_APPEND(...) ctx->p += lws_snprintf(ctx->p, \
				lws_ptr_diff(ctx->end, ctx->p), __VA_ARGS__)
#define JG2_MD5_LEN 16
#define MIB (1024 * 1024)
#define KIB (1024)

#include "conf/private.h"
#include "job/private.h"
#include "email/private.h"

#define JG2_HTML_META	 "<!-- libjsongit2:meta-description -->"
#define JG2_HTML_META_LEN 37
#define JG2_HTML_DYNAMIC "<!-- libjsongit2:initial-json -->"
#define JG2_HTML_DYNAMIC_LEN 33


/*
 * The number of bytes we might want to send with stats, etc at the end of
 * the whole transaction or at the end of one JSON unit, whichever is bigger.
 *
 * The jobs act to reserve this amount of buffer.
 */
#define JG2_RESERVE_SEAL 100
#define JG2_HAS_SPACE(ctx, num) (lws_ptr_diff(ctx->end, ctx->p) > \
				 JG2_RESERVE_SEAL + num)

/**
 * jg2_path_element: which parsed path element to receive
 *
 * JG2_PE_VIRT_ID is a synthetic element which contains a commit id, or ref
 * name depending on what is available.
 */

typedef enum {
	JG2_PE_NAME,   /**< reponame (NULL if none) */
	JG2_PE_MODE,   /**< display mode, like tree or commit (NULL if none) */
	JG2_PE_PATH,   /**< filepath inside repo, if any (NULL if none) */
	JG2_PE_BRANCH, /**< specified branch, if any (NULL implies master) */
	JG2_PE_ID,     /**< specified oid in hex, if any (NULL if none) */
	JG2_PE_OFFSET, /**< pagination offset counted in items */
	JG2_PE_SEARCH, /**< search term */

	JG2_PE_COUNT,
	JG2_PE_VIRT_ID
} jg2_path_element;

/*
 * acl name, aclv3 structs all allocate in rei_lwsac_head along with the rei,
 * so they can all be freed with freeing rei_lwsac_head lac.
 */

struct aclv3 {
	struct aclv3 *next;	/* next aclv3 that can read the repo */
	const char *acl;
};

struct repo_entry_info {
	lws_list_ptr next;
	struct aclv3 *acls_valid_head;	/* user acls that are valid for read */
	short name_len;
	short conf_len[3];

	/*
	 * - the name
	 * - config description if any
	 * - config owner if any
	 * - config url if any
	 */
};

/* this is used for both per-repodir and per-cachedir stuff */

struct jg2_repodir {
	pthread_mutex_t lock;
	char repo_base_dir[128];
	struct jg2_global *jg2_global;
	struct jg2_repodir *next;

	char hexoid_gitolite_conf[GIT_OID_HEXSZ + 1];
	int refcount; /* vhosts using this struct */
	time_t last_gitolite_admin_head_check;

	/* repos in the repodir */

	struct lwsac *rei_lwsac_head;
	/* repo_entry_info list */
	lws_list_ptr rei_head;
	struct aclv3 *acls_known_head;	/* user acls that have been computed */

	/* cache trimming */

	struct lws_diskcache_scan *dcs;

	char subsequent;
};

struct jg2_ref {
	struct jg2_ref *next; /* next in linear list */
	struct jg2_ref *hash_next; /* next in same hash list */
	git_oid oid;
	char *ref_name;
};

struct ongoing_index {
	struct ongoing_index *next;
	char hash[33];
	time_t started;
	uint32_t index_files_to_do;
	uint32_t index_files_done;
};

#define REF_HASH_SIZE 16
#define jg2_oidbin(oid) ((oid)->id[0] & (REF_HASH_SIZE - 1))

struct jg2_split_repopath {
	const char *e[JG2_PE_COUNT];
	int offset;
};

struct jg2_repo {
	struct jg2_vhost *vhost;
	struct jg2_repo *next;
	char *repo_path;
	git_repository *repo;

	pthread_mutex_t lock;

	struct jg2_ctx *ctx_repo_list; /* linked-list of ctx using repo */

	/* linked-list of oids being indexed */
	struct ongoing_index *indexing_list;

	struct jg2_ref *ref_list; /* linked-list of refs */
	struct jg2_ref *ref_list_hash[REF_HASH_SIZE];
		/* set of linked-lists into the ref objects based on hash */

	unsigned char md5_refs[JG2_MD5_LEN]; /* hash of all refs in repo */

	time_t last_update;
};

struct jg2_vhost {
	struct jg2_email_hash_bin *bins;
	struct jg2_vhost_config cfg;
	struct jg2_repo *repo_list;
	struct jg2_vhost *vhost_list;
	struct jg2_ctx *ctx_on_vh_list;

	struct jg2_global *jg2_global;

	struct jg2_repodir *repodir;
	struct jg2_repodir *cachedir;

	pthread_mutex_t lock;

	jg2_md5_context md5_ctx;

	uint64_t cache_hits,
		 cache_tries,
		 etag_hits,
		 etag_tries;

	lwsac_cached_file_t html_content;
	size_t html_len;
	size_t meta;
	size_t dynamic;
};

typedef union {
	git_object *obj;
	git_commit *commit;
	git_tree *tree;
	git_blob *blob;
	git_tag *tag;
} git_generic_ptr;

enum {
	HTML_STATE_HTML_META,
	HTML_STATE_HTML_HEADER,
	HTML_STATE_JOB1,
	HTML_STATE_JSON,
	HTML_STATE_HTML_TRAILER,
	HTML_STATE_COMPLETED
};

struct tree_entry_info {
	lws_list_ptr next;
	const git_oid *oid;
	git_filemode_t mode;
	int type;
	uint64_t size;
	short namelen;

	/* then the name */
};

#if LIBGIT2_HAS_BLAME
struct blame_line_range {
	lws_list_ptr next;
	int lines;
	int line_start_orig;
	int line_start_final;
};

struct blame_hunk_info {
	lws_list_ptr next; 		 /**< next blame hunk, in time order */
	lws_list_ptr next_same_fsig; /**< same final author */
	lws_list_ptr next_uniq_fsig; /**< list of first use of unique sigs */
	lws_list_ptr next_sort_fsig; /**< sorted list of unique sigs */

	struct blame_hunk_info *fsig_rep; /** guy accumulating for our fsig */
	git_blame_hunk hunk;
	lws_list_ptr line_range_head;
	lws_list_ptr *line_range_tail;

	const char *orig_summary;
	const char *final_summary;

	int count_line_ranges;
	int count_lines;
	int count_lines_rep_acc;
	int ordinal;
	git_signature orig, final;

	/* then (pointed-to by pointers above):
	 *
	 *  orig name        NUL
	 *  orig email       NUL
	 *  final name       NUL
	 *  final email      NUL
	 *  orig commit log  NUL
	 *  final commit log NUL
	 *  orig path        NUL
	 */
};
#endif

struct tree_iter_level {
	char *path;
	git_tree *tree;
	size_t index;
};

struct jg2_ctx {
	struct jg2_split_repopath sr;
	struct jg2_vhost *vhost;
	struct jg2_repo *jrepo;
	struct jg2_ctx *ctx_using_repo_next;
	struct jg2_ctx *ctx_on_vh_next;
	struct jg2_ctx *ctx_on_thread_pool_queue_next;
	void *user;

	jg2_md5_context md5_ctx;
	unsigned char job_hash[JG2_MD5_LEN];

	/* job parameters */
	const char *acl_user;
	char hex_oid[64]; /**< may also be a ref like refs/head/master */
	char cache[128];
	char alang[128]; /**< accept-language string, or NUL */
	char status[256];
	int count;
	int flags;
	char *outlive;

	jg2_job job;

	/* job state */
	git_reference_iterator *iter_ref;
	git_generic_ptr u;
#if LIBGIT2_HAS_GIT_BUF
	git_buf buffer;
#else
	/* libgit2 too old for git_buf uses lwsac_head + lac */
#endif
	const char *body;
	struct timeval tv_last;
	struct timeval tv_gen;
	uint64_t us_gen;
	size_t pos, size, ofs;
	jg2_job_state job_state;
	struct lwsac *lwsac_head;
	struct lwsac *lac;
	lws_list_ptr sorted_head;
	struct tree_entry_info *tei;
	struct repo_entry_info *rei;
	struct lws_fts *t;
	int trie_fd;
	struct ongoing_index *ongoing;

	/* search */
	char trie_filepath[256];
	struct lws_fts_result *result;
	struct lws_fts_result_autocomplete *ac;
	struct lws_fts_result_filepath *fp;

	char inline_filename[256];
	unsigned char if_pref;
#if LIBGIT2_HAS_BLAME
	git_blame_options blame_opts;
	struct blame_hunk_info *bhi;
	struct blame_line_range *bli;
	git_blame *blame;
	lws_list_ptr head_uniq_fsig;
	lws_list_ptr head_sort_fsig;
	lws_list_ptr contrib;
#endif
	int fd_cache;
	int job_cache_query;
	char *cache_written_p;
	size_t existing_cache_pos;
	size_t existing_cache_size;

#if defined(JG2_HAVE_ARCHIVE_H)
	/* for snapshot state */
	struct archive *a;
	size_t lacpos;
	size_t lac_chunck_end;
#endif

	struct tree_iter_level stack[16];
	int sp;

	/* chunk buffer state */
	char *buf, *p, *end;
	size_t len;

	/* html */
	size_t html_pos;
	int html_state;

	char last_from_cache[6];

	unsigned int partway:1;
	unsigned int started:1;
	/**< 0= not started, or finished JSON, 1 = in progress */
	unsigned int final:2; /* final chunk of JSON for job has been done */
	unsigned int destroying:1; /**< context destruction is underway */
	unsigned int meta:1; /**< tracks if first job for outer brackets */
	unsigned int meta_last_job:1; /**< last job for outer brackets */
	unsigned int blame_after_tree:1; /**< did a blob, so do blame after */

	unsigned int did_inline:1; /**< did the inline for tree */
	unsigned int did_bat:1; /**< did the blame after tree */
	unsigned int did_sat:1; /**< did the search after tree */
	unsigned int appended_blob:1;
	unsigned int subsequent:1; /**< not the first entry in a job json list */

	unsigned int waiting_replay_done:1;
	unsigned int archive_completion:1;

	unsigned int blame_init_phase:1;
	unsigned int raw_patch:1;
	unsigned int blog_mode:1;
	unsigned int bot:1;
	unsigned int failed_in_start:1;
	unsigned int sealed_items:1;

	unsigned int indexing:1;
	unsigned int index_open_ro:1;
	unsigned int no_rider:1;
	unsigned int onetime:1;
};

struct jg2_global {
	struct jg2_repodir *repodir_head;
	struct jg2_repodir *cachedir_head;
	struct jg2_vhost *vhost_head;

	pthread_mutex_t lock;

	pthread_t cache_thread;
	int count_cachedirs;
#if !LIBGIT2_HAS_REFCOUNTED_INIT
	int thread_init_refcount;
#endif

	pthread_mutex_t lock_query;
	int gl3_child_pid;
	int gl3_pipe[2];
	int gl3_pipe_result[2];

	char gitolite_version;
};

const char *
jg2_ctx_get_path(const struct jg2_ctx *ctx, jg2_path_element type, char *buf,
		 size_t buflen);

void
jg2_safe_libgit2_init(void);

void
jg2_safe_libgit2_deinit(void);

void *
jg2_zalloc(size_t s);

void
jg2_repo_ref_destroy(struct jg2_ref *r);

void
jg2_repo_reflist_destroy(struct jg2_ref *r);

void
jg2_repo_destroy(struct jg2_repo *r);

int
jg2_vhost_repo_reflist_update(struct jg2_vhost *vhost);

const char *
oid_to_hex_cstr(char *oid_hex, const git_oid *oid);

const char *
ellipsis_string(char *out, const char *in, int max);

const char *
ellipsis_purify(char *out, const char *in, int max);

void
time_json(const git_time *t, struct jg2_ctx *ctx);

void
signature_json(const git_signature *sig, struct jg2_ctx *ctx);

void
signature_text(const git_signature *sig, struct jg2_ctx *ctx);

const char *
otype_name(git_otype type);

int
jg2_json_purify(char *escaped, const char *string, int len, size_t *inlim_totlen);

int
lws_snprintf(char *str, size_t size, const char *format, ...);

int
commit_summary(git_commit *commit, struct jg2_ctx *ctx);

int
generic_object_summary(const git_oid *oid, struct jg2_ctx *ctx);

jg2_md5_context
jg2_md5_alloc(void);
void
jg2_md5_init(jg2_md5_context _ctx);
int
jg2_md5_upd(jg2_md5_context _ctx, const unsigned char *input, size_t ilen);
int
jg2_md5_fini(jg2_md5_context _ctx, unsigned char output[16]);

const char *
md5_to_hex_cstr(char *md5_hex_33, const unsigned char *md5);

void
identity_json(const char *name_email, struct jg2_ctx *ctx);

int
__repo_reflist_update(struct jg2_vhost *vh, struct jg2_repo *repo);

int
jg2_oid_to_ref_names(const git_oid *oid, struct jg2_ctx *ctx,
		     struct jg2_ref **result, int max);

int
jg2_json_oid(const git_oid *oid, struct jg2_ctx *ctx);

int
jg2_repopath_split(const char *urlpath, struct jg2_split_repopath *sr);

void
jg2_repopath_destroy(struct jg2_split_repopath *sr);

int
blob_from_commit(struct jg2_ctx *ctx);

int
__jg2_vhost_reference_html(struct jg2_vhost *vh);

int
__jg2_cache_query_v(struct jg2_ctx *ctx, int flags, const char *suffix, int *_fd,
		    char *cache, int cache_len, const char *format, ...)
			JG2_FORMAT(7);

int
cache_trim_thread_spawn(struct jg2_global *jg2_global);

int
jg2_oid_lookup(git_repository *repo, git_oid *oid, const char *hex_oid);

#endif
