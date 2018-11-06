struct repo_entry_info;
struct jg2_repodir;
struct jg2_global;
struct jg2_vhost;
struct jg2_ctx;

int
jg2_conf_scan_gitolite(struct jg2_vhost *vh);

int
jg2_get_repo_config(git_repository *gr, struct repo_entry_info *rei, char *p);

/* must have repodir lock */
int
__jg2_conf_scan_repos(struct jg2_repodir *rd);

int
__repo_check_acl(struct jg2_vhost *vh, const char *reponame,
	       const char *acl_name);

int
jg2_gitolite3_interface(struct jg2_global *jg2_global, const char *repodir);

void
jg2_gitolite3_interface_destroy(struct jg2_global *jg2_global);

int
jg2_gitolite3_blocking_query(struct jg2_global *jg2_global, const char *query,
			     const char *stdin_path, const char *output);

int
__jg2_conf_ensure_acl(struct jg2_ctx *ctx, const char *acl);

int
__jg2_gitolite3_acl_check(struct jg2_ctx *ctx, struct repo_entry_info *rei,
			  const char *acl);

int
__jg2_conf_gitolite_admin_head(struct jg2_ctx *ctx);

/* repodir lock must be held */

struct repo_entry_info *
__jg2_repodir_repo(struct jg2_repodir *rd, const char *reponame);

int
jg2_acl_check(struct jg2_ctx *ctx, const char *reponame, const char *auth);
