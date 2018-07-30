struct repo_entry_info;

int
jg2_conf_scan_gitolite(struct jg2_vhost *vh);

int
jg2_get_repo_config(git_repository *gr, struct repo_entry_info *rei, char *p);

int
jg2_conf_scan_repos(struct jg2_vhost *vh);

int
__repo_check_acl(struct jg2_vhost *vh, const char *reponame,
	       const char *acl_name);
