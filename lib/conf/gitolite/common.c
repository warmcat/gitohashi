/*
 * libjsongit2 - gitolite config parsing
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

#include "../../private.h"

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#define lp_to_rei(p, _n) lws_list_ptr_container(p, struct repo_entry_info, _n)

/* repodir lock must be held */

struct repo_entry_info *
__jg2_repodir_repo(struct jg2_repodir *rd, const char *repo_name)
{
	lws_list_ptr lp = rd->rei_head;

	while (lp) {
		struct repo_entry_info *rei = lp_to_rei(lp, next);

		if (!strcmp((const char *)(rei + 1), repo_name))
			return rei;
		lws_list_ptr_advance(lp);
	}

	return NULL;
}

/* return 0 for authorized */

int
jg2_acl_check(struct jg2_ctx *ctx, const char *reponame, const char *auth)
{
	struct jg2_vhost *vh = ctx->vhost;
	struct jg2_repodir *rd = vh->repodir;
	struct repo_entry_info *rei;
	int ret = 1;

	if (!reponame || !reponame[0]) {
		lwsl_err("%s: NULL or empty reponame\n", __func__);
		return 1; /* disallow */
	}

	if (auth && !strcmp(auth, "@all"))
		return 0; /* allow everything */

	pthread_mutex_lock(&rd->lock); /* ====================== repodir lock */

	rei = __jg2_repodir_repo(rd, reponame);
	if (rei) {
		if (!__jg2_gitolite3_acl_check(ctx, rei, vh->cfg.acl_user))
			ret = 0;
		else
			if (auth && !__jg2_gitolite3_acl_check(ctx, rei, auth))
				ret = 0;
	} else
		lwsl_err("%s: no rei for %s\n", __func__, reponame);

	pthread_mutex_unlock(&rd->lock); /* ------------------ repodir unlock */

	return ret;
}

int
__jg2_conf_gitolite_admin_head(struct jg2_ctx *ctx)
{
	char filepath[256], oid_hex[GIT_OID_HEXSZ + 1];
	struct jg2_vhost *vh = ctx->vhost;
	struct jg2_repodir *rd = ctx->vhost->repodir;
	time_t now = time(NULL);
	git_repository *repo;
	int m, ret = 1;
	git_oid oid;

	/* don't check more often than once per second */

	if (rd->last_gitolite_admin_head_check == now)
		return 0;

	/* it's OK if the gitolite-admin repo doesn't exist, we just return */

	lws_snprintf(filepath, sizeof(filepath), "%s/gitolite-admin.git",
		     vh->cfg.repo_base_dir);

	m = git_repository_open_ext(&repo, filepath, 0, NULL);
	if (m < 0) {
		lwsl_err("%s: git_repository_open_ext can't open %s: %d:%d uid %d gid %d\n", __func__, filepath, m, errno, (int)getuid(), (int)getgid());
		goto bail;
	}

	m = git_reference_name_to_id(&oid, repo, "refs/heads/master");
	if (m < 0) {
		lwsl_err("%s: unable to find master ref: %d\n", __func__, m);
		goto bail1;
	}

	oid_to_hex_cstr(oid_hex, &oid);
	rd->last_gitolite_admin_head_check = now;

	if (strcmp(oid_hex, rd->hexoid_gitolite_conf)) {

		/*
		 * there has been a change in gitolite-admin, or it's the first
		 * time we looked since starting... either way reload everything
		 */

		lwsl_notice("%s: gitolite-admin changed\n", __func__);

		lws_snprintf(filepath, sizeof(filepath), "/tmp/_goh_rl_%s",
				rd->hexoid_gitolite_conf);
		unlink(filepath);
		strcpy(rd->hexoid_gitolite_conf, oid_hex);

		/*
		 * Drop ALL the rei and acl collected information
		 * on the repodir and resets ALL the heads
		 */

		lwsac_free(&rd->rei_lwsac_head);
		rd->rei_head = NULL;
		rd->acls_known_head = NULL;

		/* re-acquire the basic rei list (repos in the dir) */

		__jg2_conf_scan_repos(rd);
	}

	/* if we don't have it already, re-compute the vhost acl */
	__jg2_conf_ensure_acl(ctx, vh->cfg.acl_user);

	ret = 0;

bail1:
	git_repository_free(repo);
bail:
	return ret;
}
