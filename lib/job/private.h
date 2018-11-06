typedef enum {
	JG2_JOB_REFLIST,
	JG2_JOB_LOG,
	JG2_JOB_COMMIT,
	JG2_JOB_PATCH,
	JG2_JOB_TREE,
	JG2_JOB_PLAIN,
	JG2_JOB_REPOLIST,
	JG2_JOB_SNAPSHOT,
	JG2_JOB_BLAME,
	JG2_JOB_BLOG,
	JG2_JOB_SEARCH,

	JG2_JOB_SEARCH_TRIE = 99
} jg2_job_enum;

typedef enum {
	EMIT_STATE_SUMMARY,
	EMIT_STATE_SUMMARY_LOG,

	EMIT_STATE_LOG,

	EMIT_STATE_COMMITBODY,

	EMIT_STATE_PATCH,

	EMIT_STATE_TAGS,

	EMIT_STATE_BRANCHES,

	EMIT_STATE_TREE,

	EMIT_STATE_PLAIN,

	EMIT_STATE_REPOLIST,

	EMIT_STATE_SNAPSHOT,

	EMIT_STATE_BLAME,

	EMIT_STATE_BLOG,

	EMIT_STATE_SEARCH,

} jg2_job_state;

enum rei_string_index {
	REI_STRING_NAME,
	REI_STRING_CONFIG_DESC,
	REI_STRING_CONFIG_OWNER,
	REI_STRING_CONFIG_URL
};

/*
 * Indicates this job is a consequence of the immediately previous job.  If
 * storing the results into the cache, it means this job should be appended to
 * previously used cache file and not treated as somthing on its own.
 */

#define JG2_JOB_FLAG_FINAL 1
#define JG2_JOB_FLAG_CHAINED 128

/**
 * jg2_job() - produce output from the job
 *
 * \param ctx: pointer to the context
 *
 * Writes a buffer with JSON from the execution of a "job".
 *
 * buf and len (length of buf) set where the JSON will be written.
 *
 * If the output is incomplete, 1 will be returned.  If the written chunk
 * of JSON is the last in the "job", 0 will be returned.
 *
 * Job functions set the context job to NULL when they complete the last chunk.
 */
typedef int (*jg2_job)(struct jg2_ctx *ctx);


/**
 * jg2_jobs() - Get pointer to jobs table
 *
 * \param n: One of JG2_job_enum (JG2_JOB_...)
 *
 * Returns a function pointer to the requested "job" the library can statefully
 * perform in a context.
 */
jg2_job
jg2_get_job(jg2_job_enum n);

/**
 * jg2_ctx_get_job() - get the current "job" the context is doing
 *
 * \param ctx: pointer to the context
 *
 * Returns NULL if no active job, or the pointer to the job function
 */
jg2_job
jg2_ctx_get_job(struct jg2_ctx *ctx);


/*
 * Is the mode going to produce JSON?  Return 0.  Otherwise return the
 * JG2_JOB index for the type of naked content it wants to produce.
 */
int
jg2_job_naked(struct jg2_ctx *ctx);

void
meta_header(struct jg2_ctx *ctx);

void
job_common_header(struct jg2_ctx *ctx);

void
meta_trailer(struct jg2_ctx *ctx, const char *term);

void
__jg2_job_compute_cache_hash(struct jg2_ctx *ctx, jg2_job_enum job, int count,
			     char *md5_hex33);

const char *
jg2_rei_string(const struct repo_entry_info *rei, enum rei_string_index n);

int
job_search_check_indexed(struct jg2_ctx *ctx, uint32_t *files, uint32_t *done);

/* jobs */

int
job_reflist(struct jg2_ctx *ctx);

int
job_log(struct jg2_ctx *ctx);

int
job_commit(struct jg2_ctx *ctx);

int
job_tree(struct jg2_ctx *ctx);

int
job_plain(struct jg2_ctx *ctx);

int
job_repos(struct jg2_ctx *ctx);

int
job_snapshot(struct jg2_ctx *ctx);

int
job_blame(struct jg2_ctx *ctx);

int
job_blog(struct jg2_ctx *ctx);

int
job_search(struct jg2_ctx *ctx);
