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

#include "../private.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#define lp_to_acl(p, _n) list_ptr_container(p, struct acl, _n)

typedef enum {
	GCS_IDLE,

	GCS_MACRODEF_NAME,	/* @ got us here */
	GCS_MACRODEF_WS,	/* ws before = */
	GCS_MACRODEF_WS1,	/* ws after = */
	GCS_MACRODEF_VALUE,	/* macro definition value */

	GCS_REPO,		/* r ... started collecting "repo" */
	GCS_REPO_WS1,		/* ws before = */
	GCS_REPO_REPODEF,	/* matching repo definition */

	GCS_ACL_WS_PRE,		/* ws before repo acl */
	GCS_ACL_FLAGS,		/* access flags, - or R/W/+ */
	GCS_ACL_WS1,		/* ws before = or optional refspec */
	GCS_ACL_REFSPEC,	/* refspec */
	GCS_ACL_WS2,		/* ws after refspec */
	GCS_ACL_WS3,		/* ws after = */
	GCS_ACL_USERS,		/* user list */
	GCS_ACL_IGNORE,		/* ignore rest of acl line (not READ) */

	GCS_INCLUDE,		/* i ... started collecting "include" */
	GCS_INCLUDE_WS1,
	GCS_INCLUDE_PATH,

	GCS_CONFIG,		/* c... started collecting "config" */

	GCS_CONFIG_IGNORE,

} gitolite_conf_state;

struct gitolite_conf {
	gitolite_conf_state state;
	char macro_value[1024];
	char macro_name[32];

	git_repository *repo;
	git_generic_ptr u;
	git_tree *tree;
	const char *p, *end;

	size_t pos, name_len;
	int line;
	char seen_read;
	char comment;
	char last;
};

static int
__parser_init(struct gitolite_conf *gp, struct jg2_vhost *vh,
		  git_repository *repo, git_tree *tree, jg2_md5_context md5_ctx,
		  const char *in_repo_filepath);

/*
 * This supports the following gitolite config syntax elements:
 *
 *  -         @macros : yes
 *
 *  -    repo @macros : yes
 *
 *  -             RW+ : yes
 *
 *  -         include : yes
 *
 *  -          config : ignored: we don't need to parse them, since the
 *			corresponding changes made by gitolite are picked up
 *			from the repo config and used... for
 *			gitweb.[description|owner|url]
 *
 *  -           refex : ignored (only supports repo-level ACLs)
 *
 *  -      repo regex : matched as literal (ie, ignored)
 *
 *  - Negative ACL (-): no
 *
 *  -         subconf : no
 *
 *  Patches adding support if you need the missing things are very welcome...
 */

/*
 * does the string 'check' match anything in the macro 'macro'?
 * @all will match everything always
 *
 * Requires vhost lock
 */
static int
__recursive_macro_check(const char *check, const char *macro, int string,
		      struct jg2_vhost *vh)
{
	int cl = strlen(check);
	list_ptr lp;
	char macro_name[32];
	const char *p;

	if (string) {
		p = macro;
		goto direct;
	}

	lp = vh->acl_macro_head;
	while (lp) {
		struct acl *acl = lp_to_acl(lp, next);

		p = (const char *)(acl + 1);

		if (strcmp(p, macro))
			goto again;

		/* this is the right macro */

		p = p + acl->len1 + 1;

direct:
		while (*p) {
			if (!strncmp(p, check, cl) &&
			    (p[cl] == ' ' || !p[cl]))
				return 0; /* hit */

			if (*p == '@') {
				size_t n = 0;
				p++;
				while (*p != ' ' && !*p) {
					if (n >= sizeof(macro_name) - 2)
						return 1;
					macro_name[n++] = *p++;
				}
				macro_name[n] = '\0';

				if (!strcmp(macro_name, "all"))
					return 0;

				if (!__recursive_macro_check(check, macro, 0,
							     vh))
					return 0;
			}

			p++;
		}

		return 1;

again:
		list_ptr_advance(lp);
	}

	return 1;
}

/*
 * can "user" 'acl_name' read repo 'reponame'?
 *
 * requires the vhost lock
 */

int
__repo_check_acl(struct jg2_vhost *vh, const char *reponame, const char *acl_name)
{
	list_ptr lp;
	int ret = 1;

	if (!acl_name)
		return 1; /* NULL can't match */

	if (!strcmp(acl_name, "@all"))
		return 0; /* just match everything */

	/*
	 * let's go through all the rules
	 */

	lp = vh->acl_rule_head;
	while (lp) {
		struct acl *acl = lp_to_acl(lp, next);
		const char *p = (const char *)(acl + 1);

		/* reponame isn't in the items on p (including by macros?) */
		if (!__recursive_macro_check(reponame, p, 1, vh)) {

			/* we got an acl rule matched on our repo name */

			p += acl->len1 + 1;

			/* acl_name is in the items on the acl rule? */
			if (!__recursive_macro_check(acl_name, p, 1, vh)) {
				ret = 0;

				break;
			}
		}

		list_ptr_advance(lp);
	}

	return ret;
}

/*
 * we convert the gitolite conf into two lists of struct acls
 *
 *  - vh->acl_macro_head: each is macroname, macrovalue
 *  - vh->acl_rule_head:  each is  reponame matching rule, acl list
 *
 * Requires vhost lock
 */

static int
__parse(struct gitolite_conf *gp, struct jg2_vhost *vh, jg2_md5_context md5_ctx)
{
	struct acl *acl;
	char *p;

	while (gp->p < gp->end) {

		// putchar(*gp->p);

		if (!gp->comment && *gp->p == '#')
			gp->comment = 1;

		if (gp->comment && *gp->p == '\n')
			gp->comment = 0;

		if (gp->comment)
			goto after;

		switch (gp->state) {
		case GCS_IDLE:
			gp->pos = 0;
			if (*gp->p == '@')
				gp->state = GCS_MACRODEF_NAME;
			if (*gp->p == 'r')
				gp->state = GCS_REPO;
			if (*gp->p == 'i')
				gp->state = GCS_INCLUDE;

			if (*gp->p == 'c') {
				gp->pos = 0;
				gp->state = GCS_CONFIG;
				break;
			}

			break;

		case GCS_MACRODEF_NAME:
			if (*gp->p == '=') {
				gp->state = GCS_MACRODEF_WS1;
				break;
			}
			if (*gp->p == ' ' || *gp->p == '\t') {
				gp->state = GCS_MACRODEF_WS;
				break;
			}

			if (gp->pos >= sizeof(gp->macro_name) - 1) {
				lwsl_err("%s: macro name too long: %s\n",
					 __func__, gp->macro_name);
				return -1;
			}
			gp->macro_name[gp->pos++] = *gp->p;
			gp->macro_name[gp->pos] = '\0';
			gp->name_len = gp->pos;
			break;

		case GCS_MACRODEF_WS:
			if (*gp->p == ' ' || *gp->p == '\t')
				break;
			if (*gp->p == '=') {
				gp->state = GCS_MACRODEF_WS1;
				break;
			}
			goto syntax_error;

		case GCS_MACRODEF_WS1:
			gp->pos = 0;
			if (*gp->p == ' ' || *gp->p == '\t')
				break;
			gp->state = GCS_MACRODEF_VALUE;
			/* fallthru */

		case GCS_MACRODEF_VALUE:
			if (*gp->p == '\n') {
				//lwsl_err("macro: '%s' '%s'\n", gp->macro_name,
				//		gp->macro_value);
				/* allocate the whole area at once */
				acl = lac_use(&vh->acl_lac_head,
						  sizeof(*acl) +
						  gp->name_len + 1 +
						  gp->pos + 1, 0);

				acl->len1 = gp->name_len;
				acl->len2 = gp->pos;

				p = (char *)(acl + 1);
				memcpy(p, gp->macro_name, acl->len1 + 1);
				memcpy(p + acl->len1 + 1, gp->macro_value,
				       acl->len2 + 1);

				list_ptr_insert(&vh->acl_macro_head,
						&acl->next, NULL);

				gp->state = GCS_IDLE;
				break;
			}

			if (gp->pos == sizeof(gp->macro_value) - 1) {
				lwsl_err("%s: macro value too long: %s\n",
					 __func__, gp->macro_name);
				return -1;
			}
			gp->macro_value[gp->pos++] = *gp->p;
			gp->macro_value[gp->pos] = '\0';
			break;

		case GCS_REPO:
			gp->pos++;
			if (gp->pos == 1 && *gp->p == 'e')
				break;
			if (gp->pos == 2 && *gp->p == 'p')
				break;
			if (gp->pos == 3 && *gp->p == 'o') {
				gp->state = GCS_REPO_WS1;
				break;
			}
			goto syntax_error;

		case GCS_REPO_WS1:
			gp->pos = 0;
			if (*gp->p == ' ' || *gp->p == '\t')
				break;
			gp->state = GCS_REPO_REPODEF;
			/* fallthru */

		case GCS_REPO_REPODEF:
			if (*gp->p == '\n') {
				gp->pos = 0;
				gp->state = GCS_ACL_WS_PRE;
				break;
			}
			gp->macro_name[gp->pos++] = *gp->p;
			gp->macro_name[gp->pos] = '\0';
			gp->name_len = gp->pos;
			break;

		case GCS_ACL_WS_PRE:
			gp->seen_read = 0;
			if (*gp->p == ' ' || *gp->p == '\t')
				break;
			if (*gp->p == '\n') {
				gp->state = GCS_IDLE;
				break;
			}
			gp->state = GCS_ACL_FLAGS;
			/* fallthru */

		case GCS_ACL_FLAGS:
			if (*gp->p == 'R') {
				gp->seen_read = 1;
				break;
			}
			if (*gp->p == 'W')
				break;
			if (*gp->p == '+')
				break;
			if (*gp->p == '-')
				break;
			if (*gp->p == '=') {
				if (gp->seen_read)
					gp->state = GCS_ACL_WS3;
				else
					gp->state = GCS_ACL_IGNORE;
				break;
			}
			if (*gp->p == 'c') {
				gp->pos = 0;
				gp->state = GCS_CONFIG;
				break;
			}
			if (*gp->p == 'r') {
				gp->pos = 0;
				gp->state = GCS_REPO;
				break;
			}
			if (*gp->p != ' ' && *gp->p != '\t')
				goto syntax_error;

			gp->state = GCS_ACL_WS1;
			/* fallthru */

		case GCS_ACL_WS1:
			gp->pos = 0;
			if (*gp->p == ' ' || *gp->p == '\t')
				break;
			if (*gp->p == '=') {
				gp->state = GCS_ACL_WS3;
				break;
			}

			gp->state = GCS_ACL_REFSPEC;
			break;

		case GCS_ACL_REFSPEC:
			if (*gp->p != ' ' && *gp->p != '\t')
				break;
			gp->state = GCS_ACL_WS2;
			/* fallthru */

		case GCS_ACL_WS2:
			if (*gp->p == ' ' || *gp->p == '\t')
				break;
			if (*gp->p != '=')
				goto syntax_error;

			gp->state = GCS_ACL_WS2;
			/* fallthru */

		case GCS_ACL_WS3:
			if (*gp->p == ' ' || *gp->p == '\t')
				break;
			gp->state = GCS_ACL_USERS;
			/* fallthru */

		case GCS_ACL_USERS:
			if (*gp->p == '\n') {
				//lwsl_err("repos: '%s', acl: '%s'\n",
				//	 gp->macro_name, gp->macro_value);

				/* allocate the whole area at once */
				acl = lac_use(&vh->acl_lac_head,
					      sizeof(*acl) + gp->name_len + 1 +
					      gp->pos + 1, 0);

				acl->len1 = gp->name_len;
				acl->len2 = gp->pos;

				p = (char *)(acl + 1);
				memcpy(p, gp->macro_name, acl->len1 + 1);
				memcpy(p + acl->len1 + 1, gp->macro_value,
				       acl->len2 + 1);

				list_ptr_insert(&vh->acl_rule_head,
						&acl->next, NULL);

				gp->state = GCS_ACL_WS_PRE;
				break;
			}

			if (gp->pos == sizeof(gp->macro_value) - 1) {
				lwsl_err("%s: macro value too long: %s\n",
					 __func__, gp->macro_name);
				return -1;
			}
			gp->macro_value[gp->pos++] = *gp->p;
			gp->macro_value[gp->pos] = '\0';
			break;

		case GCS_ACL_IGNORE:
			if (*gp->p != '\n')
				break;
			gp->pos = 0;
			gp->state = GCS_ACL_WS_PRE;
			break;

		case GCS_INCLUDE:
			gp->pos++;
			if (gp->pos == 1 && *gp->p == 'n')
				break;
			if (gp->pos == 2 && *gp->p == 'c')
				break;
			if (gp->pos == 3 && *gp->p == 'l')
				break;
			if (gp->pos == 4 && *gp->p == 'u')
				break;
			if (gp->pos == 5 && *gp->p == 'd')
				break;
			if (gp->pos == 6 && *gp->p == 'e') {
				gp->state = GCS_INCLUDE_WS1;

				if (gp->last) {
					lwsl_err("too many include levels\n");
					goto syntax_error;
				}
				break;
			}
			goto syntax_error;

		case GCS_INCLUDE_WS1:
			gp->pos = 0;
			if (*gp->p == ' ' || *gp->p == '\t')
				break;
			if (*gp->p != '\"')
				goto syntax_error;

			gp->state = GCS_INCLUDE_PATH;
			break;

		case GCS_INCLUDE_PATH:
			if (*gp->p == '\"') {
				gp->state = GCS_IDLE;
				gp->macro_value[gp->pos] = '\0';

				if (__parser_init(gp + 1, vh, gp->repo,
						      gp->tree, md5_ctx,
						      gp->macro_value))
					return 1;

				break;
			}

			if (gp->pos > sizeof(gp->macro_value) - 2)
				goto syntax_error;

			gp->macro_value[gp->pos++] = *gp->p;
			break;

		case GCS_CONFIG:
			gp->pos++;
			if (gp->pos == 1 && *gp->p == 'o')
				break;
			if (gp->pos == 2 && *gp->p == 'n')
				break;
			if (gp->pos == 3 && *gp->p == 'f')
				break;
			if (gp->pos == 4 && *gp->p == 'i')
				break;
			if (gp->pos == 5 && *gp->p == 'g') {
				gp->state = GCS_CONFIG_IGNORE;

				if (gp->last) {
					lwsl_err("too many include levels\n");
					goto syntax_error;
				}
				break;
			}
			goto syntax_error;
			break;

		case GCS_CONFIG_IGNORE:
			if (*gp->p != '\n')
				break;
			gp->state = GCS_ACL_WS_PRE;
			gp->pos = 0;
			gp->seen_read = 0;
			break;
		}
after:

		if (*gp->p == '\n')
			gp->line++;

		gp->p++;
	}

	return 0;

syntax_error:
	lwsl_err("%s: %d: syntax error '%c' state %d\n", __func__, gp->line,
		 *gp->p, gp->state);

	return 1;
}

/* requires vhost lock */

static int
__parser_init(struct gitolite_conf *gp, struct jg2_vhost *vh,
		  git_repository *repo, git_tree *tree, jg2_md5_context md5_ctx,
		  const char *in_repo_filepath)
{
	git_tree_entry *te;
	int m, ret;

	gp->tree = tree;
	gp->repo = repo;

	m = git_tree_entry_bypath(&te, tree, in_repo_filepath);
	if (m) {
		lwsl_err("get_tree_entry_bypath failed\n");

		return 1;
	}

	m = git_tree_entry_to_object(&gp->u.obj, repo, te);
	git_tree_entry_free(te);
	if (m) {
		lwsl_err("tree_entry_to_object failed\n");

		return 1;
	}

	if (git_object_type(gp->u.obj) != GIT_OBJ_BLOB) {
		lwsl_err("object is not a blob (%d)\n",
			 git_object_type(gp->u.obj));
		git_object_free(gp->u.obj);

		return 1;
	}

	gp->state = GCS_IDLE;
	gp->p = git_blob_rawcontent(gp->u.blob);
	gp->end = gp->p + git_blob_rawsize(gp->u.blob);
	gp->line = 1;
	gp->comment = 0;
	gp->name_len = 0;
	gp->pos = 0;

	ret = __parse(gp, vh, md5_ctx);

	git_object_free(gp->u.obj);

	return ret;
}

/*
 * gitolite ACL config parser
 *
 * #comment for rest of line
 *
 * @macro=string1 string2  defines macro to be string1 string2
 *
 * @macro anywhere is replaced by string1 string2
 *
 * repo <repo list>
 *  [RW+] [optional reflist] = [name list]
 *  [- reflist] = [name list]
 *
 * For now, anything with a reflist we ignore.
 *
 * Because later rules can countermand earlier rules, and macros may be
 * forward-referenced, we defer dereferencing the macros until we try to
 * walk the repo ACLs.
 *
 * Just to make things exciting, the config only exists in the
 * gitolite-admin bare git repo, not as files on disk.
 *
 * The set of repos is defined by the available subdirs.
 *
 * The repo token only restricts the whole set somehow.
 */

int
jg2_conf_scan_gitolite(struct jg2_vhost *vh)
{
	struct gitolite_conf stack[3];
	git_repository *repo;
	char filepath[256];
	git_generic_ptr u;
	int m, ret = 1;
	git_commit *c;
	git_oid oid;

	stack[0].last = stack[1].last = 0;
	stack[2].last = 1;

	/* it's OK if the gitolite-admin repo doesn't exist, we just return */

	lws_snprintf(filepath, sizeof(filepath), "%s/gitolite-admin.git",
		     vh->cfg.repo_base_dir);

	m = git_repository_open_ext(&repo, filepath, 0, NULL);
	if (m < 0)
		goto bail;

	m = git_reference_name_to_id(&oid, repo, "refs/heads/master");
	if (m < 0) {
		lwsl_err("%s: unable to find master ref: %d\n", __func__, m);
		goto bail1;
	}

	m = git_object_lookup(&u.obj, repo, &oid, GIT_OBJ_ANY);
	if (m < 0) {
		lwsl_err("object lookup failed\n");
		goto bail1;
	}

	if (git_object_type(u.obj) != GIT_OBJ_COMMIT) {
		lwsl_err("object not a commit\n");

		goto bail1;
	}

	/* convert the commit object to a tree object */

	c = u.commit;
	m = git_commit_tree(&u.tree, u.commit);
	git_commit_free(c);
	if (m) {
		lwsl_err("git_commit_tree failed\n");

		goto bail1;
	}

	pthread_mutex_lock(&vh->lock); /* ======================== vhost lock */

	ret = __parser_init(&stack[0], vh, repo, u.tree, NULL,
			  "conf/gitolite.conf");

	oid_to_hex_cstr(vh->repodir->hexoid_gitolite_conf, &oid);

	pthread_mutex_unlock(&vh->lock); /*--------------------- vhost unlock */

	git_tree_free(u.tree);

bail1:
	git_repository_free(repo);
bail:
	return ret;
}
